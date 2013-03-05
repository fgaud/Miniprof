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

#include "miniprof.h"
#include <numa.h>

#include <sched.h>
#include <linux/unistd.h>
#include <sys/resource.h>


int ncpus;
int nnodes;

int * cores_monitoring_node_events;

static int sleep_time = 1 * TIME_SECOND;

/*
 * Events :
 * - PERF_TYPE_RAW: raw counters. The value must be 0xz0040yyzz.
 *      For 'z-zz' values, see AMD reference manual (eg. 076h = CPU_CLK_UNHALTED).
 *      'yy' is the Unitmask.
 *      The '4' is 0100b = event is enabled (can also be enable/disabled via ioctl).
 *      The '0' before yy indicate which level to monitor (User or OS).
 *              It is modified by the event_attr when .exclude_[user/kernel] == 0.
 *              When it is the case the bits 16 or 17 of .config are set to 1 (== monitor user/kernel).
 *              If this is set to anything else than '0', it can be confusing since the kernel does not modify it when .exclude_xxx is set.
 *
 * - PERF_TYPE_HARDWARE: predefined values of HW counters in Linux (eg PERF_COUNT_HW_CPU_CYCLES = CPU_CLK_UNHALTED).
 */
static event_t default_events[] = { { .name = "CLK_UNHALTED", .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_CPU_CYCLES, }, };

static int nb_events = sizeof(default_events) / sizeof(*default_events);
static event_t *events = default_events;
static int nb_observed_pids;
static int *observed_pids;

static long sys_perf_counter_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags);
static uint64_t hex2u64(const char *ptr);
static void sig_handler(int signal);

static int with_fake_threads = 0;

uint64_t get_cpu_freq(void) {
   FILE *fd;
   uint64_t freq = 0;
   float freqf = 0;
   char *line = NULL;
   size_t len = 0;

   fd = fopen("/proc/cpuinfo", "r");
   if (!fd) {
      fprintf(stderr, "failed to get cpu frequency\n");
      perror(NULL);
      return freq;
   }

   while (getline(&line, &len, fd) != EOF) {
      if (sscanf(line, "cpu MHz\t: %f", &freqf) == 1) {
         freqf = freqf * 1000000UL;
         freq = (uint64_t) freqf;
         break;
      }
   }

   fclose(fd);
   return freq;
}

void add_tid(int tid) {
   observed_pids = realloc(observed_pids, (nb_observed_pids + 1) * sizeof(*observed_pids));
   observed_pids[nb_observed_pids] = tid;
   nb_observed_pids++;
}

int get_tids_of_app(char *app) {
   int nb_tids_found = 0, pid;

   char buffer[1024];
   FILE *procs = popen("ps -A -L -o lwp= -o comm=", "r");
   while (fscanf(procs, "%d %s\n", &pid, buffer) == 2) {
      if (!strcmp(buffer, app)) {
         printf("#Matching pid: %d (%s)\n", (int) pid, buffer);
         nb_tids_found++;
         add_tid(pid);
      }
   }
   fclose(procs);

   return nb_tids_found;
}

static pid_t gettid(void) {
   return syscall(__NR_gettid);
}

void set_affinity(int tid, int core_id) {
   cpu_set_t mask;
   CPU_ZERO(&mask);
   CPU_SET(core_id, &mask);

   int r = sched_setaffinity(tid, sizeof(mask), &mask);
   if (r < 0) {
      fprintf(stderr, "couldn't set affinity for %d\n", core_id);
      exit(1);
   }
}

/** Run a low priority thread on each profiled core **/
__attribute__((optimize("O0"))) static void* spin_loop(void *pdata) {
   pdata_t *data = (pdata_t*) pdata;

   pid_t tid = gettid();
   set_affinity(tid, data->core);
   if(setpriority(PRIO_PROCESS, tid, 20)){
      perror("Error while setting priority");
   }

   while(1);
   return NULL;
}

