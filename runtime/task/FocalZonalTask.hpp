/**
 * @file    FocalZonalTask.hpp 
 * @author  Jesús Carabaño Bravo <jcaraban@abo.fi>
 *
 */

#ifndef MAP_RUNTIME_TASK_FOCAL_ZONAL_HPP_
#define MAP_RUNTIME_TASK_FOCAL_ZONAL_HPP_

#include "Task.hpp"


namespace map { namespace detail {

struct FocalZonalTask : public Task
{
	FocalZonalTask(Group *group);

	void createVersions();

	void blocksToLoad(Coord coord, InKeyList &in_keys) const;
	void blocksToStore(Coord coord, OutKeyList &out_keys) const;

	void initialJobs(std::vector<Job> &job_vec);
	void selfJobs(Job done_job, std::vector<Job> &job_vec);
	void nextJobs(Key done_block, std::vector<Job> &job_vec);
	
	int selfInterDepends(Node *node, Coord coord) const;
	int nextInterDepends(Node *node, Coord coord) const;
	int selfIntraDepends(Node *node, Coord coord) const;
	int nextIntraDepends(Node *node, Coord coord) const;

	void preCompute(Coord coord, const BlockList &in_blk, const BlockList &out_blk);
	void postCompute(Coord coord, const BlockList &in_blk, const BlockList &out_blk);
	void compute(Coord coord, const BlockList &in_blk, const BlockList &out_blk);
	
	Pattern pattern() const { return FOCAL + ZONAL; }
};

} } // namespace map::detail

#endif
