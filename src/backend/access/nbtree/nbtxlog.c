/*-------------------------------------------------------------------------
 *
 * nbtxlog.c
 *	  WAL replay logic for btrees.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/nbtree/nbtxlog.c,v 1.69 2010/07/06 19:18:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "access/transam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "storage/standby.h"
#include "miscadmin.h"

#include "access/bufmask.h"

/*
 * We must keep track of expected insertions due to page splits, and apply
 * them manually if they are not seen in the WAL log during replay.  This
 * makes it safe for page insertion to be a multiple-WAL-action process.
 *
 * Similarly, deletion of an only child page and deletion of its parent page
 * form multiple WAL log entries, and we have to be prepared to follow through
 * with the deletion if the log ends between.
 *
 * The data structure is a simple linked list --- this should be good enough,
 * since we don't expect a page split or multi deletion to remain incomplete
 * for long.  In any case we need to respect the order of operations.
 */
typedef struct bt_incomplete_action
{
	RelFileNode node;			/* the index */
	bool		is_split;		/* T = pending split, F = pending delete */
	/* these fields are for a split: */
	bool		is_root;		/* we split the root */
	BlockNumber leftblk;		/* left half of split */
	BlockNumber rightblk;		/* right half of split */
	/* these fields are for a delete: */
	BlockNumber delblk;			/* parent block to be deleted */
} bt_incomplete_action;

static List *incomplete_actions;


static void
log_incomplete_split(RelFileNode node, BlockNumber leftblk,
					 BlockNumber rightblk, bool is_root)
{
	bt_incomplete_action *action = palloc(sizeof(bt_incomplete_action));

	action->node = node;
	action->is_split = true;
	action->is_root = is_root;
	action->leftblk = leftblk;
	action->rightblk = rightblk;
	incomplete_actions = lappend(incomplete_actions, action);
}

static void
forget_matching_split(RelFileNode node, BlockNumber downlink, bool is_root)
{
	ListCell   *l;

	foreach(l, incomplete_actions)
	{
		bt_incomplete_action *action = (bt_incomplete_action *) lfirst(l);

		if (RelFileNodeEquals(node, action->node) &&
			action->is_split &&
			downlink == action->rightblk)
		{
			if (is_root != action->is_root)
				elog(LOG, "forget_matching_split: fishy is_root data (expected %d, got %d)",
					 action->is_root, is_root);
			incomplete_actions = list_delete_ptr(incomplete_actions, action);
			pfree(action);
			break;				/* need not look further */
		}
	}
}

static void
log_incomplete_deletion(RelFileNode node, BlockNumber delblk)
{
	bt_incomplete_action *action = palloc(sizeof(bt_incomplete_action));

	action->node = node;
	action->is_split = false;
	action->delblk = delblk;
	incomplete_actions = lappend(incomplete_actions, action);
}

static void
forget_matching_deletion(RelFileNode node, BlockNumber delblk)
{
	ListCell   *l;

	foreach(l, incomplete_actions)
	{
		bt_incomplete_action *action = (bt_incomplete_action *) lfirst(l);

		if (RelFileNodeEquals(node, action->node) &&
			!action->is_split &&
			delblk == action->delblk)
		{
			incomplete_actions = list_delete_ptr(incomplete_actions, action);
			pfree(action);
			break;				/* need not look further */
		}
	}
}

/*
 * _bt_restore_page -- re-enter all the index tuples on a page
 *
 * The page is freshly init'd, and *from (length len) is a copy of what
 * had been its upper part (pd_upper to pd_special).  We assume that the
 * tuples had been added to the page in item-number order, and therefore
 * the one with highest item number appears first (lowest on the page).
 *
 * NOTE: the way this routine is coded, the rebuilt page will have the items
 * in correct itemno sequence, but physically the opposite order from the
 * original, because we insert them in the opposite of itemno order.  This
 * does not matter in any current btree code, but it's something to keep an
 * eye on.	Is it worth changing just on general principles?  See also the
 * notes in btree_xlog_split().
 */
static void
_bt_restore_page(Page page, char *from, int len)
{
	IndexTupleData itupdata;
	Size		itemsz;
	char	   *end = from + len;

	for (; from < end;)
	{
		/* Need to copy tuple header due to alignment considerations */
		memcpy(&itupdata, from, sizeof(IndexTupleData));
		itemsz = IndexTupleDSize(itupdata);
		itemsz = MAXALIGN(itemsz);
		if (PageAddItem(page, (Item) from, itemsz, FirstOffsetNumber,
						false, false) == InvalidOffsetNumber)
			elog(PANIC, "_bt_restore_page: cannot add item to page");
		from += itemsz;
	}
}

static void
_bt_restore_meta(RelFileNode rnode, XLogRecPtr lsn,
				 BlockNumber root, uint32 level,
				 BlockNumber fastroot, uint32 fastlevel)
{
	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *md;
	BTPageOpaque pageop;

	metabuf = XLogReadBuffer(rnode, BTREE_METAPAGE, true);
	Assert(BufferIsValid(metabuf));
	metapg = BufferGetPage(metabuf);

	_bt_pageinit(metapg, BufferGetPageSize(metabuf));

	md = BTPageGetMeta(metapg);
	md->btm_magic = BTREE_MAGIC;
	md->btm_version = BTREE_VERSION;
	md->btm_root = root;
	md->btm_level = level;
	md->btm_fastroot = fastroot;
	md->btm_fastlevel = fastlevel;

	pageop = (BTPageOpaque) PageGetSpecialPointer(metapg);
	pageop->btpo_flags = BTP_META;

	/*
	 * Set pd_lower just past the end of the metadata.	This is not essential
	 * but it makes the page look compressible to xlog.c.
	 */
	((PageHeader) metapg)->pd_lower =
		((char *) md + sizeof(BTMetaPageData)) - (char *) metapg;

	PageSetLSN(metapg, lsn);
	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);
}

