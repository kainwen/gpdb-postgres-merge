/*-------------------------------------------------------------------------
 *
 * parse_utilcmd.c
 *	  Perform parse analysis work for various utility commands
 *
 * Formerly we did this work during parse_analyze() in analyze.c.  However
 * that is fairly unsafe in the presence of querytree caching, since any
 * database state that we depend on in making the transformations might be
 * obsolete by the time the utility command is executed; and utility commands
 * have no infrastructure for holding locks or rechecking plan validity.
 * Hence these functions are now called at the start of execution of their
 * respective utility commands.
 *
 * NOTE: in general we must avoid scribbling on the passed-in raw parse
 * tree, since it might be in a plan cache.  The simplest solution is
 * a quick copyObject() call before manipulating the query tree.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/parser/parse_utilcmd.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parse_utilcmd.h"
#include "parser/parser.h"
#include "rewrite/rewriteManip.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "catalog/pg_compression.h"
#include "catalog/pg_type_encoding.h"
#include "cdb/cdbhash.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbsreh.h"
#include "cdb/cdbvars.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/memutils.h"

/* State shared by transformCreateStmt and its subroutines */
typedef struct
{
	ParseState *pstate;			/* overall parser state */
	const char *stmtType;		/* "CREATE [FOREIGN] TABLE" or "ALTER TABLE" */
	RangeVar   *relation;		/* relation to create */
	Relation	rel;			/* opened/locked rel, if ALTER */
	List	   *inhRelations;	/* relations to inherit from */
	bool		isforeign;		/* true if CREATE/ALTER FOREIGN TABLE */
	bool		isalter;		/* true if altering existing table */
	List	   *columns;		/* ColumnDef items */
	List	   *ckconstraints;	/* CHECK constraints */
	List	   *fkconstraints;	/* FOREIGN KEY constraints */
	List	   *ixconstraints;	/* index-creating constraints */
	List	   *inh_indexes;	/* cloned indexes from INCLUDING INDEXES */
	List	   *extstats;		/* cloned extended statistics */
	List	   *blist;			/* "before list" of things to do before
								 * creating the table */
	List	   *alist;			/* "after list" of things to do after creating
								 * the table */
	IndexStmt  *pkey;			/* PRIMARY KEY index, if any */
	bool		ispartitioned;	/* true if table is partitioned */
	PartitionBoundSpec *partbound;	/* transformed FOR VALUES */
	bool		ofType;			/* true if statement contains OF typename */

	MemoryContext tempCtx;
} CreateStmtContext;

/* State shared by transformCreateSchemaStmt and its subroutines */
typedef struct
{
	const char *stmtType;		/* "CREATE SCHEMA" or "ALTER SCHEMA" */
	char	   *schemaname;		/* name of schema */
	RoleSpec   *authrole;		/* owner of schema */
	List	   *sequences;		/* CREATE SEQUENCE items */
	List	   *tables;			/* CREATE TABLE items */
	List	   *views;			/* CREATE VIEW items */
	List	   *indexes;		/* CREATE INDEX items */
	List	   *triggers;		/* CREATE TRIGGER items */
	List	   *grants;			/* GRANT items */
} CreateSchemaStmtContext;


static void transformColumnDefinition(CreateStmtContext *cxt,
									  ColumnDef *column);
static void transformTableConstraint(CreateStmtContext *cxt,
									 Constraint *constraint);
static void transformTableLikeClause(CreateStmtContext *cxt,
									 TableLikeClause *table_like_clause,
									 bool forceBareCol, CreateStmt *stmt, List **stenc);
static void transformOfType(CreateStmtContext *cxt,
							TypeName *ofTypename);
static CreateStatsStmt *generateClonedExtStatsStmt(RangeVar *heapRel,
												   Oid heapRelid, Oid source_statsid);
static List *get_collation(Oid collation, Oid actual_datatype);
static List *get_opclass(Oid opclass, Oid actual_datatype);
static void transformIndexConstraints(CreateStmtContext *cxt);
static IndexStmt *transformIndexConstraint(Constraint *constraint,
										   CreateStmtContext *cxt);
static void transformExtendedStatistics(CreateStmtContext *cxt);
static void transformFKConstraints(CreateStmtContext *cxt,
								   bool skipValidation,
								   bool isAddConstraint);
static void transformCheckConstraints(CreateStmtContext *cxt,
									  bool skipValidation);
static void transformConstraintAttrs(CreateStmtContext *cxt,
									 List *constraintList);
static void transformColumnType(CreateStmtContext *cxt, ColumnDef *column);
static void setSchemaName(char *context_schema, char **stmt_schema_name);
static void transformPartitionCmd(CreateStmtContext *cxt, PartitionCmd *cmd);
static List *transformPartitionRangeBounds(ParseState *pstate, List *blist,
										   Relation parent);
static void validateInfiniteBounds(ParseState *pstate, List *blist);

static DistributedBy *getLikeDistributionPolicy(TableLikeClause *e);
static DistributedBy *transformDistributedBy(CreateStmtContext *cxt,
					   DistributedBy *distributedBy,
					   DistributedBy *likeDistributedBy,
					   bool bQuiet);
static List *transformAttributeEncoding(List *stenc, CreateStmt *stmt,
										CreateStmtContext *cxt);
static bool encodings_overlap(List *a, List *b, bool test_conflicts);
static bool is_aocs(const char *access_method);

/*
 * transformCreateStmt -
 *	  parse analysis for CREATE TABLE
 *
 * Returns a List of utility commands to be done in sequence.  One of these
 * will be the transformed CreateStmt, but there may be additional actions
 * to be done before and after the actual DefineRelation() call.
 *
 * SQL allows constraints to be scattered all over, so thumb through
 * the columns and collect all constraints into one place.
 * If there are any implied indices (e.g. UNIQUE or PRIMARY KEY)
 * then expand those into multiple IndexStmt blocks.
 *	  - thomas 1997-12-02
 */
List *
transformCreateStmt(CreateStmt *stmt, const char *queryString)
{
	ParseState *pstate;
	CreateStmtContext cxt;
	List	   *result;
	List	   *save_alist;
	List	   *save_root_partition_alist = NIL;
	ListCell   *elements;
	Oid			namespaceid;
	Oid			existing_relid;
	ParseCallbackState pcbstate;
	bool		is_foreign_table = IsA(stmt, CreateForeignTableStmt);

	DistributedBy *likeDistributedBy = NULL;
	bool		bQuiet = false;		/* shut up transformDistributedBy messages */
	List	   *stenc = NIL;		/* column reference storage encoding clauses */

 	/*
	 * We don't normally care much about the memory consumption of parsing,
	 * because any memory leaked is leaked into MessageContext which is
	 * reset between each command. But if a table is heavily partitioned,
	 * the CREATE TABLE statement can be expanded into hundreds or even
	 * thousands of CreateStmts, so the leaks start to add up. To reduce
	 * the memory consumption, we use a temporary memory context that's
	 * destroyed after processing the CreateStmt for some parts of the
	 * processing.
	 */
	cxt.tempCtx =
		AllocSetContextCreate(CurrentMemoryContext,
							  "CreateStmt analyze context",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * We must not scribble on the passed-in CreateStmt, so copy it.  (This is
	 * overkill, but easy.)
	 */
	stmt = copyObject(stmt);

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * Look up the creation namespace.  This also checks permissions on the
	 * target namespace, locks it against concurrent drops, checks for a
	 * preexisting relation in that namespace with the same name, and updates
	 * stmt->relation->relpersistence if the selected namespace is temporary.
	 */
	setup_parser_errposition_callback(&pcbstate, pstate,
									  stmt->relation->location);
	namespaceid =
		RangeVarGetAndCheckCreationNamespace(stmt->relation, NoLock,
											 &existing_relid);
	cancel_parser_errposition_callback(&pcbstate);

	/*
	 * If the relation already exists and the user specified "IF NOT EXISTS",
	 * bail out with a NOTICE.
	 */
	if (stmt->if_not_exists && OidIsValid(existing_relid))
	{
		ereport(NOTICE,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists, skipping",
						stmt->relation->relname)));
		return NIL;
	}

	/*
	 * If the target relation name isn't schema-qualified, make it so.  This
	 * prevents some corner cases in which added-on rewritten commands might
	 * think they should apply to other relations that have the same name and
	 * are earlier in the search path.  But a local temp table is effectively
	 * specified to be in pg_temp, so no need for anything extra in that case.
	 */
	if (stmt->relation->schemaname == NULL
		&& stmt->relation->relpersistence != RELPERSISTENCE_TEMP)
		stmt->relation->schemaname = get_namespace_name(namespaceid);

	/* Set up CreateStmtContext */
	cxt.pstate = pstate;
	if (IsA(stmt, CreateForeignTableStmt))
	{
		cxt.stmtType = "CREATE FOREIGN TABLE";
		cxt.isforeign = true;
	}
	else
	{
		cxt.stmtType = "CREATE TABLE";
		cxt.isforeign = false;
	}
	cxt.relation = stmt->relation;
	cxt.rel = NULL;
	cxt.inhRelations = stmt->inhRelations;
	cxt.isalter = false;
	cxt.columns = NIL;
	cxt.ckconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
	cxt.inh_indexes = NIL;
	cxt.extstats = NIL;
	cxt.blist = NIL;
	cxt.alist = NIL;
	cxt.pkey = NULL;
	cxt.ispartitioned = stmt->partspec != NULL;
	cxt.partbound = stmt->partbound;
	cxt.ofType = (stmt->ofTypename != NULL);

	Assert(!stmt->ofTypename || !stmt->inhRelations);	/* grammar enforces */

	if (stmt->ofTypename)
		transformOfType(&cxt, stmt->ofTypename);

	if (stmt->partspec)
	{
		if (stmt->inhRelations && !stmt->partbound)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot create partitioned table as inheritance child")));
	}

	/*
	 * Run through each primary element in the table creation clause. Separate
	 * column defs from constraints, and do preliminary analysis.
	 */
	foreach(elements, stmt->tableElts)
	{
		Node	   *element = lfirst(elements);

		switch (nodeTag(element))
		{
			case T_ColumnDef:
				transformColumnDefinition(&cxt, (ColumnDef *) element);
				break;

			case T_Constraint:
				transformTableConstraint(&cxt, (Constraint *) element);
				break;

			case T_TableLikeClause:
			{
				bool            isBeginning = (cxt.columns == NIL);

				transformTableLikeClause(&cxt, (TableLikeClause *) element, false, stmt, &stenc);

				if (Gp_role == GP_ROLE_DISPATCH && isBeginning &&
					stmt->distributedBy == NULL &&
					stmt->inhRelations == NIL)
				{
					likeDistributedBy = getLikeDistributionPolicy((TableLikeClause*) element);
				}
				break;
			}

			case T_ColumnReferenceStorageDirective:
				/* processed below in transformAttributeEncoding() */
				stenc = lappend(stenc, element);
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(element));
				break;
		}
	}

	/*
	 * Transfer anything we already have in cxt.alist into save_alist, to keep
	 * it separate from the output of transformIndexConstraints.  (This may
	 * not be necessary anymore, but we'll keep doing it to preserve the
	 * historical order of execution of the alist commands.)
	 */
	save_alist = cxt.alist;
	cxt.alist = NIL;

	Assert(stmt->constraints == NIL);

	/*
	 * Postprocess constraints that give rise to index definitions.
	 */
	transformIndexConstraints(&cxt);

	/*
	 * Postprocess foreign-key constraints.
	 * But don't cascade FK constraints to parts, yet.
	 */
	transformFKConstraints(&cxt, true, false);

	/*-----------
	 * Analyze attribute encoding clauses.
	 *
	 * Partitioning configurations may have things like:
	 *
	 * CREATE TABLE ...
	 *  ( a int ENCODING (...))
	 * WITH (appendonly=true, orientation=column)
	 * PARTITION BY ...
	 * (PARTITION ... WITH (appendonly=false));
	 *
	 * We don't want to throw an error when we try to apply the ENCODING clause
	 * to the partition which the user wants to be non-AO. Just ignore it
	 * instead.
	 *-----------
	 */
	/* GPDB_12_MERGE_FIXME */
#if 0
	if (!is_aocs(stmt->options) && stmt->is_part_child)
	{
		if (co_explicitly_disabled(stmt->options) || !stenc)
			stmt->attr_encodings = NIL;
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("ENCODING clause only supported with column oriented partitioned tables")));
		}
	}
	else
#endif
	stmt->attr_encodings = transformAttributeEncoding(stenc, stmt, &cxt);

	/*
	 * Transform DISTRIBUTED BY (or construct a default one, if not given
	 * explicitly). Not for foreign tables, though.
	 */
	if (stmt->relKind == RELKIND_RELATION)
	{
		int			numsegments = -1;

		/* GPDB_12_MERGE_FIXME */
#if 0
		AssertImply(stmt->is_part_parent,
					stmt->distributedBy == NULL);
		AssertImply(stmt->is_part_child,
					stmt->distributedBy != NULL);

		/*
		 * We want children have the same numsegments with parent.  As
		 * transformDistributedBy() always set numsegments to DEFAULT, does
		 * this meet our expectation?  No, because DEFAULT does not always
		 * equal to DEFAULT itself.  When DEFAULT is set to RANDOM a different
		 * value is returned each time.
		 *
		 * So we have to save the parent numsegments here.
		 */
		if (stmt->is_part_child)
			numsegments = stmt->distributedBy->numsegments;
#endif

		stmt->distributedBy = transformDistributedBy(&cxt, stmt->distributedBy,
							   likeDistributedBy, bQuiet);

		/*
		 * And forcely set it on children after transformDistributedBy().
		 */
		/* GPDB_12_MERGE_FIXME */
#if 0
		if (stmt->is_part_child)
			stmt->distributedBy->numsegments = numsegments;
#endif
	}

	if (stmt->partitionBy != NULL &&
		stmt->distributedBy &&
		stmt->distributedBy->ptype == POLICYTYPE_REPLICATED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PARTITION BY clause cannot be used with DISTRIBUTED REPLICATED clause")));

	/*
	 * Save the alist for root partitions before transformPartitionBy adds the
	 * child create statements.
	 */
	/* GPDB_12_MERGE_FIXME */
#if 0
	if (stmt->partitionBy && !stmt->is_part_child)
	{
		save_root_partition_alist = cxt.alist;
		cxt.alist = NIL;
	}
#endif

	/*
	 * Process table partitioning clause
	 */
	/* GDPB_12_MERGE_FIXME: need to re-implement this */
#if 0
	transformPartitionBy(&cxt, stmt, stmt->partitionBy);
#endif

	/*
	 * Postprocess check constraints.
	 */
	transformCheckConstraints(&cxt, !is_foreign_table ? true : false);

	/*
	 * Postprocess extended statistics.
	 */
	transformExtendedStatistics(&cxt);

	/*
	 * Output results.
	 */
	stmt->tableElts = cxt.columns;
	stmt->constraints = cxt.ckconstraints;

	result = lappend(cxt.blist, stmt);
	result = list_concat(result, cxt.alist);
	result = list_concat(result, save_alist);

	MemoryContextDelete(cxt.tempCtx);

	return result;
}

/*
 * generateSerialExtraStmts
 *		Generate CREATE SEQUENCE and ALTER SEQUENCE ... OWNED BY statements
 *		to create the sequence for a serial or identity column.
 *
 * This includes determining the name the sequence will have.  The caller
 * can ask to get back the name components by passing non-null pointers
 * for snamespace_p and sname_p.
 */
static void
generateSerialExtraStmts(CreateStmtContext *cxt, ColumnDef *column,
						 Oid seqtypid, List *seqoptions, bool for_identity,
						 char **snamespace_p, char **sname_p)
{
	ListCell   *option;
	DefElem    *nameEl = NULL;
	Oid			snamespaceid;
	char	   *snamespace;
	char	   *sname;
	CreateSeqStmt *seqstmt;
	AlterSeqStmt *altseqstmt;
	List	   *attnamelist;

	/*
	 * Determine namespace and name to use for the sequence.
	 *
	 * First, check if a sequence name was passed in as an option.  This is
	 * used by pg_dump.  Else, generate a name.
	 *
	 * Although we use ChooseRelationName, it's not guaranteed that the
	 * selected sequence name won't conflict; given sufficiently long field
	 * names, two different serial columns in the same table could be assigned
	 * the same sequence name, and we'd not notice since we aren't creating
	 * the sequence quite yet.  In practice this seems quite unlikely to be a
	 * problem, especially since few people would need two serial columns in
	 * one table.
	 */
	foreach(option, seqoptions)
	{
		DefElem    *defel = lfirst_node(DefElem, option);

		if (strcmp(defel->defname, "sequence_name") == 0)
		{
			if (nameEl)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			nameEl = defel;
		}
	}

	if (nameEl)
	{
		RangeVar   *rv = makeRangeVarFromNameList(castNode(List, nameEl->arg));

		snamespace = rv->schemaname;
		if (!snamespace)
		{
			/* Given unqualified SEQUENCE NAME, select namespace */
			if (cxt->rel)
				snamespaceid = RelationGetNamespace(cxt->rel);
			else
				snamespaceid = RangeVarGetCreationNamespace(cxt->relation);
			snamespace = get_namespace_name(snamespaceid);
		}
		sname = rv->relname;
		/* Remove the SEQUENCE NAME item from seqoptions */
		seqoptions = list_delete_ptr(seqoptions, nameEl);
	}
	else
	{
		if (cxt->rel)
			snamespaceid = RelationGetNamespace(cxt->rel);
		else
		{
			snamespaceid = RangeVarGetCreationNamespace(cxt->relation);
			RangeVarAdjustRelationPersistence(cxt->relation, snamespaceid);
		}
		snamespace = get_namespace_name(snamespaceid);
		sname = ChooseRelationName(cxt->relation->relname,
								   column->colname,
								   "seq",
								   snamespaceid,
								   false);
	}

	ereport(DEBUG1,
			(errmsg("%s will create implicit sequence \"%s\" for serial column \"%s.%s\"",
					cxt->stmtType, sname,
					cxt->relation->relname, column->colname)));

	/*
	 * Build a CREATE SEQUENCE command to create the sequence object, and add
	 * it to the list of things to be done before this CREATE/ALTER TABLE.
	 */
	seqstmt = makeNode(CreateSeqStmt);
	seqstmt->for_identity = for_identity;
	seqstmt->sequence = makeRangeVar(snamespace, sname, -1);
	seqstmt->options = seqoptions;

	/*
	 * If a sequence data type was specified, add it to the options.  Prepend
	 * to the list rather than append; in case a user supplied their own AS
	 * clause, the "redundant options" error will point to their occurrence,
	 * not our synthetic one.
	 */
	if (seqtypid)
		seqstmt->options = lcons(makeDefElem("as",
											 (Node *) makeTypeNameFromOid(seqtypid, -1),
											 -1),
								 seqstmt->options);

	/*
	 * If this is ALTER ADD COLUMN, make sure the sequence will be owned by
	 * the table's owner.  The current user might be someone else (perhaps a
	 * superuser, or someone who's only a member of the owning role), but the
	 * SEQUENCE OWNED BY mechanisms will bleat unless table and sequence have
	 * exactly the same owning role.
	 */
	if (cxt->rel)
		seqstmt->ownerId = cxt->rel->rd_rel->relowner;
	else
		seqstmt->ownerId = InvalidOid;

	cxt->blist = lappend(cxt->blist, seqstmt);

	/*
	 * Store the identity sequence name that we decided on.  ALTER TABLE ...
	 * ADD COLUMN ... IDENTITY needs this so that it can fill the new column
	 * with values from the sequence, while the association of the sequence
	 * with the table is not set until after the ALTER TABLE.
	 */
	column->identitySequence = seqstmt->sequence;

	/*
	 * Build an ALTER SEQUENCE ... OWNED BY command to mark the sequence as
	 * owned by this column, and add it to the list of things to be done after
	 * this CREATE/ALTER TABLE.
	 */
	altseqstmt = makeNode(AlterSeqStmt);
	altseqstmt->sequence = makeRangeVar(snamespace, sname, -1);
	attnamelist = list_make3(makeString(snamespace),
							 makeString(cxt->relation->relname),
							 makeString(column->colname));
	altseqstmt->options = list_make1(makeDefElem("owned_by",
												 (Node *) attnamelist, -1));
	altseqstmt->for_identity = for_identity;

	cxt->alist = lappend(cxt->alist, altseqstmt);

	if (snamespace_p)
		*snamespace_p = snamespace;
	if (sname_p)
		*sname_p = sname;
}

/*
 * transformColumnDefinition -
 *		transform a single ColumnDef within CREATE TABLE
 *		Also used in ALTER TABLE ADD COLUMN
 */
