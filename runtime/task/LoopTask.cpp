/**
 * @file    LoopTask.cpp 
 * @author  Jesús Carabaño Bravo <jcaraban@abo.fi>
 *
 */

#include "LoopTask.hpp"
#include "../Runtime.hpp"


namespace map { namespace detail {

LoopTask::LoopTask(Program &prog, Clock &clock, Config &conf, Group *group)
	: Task(prog,clock,conf,group)
{
	assert(inputList().size() % 2 == 0);
	assert(backList().empty());
	//self_jobs_count[0] = -1;

	for (auto node : group->nodeList()) {
		if (node->pattern().is(LOOP))
		{
			cond_node = dynamic_cast<LoopCond*>(node);
			assert(cond_node != nullptr);
		}
		else if (node->pattern().is(MERGE))
		{
			merge_list.push_back( dynamic_cast<Merge*>(node) );
			assert(merge_list.back() != nullptr);
		}
		else if (node->pattern().is(SWITCH))
		{
			switch_list.push_back( dynamic_cast<Switch*>(node) );
			assert(switch_list.back() != nullptr);
		}
	}
}

void LoopTask::blocksToLoad(Job job, KeyList &in_key) const {
	in_key.clear();
	auto coord = job.coord;
	auto iter = job.iter;

	mtx.lock();	
	bool cycling = cycling_input.find(job)->second;
	mtx.unlock();
	iter = cycling ? iter - 1 : iter;

	for (auto node : inputList()) {
		if (not cycling && node->pattern().isNot(HEAD))
			continue;
		if (cycling && node->pattern().is(HEAD))
			continue;

		auto reach = accuInputReach(node,coord);
		auto space = reach.blockSpace(blocksize());

		for (auto offset : space) {	
			Coord nbc = coord + offset;
			HoldType hold = node->holdtype(nbc);
			Depend dep = node->isInput() ? nextInputDepends(node,nbc) : -1;

			in_key.push_back( std::make_tuple(Key(node,nbc,iter),hold,dep) );
		}
	}
}

void LoopTask::blocksToStore(Job job, KeyList &out_key) const {
	Task::blocksToStore(job,out_key);
}

void LoopTask::initialJobs(std::vector<Job> &job_vec) {
	assert(0); // Should never be called
}

void LoopTask::askJobs(Job done_job, std::vector<Job> &job_vec) {
	assert(done_job.task == this);
	auto coord = done_job.coord;
	auto iter = done_job.iter;
	
	mtx.lock();	
	bool cycling = cycling_output[done_job];
	mtx.unlock();	

	// Asks next-tasks for their next-jobs, a.k.a inter-dependencies (all Op)
	for (auto next_task : this->nextList()) {
		if (not cycling && next_task->pattern().isNot(TAIL))
			continue;
		if (cycling && next_task->pattern().is(TAIL))
			continue;
		next_task->nextJobs(done_job,job_vec,Tid==last);
	}

	// Erases the 'cycling' entries for the future job in this same 'coord'
	mtx.lock();	
	cycling_input.erase(done_job);
	cycling_output.erase(done_job);
	mtx.unlock();

	if (Tid == last) {
		std::lock_guard<std::mutex> lock(mtx);
		last = ThreadId(); // @
	}
}

void LoopTask::selfJobs(Job done_job, std::vector<Job> &job_vec) {
	return; // nothing to do
}

void LoopTask::nextJobs(Job done_job, std::vector<Job> &job_vec, bool end) {
	bool cycling = not done_job.task->pattern().is(HEAD);
	if (cycling)
		done_job.iter++;

	auto common_nodes = inner_join(inputList(),done_job.task->outputList());
	mtx.lock();
	for (auto node : common_nodes) {
		if (node->numdim() == D0)
		{
			continue; // @@@
			auto beg = Coord(numblock().size(),0);
			auto end = numblock();
			for (auto coord : iterSpace(beg,end))
			{
				Job new_job = Job(this,coord,done_job.iter);
				if (cycling_input.find(new_job) == cycling_input.end())
					cycling_input[new_job] = cycling;
				assert(cycling_input[new_job] == cycling);
			}
		}
		else
		{
			Job new_job = Job(this,done_job.coord,done_job.iter);
			if (cycling_input.find(new_job) == cycling_input.end())
				cycling_input[new_job] = cycling;
			assert(cycling_input[new_job] == cycling);
		}
	}
	mtx.unlock();	

	Task::nextJobs(done_job,job_vec,end);
}

int LoopTask::prevDependencies(Coord coord) const {
	int dep = Task::prevDependencies(coord);
	assert(dep % 2 == 0);
	return dep / 2;
}

void LoopTask::postStore(Job job, const BlockList &in_blk, const BlockList &out_blk) {
	assert(cond_node != nullptr);

	// Finds 'cond_blk' among the blocks
	Block *cond_blk = nullptr;
	for (auto blk : out_blk)
		cond_blk = (blk->key.node == cond_node) ? blk : cond_blk;
	assert(cond_blk != nullptr);

	// Sets 'cycling' according to the result of 'cond_blk'
	bool cycling = cond_blk->value ? true : false;

	mtx.lock();
	assert(cycling_output.find(job) == cycling_output.end());
	cycling_output[job] = cycling;
	mtx.unlock();

	// Out blocks carry the sum of dependencies for both true and false branches
	// --> Needs to consume (i.e. notify) the dependencies of the non-taken branch
	for (int i=0, j=0; i<outputList().size(); i++) {
		if (outputList()[i]->pattern().isNot(SWITCH))
			continue; // only switch nodes
		auto *swit = switch_list[j++];
		auto blk = *std::find_if(out_blk.begin(),out_blk.end(),[&](Block *b){ return swit==b->key.node; });

		for (auto next : next_of_out[i]) {
			if (cycling) {
				if (inner_join(next->nodeList(),swit->falseList()).size() > 0)
					blk->notify();
			} else { // cycling = false
				if (inner_join(next->nodeList(),swit->trueList()).size() > 0)
					blk->notify();
			}
			 // @@ this notify should happen before releaseEntries()
			if (blk->discardable()) {
				if (blk->entry != nullptr) {
					blk->entry->block = nullptr;
					if (blk->entry->isDirty())
						blk->entry->unsetDirty();
					blk->entry = nullptr;
				}
			}
			// @@
		}
	}
}

void LoopTask::compute(Job job, const BlockList &in_blk, const BlockList &out_blk) {
	//return Task::compute(job,in_blk,out_blk);
	const Version *ver = getVersion(DEV_ALL,{},""); // Any device, No detail
	assert(ver != nullptr);

	// TODO: forward first, check for fixed after ?

	auto all_pred = [&](Block *b){ return b->fixed || b->key.node->canForward(); };
	if (std::all_of(out_blk.begin(),out_blk.end(),all_pred)) {
		clock.incr(NOT_COMPUTED);

		std::unordered_map<Node*,Block*> forward;
		for (auto iblk : in_blk)
			forward[iblk->key.node] = iblk;
		for (auto node : nodeList()) {
			auto prev = node->prevList().front(); // @ always the first 'prev' (Switch)
			if (forward.find(prev) == forward.end())
				prev = node->forwList().front(); // @ Merge
			forward[node] = forward[prev];
		}

		for (auto oblk : out_blk) {
			Block *iblk = forward[oblk->key.node];
			assert(iblk != nullptr);
			
			if (iblk->entry && oblk->entry) {
				//std::cout << "in_blk " << iblk->key.node->id << " " << iblk->numdim().toString() << " --> ";
				//std::cout << "out_blk " << oblk->key.node->id << " " << oblk->numdim().toString() << std::endl;
				std::swap( iblk->entry->dev_mem, oblk->entry->dev_mem ); // @ better swap the entry, careful with cross refs
			}
		}

		return; // All output blocks are fixed, no need to compute
	}

	computeVersion(job,in_blk,out_blk,ver);
}

} } // namespace map::detail
