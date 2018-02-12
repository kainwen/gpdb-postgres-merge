/*-------------------------------------------------------------------------
 *
 * parse_agg.h
 *	  handle aggregates and window functions in parser
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/parse_agg.h,v 1.43 2010/03/17 16:52:38 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_AGG_H
#define PARSE_AGG_H

#include "parser/parse_node.h"

extern void transformAggregateCall(ParseState *pstate, Aggref *agg,
<<<<<<< HEAD
					   List *args, List *aggorder, bool agg_distinct);
=======
					   List *args, List *aggorder,
					   bool agg_distinct);
>>>>>>> 1084f317702e1a039696ab8a37caf900e55ec8f2
extern void transformWindowFuncCall(ParseState *pstate, WindowFunc *wfunc,
						WindowDef *windef);

extern void parseCheckAggregates(ParseState *pstate, Query *qry);

extern int	get_aggregate_argtypes(Aggref *aggref, Oid *inputTypes);

extern Oid resolve_aggregate_transtype(Oid aggfuncid,
							Oid aggtranstype,
							Oid *inputTypes,
							int numArguments);

extern void build_aggregate_fnexprs(Oid *agg_input_types,
						int agg_num_inputs,
						int agg_num_direct_inputs,
						int num_finalfn_inputs,
						bool agg_variadic,
						Oid agg_state_type,
						Oid agg_result_type,
						Oid transfn_oid,
						Oid finalfn_oid,
						Oid prelimfn_oid,
						Oid invtransfn_oid,
						Oid invprelimfn_oid,
						Expr **transfnexpr,
						Expr **finalfnexpr,
						Expr **prelimfnexpr,
						Expr **invtransfnexpr,
						Expr **invprelimfnexpr);

extern bool checkExprHasGroupExtFuncs(Node *node);

#endif   /* PARSE_AGG_H */