static void
transformColumnDefinition(CreateStmtContext *cxt, ColumnDef *column)
{
	bool		is_serial;
	bool		saw_nullable;
	bool		saw_default;
	bool		saw_identity;
	bool		saw_generated;
	ListCell   *clist;

	cxt->columns = lappend(cxt->columns, column);

	/* Check for SERIAL pseudo-types */
	is_serial = false;
	if (column->typeName
		&& list_length(column->typeName->names) == 1
		&& !column->typeName->pct_type)
	{
		char	   *typname = strVal(linitial(column->typeName->names));

		if (strcmp(typname, "smallserial") == 0 ||
			strcmp(typname, "serial2") == 0)
		{
			is_serial = true;
			column->typeName->names = NIL;
			column->typeName->typeOid = INT2OID;
		}
		else if (strcmp(typname, "serial") == 0 ||
				 strcmp(typname, "serial4") == 0)
		{
			is_serial = true;
			column->typeName->names = NIL;
			column->typeName->typeOid = INT4OID;
		}
		else if (strcmp(typname, "bigserial") == 0 ||
				 strcmp(typname, "serial8") == 0)
		{
			is_serial = true;
			column->typeName->names = NIL;
			column->typeName->typeOid = INT8OID;
		}

		/*
		 * We have to reject "serial[]" explicitly, because once we've set
		 * typeid, LookupTypeName won't notice arrayBounds.  We don't need any
		 * special coding for serial(typmod) though.
		 */
		if (is_serial && column->typeName->arrayBounds != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("array of serial is not implemented"),
					 parser_errposition(cxt->pstate,
										column->typeName->location)));
	}

	/* Do necessary work on the column type declaration */
	if (column->typeName)
		transformColumnType(cxt, column);

	/* Special actions for SERIAL pseudo-types */
	if (is_serial)
	{
		char	   *snamespace;
		char	   *sname;
		char	   *qstring;
		A_Const    *snamenode;
		TypeCast   *castnode;
		FuncCall   *funccallnode;
		Constraint *constraint;

		generateSerialExtraStmts(cxt, column,
								 column->typeName->typeOid, NIL, false,
								 &snamespace, &sname);

		/*
		 * Create appropriate constraints for SERIAL.  We do this in full,
		 * rather than shortcutting, so that we will detect any conflicting
		 * constraints the user wrote (like a different DEFAULT).
		 *
		 * Create an expression tree representing the function call
		 * nextval('sequencename').  We cannot reduce the raw tree to cooked
		 * form until after the sequence is created, but there's no need to do
		 * so.
		 */
		qstring = quote_qualified_identifier(snamespace, sname);
		snamenode = makeNode(A_Const);
		snamenode->val.type = T_String;
		snamenode->val.val.str = qstring;
		snamenode->location = -1;
		castnode = makeNode(TypeCast);
		castnode->typeName = SystemTypeName("regclass");
		castnode->arg = (Node *) snamenode;
		castnode->location = -1;
		funccallnode = makeFuncCall(SystemFuncName("nextval"),
									list_make1(castnode),
									-1);
		constraint = makeNode(Constraint);
		constraint->contype = CONSTR_DEFAULT;
		constraint->location = -1;
		constraint->raw_expr = (Node *) funccallnode;
		constraint->cooked_expr = NULL;
		column->constraints = lappend(column->constraints, constraint);

		constraint = makeNode(Constraint);
		constraint->contype = CONSTR_NOTNULL;
		constraint->location = -1;
		column->constraints = lappend(column->constraints, constraint);
	}

	/* Process column constraints, if any... */
	transformConstraintAttrs(cxt, column->constraints);

	saw_nullable = false;
	saw_default = false;
	saw_identity = false;
	saw_generated = false;

	foreach(clist, column->constraints)
	{
		Constraint *constraint = lfirst_node(Constraint, clist);

		switch (constraint->contype)
		{
			case CONSTR_NULL:
				if (saw_nullable && column->is_not_null)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->is_not_null = false;
				saw_nullable = true;
				break;

			case CONSTR_NOTNULL:
				if (saw_nullable && !column->is_not_null)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->is_not_null = true;
				saw_nullable = true;
				break;

			case CONSTR_DEFAULT:
				if (saw_default)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple default values specified for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->raw_default = constraint->raw_expr;
				Assert(constraint->cooked_expr == NULL);
				saw_default = true;
				break;

			case CONSTR_IDENTITY:
				{
					Type		ctype;
					Oid			typeOid;

					if (cxt->ofType)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("identity columns are not supported on typed tables")));
					if (cxt->partbound)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("identity columns are not supported on partitions")));

					ctype = typenameType(cxt->pstate, column->typeName, NULL);
					typeOid = ((Form_pg_type) GETSTRUCT(ctype))->oid;
					ReleaseSysCache(ctype);

					if (saw_identity)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("multiple identity specifications for column \"%s\" of table \"%s\"",
										column->colname, cxt->relation->relname),
								 parser_errposition(cxt->pstate,
													constraint->location)));

					generateSerialExtraStmts(cxt, column,
											 typeOid, constraint->options, true,
											 NULL, NULL);

					column->identity = constraint->generated_when;
					saw_identity = true;
					column->is_not_null = true;
					break;
				}

			case CONSTR_GENERATED:
				if (cxt->ofType)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("generated columns are not supported on typed tables")));
				if (cxt->partbound)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("generated columns are not supported on partitions")));

				if (saw_generated)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple generation clauses specified for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->generated = ATTRIBUTE_GENERATED_STORED;
				column->raw_default = constraint->raw_expr;
				Assert(constraint->cooked_expr == NULL);
				saw_generated = true;
				break;

			case CONSTR_CHECK:
				cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
				break;

			case CONSTR_PRIMARY:
				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("primary key constraints are not supported on foreign tables"),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				/* FALL THRU */

			case CONSTR_UNIQUE:
				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("unique constraints are not supported on foreign tables"),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				if (constraint->keys == NIL)
					constraint->keys = list_make1(makeString(column->colname));
				cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
				break;

			case CONSTR_EXCLUSION:
				/* grammar does not allow EXCLUDE as a column constraint */
				elog(ERROR, "column exclusion constraints are not supported");
				break;

			case CONSTR_FOREIGN:
				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("foreign key constraints are not supported on foreign tables"),
							 parser_errposition(cxt->pstate,
												constraint->location)));

				/*
				 * Fill in the current attribute's name and throw it into the
				 * list of FK constraints to be processed later.
				 */
				constraint->fk_attrs = list_make1(makeString(column->colname));
				cxt->fkconstraints = lappend(cxt->fkconstraints, constraint);
				break;

			case CONSTR_ATTR_DEFERRABLE:
			case CONSTR_ATTR_NOT_DEFERRABLE:
			case CONSTR_ATTR_DEFERRED:
			case CONSTR_ATTR_IMMEDIATE:
				/* transformConstraintAttrs took care of these */
				break;

			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 constraint->contype);
				break;
		}

		if (saw_default && saw_identity)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both default and identity specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));

		if (saw_default && saw_generated)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both default and generation expression specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));

		if (saw_identity && saw_generated)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both identity and generation expression specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));
	}

	/*
	 * If needed, generate ALTER FOREIGN TABLE ALTER COLUMN statement to add
	 * per-column foreign data wrapper options to this column after creation.
	 */
	if (column->fdwoptions != NIL)
	{
		AlterTableStmt *stmt;
		AlterTableCmd *cmd;

		cmd = makeNode(AlterTableCmd);
		cmd->subtype = AT_AlterColumnGenericOptions;
		cmd->name = column->colname;
		cmd->def = (Node *) column->fdwoptions;
		cmd->behavior = DROP_RESTRICT;
		cmd->missing_ok = false;

		stmt = makeNode(AlterTableStmt);
		stmt->relation = cxt->relation;
		stmt->cmds = NIL;
		stmt->relkind = OBJECT_FOREIGN_TABLE;
		stmt->cmds = lappend(stmt->cmds, cmd);

		cxt->alist = lappend(cxt->alist, stmt);
	}
}

/*
 * transformTableConstraint
 *		transform a Constraint node within CREATE TABLE or ALTER TABLE
 */
static void
transformTableConstraint(CreateStmtContext *cxt, Constraint *constraint)
{
	switch (constraint->contype)
	{
		case CONSTR_PRIMARY:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("primary key constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;

		case CONSTR_UNIQUE:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unique constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;

		case CONSTR_EXCLUSION:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("exclusion constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			if (cxt->ispartitioned)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("exclusion constraints are not supported on partitioned tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;

		case CONSTR_CHECK:
			cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
			break;

		case CONSTR_FOREIGN:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("foreign key constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->fkconstraints = lappend(cxt->fkconstraints, constraint);
			break;

		case CONSTR_NULL:
		case CONSTR_NOTNULL:
		case CONSTR_DEFAULT:
		case CONSTR_ATTR_DEFERRABLE:
		case CONSTR_ATTR_NOT_DEFERRABLE:
		case CONSTR_ATTR_DEFERRED:
		case CONSTR_ATTR_IMMEDIATE:
			elog(ERROR, "invalid context for constraint type %d",
				 constraint->contype);
			break;

		default:
			elog(ERROR, "unrecognized constraint type: %d",
				 constraint->contype);
			break;
	}
}

/*
 * transformTableLikeClause
 *
 * Change the LIKE <srctable> portion of a CREATE TABLE statement into
 * column definitions which recreate the user defined column portions of
 * <srctable>.
 *
 * GPDB: if forceBareCol is true we disallow inheriting any indexes/constr/defaults.
 */
static void
transformTableLikeClause(CreateStmtContext *cxt, TableLikeClause *table_like_clause,
						 bool forceBareCol, CreateStmt *stmt, List **stenc)
{
	AttrNumber	parent_attno;
	Relation	relation;
	TupleDesc	tupleDesc;
	TupleConstr *constr;
	AttrNumber *attmap;
	AclResult	aclresult;
	char	   *comment;
	ParseCallbackState pcbstate;

	setup_parser_errposition_callback(&pcbstate, cxt->pstate,
									  table_like_clause->relation->location);

	/* LIKE INCLUDING is not supported for external tables */
	if (forceBareCol && table_like_clause->options != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("LIKE INCLUDING may not be used with this kind of relation")));

	/* we could support LIKE in many cases, but worry about it another day */
	if (cxt->isforeign)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("LIKE is not supported for creating foreign tables")));

	relation = relation_openrv(table_like_clause->relation, AccessShareLock);

	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_VIEW &&
		relation->rd_rel->relkind != RELKIND_MATVIEW &&
		relation->rd_rel->relkind != RELKIND_COMPOSITE_TYPE &&
		relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
		relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table, view, materialized view, composite type, or foreign table",
						RelationGetRelationName(relation))));

	cancel_parser_errposition_callback(&pcbstate);

	/*
	 * Check for privileges
	 */
	if (relation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
	{
		aclresult = pg_type_aclcheck(relation->rd_rel->reltype, GetUserId(),
									 ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TYPE,
						   RelationGetRelationName(relation));
	}
	else
	{
		aclresult = pg_class_aclcheck(RelationGetRelid(relation), GetUserId(),
									  ACL_SELECT);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, get_relkind_objtype(relation->rd_rel->relkind),
						   RelationGetRelationName(relation));
	}

	tupleDesc = RelationGetDescr(relation);
	constr = tupleDesc->constr;

	/*
	 * Initialize column number map for map_variable_attnos().  We need this
	 * since dropped columns in the source table aren't copied, so the new
	 * table can have different column numbers.
	 */
	attmap = (AttrNumber *) palloc0(sizeof(AttrNumber) * tupleDesc->natts);

	/*
	 * Insert the copied attributes into the cxt for the new table definition.
	 */
	for (parent_attno = 1; parent_attno <= tupleDesc->natts;
		 parent_attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc,
													parent_attno - 1);
		char	   *attributeName = NameStr(attribute->attname);
		ColumnDef  *def;

		/*
		 * Ignore dropped columns in the parent.  attmap entry is left zero.
		 */
		if (attribute->attisdropped)
			continue;

		/*
		 * Create a new column, which is marked as NOT inherited.
		 *
		 * For constraints, ONLY the NOT NULL constraint is inherited by the
		 * new column definition per SQL99.
		 */
		def = makeNode(ColumnDef);
		def->colname = pstrdup(attributeName);
		def->typeName = makeTypeNameFromOid(attribute->atttypid,
											attribute->atttypmod);
		def->inhcount = 0;
		def->is_local = true;
		def->is_not_null = (forceBareCol ? false : attribute->attnotnull);
		def->is_from_type = false;
		def->storage = 0;
		def->raw_default = NULL;
		def->cooked_default = NULL;
		def->collClause = NULL;
		def->collOid = attribute->attcollation;
		def->constraints = NIL;
		def->location = -1;

		/*
		 * Add to column list
		 */
		cxt->columns = lappend(cxt->columns, def);

		attmap[parent_attno - 1] = list_length(cxt->columns);

		/*
		 * Copy default, if present and the default has been requested
		 */
		if (attribute->atthasdef &&
			(table_like_clause->options & CREATE_TABLE_LIKE_DEFAULTS ||
			 table_like_clause->options & CREATE_TABLE_LIKE_GENERATED))
		{
			Node	   *this_default = NULL;
			AttrDefault *attrdef;
			int			i;
			bool		found_whole_row;

			/* Find default in constraint structure */
			Assert(constr != NULL);
			attrdef = constr->defval;
			for (i = 0; i < constr->num_defval; i++)
			{
				if (attrdef[i].adnum == parent_attno)
				{
					this_default = stringToNode(attrdef[i].adbin);
					break;
				}
			}
			Assert(this_default != NULL);

			def->cooked_default = map_variable_attnos(this_default,
													  1, 0,
													  attmap, tupleDesc->natts,
													  InvalidOid, &found_whole_row);

			/*
			 * Prevent this for the same reason as for constraints below. Note
			 * that defaults cannot contain any vars, so it's OK that the
			 * error message refers to generated columns.
			 */
			if (found_whole_row)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot convert whole-row table reference"),
						 errdetail("Generation expression for column \"%s\" contains a whole-row reference to table \"%s\".",
								   attributeName,
								   RelationGetRelationName(relation))));

			if (attribute->attgenerated &&
				(table_like_clause->options & CREATE_TABLE_LIKE_GENERATED))
				def->generated = attribute->attgenerated;
		}

		/*
		 * Copy identity if requested
		 */
		if (attribute->attidentity &&
			(table_like_clause->options & CREATE_TABLE_LIKE_IDENTITY))
		{
			Oid			seq_relid;
			List	   *seq_options;

			/*
			 * find sequence owned by old column; extract sequence parameters;
			 * build new create sequence command
			 */
			seq_relid = getOwnedSequence(RelationGetRelid(relation), attribute->attnum);
			seq_options = sequence_options(seq_relid);
			generateSerialExtraStmts(cxt, def,
									 InvalidOid, seq_options, true,
									 NULL, NULL);
			def->identity = attribute->attidentity;
		}

		/* Likewise, copy storage if requested */
		if (table_like_clause->options & CREATE_TABLE_LIKE_STORAGE)
			def->storage = attribute->attstorage;
		else
			def->storage = 0;

		/* Likewise, copy comment if requested */
		if ((table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS) &&
			(comment = GetComment(attribute->attrelid,
								  RelationRelationId,
								  attribute->attnum)) != NULL)
		{
			CommentStmt *stmt = makeNode(CommentStmt);

			stmt->objtype = OBJECT_COLUMN;
			stmt->object = (Node *) list_make3(makeString(cxt->relation->schemaname),
											   makeString(cxt->relation->relname),
											   makeString(def->colname));
			stmt->comment = comment;

			cxt->alist = lappend(cxt->alist, stmt);
		}
	}

	/*
	 * Copy CHECK constraints if requested, being careful to adjust attribute
	 * numbers so they match the child.
	 */
	if ((table_like_clause->options & CREATE_TABLE_LIKE_CONSTRAINTS) &&
		tupleDesc->constr)
	{
		int			ccnum;

		for (ccnum = 0; ccnum < tupleDesc->constr->num_check; ccnum++)
		{
			char	   *ccname = tupleDesc->constr->check[ccnum].ccname;
			char	   *ccbin = tupleDesc->constr->check[ccnum].ccbin;
			Constraint *n = makeNode(Constraint);
			Node	   *ccbin_node;
			bool		found_whole_row;

			ccbin_node = map_variable_attnos(stringToNode(ccbin),
											 1, 0,
											 attmap, tupleDesc->natts,
											 InvalidOid, &found_whole_row);

			/*
			 * We reject whole-row variables because the whole point of LIKE
			 * is that the new table's rowtype might later diverge from the
			 * parent's.  So, while translation might be possible right now,
			 * it wouldn't be possible to guarantee it would work in future.
			 */
			if (found_whole_row)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot convert whole-row table reference"),
						 errdetail("Constraint \"%s\" contains a whole-row reference to table \"%s\".",
								   ccname,
								   RelationGetRelationName(relation))));

			n->contype = CONSTR_CHECK;
			n->location = -1;
			n->conname = pstrdup(ccname);
			n->raw_expr = NULL;
			n->cooked_expr = nodeToString(ccbin_node);
			cxt->ckconstraints = lappend(cxt->ckconstraints, n);

			/* Copy comment on constraint */
			if ((table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS) &&
				(comment = GetComment(get_relation_constraint_oid(RelationGetRelid(relation),
																  n->conname, false),
									  ConstraintRelationId,
									  0)) != NULL)
			{
				CommentStmt *stmt = makeNode(CommentStmt);

				stmt->objtype = OBJECT_TABCONSTRAINT;
				stmt->object = (Node *) list_make3(makeString(cxt->relation->schemaname),
												   makeString(cxt->relation->relname),
												   makeString(n->conname));
				stmt->comment = comment;

				cxt->alist = lappend(cxt->alist, stmt);
			}
		}
	}

	/*
	 * Likewise, copy indexes if requested
	 */
	if ((table_like_clause->options & CREATE_TABLE_LIKE_INDEXES) &&
		relation->rd_rel->relhasindex)
	{
		List	   *parent_indexes;
		ListCell   *l;

		parent_indexes = RelationGetIndexList(relation);

		foreach(l, parent_indexes)
		{
			Oid			parent_index_oid = lfirst_oid(l);
			Relation	parent_index;
			IndexStmt  *index_stmt;

			parent_index = index_open(parent_index_oid, AccessShareLock);

			/* Build CREATE INDEX statement to recreate the parent_index */
			index_stmt = generateClonedIndexStmt(cxt->relation,
												 parent_index,
												 attmap, tupleDesc->natts,
												 NULL);

			/* Copy comment on index, if requested */
			if (table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS)
			{
				comment = GetComment(parent_index_oid, RelationRelationId, 0);

				/*
				 * We make use of IndexStmt's idxcomment option, so as not to
				 * need to know now what name the index will have.
				 */
				index_stmt->idxcomment = comment;
			}

			/* Save it in the inh_indexes list for the time being */
			cxt->inh_indexes = lappend(cxt->inh_indexes, index_stmt);

			index_close(parent_index, AccessShareLock);
		}
	}

	/*
	 * If STORAGE is included, we need to copy over the table storage params
	 * as well as the attribute encodings.
	 */
	if (stmt && table_like_clause->options & CREATE_TABLE_LIKE_STORAGE)
	{
		/* GDPB_12_MERGE_FIXME: need to re-implement this */
		elog(ERROR, "not implemented");
#if 0
		MemoryContext oldcontext;
		/*
		 * As we are modifying the utility statement we must make sure these
		 * DefElem allocations can survive outside of this context.
		 */
		oldcontext = MemoryContextSwitchTo(CurTransactionContext);

		if (relation->rd_appendonly)
		{
			Form_pg_appendonly ao = relation->rd_appendonly;

			stmt->options = lappend(stmt->options,
									makeDefElem("appendonly", (Node *) makeString(pstrdup("true")), -1));
			if (ao->columnstore)
				stmt->options = lappend(stmt->options,
										makeDefElem("orientation", (Node *) makeString(pstrdup("column")), -1));
			stmt->options = lappend(stmt->options,
									makeDefElem("checksum", (Node *) makeInteger(ao->checksum), -1));
			stmt->options = lappend(stmt->options,
									makeDefElem("compresslevel", (Node *) makeInteger(ao->compresslevel), -1));
			if (strlen(NameStr(ao->compresstype)) > 0)
				stmt->options = lappend(stmt->options,
										makeDefElem("compresstype", (Node *) makeString(pstrdup(NameStr(ao->compresstype))), -1));
		}

		/*
		 * Set the attribute encodings.
		 */
		*stenc = list_union(*stenc, rel_get_column_encodings(relation));
		MemoryContextSwitchTo(oldcontext);
#endif
	}

	/*
	 * Likewise, copy extended statistics if requested
	 */
	if (table_like_clause->options & CREATE_TABLE_LIKE_STATISTICS)
	{
		List	   *parent_extstats;
		ListCell   *l;

		parent_extstats = RelationGetStatExtList(relation);

		foreach(l, parent_extstats)
		{
			Oid			parent_stat_oid = lfirst_oid(l);
			CreateStatsStmt *stats_stmt;

			stats_stmt = generateClonedExtStatsStmt(cxt->relation,
													RelationGetRelid(relation),
													parent_stat_oid);

			/* Copy comment on statistics object, if requested */
			if (table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS)
			{
				comment = GetComment(parent_stat_oid, StatisticExtRelationId, 0);

				/*
				 * We make use of CreateStatsStmt's stxcomment option, so as
				 * not to need to know now what name the statistics will have.
				 */
				stats_stmt->stxcomment = comment;
			}

			cxt->extstats = lappend(cxt->extstats, stats_stmt);
		}

		list_free(parent_extstats);
	}

	/*
	 * Close the parent rel, but keep our AccessShareLock on it until xact
	 * commit.  That will prevent someone else from deleting or ALTERing the
	 * parent before the child is committed.
	 */
	table_close(relation, NoLock);
}

