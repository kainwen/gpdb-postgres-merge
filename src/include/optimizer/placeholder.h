/*-------------------------------------------------------------------------
 *
 * placeholder.h
 *	  prototypes for optimizer/util/placeholder.c.
 *
 *
<<<<<<< HEAD
 * Portions Copyright (c) 2017, Pivotal Inc.
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
=======
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
>>>>>>> 1084f317702e1a039696ab8a37caf900e55ec8f2
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/placeholder.h,v 1.5 2010/03/28 22:59:33 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLACEHOLDER_H
#define PLACEHOLDER_H

#include "nodes/relation.h"


extern PlaceHolderVar *make_placeholder_expr(PlannerInfo *root, Expr *expr,
					  Relids phrels);
extern PlaceHolderInfo *find_placeholder_info(PlannerInfo *root,
					  PlaceHolderVar *phv);
<<<<<<< HEAD
extern void find_placeholders_in_jointree(PlannerInfo *root);
extern void update_placeholder_eval_levels(PlannerInfo *root,
											  SpecialJoinInfo *new_sjinfo);
extern void add_placeholders_to_base_rels(PlannerInfo *root);
extern void fix_placeholder_input_needed_levels(PlannerInfo *root);
=======
extern void fix_placeholder_eval_levels(PlannerInfo *root);
extern void add_placeholders_to_base_rels(PlannerInfo *root);
>>>>>>> 1084f317702e1a039696ab8a37caf900e55ec8f2
extern void add_placeholders_to_joinrel(PlannerInfo *root,
							RelOptInfo *joinrel);

#endif   /* PLACEHOLDER_H */
