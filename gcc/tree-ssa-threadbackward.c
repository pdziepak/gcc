/* SSA Jump Threading
   Copyright (C) 2005-2016 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "predict.h"
#include "tree.h"
#include "gimple.h"
#include "fold-const.h"
#include "cfgloop.h"
#include "gimple-iterator.h"
#include "tree-cfg.h"
#include "tree-ssa-threadupdate.h"
#include "params.h"
#include "tree-ssa-loop.h"
#include "cfganal.h"
#include "tree-pass.h"

static int max_threaded_paths;

/* Simple helper to get the last statement from BB, which is assumed
   to be a control statement.   Return NULL if the last statement is
   not a control statement.  */

static gimple *
get_gimple_control_stmt (basic_block bb)
{
  gimple_stmt_iterator gsi = gsi_last_nondebug_bb (bb);

  if (gsi_end_p (gsi))
    return NULL;

  gimple *stmt = gsi_stmt (gsi);
  enum gimple_code code = gimple_code (stmt);
  if (code == GIMPLE_COND || code == GIMPLE_SWITCH || code == GIMPLE_GOTO)
    return stmt;
  return NULL;
}

/* Return true if the CFG contains at least one path from START_BB to END_BB.
   When a path is found, record in PATH the blocks from END_BB to START_BB.
   VISITED_BBS is used to make sure we don't fall into an infinite loop.  Bound
   the recursion to basic blocks belonging to LOOP.  */

static bool
fsm_find_thread_path (basic_block start_bb, basic_block end_bb,
		      vec<basic_block, va_gc> *&path,
		      hash_set<basic_block> *visited_bbs, loop_p loop)
{
  if (loop != start_bb->loop_father)
    return false;

  if (start_bb == end_bb)
    {
      vec_safe_push (path, start_bb);
      return true;
    }

  if (!visited_bbs->add (start_bb))
    {
      edge e;
      edge_iterator ei;
      FOR_EACH_EDGE (e, ei, start_bb->succs)
	if (fsm_find_thread_path (e->dest, end_bb, path, visited_bbs, loop))
	  {
	    vec_safe_push (path, start_bb);
	    return true;
	  }
    }

  return false;
}

/* We trace the value of the SSA_NAME NAME back through any phi nodes looking
   for places where it gets a constant value and save the path.  Stop after
   having recorded MAX_PATHS jump threading paths.  */

