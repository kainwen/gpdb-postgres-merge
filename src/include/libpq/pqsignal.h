/*-------------------------------------------------------------------------
 *
 * pqsignal.h
 *	  prototypes for the reliable BSD-style signal(2) routine.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/libpq/pqsignal.h,v 1.41 2010/02/26 02:01:24 momjian Exp $
 *
 * NOTES
 *	  This shouldn't be in libpq, but the monitor and some other
 *	  things need it...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQSIGNAL_H
#define PQSIGNAL_H

#include <signal.h>

#ifdef HAVE_SIGPROCMASK
extern sigset_t UnBlockSig,
			BlockSig,
			StartupBlockSig;

/* 
 * Use pthread_sigmask() instead of sigprocmask() as the latter has undefined
 * behaviour in multithreaded processes.
 */
#define PG_SETMASK(mask)	pthread_sigmask(SIG_SETMASK, mask, NULL)
#else							/* not HAVE_SIGPROCMASK */
extern int	UnBlockSig,
			BlockSig,
			StartupBlockSig;

#ifndef WIN32
#define PG_SETMASK(mask)	sigsetmask(*((int*)(mask)))
#else
#define PG_SETMASK(mask)		pqsigsetmask(*((int*)(mask)))
int			pqsigsetmask(int mask);
#endif

#define sigaddset(set, signum)	(*(set) |= (sigmask(signum)))
#define sigdelset(set, signum)	(*(set) &= ~(sigmask(signum)))
#endif   /* not HAVE_SIGPROCMASK */

typedef void (*pqsigfunc) (int);

extern void pqinitmask(void);

extern pqsigfunc pqsignal(int signo, pqsigfunc func);

#endif   /* PQSIGNAL_H */