static void
transformOfType(CreateStmtContext *cxt, TypeName *ofTypename)
{
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	int			i;
	Oid			ofTypeId;

	AssertArg(ofTypename);

	tuple = typenameType(NULL, ofTypename, NULL);
	check_of_type(tuple);
	ofTypeId = ((Form_pg_type) GETSTRUCT(tuple))->oid;
	ofTypename->typeOid = ofTypeId; /* cached for later */

	tupdesc = lookup_rowtype_tupdesc(ofTypeId, -1);
	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
		ColumnDef  *n;

		if (attr->attisdropped)
			continue;

		n = makeNode(ColumnDef);
		n->colname = pstrdup(NameStr(attr->attname));
		n->typeName = makeTypeNameFromOid(attr->atttypid, attr->atttypmod);
		n->inhcount = 0;
		n->is_local = true;
		n->is_not_null = false;
		n->is_from_type = true;
		n->storage = 0;
		n->raw_default = NULL;
		n->cooked_default = NULL;
		n->collClause = NULL;
		n->collOid = attr->attcollation;
		n->constraints = NIL;
		n->location = -1;
		cxt->columns = lappend(cxt->columns, n);
	}
	DecrTupleDescRefCount(tupdesc);

	ReleaseSysCache(tuple);
}

/*
 * Generate an IndexStmt node using information from an already existing index
 * "source_idx".
 *
 * heapRel is stored into the IndexStmt's relation field, but we don't use it
 * otherwise; some callers pass NULL, if they don't need it to be valid.
 * (The target relation might not exist yet, so we mustn't try to access it.)
 *
 * Attribute numbers in expression Vars are adjusted according to attmap.
 *
 * If constraintOid isn't NULL, we store the OID of any constraint associated
 * with the index there.
 *
 * Unlike transformIndexConstraint, we don't make any effort to force primary
 * key columns to be NOT NULL.  The larger cloning process this is part of
 * should have cloned their NOT NULL status separately (and DefineIndex will
 * complain if that fails to happen).
 */
IndexStmt *
generateClonedIndexStmt(RangeVar *heapRel, Relation source_idx,
						const AttrNumber *attmap, int attmap_length,
						Oid *constraintOid)
{
	Oid			source_relid = RelationGetRelid(source_idx);
	HeapTuple	ht_idxrel;
	HeapTuple	ht_idx;
	HeapTuple	ht_am;
	Form_pg_class idxrelrec;
	Form_pg_index idxrec;
	Form_pg_am	amrec;
	oidvector  *indcollation;
	oidvector  *indclass;
	IndexStmt  *index;
	List	   *indexprs;
	ListCell   *indexpr_item;
	Oid			indrelid;
	Oid			constraintId = InvalidOid;
	int			keyno;
	Oid			keycoltype;
	Datum		datum;
	bool		isnull;

	if (constraintOid)
		*constraintOid = InvalidOid;

	/*
	 * Fetch pg_class tuple of source index.  We can't use the copy in the
	 * relcache entry because it doesn't include optional fields.
	 */
	ht_idxrel = SearchSysCache1(RELOID, ObjectIdGetDatum(source_relid));
	if (!HeapTupleIsValid(ht_idxrel))
		elog(ERROR, "cache lookup failed for relation %u", source_relid);
	idxrelrec = (Form_pg_class) GETSTRUCT(ht_idxrel);

	/* Fetch pg_index tuple for source index from relcache entry */
	ht_idx = source_idx->rd_indextuple;
	idxrec = (Form_pg_index) GETSTRUCT(ht_idx);
	indrelid = idxrec->indrelid;

	/* Fetch the pg_am tuple of the index' access method */
	ht_am = SearchSysCache1(AMOID, ObjectIdGetDatum(idxrelrec->relam));
	if (!HeapTupleIsValid(ht_am))
		elog(ERROR, "cache lookup failed for access method %u",
			 idxrelrec->relam);
	amrec = (Form_pg_am) GETSTRUCT(ht_am);

	/* Extract indcollation from the pg_index tuple */
	datum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indcollation, &isnull);
	Assert(!isnull);
	indcollation = (oidvector *) DatumGetPointer(datum);

	/* Extract indclass from the pg_index tuple */
	datum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indclass, &isnull);
	Assert(!isnull);
	indclass = (oidvector *) DatumGetPointer(datum);

	/* Begin building the IndexStmt */
	index = makeNode(IndexStmt);
	index->relation = heapRel;
	index->accessMethod = pstrdup(NameStr(amrec->amname));
	if (OidIsValid(idxrelrec->reltablespace))
		index->tableSpace = get_tablespace_name(idxrelrec->reltablespace);
	else
		index->tableSpace = NULL;
	index->excludeOpNames = NIL;
	index->idxcomment = NULL;
	index->indexOid = InvalidOid;
	index->oldNode = InvalidOid;
	index->unique = idxrec->indisunique;
	index->primary = idxrec->indisprimary;
	index->transformed = true;	/* don't need transformIndexStmt */
	index->concurrent = false;
	index->if_not_exists = false;
	index->reset_default_tblspc = false;

	/*
	 * We don't try to preserve the name of the source index; instead, just
	 * let DefineIndex() choose a reasonable name.  (If we tried to preserve
	 * the name, we'd get duplicate-relation-name failures unless the source
	 * table was in a different schema.)
	 */
	index->idxname = NULL;

	/*
	 * If the index is marked PRIMARY or has an exclusion condition, it's
	 * certainly from a constraint; else, if it's not marked UNIQUE, it
	 * certainly isn't.  If it is or might be from a constraint, we have to
	 * fetch the pg_constraint record.
	 */
	if (index->primary || index->unique || idxrec->indisexclusion)
	{
		constraintId = get_index_constraint(source_relid);

		if (OidIsValid(constraintId))
		{
			HeapTuple	ht_constr;
			Form_pg_constraint conrec;

			if (constraintOid)
				*constraintOid = constraintId;

			ht_constr = SearchSysCache1(CONSTROID,
										ObjectIdGetDatum(constraintId));
			if (!HeapTupleIsValid(ht_constr))
				elog(ERROR, "cache lookup failed for constraint %u",
					 constraintId);
			conrec = (Form_pg_constraint) GETSTRUCT(ht_constr);

			index->isconstraint = true;
			index->deferrable = conrec->condeferrable;
			index->initdeferred = conrec->condeferred;

			/* If it's an exclusion constraint, we need the operator names */
			if (idxrec->indisexclusion)
			{
				Datum	   *elems;
				int			nElems;
				int			i;

				Assert(conrec->contype == CONSTRAINT_EXCLUSION);
				/* Extract operator OIDs from the pg_constraint tuple */
				datum = SysCacheGetAttr(CONSTROID, ht_constr,
										Anum_pg_constraint_conexclop,
										&isnull);
				if (isnull)
					elog(ERROR, "null conexclop for constraint %u",
						 constraintId);

				deconstruct_array(DatumGetArrayTypeP(datum),
								  OIDOID, sizeof(Oid), true, 'i',
								  &elems, NULL, &nElems);

				for (i = 0; i < nElems; i++)
				{
					Oid			operid = DatumGetObjectId(elems[i]);
					HeapTuple	opertup;
					Form_pg_operator operform;
					char	   *oprname;
					char	   *nspname;
					List	   *namelist;

					opertup = SearchSysCache1(OPEROID,
											  ObjectIdGetDatum(operid));
					if (!HeapTupleIsValid(opertup))
						elog(ERROR, "cache lookup failed for operator %u",
							 operid);
					operform = (Form_pg_operator) GETSTRUCT(opertup);
					oprname = pstrdup(NameStr(operform->oprname));
					/* For simplicity we always schema-qualify the op name */
					nspname = get_namespace_name(operform->oprnamespace);
					namelist = list_make2(makeString(nspname),
										  makeString(oprname));
					index->excludeOpNames = lappend(index->excludeOpNames,
													namelist);
					ReleaseSysCache(opertup);
				}
			}

			ReleaseSysCache(ht_constr);
		}
		else
			index->isconstraint = false;
	}
	else
		index->isconstraint = false;

	/* Get the index expressions, if any */
	datum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indexprs, &isnull);
	if (!isnull)
	{
		char	   *exprsString;

		exprsString = TextDatumGetCString(datum);
		indexprs = (List *) stringToNode(exprsString);
	}
	else
		indexprs = NIL;

	/* Build the list of IndexElem */
	index->indexParams = NIL;
	index->indexIncludingParams = NIL;

	indexpr_item = list_head(indexprs);
	for (keyno = 0; keyno < idxrec->indnkeyatts; keyno++)
	{
		IndexElem  *iparam;
		AttrNumber	attnum = idxrec->indkey.values[keyno];
		Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(source_idx),
											   keyno);
		int16		opt = source_idx->rd_indoption[keyno];

		iparam = makeNode(IndexElem);

		if (AttributeNumberIsValid(attnum))
		{
			/* Simple index column */
			char	   *attname;

			attname = get_attname(indrelid, attnum, false);
			keycoltype = get_atttype(indrelid, attnum);

			iparam->name = attname;
			iparam->expr = NULL;
		}
		else
		{
			/* Expressional index */
			Node	   *indexkey;
			bool		found_whole_row;

			if (indexpr_item == NULL)
				elog(ERROR, "too few entries in indexprs list");
			indexkey = (Node *) lfirst(indexpr_item);
			indexpr_item = lnext(indexpr_item);

			/* Adjust Vars to match new table's column numbering */
			indexkey = map_variable_attnos(indexkey,
										   1, 0,
										   attmap, attmap_length,
										   InvalidOid, &found_whole_row);

			/* As in transformTableLikeClause, reject whole-row variables */
			if (found_whole_row)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot convert whole-row table reference"),
						 errdetail("Index \"%s\" contains a whole-row table reference.",
								   RelationGetRelationName(source_idx))));

			iparam->name = NULL;
			iparam->expr = indexkey;

			keycoltype = exprType(indexkey);
		}

		/* Copy the original index column name */
		iparam->indexcolname = pstrdup(NameStr(attr->attname));

		/* Add the collation name, if non-default */
		iparam->collation = get_collation(indcollation->values[keyno], keycoltype);

		/* Add the operator class name, if non-default */
		iparam->opclass = get_opclass(indclass->values[keyno], keycoltype);

		iparam->ordering = SORTBY_DEFAULT;
		iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;

		/* Adjust options if necessary */
		if (source_idx->rd_indam->amcanorder)
		{
			/*
			 * If it supports sort ordering, copy DESC and NULLS opts. Don't
			 * set non-default settings unnecessarily, though, so as to
			 * improve the chance of recognizing equivalence to constraint
			 * indexes.
			 */
			if (opt & INDOPTION_DESC)
			{
				iparam->ordering = SORTBY_DESC;
				if ((opt & INDOPTION_NULLS_FIRST) == 0)
					iparam->nulls_ordering = SORTBY_NULLS_LAST;
			}
			else
			{
				if (opt & INDOPTION_NULLS_FIRST)
					iparam->nulls_ordering = SORTBY_NULLS_FIRST;
			}
		}

		index->indexParams = lappend(index->indexParams, iparam);
	}

	/* Handle included columns separately */
	for (keyno = idxrec->indnkeyatts; keyno < idxrec->indnatts; keyno++)
	{
		IndexElem  *iparam;
		AttrNumber	attnum = idxrec->indkey.values[keyno];
		Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(source_idx),
											   keyno);

		iparam = makeNode(IndexElem);

		if (AttributeNumberIsValid(attnum))
		{
			/* Simple index column */
			char	   *attname;

			attname = get_attname(indrelid, attnum, false);
			keycoltype = get_atttype(indrelid, attnum);

			iparam->name = attname;
			iparam->expr = NULL;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("expressions are not supported in included columns")));

		/* Copy the original index column name */
		iparam->indexcolname = pstrdup(NameStr(attr->attname));

		index->indexIncludingParams = lappend(index->indexIncludingParams, iparam);
	}
	/* Copy reloptions if any */
	datum = SysCacheGetAttr(RELOID, ht_idxrel,
							Anum_pg_class_reloptions, &isnull);
	if (!isnull)
		index->options = untransformRelOptions(datum);

	/* If it's a partial index, decompile and append the predicate */
	datum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indpred, &isnull);
	if (!isnull)
	{
		char	   *pred_str;
		Node	   *pred_tree;
		bool		found_whole_row;

		/* Convert text string to node tree */
		pred_str = TextDatumGetCString(datum);
		pred_tree = (Node *) stringToNode(pred_str);

		/* Adjust Vars to match new table's column numbering */
		pred_tree = map_variable_attnos(pred_tree,
										1, 0,
										attmap, attmap_length,
										InvalidOid, &found_whole_row);

		/* As in transformTableLikeClause, reject whole-row variables */
		if (found_whole_row)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot convert whole-row table reference"),
					 errdetail("Index \"%s\" contains a whole-row table reference.",
							   RelationGetRelationName(source_idx))));

		index->whereClause = pred_tree;
	}

	/* Clean up */
	ReleaseSysCache(ht_idxrel);
	ReleaseSysCache(ht_am);

	return index;
}

/*
 * Generate a CreateStatsStmt node using information from an already existing
 * extended statistic "source_statsid", for the rel identified by heapRel and
 * heapRelid.
 */
static CreateStatsStmt *
generateClonedExtStatsStmt(RangeVar *heapRel, Oid heapRelid,
						   Oid source_statsid)
{
	HeapTuple	ht_stats;
	Form_pg_statistic_ext statsrec;
	CreateStatsStmt *stats;
	List	   *stat_types = NIL;
	List	   *def_names = NIL;
	bool		isnull;
	Datum		datum;
	ArrayType  *arr;
	char	   *enabled;
	int			i;

	Assert(OidIsValid(heapRelid));
	Assert(heapRel != NULL);

	/*
	 * Fetch pg_statistic_ext tuple of source statistics object.
	 */
	ht_stats = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(source_statsid));
	if (!HeapTupleIsValid(ht_stats))
		elog(ERROR, "cache lookup failed for statistics object %u", source_statsid);
	statsrec = (Form_pg_statistic_ext) GETSTRUCT(ht_stats);

	/* Determine which statistics types exist */
	datum = SysCacheGetAttr(STATEXTOID, ht_stats,
							Anum_pg_statistic_ext_stxkind, &isnull);
	Assert(!isnull);
	arr = DatumGetArrayTypeP(datum);
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != CHAROID)
		elog(ERROR, "stxkind is not a 1-D char array");
	enabled = (char *) ARR_DATA_PTR(arr);
	for (i = 0; i < ARR_DIMS(arr)[0]; i++)
	{
		if (enabled[i] == STATS_EXT_NDISTINCT)
			stat_types = lappend(stat_types, makeString("ndistinct"));
		else if (enabled[i] == STATS_EXT_DEPENDENCIES)
			stat_types = lappend(stat_types, makeString("dependencies"));
		else if (enabled[i] == STATS_EXT_MCV)
			stat_types = lappend(stat_types, makeString("mcv"));
		else
			elog(ERROR, "unrecognized statistics kind %c", enabled[i]);
	}

	/* Determine which columns the statistics are on */
	for (i = 0; i < statsrec->stxkeys.dim1; i++)
	{
		ColumnRef  *cref = makeNode(ColumnRef);
		AttrNumber	attnum = statsrec->stxkeys.values[i];

		cref->fields = list_make1(makeString(get_attname(heapRelid,
														 attnum, false)));
		cref->location = -1;

		def_names = lappend(def_names, cref);
	}

	/* finally, build the output node */
	stats = makeNode(CreateStatsStmt);
	stats->defnames = NULL;
	stats->stat_types = stat_types;
	stats->exprs = def_names;
	stats->relations = list_make1(heapRel);
	stats->stxcomment = NULL;
	stats->if_not_exists = false;

	/* Clean up */
	ReleaseSysCache(ht_stats);

	return stats;
}

/*
 * get_collation		- fetch qualified name of a collation
 *
 * If collation is InvalidOid or is the default for the given actual_datatype,
 * then the return value is NIL.
 */
static List *
get_collation(Oid collation, Oid actual_datatype)
{
	List	   *result;
	HeapTuple	ht_coll;
	Form_pg_collation coll_rec;
	char	   *nsp_name;
	char	   *coll_name;

	if (!OidIsValid(collation))
		return NIL;				/* easy case */
	if (collation == get_typcollation(actual_datatype))
		return NIL;				/* just let it default */

	ht_coll = SearchSysCache1(COLLOID, ObjectIdGetDatum(collation));
	if (!HeapTupleIsValid(ht_coll))
		elog(ERROR, "cache lookup failed for collation %u", collation);
	coll_rec = (Form_pg_collation) GETSTRUCT(ht_coll);

	/* For simplicity, we always schema-qualify the name */
	nsp_name = get_namespace_name(coll_rec->collnamespace);
	coll_name = pstrdup(NameStr(coll_rec->collname));
	result = list_make2(makeString(nsp_name), makeString(coll_name));

	ReleaseSysCache(ht_coll);
	return result;
}

/*
 * get_opclass			- fetch qualified name of an index operator class
 *
 * If the opclass is the default for the given actual_datatype, then
 * the return value is NIL.
 */
static List *
get_opclass(Oid opclass, Oid actual_datatype)
{
	List	   *result = NIL;
	HeapTuple	ht_opc;
	Form_pg_opclass opc_rec;

	ht_opc = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
	if (!HeapTupleIsValid(ht_opc))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	opc_rec = (Form_pg_opclass) GETSTRUCT(ht_opc);

	if (GetDefaultOpClass(actual_datatype, opc_rec->opcmethod) != opclass)
	{
		/* For simplicity, we always schema-qualify the name */
		char	   *nsp_name = get_namespace_name(opc_rec->opcnamespace);
		char	   *opc_name = pstrdup(NameStr(opc_rec->opcname));

		result = list_make2(makeString(nsp_name), makeString(opc_name));
	}

	ReleaseSysCache(ht_opc);
	return result;
}

List *
transformCreateExternalStmt(CreateExternalStmt *stmt, const char *queryString)
{
	ParseState *pstate;
	CreateStmtContext cxt;
	List	   *result;
	ListCell   *elements;
	DistributedBy *likeDistributedBy = NULL;
	bool	    bQuiet = false;	/* shut up transformDistributedBy messages */
	bool		iswritable = false;

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	memset(&cxt, 0, sizeof(CreateStmtContext));

	/*
	 * Create a temporary context in order to confine memory leaks due
	 * to expansions within a short lived context
	 */
	cxt.tempCtx = AllocSetContextCreate(CurrentMemoryContext,
							  "CreateExteranlStmt analyze context",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * There exist transformations that might write on the passed on stmt.
	 * Create a copy of it to both protect from (un)intentional writes and be
	 * a bit more explicit of the intended ownership.
	 */
	stmt = (CreateExternalStmt *)copyObject(stmt);

	cxt.pstate = pstate;
	cxt.stmtType = "CREATE EXTERNAL TABLE";
	cxt.relation = stmt->relation;
	cxt.inhRelations = NIL;
	cxt.isalter = false;
	cxt.columns = NIL;
	cxt.ckconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
	cxt.pkey = NULL;
	cxt.rel = NULL;

	cxt.blist = NIL;
	cxt.alist = NIL;

	iswritable = stmt->iswritable;

	/*
	 * Run through each primary element in the table creation clause. Separate
	 * column defs from constraints, and do preliminary analysis.
	 */
	foreach(elements, stmt->tableElts)
	{
		Node	   *element = lfirst(elements);

		switch (nodeTag(element))
		{
			case T_ColumnDef:
				transformColumnDefinition(&cxt, (ColumnDef *) element);
				break;

			case T_Constraint:
				/* should never happen. If it does fix gram.y */
				elog(ERROR, "node type %d not supported for external tables",
					 (int) nodeTag(element));
				break;

			case T_TableLikeClause:
				{
					/* LIKE */
					bool	isBeginning = (cxt.columns == NIL);

					transformTableLikeClause(&cxt, (TableLikeClause *) element, true, NULL, NULL);

					if (Gp_role == GP_ROLE_DISPATCH && isBeginning &&
						stmt->distributedBy == NULL &&
						iswritable /* dont bother if readable table */)
					{
						likeDistributedBy = getLikeDistributionPolicy((TableLikeClause *) element);
					}
				}
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(element));
				break;
		}
	}

	/*
	 * Forbid LOG ERRORS and ON MASTER combination.
	 */
	if (stmt->exttypedesc->exttabletype == EXTTBL_TYPE_EXECUTE)
	{
		ListCell   *exec_location_opt;

		foreach(exec_location_opt, stmt->exttypedesc->on_clause)
		{
			DefElem    *defel = (DefElem *) lfirst(exec_location_opt);

			if (strcmp(defel->defname, "master") == 0)
			{
				SingleRowErrorDesc *srehDesc = (SingleRowErrorDesc *)stmt->sreh;

				if(srehDesc && srehDesc->log_error_type != LOG_ERRORS_DISABLE)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("external web table with ON MASTER clause cannot use LOG ERRORS feature")));
			}
		}
	}

	/*
	 * Handle DISTRIBUTED BY clause, if any.
	 *
	 * For writeable external tables, by default we distribute RANDOMLY, or
	 * by the distribution key of the LIKE table if exists. However, if
	 * DISTRIBUTED BY was specified we use it by calling the regular
	 * transformDistributedBy and handle it like we would for non external
	 * tables.
	 *
	 * For readable external tables, don't create a policy row at all.
	 * Non-EXECUTE type external tables are implicitly randomly distributed.
	 * EXECUTE type external tables encapsulate similar information in the
	 * "ON <segment spec>" clause, which is stored in pg_exttable.location.
	 */
	if (iswritable)
	{
		if (stmt->distributedBy == NULL && likeDistributedBy == NULL)
		{
			/*
			 * defaults to DISTRIBUTED RANDOMLY irrespective of the
			 * gp_create_table_random_default_distribution guc.
			 */
			stmt->distributedBy = makeNode(DistributedBy);
			stmt->distributedBy->ptype = POLICYTYPE_PARTITIONED;
			stmt->distributedBy->keyCols = NIL;
			stmt->distributedBy->numsegments = GP_POLICY_DEFAULT_NUMSEGMENTS();
		}
		else
		{
			/* regular DISTRIBUTED BY transformation */
			stmt->distributedBy = transformDistributedBy(&cxt, stmt->distributedBy,
								   (DistributedBy *)likeDistributedBy, bQuiet);
			if (stmt->distributedBy->ptype == POLICYTYPE_REPLICATED)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("external tables can't have DISTRIBUTED REPLICATED clause")));
		}
	}
	else if (stmt->distributedBy != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("readable external tables can\'t specify a DISTRIBUTED BY clause")));

	Assert(cxt.ckconstraints == NIL);
	Assert(cxt.fkconstraints == NIL);
	Assert(cxt.ixconstraints == NIL);

	/*
	 * Output results.
	 */
	stmt->tableElts = cxt.columns;

	result = lappend(cxt.blist, stmt);
	result = list_concat(result, cxt.alist);

	MemoryContextDelete(cxt.tempCtx);

	return result;
}