static void
fsm_find_control_statement_thread_paths (tree name,
					 hash_set<basic_block> *visited_bbs,
					 vec<basic_block, va_gc> *&path,
					 bool seen_loop_phi)
{
  /* If NAME appears in an abnormal PHI, then don't try to trace its
     value back through PHI nodes.  */
  if (SSA_NAME_OCCURS_IN_ABNORMAL_PHI (name))
    return;

  gimple *def_stmt = SSA_NAME_DEF_STMT (name);
  basic_block var_bb = gimple_bb (def_stmt);

  if (var_bb == NULL)
    return;

  /* For the moment we assume that an SSA chain only contains phi nodes, and
     eventually one of the phi arguments will be an integer constant.  In the
     future, this could be extended to also handle simple assignments of
     arithmetic operations.  */
  if (gimple_code (def_stmt) != GIMPLE_PHI)
    return;

  /* Avoid infinite recursion.  */
  if (visited_bbs->add (var_bb))
    return;

  gphi *phi = as_a <gphi *> (def_stmt);
  int next_path_length = 0;
  basic_block last_bb_in_path = path->last ();

  if (loop_containing_stmt (phi)->header == gimple_bb (phi))
    {
      /* Do not walk through more than one loop PHI node.  */
      if (seen_loop_phi)
	return;
      seen_loop_phi = true;
    }

  /* Following the chain of SSA_NAME definitions, we jumped from a definition in
     LAST_BB_IN_PATH to a definition in VAR_BB.  When these basic blocks are
     different, append to PATH the blocks from LAST_BB_IN_PATH to VAR_BB.  */
  if (var_bb != last_bb_in_path)
    {
      edge e;
      int e_count = 0;
      edge_iterator ei;
      vec<basic_block, va_gc> *next_path;
      vec_alloc (next_path, 10);

      /* When VAR_BB == LAST_BB_IN_PATH, then the first block in the path
	 will already be in VISITED_BBS.  When they are not equal, then we
	 must ensure that first block is accounted for to ensure we do not
	 create bogus jump threading paths.  */
      visited_bbs->add ((*path)[0]);
      FOR_EACH_EDGE (e, ei, last_bb_in_path->preds)
	{
	  hash_set<basic_block> *visited_bbs = new hash_set<basic_block>;

	  if (fsm_find_thread_path (var_bb, e->src, next_path, visited_bbs,
				    e->src->loop_father))
	    ++e_count;

	  delete visited_bbs;

	  /* If there is more than one path, stop.  */
	  if (e_count > 1)
	    {
	      vec_free (next_path);
	      return;
	    }
	}

      /* Stop if we have not found a path: this could occur when the recursion
	 is stopped by one of the bounds.  */
      if (e_count == 0)
	{
	  vec_free (next_path);
	  return;
	}

      /* Make sure we haven't already visited any of the nodes in
	 NEXT_PATH.  Don't add them here to avoid pollution.  */
      for (unsigned int i = 0; i < next_path->length () - 1; i++)
	{
	  if (visited_bbs->contains ((*next_path)[i]))
	    {
	      vec_free (next_path);
	      return;
	    }
	}

      /* Now add the nodes to VISISTED_BBS.  */
      for (unsigned int i = 0; i < next_path->length () - 1; i++)
	visited_bbs->add ((*next_path)[i]);

      /* Append all the nodes from NEXT_PATH to PATH.  */
      vec_safe_splice (path, next_path);
      next_path_length = next_path->length ();
      vec_free (next_path);
    }

  gcc_assert (path->last () == var_bb);

  /* Iterate over the arguments of PHI.  */
  unsigned int i;
  for (i = 0; i < gimple_phi_num_args (phi); i++)
    {
      tree arg = gimple_phi_arg_def (phi, i);
      basic_block bbi = gimple_phi_arg_edge (phi, i)->src;

      /* Skip edges pointing outside the current loop.  */
      if (!arg || var_bb->loop_father != bbi->loop_father)
	continue;

      if (TREE_CODE (arg) == SSA_NAME)
	{
	  vec_safe_push (path, bbi);
	  /* Recursively follow SSA_NAMEs looking for a constant definition.  */
	  fsm_find_control_statement_thread_paths (arg, visited_bbs, path,
						   seen_loop_phi);

	  path->pop ();
	  continue;
	}

      if (TREE_CODE (arg) != INTEGER_CST)
	continue;

      /* Note BBI is not in the path yet, hence the +1 in the test below
	 to make sure BBI is accounted for in the path length test.  */
      int path_length = path->length ();
      if (path_length + 1 > PARAM_VALUE (PARAM_MAX_FSM_THREAD_LENGTH))
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file, "FSM jump-thread path not considered: "
		     "the number of basic blocks on the path "
		     "exceeds PARAM_MAX_FSM_THREAD_LENGTH.\n");
	  continue;
	}

      if (max_threaded_paths <= 0)
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file, "FSM jump-thread path not considered: "
		     "the number of previously recorded FSM paths to thread "
		     "exceeds PARAM_MAX_FSM_THREAD_PATHS.\n");
	  continue;
	}

      /* Add BBI to the path.  */
      vec_safe_push (path, bbi);
      ++path_length;

      int n_insns = 0;
      gimple_stmt_iterator gsi;
      int j;
      loop_p loop = (*path)[0]->loop_father;
      bool path_crosses_loops = false;
      bool threaded_through_latch = false;
      bool multiway_branch_in_path = false;
      bool threaded_multiway_branch = false;

      /* Count the number of instructions on the path: as these instructions
	 will have to be duplicated, we will not record the path if there are
	 too many instructions on the path.  Also check that all the blocks in
	 the path belong to a single loop.  */
      for (j = 0; j < path_length; j++)
	{
	  basic_block bb = (*path)[j];

	  /* Remember, blocks in the path are stored in opposite order
	     in the PATH array.  The last entry in the array reprensents
	     the block with an outgoing edge that we will redirect to the
	     jump threading path.  Thus we don't care about that block's
	     loop father, nor how many statements are in that block because
	     it will not be copied or whether or not it ends in a multiway
	     branch.  */
	  if (j < path_length - 1)
	    {
	      if (bb->loop_father != loop)
		{
		  path_crosses_loops = true;
		  break;
		}

	      for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
		{
		  gimple *stmt = gsi_stmt (gsi);
		  /* Do not count empty statements and labels.  */
		  if (gimple_code (stmt) != GIMPLE_NOP
		      && gimple_code (stmt) != GIMPLE_LABEL
		      && !(gimple_code (stmt) == GIMPLE_ASSIGN
			   && gimple_assign_rhs_code (stmt) == ASSERT_EXPR)
		      && !is_gimple_debug (stmt))
		    ++n_insns;
		}

	      gphi_iterator gsip;
	      for (gsip = gsi_start_phis (bb);
		   !gsi_end_p (gsip);
		   gsi_next (&gsip))
		{
		  gphi *phi = gsip.phi ();
		  tree dst = gimple_phi_result (phi);

		  /* We consider any non-virtual PHI as a statement since it
		     count result in a constant assignment or copy
		     operation.  */
		  if (!virtual_operand_p (dst))
		    ++n_insns;
		}

	      /* We do not look at the block with the threaded branch
		 in this loop.  So if any block with a last statement that
		 is a GIMPLE_SWITCH or GIMPLE_GOTO is seen, then we have a
		 multiway branch on our path.

		 The block in PATH[0] is special, it's the block were we're
		 going to be able to eliminate its branch.  */
	      gimple *last = last_stmt (bb);
	      if (last && (gimple_code (last) == GIMPLE_SWITCH
			   || gimple_code (last) == GIMPLE_GOTO))
		{
		  if (j == 0)
		    threaded_multiway_branch = true;
		  else
		    multiway_branch_in_path = true;
		}
	    }

	  /* Note if we thread through the latch, we will want to include
	     the last entry in the array when determining if we thread
	     through the loop latch.  */
	  if (loop->latch == bb)
	    threaded_through_latch = true;
	}

      gimple *stmt = get_gimple_control_stmt ((*path)[0]);
      gcc_assert (stmt);
      /* We have found a constant value for ARG.  For GIMPLE_SWITCH
	 and GIMPLE_GOTO, we use it as-is.  However, for a GIMPLE_COND
	 we need to substitute, fold and simplify so we can determine
	 the edge taken out of the last block.  */
      if (gimple_code (stmt) == GIMPLE_COND)
	{
	  enum tree_code cond_code = gimple_cond_code (stmt);

	  /* We know the underyling format of the condition.  */
	  arg = fold_binary (cond_code, boolean_type_node,
			     arg, gimple_cond_rhs (stmt));
	}

      /* If this path threaded through the loop latch back into the
	 same loop and the destination does not dominate the loop
	 latch, then this thread would create an irreducible loop.

	 We have to know the outgoing edge to figure this out.  */
      edge taken_edge = find_taken_edge ((*path)[0], arg);
      bool creates_irreducible_loop = false;
      if (threaded_through_latch
	  && loop == taken_edge->dest->loop_father
	  && (determine_bb_domination_status (loop, taken_edge->dest)
	      == DOMST_NONDOMINATING))
	creates_irreducible_loop = true;

      if (path_crosses_loops)
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file, "FSM jump-thread path not considered: "
		     "the path crosses loops.\n");
	  path->pop ();
	  continue;
	}

      if (n_insns >= PARAM_VALUE (PARAM_MAX_FSM_THREAD_PATH_INSNS))
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file, "FSM jump-thread path not considered: "
		     "the number of instructions on the path "
		     "exceeds PARAM_MAX_FSM_THREAD_PATH_INSNS.\n");
	  path->pop ();
	  continue;
	}

      /* We avoid creating irreducible loops unless we thread through
	 a multiway branch, in which case we have deemed it worth losing other
	 loop optimizations later.  */
      if (!threaded_multiway_branch && creates_irreducible_loop)
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file,
		     "FSM would create irreducible loop without threading "
		     "multiway branch.\n");
	  path->pop ();
	  continue;
	}

      /* When there is a multi-way branch on the path, then threading can
	 explode the CFG due to duplicating the edges for that multi-way
	 branch.  So like above, only allow a multi-way branch on the path
	 if we actually thread a multi-way branch.  */
      if (!threaded_multiway_branch && multiway_branch_in_path)
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file,
		     "FSM Thread through multiway branch without threading "
		     "a multiway branch.\n");
	  path->pop ();
	  continue;
	}

      vec<jump_thread_edge *> *jump_thread_path
	= new vec<jump_thread_edge *> ();

      /* Record the edges between the blocks in PATH.  */
      for (j = 0; j < path_length - 1; j++)
	{
	  edge e = find_edge ((*path)[path_length - j - 1],
			      (*path)[path_length - j - 2]);
	  gcc_assert (e);
	  jump_thread_edge *x = new jump_thread_edge (e, EDGE_FSM_THREAD);
	  jump_thread_path->safe_push (x);
	}

      /* Add the edge taken when the control variable has value ARG.  */
      jump_thread_edge *x
	= new jump_thread_edge (taken_edge, EDGE_NO_COPY_SRC_BLOCK);
      jump_thread_path->safe_push (x);

      register_jump_thread (jump_thread_path);
      --max_threaded_paths;

      /* Remove BBI from the path.  */
      path->pop ();
    }

  /* Remove all the nodes that we added from NEXT_PATH.  */
  if (next_path_length)
    vec_safe_truncate (path, (path->length () - next_path_length));
}

