/**
 * @file	Block1.cpp 
 * @author	Jesús Carabaño Bravo <jcaraban@abo.fi>
 *
 * TODO: substitue 'file' for a reduction device?
 */

#include "Block1.hpp"
#include "../Runtime.hpp"


namespace map { namespace detail {

Block1::Block1()
	: Block()
	, host_mem(nullptr)
	, file(nullptr)
	, scalar_page(nullptr)
	, value()
	, fixed(false)
{ }


Block1::Block1(Key key, int dep, cl_mem scalar_page)
	: Block(key,dep)
	, host_mem(nullptr)
	, file(nullptr)
	, scalar_page(scalar_page)
	, value()
	, fixed(false)
{ }

Block1::~Block1() { }

int Block1::size() const {
	return datatype().sizeOf();
}

HoldType Block1::holdtype() const {
	return HOLD_1;
}

Berr Block1::load() {
	if (node()->isConstant())
		fixValue(node()->value);
	else // not constant
		read();
	return 0;
}

Berr Block1::store() {
	if (node()->isReduction())
		return 0; // not for reductions

	assert(not getValue().isNone());
	assert(isFixed());

	write();
	return 0;
}

Berr Block1::init() {
	if (not node()->isReduction() || isFixed())
		return 0; // initialize only non-fixed reductions

	auto max_out_block = Runtime::getConfig().max_out_block;
	int offset = sizeof(double) * (max_out_block*Tid.rnk() + order);
	size_t dtsz = datatype().sizeOf();
	size_t size = dtsz;

	{ // Fill just 1 value
		TimedRegion region(Runtime::getClock(),SEND);
		cle::Queue que = Runtime::getOclEnv().D(Tid.dev()).Q(Tid.rnk());

		setValue( node()->initialValue() );
		auto val = getValue();

		cl_int err = clEnqueueFillBuffer(*que,scalar_page,&val.ref(),dtsz,offset,size,0,nullptr,nullptr);
		cle::clCheckError(err);
	}

	return 0;
}

Berr Block1::reduce() {
	if (not node()->isReduction() || isFixed())
		return 0; // only for non-fixed reductions

	assert(not getValue().isNone());

	auto max_out_block = Runtime::getConfig().max_out_block;
	int offset = sizeof(double) * (max_out_block*Tid.rnk() + order);
	size_t size = datatype().sizeOf();

	{
		TimedRegion region(Runtime::getClock(),RECV);
		cle::Queue que = Runtime::getOclEnv().D(Tid.dev()).Q(Tid.rnk());

		auto val = getValue();

		cl_int clerr = clEnqueueReadBuffer(*que,scalar_page,CL_TRUE,offset,size,&val.ref(),0,nullptr,nullptr);
		cle::clCheckError(clerr);

		setValue(val);
	}

	write(); //
	return 0;
}

Berr Block1::preLoad() {
	assert(getValue().isNone());

	if (node()->isConstant())
	{
		fixValue(node()->value);
	}
	else // not constant
	{
		read();
		send();
	}

	return 0;
}

Berr Block1::send() {
	// Scalar values are sent to the device as kernel arguments
}

Berr Block1::recv() {
	TimedRegion region(Runtime::getClock(),RECV);
	cle::Queue que = Runtime::getOclEnv().D(Tid.dev()).Q(Tid.rnk());

	cl_int clerr = clEnqueueReadBuffer(*que, entry->dev_mem, CL_TRUE, 0, size(), host_mem, 0, nullptr, nullptr);
	cle::clCheckError(clerr);
	return 0;
}

Berr Block1::read() {
	file->readBlock(this);
	return 0;
}

Berr Block1::write() {
	file->writeBlock(this);
	return 0;
}

std::shared_ptr<IFile> Block1::getFile() const {
	return file;
}

void Block1::setFile(std::shared_ptr<IFile> file) {
	this->file = file;
}

void* Block1::getHostMem() const {
	return host_mem;
}

void Block1::setHostMem(void *host_mem) {
	this->host_mem = host_mem;
}

void Block1::unsetHostMem() {
	this->host_mem = nullptr;
}

void* Block1::getDevMem() {
	return scalar_page;
}

//CellStats Block1::getStats() const {
//	return CellStats();
//}

void Block1::setStats(CellStats sta) {
	// nothing to do
}

VariantType Block1::getValue() const {
	return value;
}

void Block1::setValue(VariantType val) {
	this->value = val;
}

bool Block1::isFixed() const {
	return fixed;
}

void Block1::fixValue(VariantType val) {
	assert(not val.isNone());
	fixed = true;
	ready = true;
	value = val;
}


} } // namespace map::detail
