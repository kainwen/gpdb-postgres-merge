/*-------------------------------------------------------------------------
 *
 * xact.h
 *	  postgres transaction system definitions
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/xact.h,v 1.103 2010/02/26 02:01:21 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef XACT_H
#define XACT_H

#include "access/xlog.h"
#include "nodes/pg_list.h"
#include "storage/relfilenode.h"
#include "utils/timestamp.h"

#include "cdb/cdbpublic.h"

/*
 * Xact isolation levels
 */
#define XACT_READ_UNCOMMITTED	0
#define XACT_READ_COMMITTED		1
#define XACT_REPEATABLE_READ	2
#define XACT_SERIALIZABLE		3

extern int	DefaultXactIsoLevel;
extern int	XactIsoLevel;

/*
 * We only implement two isolation levels internally.  This macro should
 * be used to check which one is selected.
 */
#define IsXactIsoLevelSerializable (XactIsoLevel >= XACT_REPEATABLE_READ)

/* Xact read-only state */
extern bool DefaultXactReadOnly;
extern bool XactReadOnly;

/* Asynchronous commits */
extern bool XactSyncCommit;

/* Kluge for 2PC support */
extern bool MyXactAccessedTempRel;

/*
 *	start- and end-of-transaction callbacks for dynamically loaded modules
 */
typedef enum
{
	XACT_EVENT_COMMIT,
	XACT_EVENT_ABORT,
	XACT_EVENT_PREPARE
} XactEvent;

typedef void (*XactCallback) (XactEvent event, void *arg);

typedef enum
{
	SUBXACT_EVENT_START_SUB,
	SUBXACT_EVENT_COMMIT_SUB,
	SUBXACT_EVENT_ABORT_SUB
} SubXactEvent;

typedef void (*SubXactCallback) (SubXactEvent event, SubTransactionId mySubid,
									SubTransactionId parentSubid, void *arg);


/* ----------------
 *		transaction-related XLOG entries
 * ----------------
 */

/*
 * XLOG allows to store some information in high 4 bits of log
 * record xl_info field
 */
#define XLOG_XACT_COMMIT			0x00
#define XLOG_XACT_PREPARE			0x10
#define XLOG_XACT_ABORT				0x20
#define XLOG_XACT_COMMIT_PREPARED	0x30
#define XLOG_XACT_ABORT_PREPARED	0x40
<<<<<<< HEAD
#define XLOG_XACT_DISTRIBUTED_COMMIT 0x50
#define XLOG_XACT_DISTRIBUTED_FORGET 0x60
=======
#define XLOG_XACT_ASSIGNMENT		0x50

typedef struct xl_xact_assignment
{
	TransactionId xtop;			/* assigned XID's top-level XID */
	int			nsubxacts;		/* number of subtransaction XIDs */
	TransactionId xsub[1];		/* assigned subxids */
} xl_xact_assignment;

#define MinSizeOfXactAssignment offsetof(xl_xact_assignment, xsub)
>>>>>>> 1084f317702e1a039696ab8a37caf900e55ec8f2

typedef struct xl_xact_commit
{
	TimestampTz xact_time;		/* time of commit */
<<<<<<< HEAD
	time_t		xtime;
=======
	uint32		xinfo;			/* info flags */
>>>>>>> 1084f317702e1a039696ab8a37caf900e55ec8f2
	int			nrels;			/* number of RelFileNodes */
	int			nsubxacts;		/* number of subtransaction XIDs */
	int			nmsgs;			/* number of shared inval msgs */
	Oid			dbId;			/* MyDatabaseId */
	Oid			tsId;			/* MyDatabaseTableSpace */
	/* Array of RelFileNode(s) to drop at commit */
	RelFileNode xnodes[1];		/* VARIABLE LENGTH ARRAY */
	/* ARRAY OF COMMITTED SUBTRANSACTION XIDs FOLLOWS */
<<<<<<< HEAD
	/* DISTRIBUTED XACT STUFF FOLLOWS */
=======
	/* ARRAY OF SHARED INVALIDATION MESSAGES FOLLOWS */
>>>>>>> 1084f317702e1a039696ab8a37caf900e55ec8f2
} xl_xact_commit;

#define MinSizeOfXactCommit offsetof(xl_xact_commit, xnodes)

/*
 * These flags are set in the xinfo fields of WAL commit records,
 * indicating a variety of additional actions that need to occur
 * when emulating transaction effects during recovery.
 * They are named XactCompletion... to differentiate them from
 * EOXact... routines which run at the end of the original
 * transaction completion.
 */
#define XACT_COMPLETION_UPDATE_RELCACHE_FILE	0x01
#define XACT_COMPLETION_FORCE_SYNC_COMMIT		0x02

/* Access macros for above flags */
#define XactCompletionRelcacheInitFileInval(xlrec)	((xlrec)->xinfo & XACT_COMPLETION_UPDATE_RELCACHE_FILE)
#define XactCompletionForceSyncCommit(xlrec)		((xlrec)->xinfo & XACT_COMPLETION_FORCE_SYNC_COMMIT)

typedef struct xl_xact_abort
{
	TimestampTz xact_time;		/* time of abort */
	time_t		xtime;
	int			nrels;			/* number of RelFileNodes */
	int			nsubxacts;		/* number of subtransaction XIDs */
	/* Array of RelFileNode(s) to drop at abort */
	RelFileNode xnodes[1];		/* VARIABLE LENGTH ARRAY */
	/* ARRAY OF ABORTED SUBTRANSACTION XIDs FOLLOWS */
} xl_xact_abort;

