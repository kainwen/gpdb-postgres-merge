/*-------------------------------------------------------------------------
 *
 * itemptr.c
 *	  POSTGRES disk item pointer code.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/page/itemptr.c,v 1.23 2010/01/02 16:57:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/itemptr.h"


/*
 * ItemPointerEquals
 *	Returns true if both item pointers point to the same item,
 *	 otherwise returns false.
 *
 * Note:
 *	Asserts that the disk item pointers are both valid!
 */
bool
ItemPointerEquals(ItemPointer pointer1, ItemPointer pointer2)
{
	if (ItemPointerGetBlockNumber(pointer1) ==
		ItemPointerGetBlockNumber(pointer2) &&
		ItemPointerGetOffsetNumber(pointer1) ==
		ItemPointerGetOffsetNumber(pointer2))
		return true;
	else
		return false;
}

/*
 * ItemPointerCompare
 *		Generic btree-style comparison for item pointers.
 */
int32
ItemPointerCompare(ItemPointer arg1, ItemPointer arg2)
{
	/*
	 * Don't use ItemPointerGetBlockNumber or ItemPointerGetOffsetNumber here,
	 * because they assert ip_posid != 0 which might not be true for a
	 * user-supplied TID.
	 */
	BlockNumber b1 = BlockIdGetBlockNumber(&(arg1->ip_blkid));
	BlockNumber b2 = BlockIdGetBlockNumber(&(arg2->ip_blkid));

	if (b1 < b2)
		return -1;
	else if (b1 > b2)
		return 1;
	else if (arg1->ip_posid < arg2->ip_posid)
		return -1;
	else if (arg1->ip_posid > arg2->ip_posid)
		return 1;
	else
		return 0;
}

static char *
ItemPointerToBuffer(char *buffer, ItemPointer tid)
{
	// Do not assert valid ItemPointer -- it is ok if it is (0,0)...
	BlockNumber blockNumber = BlockIdGetBlockNumber(&tid->ip_blkid);
	OffsetNumber offsetNumber = tid->ip_posid;
	
	sprintf(buffer,
		    "(%u,%u)",
		    blockNumber, 
		    offsetNumber);

	return buffer;
}

static char itemPointerBuffer[50];
static char itemPointerBuffer2[50];

char *
ItemPointerToString(ItemPointer tid)
{
	return ItemPointerToBuffer(itemPointerBuffer, tid);
}

char *
ItemPointerToString2(ItemPointer tid)
{
	return ItemPointerToBuffer(itemPointerBuffer2, tid);
}