static void* thread_loop(void *pdata) {
   int i, watch_tid;
   pdata_t *data = (pdata_t*) pdata;
   watch_tid = (data->tid != 0);

   int monitor_node_events = 0;
   for (i = 0; i < nnodes; i++) {
      if (cores_monitoring_node_events[i] == data->core)
         monitor_node_events = 1;
   }

   if (!watch_tid) {
      set_affinity(gettid(), data->core);
   }

   struct perf_event_attr *events_attr = calloc(data->nb_events * sizeof(*events_attr), 1);
   assert(events_attr != NULL);
   data->fd = malloc(data->nb_events * sizeof(*data->fd));
   assert(data->fd);

   for (i = 0; i < data->nb_events; i++) {
      if (data->events[i].per_node && !monitor_node_events) {
         // IGNORE THIS EVENT
         continue;
      }

      events_attr[i].size = sizeof(struct perf_event_attr);
      events_attr[i].type = data->events[i].type;
      events_attr[i].config = data->events[i].config;
      events_attr[i].exclude_kernel = data->events[i].exclude_kernel;
      events_attr[i].exclude_user = data->events[i].exclude_user;

      events_attr[i].read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

      data->fd[i] = sys_perf_counter_open(&events_attr[i], watch_tid ? data->tid : -1, watch_tid ? -1 : data->core, -1, 0);
      if (data->fd[i] < 0) {
         thread_die("#[%d] sys_perf_counter_open failed: %s", watch_tid ? data->tid : data->core, strerror(errno));
      }
   }

   struct perf_read_ev *last_counts = calloc(data->nb_events, sizeof(struct perf_read_ev));
   int logical_time = 0;
   while (1) {
      struct perf_read_ev single_count;
      uint64_t rdtsc;

      logical_time++;

      rdtscll(rdtsc);
      for (i = 0; i < data->nb_events; i++) {
         if (data->events[i].per_node && !monitor_node_events) {
            // IGNORE THIS EVENT
            continue;
         }

         assert(read(data->fd[i], &single_count, sizeof(single_count)) == sizeof(single_count));

         uint64_t value = single_count.value - last_counts[i].value;
         uint64_t time_running = single_count.time_running - last_counts[i].time_running;
         uint64_t time_enabled = single_count.time_enabled - last_counts[i].time_enabled;
         double percent_running = (double) time_running / (double) time_enabled;


         printf("%d\t%d\t%llu\t%llu\t%.3f\t%d\n", i, watch_tid ? data->tid : data->core, (long long unsigned) rdtsc, (long long unsigned) value, percent_running, logical_time);
         last_counts[i] = single_count;
      }

      usleep(sleep_time);
   }

   return NULL;
}

void usage (char ** argv) {
   int i;
   printf("Usage: %s [-e NAME COUNTER EXCLUDE_KERNEL EXCLUDE_USER PER_NODE] [-s NAME COUNTER  EXCLUDE_KERNEL EXCLUDE_USER] [-t TID] [-a APP_NAME] [-ft] [-h]\n", argv[0]);
   printf("-e: hardware events\n");
   printf("\tNAME: You can give any name to the counter\n");
   printf("\tCOUNTER: Same format as raw perf events, except that it starts by 0x istead of r\n");
   printf("\tEXCLUDE_KERNEL: Do not include kernel-level samples\n");
   printf("\tEXCLUDE_USER: Do not include user-level samples\n");
   printf("\tPER_NODE: Is this counter an off-core counter (will be monitored on a single core per NUMA node) ?\n\n");

   printf("-s: software events\n");
   printf("\tCOUNTER: Must be a software event. Supported events are:\n");
   for (i = 0; i < PERF_COUNT_SW_MAX; i++) {
      printf("\t\t%s\n", event_symbols_sw[i].symbol); 
   } 
   printf("\tEXCLUDE_KERNEL: Do not include kernel-level samples\n");
   printf("\tEXCLUDE_USER: Do not include user-level samples\n\n");

   printf("-t\n");
   printf("\tTID: do a per-tid profiling instead of a per-core profiling and consider this TID\n\n");
   
   printf("-a\n");
   printf("\tAPP_NAME: same as -t but with the application name\n");
}

void parse_options(int argc, char **argv) {
   int i = 1;
   event_t *evts = NULL;
   int nb_evts = 0;
   for (;;) {
      if (i >= argc)
         break;
      if (!strcmp(argv[i], "-e")) {
         if (i + 5 >= argc)
            die("Missing argument for -e NAME COUNTER EXCLUDE_KERNEL EXCLUDE_USER PER_NODE\n");
         evts = realloc(evts, (nb_evts + 1) * sizeof(*evts));
         evts[nb_evts].name = strdup(argv[i + 1]);
         evts[nb_evts].type = PERF_TYPE_RAW;
         evts[nb_evts].config = hex2u64(argv[i + 2]);
         evts[nb_evts].exclude_kernel = atoi(argv[i + 3]);
         evts[nb_evts].exclude_user = atoi(argv[i + 4]);
         evts[nb_evts].exclude_user = atoi(argv[i + 4]);
         evts[nb_evts].per_node = atoi(argv[i + 5]);
         nb_evts++;

         i += 6;
      }
      if (!strcmp(argv[i], "-s")) {
         int j;

         if (i + 3 >= argc)
            die("Missing argument for -e COUNTER EXCLUDE_KERNEL EXCLUDE_USER\n");
         evts = realloc(evts, (nb_evts + 1) * sizeof(*evts));
         evts[nb_evts].name = strdup(argv[i + 1]);
         evts[nb_evts].type = PERF_TYPE_SOFTWARE;

         // Looking for the event number
         for (j = 0; j < PERF_COUNT_SW_MAX; j++) {
            if(! strcmp(event_symbols_sw[j].symbol, argv[i + 1]))
               break;
         } 

         if(j == PERF_COUNT_SW_MAX) {
            usage(argv);
            printf("\n%s is not a valid software event\n", argv[i + 1]);
            exit(1);
         }
        
         evts[nb_evts].config = j;
         evts[nb_evts].exclude_kernel = atoi(argv[i + 2]);
         evts[nb_evts].exclude_user = atoi(argv[i + 3]);
         nb_evts++;

         i += 4;
      }

      else if (!strcmp(argv[i], "-t")) {
         if (i + 1 >= argc)
            die("Missing argument for -t TID\n");
         add_tid(atoi(argv[i + 1]));
         printf("#Matching pid: %d (user_provided)\n", atoi(argv[i + 1]));
         i += 2;
      }
      else if (!strcmp(argv[i], "-a")) {
         if (i + 1 >= argc)
            die("Missing argument for -a APPLICATION\n");
         get_tids_of_app(argv[i + 1]);
         i += 2;
      }
      else if (!strcmp(argv[i], "-ft")) {
         with_fake_threads = 1;
         printf("#WARNING: with fake threads\n");
         i++;
      }
      else if (!strcmp(argv[i], "-h")) {
         usage(argv);
         exit(0);
      }
      else {
         usage(argv);
         printf("After usage\n");
         die("Unknown option %s\n", argv[i]);
      }
   }

   if (nb_evts != 0) {
      nb_events = nb_evts;
      events = evts;
   }
}

