/**
 * @file    Predictor.hpp 
 * @author  Jesús Carabaño Bravo <jcaraban@abo.fi>
 *
 */

#ifndef MAP_RUNTIME_VISITOR_PREDICTOR_HPP_
#define MAP_RUNTIME_VISITOR_PREDICTOR_HPP_

#include "Visitor.hpp"
#include "../runtime/block/BlockList.hpp"
#include <unordered_map>


namespace map { namespace detail {

#define DECLARE_VISIT(class) virtual void visit(class *node);

/*
 *
 */
struct Predictor : public Visitor
{
  // constructor and main function
	Predictor(Cluster *cluster);
	void predict(Coord coord, const BlockList &in_blk, const BlockList &out_blk);

  // methods
	void clear();

  // vars
	Cluster *cluster;

	std::unordered_map<Key,ValFix,key_hash> hash;
};

#undef DECLARE_VISIT

} } // namespace map::detail

#endif