/*
 * Process a DISTRIBUTED BY clause.
 *
 * If no DISTRIBUTED BY was given, this deduces a suitable default based on
 * various things.
 *
 * NOTE: We cannot form a GpPolicy object yet, because we don't know the
 * attribute numbers the columns will get. With inheritance, the table might
 * inherit more columns from a parent table, which are not visible in the
 * CreateStmt.
 */
static DistributedBy *
transformDistributedBy(CreateStmtContext *cxt,
					   DistributedBy *distributedBy,
					   DistributedBy *likeDistributedBy,
					   bool bQuiet)
{
	ListCell	*keys = NULL;
	List		*distrkeys = NIL;
	ListCell   *lc;
	int			numsegments;

	/*
	 * utility mode creates can't have a policy.  Only the QD can have policies
	 */
	if (Gp_role != GP_ROLE_DISPATCH && !IsBinaryUpgrade)
		return NULL;

	if (distributedBy && distributedBy->numsegments > 0)
		/* If numsegments is set in DISTRIBUTED BY use the specified value */
		numsegments = distributedBy->numsegments;
	else
		/* Otherwise use DEFAULT as numsegments */
		numsegments = GP_POLICY_DEFAULT_NUMSEGMENTS();

	/* Explicitly specified distributed randomly, no further check needed */
	if (distributedBy &&
		(distributedBy->ptype == POLICYTYPE_PARTITIONED && distributedBy->keyCols == NIL))
	{
		distributedBy->numsegments = numsegments;
		return distributedBy;
	}

	/* Check replicated policy */
	if (distributedBy && distributedBy->ptype == POLICYTYPE_REPLICATED)
	{
		if (cxt->inhRelations != NIL)
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("INHERITS clause cannot be used with DISTRIBUTED REPLICATED clause")));

		distributedBy->numsegments = numsegments;
		return distributedBy;
	}

	if (distributedBy)
		distrkeys = distributedBy->keyCols;

	/*
	 * If distributedBy is NIL, the user did not explicitly say what he
	 * wanted for a distribution policy.  So, we need to assign one.
	 */
	if (distrkeys == NIL)
	{
		/*
		 * If we have a PRIMARY KEY or UNIQUE constraints, derive the distribution key
		 * from them.
		 *
		 * The distribution key chosen to be the largest common subset of columns, across
		 * all the PRIMARY KEY / UNIQUE constraints.
		 */
		/* begin with the PRIMARY KEY, if any */
		if (cxt->pkey != NULL)
		{
			IndexStmt  *index = cxt->pkey;
			List	   *indexParams;
			ListCell   *ip;

			Assert(index->indexParams != NULL);
			indexParams = index->indexParams;

			foreach(ip, indexParams)
			{
				IndexElem  *iparam = lfirst(ip);

				if (iparam && iparam->name != 0)
				{
					IndexElem *distrkey = makeNode(IndexElem);

					distrkey->name = iparam->name;
					distrkey->opclass = NULL;

					distrkeys = lappend(distrkeys, distrkey);
				}
			}
		}

		/* walk through all UNIQUE constraints next. */
		foreach(lc, cxt->ixconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(lc);
			ListCell   *ip;
			List	   *new_distrkeys = NIL;

			if (constraint->contype != CONSTR_UNIQUE)
				continue;

			if (distrkeys)
			{
				/*
				 * We saw a PRIMARY KEY or UNIQUE constraint already. Find
				 * the columns that are present in the key chosen so far,
				 * and this constraint.
				 */
				foreach(ip, constraint->keys)
				{
					Value	   *v = lfirst(ip);
					ListCell   *dkcell;

					foreach(dkcell, distrkeys)
					{
						IndexElem  *dk = (IndexElem *) lfirst(dkcell);

						if (strcmp(dk->name, strVal(v)) == 0)
						{
							new_distrkeys = lappend(new_distrkeys, dk);
							break;
						}
					}
				}

				/* If there were no common columns, we're out of luck. */
				if (new_distrkeys == NIL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("UNIQUE or PRIMARY KEY definitions are incompatible with each other"),
							 errhint("When there are multiple PRIMARY KEY / UNIQUE constraints, they must have at least one column in common.")));
			}
			else
			{
				/*
				 * No distribution key chosen yet. Use this key as is.
				 */
				new_distrkeys = NIL;
				foreach(ip, constraint->keys)
				{
					Value	   *v = lfirst(ip);
					IndexElem  *dk = makeNode(IndexElem);

					dk->name = strVal(v);
					dk->opclass = NULL;

					new_distrkeys = lappend(new_distrkeys, dk);
				}
			}

			distrkeys = new_distrkeys;
		}
	}

	/*
	 * If new table INHERITS from one or more parent tables, check parents.
	 */
	if (cxt->inhRelations != NIL)
	{
		ListCell   *entry;

		foreach(entry, cxt->inhRelations)
		{
			RangeVar   *parent = (RangeVar *) lfirst(entry);
			GpPolicy   *parentPolicy;
			Relation	parentrel;

			parentrel = heap_openrv(parent, AccessShareLock);
			parentPolicy = parentrel->rd_cdbpolicy;

			if (parentrel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot inherit from foreign table \"%s\" to create table \"%s\"",
								parent->relname, cxt->relation->relname),
						 errdetail("An inheritance hierarchy cannot contain a mixture of distributed and non-distributed tables.")));
			}

			/*
			 * Partitioned child must have partitioned parents. During binary
			 * upgrade we allow to skip this check since that runs against a
			 * segment in utility mode and the distribution policy isn't stored
			 * in the segments.
			 */
			if ((parentPolicy == NULL ||
					parentPolicy->ptype == POLICYTYPE_ENTRY) &&
					!IsBinaryUpgrade)
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot inherit from catalog table \"%s\" to create table \"%s\"",
								parent->relname, cxt->relation->relname),
						 errdetail("An inheritance hierarchy cannot contain a mixture of distributed and non-distributed tables.")));
			}

			if ((parentPolicy == NULL ||
					GpPolicyIsReplicated(parentPolicy)) &&
					!IsBinaryUpgrade)
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot inherit from replicated table \"%s\" to create table \"%s\"",
								parent->relname, cxt->relation->relname),
						 errdetail("An inheritance hierarchy cannot contain a mixture of distributed and non-distributed tables.")));
			}

			/*
			 * If we still don't know what distribution to use, and this
			 * is an inherited table, set the distribution based on the
			 * parent (or one of the parents)
			 */
			if (distrkeys == NIL && parentPolicy->nattrs >= 0)
			{
				if (!bQuiet)
					ereport(NOTICE,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("table has parent, setting distribution columns to match parent table")));

				distributedBy = make_distributedby_for_rel(parentrel);
				heap_close(parentrel, AccessShareLock);

				distributedBy->numsegments = numsegments;
				return distributedBy;
			}
			heap_close(parentrel, AccessShareLock);
		}
	}

	if (distrkeys == NIL && likeDistributedBy != NULL)
	{
		if (!bQuiet)
			ereport(NOTICE,
					(errmsg("table doesn't have 'DISTRIBUTED BY' clause, defaulting to distribution columns from LIKE table")));

		if (likeDistributedBy->ptype == POLICYTYPE_PARTITIONED &&
			likeDistributedBy->keyCols == NIL)
		{
			distributedBy = makeNode(DistributedBy);
			distributedBy->ptype = POLICYTYPE_PARTITIONED;
			distributedBy->numsegments = numsegments;
			return distributedBy;
		}
		else if (likeDistributedBy->ptype == POLICYTYPE_REPLICATED)
		{
			distributedBy = makeNode(DistributedBy);
			distributedBy->ptype = POLICYTYPE_REPLICATED;
			distributedBy->numsegments = numsegments;
			return distributedBy;
		}

		distrkeys = likeDistributedBy->keyCols;
	}

	if (gp_create_table_random_default_distribution && NIL == distrkeys)
	{
		Assert(NULL == likeDistributedBy);

		if (!bQuiet)
		{
			ereport(NOTICE,
				(errcode(ERRCODE_SUCCESSFUL_COMPLETION),
				 errmsg("using default RANDOM distribution since no distribution was specified"),
				 errhint("Consider including the 'DISTRIBUTED BY' clause to determine the distribution of rows.")));
		}

		distributedBy = makeNode(DistributedBy);
		distributedBy->ptype = POLICYTYPE_PARTITIONED;
		distributedBy->numsegments = numsegments;
		return distributedBy;
	}
	else if (distrkeys == NIL)
	{
		/*
		 * if we get here, we haven't a clue what to use for the distribution columns.
		 * table has one or more attributes and there is still no distribution
		 * key. pick a default one. the winner is the first attribute that is
		 * an Greenplum Database-hashable data type.
		 */

		ListCell   *columns;

		if (cxt->inhRelations)
		{
			bool		found = false;
			/* try inherited tables */
			ListCell   *inher;

			foreach(inher, cxt->inhRelations)
			{
				RangeVar   *inh = (RangeVar *) lfirst(inher);
				Relation	rel;
				int			count;

				Assert(IsA(inh, RangeVar));
				rel = heap_openrv(inh, AccessShareLock);
				if (rel->rd_rel->relkind != RELKIND_RELATION)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("inherited relation \"%s\" is not a table",
									inh->relname)));
				for (count = 0; count < rel->rd_att->natts; count++)
				{
					Form_pg_attribute inhattr = TupleDescAttr(rel->rd_att, count);
					Oid typeOid = inhattr->atttypid;

					if (inhattr->attisdropped)
						continue;
					if (cdb_default_distribution_opclass_for_type(typeOid) != InvalidOid)
					{
						char	   *inhname = NameStr(inhattr->attname);
						IndexElem  *ielem;

						ielem = makeNode(IndexElem);
						ielem->name = inhname;
						ielem->opclass = NULL;

						distrkeys = list_make1(ielem);
						if (!bQuiet)
							ereport(NOTICE,
								(errcode(ERRCODE_SUCCESSFUL_COMPLETION),
								 errmsg("Table doesn't have 'DISTRIBUTED BY' clause -- Using column "
										"named '%s' from parent table as the Greenplum Database data distribution key for this "
										"table. ", inhname),
								 errhint("The 'DISTRIBUTED BY' clause determines the distribution of data."
								 		 " Make sure column(s) chosen are the optimal data distribution key to minimize skew.")));
						found = true;
						break;
					}
				}
				heap_close(rel, NoLock);

				if (distrkeys != NIL)
					break;
			}

		}

		if (distrkeys == NIL)
		{
			foreach(columns, cxt->columns)
			{
				ColumnDef  *column = (ColumnDef *) lfirst(columns);
				Oid			typeOid;

				typeOid = typenameTypeId(NULL, column->typeName);

				/*
				 * If we can hash this type, this column will be our default
				 * key.
				 */
				if (cdb_default_distribution_opclass_for_type(typeOid))
				{
					IndexElem  *ielem = makeNode(IndexElem);

					ielem->name = column->colname;
					ielem->opclass = NULL;		/* or should we explicitly set the opclass we just looked up? */

					distrkeys = list_make1(ielem);
					if (!bQuiet)
						ereport(NOTICE,
							(errcode(ERRCODE_SUCCESSFUL_COMPLETION),
							 errmsg("Table doesn't have 'DISTRIBUTED BY' clause -- Using column "
									"named '%s' as the Greenplum Database data distribution key for this "
									"table. ", column->colname),
							 errhint("The 'DISTRIBUTED BY' clause determines the distribution of data."
							 		 " Make sure column(s) chosen are the optimal data distribution key to minimize skew.")));
					break;
				}
			}
		}

		if (distrkeys == NIL)
		{
			/*
			 * There was no eligible distribution column to default to. This table
			 * will be partitioned on an empty distribution key list. In other words,
			 * tuples coming into the system will be randomly assigned a bucket.
			 */
			if (!bQuiet)
				ereport(NOTICE,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("Table doesn't have 'DISTRIBUTED BY' clause, and no column type is suitable for a distribution key. Creating a NULL policy entry.")));

			distributedBy = makeNode(DistributedBy);
			distributedBy->ptype = POLICYTYPE_PARTITIONED;
			distributedBy->numsegments = numsegments;
			return distributedBy;
		}
	}
	else
	{
		/*
		 * We have a DISTRIBUTED BY column list, either specified by the user
		 * or defaulted to a primary key or unique column. Process it now.
		 */
		foreach(keys, distrkeys)
		{
			IndexElem  *ielem = (IndexElem *) lfirst(keys);
			char	   *colname = ielem->name;
			bool		found = false;
			ListCell   *columns;

			if (cxt->inhRelations)
			{
				/* try inherited tables */
				ListCell   *inher;

				foreach(inher, cxt->inhRelations)
				{
					RangeVar   *inh = (RangeVar *) lfirst(inher);
					Relation	rel;
					int			count;

					Assert(IsA(inh, RangeVar));
					rel = heap_openrv(inh, AccessShareLock);
					if (rel->rd_rel->relkind != RELKIND_RELATION)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("inherited relation \"%s\" is not a table",
										inh->relname)));
					for (count = 0; count < rel->rd_att->natts; count++)
					{
						Form_pg_attribute inhattr = TupleDescAttr(rel->rd_att, count);
						char	   *inhname = NameStr(inhattr->attname);

						if (inhattr->attisdropped)
							continue;
						if (strcmp(colname, inhname) == 0)
						{
							found = true;

							break;
						}
					}
					heap_close(rel, NoLock);
					if (found)
						elog(DEBUG1, "DISTRIBUTED BY clause refers to columns of inherited table");

					if (found)
						break;
				}
			}

			if (!found)
			{
				foreach(columns, cxt->columns)
				{
					ColumnDef *column = (ColumnDef *) lfirst(columns);
					Assert(IsA(column, ColumnDef));

					if (strcmp(column->colname, colname) == 0)
					{
						found = true;
						break;
					}
				}
			}

			/*
			 * In the ALTER TABLE case, don't complain about index keys
			 * not created in the command; they may well exist already.
			 * DefineIndex will complain about them if not, and will also
			 * take care of marking them NOT NULL.
			 */
			if (!found && !cxt->isalter)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" named in 'DISTRIBUTED BY' clause does not exist",
								colname)));
		}
	}

	/*
	 * Ok, we have decided on the distribution key columns now, and have the column
	 * names in 'distrkeys'. Perform last cross-checks between UNIQUE and PRIMARY KEY
	 * constraints and the chosen distribution key. (These tests should always pass,
	 * if the distribution key was derived from the PRIMARY KEY or UNIQUE constraints,
	 * but it doesn't hurt to check even in those cases.)
	 */
	if (cxt && cxt->pkey)
	{
		/* The distribution key must be a subset of the primary key */
		IndexStmt  *index = cxt->pkey;
		ListCell   *dk;

		foreach(dk, distrkeys)
		{
			char	   *distcolname = strVal(lfirst(dk));
			ListCell   *ip;
			bool		found = false;

			foreach(ip, index->indexParams)
			{
				IndexElem  *iparam = lfirst(ip);

				if (!iparam->name)
					elog(ERROR, "PRIMARY KEY on an expression index not supported");

				if (strcmp(iparam->name, distcolname) == 0)
				{
					found = true;
					break;
				}
			}

			if (!found)
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("PRIMARY KEY and DISTRIBUTED BY definitions are incompatible"),
						 errhint("When there is both a PRIMARY KEY and a DISTRIBUTED BY clause, the DISTRIBUTED BY clause must be a subset of the PRIMARY KEY.")));
			}
		}
	}

	/* Make sure distribution columns match any UNIQUE and PRIMARY KEY constraints. */
	foreach (lc, cxt->ixconstraints)
	{
		Constraint *constraint = (Constraint *) lfirst(lc);
		ListCell   *dk;

		if (constraint->contype != CONSTR_PRIMARY &&
			constraint->contype != CONSTR_UNIQUE)
			continue;

		foreach(dk, distrkeys)
		{
			char	   *distcolname = strVal(lfirst(dk));
			ListCell   *ip;
			bool		found = false;

			foreach (ip, constraint->keys)
			{
				IndexElem  *iparam = lfirst(ip);

				if (!iparam->name)
					elog(ERROR, "UNIQUE constraint on an expression index not supported");

				if (strcmp(iparam->name, distcolname) == 0)
				{
					found = true;
					break;
				}
			}

			if (!found)
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("UNIQUE constraint and DISTRIBUTED BY definitions are incompatible"),
						 errhint("When there is both a UNIQUE constraint and a DISTRIBUTED BY clause, the DISTRIBUTED BY clause must be a subset of the UNIQUE constraint.")));
			}
		}
	}

	/* Form the resulting Distributed By clause */
	distributedBy = makeNode(DistributedBy);
	distributedBy->ptype = POLICYTYPE_PARTITIONED;
	distributedBy->keyCols = distrkeys;
	distributedBy->numsegments = numsegments;

	return distributedBy;
}

/*
 * Given a DistributedBy clause, construct a GpPolicy for it.
 */
GpPolicy *
getPolicyForDistributedBy(DistributedBy *distributedBy, TupleDesc tupdesc)
{
	List	   *policykeys;
	List	   *policyopclasses;
	ListCell   *lc;

	if (!distributedBy)
		return NULL; /* XXX or should we complain? */

	switch(distributedBy->ptype)
	{
		case POLICYTYPE_PARTITIONED:
			/* Look up the attribute numbers for each column */
			policykeys = NIL;
			policyopclasses = NIL;
			foreach(lc, distributedBy->keyCols)
			{
				IndexElem  *ielem = (IndexElem *) lfirst(lc);
				char	   *colname = ielem->name;
				int			i;
				bool		found = false;

				for (i = 0; i < tupdesc->natts; i++)
				{
					Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

					if (strcmp(colname, NameStr(attr->attname)) == 0)
					{
						Oid			opclass;

						opclass = cdb_get_opclass_for_column_def(ielem->opclass, attr->atttypid);

						policykeys = lappend_int(policykeys, attr->attnum);
						policyopclasses = lappend_oid(policyopclasses, opclass);
						found = true;
					}
				}
				if (!found)
					elog(ERROR, "could not find DISTRIBUTED BY column \"%s\"", colname);
			}

			return createHashPartitionedPolicy(policykeys,
											   policyopclasses,
											   distributedBy->numsegments);;

		case POLICYTYPE_ENTRY:
			elog(ERROR, "unexpected entry distribution policy");
			return NULL;

		case POLICYTYPE_REPLICATED:
			return createReplicatedGpPolicy(distributedBy->numsegments);
	}
	elog(ERROR, "unrecognized policy type %d", distributedBy->ptype);
	return NULL;
}

/*
 * Add any missing encoding attributes (compresstype = none,
 * blocksize=...).  The column specific encoding attributes supported
 * today are compresstype, compresslevel and blocksize.  Refer to
 * pg_compression.c for more info.
 */
