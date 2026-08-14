#ifndef _NUVIX_WORKER_H
#define _NUVIX_WORKER_H

/*
 * include/nuvix/worker.h - small kernel worker helpers
 */

void worker_run_periodic(unsigned int interval_sec, void (*work)(void *),
			 void *arg);

#endif
