/**
 * @file	Block.hpp 
 * @author	Jesús Carabaño Bravo <jcaraban@abo.fi>
 *
 * TODO: speciallizing Block by 'datatype' and 'holdtype' could reduce the runtime overhead
 */

#ifndef MAP_RUNTIME_BLOCK_HPP_
#define MAP_RUNTIME_BLOCK_HPP_

#include "../Key.hpp"
#include "../Entry.hpp"
#include "../../file/DataStats.hpp"
#include "../../util/util.hpp"
#include "../../cle/OclEnv.hpp"
#include <mutex>


namespace map { namespace detail {

typedef int Berr; // Should be enum
class Node; // forward declaration
class IFile; // forward declaration

struct Block;
typedef std::vector<Block*> BlockList;


/*
 * @class Block
 */
struct Block
{
  // Constructors
	Block();
	Block(Key key, int dep);
	virtual ~Block();

  // Getters
	Node* node() const;
	Coord coord() const;

	StreamDir streamdir() const;
	DataType datatype() const;
	NumDim numdim() const;
	MemOrder memorder() const;

	virtual int size() const;
	virtual HoldType holdtype() const = 0;

  // Methods
	//virtual Berr send();
	virtual Berr recv();

	virtual Berr preLoad();
	virtual Berr load();
	virtual Berr store();
	virtual Berr init();
	virtual Berr reduce();

	virtual bool needEntry() const;
	virtual bool giveEntry() const;

	virtual std::shared_ptr<IFile> getFile() const;
	virtual void setFile(std::shared_ptr<IFile> file);

	virtual void* getHostMem() const;
	virtual void setHostMem(void *host);
	virtual void unsetHostMem();

	virtual void* getDevMem();

	virtual CellStats getStats() const;
	virtual void setStats(CellStats sta);

	virtual VariantType getValue() const;
	virtual void setValue(VariantType val);

	virtual bool isFixed() const;
	virtual void fixValue(VariantType val);

	virtual bool forward() const;
	virtual void forward(bool forward);
	virtual void forwardEntry(Block *out);

	void notify();
	bool discardable() const;

	void setReady();
	void unsetReady();
	bool isReady() const;
	
	void setDirty();
	void unsetDirty();
	bool isDirty() const;

	void setUsed();
	void unsetUsed();
	bool isUsed() const;

  // Variables
	Key key;
	Entry *entry; // @

	int dependencies;
	//HoldType hold_type;

	bool ready; // The data is ready to be used (i.e. loaded in entry)
	bool dirty;
	short used;
	char order;

	std::mutex mtx;
};

} } // namespace map::detail

#endif