static List *
fillin_encoding(List *list)
{
	bool foundCompressType = false;
	bool foundCompressTypeNone = false;
	char *cmplevel = NULL;
	bool foundBlockSize = false;
	char *arg;
	List *retList = list_copy(list);
	ListCell *lc;
	DefElem *el;
	const StdRdOptions *ao_opts = currentAOStorageOptions();

	foreach(lc, list)
	{
		el = lfirst(lc);

		if (pg_strcasecmp("compresstype", el->defname) == 0)
		{
			foundCompressType = true;
			arg = defGetString(el);
			if (pg_strcasecmp("none", arg) == 0)
				foundCompressTypeNone = true;
		}
		else if (pg_strcasecmp("compresslevel", el->defname) == 0)
		{
			cmplevel = defGetString(el);
		}
		else if (pg_strcasecmp("blocksize", el->defname) == 0)
			foundBlockSize = true;
	}

	if (foundCompressType == false && cmplevel == NULL)
	{
		/* No compression option specified, use current defaults. */
		arg = ao_opts->compresstype[0] ?
				pstrdup(ao_opts->compresstype) : "none";
		el = makeDefElem("compresstype", (Node *) makeString(arg), -1);
		retList = lappend(retList, el);
		el = makeDefElem("compresslevel",
						 (Node *) makeInteger(ao_opts->compresslevel),
						 -1);
		retList = lappend(retList, el);
	}
	else if (foundCompressType == false && cmplevel)
	{
		if (strcmp(cmplevel, "0") == 0)
		{
			/*
			 * User wants to disable compression by specifying
			 * compresslevel=0.
			 */
			el = makeDefElem("compresstype", (Node *) makeString("none"), -1);
			retList = lappend(retList, el);
		}
		else
		{
			/*
			 * User wants to enable compression by specifying non-zero
			 * compresslevel.  Therefore, choose default compresstype
			 * if configured, otherwise use zlib.
			 */
			if (ao_opts->compresstype[0] &&
				strcmp(ao_opts->compresstype, "none") != 0)
			{
				arg = pstrdup(ao_opts->compresstype);
			}
			else
			{
				arg = AO_DEFAULT_COMPRESSTYPE;
			}
			el = makeDefElem("compresstype", (Node *) makeString(arg), -1);
			retList = lappend(retList, el);
		}
	}
	else if (foundCompressType && cmplevel == NULL)
	{
		if (foundCompressTypeNone)
		{
			/*
			 * User wants to disable compression by specifying
			 * compresstype=none.
			 */
			el = makeDefElem("compresslevel", (Node *) makeInteger(0), -1);
			retList = lappend(retList, el);
		}
		else
		{
			/*
			 * Valid compresstype specified.  Use default
			 * compresslevel if it's non-zero, otherwise use 1.
			 */
			el = makeDefElem("compresslevel",
							 (Node *) makeInteger(ao_opts->compresslevel > 0 ?
												  ao_opts->compresslevel : 1),
							 -1);
			retList = lappend(retList, el);
		}
	}
	if (foundBlockSize == false)
	{
		el = makeDefElem("blocksize", (Node *) makeInteger(ao_opts->blocksize), -1);
		retList = lappend(retList, el);
	}
	return retList;
}

/*
 * transformIndexConstraints
 *		Handle UNIQUE, PRIMARY KEY, EXCLUDE constraints, which create indexes.
 *		We also merge in any index definitions arising from
 *		LIKE ... INCLUDING INDEXES.
 */
static void
transformIndexConstraints(CreateStmtContext *cxt)
{
	IndexStmt  *index;
	List	   *indexlist = NIL;
	List	   *finalindexlist = NIL;
	ListCell   *lc;

	/*
	 * Run through the constraints that need to generate an index. For PRIMARY
	 * KEY, mark each column as NOT NULL and create an index. For UNIQUE or
	 * EXCLUDE, create an index as for PRIMARY KEY, but do not insist on NOT
	 * NULL.
	 */
	foreach(lc, cxt->ixconstraints)
	{
		Constraint *constraint = lfirst_node(Constraint, lc);

		Assert(constraint->contype == CONSTR_PRIMARY ||
			   constraint->contype == CONSTR_UNIQUE ||
			   constraint->contype == CONSTR_EXCLUSION);

		index = transformIndexConstraint(constraint, cxt);

		indexlist = lappend(indexlist, index);
	}

	/* Add in any indexes defined by LIKE ... INCLUDING INDEXES */
	foreach(lc, cxt->inh_indexes)
	{
		index = (IndexStmt *) lfirst(lc);

		if (index->primary)
		{
			if (cxt->pkey != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("multiple primary keys for table \"%s\" are not allowed",
								cxt->relation->relname)));
			cxt->pkey = index;
		}

		indexlist = lappend(indexlist, index);
	}

	/*
	 * Scan the index list and remove any redundant index specifications. This
	 * can happen if, for instance, the user writes UNIQUE PRIMARY KEY. A
	 * strict reading of SQL would suggest raising an error instead, but that
	 * strikes me as too anal-retentive. - tgl 2001-02-14
	 *
	 * XXX in ALTER TABLE case, it'd be nice to look for duplicate
	 * pre-existing indexes, too.
	 */
	if (cxt->pkey != NULL)
	{
		/* Make sure we keep the PKEY index in preference to others... */
		finalindexlist = list_make1(cxt->pkey);
	}

	foreach(lc, indexlist)
	{
		bool		keep = true;
		bool		defer = false;
		ListCell   *k;

		index = lfirst(lc);

		/* if it's pkey, it's already in finalindexlist */
		if (index == cxt->pkey)
			continue;

		foreach(k, finalindexlist)
		{
			IndexStmt  *priorindex = lfirst(k);

			if (equal(index->indexParams, priorindex->indexParams) &&
				equal(index->indexIncludingParams, priorindex->indexIncludingParams) &&
				equal(index->whereClause, priorindex->whereClause) &&
				equal(index->excludeOpNames, priorindex->excludeOpNames) &&
				strcmp(index->accessMethod, priorindex->accessMethod) == 0 &&
				index->deferrable == priorindex->deferrable &&
				index->initdeferred == priorindex->initdeferred)
			{
				priorindex->unique |= index->unique;

				/*
				 * If the prior index is as yet unnamed, and this one is
				 * named, then transfer the name to the prior index. This
				 * ensures that if we have named and unnamed constraints,
				 * we'll use (at least one of) the names for the index.
				 */
				if (priorindex->idxname == NULL)
					priorindex->idxname = index->idxname;
				keep = false;
				break;
			}
		}
		
		defer = index->whereClause != NULL;
		if ( !defer )
		{
			ListCell *j;
			foreach(j, index->indexParams)
			{
				IndexElem *elt = (IndexElem*)lfirst(j);
				Assert(IsA(elt, IndexElem));
				
				if (elt->expr != NULL)
				{
					defer = true;
					break;
				}
			}
		}

		if (keep)
			finalindexlist = lappend(finalindexlist, index);
	}

	/*
	 * Now append all the IndexStmts to cxt->alist.  If we generated an ALTER
	 * TABLE SET NOT NULL statement to support a primary key, it's already in
	 * cxt->alist.
	 */
	cxt->alist = list_concat(cxt->alist, finalindexlist);
}

/*
 * transformIndexConstraint
 *		Transform one UNIQUE, PRIMARY KEY, or EXCLUDE constraint for
 *		transformIndexConstraints.
 *
 * We return an IndexStmt.  For a PRIMARY KEY constraint, we additionally
 * produce NOT NULL constraints, either by marking ColumnDefs in cxt->columns
 * as is_not_null or by adding an ALTER TABLE SET NOT NULL command to
 * cxt->alist.
 */
static IndexStmt *
transformIndexConstraint(Constraint *constraint, CreateStmtContext *cxt)
{
	IndexStmt  *index;
	List	   *notnullcmds = NIL;
	ListCell   *lc;

	index = makeNode(IndexStmt);

	index->unique = (constraint->contype != CONSTR_EXCLUSION);
	index->primary = (constraint->contype == CONSTR_PRIMARY);
	if (index->primary)
	{
		if (cxt->pkey != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("multiple primary keys for table \"%s\" are not allowed",
							cxt->relation->relname),
					 parser_errposition(cxt->pstate, constraint->location)));
		cxt->pkey = index;

		/*
		 * In ALTER TABLE case, a primary index might already exist, but
		 * DefineIndex will check for it.
		 */
	}
	index->isconstraint = true;
	index->deferrable = constraint->deferrable;
	index->initdeferred = constraint->initdeferred;

	if (constraint->conname != NULL)
		index->idxname = pstrdup(constraint->conname);
	else
		index->idxname = NULL;	/* DefineIndex will choose name */

	index->relation = cxt->relation;
	index->accessMethod = constraint->access_method ? constraint->access_method : DEFAULT_INDEX_TYPE;
	index->options = constraint->options;
	index->tableSpace = constraint->indexspace;
	index->whereClause = constraint->where_clause;
	index->indexParams = NIL;
	index->indexIncludingParams = NIL;
	index->excludeOpNames = NIL;
	index->idxcomment = NULL;
	index->indexOid = InvalidOid;
	index->oldNode = InvalidOid;
	index->transformed = false;
	index->concurrent = false;
	index->if_not_exists = false;
	index->reset_default_tblspc = constraint->reset_default_tblspc;

	/*
	 * If it's ALTER TABLE ADD CONSTRAINT USING INDEX, look up the index and
	 * verify it's usable, then extract the implied column name list.  (We
	 * will not actually need the column name list at runtime, but we need it
	 * now to check for duplicate column entries below.)
	 */
	if (constraint->indexname != NULL)
	{
		char	   *index_name = constraint->indexname;
		Relation	heap_rel = cxt->rel;
		Oid			index_oid;
		Relation	index_rel;
		Form_pg_index index_form;
		oidvector  *indclass;
		Datum		indclassDatum;
		bool		isnull;
		int			i;

		/* Grammar should not allow this with explicit column list */
		Assert(constraint->keys == NIL);

		/* Grammar should only allow PRIMARY and UNIQUE constraints */
		Assert(constraint->contype == CONSTR_PRIMARY ||
			   constraint->contype == CONSTR_UNIQUE);

		/* Must be ALTER, not CREATE, but grammar doesn't enforce that */
		if (!cxt->isalter)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot use an existing index in CREATE TABLE"),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* Look for the index in the same schema as the table */
		index_oid = get_relname_relid(index_name, RelationGetNamespace(heap_rel));

		if (!OidIsValid(index_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("index \"%s\" does not exist", index_name),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* Open the index (this will throw an error if it is not an index) */
		index_rel = index_open(index_oid, AccessShareLock);
		index_form = index_rel->rd_index;

		/* Check that it does not have an associated constraint already */
		if (OidIsValid(get_index_constraint(index_oid)))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("index \"%s\" is already associated with a constraint",
							index_name),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* Perform validity checks on the index */
		if (index_form->indrelid != RelationGetRelid(heap_rel))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("index \"%s\" does not belong to table \"%s\"",
							index_name, RelationGetRelationName(heap_rel)),
					 parser_errposition(cxt->pstate, constraint->location)));

		if (!index_form->indisvalid)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("index \"%s\" is not valid", index_name),
					 parser_errposition(cxt->pstate, constraint->location)));

		if (!index_form->indisunique)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a unique index", index_name),
					 errdetail("Cannot create a primary key or unique constraint using such an index."),
					 parser_errposition(cxt->pstate, constraint->location)));

		if (RelationGetIndexExpressions(index_rel) != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" contains expressions", index_name),
					 errdetail("Cannot create a primary key or unique constraint using such an index."),
					 parser_errposition(cxt->pstate, constraint->location)));

		if (RelationGetIndexPredicate(index_rel) != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a partial index", index_name),
					 errdetail("Cannot create a primary key or unique constraint using such an index."),
					 parser_errposition(cxt->pstate, constraint->location)));

		/*
		 * It's probably unsafe to change a deferred index to non-deferred. (A
		 * non-constraint index couldn't be deferred anyway, so this case
		 * should never occur; no need to sweat, but let's check it.)
		 */
		if (!index_form->indimmediate && !constraint->deferrable)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a deferrable index", index_name),
					 errdetail("Cannot create a non-deferrable constraint using a deferrable index."),
					 parser_errposition(cxt->pstate, constraint->location)));

		/*
		 * Insist on it being a btree.  That's the only kind that supports
		 * uniqueness at the moment anyway; but we must have an index that
		 * exactly matches what you'd get from plain ADD CONSTRAINT syntax,
		 * else dump and reload will produce a different index (breaking
		 * pg_upgrade in particular).
		 */
		if (index_rel->rd_rel->relam != get_index_am_oid(DEFAULT_INDEX_TYPE, false))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" is not a btree", index_name),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* Must get indclass the hard way */
		indclassDatum = SysCacheGetAttr(INDEXRELID, index_rel->rd_indextuple,
										Anum_pg_index_indclass, &isnull);
		Assert(!isnull);
		indclass = (oidvector *) DatumGetPointer(indclassDatum);

		for (i = 0; i < index_form->indnatts; i++)
		{
			int16		attnum = index_form->indkey.values[i];
			const FormData_pg_attribute *attform;
			char	   *attname;
			Oid			defopclass;

			/*
			 * We shouldn't see attnum == 0 here, since we already rejected
			 * expression indexes.  If we do, SystemAttributeDefinition will
			 * throw an error.
			 */
			if (attnum > 0)
			{
				Assert(attnum <= heap_rel->rd_att->natts);
				attform = TupleDescAttr(heap_rel->rd_att, attnum - 1);
			}
			else
				attform = SystemAttributeDefinition(attnum);
			attname = pstrdup(NameStr(attform->attname));

			if (i < index_form->indnkeyatts)
			{
				/*
				 * Insist on default opclass and sort options.  While the
				 * index would still work as a constraint with non-default
				 * settings, it might not provide exactly the same uniqueness
				 * semantics as you'd get from a normally-created constraint;
				 * and there's also the dump/reload problem mentioned above.
				 */
				defopclass = GetDefaultOpClass(attform->atttypid,
											   index_rel->rd_rel->relam);
				if (indclass->values[i] != defopclass ||
					index_rel->rd_indoption[i] != 0)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("index \"%s\" column number %d does not have default sorting behavior", index_name, i + 1),
							 errdetail("Cannot create a primary key or unique constraint using such an index."),
							 parser_errposition(cxt->pstate, constraint->location)));

				constraint->keys = lappend(constraint->keys, makeString(attname));
			}
			else
				constraint->including = lappend(constraint->including, makeString(attname));
		}

		/* Close the index relation but keep the lock */
		relation_close(index_rel, NoLock);

		index->indexOid = index_oid;
	}

	/*
	 * If it's an EXCLUDE constraint, the grammar returns a list of pairs of
	 * IndexElems and operator names.  We have to break that apart into
	 * separate lists.
	 */
	if (constraint->contype == CONSTR_EXCLUSION)
	{
		foreach(lc, constraint->exclusions)
		{
			List	   *pair = (List *) lfirst(lc);
			IndexElem  *elem;
			List	   *opname;

			Assert(list_length(pair) == 2);
			elem = linitial_node(IndexElem, pair);
			opname = lsecond_node(List, pair);

			index->indexParams = lappend(index->indexParams, elem);
			index->excludeOpNames = lappend(index->excludeOpNames, opname);
		}
	}

	/*
	 * For UNIQUE and PRIMARY KEY, we just have a list of column names.
	 *
	 * Make sure referenced keys exist.  If we are making a PRIMARY KEY index,
	 * also make sure they are NOT NULL.
	 */
	else
	{
		foreach(lc, constraint->keys)
		{
			char	   *key = strVal(lfirst(lc));
			bool		found = false;
			bool		forced_not_null = false;
			ColumnDef  *column = NULL;
			ListCell   *columns;
			IndexElem  *iparam;

			/* Make sure referenced column exists. */
			foreach(columns, cxt->columns)
			{
				column = castNode(ColumnDef, lfirst(columns));
				if (strcmp(column->colname, key) == 0)
				{
					found = true;
					break;
				}
			}
			if (found)
			{
				/*
				 * column is defined in the new table.  For PRIMARY KEY, we
				 * can apply the NOT NULL constraint cheaply here ... unless
				 * the column is marked is_from_type, in which case marking it
				 * here would be ineffective (see MergeAttributes).
				 */
				if (constraint->contype == CONSTR_PRIMARY &&
					!column->is_from_type)
				{
					column->is_not_null = true;
					forced_not_null = true;
				}
			}
			else if (SystemAttributeByName(key) != NULL)
			{
				/*
				 * column will be a system column in the new table, so accept
				 * it. System columns can't ever be null, so no need to worry
				 * about PRIMARY/NOT NULL constraint.
				 */
				found = true;
			}
			else if (cxt->inhRelations)
			{
				/* try inherited tables */
				ListCell   *inher;

				foreach(inher, cxt->inhRelations)
				{
					RangeVar   *inh = castNode(RangeVar, lfirst(inher));
					Relation	rel;
					int			count;

					rel = table_openrv(inh, AccessShareLock);
					/* check user requested inheritance from valid relkind */
					if (rel->rd_rel->relkind != RELKIND_RELATION &&
						rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
						rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("inherited relation \"%s\" is not a table or foreign table",
										inh->relname)));
					for (count = 0; count < rel->rd_att->natts; count++)
					{
						Form_pg_attribute inhattr = TupleDescAttr(rel->rd_att,
																  count);
						char	   *inhname = NameStr(inhattr->attname);

						if (inhattr->attisdropped)
							continue;
						if (strcmp(key, inhname) == 0)
						{
							found = true;

							/*
							 * It's tempting to set forced_not_null if the
							 * parent column is already NOT NULL, but that
							 * seems unsafe because the column's NOT NULL
							 * marking might disappear between now and
							 * execution.  Do the runtime check to be safe.
							 */
							break;
						}
					}
					table_close(rel, NoLock);
					if (found)
						break;
				}
			}

			/*
			 * In the ALTER TABLE case, don't complain about index keys not
			 * created in the command; they may well exist already.
			 * DefineIndex will complain about them if not.
			 */
			if (!found && !cxt->isalter)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" named in key does not exist", key),
						 parser_errposition(cxt->pstate, constraint->location)));

			/* Check for PRIMARY KEY(foo, foo) */
			foreach(columns, index->indexParams)
			{
				iparam = (IndexElem *) lfirst(columns);
				if (iparam->name && strcmp(key, iparam->name) == 0)
				{
					if (index->primary)
						ereport(ERROR,
								(errcode(ERRCODE_DUPLICATE_COLUMN),
								 errmsg("column \"%s\" appears twice in primary key constraint",
										key),
								 parser_errposition(cxt->pstate, constraint->location)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_DUPLICATE_COLUMN),
								 errmsg("column \"%s\" appears twice in unique constraint",
										key),
								 parser_errposition(cxt->pstate, constraint->location)));
				}
			}

			/* OK, add it to the index definition */
			iparam = makeNode(IndexElem);
			iparam->name = pstrdup(key);
			iparam->expr = NULL;
			iparam->indexcolname = NULL;
			iparam->collation = NIL;
			iparam->opclass = NIL;
			iparam->ordering = SORTBY_DEFAULT;
			iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;
			index->indexParams = lappend(index->indexParams, iparam);

			/*
			 * For a primary-key column, also create an item for ALTER TABLE
			 * SET NOT NULL if we couldn't ensure it via is_not_null above.
			 */
			if (constraint->contype == CONSTR_PRIMARY && !forced_not_null)
			{
				AlterTableCmd *notnullcmd = makeNode(AlterTableCmd);

				notnullcmd->subtype = AT_SetNotNull;
				notnullcmd->name = pstrdup(key);
				notnullcmds = lappend(notnullcmds, notnullcmd);
			}
		}
	}

	/*
	 * Add included columns to index definition.  This is much like the
	 * simple-column-name-list code above, except that we don't worry about
	 * NOT NULL marking; included columns in a primary key should not be
	 * forced NOT NULL.  We don't complain about duplicate columns, either,
	 * though maybe we should?
	 */
	foreach(lc, constraint->including)
	{
		char	   *key = strVal(lfirst(lc));
		bool		found = false;
		ColumnDef  *column = NULL;
		ListCell   *columns;
		IndexElem  *iparam;

		foreach(columns, cxt->columns)
		{
			column = lfirst_node(ColumnDef, columns);
			if (strcmp(column->colname, key) == 0)
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			if (SystemAttributeByName(key) != NULL)
			{
				/*
				 * column will be a system column in the new table, so accept
				 * it.
				 */
				found = true;
			}
			else if (cxt->inhRelations)
			{
				/* try inherited tables */
				ListCell   *inher;

				foreach(inher, cxt->inhRelations)
				{
					RangeVar   *inh = lfirst_node(RangeVar, inher);
					Relation	rel;
					int			count;

					rel = table_openrv(inh, AccessShareLock);
					/* check user requested inheritance from valid relkind */
					if (rel->rd_rel->relkind != RELKIND_RELATION &&
						rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
						rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("inherited relation \"%s\" is not a table or foreign table",
										inh->relname)));
					for (count = 0; count < rel->rd_att->natts; count++)
					{
						Form_pg_attribute inhattr = TupleDescAttr(rel->rd_att,
																  count);
						char	   *inhname = NameStr(inhattr->attname);

						if (inhattr->attisdropped)
							continue;
						if (strcmp(key, inhname) == 0)
						{
							found = true;
							break;
						}
					}
					table_close(rel, NoLock);
					if (found)
						break;
				}
			}
		}

		/*
		 * In the ALTER TABLE case, don't complain about index keys not
		 * created in the command; they may well exist already. DefineIndex
		 * will complain about them if not.
		 */
		if (!found && !cxt->isalter)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" named in key does not exist", key),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* OK, add it to the index definition */
		iparam = makeNode(IndexElem);
		iparam->name = pstrdup(key);
		iparam->expr = NULL;
		iparam->indexcolname = NULL;
		iparam->collation = NIL;
		iparam->opclass = NIL;
		index->indexIncludingParams = lappend(index->indexIncludingParams, iparam);
	}

	/*
	 * If we found anything that requires run-time SET NOT NULL, build a full
	 * ALTER TABLE command for that and add it to cxt->alist.
	 */
	if (notnullcmds)
	{
		AlterTableStmt *alterstmt = makeNode(AlterTableStmt);

		alterstmt->relation = copyObject(cxt->relation);
		alterstmt->cmds = notnullcmds;
		alterstmt->relkind = OBJECT_TABLE;
		alterstmt->missing_ok = false;

		cxt->alist = lappend(cxt->alist, alterstmt);
	}

	return index;
}

