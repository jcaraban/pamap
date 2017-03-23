/**
 * @file	LhsAccess.hpp 
 * @author	Jesús Carabaño Bravo <jcaraban@abo.fi>
 *
 * Node representing an LhsAccess operation
 *
 * Note: was and could be named 'Blend', 'Compose', 'Combine'
 */

#ifndef MAP_RUNTIME_DAG_LHSACCESS_HPP_
#define MAP_RUNTIME_DAG_LHSACCESS_HPP_

#include "Node.hpp"


namespace map { namespace detail {

struct LhsAccess : public Node
{
	// Internal declarations
	struct Key {
		Key(LhsAccess *node);
		bool operator==(const Key& k) const;
		Node *lprev, *rprev;
		Coord coord;
	};
	struct Hash {
		std::size_t operator()(const Key& k) const;
	};

	// Factory
	static Node* Factory(Node *lhs, Node *rhs, const Coord &coord);
	Node* clone(NodeList new_prev_list, NodeList new_back_list);

	// Constructors
	LhsAccess(const MetaData &meta, Node *lprev, Node *rprev, const Coord &coord);
	LhsAccess(const LhsAccess *other, NodeList new_prev_list, NodeList new_back_list);

	// Methods
	void accept(Visitor *visitor);
	std::string getName() const;
	std::string signature() const;
	char classSignature() const;
	//Node*& left();
	Node* left() const;
	//Node*& right();
	Node* right() const;
	Coord coord() const;
	Pattern pattern() const { return LOCAL; }

	// Variables
	Coord cell_coord; //!< LhsAccess coordinate
};

} } // namespace map::detail

#endif