/* Note the intentional lack of an invalidation message array c.f. commit */

#define MinSizeOfXactAbort offsetof(xl_xact_abort, xnodes)

/*
 * COMMIT_PREPARED and ABORT_PREPARED are identical to COMMIT/ABORT records
 * except that we have to store the XID of the prepared transaction explicitly
 * --- the XID in the record header will be for the transaction doing the
 * COMMIT PREPARED or ABORT PREPARED command.
 */

typedef struct xl_xact_commit_prepared
{
	TransactionId xid;			/* XID of prepared xact */
	DistributedTransactionTimeStamp distribTimeStamp;
	DistributedTransactionId        distribXid;
	xl_xact_commit crec;		/* COMMIT record */
	/* MORE DATA FOLLOWS AT END OF STRUCT */
} xl_xact_commit_prepared;

#define MinSizeOfXactCommitPrepared offsetof(xl_xact_commit_prepared, crec.xnodes)

typedef struct xl_xact_abort_prepared
{
	TransactionId xid;			/* XID of prepared xact */
	xl_xact_abort arec;			/* ABORT record */
	/* MORE DATA FOLLOWS AT END OF STRUCT */
} xl_xact_abort_prepared;

#define MinSizeOfXactAbortPrepared offsetof(xl_xact_abort_prepared, arec.xnodes)

/* 
 * xl_xact_distributed_forget - moved to cdb/cdbtm.h 
 */
typedef struct xl_xact_distributed_forget
{
	TMGXACT_LOG gxact_log;
} xl_xact_distributed_forget;

/* ----------------
 *		extern definitions
 * ----------------
 */

/* Greenplum Database specific */ 
extern void SetSharedTransactionId_writer(void);
extern void SetSharedTransactionId_reader(TransactionId xid, CommandId cid);
extern bool IsTransactionState(void);
extern bool IsAbortInProgress(void);
extern bool IsTransactionPreparing(void);
extern bool IsAbortedTransactionBlockState(void);
extern void GetAllTransactionXids(
	DistributedTransactionId	*distribXid,
	TransactionId				*localXid,
	TransactionId				*subXid);
extern TransactionId GetTopTransactionId(void);
extern TransactionId GetTopTransactionIdIfAny(void);
extern TransactionId GetCurrentTransactionId(void);
extern TransactionId GetCurrentTransactionIdIfAny(void);
extern SubTransactionId GetCurrentSubTransactionId(void);
extern CommandId GetCurrentCommandId(bool used);
extern TimestampTz GetCurrentTransactionStartTimestamp(void);
extern TimestampTz GetCurrentStatementStartTimestamp(void);
extern TimestampTz GetCurrentTransactionStopTimestamp(void);
extern void SetCurrentStatementStartTimestamp(void);
extern void SetCurrentStatementStartTimestampToMaster(TimestampTz masterTime);
extern int	GetCurrentTransactionNestLevel(void);
extern bool TransactionIdIsCurrentTransactionId(TransactionId xid);
extern void CommandCounterIncrement(void);
extern void ForceSyncCommit(void);
extern void StartTransactionCommand(void);
extern void CommitTransactionCommand(void);
extern void AbortCurrentTransaction(void);
extern void BeginTransactionBlock(void);
extern bool EndTransactionBlock(void);
extern bool PrepareTransactionBlock(char *gid);
extern void UserAbortTransactionBlock(void);
extern void ReleaseSavepoint(List *options);
extern void DefineSavepoint(char *name);
extern void DefineDispatchSavepoint(char *name);
extern void RollbackToSavepoint(List *options);
extern void BeginInternalSubTransaction(char *name);
extern void ReleaseCurrentSubTransaction(void);
extern void RollbackAndReleaseCurrentSubTransaction(void);
extern bool IsSubTransaction(void);
extern bool IsTransactionBlock(void);
extern bool IsTransactionOrTransactionBlock(void);
extern void ExecutorMarkTransactionUsesSequences(void);
extern void ExecutorMarkTransactionDoesWrites(void);
extern bool ExecutorSaysTransactionDoesWrites(void);
extern char TransactionBlockStatusCode(void);
extern void AbortOutOfAnyTransaction(void);
extern void PreventTransactionChain(bool isTopLevel, const char *stmtType);
extern void RequireTransactionChain(bool isTopLevel, const char *stmtType);
extern bool IsInTransactionChain(bool isTopLevel);
extern void RegisterXactCallback(XactCallback callback, void *arg);
extern void UnregisterXactCallback(XactCallback callback, void *arg);
extern void RegisterXactCallbackOnce(XactCallback callback, void *arg);
extern void UnregisterXactCallbackOnce(XactCallback callback, void *arg);
extern void RegisterSubXactCallback(SubXactCallback callback, void *arg);
extern void UnregisterSubXactCallback(SubXactCallback callback, void *arg);

<<<<<<< HEAD
extern void RecordDistributedForgetCommitted(struct TMGXACT_LOG *gxact_log);

=======
>>>>>>> 1084f317702e1a039696ab8a37caf900e55ec8f2
extern int	xactGetCommittedChildren(TransactionId **ptr);

extern void xact_redo(XLogRecPtr beginLoc, XLogRecPtr lsn, XLogRecord *record);
extern void xact_desc(StringInfo buf, XLogRecPtr beginLoc, XLogRecord *record);
extern const char *IsoLevelAsUpperString(int IsoLevel);

#endif   /* XACT_H */