/*
 * transformExtendedStatistics
 *     Handle extended statistic objects
 *
 * Right now, there's nothing to do here, so we just append the list to
 * the existing "after" list.
 */
static void
transformExtendedStatistics(CreateStmtContext *cxt)
{
	cxt->alist = list_concat(cxt->alist, cxt->extstats);
}

/*
 * transformCheckConstraints
 *		handle CHECK constraints
 *
 * Right now, there's nothing to do here when called from ALTER TABLE,
 * but the other constraint-transformation functions are called in both
 * the CREATE TABLE and ALTER TABLE paths, so do the same here, and just
 * don't do anything if we're not authorized to skip validation.
 */
static void
transformCheckConstraints(CreateStmtContext *cxt, bool skipValidation)
{
	ListCell   *ckclist;

	if (cxt->ckconstraints == NIL)
		return;

	/*
	 * If creating a new table (but not a foreign table), we can safely skip
	 * validation of check constraints, and nonetheless mark them valid. (This
	 * will override any user-supplied NOT VALID flag.)
	 */
	if (skipValidation)
	{
		foreach(ckclist, cxt->ckconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(ckclist);

			constraint->skip_validation = true;
			constraint->initially_valid = true;
		}
	}
}

/*
 * transformFKConstraints
 *		handle FOREIGN KEY constraints
 */
static void
transformFKConstraints(CreateStmtContext *cxt,
					   bool skipValidation, bool isAddConstraint)
{
	ListCell   *fkclist;

	if (cxt->fkconstraints == NIL)
		return;

	/*
	 * If CREATE TABLE or adding a column with NULL default, we can safely
	 * skip validation of FK constraints, and nonetheless mark them valid.
	 * (This will override any user-supplied NOT VALID flag.)
	 */
	if (skipValidation)
	{
		foreach(fkclist, cxt->fkconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(fkclist);

			constraint->skip_validation = true;
			constraint->initially_valid = true;
		}
	}

	/*
	 * For CREATE TABLE or ALTER TABLE ADD COLUMN, gin up an ALTER TABLE ADD
	 * CONSTRAINT command to execute after the basic command is complete. (If
	 * called from ADD CONSTRAINT, that routine will add the FK constraints to
	 * its own subcommand list.)
	 *
	 * Note: the ADD CONSTRAINT command must also execute after any index
	 * creation commands.  Thus, this should run after
	 * transformIndexConstraints, so that the CREATE INDEX commands are
	 * already in cxt->alist.
	 */
	if (!isAddConstraint)
	{
		AlterTableStmt *alterstmt = makeNode(AlterTableStmt);

		alterstmt->relation = cxt->relation;
		alterstmt->cmds = NIL;
		alterstmt->relkind = OBJECT_TABLE;

		foreach(fkclist, cxt->fkconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(fkclist);
			AlterTableCmd *altercmd = makeNode(AlterTableCmd);

			altercmd->subtype = AT_ProcessedConstraint;
			altercmd->name = NULL;
			altercmd->def = (Node *) constraint;
			alterstmt->cmds = lappend(alterstmt->cmds, altercmd);
		}

		cxt->alist = lappend(cxt->alist, alterstmt);
	}
}

/*
 * transformIndexStmt - parse analysis for CREATE INDEX and ALTER TABLE
 *
 * Note: this is a no-op for an index not using either index expressions or
 * a predicate expression.  There are several code paths that create indexes
 * without bothering to call this, because they know they don't have any
 * such expressions to deal with.
 *
 * To avoid race conditions, it's important that this function rely only on
 * the passed-in relid (and not on stmt->relation) to determine the target
 * relation.
 */
IndexStmt *
transformIndexStmt(Oid relid, IndexStmt *stmt, const char *queryString)
{
	ParseState *pstate;
	RangeTblEntry *rte;
	ListCell   *l;
	Relation	rel;

	/* Nothing to do if statement already transformed. */
	if (stmt->transformed)
		return stmt;

	/*
	 * We must not scribble on the passed-in IndexStmt, so copy it.  (This is
	 * overkill, but easy.)
	 */
	stmt = copyObject(stmt);

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * Put the parent table into the rtable so that the expressions can refer
	 * to its fields without qualification.  Caller is responsible for locking
	 * relation, but we still need to open it.
	 */
	rel = relation_open(relid, NoLock);
	rte = addRangeTableEntryForRelation(pstate, rel,
										AccessShareLock,
										NULL, false, true);

	/* no to join list, yes to namespaces */
	addRTEtoQuery(pstate, rte, false, true, true);

	/* take care of the where clause */
	if (stmt->whereClause)
	{
		stmt->whereClause = transformWhereClause(pstate,
												 stmt->whereClause,
												 EXPR_KIND_INDEX_PREDICATE,
												 "WHERE");
		/* we have to fix its collations too */
		assign_expr_collations(pstate, stmt->whereClause);
	}

	/* take care of any index expressions */
	foreach(l, stmt->indexParams)
	{
		IndexElem  *ielem = (IndexElem *) lfirst(l);

		if (ielem->expr)
		{
			/* Extract preliminary index col name before transforming expr */
			if (ielem->indexcolname == NULL)
				ielem->indexcolname = FigureIndexColname(ielem->expr);

			/* Now do parse transformation of the expression */
			ielem->expr = transformExpr(pstate, ielem->expr,
										EXPR_KIND_INDEX_EXPRESSION);

			/* We have to fix its collations too */
			assign_expr_collations(pstate, ielem->expr);

			/*
			 * transformExpr() should have already rejected subqueries,
			 * aggregates, window functions, and SRFs, based on the EXPR_KIND_
			 * for an index expression.
			 *
			 * DefineIndex() will make more checks.
			 */
		}
	}

	/*
	 * Check that only the base rel is mentioned.  (This should be dead code
	 * now that add_missing_from is history.)
	 */
	if (list_length(pstate->p_rtable) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("index expressions and predicates can refer only to the table being indexed")));

	free_parsestate(pstate);

	/* GPDB_12_MERGE_FIXME: Where does this logic belong now? Remove? */
#if 0
	/*
	 * Close relation. Unless this is a CREATE INDEX
	 * for a partitioned table, and we're processing a partition. In that
	 * case, we want to release the lock on the partition early, so that
	 * you don't run out of space in the lock manager if there are a lot
	 * of partitions. Holding the lock on the parent table should be
	 * enough.
	 */
	if (!rel_needs_long_lock(RelationGetRelid(rel)))
		heap_close(rel, lockmode);
	else
		heap_close(rel, NoLock);
#endif
	
	/* Close relation */
	table_close(rel, NoLock);

	/* Mark statement as successfully transformed */
	stmt->transformed = true;

	return stmt;
}


/*
 * transformRuleStmt -
 *	  transform a CREATE RULE Statement. The action is a list of parse
 *	  trees which is transformed into a list of query trees, and we also
 *	  transform the WHERE clause if any.
 *
 * actions and whereClause are output parameters that receive the
 * transformed results.
 *
 * Note that we must not scribble on the passed-in RuleStmt, so we do
 * copyObject() on the actions and WHERE clause.
 */
void
transformRuleStmt(RuleStmt *stmt, const char *queryString,
				  List **actions, Node **whereClause)
{
	Relation	rel;
	ParseState *pstate;
	RangeTblEntry *oldrte;
	RangeTblEntry *newrte;

	/*
	 * To avoid deadlock, make sure the first thing we do is grab
	 * AccessExclusiveLock on the target relation.  This will be needed by
	 * DefineQueryRewrite(), and we don't want to grab a lesser lock
	 * beforehand.
	 */
	rel = table_openrv(stmt->relation, AccessExclusiveLock);

	if (rel->rd_rel->relkind == RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("rules on materialized views are not supported")));

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * NOTE: 'OLD' must always have a varno equal to 1 and 'NEW' equal to 2.
	 * Set up their RTEs in the main pstate for use in parsing the rule
	 * qualification.
	 */
	oldrte = addRangeTableEntryForRelation(pstate, rel,
										   AccessShareLock,
										   makeAlias("old", NIL),
										   false, false);
	newrte = addRangeTableEntryForRelation(pstate, rel,
										   AccessShareLock,
										   makeAlias("new", NIL),
										   false, false);
	/* Must override addRangeTableEntry's default access-check flags */
	oldrte->requiredPerms = 0;
	newrte->requiredPerms = 0;

	/*
	 * They must be in the namespace too for lookup purposes, but only add the
	 * one(s) that are relevant for the current kind of rule.  In an UPDATE
	 * rule, quals must refer to OLD.field or NEW.field to be unambiguous, but
	 * there's no need to be so picky for INSERT & DELETE.  We do not add them
	 * to the joinlist.
	 */
	switch (stmt->event)
	{
		case CMD_SELECT:
			addRTEtoQuery(pstate, oldrte, false, true, true);
			break;
		case CMD_UPDATE:
			addRTEtoQuery(pstate, oldrte, false, true, true);
			addRTEtoQuery(pstate, newrte, false, true, true);
			break;
		case CMD_INSERT:
			addRTEtoQuery(pstate, newrte, false, true, true);
			break;
		case CMD_DELETE:
			addRTEtoQuery(pstate, oldrte, false, true, true);
			break;
		default:
			elog(ERROR, "unrecognized event type: %d",
				 (int) stmt->event);
			break;
	}

	/* take care of the where clause */
	*whereClause = transformWhereClause(pstate,
										(Node *) copyObject(stmt->whereClause),
										EXPR_KIND_WHERE,
										"WHERE");
	/* we have to fix its collations too */
	assign_expr_collations(pstate, *whereClause);

	/* this is probably dead code without add_missing_from: */
	if (list_length(pstate->p_rtable) != 2) /* naughty, naughty... */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("rule WHERE condition cannot contain references to other relations")));

	/*
	 * 'instead nothing' rules with a qualification need a query rangetable so
	 * the rewrite handler can add the negated rule qualification to the
	 * original query. We create a query with the new command type CMD_NOTHING
	 * here that is treated specially by the rewrite system.
	 */
	if (stmt->actions == NIL)
	{
		Query	   *nothing_qry = makeNode(Query);

		nothing_qry->commandType = CMD_NOTHING;
		nothing_qry->rtable = pstate->p_rtable;
		nothing_qry->jointree = makeFromExpr(NIL, NULL);	/* no join wanted */

		*actions = list_make1(nothing_qry);
	}
	else
	{
		ListCell   *l;
		List	   *newactions = NIL;

		/*
		 * transform each statement, like parse_sub_analyze()
		 */
		foreach(l, stmt->actions)
		{
			Node	   *action = (Node *) lfirst(l);
			ParseState *sub_pstate = make_parsestate(NULL);
			Query	   *sub_qry,
					   *top_subqry;
			bool		has_old,
						has_new;

			/*
			 * Since outer ParseState isn't parent of inner, have to pass down
			 * the query text by hand.
			 */
			sub_pstate->p_sourcetext = queryString;

			/*
			 * Set up OLD/NEW in the rtable for this statement.  The entries
			 * are added only to relnamespace, not varnamespace, because we
			 * don't want them to be referred to by unqualified field names
			 * nor "*" in the rule actions.  We decide later whether to put
			 * them in the joinlist.
			 */
			oldrte = addRangeTableEntryForRelation(sub_pstate, rel,
												   AccessShareLock,
												   makeAlias("old", NIL),
												   false, false);
			newrte = addRangeTableEntryForRelation(sub_pstate, rel,
												   AccessShareLock,
												   makeAlias("new", NIL),
												   false, false);
			oldrte->requiredPerms = 0;
			newrte->requiredPerms = 0;
			addRTEtoQuery(sub_pstate, oldrte, false, true, false);
			addRTEtoQuery(sub_pstate, newrte, false, true, false);

			/* Transform the rule action statement */
			top_subqry = transformStmt(sub_pstate,
									   (Node *) copyObject(action));

			/*
			 * We cannot support utility-statement actions (eg NOTIFY) with
			 * nonempty rule WHERE conditions, because there's no way to make
			 * the utility action execute conditionally.
			 */
			if (top_subqry->commandType == CMD_UTILITY &&
				*whereClause != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("rules with WHERE conditions can only have SELECT, INSERT, UPDATE, or DELETE actions")));

			/*
			 * If the action is INSERT...SELECT, OLD/NEW have been pushed down
			 * into the SELECT, and that's what we need to look at. (Ugly
			 * kluge ... try to fix this when we redesign querytrees.)
			 */
			sub_qry = getInsertSelectQuery(top_subqry, NULL);

			/*
			 * If the sub_qry is a setop, we cannot attach any qualifications
			 * to it, because the planner won't notice them.  This could
			 * perhaps be relaxed someday, but for now, we may as well reject
			 * such a rule immediately.
			 */
			if (sub_qry->setOperations != NULL && *whereClause != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));

			/*
			 * Validate action's use of OLD/NEW, qual too
			 */
			has_old =
				rangeTableEntry_used((Node *) sub_qry, PRS2_OLD_VARNO, 0) ||
				rangeTableEntry_used(*whereClause, PRS2_OLD_VARNO, 0);
			has_new =
				rangeTableEntry_used((Node *) sub_qry, PRS2_NEW_VARNO, 0) ||
				rangeTableEntry_used(*whereClause, PRS2_NEW_VARNO, 0);

			switch (stmt->event)
			{
				case CMD_SELECT:
					if (has_old)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON SELECT rule cannot use OLD")));
					if (has_new)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON SELECT rule cannot use NEW")));
					break;
				case CMD_UPDATE:
					/* both are OK */
					break;
				case CMD_INSERT:
					if (has_old)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON INSERT rule cannot use OLD")));
					break;
				case CMD_DELETE:
					if (has_new)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON DELETE rule cannot use NEW")));
					break;
				default:
					elog(ERROR, "unrecognized event type: %d",
						 (int) stmt->event);
					break;
			}

			/*
			 * OLD/NEW are not allowed in WITH queries, because they would
			 * amount to outer references for the WITH, which we disallow.
			 * However, they were already in the outer rangetable when we
			 * analyzed the query, so we have to check.
			 *
			 * Note that in the INSERT...SELECT case, we need to examine the
			 * CTE lists of both top_subqry and sub_qry.
			 *
			 * Note that we aren't digging into the body of the query looking
			 * for WITHs in nested sub-SELECTs.  A WITH down there can
			 * legitimately refer to OLD/NEW, because it'd be an
			 * indirect-correlated outer reference.
			 */
			if (rangeTableEntry_used((Node *) top_subqry->cteList,
									 PRS2_OLD_VARNO, 0) ||
				rangeTableEntry_used((Node *) sub_qry->cteList,
									 PRS2_OLD_VARNO, 0))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot refer to OLD within WITH query")));
			if (rangeTableEntry_used((Node *) top_subqry->cteList,
									 PRS2_NEW_VARNO, 0) ||
				rangeTableEntry_used((Node *) sub_qry->cteList,
									 PRS2_NEW_VARNO, 0))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot refer to NEW within WITH query")));

			/*
			 * For efficiency's sake, add OLD to the rule action's jointree
			 * only if it was actually referenced in the statement or qual.
			 *
			 * For INSERT, NEW is not really a relation (only a reference to
			 * the to-be-inserted tuple) and should never be added to the
			 * jointree.
			 *
			 * For UPDATE, we treat NEW as being another kind of reference to
			 * OLD, because it represents references to *transformed* tuples
			 * of the existing relation.  It would be wrong to enter NEW
			 * separately in the jointree, since that would cause a double
			 * join of the updated relation.  It's also wrong to fail to make
			 * a jointree entry if only NEW and not OLD is mentioned.
			 */
			if (has_old || (has_new && stmt->event == CMD_UPDATE))
			{
				/*
				 * If sub_qry is a setop, manipulating its jointree will do no
				 * good at all, because the jointree is dummy. (This should be
				 * a can't-happen case because of prior tests.)
				 */
				if (sub_qry->setOperations != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));
				/* hack so we can use addRTEtoQuery() */
				sub_pstate->p_rtable = sub_qry->rtable;
				sub_pstate->p_joinlist = sub_qry->jointree->fromlist;
				addRTEtoQuery(sub_pstate, oldrte, true, false, false);
				sub_qry->jointree->fromlist = sub_pstate->p_joinlist;
			}

			newactions = lappend(newactions, top_subqry);

			free_parsestate(sub_pstate);
		}

		*actions = newactions;
	}

	free_parsestate(pstate);

	/* Close relation, but keep the exclusive lock */
	table_close(rel, NoLock);
}


/*
 * transformAlterTableStmt -
 *		parse analysis for ALTER TABLE
 *
 * Returns a List of utility commands to be done in sequence.  One of these
 * will be the transformed AlterTableStmt, but there may be additional actions
 * to be done before and after the actual AlterTable() call.
 *
 * To avoid race conditions, it's important that this function rely only on
 * the passed-in relid (and not on stmt->relation) to determine the target
 * relation.
 */
List *
transformAlterTableStmt(Oid relid, AlterTableStmt *stmt,
						const char *queryString)
{
	Relation	rel;
	TupleDesc	tupdesc;
	ParseState *pstate;
	CreateStmtContext cxt;
	List	   *result;
	List	   *save_alist;
	ListCell   *lcmd,
			   *l;
	List	   *newcmds = NIL;
	bool		skipValidation = true;
	AlterTableCmd *newcmd;
	RangeTblEntry *rte;

	/*
	 * We must not scribble on the passed-in AlterTableStmt, so copy it. (This
	 * is overkill, but easy.)
	 */
	stmt = copyObject(stmt);

	/* Caller is responsible for locking the relation */
	/* GPDB_94_MERGE_FIXME: this function used to be responsible, and we had some
	 * more complicated logic here for partitions:
	 *
	 * In GPDB, we release the lock early if this command is part of a
	 * partitioned CREATE TABLE.
	 */
	rel = relation_open(relid, NoLock);
	tupdesc = RelationGetDescr(rel);

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	rte = addRangeTableEntryForRelation(pstate,
										rel,
										AccessShareLock,
										NULL,
										false,
										true);
	addRTEtoQuery(pstate, rte, false, true, true);

	/* Set up CreateStmtContext */
	cxt.pstate = pstate;
	if (stmt->relkind == OBJECT_FOREIGN_TABLE)
	{
		cxt.stmtType = "ALTER FOREIGN TABLE";
		cxt.isforeign = true;
	}
	else
	{
		cxt.stmtType = "ALTER TABLE";
		cxt.isforeign = false;
	}
	cxt.relation = stmt->relation;
	cxt.rel = rel;
	cxt.inhRelations = NIL;
	cxt.isalter = true;
	cxt.columns = NIL;
	cxt.ckconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
	cxt.inh_indexes = NIL;
	cxt.extstats = NIL;
	cxt.blist = NIL;
	cxt.alist = NIL;
	cxt.pkey = NULL;
	cxt.ispartitioned = (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
	cxt.partbound = NULL;
	cxt.ofType = false;

	/*
	 * The only subtypes that currently require parse transformation handling
	 * are ADD COLUMN, ADD CONSTRAINT and SET DATA TYPE.  These largely re-use
	 * code from CREATE TABLE.
	 * And ALTER TABLE ... <operator> PARTITION ...
	 */
	foreach(lcmd, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

		switch (cmd->subtype)
		{
			case AT_AddColumn:
			case AT_AddColumnToView:
				{
					ColumnDef  *def = castNode(ColumnDef, cmd->def);

					/*
					 * Adding a column with a primary key or unique constraint
					 * is not supported in GPDB.
					 */
					if (Gp_role == GP_ROLE_DISPATCH)
					{
						ListCell *c;
						foreach(c, def->constraints)
						{
							Constraint *cons = (Constraint *) lfirst(c);
							if (cons->contype == CONSTR_PRIMARY)
								ereport(ERROR,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("cannot add column with primary key constraint")));
							if (cons->contype == CONSTR_UNIQUE)
								ereport(ERROR,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("cannot add column with unique constraint")));
						}
					}
					transformColumnDefinition(&cxt, def);

					/*
					 * If the column has a non-null default, we can't skip
					 * validation of foreign keys.
					 */
					if (def->raw_default != NULL)
						skipValidation = false;

					/*
					 * All constraints are processed in other ways. Remove the
					 * original list
					 */
					def->constraints = NIL;

					newcmds = lappend(newcmds, cmd);
					break;
				}

			case AT_AddConstraint:

				/*
				 * The original AddConstraint cmd node doesn't go to newcmds
				 */
				if (IsA(cmd->def, Constraint))
				{
					transformTableConstraint(&cxt, (Constraint *) cmd->def);
					if (((Constraint *) cmd->def)->contype == CONSTR_FOREIGN)
					{
						/* GPDB: always skip validation of foreign keys */
						skipValidation = true;
					}
				}
				else
					elog(ERROR, "unrecognized node type: %d",
						 (int) nodeTag(cmd->def));
				break;

			case AT_ProcessedConstraint:

				/*
				 * Already-transformed ADD CONSTRAINT, so just make it look
				 * like the standard case.
				 */
				cmd->subtype = AT_AddConstraint;
				newcmds = lappend(newcmds, cmd);
				break;

				/* CDB: Partitioned Tables */
            case AT_PartDrop:			/* Drop */
				if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("table \"%s\" is not partitioned",
									RelationGetRelationName(rel))));
				}

				newcmds = lappend(newcmds, cmd);
				break;

            case AT_PartAdd:			/* Add */
				{
					if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("table \"%s\" is not partitioned",
										RelationGetRelationName(rel))));
					}

					GpAlterPartitionCmd *add_cmd = castNode(GpAlterPartitionCmd, cmd->def);
					GpPartitionElem *pelem = castNode(GpPartitionElem, add_cmd->arg);
					GpPartitionSpec *gpPartSpec = makeNode(GpPartitionSpec);

					gpPartSpec->partElem = list_make1(pelem);
					List *cstmts = generatePartitions(RelationGetRelid(rel), gpPartSpec, NULL, queryString);
					foreach(l, cstmts)
					{
						Node *stmt = (Node *) lfirst(l);
						cxt.blist = lappend(cxt.blist, stmt);
					}
				}
				break;

			case AT_PartTruncate:
				{
					GpAlterPartitionCmd *pc = castNode(GpAlterPartitionCmd, cmd->def);
					GpAlterPartitionId *pid = (GpAlterPartitionId *) pc->partid;
					TruncateStmt *truncstmt = (TruncateStmt *) pc->arg;
					Oid partrelid;
					RangeVar *rv;
					Relation partrel;

					if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("table \"%s\" is not partitioned",
										RelationGetRelationName(rel))));
					}

					partrelid = GpFindTargetPartition(rel, pid, false);
					Assert(OidIsValid(partrelid));
					partrel = table_open(partrelid, AccessShareLock);
					rv = makeRangeVar(get_namespace_name(RelationGetNamespace(partrel)),
									  pstrdup(RelationGetRelationName(partrel)),
									  pc->location);
					truncstmt->relations = list_make1(rv);
					table_close(partrel, AccessShareLock);

					cxt.blist = lappend(cxt.blist, truncstmt);
				}
				break;

			case AT_PartAlter:          /* Alter */
            case AT_PartExchange:		/* Exchange */
            case AT_PartRename:			/* Rename */
            case AT_PartSetTemplate:	/* Set Subpartition Template */
            case AT_PartSplit:			/* Split */
				/* GDPB_12_MERGE_FIXME: need to re-implement this */
				elog(ERROR, "not implemented");
