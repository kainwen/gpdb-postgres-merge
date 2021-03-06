/*-------------------------------------------------------------------------
 *
 * version.c
 *	 Returns the PostgreSQL version string
 *
 * Copyright (c) 1998-2010, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *
 * $PostgreSQL: pgsql/src/backend/utils/adt/version.c,v 1.19 2010/01/02 16:57:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/builtins.h"


Datum
pgsql_version(PG_FUNCTION_ARGS __attribute__((unused)) )
{
	char version[512];

	strcpy(version, PG_VERSION_STR " compiled on " __DATE__ " " __TIME__);
	
#ifdef USE_ASSERT_CHECKING
	strcat(version, " (with assert checking)");
#endif 

	PG_RETURN_TEXT_P(cstring_to_text(version));
}
