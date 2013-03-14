/*
Copyright (C) 2012
Fabien Gaud <fgaud@sfu.ca>, Baptiste Lepers <baptiste.lepers@inria.fr>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef PROFILER_H_
#define PROFILER_H_

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <sched.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/syscall.h>
#define __EXPORTED_HEADERS__
#include <sys/sysinfo.h>
#undef __EXPORTED_HEADERS__
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <dirent.h>

#define TIME_SECOND             1000000
#define PAGE_SIZE               (4*1024)

#define die(msg, args...) \
do {                         \
            fprintf(stderr,"(%s,%d) " msg "\n", __FUNCTION__ , __LINE__, ##args); \
            exit(-1);                 \
         } while(0)

#define thread_die(msg, args...) \
do {                         \
            fprintf(stderr,"(%s,%d) " msg "\n", __FUNCTION__ , __LINE__, ##args); \
            pthread_exit(NULL); \
         } while(0)

typedef struct _event {
   /* Useless in the current version (we only monitor "raw" events) */
   uint64_t type;
   /* Value describing the chosen event and unitmask (see details below). */
   uint64_t config;
   /* Boolean indicating if kernel-level events must be monitored */
   uint64_t exclude_kernel;
   /* Boolean indicating if user-level events must be monitored */
   uint64_t exclude_user;

   const char* name;
   char per_node;

   /* Id of the MSR control register that will be used to monitor the event */
   uint64_t msr_select; /* msr that will be used to monitor the event */
   /* Id of the MSR counter register that will be used to monitor the event */
   uint64_t msr_value; /* msr that will be used to monitor the event */
} event_t;

/*
 ***Additional details about event IDs for AMD 10h and 15h architectures***
 * 
 * The "config" field in a "event_t" struct must be set with values using the
 * following scheme: 0xz0040yyzz
 * (for details, see PERF_CTL description in AMD BKDG)
 *
 *  'z-zz' values: EventSelect 
 *  'yy': UnitMask
 *  '4': enable performance counter
 *       (not strictly required because forced by miniprof anyways)
 *
 *  Examples for AMD 15h architectures (BKDG 15h section 3.15):
 *     1) CPU clocks not halted (per-core event):
 *             z-zz: 0x076
 *             yy: 0x00 (no unitmask)
 *     2) L2 cache misses (per core event) with unitmask selecting
 *          "DC fill" + "TLB page table walk":
 *             z-zz: 0x07E
 *             yy: 0x03
 *     3) Tagged IBS Ops (per core event) with unitmask selecting
 *           "Number of ops tagged by IBS that retired"
 *             z-zz: 0x1CF
 *             yy: 0x01
 *     4) DRAM accesses (per-node event) with unitmask selecting
 *          "DCT1 Page hit":
 *             z-zz: 0x0E0
 *             yy: 0x08
 */


#ifdef __x86_64__
#define rdtscll(val) {                                           \
    unsigned int __a,__d;                                        \
    asm volatile("rdtsc" : "=a" (__a), "=d" (__d));              \
    (val) = ((unsigned long)__a) | (((unsigned long)__d)<<32);   \
}
#else
#define rdtscll(val) __asm__ __volatile__("rdtsc" : "=A" (val))
#endif

typedef struct pdata {
   int core;
   int nb_events;
   event_t *events;
   int tid; /* Tid to observe (useless in current version?) */
} pdata_t;

#endif /* PROFILER_H_ */