#if 0
				cmd = transformAlterTable_all_PartitionStmt(
					pstate,
					stmt,
					&cxt,
					cmd);
#endif
				newcmds = lappend(newcmds, cmd);
				break;

			case AT_PartAddInternal:	/* Add partition, as part of CREATE TABLE */
				/* GDPB_12_MERGE_FIXME: need to re-implement this */
				elog(ERROR, "not implemented");
#if 0
				cxt.iscreatepart = true;
#endif
				newcmds = lappend(newcmds, cmd);
				break;

			case AT_AlterColumnType:
				{
					ColumnDef  *def = (ColumnDef *) cmd->def;
					AttrNumber	attnum;

					/*
					 * For ALTER COLUMN TYPE, transform the USING clause if
					 * one was specified.
					 */
					if (def->raw_default)
					{
						def->cooked_default =
							transformExpr(pstate, def->raw_default,
										  EXPR_KIND_ALTER_COL_TRANSFORM);
					}

					/*
					 * For identity column, create ALTER SEQUENCE command to
					 * change the data type of the sequence.
					 */
					attnum = get_attnum(relid, cmd->name);

					/*
					 * if attribute not found, something will error about it
					 * later
					 */
					if (attnum != InvalidAttrNumber &&
						TupleDescAttr(tupdesc, attnum - 1)->attidentity)
					{
						Oid			seq_relid = getOwnedSequence(relid, attnum);
						Oid			typeOid = typenameTypeId(pstate, def->typeName);
						AlterSeqStmt *altseqstmt = makeNode(AlterSeqStmt);

						altseqstmt->sequence = makeRangeVar(get_namespace_name(get_rel_namespace(seq_relid)),
															get_rel_name(seq_relid),
															-1);
						altseqstmt->options = list_make1(makeDefElem("as", (Node *) makeTypeNameFromOid(typeOid, -1), -1));
						altseqstmt->for_identity = true;
						cxt.blist = lappend(cxt.blist, altseqstmt);
					}

					newcmds = lappend(newcmds, cmd);
					break;
				}

			case AT_AddIdentity:
				{
					Constraint *def = castNode(Constraint, cmd->def);
					ColumnDef  *newdef = makeNode(ColumnDef);
					AttrNumber	attnum;

					newdef->colname = cmd->name;
					newdef->identity = def->generated_when;
					cmd->def = (Node *) newdef;

					attnum = get_attnum(relid, cmd->name);

					/*
					 * if attribute not found, something will error about it
					 * later
					 */
					if (attnum != InvalidAttrNumber)
						generateSerialExtraStmts(&cxt, newdef,
												 get_atttype(relid, attnum),
												 def->options, true,
												 NULL, NULL);

					newcmds = lappend(newcmds, cmd);
					break;
				}

			case AT_SetIdentity:
				{
					/*
					 * Create an ALTER SEQUENCE statement for the internal
					 * sequence of the identity column.
					 */
					ListCell   *lc;
					List	   *newseqopts = NIL;
					List	   *newdef = NIL;
					List	   *seqlist;
					AttrNumber	attnum;

					/*
					 * Split options into those handled by ALTER SEQUENCE and
					 * those for ALTER TABLE proper.
					 */
					foreach(lc, castNode(List, cmd->def))
					{
						DefElem    *def = lfirst_node(DefElem, lc);

						if (strcmp(def->defname, "generated") == 0)
							newdef = lappend(newdef, def);
						else
							newseqopts = lappend(newseqopts, def);
					}

					attnum = get_attnum(relid, cmd->name);

					if (attnum)
					{
						seqlist = getOwnedSequences(relid, attnum);
						if (seqlist)
						{
							AlterSeqStmt *seqstmt;
							Oid			seq_relid;

							seqstmt = makeNode(AlterSeqStmt);
							seq_relid = linitial_oid(seqlist);
							seqstmt->sequence = makeRangeVar(get_namespace_name(get_rel_namespace(seq_relid)),
															 get_rel_name(seq_relid), -1);
							seqstmt->options = newseqopts;
							seqstmt->for_identity = true;
							seqstmt->missing_ok = false;

							cxt.alist = lappend(cxt.alist, seqstmt);
						}
					}

					/*
					 * If column was not found or was not an identity column,
					 * we just let the ALTER TABLE command error out later.
					 */

					cmd->def = (Node *) newdef;
					newcmds = lappend(newcmds, cmd);
					break;
				}

			case AT_AttachPartition:
			case AT_DetachPartition:
				{
					PartitionCmd *partcmd = (PartitionCmd *) cmd->def;

					transformPartitionCmd(&cxt, partcmd);
					/* assign transformed value of the partition bound */
					partcmd->bound = cxt.partbound;
				}

				newcmds = lappend(newcmds, cmd);
				break;

			default:
				newcmds = lappend(newcmds, cmd);
				break;
		}
	}

	/*
	 * Transfer anything we already have in cxt.alist into save_alist, to keep
	 * it separate from the output of transformIndexConstraints.
	 */
	save_alist = cxt.alist;
	cxt.alist = NIL;

	/* Postprocess constraints */
	transformIndexConstraints(&cxt);
	transformFKConstraints(&cxt, skipValidation, true);
	transformCheckConstraints(&cxt, false);

	/*
	 * Push any index-creation commands into the ALTER, so that they can be
	 * scheduled nicely by tablecmds.c.  Note that tablecmds.c assumes that
	 * the IndexStmt attached to an AT_AddIndex or AT_AddIndexConstraint
	 * subcommand has already been through transformIndexStmt.
	 */
	foreach(l, cxt.alist)
	{
		Node	   *istmt = (Node *) lfirst(l);

		/*
		 * We assume here that cxt.alist contains only IndexStmts and possibly
		 * ALTER TABLE SET NOT NULL statements generated from primary key
		 * constraints.  We absorb the subcommands of the latter directly.
		 */
		if (IsA(istmt, IndexStmt))
		{
			IndexStmt  *idxstmt = (IndexStmt *) istmt;

			idxstmt = transformIndexStmt(relid, idxstmt, queryString);
			newcmd = makeNode(AlterTableCmd);
			newcmd->subtype = OidIsValid(idxstmt->indexOid) ? AT_AddIndexConstraint : AT_AddIndex;
			newcmd->def = (Node *) idxstmt;
			newcmds = lappend(newcmds, newcmd);
		}
		else if (IsA(istmt, AlterTableStmt))
		{
			AlterTableStmt *alterstmt = (AlterTableStmt *) istmt;

			newcmds = list_concat(newcmds, alterstmt->cmds);
		}
		else
			elog(ERROR, "unexpected stmt type %d", (int) nodeTag(istmt));
	}
	cxt.alist = NIL;

	/* Append any CHECK or FK constraints to the commands list */
	foreach(l, cxt.ckconstraints)
	{
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddConstraint;
		newcmd->def = (Node *) lfirst(l);
		newcmds = lappend(newcmds, newcmd);
	}
	foreach(l, cxt.fkconstraints)
	{
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddConstraint;
		newcmd->def = (Node *) lfirst(l);
		newcmds = lappend(newcmds, newcmd);
	}

	/* Append extended statistic objects */
	transformExtendedStatistics(&cxt);

#if 0
	/* GPDB_12_MERGE_FIXME: Where does this logic go now?
	 *
	 * If this is part of a CREATE TABLE of a partitioned table, creating
	 * the partitions, we release the lock immediately, however. We hold
	 * a lock on the parent table, and no-one can see the partitions yet,
	 * so the lock on each partition isn't strictly required. Creating a
	 * massively partitioned table could otherwise require holding a lot
	 * of locks, running out of shared memory in the lock manager.
	 */
	if (cxt.iscreatepart)
		relation_close(rel, AccessExclusiveLock);
	else
		relation_close(rel, NoLock);
#endif

	/* Close rel */
	relation_close(rel, NoLock);

	/*
	 * Output results.
	 */
	stmt->cmds = newcmds;

	result = lappend(cxt.blist, stmt);
	result = list_concat(result, cxt.alist);
	result = list_concat(result, save_alist);

	return result;
}


/*
 * Preprocess a list of column constraint clauses
 * to attach constraint attributes to their primary constraint nodes
 * and detect inconsistent/misplaced constraint attributes.
 *
 * NOTE: currently, attributes are only supported for FOREIGN KEY, UNIQUE,
 * EXCLUSION, and PRIMARY KEY constraints, but someday they ought to be
 * supported for other constraint types.
 */
static void
transformConstraintAttrs(CreateStmtContext *cxt, List *constraintList)
{
	Constraint *lastprimarycon = NULL;
	bool		saw_deferrability = false;
	bool		saw_initially = false;
	ListCell   *clist;

#define SUPPORTS_ATTRS(node)				\
	((node) != NULL &&						\
	 ((node)->contype == CONSTR_PRIMARY ||	\
	  (node)->contype == CONSTR_UNIQUE ||	\
	  (node)->contype == CONSTR_EXCLUSION || \
	  (node)->contype == CONSTR_FOREIGN))

	foreach(clist, constraintList)
	{
		Constraint *con = (Constraint *) lfirst(clist);

		if (!IsA(con, Constraint))
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(con));
		switch (con->contype)
		{
			case CONSTR_ATTR_DEFERRABLE:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced DEFERRABLE clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_deferrability)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple DEFERRABLE/NOT DEFERRABLE clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_deferrability = true;
				lastprimarycon->deferrable = true;
				break;

			case CONSTR_ATTR_NOT_DEFERRABLE:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced NOT DEFERRABLE clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_deferrability)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple DEFERRABLE/NOT DEFERRABLE clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_deferrability = true;
				lastprimarycon->deferrable = false;
				if (saw_initially &&
					lastprimarycon->initdeferred)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("constraint declared INITIALLY DEFERRED must be DEFERRABLE"),
							 parser_errposition(cxt->pstate, con->location)));
				break;

			case CONSTR_ATTR_DEFERRED:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced INITIALLY DEFERRED clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_initially)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple INITIALLY IMMEDIATE/DEFERRED clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_initially = true;
				lastprimarycon->initdeferred = true;

				/*
				 * If only INITIALLY DEFERRED appears, assume DEFERRABLE
				 */
				if (!saw_deferrability)
					lastprimarycon->deferrable = true;
				else if (!lastprimarycon->deferrable)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("constraint declared INITIALLY DEFERRED must be DEFERRABLE"),
							 parser_errposition(cxt->pstate, con->location)));
				break;

			case CONSTR_ATTR_IMMEDIATE:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced INITIALLY IMMEDIATE clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_initially)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple INITIALLY IMMEDIATE/DEFERRED clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_initially = true;
				lastprimarycon->initdeferred = false;
				break;

			default:
				/* Otherwise it's not an attribute */
				lastprimarycon = con;
				/* reset flags for new primary node */
				saw_deferrability = false;
				saw_initially = false;
				break;
		}
	}
}

/*
 * Special handling of type definition for a column
 */
static void
transformColumnType(CreateStmtContext *cxt, ColumnDef *column)
{
	/*
	 * All we really need to do here is verify that the type is valid,
	 * including any collation spec that might be present.
	 */
	Type		ctype = typenameType(cxt->pstate, column->typeName, NULL);

	if (column->collClause)
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(ctype);

		LookupCollation(cxt->pstate,
						column->collClause->collname,
						column->collClause->location);
		/* Complain if COLLATE is applied to an uncollatable type */
		if (!OidIsValid(typtup->typcollation))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("collations are not supported by type %s",
							format_type_be(typtup->oid)),
					 parser_errposition(cxt->pstate,
										column->collClause->location)));
	}

	ReleaseSysCache(ctype);
}


/*
 * transformCreateSchemaStmt -
 *	  analyzes the CREATE SCHEMA statement
 *
 * Split the schema element list into individual commands and place
 * them in the result list in an order such that there are no forward
 * references (e.g. GRANT to a table created later in the list). Note
 * that the logic we use for determining forward references is
 * presently quite incomplete.
 *
 * SQL also allows constraints to make forward references, so thumb through
 * the table columns and move forward references to a posterior alter-table
 * command.
 *
 * The result is a list of parse nodes that still need to be analyzed ---
 * but we can't analyze the later commands until we've executed the earlier
 * ones, because of possible inter-object references.
 *
 * Note: this breaks the rules a little bit by modifying schema-name fields
 * within passed-in structs.  However, the transformation would be the same
 * if done over, so it should be all right to scribble on the input to this
 * extent.
 */
List *
transformCreateSchemaStmt(CreateSchemaStmt *stmt)
{
	CreateSchemaStmtContext cxt;
	List	   *result;
	ListCell   *elements;

	cxt.stmtType = "CREATE SCHEMA";
	cxt.schemaname = stmt->schemaname;
	cxt.authrole = (RoleSpec *) stmt->authrole;
	cxt.sequences = NIL;
	cxt.tables = NIL;
	cxt.views = NIL;
	cxt.indexes = NIL;
	cxt.triggers = NIL;
	cxt.grants = NIL;

	/*
	 * Run through each schema element in the schema element list. Separate
	 * statements by type, and do preliminary analysis.
	 */
	foreach(elements, stmt->schemaElts)
	{
		Node	   *element = lfirst(elements);

		switch (nodeTag(element))
		{
			case T_CreateSeqStmt:
				{
					CreateSeqStmt *elp = (CreateSeqStmt *) element;

					setSchemaName(cxt.schemaname, &elp->sequence->schemaname);
					cxt.sequences = lappend(cxt.sequences, element);
				}
				break;

			case T_CreateStmt:
				{
					CreateStmt *elp = (CreateStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);

					/*
					 * XXX todo: deal with constraints
					 */
					cxt.tables = lappend(cxt.tables, element);
				}
				break;

			case T_ViewStmt:
				{
					ViewStmt   *elp = (ViewStmt *) element;

					setSchemaName(cxt.schemaname, &elp->view->schemaname);

					/*
					 * XXX todo: deal with references between views
					 */
					cxt.views = lappend(cxt.views, element);
				}
				break;

			case T_IndexStmt:
				{
					IndexStmt  *elp = (IndexStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);
					cxt.indexes = lappend(cxt.indexes, element);
				}
				break;

			case T_CreateTrigStmt:
				{
					CreateTrigStmt *elp = (CreateTrigStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);
					cxt.triggers = lappend(cxt.triggers, element);
				}
				break;

			case T_GrantStmt:
				cxt.grants = lappend(cxt.grants, element);
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(element));
		}
	}

	result = NIL;
	result = list_concat(result, cxt.sequences);
	result = list_concat(result, cxt.tables);
	result = list_concat(result, cxt.views);
	result = list_concat(result, cxt.indexes);
	result = list_concat(result, cxt.triggers);
	result = list_concat(result, cxt.grants);

	return result;
}

/*
 * setSchemaName
 *		Set or check schema name in an element of a CREATE SCHEMA command
 */
static void
setSchemaName(char *context_schema, char **stmt_schema_name)
{
	if (*stmt_schema_name == NULL)
		*stmt_schema_name = context_schema;
	else if (strcmp(context_schema, *stmt_schema_name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_SCHEMA_DEFINITION),
				 errmsg("CREATE specifies a schema (%s) "
						"different from the one being created (%s)",
						*stmt_schema_name, context_schema)));
}

/*
 * getLikeDistributionPolicy
 *
 * For Greenplum Database distributed tables, default to
 * the same distribution as the first LIKE table, unless
 * we also have INHERITS
 */
static DistributedBy *
getLikeDistributionPolicy(TableLikeClause *e)
{
	DistributedBy *likeDistributedBy = NULL;
	Relation	rel;

	rel = relation_openrv(e->relation, AccessShareLock);

	if (rel->rd_cdbpolicy != NULL && rel->rd_cdbpolicy->ptype != POLICYTYPE_ENTRY)
	{
		likeDistributedBy = make_distributedby_for_rel(rel);
	}

	relation_close(rel, AccessShareLock);

	return likeDistributedBy;
}

/*
 * Transform and validate the actual encoding clauses.
 *
 * We need tell the underlying system that these are AO/CO tables too,
 * hence the concatenation of the extra elements.
 */
List *
transformStorageEncodingClause(List *options)
{
	Datum d;
	ListCell *lc;
	DefElem *dl;
	static char *validnsps[] = HEAP_RELOPT_NAMESPACES;
	foreach(lc, options)
	{
		dl = (DefElem *) lfirst(lc);
		if (pg_strncasecmp(dl->defname, SOPT_CHECKSUM, strlen(SOPT_CHECKSUM)) == 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"%s\" is not a column specific option",
							SOPT_CHECKSUM)));
		}
	}

	/* add defaults for missing values */
	options = fillin_encoding(options);

	return options;
}

/*
 * Validate the sanity of column reference storage clauses.
 *
 * 1. Ensure that we only refer to columns that exist.
 * 2. Ensure that each column is referenced either zero times or once.
 */
static void
validateColumnStorageEncodingClauses(List *stenc, CreateStmt *stmt)
{
	ListCell *lc;
	struct HTAB *ht = NULL;
	struct colent {
		char colname[NAMEDATALEN];
		int count;
	} *ce = NULL;

	if (!stenc)
		return;

	/* Generate a hash table for all the columns */
	foreach(lc, stmt->tableElts)
	{
		Node *n = lfirst(lc);

		if (IsA(n, ColumnDef))
		{
			ColumnDef *c = (ColumnDef *)n;
			char *colname;
			bool found = false;
			size_t n = NAMEDATALEN - 1 < strlen(c->colname) ?
							NAMEDATALEN - 1 : strlen(c->colname);

			colname = palloc0(NAMEDATALEN);
			MemSet(colname, 0, NAMEDATALEN);
			memcpy(colname, c->colname, n);
			colname[n] = '\0';

			if (!ht)
			{
				HASHCTL  cacheInfo;
				int      cacheFlags;

				memset(&cacheInfo, 0, sizeof(cacheInfo));
				cacheInfo.keysize = NAMEDATALEN;
				cacheInfo.entrysize = sizeof(*ce);
				cacheFlags = HASH_ELEM;

				ht = hash_create("column info cache",
								 list_length(stmt->tableElts),
								 &cacheInfo, cacheFlags);
			}

			ce = hash_search(ht, colname, HASH_ENTER, &found);

			/*
			 * The user specified a duplicate column name. We check duplicate
			 * column names VERY late (under MergeAttributes(), which is called
			 * by DefineRelation(). For the specific case here, it is safe to
			 * call out that this is a duplicate. We don't need to delay until
			 * we look at inheritance.
			 */
			if (found)
			{
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column \"%s\" duplicated",
								colname)));
				
			}
			ce->count = 0;
		}
	}

	/*
	 * If the table has no columns -- usually in the partitioning case -- then
	 * we can short circuit.
	 */
	if (!ht)
		return;

	/*
	 * All column reference storage directives without the DEFAULT
	 * clause should refer to real columns.
	 */
	foreach(lc, stenc)
	{
		ColumnReferenceStorageDirective *c = lfirst(lc);

		Insist(IsA(c, ColumnReferenceStorageDirective));

		if (c->deflt)
			continue;
		else
		{
			bool found = false;
			char colname[NAMEDATALEN];
			size_t collen = strlen(c->column);
			size_t n = NAMEDATALEN - 1 < collen ? NAMEDATALEN - 1 : collen;
			MemSet(colname, 0, NAMEDATALEN);
			memcpy(colname, c->column, n);
			colname[n] = '\0';

			ce = hash_search(ht, colname, HASH_FIND, &found);

			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("column \"%s\" does not exist", colname)));

			ce->count++;

			if (ce->count > 1)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("column \"%s\" referenced in more than one COLUMN ENCODING clause",
								colname)));
		}
	}

	hash_destroy(ht);
}

