/**
 * @file	Runtime.cpp 
 * @author	Jesús Carabaño Bravo <jcaraban@abo.fi>
 *
 */

#include "Runtime.hpp"
#include "dag/util.hpp"
#include "dag/Loop.hpp"
#include "visitor/Lister.hpp"
#include "visitor/Sorter.hpp"
#include "visitor/Partitioner.hpp"
#include "visitor/Fusioner.hpp"
#include "visitor/Exporter.hpp"
#include "visitor/Cloner.hpp"
#include <algorithm>


namespace map { namespace detail {

/***************
   Definitions
****************/

thread_local ThreadId Tid;  //!< Per thread local storage for the ID = {node,device,rank}

/************
   Runtime
*************/

Runtime& Runtime::getInstance() {
	static Runtime instance; // Guaranteed to be destroyed. Instantiated on first use.
    return instance;
}

Config& Runtime::getConfig() {
	return getInstance().conf;
}

Clock& Runtime::getClock() {
	return getInstance().clock;
}

LoopAssembler& Runtime::getLoopAssembler() {
	return getInstance().assembler;
}

cle::OclEnv& Runtime::getOclEnv() {
	return getInstance().clenv;
}

Runtime::Runtime()
	: clenv()
	, conf()
	, clock(conf)
	, program(clock,conf)
	, cache(program,clock,conf)
	, scheduler(program,clock,conf)
	, workers()
	, threads()
	, node_list()
	, group_list()
	, task_list()
	, simplifier(node_list)
	, assembler(conf.loop_nested_limit)
{
	// Overall process timing
	clock.start(OVERALL);

	// Workers construction
	for (int i=0; i<conf.max_num_workers; i++) {
		workers.emplace_back(cache,scheduler,clock,conf);
	}

	//// something else could be initialized here ////
}

Runtime::~Runtime() {
	// something could be deleted here

	// Nodes cannot be deleted until unlinked
	unlinkIsolated(node_list);

	cache.freeChunks();
	clock.stop(OVERALL);

	reportOver();
}

void Runtime::clear() {
	node_list.clear();
	group_list.clear();
	task_list.clear();
	program.clear();
	cache.clear();
	scheduler.clear();
	Node::id_count = 0;
}

void Runtime::setupDevices(std::string plat_name, DeviceType dev, std::string dev_name) {
	// Free memory cache. Does nothing if chunks were not allocated yet
	cache.freeChunks();

	// Times related to preparing OpenCL to use the parallel devices
	clock.start(DEVICES);

	// Free old queues, programs, kernels, contexts, etc
	clenv.clear();

	// Only 1 device accepted
	clenv.init("P=# P_NAME=%s, D=1 D_TYPE=%d D_NAME=%s", plat_name.data(), dev, dev_name.data()); //, C=1xD

	// Apply device fission if: 'Intel' and 'CPU' and 'conf.interpreted'
	if (plat_name.compare("Intel")==0 && dev==DEV_CPU && conf.interpreted)
	{
		assert(conf.num_ranks == 1);
		cl_device_partition_property dprops[5] = // Reduce CPU to 1 device with 1 thread
			{CL_DEVICE_PARTITION_BY_COUNTS, 1, CL_DEVICE_PARTITION_BY_COUNTS_LIST_END, 0};
		cl_device_id subdev_id[1];
		cl_int err;
		err = clCreateSubDevices(*clenv.D(0), dprops, 1, subdev_id, NULL);
		cle::clCheckError(err);
		// Remove original CPU device, add new sub-device
		clenv.P(0).removeDevice(*clenv.D(0));
		clenv.P(0).addDevice(subdev_id[0]);
	}

	// Adding 1 context
	cl_device_id dev_group[1] = {*clenv.D(0)};
	cl_context_properties cp[3] = {CL_CONTEXT_PLATFORM, (cl_context_properties)*clenv.P(0), 0};
	cl_int err;
	cl_context ctx = clCreateContext(cp, 1, dev_group, NULL, NULL, &err);
	cle::clCheckError(err);
	clenv.D(0).addContext(ctx);

	// Adding queues to the contexts
	for (int i=0; i<clenv.nC(); i++) {
		cle::Context ctx = clenv.C(i);
		for (int j=0; j<ctx.nD(); j++) {
			cle::Device dev = ctx.D(j);
			for (int k=0; k<conf.num_ranks; k++) {
				cl_int err;
				cl_command_queue que = clCreateCommandQueue(*ctx, *dev, 0, &err);
				cle::clCheckError(err);
				ctx.addQueue(*dev, que);
			}
		}
	}

	clock.stop(DEVICES);

	// Initializing memory cache. Allocates chunks of memory now
	cache.allocChunks(clenv.C(0));
}

Node* Runtime::loopAssemble() {
	Node* node = assembler.assemble();
	node_list.push_back( std::unique_ptr<Node>(node) );

	// TODO: how to simplify a loop?
	Node *orig = node; // = simplifier.simplify(node);

	// Ask 'loop' for its newly created 'head', 'tail' and 'feed' nodes
	Loop *loop_node = dynamic_cast<Loop*>(orig);
	assert(loop_node != nullptr);

	// Inserts the 'cond' / 'head' / 'feed' in/out / 'tail' nodes into the node_list
	node_list.push_back( std::unique_ptr<Node>(loop_node->cond_node) );
	for (auto node : loop_node->head_list)
		node_list.push_back( std::unique_ptr<Node>(node) );
	for (auto node : loop_node->feed_in_list)
		node_list.push_back( std::unique_ptr<Node>(node) );
	for (auto node : loop_node->feed_out_list)
		node_list.push_back( std::unique_ptr<Node>(node) );
	for (auto node : loop_node->tail_list)
		node_list.push_back( std::unique_ptr<Node>(node) );

	return node;
}

Node* Runtime::addNode(Node *node) {
	// TimedRegion region(clock,ADDNODE);

	node_list.push_back( std::unique_ptr<Node>(node) );
	Node *orig = simplifier.simplify(node);

	if (assembler.mode() != NORMAL_MODE) // Inside a (possibly nested) loop
		assembler.addNode(node,orig);

	return orig;
}

Group* Runtime::addGroup(Group *group) {
	group_list.push_back( std::unique_ptr<Group>(group) );
	return group;
}

Task* Runtime::addTask(Task *task) {
	task_list.push_back( std::unique_ptr<Task>(task) );
	return task;
}

Version* Runtime::addVersion(Version *ver) {
	ver_list.push_back( std::unique_ptr<Version>(ver) );
	return ver;
}

void Runtime::removeNode(Node *node) {
	simplifier.drop(node);
}

void Runtime::updateNode(Node *node) {
	Node *orig = simplifier.simplify(node);
	assert(orig == node);
}

void Runtime::unlinkIsolated(const OwnerNodeList &node_list, bool drop) {
	// In reverse order, new isolated appear as we unlink nodes
	for (auto it=node_list.rbegin(); it!=node_list.rend(); it++) {
		Node *node = it->get();
		// Skip referred nodes
		if (node->ref > 0)
			continue;
		// Drop from Simplifier
		if (drop)
			simplifier.drop(node);
		// Inform prev nodes
		for (auto &prev : node->prevList())
			prev->removeNext(node);
		node->prev_list.clear();
	}
}

void Runtime::evaluate(NodeList list_to_eval) {
	/****************************************/
	/******** ENTRY POINT to Runtime ********/
	/****************************************/
	assert(clenv.contextSize() == 1);

	// Prepares the clock for another round
	clock.prepare();
	clock.start(EVAL);

// @ Prints nodes
std::cout << "----" << std::endl;
for (auto &node : node_list)
	std::cout << node->id << "\t" << node->getName() << "\t " << node->ref << std::endl;
std::cout << "----" << std::endl;

	// Unlinks all unaccessible (i.e. isolated) nodes & removes them from simplifier 
	unlinkIsolated(node_list,true);

	// Cleaning of old unaccessible nodes
	auto pred = [](std::unique_ptr<Node> &node){ return node->ref==0; };
	node_list.erase(std::remove_if(node_list.begin(),node_list.end(),pred),node_list.end());

	NodeList full_list;
	if (list_to_eval.size() == 0) // eval all nodes
	{
		full_list.reserve(node_list.size());	// @ What is the intended behaviour of eval_all ?
		for (auto &node : node_list)			//  - eval all accumulated Output and D0 nodes ?
			full_list.push_back(node.get());	//  - eval absolutely everything like matlab ?
	}
	else if (list_to_eval.size() == 1) // eval one node
	{
		full_list = Lister().list(list_to_eval.front());
	}
	else if (list_to_eval.size() >= 2) // eval few nodes
	{
		full_list = Lister().list(list_to_eval);
	}

// @ Prints nodes
std::cout << "----" << std::endl;
for (auto &node : full_list)
	std::cout << node->id << "\t" << node->getName() << "\t " << node->ref << std::endl;
std::cout << "----" << std::endl;

	// Sorts the list by 'dependencies' 1st, and 'id' 2nd
	auto sort_list = Sorter().sort(full_list);

// @ Prints nodes
std::cout << "----" << std::endl;
for (auto &node : sort_list)
	std::cout << node->id << "\t" << node->getName() << "\t " << node->ref << std::endl;
std::cout << "----" << std::endl;

	// Clones the list of sorted nodes into new list of new nodes
	OwnerNodeList priv_list; //!< Owned by this particular evaluation
	auto cloner = Cloner(priv_list);
	auto graph = cloner.clone(sort_list);
	auto map_new_old = cloner.new_hash;

// @ Prints nodes 5rd
std::cout << "----" << std::endl;
for (auto &node : priv_list)
	std::cout << node->id << "\t" << node->getName() << "\t " << node->ref << std::endl;
std::cout << "----" << std::endl;

	/**/
	workflow(graph);
	/**/

	// Transfers scalar values to original nodes
	for (auto node : graph)
		if (node->numdim() == D0 && node->pattern().isNot(FREE))
			map_new_old.find(node)->second->value = node->value;

	// Unlinks the private nodes before they are deleted
	unlinkIsolated(priv_list);

	clock.stop(EVAL);
	reportEval();
}

void Runtime::workflow(NodeList list) {
	// Variables reused for every pipeline pass
	group_list.clear();
	task_list.clear();
	program.clear();
	scheduler.clear();

	// Task fusion: fusing nodes into groups
	Fusioner fusioner(group_list);
	fusioner.fuse(list);

	// Export DAG
	Exporter exporter(fusioner.group_list_of);
	exporter.exportDag(list);
	
	// Program tasks composition
	program.compose(group_list);

	// Parallel code generation
	program.generate();

	// Code compilation
	program.compile();

	// Allocation of cache entries
	cache.allocEntries();
	
	// Adding initial jobs
	scheduler.addInitialJobs();

	// Make workers work
	this->work();

	// Release of cache entries
	cache.freeEntries();
}

void Runtime::work() {
	TimedRegion region(clock,EXEC);

	threads.clear();

	// Threads spawn, each with a worker
	int i = 0;
	for (int n=0; n<conf.num_machines; n++) {
		for (int d=0; d<conf.num_devices; d++) {
			for (int r=0; r<conf.num_ranks; r++) {
				auto thr = new std::thread(&Worker::work, &workers[i], ThreadId(n,d,r));
				threads.push_back( std::unique_ptr<std::thread>(thr) );
				i++;
			}
		}
	}

	// Workers gathering
	for (auto &thr : threads)
		thr->join();
}

void Runtime::reportEval() {
	// Synchronizes all times up till the system level
	clock.syncAll({ID_ALL,ID_ALL,ID_ALL});
	const int W = conf.num_workers;
	const double V = clock.get(EVAL) / 100;
	const double E = clock.get(EXEC) / 100;

	std::cerr.setf(std::ios::fixed);
	std::cerr.precision(2);

	std::cerr << "Eval:    " << clock.get(EVAL) << "s" << std::endl;
	//std::cerr << " Simplif: " << clock.get("Simplif")/V << "%" << std::endl;
	std::cerr << " Fusion:  " << clock.get(FUSION)/V << "%" << std::endl;
	std::cerr << " Taskif:  " << clock.get(TASKIF)/V << "%" << std::endl;
	std::cerr << " CodGen:  " << clock.get(CODGEN)/V << "%" << std::endl;
	std::cerr << " Compil:  " << clock.get(COMPIL)/V << "%" << std::endl;
	std::cerr << " Add Job: " << clock.get(ADD_JOB)/V << "%" << std::endl;
	std::cerr << " AllocE:  " << clock.get(ALLOC_E)/V << "%" << std::endl;
	std::cerr << " Exec:    " << clock.get(EXEC)/V << "%" << std::endl;
	std::cerr << " FreeE:   " << clock.get(FREE_E)/V << "%" << std::endl;

	std::cerr << "  get job: " << clock.get(GET_JOB)/W/E << "%" << std::endl;
	std::cerr << "  load:    " << clock.get(LOAD)/W/E << "%" << std::endl;
	std::cerr << "  compute: " << clock.get(COMPUTE)/W/E << "%" << std::endl;
	std::cerr << "  store:   " << clock.get(STORE)/W/E << "%" << std::endl;
	std::cerr << "  notify:  " << clock.get(NOTIFY)/W/E << "%" << std::endl;
	
	std::cerr << "    read:  " << clock.get(READ)/W/E << "%" << std::endl;
	std::cerr << "    send:  " << clock.get(SEND)/W/E << "%" << std::endl;
	std::cerr << "    kernl: " << clock.get(KERNEL)/W/E << "%" << std::endl;
	std::cerr << "    recv:  " << clock.get(RECV)/W/E << "%" << std::endl;
	std::cerr << "    write: " << clock.get(WRITE)/W/E << "%" << std::endl;

	const size_t L = clock.get(LOADED) + clock.get(NOT_LOADED);
	const size_t S = clock.get(STORED) + clock.get(NOT_STORED);
	const size_t C = clock.get(COMPUTED) + clock.get(NOT_COMPUTED);
	std::cerr << "  loaded: " << clock.get(LOADED) << " (" << clock.get(NOT_LOADED) << ") " << clock.get(LOADED)/(double)L*100 << "%" << std::endl;
	std::cerr << "  stored: " << clock.get(STORED) << " (" << clock.get(NOT_STORED) << ") " << clock.get(STORED)/(double)S*100 << "%" << std::endl;
	std::cerr << "  computed: " << clock.get(COMPUTED) << " (" << clock.get(NOT_COMPUTED) << ") " << clock.get(COMPUTED)/(double)C*100 << "%" << std::endl;
	std::cerr << "  discarded: " << clock.get(DISCARDED) << " evicted: " << clock.get(EVICTED) << std::endl;

	std::cerr << (char*)clenv.D(0).get(CL_DEVICE_NAME) << std::endl;
}

void Runtime::reportOver() {
	// Synchronizes all times up till the system level
	clock.syncAll({ID_ALL,ID_ALL,ID_ALL});
	const double O = clock.get(OVERALL) / 100;

	std::cerr.setf(std::ios::fixed);
	std::cerr.precision(2);

	std::cerr << "Overall: " << clock.get(OVERALL) << "s" << std::endl;
	std::cerr << " OpenCL: " << clock.get(DEVICES) << "s" << std::endl;
	std::cerr << " AllocC:  " << clock.get(ALLOC_C)/O << "%" << std::endl;
	std::cerr << " FreeC:   " << clock.get(FREE_C)/O << "%" << std::endl;
}

} } // namespace map::detail
