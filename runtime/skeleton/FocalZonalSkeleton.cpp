/**
 * @file	FocalZonalSkeleton.cpp 
 * @author	Jesús Carabaño Bravo <jcaraban@abo.fi>
 *
 */

#include "FocalZonalSkeleton.hpp"
#include "util.hpp"
#include "../Version.hpp"
#include "../task/Task.hpp"
#include <iostream>
#include <functional>


namespace map { namespace detail {

namespace { // anonymous namespace
	using std::string;
}

/***************
   Constructor
 ***************/

FocalZonalSkeleton::FocalZonalSkeleton(Version *ver)
	: Skeleton(ver)
	, mask()
	, conv()
	, func()
	, percent()
	, flow()
	, halo()
	, reduc()
	, zonal_code()
{
	indent_count = 2;
	level = 0;
}

void FocalZonalSkeleton::generate() {
	fill(); // fill structures
	compact(); // compact structures

	ver->shared_size = -1;
	ver->group_size = BlockSize{16,16}; // @
	ver->num_group = (ver->task->blocksize() - 1) / ver->groupsize() + 1;
	ver->code = versionCode();

	// Gives numgroup() as extra_argument
	for (int i=0; i<ver->task->numdim().toInt(); i++)
		ver->extra_arg.push_back( ver->numgroup()[i] );
}

/***********
   Methods
 ***********/

void FocalZonalSkeleton::compact() {
	Skeleton::compact();
	//sort_unique(mask,node_id_less(),node_id_equal());
	sort_unique(conv,node_id_less(),node_id_equal());
	sort_unique(func,node_id_less(),node_id_equal());
	sort_unique(percent,node_id_less(),node_id_equal());
	sort_unique(flow,node_id_less(),node_id_equal());
}

string FocalZonalSkeleton::versionCode() {
	//// Variables ////
	const int N = 2;

	//// Header ////
	indent_count = 0;

	// Includes
	for (auto &incl : includes)
		add_line( "#include " + incl );
	add_line( "" );

	// Adding definitions and utilities
	add_section( defines_local() );
	add_line( "" );
	add_section( defines_focal() );
	add_line( "" );

	std::vector<bool> added_L(N_DATATYPE,false);
	std::vector<bool> added_F(N_DATATYPE,false);
	for (auto &node : ver->task->inputList()) {
		DataType dt = node->datatype();
		if (!added_F[dt.get()] && isInputOf(node,ver->task->group()).is(FOCAL)) {
			add_section( defines_focal_type(dt) );
			added_F[dt.get()] = true;
			add_line( "" );
		} else if (!added_L[dt.get()] && isInputOf(node,ver->task->group()).is(LOCAL)) {
			add_section( defines_local_type(dt) );
			added_L[dt.get()] = true;
			add_line( "" );
		}
	}

	if (!flow.empty()) {
		add_section( defines_focal_flow() );
		add_line( "" );
	}

	std::vector<bool> added_Z(N_REDUCTION,false);
	for (auto &node : reduc) {
		if (!added_Z[node->type.get()]) {
			add_section( defines_zonal_reduc(node->type,node->datatype()) );
			added_Z[node->type.get()] = true;
		}
	}
	add_line( "" );
	
	// Signature
	add_line( kernel_sign(ver->signature()) );

	// Arguments
	add_line( "(" );
	indent_count++;
	for (auto &node : ver->task->inputList()) { // keeps the order IN_0, IN_8, ...
		if (tag_hash[node] == PRECORE && node->numdim() != D0)
			add_line( "TYPE_VAR_LIST(" + node->datatype().ctypeString() + ",IN_" + node->id + ")," );
		else if (tag_hash[node] == POSCORE || node->numdim() == D0)
			add_line( in_arg(node) );
	}
	for (auto &node : ver->task->outputList()) {
		add_line( out_arg(node) );
	}
	for (int n=0; n<N; n++) {
		add_line( string("const int BS") + n + "," );
	}
	for (int n=0; n<N; n++) {
		add_line( string("const int BC") + n + "," );
	}
	for (int n=0; n<N; n++) {
		add_line( string("const int _GS") + n + "," + " // @" );
	}
	for (int n=0; n<N; n++) {
		string comma = (n < N-1) ? "," : "";
		add_line( string("const int GN") + n + comma );
	}
	indent_count--;
	add_line( ")" );

	add_line( "{" ); // Opens kernel body

	//// Declarations ////
	indent_count++;

	// Declaring scalars
	for (int i=F32; i<N_DATATYPE; i++) {
		if (!scalar[i].empty()) {
			add_line( scalar_decl(scalar[i],static_cast<DataTypeEnum>(i)) );
		}
	}

	BlockSize full_halo = {0,0};
	for (auto h : halo)
		full_halo += h;
	
	// Declaring focal shared memory
	for (auto &node : shared) {
		add_line( shared_decl(node,prod(ver->groupsize()+2*full_halo)) );
	}

	// Declaring zonal shared memory
	for (auto &node : reduc) {
		add_line( shared_decl(node,prod(ver->groupsize())) );
	}

	add_line( "" );

	// Declaring indexing variables
	for (int n=0; n<N; n++) {
		add_line( string("int gc")+n+" = get_local_id("+n+");" );
	}
	for (int n=0; n<N; n++) {
		add_line( string("int GC")+n+" = get_group_id("+n+");" );
	}
	for (int n=0; n<N; n++) {
		add_line( string("int bc")+n+" = get_global_id("+n+");" );
	}
	for (int n=0; n<N; n++) {
		add_line( std::string("int H")+n + " = " + halo_sum(n,halo) + ";" );
	}
	for (int n=0; n<N; n++) {
		add_line( string("int GS")+n+" = 16; // @" );
	}

	add_line( "" );

	// Decaring masks
	for (auto &pair : mask) {
		add_line( mask_decl(pair.first,pair.second) );
	}

	add_line( "" );

	//// Previous to focal core ////
	add_line( "// Previous to FOCAL core\n" );

	// Global-if
	add_line( "if ("+global_cond(N)+") {" );
	indent_count++;

	// Load-loop
	add_line( "for ("+pre_load_loop(N)+")" );
	add_line( "{" );
	indent_count++;

	// Displaced indexing variables
	add_line( "int proj = "+local_proj(N)+" + i*("+group_size_prod(N)+");" );
	add_line( "if (proj >= "+group_size_prod_H(N)+") continue;" );
	for (int n=0; n<N; n++) {
		add_line( string("int gc")+n+" = proj % ("+group_size_prod_H(n+1)+") / "+group_size_prod_H(n)+";" );
	}
	for (int n=0; n<N; n++) {
		add_line( string("int bc")+n+" = get_group_id("+n+")*GS"+n+" + gc"+n+" - H"+n+";" );
	}
	add_line( "" );

	// Adds PRECORE input-nodes
	for (auto &node : ver->task->inputList()) {
		if (tag_hash[node] == PRECORE) {
			if (node->numdim() == D0) { // @ erroneus, needs two PRECORE to differentiate FOCAL / ZONAL
				add_line( var_name(node) + " = " + in_var(node) + ";" );	
			} else {
				add_line( var_name(node) + " = " + in_var_focal(node) + ";" );
			}
		}
	}

	// Adds accumulated 'precore' to 'all'
	code[ALL_POS] += code[PRECORE];

	// Filling focal shared memory
	for (auto &node : shared) {
		string svar = var_name(node,SHARED) + "[" + local_proj_focal(N) + "]";
		string var = var_name(node);
		add_line( svar + " = " + var + ";" );
	}
	
	// Closes load-loop
	indent_count--;
	add_line( "}" );
	// Closes global-if
	indent_count--;
	add_line( "}" );
	// Synchronizes
	add_line( "barrier(CLK_LOCAL_MEM_FENCE);" );
	add_line( "" );

	//// Focal core ////
	add_line( "// FOCAL core\n" );

	// Global-if
	add_line( "if ("+global_cond(N)+")" );
	add_line( "{" );
	indent_count++;

	// Adds accumulated 'core' to 'all'
	code[ALL_POS] += code[CORE];

	indent_count--;
	add_line( "}" ); // Closes global-if
	add_line( "" );

	//// Posterior to focal core ////
	add_line( "// Posterior to FOCAL core\n" );

	// Global-if
	add_line( "if ("+global_cond(N)+") {" );
	indent_count++;

	// Adds POSCORE input-nodes
	for (auto &node : ver->task->inputList()) {
		if (tag_hash[node] != POSCORE)
			continue;
		if (is_included(node,shared)) {
			add_line( var_name(node) + " = " + var_name(node,SHARED) + "[" + local_proj_focal_H(N) + "];" );
		} else if (tag_hash[node] == PRECORE) {
			add_line( var_name(node) + " = IN_" + node->id + "[" + global_proj(N) + "];" );
		} else {
			add_line( var_name(node) + " = " + in_var(node) + ";" );
		}
	}

	// Adds accumulated 'poscore' to 'all'
	code[ALL_POS] += code[POSCORE];

	// Adds POSCORE output-nodes
	for (auto &node : ver->task->outputList()) {
		if (tag_hash[node]==POSCORE && node->pattern().isNot(ZONAL) && node->numdim() != D0) {
			add_line( out_var(node) + " = " + var_name(node) + ";" );
		}
	}

	indent_count--;
	add_line( "}" ); // Closes global-if
	add_line( "" );

	//// Previous to zonal core ////
	add_line( "// Previous to ZONAL core\n" );

	// Global-if
	add_line( "if ("+global_cond(N)+")" );
	add_line( "{" );
	indent_count++;

	// Filling shared memory
	for (auto &node : reduc) {
		string svar = var_name(node,SHARED) + "[" + local_proj_zonal(N) + "]";
		string var = var_name(node->prev());
		add_line( svar + " = " + var + ";" );
	}

	indent_count--;
	add_line( "}" );
	add_line( "else" );
	add_line( "{" );
	indent_count++;

	// Filling shared memory, corner cases with neutral element
	for (auto &node : reduc) {
		string svar = var_name(node,SHARED) + "[" + local_proj_zonal(N) + "]";
		string neutral = node->type.neutralString( node->datatype() );
		add_line( svar + " = " + neutral + ";" );
	}

	// Closes global-if and synchronizes
	indent_count--;
	add_line( "}" );
	add_line( "" );

	//// Zonal Core ////
	add_line( "// Zonal core\n" );

	// Zonal-loop
	add_line( "for (int i="+group_size_prod(N)+"/2; i>0; i/=2) {" );
	indent_count++;
	add_line( "barrier(CLK_LOCAL_MEM_FENCE);" );
	add_line( "if ("+local_proj_zonal(N)+" < i)" );
	add_line( "{" );
	indent_count++;
	
	// Adds accumulated 'core' to 'all'
	code[ALL_POS] += zonal_code;

	indent_count--;
	add_line( "}" ); // Closes if
	indent_count--;
	add_line( "}" ); // Closes for
	add_line( "" );

	// Write-if
	add_line( "if ("+local_cond_zonal(N)+")" );
	add_line( "{" );
	indent_count++;

	// Zonal output
	for (auto &node : reduc) {
		string atomic = "atomic" + node->type.toString();
		string var = var_name(node,SHARED) + "["+local_proj_zonal(N)+"]";
		string ovar = string("OUT_") + node->id + "+idx_" + node->id;
		add_line( atomic + "( " + ovar + " , " + var + ");" );
	}

	indent_count--;
	add_line( "}" ); // Closes write-if
	add_line( "" );

	indent_count--;
	add_line( "}" ); // Closes kernel body

	//// Printing ////
	std::cout << "***\n" << code[ALL_POS] << "***" << std::endl;

	return code[ALL_POS];
}

/*********
   Visit
 *********/

void FocalZonalSkeleton::visit(Neighbor *node) {
	// Adds Neighbor code
	{
		const int N = node->numdim().toInt();
		Coord nbh = node->coord();
		string var = var_name(node);
		string svar = var_name(node->prev(),SHARED) + "[" + local_proj_focal_nbh(N,nbh) + "]";

		add_line( var + " = " + svar + ";" );
	}

	if (halo.size() > level) {
		halo[level] = cond(node->halo() > halo[level], node->halo(), halo[level]);
	} else {
		halo.push_back(node->halo());
	}

	shared.push_back(node->prev());
}

void FocalZonalSkeleton::visit(Convolution *node) {
	// Adds convolution code
	{
		const int N = node->numdim().toInt();
		string var = var_name(node);
		string svar = var_name(node->prev(),SHARED) + "[" + local_proj_focal_Hi(N) + "]";
		string mvar = node->mask().datatype().toString() + "L_" + std::to_string(node->id);

		add_line( var + " = 0;" );

		for (int n=N-1; n>=0; n--) {
			int h = node->halo()[n];
			string i = string("i") + n;
			add_line( "for (int "+i+"=-"+h+"; "+i+"<="+h+"; "+i+"++) {" );
			indent_count++;
		}

		for (int n=N-1; n>=0; n--)
			mvar += string("[")+"i"+n+"+H"+n+"]";

		add_line( var + " += " + svar + " * " + mvar + ";" );

		for (int n=N-1; n>=0; n--) {
			indent_count--;
			add_line( "}" );
		}
	}

	if (halo.size() > level) {
		halo[level] = cond(node->halo() > halo[level], node->halo(), halo[level]);
	} else {
		halo.push_back(node->halo());
	}

	shared.push_back(node->prev());
	mask.push_back( std::make_pair(node->mask(),node->id) );
}

void FocalZonalSkeleton::visit(FocalFunc *node) {
	// Adds focal code
	{
		const int N = node->numdim().toInt();
		string var = var_name(node);
		string svar = var_name(node->prev(),SHARED) + "[" + local_proj_focal_Hi(N) + "]";

		add_line( var + " = " + node->type.neutralString(node->datatype()) + ";" );

		for (int n=N-1; n>=0; n--) {
			string i = string("i") + n;
			add_line( "for (int "+i+"=0; "+i+"<"+(node->halo()[n]*2+1)+"; "+i+"++) {" );
			indent_count++;
		}

		if (node->type.isOperator())
			add_line( var + " = " + var + " "+node->type.code()+" " + svar + ";" );
		else if (node->type.isFunction())
			add_line( var + " = " + node->type.code()+"(" + var+"," + svar+")" + ";" );

		for (int n=N-1; n>=0; n--) {
			indent_count--;
			add_line( "}" );
		}
	}
	
	if (halo.size() > level) {
		halo[level] = cond(node->halo() > halo[level], node->halo(), halo[level]);
	} else {
		halo.push_back(node->halo());
	}

	shared.push_back(node->prev());
	func.push_back(node);
}

void FocalZonalSkeleton::visit(FocalPercent *node) {
	// Adds FocalPercent code
	{
		const int N = node->numdim().toInt();
		string var = var_name(node);
		string pvar = var_name(node->prev());
		string svar = var_name(node->prev(),SHARED) + "[" + local_proj_focal_Hi(N) + "]";

		add_line( var + " = 0;" );

		for (int n=N-1; n>=0; n--) {
			string i = string("i") + n;
			add_line( "for (int "+i+"=0; "+i+"<"+(node->halo()[n]*2+1)+"; "+i+"++) {" );
			indent_count++;
		}

		add_line( var + " += (" + pvar + " "+node->type.code()+" " + svar + ");" );

		for (int n=N-1; n>=0; n--) {
			indent_count--;
			add_line( "}" );
		}

		add_line( var + " /= " + nbh_size(N) + ";" );
	}

	shared.push_back(node->prev());
	percent.push_back(node);
}

void FocalZonalSkeleton::visit(FocalFlow *node) {
	// Adds FocalFlow code
	{
		const int N = node->numdim().toInt();
		DataType dt = node->prev()->datatype();
		string dt_str = dt.ctypeString();
		string var = var_name(node);
		string self = var_name(node->prev(),SHARED) + "[" + local_proj_focal_H(N) + "]";
		string nbh = var_name(node->prev(),SHARED) + "[" + local_proj_focal_of(N) + "]";

		add_line( dt_str + " max_"+node->id + " = 0;" );// + ReductionType(MAX).neutralString(dt) + ";" );
		add_line( string("int pos_")+node->id + " = -1;" );
		add_line( "for (int dir=0; dir<N_DIR; dir++) {" );
		indent_count++;
		add_line( "int of0 = offset0[dir];" );
		add_line( "int of1 = offset1[dir];" );
		add_line( dt_str + " zdif = " + self + " - " + nbh + ";" );
		add_line( dt_str + " dist = (dir%2 == 0) ? 1 : 1.414213f;" );
		add_line( dt_str + " drop = zdif / dist;" );
		add_line( string("if (drop > max_")+node->id + ") {" );
		indent_count++;
		add_line( string("max_")+node->id + " = drop;" );
		add_line( string("pos_")+node->id + " = dir;" );
		indent_count--;
		add_line( "}" );
		indent_count--;
		add_line( "}" );
		add_line( var + " = (" + "pos_"+node->id + " == -1) ? 0 : 1 << pos_"+node->id + ";" );
	}
	
	if (halo.size() > level) {
		halo[level] = cond(node->halo() > halo[level], node->halo(), halo[level]);
	} else {
		halo.push_back(node->halo());
	}
	
	shared.push_back(node->prev());
	flow.push_back(node);
}

void FocalZonalSkeleton::visit(ZonalReduc *node) {
	// Adds zonal code
	{
		const int N = node->prev()->numdim().toInt();
		string lvar = var_name(node,SHARED) + "[" + local_proj_zonal(N) + "]";
		string rvar = var_name(node,SHARED) + "[" + local_proj_zonal(N) + " + i]";

		if (node->type.isOperator())
			zonal_code += "\t\t\t" + lvar + " = " + lvar + " " + node->type.code() + " " + rvar + ";\n";
		else if (node->type.isFunction())
			zonal_code += "\t\t\t" + lvar + " = " + node->type.code() + "(" + lvar + ", " + rvar + ");\n";
		else 
			assert(0);
	}

	reduc.push_back(node);
}

} } // namespace map::detail