static void
btree_xlog_insert(bool isleaf, bool ismeta,
				  XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_insert *xlrec = (xl_btree_insert *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	char	   *datapos;
	int			datalen;
	xl_btree_metadata md;
	BlockNumber downlink = 0;

	datapos = (char *) xlrec + SizeOfBtreeInsert;
	datalen = record->xl_len - SizeOfBtreeInsert;
	if (!isleaf)
	{
		memcpy(&downlink, datapos, sizeof(BlockNumber));
		datapos += sizeof(BlockNumber);
		datalen -= sizeof(BlockNumber);
	}
	if (ismeta)
	{
		memcpy(&md, datapos, sizeof(xl_btree_metadata));
		datapos += sizeof(xl_btree_metadata);
		datalen -= sizeof(xl_btree_metadata);
	}

	if ((IsBkpBlockApplied(record, 0)) && !ismeta && isleaf)
		return;					/* nothing to do */

	if (!(IsBkpBlockApplied(record, 0)))
	{
		buffer = XLogReadBuffer(xlrec->target.node,
							 ItemPointerGetBlockNumber(&(xlrec->target.tid)),
								false);
		if (BufferIsValid(buffer))
		{
			page = (Page) BufferGetPage(buffer);

			if (XLByteLE(lsn, PageGetLSN(page)))
			{
				UnlockReleaseBuffer(buffer);
			}
			else
			{
				if (PageAddItem(page, (Item) datapos, datalen,
							ItemPointerGetOffsetNumber(&(xlrec->target.tid)),
								false, false) == InvalidOffsetNumber)
					elog(PANIC, "btree_insert_redo: failed to add item");

				PageSetLSN(page, lsn);
				MarkBufferDirty(buffer);
				UnlockReleaseBuffer(buffer);
			}
		}
	}

	if (ismeta)
		_bt_restore_meta(xlrec->target.node, lsn,
						 md.root, md.level,
						 md.fastroot, md.fastlevel);

	/* Forget any split this insertion completes */
	if (!isleaf)
		forget_matching_split(xlrec->target.node, downlink, false);
}

static void
btree_xlog_split(bool onleft, bool isroot,
				 XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_split *xlrec = (xl_btree_split *) XLogRecGetData(record);
	Buffer		rbuf;
	Page		rpage;
	BTPageOpaque ropaque;
	char	   *datapos;
	int			datalen;
	OffsetNumber newitemoff = 0;
	Item		newitem = NULL;
	Size		newitemsz = 0;
	Item		left_hikey = NULL;
	Size		left_hikeysz = 0;

	datapos = (char *) xlrec + SizeOfBtreeSplit;
	datalen = record->xl_len - SizeOfBtreeSplit;

	/* Forget any split this insertion completes */
	if (xlrec->level > 0)
	{
		/* we assume SizeOfBtreeSplit is at least 16-bit aligned */
		BlockNumber downlink = BlockIdGetBlockNumber((BlockId) datapos);

		datapos += sizeof(BlockIdData);
		datalen -= sizeof(BlockIdData);

		forget_matching_split(xlrec->node, downlink, false);

		/* Extract left hikey and its size (still assuming 16-bit alignment) */
		if (!(IsBkpBlockApplied(record, 0)))
		{
			/* We assume 16-bit alignment is enough for IndexTupleSize */
			left_hikey = (Item) datapos;
			left_hikeysz = MAXALIGN(IndexTupleSize(left_hikey));

			datapos += left_hikeysz;
			datalen -= left_hikeysz;
		}
	}

	/* Extract newitem and newitemoff, if present */
	if (onleft)
	{
		/* Extract the offset (still assuming 16-bit alignment) */
		memcpy(&newitemoff, datapos, sizeof(OffsetNumber));
		datapos += sizeof(OffsetNumber);
		datalen -= sizeof(OffsetNumber);
	}

	if (onleft && !(IsBkpBlockApplied(record, 0)))
	{
		/*
		 * We assume that 16-bit alignment is enough to apply IndexTupleSize
		 * (since it's fetching from a uint16 field) and also enough for
		 * PageAddItem to insert the tuple.
		 */
		newitem = (Item) datapos;
		newitemsz = MAXALIGN(IndexTupleSize(newitem));
		datapos += newitemsz;
		datalen -= newitemsz;
	}

	/* Reconstruct right (new) sibling from scratch */
	rbuf = XLogReadBuffer(xlrec->node, xlrec->rightsib, true);
	Assert(BufferIsValid(rbuf));
	rpage = (Page) BufferGetPage(rbuf);

	_bt_pageinit(rpage, BufferGetPageSize(rbuf));
	ropaque = (BTPageOpaque) PageGetSpecialPointer(rpage);

	ropaque->btpo_prev = xlrec->leftsib;
	ropaque->btpo_next = xlrec->rnext;
	ropaque->btpo.level = xlrec->level;
	ropaque->btpo_flags = (xlrec->level == 0) ? BTP_LEAF : 0;
	ropaque->btpo_cycleid = 0;

	_bt_restore_page(rpage, datapos, datalen);

	/*
	 * On leaf level, the high key of the left page is equal to the first key
	 * on the right page.
	 */
	if (xlrec->level == 0)
	{
		ItemId		hiItemId = PageGetItemId(rpage, P_FIRSTDATAKEY(ropaque));

		left_hikey = PageGetItem(rpage, hiItemId);
		left_hikeysz = ItemIdGetLength(hiItemId);
	}

	PageSetLSN(rpage, lsn);
	MarkBufferDirty(rbuf);

	/* don't release the buffer yet; we touch right page's first item below */

	/*
	 * Reconstruct left (original) sibling if needed.  Note that this code
	 * ensures that the items remaining on the left page are in the correct
	 * item number order, but it does not reproduce the physical order they
	 * would have had.	Is this worth changing?  See also _bt_restore_page().
	 */
	if (!(IsBkpBlockApplied(record, 0)))
	{
		Buffer		lbuf = XLogReadBuffer(xlrec->node, xlrec->leftsib, false);

		if (BufferIsValid(lbuf))
		{
			Page		lpage = (Page) BufferGetPage(lbuf);
			BTPageOpaque lopaque = (BTPageOpaque) PageGetSpecialPointer(lpage);

			if (!XLByteLE(lsn, PageGetLSN(lpage)))
			{
				OffsetNumber off;
				OffsetNumber maxoff = PageGetMaxOffsetNumber(lpage);
				OffsetNumber deletable[MaxOffsetNumber];
				int			ndeletable = 0;

				/*
				 * Remove the items from the left page that were copied to the
				 * right page.	Also remove the old high key, if any. (We must
				 * remove everything before trying to insert any items, else
				 * we risk not having enough space.)
				 */
				if (!P_RIGHTMOST(lopaque))
				{
					deletable[ndeletable++] = P_HIKEY;

					/*
					 * newitemoff is given to us relative to the original
					 * page's item numbering, so adjust it for this deletion.
					 */
					newitemoff--;
				}
				for (off = xlrec->firstright; off <= maxoff; off++)
					deletable[ndeletable++] = off;
				if (ndeletable > 0)
					PageIndexMultiDelete(lpage, deletable, ndeletable);

				/*
				 * Add the new item if it was inserted on left page.
				 */
				if (onleft)
				{
					if (PageAddItem(lpage, newitem, newitemsz, newitemoff,
									false, false) == InvalidOffsetNumber)
						elog(PANIC, "failed to add new item to left page after split");
				}

				/* Set high key */
				if (PageAddItem(lpage, left_hikey, left_hikeysz,
								P_HIKEY, false, false) == InvalidOffsetNumber)
					elog(PANIC, "failed to add high key to left page after split");

				/* Fix opaque fields */
				lopaque->btpo_flags = (xlrec->level == 0) ? BTP_LEAF : 0;
				lopaque->btpo_next = xlrec->rightsib;
				lopaque->btpo_cycleid = 0;

				PageSetLSN(lpage, lsn);
				MarkBufferDirty(lbuf);
			}

			UnlockReleaseBuffer(lbuf);
		}
	}

	/* We no longer need the right buffer */
	UnlockReleaseBuffer(rbuf);

	/* Fix left-link of the page to the right of the new right sibling */
	if (xlrec->rnext != P_NONE && !(IsBkpBlockApplied(record, 1)))
	{
		Buffer		buffer = XLogReadBuffer(xlrec->node, xlrec->rnext, false);

		if (BufferIsValid(buffer))
		{
			Page		page = (Page) BufferGetPage(buffer);

			if (!XLByteLE(lsn, PageGetLSN(page)))
			{
				BTPageOpaque pageop = (BTPageOpaque) PageGetSpecialPointer(page);

				pageop->btpo_prev = xlrec->rightsib;

				PageSetLSN(page, lsn);
				MarkBufferDirty(buffer);
			}
			UnlockReleaseBuffer(buffer);
		}
	}

	/* The job ain't done till the parent link is inserted... */
	log_incomplete_split(xlrec->node,
						 xlrec->leftsib, xlrec->rightsib, isroot);
}

static void
btree_xlog_vacuum(XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_vacuum *xlrec;
	Buffer		buffer;
	Page		page;
	BTPageOpaque opaque;

	xlrec = (xl_btree_vacuum *) XLogRecGetData(record);

	/*
	 * If queries might be active then we need to ensure every block is
	 * unpinned between the lastBlockVacuumed and the current block, if there
	 * are any. This ensures that every block in the index is touched during
	 * VACUUM as required to ensure scans work correctly.
	 */
	if (standbyState == STANDBY_SNAPSHOT_READY &&
		(xlrec->lastBlockVacuumed + 1) != xlrec->block)
	{
		BlockNumber blkno = xlrec->lastBlockVacuumed + 1;

		for (; blkno < xlrec->block; blkno++)
		{
			/*
			 * XXX we don't actually need to read the block, we just need to
			 * confirm it is unpinned. If we had a special call into the
			 * buffer manager we could optimise this so that if the block is
			 * not in shared_buffers we confirm it as unpinned.
			 *
			 * Another simple optimization would be to check if there's any
			 * backends running; if not, we could just skip this.
			 */
			buffer = XLogReadBufferExtended(xlrec->node, MAIN_FORKNUM, blkno, RBM_NORMAL);
			if (BufferIsValid(buffer))
			{
				LockBufferForCleanup(buffer);
				UnlockReleaseBuffer(buffer);
			}
		}
	}

	/*
	 * If the block was restored from a full page image, nothing more to do.
	 * The RestoreBkpBlocks() call already pinned and took cleanup lock on it.
	 * XXX: Perhaps we should call RestoreBkpBlocks() *after* the loop above,
	 * to make the disk access more sequential.
	 */
	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	/*
	 * Like in btvacuumpage(), we need to take a cleanup lock on every leaf
	 * page. See nbtree/README for details.
	 */
	buffer = XLogReadBufferExtended(xlrec->node, MAIN_FORKNUM, xlrec->block, RBM_NORMAL);
	if (!BufferIsValid(buffer))
		return;
	LockBufferForCleanup(buffer);
	page = (Page) BufferGetPage(buffer);

	if (XLByteLE(lsn, PageGetLSN(page)))
	{
		UnlockReleaseBuffer(buffer);
		return;
	}

	if (record->xl_len > SizeOfBtreeVacuum)
	{
		OffsetNumber *unused;
		OffsetNumber *unend;

		unused = (OffsetNumber *) ((char *) xlrec + SizeOfBtreeVacuum);
		unend = (OffsetNumber *) ((char *) xlrec + record->xl_len);

		if ((unend - unused) > 0)
			PageIndexMultiDelete(page, unused, unend - unused);
	}

	/*
	 * Mark the page as not containing any LP_DEAD items --- see comments in
	 * _bt_delitems().
	 */
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

/*
 * Get the latestRemovedXid from the heap pages pointed at by the index
 * tuples being deleted. This puts the work for calculating latestRemovedXid
 * into the recovery path rather than the primary path.
 *
 * It's possible that this generates a fair amount of I/O, since an index
 * block may have hundreds of tuples being deleted. Repeat accesses to the
 * same heap blocks are common, though are not yet optimised.
 *
 * XXX optimise later with something like XLogPrefetchBuffer()
 */
static TransactionId
btree_xlog_delete_get_latestRemovedXid(XLogRecord *record)
{
	xl_btree_delete *xlrec = (xl_btree_delete *) XLogRecGetData(record);
	OffsetNumber *unused;
	Buffer		ibuffer,
				hbuffer;
	Page		ipage,
				hpage;
	ItemId		iitemid,
				hitemid;
	IndexTuple	itup;
	HeapTupleHeader htuphdr;
	BlockNumber hblkno;
	OffsetNumber hoffnum;
	TransactionId latestRemovedXid = InvalidTransactionId;
	TransactionId htupxid = InvalidTransactionId;
	int			i;

	/*
	 * If there's nothing running on the standby we don't need to derive a
	 * full latestRemovedXid value, so use a fast path out of here. That
	 * returns InvalidTransactionId, and so will conflict with users, but
	 * since we just worked out that's zero people, its OK.
	 */
	if (CountDBBackends(InvalidOid) == 0)
		return latestRemovedXid;

	/*
	 * Get index page
	 */
	ibuffer = XLogReadBuffer(xlrec->node, xlrec->block, false);
	if (!BufferIsValid(ibuffer))
		return InvalidTransactionId;
	ipage = (Page) BufferGetPage(ibuffer);

	/*
	 * Loop through the deleted index items to obtain the TransactionId from
	 * the heap items they point to.
	 */
	unused = (OffsetNumber *) ((char *) xlrec + SizeOfBtreeDelete);

	for (i = 0; i < xlrec->nitems; i++)
	{
		/*
		 * Identify the index tuple about to be deleted
		 */
		iitemid = PageGetItemId(ipage, unused[i]);
		itup = (IndexTuple) PageGetItem(ipage, iitemid);

		/*
		 * Locate the heap page that the index tuple points at
		 */
		hblkno = ItemPointerGetBlockNumber(&(itup->t_tid));
		hbuffer = XLogReadBuffer(xlrec->hnode, hblkno, false);
		if (!BufferIsValid(hbuffer))
		{
			UnlockReleaseBuffer(ibuffer);
			return InvalidTransactionId;
		}
		hpage = (Page) BufferGetPage(hbuffer);

		/*
		 * Look up the heap tuple header that the index tuple points at by
		 * using the heap node supplied with the xlrec. We can't use
		 * heap_fetch, since it uses ReadBuffer rather than XLogReadBuffer.
		 * Note that we are not looking at tuple data here, just headers.
		 */
		hoffnum = ItemPointerGetOffsetNumber(&(itup->t_tid));
		hitemid = PageGetItemId(hpage, hoffnum);

		/*
		 * Follow any redirections until we find something useful.
		 */
		while (ItemIdIsRedirected(hitemid))
		{
			hoffnum = ItemIdGetRedirect(hitemid);
			hitemid = PageGetItemId(hpage, hoffnum);
			CHECK_FOR_INTERRUPTS();
		}

		/*
		 * If the heap item has storage, then read the header. Some LP_DEAD
		 * items may not be accessible, so we ignore them.
		 */
		if (ItemIdHasStorage(hitemid))
		{
			htuphdr = (HeapTupleHeader) PageGetItem(hpage, hitemid);

			/*
			 * Get the heap tuple's xmin/xmax and ratchet up the
			 * latestRemovedXid. No need to consider xvac values here.
			 */
			htupxid = HeapTupleHeaderGetXmin(htuphdr);
			if (TransactionIdFollows(htupxid, latestRemovedXid))
				latestRemovedXid = htupxid;

			htupxid = HeapTupleHeaderGetXmax(htuphdr);
			if (TransactionIdFollows(htupxid, latestRemovedXid))
				latestRemovedXid = htupxid;
		}
		else if (ItemIdIsDead(hitemid))
		{
			/*
			 * Conjecture: if hitemid is dead then it had xids before the xids
			 * marked on LP_NORMAL items. So we just ignore this item and move
			 * onto the next, for the purposes of calculating
			 * latestRemovedxids.
			 */
		}
		else
			Assert(!ItemIdIsUsed(hitemid));

		UnlockReleaseBuffer(hbuffer);
	}

	UnlockReleaseBuffer(ibuffer);

	/*
	 * Note that if all heap tuples were LP_DEAD then we will be returning
	 * InvalidTransactionId here. That can happen if we are re-replaying this
	 * record type, though that will be before the consistency point and will
	 * not cause problems. It should happen very rarely after the consistency
	 * point, though note that we can't tell the difference between this and
	 * the fast path exit above. May need to change that in future.
	 */
	return latestRemovedXid;
}

static void
btree_xlog_delete(XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_delete *xlrec;
	Buffer		buffer;
	Page		page;
	BTPageOpaque opaque;

	if (IsBkpBlockApplied(record, 0))
		return;

	xlrec = (xl_btree_delete *) XLogRecGetData(record);

<<<<<<< HEAD
=======
	/*
	 * We don't need to take a cleanup lock to apply these changes. See
	 * nbtree/README for details.
	 */
>>>>>>> 1084f317702e1a039696ab8a37caf900e55ec8f2
	buffer = XLogReadBuffer(xlrec->node, xlrec->block, false);
	if (!BufferIsValid(buffer))
		return;
	page = (Page) BufferGetPage(buffer);

	if (XLByteLE(lsn, PageGetLSN(page)))
	{
		UnlockReleaseBuffer(buffer);
		return;
	}

	if (record->xl_len > SizeOfBtreeDelete)
	{
		OffsetNumber *unused;

		unused = (OffsetNumber *) ((char *) xlrec + SizeOfBtreeDelete);

		PageIndexMultiDelete(page, unused, xlrec->nitems);
	}

	/*
	 * Mark the page as not containing any LP_DEAD items --- see comments in
	 * _bt_delitems().
	 */
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

	PageSetLSN(page, lsn);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
btree_xlog_delete_page(uint8 info, XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_delete_page *xlrec = (xl_btree_delete_page *) XLogRecGetData(record);
	BlockNumber parent;
	BlockNumber target;
	BlockNumber leftsib;
	BlockNumber rightsib;
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;

	parent = ItemPointerGetBlockNumber(&(xlrec->target.tid));
	target = xlrec->deadblk;
	leftsib = xlrec->leftblk;
	rightsib = xlrec->rightblk;

	/* parent page */
	if (!(IsBkpBlockApplied(record, 0)))
	{
		buffer = XLogReadBuffer(xlrec->target.node, parent, false);
		if (BufferIsValid(buffer))
		{
			page = (Page) BufferGetPage(buffer);
			pageop = (BTPageOpaque) PageGetSpecialPointer(page);
			if (XLByteLE(lsn, PageGetLSN(page)))
			{
				UnlockReleaseBuffer(buffer);
			}
			else
			{
				OffsetNumber poffset;

				poffset = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
				if (poffset >= PageGetMaxOffsetNumber(page))
				{
					Assert(info == XLOG_BTREE_DELETE_PAGE_HALF);
					Assert(poffset == P_FIRSTDATAKEY(pageop));
					PageIndexTupleDelete(page, poffset);
					pageop->btpo_flags |= BTP_HALF_DEAD;
				}
				else
				{
					ItemId		itemid;
					IndexTuple	itup;
					OffsetNumber nextoffset;

					Assert(info != XLOG_BTREE_DELETE_PAGE_HALF);
					itemid = PageGetItemId(page, poffset);
					itup = (IndexTuple) PageGetItem(page, itemid);
					ItemPointerSet(&(itup->t_tid), rightsib, P_HIKEY);
					nextoffset = OffsetNumberNext(poffset);
					PageIndexTupleDelete(page, nextoffset);
				}

				PageSetLSN(page, lsn);
				MarkBufferDirty(buffer);
				UnlockReleaseBuffer(buffer);
			}
		}
	}

	/* Fix left-link of right sibling */
	if (!(IsBkpBlockApplied(record, 1)))
	{
		buffer = XLogReadBuffer(xlrec->target.node, rightsib, false);
		if (BufferIsValid(buffer))
		{
			page = (Page) BufferGetPage(buffer);
			if (XLByteLE(lsn, PageGetLSN(page)))
			{
				UnlockReleaseBuffer(buffer);
			}
			else
			{
				pageop = (BTPageOpaque) PageGetSpecialPointer(page);
				pageop->btpo_prev = leftsib;

				PageSetLSN(page, lsn);
				MarkBufferDirty(buffer);
				UnlockReleaseBuffer(buffer);
			}
		}
	}

	/* Fix right-link of left sibling, if any */
	if (!(IsBkpBlockApplied(record, 2)))
	{
		if (leftsib != P_NONE)
		{
			buffer = XLogReadBuffer(xlrec->target.node, leftsib, false);
			if (BufferIsValid(buffer))
			{
				page = (Page) BufferGetPage(buffer);
				if (XLByteLE(lsn, PageGetLSN(page)))
				{
					UnlockReleaseBuffer(buffer);
				}
				else
				{
					pageop = (BTPageOpaque) PageGetSpecialPointer(page);
					pageop->btpo_next = rightsib;

					PageSetLSN(page, lsn);
					MarkBufferDirty(buffer);
					UnlockReleaseBuffer(buffer);
				}
			}
		}
	}

	/* Rewrite target page as empty deleted page */
	buffer = XLogReadBuffer(xlrec->target.node, target, true);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	_bt_pageinit(page, BufferGetPageSize(buffer));
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	pageop->btpo_prev = leftsib;
	pageop->btpo_next = rightsib;
	pageop->btpo.xact = xlrec->btpo_xact;
	pageop->btpo_flags = BTP_DELETED;
	pageop->btpo_cycleid = 0;

	PageSetLSN(page, lsn);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	/* Update metapage if needed */
	if (info == XLOG_BTREE_DELETE_PAGE_META)
	{
		xl_btree_metadata md;

		memcpy(&md, (char *) xlrec + SizeOfBtreeDeletePage,
			   sizeof(xl_btree_metadata));
		_bt_restore_meta(xlrec->target.node, lsn,
						 md.root, md.level,
						 md.fastroot, md.fastlevel);
	}

	/* Forget any completed deletion */
	forget_matching_deletion(xlrec->target.node, target);

	/* If parent became half-dead, remember it for deletion */
	if (info == XLOG_BTREE_DELETE_PAGE_HALF)
		log_incomplete_deletion(xlrec->target.node, parent);
}

static void
btree_xlog_newroot(XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_newroot *xlrec = (xl_btree_newroot *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;
	BlockNumber downlink = 0;

	buffer = XLogReadBuffer(xlrec->node, xlrec->rootblk, true);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	_bt_pageinit(page, BufferGetPageSize(buffer));
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	pageop->btpo_flags = BTP_ROOT;
	pageop->btpo_prev = pageop->btpo_next = P_NONE;
	pageop->btpo.level = xlrec->level;
	if (xlrec->level == 0)
		pageop->btpo_flags |= BTP_LEAF;
	pageop->btpo_cycleid = 0;

	if (record->xl_len > SizeOfBtreeNewroot)
	{
		IndexTuple	itup;

		_bt_restore_page(page,
						 (char *) xlrec + SizeOfBtreeNewroot,
						 record->xl_len - SizeOfBtreeNewroot);
		/* extract downlink to the right-hand split page */
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, P_FIRSTKEY));
		downlink = ItemPointerGetBlockNumber(&(itup->t_tid));
		Assert(ItemPointerGetOffsetNumber(&(itup->t_tid)) == P_HIKEY);
	}

	PageSetLSN(page, lsn);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	_bt_restore_meta(xlrec->node, lsn,
					 xlrec->rootblk, xlrec->level,
					 xlrec->rootblk, xlrec->level);

	/* Check to see if this satisfies any incomplete insertions */
	if (record->xl_len > SizeOfBtreeNewroot)
		forget_matching_split(xlrec->node, downlink, true);
}


void
btree_redo(XLogRecPtr beginLoc, XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	if (InHotStandby)
	{
		switch (info)
		{
			case XLOG_BTREE_DELETE:

				/*
				 * Btree delete records can conflict with standby queries. You
				 * might think that vacuum records would conflict as well, but
				 * we've handled that already. XLOG_HEAP2_CLEANUP_INFO records
				 * provide the highest xid cleaned by the vacuum of the heap
				 * and so we can resolve any conflicts just once when that
				 * arrives. After that any we know that no conflicts exist
				 * from individual btree vacuum records on that index.
				 */
				{
					TransactionId latestRemovedXid = btree_xlog_delete_get_latestRemovedXid(record);
					xl_btree_delete *xlrec = (xl_btree_delete *) XLogRecGetData(record);

					ResolveRecoveryConflictWithSnapshot(latestRemovedXid, xlrec->node);
				}
				break;

			case XLOG_BTREE_REUSE_PAGE:

				/*
				 * Btree reuse page records exist to provide a conflict point
				 * when we reuse pages in the index via the FSM. That's all it
				 * does though.
				 */
				{
					xl_btree_reuse_page *xlrec = (xl_btree_reuse_page *) XLogRecGetData(record);

					ResolveRecoveryConflictWithSnapshot(xlrec->latestRemovedXid, xlrec->node);
				}
				return;

			default:
				break;
		}
	}

	/*
	 * Vacuum needs to pin and take cleanup lock on every leaf page, a regular
	 * exclusive lock is enough for all other purposes.
	 */
	RestoreBkpBlocks(lsn, record, (info == XLOG_BTREE_VACUUM));

	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			btree_xlog_insert(true, false, lsn, record);
			break;
		case XLOG_BTREE_INSERT_UPPER:
			btree_xlog_insert(false, false, lsn, record);
			break;
		case XLOG_BTREE_INSERT_META:
			btree_xlog_insert(false, true, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_L:
			btree_xlog_split(true, false, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_R:
			btree_xlog_split(false, false, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_L_ROOT:
			btree_xlog_split(true, true, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_R_ROOT:
			btree_xlog_split(false, true, lsn, record);
			break;
		case XLOG_BTREE_VACUUM:
			btree_xlog_vacuum(lsn, record);
			break;
		case XLOG_BTREE_DELETE:
			btree_xlog_delete(lsn, record);
			break;
		case XLOG_BTREE_DELETE_PAGE:
		case XLOG_BTREE_DELETE_PAGE_META:
		case XLOG_BTREE_DELETE_PAGE_HALF:
			btree_xlog_delete_page(info, lsn, record);
			break;
		case XLOG_BTREE_NEWROOT:
			btree_xlog_newroot(lsn, record);
			break;
		case XLOG_BTREE_REUSE_PAGE:
			/* Handled above before restoring bkp block */
			break;
		default:
			elog(PANIC, "btree_redo: unknown op code %u", info);
	}
}

static void
out_target(StringInfo buf, xl_btreetid *target)
{
	appendStringInfo(buf, "rel %u/%u/%u; tid %u/%u",
			 target->node.spcNode, target->node.dbNode, target->node.relNode,
					 ItemPointerGetBlockNumber(&(target->tid)),
					 ItemPointerGetOffsetNumber(&(target->tid)));
}

/*
 * Print additional information about an INSERT record.
 */
static void
out_insert(StringInfo buf, bool isleaf, bool ismeta, XLogRecord *record)
{
	char			*rec = XLogRecGetData(record);
	xl_btree_insert *xlrec = (xl_btree_insert *) rec;

	char	   *datapos;
	int			datalen;
	xl_btree_metadata md = { InvalidBlockNumber, 0, InvalidBlockNumber, 0 };
	BlockNumber downlink = 0;

	datapos = (char *) xlrec + SizeOfBtreeInsert;
	datalen = record->xl_len - SizeOfBtreeInsert;
	if (!isleaf)
	{
		memcpy(&downlink, datapos, sizeof(BlockNumber));
		datapos += sizeof(BlockNumber);
		datalen -= sizeof(BlockNumber);
	}
	if (ismeta)
	{
		memcpy(&md, datapos, sizeof(xl_btree_metadata));
		datapos += sizeof(xl_btree_metadata);
		datalen -= sizeof(xl_btree_metadata);
	}

	if ((IsBkpBlockApplied(record, 0)) && !ismeta && isleaf)
	{
		appendStringInfo(buf, "; page %u",
						 ItemPointerGetBlockNumber(&(xlrec->target.tid)));
		return;					/* nothing to do */
	}

	if (!(IsBkpBlockApplied(record, 0)))
	{
		appendStringInfo(buf, "; add length %d item at offset %d in page %u",
						 datalen, 
						 ItemPointerGetOffsetNumber(&(xlrec->target.tid)),
						 ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	}

	if (ismeta)
		appendStringInfo(buf, "; restore metadata page 0 (root page value %u, level %d, fastroot page value %u, fastlevel %d)",
						 md.root, 
						 md.level,
						 md.fastroot, 
						 md.fastlevel);

	/* Forget any split this insertion completes */
//	if (!isleaf)
//		appendStringInfo(buf, "; completes split for page %u",
//		 				 downlink);
}

/*
 * Print additional information about a DELETE record.
 */
static void
out_delete(StringInfo buf, XLogRecord *record)
{
	char			*rec = XLogRecGetData(record);
	xl_btree_delete *xlrec = (xl_btree_delete *) rec;

	if (IsBkpBlockApplied(record, 0))
		return;

	xlrec = (xl_btree_delete *) XLogRecGetData(record);

	if (record->xl_len > SizeOfBtreeDelete)
	{
		OffsetNumber *unused;
		OffsetNumber *unend;

		unused = (OffsetNumber *) ((char *) xlrec + SizeOfBtreeDelete);
		unend = (OffsetNumber *) ((char *) xlrec + record->xl_len);

		appendStringInfo(buf, "; page index (unend - unused = %u)",
						 (unsigned int)(unend - unused));
	}
}

/*
 * Print additional information about a DELETE_PAGE record.
 */
static void
out_delete_page(StringInfo buf, uint8 info, XLogRecord *record)
{
	char					*rec = XLogRecGetData(record);
	xl_btree_delete_page 	*xlrec = (xl_btree_delete_page *) rec;

	/* Update metapage if needed */
	if (info == XLOG_BTREE_DELETE_PAGE_META)
	{
		xl_btree_metadata md;

		memcpy(&md, (char *) xlrec + SizeOfBtreeDeletePage,
			   sizeof(xl_btree_metadata));
		appendStringInfo(buf, "; update metadata page 0 (root page value %u, level %d, fastroot page value %u, fastlevel %d)",
						 md.root, 
						 md.level,
						 md.fastroot, 
						 md.fastlevel);
	}
}

void
btree_desc(StringInfo buf, XLogRecPtr beginLoc, XLogRecord *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		xl_info = record->xl_info;
	uint8		info = xl_info & ~XLR_INFO_MASK;

	/*
	 * To get the extra information about the 2nd and 3rd blocks afftected by redo,
	 * we model the call hierarchy here after the btree_redo routine.
	 */
	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			{
				xl_btree_insert *xlrec = (xl_btree_insert *) rec;

				appendStringInfo(buf, "insert: ");
				out_target(buf, &(xlrec->target));
				out_insert(buf, /* isleaf */ true, /* ismeta */ false, record);
				break;
			}
		case XLOG_BTREE_INSERT_UPPER:
			{
				xl_btree_insert *xlrec = (xl_btree_insert *) rec;

				appendStringInfo(buf, "insert_upper: ");
				out_target(buf, &(xlrec->target));
				out_insert(buf, /* isleaf */ false, /* ismeta */ false, record);
				break;
			}
		case XLOG_BTREE_INSERT_META:
			{
				xl_btree_insert *xlrec = (xl_btree_insert *) rec;

				appendStringInfo(buf, "insert_meta: ");
				out_target(buf, &(xlrec->target));
				out_insert(buf, /* isleaf */ false, /* ismeta */ true, record);
				break;
			}
		case XLOG_BTREE_SPLIT_L:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				appendStringInfo(buf, "split_l: rel %u/%u/%u ",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode);
				appendStringInfo(buf, "left %u, right %u, next %u, level %u, firstright %d",
							     xlrec->leftsib, xlrec->rightsib, xlrec->rnext,
								 xlrec->level, xlrec->firstright);
				break;
			}
		case XLOG_BTREE_SPLIT_R:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				appendStringInfo(buf, "split_r: rel %u/%u/%u ",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode);
				appendStringInfo(buf, "left %u, right %u, next %u, level %u, firstright %d",
								 xlrec->leftsib, xlrec->rightsib, xlrec->rnext,
								 xlrec->level, xlrec->firstright);
				break;
			}
		case XLOG_BTREE_SPLIT_L_ROOT:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				appendStringInfo(buf, "split_l_root: rel %u/%u/%u ",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode);
				appendStringInfo(buf, "left %u, right %u, next %u, level %u, firstright %d",
								 xlrec->leftsib, xlrec->rightsib, xlrec->rnext,
								 xlrec->level, xlrec->firstright);
				break;
			}
		case XLOG_BTREE_SPLIT_R_ROOT:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				appendStringInfo(buf, "split_r_root: rel %u/%u/%u ",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode);
				appendStringInfo(buf, "left %u, right %u, next %u, level %u, firstright %d",
								 xlrec->leftsib, xlrec->rightsib, xlrec->rnext,
								 xlrec->level, xlrec->firstright);
				break;
			}
		case XLOG_BTREE_VACUUM:
			{
				xl_btree_vacuum *xlrec = (xl_btree_vacuum *) rec;

				appendStringInfo(buf, "vacuum: rel %u/%u/%u; blk %u, lastBlockVacuumed %u",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode, xlrec->block,
								 xlrec->lastBlockVacuumed);
				break;
			}
		case XLOG_BTREE_DELETE:
			{
				xl_btree_delete *xlrec = (xl_btree_delete *) rec;

<<<<<<< HEAD
				appendStringInfo(buf, "delete: rel %u/%u/%u; blk %u",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode, xlrec->block);
				out_delete(buf, record);
=======
				appendStringInfo(buf, "delete: index %u/%u/%u; iblk %u, heap %u/%u/%u;",
				xlrec->node.spcNode, xlrec->node.dbNode, xlrec->node.relNode,
								 xlrec->block,
								 xlrec->hnode.spcNode, xlrec->hnode.dbNode, xlrec->hnode.relNode);
>>>>>>> 1084f317702e1a039696ab8a37caf900e55ec8f2
				break;
			}
		case XLOG_BTREE_DELETE_PAGE:
		case XLOG_BTREE_DELETE_PAGE_META:
		case XLOG_BTREE_DELETE_PAGE_HALF:
			{
				xl_btree_delete_page *xlrec = (xl_btree_delete_page *) rec;

				appendStringInfo(buf, "delete_page: ");
				out_target(buf, &(xlrec->target));
				appendStringInfo(buf, "; dead %u; left %u; right %u",
								 xlrec->deadblk, xlrec->leftblk, xlrec->rightblk);
				out_delete_page(buf, info, record);
				break;
			}
		case XLOG_BTREE_NEWROOT:
			{
				xl_btree_newroot *xlrec = (xl_btree_newroot *) rec;

				appendStringInfo(buf, "newroot: rel %u/%u/%u; root %u lev %u",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode,
								 xlrec->rootblk, xlrec->level);
				break;
			}
		case XLOG_BTREE_REUSE_PAGE:
			{
				xl_btree_reuse_page *xlrec = (xl_btree_reuse_page *) rec;

				appendStringInfo(buf, "reuse_page: rel %u/%u/%u; latestRemovedXid %u",
								 xlrec->node.spcNode, xlrec->node.dbNode,
							   xlrec->node.relNode, xlrec->latestRemovedXid);
				break;
			}
		default:
			appendStringInfo(buf, "UNKNOWN");
			break;
	}
}

void
btree_xlog_startup(void)
{
	incomplete_actions = NIL;
}

void
btree_xlog_cleanup(void)
{
	ListCell   *l;

	foreach(l, incomplete_actions)
	{
		bt_incomplete_action *action = (bt_incomplete_action *) lfirst(l);

		if (action->is_split)
		{
			/* finish an incomplete split */
			Buffer		lbuf,
						rbuf;
			Page		lpage,
						rpage;
			BTPageOpaque lpageop,
						rpageop;
			bool		is_only;
			Relation	reln;

			lbuf = XLogReadBuffer(action->node, action->leftblk, false);
			/* failure is impossible because we wrote this page earlier */
			if (!BufferIsValid(lbuf))
				elog(PANIC, "btree_xlog_cleanup: left block unfound");
			lpage = (Page) BufferGetPage(lbuf);
			lpageop = (BTPageOpaque) PageGetSpecialPointer(lpage);
			rbuf = XLogReadBuffer(action->node, action->rightblk, false);
			/* failure is impossible because we wrote this page earlier */
			if (!BufferIsValid(rbuf))
				elog(PANIC, "btree_xlog_cleanup: right block unfound");
			rpage = (Page) BufferGetPage(rbuf);
			rpageop = (BTPageOpaque) PageGetSpecialPointer(rpage);

			/* if the pages are all of their level, it's a only-page split */
			is_only = P_LEFTMOST(lpageop) && P_RIGHTMOST(rpageop);

			reln = CreateFakeRelcacheEntry(action->node);
			_bt_insert_parent(reln, lbuf, rbuf, NULL,
							  action->is_root, is_only);
			FreeFakeRelcacheEntry(reln);
		}
		else
		{
			/* finish an incomplete deletion (of a half-dead page) */
			Buffer		buf;

			buf = XLogReadBuffer(action->node, action->delblk, false);
			if (BufferIsValid(buf))
			{
				Relation	reln;

				reln = CreateFakeRelcacheEntry(action->node);
				if (_bt_pagedel(reln, buf, NULL) == 0)
					elog(PANIC, "btree_xlog_cleanup: _bt_pagedel failed");
				FreeFakeRelcacheEntry(reln);
			}
		}
	}
	incomplete_actions = NIL;
}

bool
btree_safe_restartpoint(void)
{
	if (incomplete_actions)
		return false;
	return true;
}

/*
 * Mask a btree page before performing consistency checks on it.
 */
void
btree_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;
	BTPageOpaque maskopaq;

	mask_page_lsn_and_checksum(page);

	mask_page_hint_bits(page);
	mask_unused_space(page);

	maskopaq = (BTPageOpaque) PageGetSpecialPointer(page);

	if (P_ISDELETED(maskopaq))
	{
		/*
		 * Mask page content on a DELETED page since it will be re-initialized
		 * during replay. See btree_xlog_unlink_page() for details.
		 */
		mask_page_content(page);
	}
	else if (P_ISLEAF(maskopaq))
	{
		/*
		 * In btree leaf pages, it is possible to modify the LP_FLAGS without
		 * emitting any WAL record. Hence, mask the line pointer flags. See
		 * _bt_killitems(), _bt_check_unique() for details.
		 */
		mask_lp_flags(page);
	}

	/*
	 * BTP_HAS_GARBAGE is just an un-logged hint bit. So, mask it. See
	 * _bt_killitems(), _bt_check_unique() for details.
	 */
	maskopaq->btpo_flags &= ~BTP_HAS_GARBAGE;

	/*
	 * During replay of a btree page split, we don't set the BTP_SPLIT_END
	 * flag of the right sibling and initialize the cycle_id to 0 for the same
	 * page. See btree_xlog_split() for details.
	 */
	maskopaq->btpo_flags &= ~BTP_SPLIT_END;
	maskopaq->btpo_cycleid = 0;
}