int main(int argc, char**argv) {
   int i;
   
   signal(SIGPIPE, sig_handler);
   signal(SIGTERM, sig_handler);
   signal(SIGINT, sig_handler);

   /* Fill important informations */
   ncpus = get_nprocs();
   nnodes = numa_num_configured_nodes();

   printf("#NB cpus :\t%d\n", ncpus);
   printf("#NB nodes :\t%d\n", nnodes);

   cores_monitoring_node_events = (int*) calloc(ncpus, sizeof(int));

   for (i = 0; i < nnodes; i++) {
      struct bitmask * bm = numa_allocate_cpumask();
      numa_node_to_cpus(i, bm);

      printf("#Node %d :\t", i);
      int j = 0;
      int set = 0;
      for (j = 0; j < ncpus; j++) {
         if (numa_bitmask_isbitset(bm, j)) {
            if (!set) {
               cores_monitoring_node_events[i] = j;
               set = 1;
            }
            printf("%d ", j);
         }
      }
      printf("\n");
      numa_free_cpumask(bm);
   }

   parse_options(argc, argv);
   uint64_t clk_speed = get_cpu_freq();

   printf("#Clock speed: %llu\n", (long long unsigned) clk_speed);
   for (i = 0; i < nb_events; i++) {
      printf("#Event %d: %s (%llx) (Exclude Kernel: %s; Exclude User: %s, Per node: %s)\n", i, events[i].name, (long long unsigned) events[i].config, (events[i].exclude_kernel) ? "yes" : "no",
            (events[i].exclude_user) ? "yes" : "no", (events[i].per_node) ? "yes" : "no");
   }

   int nb_threads = nb_observed_pids ? nb_observed_pids : ncpus;
   if (nb_observed_pids)
      printf("#Event\tTID\tTime\t\t\tSamples\t%% time enabled\tlogical time\n");
   else
      printf("#Event\tCore\tTime\t\t\tSamples\t%% time enabled\tlogical time\n");

   pthread_t threads[nb_threads];
   for (i = 0; i < nb_threads; i++) {
      pdata_t *data = calloc(1, sizeof(*data));
      if (nb_observed_pids > 0)
         data->tid = observed_pids[i];
      else
         data->core = i;

      data->nb_events = nb_events;
      data->events = events;

      if(with_fake_threads) {
         pthread_create(&threads[i], NULL, spin_loop, data);
      }

      if (i != nb_threads - 1) {
         pthread_create(&threads[i], NULL, thread_loop, data);
      }
      else {
         thread_loop(data);
      }
   }

   for (i = 0; i < nb_threads - 1; i++) {
      pthread_join(threads[i], NULL);
   }

   printf("#END??\n");
   return 0;
}

static long sys_perf_counter_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags) {
   int ret = syscall(__NR_perf_counter_open, hw_event, pid, cpu, group_fd, flags);
#  if defined(__x86_64__) || defined(__i386__)
   if (ret < 0 && ret > -4096) {
      errno = -ret;
      ret = -1;
   }
#  endif
   return ret;
}

static void sig_handler(int signal) {
   printf("#signal caught: %d\n", signal);
   fflush(NULL);
   exit(0);
}

static int hex(char ch) {
   if ((ch >= '0') && (ch <= '9'))
      return ch - '0';
   if ((ch >= 'a') && (ch <= 'f'))
      return ch - 'a' + 10;
   if ((ch >= 'A') && (ch <= 'F'))
      return ch - 'A' + 10;
   return -1;
}

static uint64_t hex2u64(const char *ptr) {
   const char *p = ptr;
   uint64_t long_val = 0;

   if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X'))
      die("Wrong format for counter. Expected 0xXXXXXX\n");
   p += 2;

   while (*p) {
      const int hex_val = hex(*p);
      if (hex_val < 0)
         break;

      long_val = (long_val << 4) | hex_val;
      p++;
   }
   return long_val;
}