/* Search backwards from BB looking for paths where NAME (an SSA_NAME)
   is a constant.  Record such paths for jump threading.

   It is assumed that BB ends with a control statement and that by
   finding a path where NAME is a constant, we can thread the path.  */

void  
find_jump_threads_backwards (edge e)
{     
  if (!flag_expensive_optimizations
      || optimize_function_for_size_p (cfun)
      || e->dest->loop_father != e->src->loop_father
      || loop_depth (e->dest->loop_father) == 0)
    return;

  gimple *stmt = get_gimple_control_stmt (e->dest);
  if (!stmt)
    return;

  enum gimple_code code = gimple_code (stmt);
  tree name = NULL;
  if (code == GIMPLE_SWITCH)
    name = gimple_switch_index (as_a <gswitch *> (stmt));
  else if (code == GIMPLE_GOTO)
    name = gimple_goto_dest (stmt);
  else if (code == GIMPLE_COND)
    {
      if (TREE_CODE (gimple_cond_lhs (stmt)) == SSA_NAME
	  && TREE_CODE (gimple_cond_rhs (stmt)) == INTEGER_CST
	  && (INTEGRAL_TYPE_P (TREE_TYPE (gimple_cond_lhs (stmt)))
	      || POINTER_TYPE_P (TREE_TYPE (gimple_cond_lhs (stmt)))))
	name = gimple_cond_lhs (stmt);
    }

  if (!name || TREE_CODE (name) != SSA_NAME)
    return;

  vec<basic_block, va_gc> *bb_path;
  vec_alloc (bb_path, 10);
  vec_safe_push (bb_path, e->dest);
  hash_set<basic_block> *visited_bbs = new hash_set<basic_block>;

  max_threaded_paths = PARAM_VALUE (PARAM_MAX_FSM_THREAD_PATHS);
  fsm_find_control_statement_thread_paths (name, visited_bbs, bb_path, false);

  delete visited_bbs;
  vec_free (bb_path);
}