/*
 * Find the column reference storage encoding clause for `column'.
 *
 * This is called by transformAttributeEncoding() in a loop but stenc should be
 * quite small in practice.
 */
static ColumnReferenceStorageDirective *
find_crsd(char *column, List *stenc)
{
	ListCell *lc;

	foreach(lc, stenc)
	{
		ColumnReferenceStorageDirective *c = lfirst(lc);

		if (c->deflt == false && strcmp(column, c->column) == 0)
			return c;
	}
	return NULL;
}


List *
TypeNameGetStorageDirective(TypeName *typname)
{
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc sscan;
	HeapTuple	tuple;
	Oid			typid;
	List	   *out = NIL;

	typid = typenameTypeId(NULL, typname);

	rel = heap_open(TypeEncodingRelationId, AccessShareLock);

	/* SELECT typoptions FROM pg_type_encoding where typid = :1 */
	ScanKeyInit(&scankey,
				Anum_pg_type_encoding_typid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typid));
	sscan = systable_beginscan(rel, TypeEncodingTypidIndexId,
							   true, NULL, 1, &scankey);
	tuple = systable_getnext(sscan);
	if (HeapTupleIsValid(tuple))
	{
		Datum options;
		bool isnull;

		options = heap_getattr(tuple,
							   Anum_pg_type_encoding_typoptions,
							   RelationGetDescr(rel),
							   &isnull);

		Insist(!isnull);

		out = untransformRelOptions(options);
	}

	systable_endscan(sscan);
	heap_close(rel, AccessShareLock);

	return out;
}

/*
 * Make a default column storage directive from a WITH clause
 * Ignore options in the WITH clause that don't appear in 
 * storage_directives for column-level compression.
 */
List *
form_default_storage_directive(List *enc)
{
	List *out = NIL;
	ListCell *lc;

	foreach(lc, enc)
	{
		DefElem *el = lfirst(lc);

		if (!el->defname)
			out = lappend(out, copyObject(el));

		if (pg_strcasecmp("appendonly", el->defname) == 0)
			continue;
		if (pg_strcasecmp("orientation", el->defname) == 0)
			continue;
		if (pg_strcasecmp("oids", el->defname) == 0)
			continue;
		if (pg_strcasecmp("fillfactor", el->defname) == 0)
			continue;
		if (pg_strcasecmp("tablename", el->defname) == 0)
			continue;
		/* checksum is not a column specific attribute. */
		if (pg_strcasecmp("checksum", el->defname) == 0)
			continue;
		out = lappend(out, copyObject(el));
	}
	return out;
}

static List *
transformAttributeEncoding(List *stenc, CreateStmt *stmt, CreateStmtContext *cxt)
{
	ListCell *lc;
	bool found_enc = stenc != NIL;
	bool can_enc = is_aocs(stmt->accessMethod);
	ColumnReferenceStorageDirective *deflt = NULL;
	List *newenc = NIL;
	List *tmpenc;
	MemoryContext oldCtx;

	/* We only support the attribute encoding clause on AOCS tables */
	if (stenc && !can_enc)
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("ENCODING clause only supported with column oriented tables")));

	/* Use the temporary context to avoid leaving behind so much garbage. */
	oldCtx = MemoryContextSwitchTo(cxt->tempCtx);

	/* get the default clause, if there is one. */
	foreach(lc, stenc)
	{
		ColumnReferenceStorageDirective *c = lfirst(lc);
		Insist(IsA(c, ColumnReferenceStorageDirective));

		if (c->deflt)
		{
			/*
			 * Some quick validation: there should only be one default
			 * clause
			 */
			if (deflt)
				elog(ERROR, "only one default column encoding may be specified");

			deflt = copyObject(c);
			deflt->encoding = transformStorageEncodingClause(deflt->encoding);

			/*
			 * The default encoding and the with clause better not
			 * try and set the same options!
			 */
			if (encodings_overlap(stmt->options, deflt->encoding, false))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("DEFAULT COLUMN ENCODING clause cannot override values set in WITH clause")));
		}
	}

	/*
	 * If no default has been specified, we might create one out of the
	 * WITH clause.
	 */
	tmpenc = form_default_storage_directive(stmt->options);

	if (tmpenc)
	{
		deflt = makeNode(ColumnReferenceStorageDirective);
		deflt->deflt = true;
		deflt->encoding = transformStorageEncodingClause(tmpenc);
	}

	/*
	 * Loop over all columns. If a column has a column reference storage clause
	 * -- i.e., COLUMN name ENCODING () -- apply that. Otherwise, apply the
	 * default.
	 */
	foreach(lc, cxt->columns)
	{
		ColumnDef *d = (ColumnDef *) lfirst(lc);
		ColumnReferenceStorageDirective *c;

		Insist(IsA(d, ColumnDef));

		c = makeNode(ColumnReferenceStorageDirective);
		c->column = pstrdup(d->colname);

		/*
		 * Find a storage encoding for this column, in this order:
		 *
		 * 1. An explicit encoding clause in the ColumnDef
		 * 2. A column reference storage directive for this column
		 * 3. A default column encoding in the statement
		 * 4. A default for the type.
		 */
		if (d->encoding)
		{
			found_enc = true;
			c->encoding = transformStorageEncodingClause(d->encoding);
		}
		else
		{
			ColumnReferenceStorageDirective *s = find_crsd(c->column, stenc);

			if (s)
				c->encoding = transformStorageEncodingClause(s->encoding);
			else
			{
				if (deflt)
					c->encoding = copyObject(deflt->encoding);
				else
				{
					List	   *te;

					if (d->typeName)
						te = TypeNameGetStorageDirective(d->typeName);
					else
						te = NIL;

					if (te)
						c->encoding = copyObject(te);
					else
						c->encoding = default_column_encoding_clause();
				}
			}
		}
		newenc = lappend(newenc, c);
	}

	/* Check again in case we expanded a some column encoding clauses */
	if (!can_enc)
	{
		if (found_enc)
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ENCODING clause only supported with column oriented tables")));
		else
			newenc = NULL;
	}

	validateColumnStorageEncodingClauses(newenc, stmt);

	/* copy the result out of the temporary memory context */
	MemoryContextSwitchTo(oldCtx);
	newenc = copyObject(newenc);

	return newenc;
}

#ifdef GPDB_12_MERGE_FIXME
/*
 * GPDB_12_MERGE_FIXME: appendonly and orientation are no loger passed as
 * arguments. This code was used in child partition cases and the caller is
 * currently disabled. Address this issue when the caller gets reenabled
 */

/*
 * Tells the caller if CO is explicitly disabled, to handle cases where we
 * want to ignore encoding clauses in partition expansion.
 *
 * This is an ugly special case that backup expects to work and since we've got
 * tonnes of dumps out there and the possibility that users have learned this
 * grammar from them, we must continue to support it.
 */
static bool
co_explicitly_disabled(List *opts)
{
	ListCell *lc;

	foreach(lc, opts)
	{
		DefElem *el = lfirst(lc);
		char *arg = NULL;

		/* Argument will be a Value */
		if (!el->arg)
		{
			continue;
		}

		arg = defGetString(el);
		bool result = false;
		if (pg_strcasecmp("appendonly", el->defname) == 0 &&
			pg_strcasecmp("false", arg) == 0)
		{
			result = true;
		}
		else if (pg_strcasecmp("orientation", el->defname) == 0 &&
				 pg_strcasecmp("column", arg) != 0)
		{
			result = true;
		}

		if (result)
		{
			return true;
		}
	}
	return false;
}
#endif

/*
 * Greenplum: We used to specify appendonly and columnar options prior to the
 * introduction of the table access methods. There still exists code that needs
 * to fail early before we have parsed and processed the access method.
 * In all other cases use the provided RelationIsAOColumn() macros.
 */
static bool
is_aocs(const char *accessMethod)
{
	Oid accessMethodId;

	if (accessMethod == NULL)
		accessMethodId = get_table_am_oid(default_table_access_method, true);
	else
		accessMethodId = get_table_am_oid(accessMethod, true);
	return (accessMethodId == AOCO_TABLE_AM_OID);
}

/*
 * See if two encodings attempt to see the same parameters. If test_conflicts is
 * true, allow setting the same value, but the setting must be identical.
 */
static bool
encodings_overlap(List *a, List *b, bool test_conflicts)
{
	ListCell *lca;

	foreach(lca, a)
	{
		ListCell *lcb;
		DefElem *ela = lfirst(lca);

		foreach(lcb, b)
		{
			DefElem *elb = lfirst(lcb);

			if (pg_strcasecmp(ela->defname, elb->defname) == 0)
			{
				if (test_conflicts)
				{
					if (!ela->arg && !elb->arg)
						return true;
					else if (!ela->arg || !elb->arg)
					{
						/* skip */
					}
					else
					{
						char *ela_str = defGetString(ela);
						char *elb_str = defGetString(elb);

						if (pg_strcasecmp(ela_str,elb_str) != 0)
							return true;
					}
				}
				else
					return true;
			}
		}
	}
	return false;
}

/*
 * transformPartitionCmd
 *		Analyze the ATTACH/DETACH PARTITION command
 *
 * In case of the ATTACH PARTITION command, cxt->partbound is set to the
 * transformed value of cmd->bound.
 */
static void
transformPartitionCmd(CreateStmtContext *cxt, PartitionCmd *cmd)
{
	Relation	parentRel = cxt->rel;

	switch (parentRel->rd_rel->relkind)
	{
		case RELKIND_PARTITIONED_TABLE:
			/* transform the partition bound, if any */
			Assert(RelationGetPartitionKey(parentRel) != NULL);
			if (cmd->bound != NULL)
				cxt->partbound = transformPartitionBound(cxt->pstate, parentRel,
														 cmd->bound);
			break;
		case RELKIND_PARTITIONED_INDEX:
			/* nothing to check */
			Assert(cmd->bound == NULL);
			break;
		case RELKIND_RELATION:
			/* the table must be partitioned */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("table \"%s\" is not partitioned",
							RelationGetRelationName(parentRel))));
			break;
		case RELKIND_INDEX:
			/* the index must be partitioned */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("index \"%s\" is not partitioned",
							RelationGetRelationName(parentRel))));
			break;
		default:
			/* parser shouldn't let this case through */
			elog(ERROR, "\"%s\" is not a partitioned table or index",
				 RelationGetRelationName(parentRel));
			break;
	}
}

/*
 * transformPartitionBound
 *
 * Transform a partition bound specification
 */
PartitionBoundSpec *
transformPartitionBound(ParseState *pstate, Relation parent,
						PartitionBoundSpec *spec)
{
	PartitionBoundSpec *result_spec;
	PartitionKey key = RelationGetPartitionKey(parent);
	char		strategy = get_partition_strategy(key);
	int			partnatts = get_partition_natts(key);
	List	   *partexprs = get_partition_exprs(key);

	/* Avoid scribbling on input */
	result_spec = copyObject(spec);

	if (spec->is_default)
	{
		if (strategy == PARTITION_STRATEGY_HASH)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("a hash-partitioned table may not have a default partition")));

		/*
		 * In case of the default partition, parser had no way to identify the
		 * partition strategy. Assign the parent's strategy to the default
		 * partition bound spec.
		 */
		result_spec->strategy = strategy;

		return result_spec;
	}

	if (strategy == PARTITION_STRATEGY_HASH)
	{
		if (spec->strategy != PARTITION_STRATEGY_HASH)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid bound specification for a hash partition"),
					 parser_errposition(pstate, exprLocation((Node *) spec))));

		if (spec->modulus <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("modulus for hash partition must be a positive integer")));

		Assert(spec->remainder >= 0);

		if (spec->remainder >= spec->modulus)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("remainder for hash partition must be less than modulus")));
	}
	else if (strategy == PARTITION_STRATEGY_LIST)
	{
		ListCell   *cell;
		char	   *colname;
		Oid			coltype;
		int32		coltypmod;
		Oid			partcollation;

		if (spec->strategy != PARTITION_STRATEGY_LIST)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid bound specification for a list partition"),
					 parser_errposition(pstate, exprLocation((Node *) spec))));

		/* Get the only column's name in case we need to output an error */
		if (key->partattrs[0] != 0)
			colname = get_attname(RelationGetRelid(parent),
								  key->partattrs[0], false);
		else
			colname = deparse_expression((Node *) linitial(partexprs),
										 deparse_context_for(RelationGetRelationName(parent),
															 RelationGetRelid(parent)),
										 false, false);
		/* Need its type data too */
		coltype = get_partition_col_typid(key, 0);
		coltypmod = get_partition_col_typmod(key, 0);
		partcollation = get_partition_col_collation(key, 0);

		result_spec->listdatums = NIL;
		foreach(cell, spec->listdatums)
		{
			Node	   *expr = lfirst(cell);
			Const	   *value;
			ListCell   *cell2;
			bool		duplicate;

			value = transformPartitionBoundValue(pstate, expr,
												 colname, coltype, coltypmod,
												 partcollation);

			/* Don't add to the result if the value is a duplicate */
			duplicate = false;
			foreach(cell2, result_spec->listdatums)
			{
				Const	   *value2 = castNode(Const, lfirst(cell2));

				if (equal(value, value2))
				{
					duplicate = true;
					break;
				}
			}
			if (duplicate)
				continue;

			result_spec->listdatums = lappend(result_spec->listdatums,
											  value);
		}
	}
	else if (strategy == PARTITION_STRATEGY_RANGE)
	{
		if (spec->strategy != PARTITION_STRATEGY_RANGE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid bound specification for a range partition"),
					 parser_errposition(pstate, exprLocation((Node *) spec))));

		if (list_length(spec->lowerdatums) != partnatts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("FROM must specify exactly one value per partitioning column")));
		if (list_length(spec->upperdatums) != partnatts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("TO must specify exactly one value per partitioning column")));

		/*
		 * Convert raw parse nodes into PartitionRangeDatum nodes and perform
		 * any necessary validation.
		 */
		result_spec->lowerdatums =
			transformPartitionRangeBounds(pstate, spec->lowerdatums,
										  parent);
		result_spec->upperdatums =
			transformPartitionRangeBounds(pstate, spec->upperdatums,
										  parent);
	}
	else
		elog(ERROR, "unexpected partition strategy: %d", (int) strategy);

	return result_spec;
}

/*
 * transformPartitionRangeBounds
 *		This converts the expressions for range partition bounds from the raw
 *		grammar representation to PartitionRangeDatum structs
 */
static List *
transformPartitionRangeBounds(ParseState *pstate, List *blist,
							  Relation parent)
{
	List	   *result = NIL;
	PartitionKey key = RelationGetPartitionKey(parent);
	List	   *partexprs = get_partition_exprs(key);
	ListCell   *lc;
	int			i,
				j;

	i = j = 0;
	foreach(lc, blist)
	{
		Node	   *expr = lfirst(lc);
		PartitionRangeDatum *prd = NULL;

		/*
		 * Infinite range bounds -- "minvalue" and "maxvalue" -- get passed in
		 * as ColumnRefs.
		 */
		if (IsA(expr, ColumnRef))
		{
			ColumnRef  *cref = (ColumnRef *) expr;
			char	   *cname = NULL;

			/*
			 * There should be a single field named either "minvalue" or
			 * "maxvalue".
			 */
			if (list_length(cref->fields) == 1 &&
				IsA(linitial(cref->fields), String))
				cname = strVal(linitial(cref->fields));

			if (cname == NULL)
			{
				/*
				 * ColumnRef is not in the desired single-field-name form. For
				 * consistency between all partition strategies, let the
				 * expression transformation report any errors rather than
				 * doing it ourselves.
				 */
			}
			else if (strcmp("minvalue", cname) == 0)
			{
				prd = makeNode(PartitionRangeDatum);
				prd->kind = PARTITION_RANGE_DATUM_MINVALUE;
				prd->value = NULL;
			}
			else if (strcmp("maxvalue", cname) == 0)
			{
				prd = makeNode(PartitionRangeDatum);
				prd->kind = PARTITION_RANGE_DATUM_MAXVALUE;
				prd->value = NULL;
			}
		}

		if (prd == NULL)
		{
			char	   *colname;
			Oid			coltype;
			int32		coltypmod;
			Oid			partcollation;
			Const	   *value;

			/* Get the column's name in case we need to output an error */
			if (key->partattrs[i] != 0)
				colname = get_attname(RelationGetRelid(parent),
									  key->partattrs[i], false);
			else
			{
				colname = deparse_expression((Node *) list_nth(partexprs, j),
											 deparse_context_for(RelationGetRelationName(parent),
																 RelationGetRelid(parent)),
											 false, false);
				++j;
			}

			/* Need its type data too */
			coltype = get_partition_col_typid(key, i);
			coltypmod = get_partition_col_typmod(key, i);
			partcollation = get_partition_col_collation(key, i);

			value = transformPartitionBoundValue(pstate, expr,
												 colname,
												 coltype, coltypmod,
												 partcollation);
			if (value->constisnull)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot specify NULL in range bound")));
			prd = makeNode(PartitionRangeDatum);
			prd->kind = PARTITION_RANGE_DATUM_VALUE;
			prd->value = (Node *) value;
			++i;
		}

		prd->location = exprLocation(expr);

		result = lappend(result, prd);
	}

	/*
	 * Once we see MINVALUE or MAXVALUE for one column, the remaining columns
	 * must be the same.
	 */
	validateInfiniteBounds(pstate, result);

	return result;
}

/*
 * validateInfiniteBounds
 *
 * Check that a MAXVALUE or MINVALUE specification in a partition bound is
 * followed only by more of the same.
 */
static void
validateInfiniteBounds(ParseState *pstate, List *blist)
{
	ListCell   *lc;
	PartitionRangeDatumKind kind = PARTITION_RANGE_DATUM_VALUE;

	foreach(lc, blist)
	{
		PartitionRangeDatum *prd = castNode(PartitionRangeDatum, lfirst(lc));

		if (kind == prd->kind)
			continue;

		switch (kind)
		{
			case PARTITION_RANGE_DATUM_VALUE:
				kind = prd->kind;
				break;

			case PARTITION_RANGE_DATUM_MAXVALUE:
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("every bound following MAXVALUE must also be MAXVALUE"),
						 parser_errposition(pstate, exprLocation((Node *) prd))));
				break;

			case PARTITION_RANGE_DATUM_MINVALUE:
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("every bound following MINVALUE must also be MINVALUE"),
						 parser_errposition(pstate, exprLocation((Node *) prd))));
				break;
		}
	}
}

/*
 * Transform one constant in a partition bound spec
 */
Const *
transformPartitionBoundValue(ParseState *pstate, Node *val,
							 const char *colName, Oid colType, int32 colTypmod,
							 Oid partCollation)
{
	Node	   *value;

	/* Transform raw parsetree */
	value = transformExpr(pstate, val, EXPR_KIND_PARTITION_BOUND);

	/*
	 * Check that the input expression's collation is compatible with one
	 * specified for the parent's partition key (partcollation).  Don't throw
	 * an error if it's the default collation which we'll replace with the
	 * parent's collation anyway.
	 */
	if (IsA(value, CollateExpr))
	{
		Oid			exprCollOid = exprCollation(value);

		if (OidIsValid(exprCollOid) &&
			exprCollOid != DEFAULT_COLLATION_OID &&
			exprCollOid != partCollation)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("collation of partition bound value for column \"%s\" does not match partition key collation \"%s\"",
							colName, get_collation_name(partCollation)),
					 parser_errposition(pstate, exprLocation(value))));
	}

	/* Coerce to correct type */
	value = coerce_to_target_type(pstate,
								  value, exprType(value),
								  colType,
								  colTypmod,
								  COERCION_ASSIGNMENT,
								  COERCE_IMPLICIT_CAST,
								  -1);

	if (value == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("specified value cannot be cast to type %s for column \"%s\"",
						format_type_be(colType), colName),
				 parser_errposition(pstate, exprLocation(val))));

	/* Simplify the expression, in case we had a coercion */
	if (!IsA(value, Const))
		value = (Node *) expression_planner((Expr *) value);

	/*
	 * transformExpr() should have already rejected column references,
	 * subqueries, aggregates, window functions, and SRFs, based on the
	 * EXPR_KIND_ for a default expression.
	 */
	Assert(!contain_var_clause(value));

	/*
	 * Evaluate the expression, assigning the partition key's collation to the
	 * resulting Const expression.
	 */
	value = (Node *) evaluate_expr((Expr *) value, colType, colTypmod,
								   partCollation);
	if (!IsA(value, Const))
		elog(ERROR, "could not evaluate partition bound expression");

	return (Const *) value;
}
