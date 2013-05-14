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

int ncpus;
int nnodes;

/* points to an array indicating which core is in charge
   of monitoring the per-node events of a given node */
int * cores_monitoring_node_events;

/* sampling period (time interval between two dumps of the performance counters) */
static int sleep_time = 1000 * TIME_MSECOND;

static event_t *events = NULL;
static int nb_events = 0;

static long sys_perf_counter_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags);

static uint64_t hex2u64(const char *ptr);
static void sig_handler(int signal);
static int wrmsr(int cpu, uint32_t msr, uint64_t val);
static uint64_t rdmsr(int cpu, uint32_t msr);
static void stop_all_pmu(void);

static int with_fake_threads = 0;


// This code is directly imported from <linux_src>/tools/perf/util/parse-events.c
struct event_symbol {
   const char  *symbol;                                                                                                                                                                                       
};

static struct event_symbol event_symbols_sw[PERF_COUNT_SW_MAX] = {
   [PERF_COUNT_SW_CPU_CLOCK] = {
      .symbol = "cpu-clock",
   },
   [PERF_COUNT_SW_TASK_CLOCK] = {
      .symbol = "task-clock",
   },
   [PERF_COUNT_SW_PAGE_FAULTS] = {
      .symbol = "page-faults",
   },
   [PERF_COUNT_SW_CONTEXT_SWITCHES] = {
      .symbol = "context-switches",
   },
   [PERF_COUNT_SW_CPU_MIGRATIONS] = {
      .symbol = "cpu-migrations",
   },
   [PERF_COUNT_SW_PAGE_FAULTS_MIN] = {
      .symbol = "minor-faults",
   },
   [PERF_COUNT_SW_PAGE_FAULTS_MAJ] = {
      .symbol = "major-faults",
   },
   [PERF_COUNT_SW_ALIGNMENT_FAULTS] = {
      .symbol = "alignment-faults",
   },
   [PERF_COUNT_SW_EMULATION_FAULTS] = {
      .symbol = "emulation-faults",
   },                                                                                                                                                            
};



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

/* 
 * Routine executed by the low priority threads created
 * by the -ft (fake thread) option.
 * This is used to make sure that a core is never halted,
 * in order to avoid bugs/inconsistencies with the performance counters.
 */
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

/*
 * Routine executed by the miniprof threads in order to periodically dump
 * the state of the performance counters.
 *
 * Note that the same per-core (resp. per-node) counters are monitored on
 * all cores (resp. nodes).
 */
static void* thread_loop(void *pdata) {
   int i;
   uint64_t event_mask;
   pdata_t *data = (pdata_t*) pdata;

   int monitor_node_events = 0;
   for (i = 0; i < nnodes; i++) {
      if (cores_monitoring_node_events[i] == data->core)
         monitor_node_events = 1;
   }

   set_affinity(gettid(), data->core);

   for (i = 0; i < nb_events; i++) {
      if (events[i].per_node && !monitor_node_events) 
         continue;
      if (events[i].cpu_filter != -1 && data->core != events[i].cpu_filter) 
         continue;

      if(events[i].type == PERF_TYPE_RAW) {
         event_mask = events[i].config;
         event_mask |= 0x530000; /* see README */
         if(events[i].exclude_kernel)
            event_mask &= ~(0x020000ll);
         if(events[i].exclude_user)   
            event_mask &= ~(0x010000ll);

         wrmsr(data->core, events[i].msr_select, event_mask);
         wrmsr(data->core, events[i].msr_value, 0);
      }
      else if (events[i].type == PERF_TYPE_SOFTWARE) {
         memset(&events[i].event_attr, 0, sizeof(struct perf_event_attr));

         events[i].event_attr.size = sizeof(struct perf_event_attr);
         events[i].event_attr.type = events[i].type;
         events[i].event_attr.config = events[i].config;
         events[i].event_attr.exclude_kernel = events[i].exclude_kernel;
         events[i].event_attr.exclude_user = events[i].exclude_user;

         events[i].event_attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

         events[i].fd = sys_perf_counter_open(&events[i].event_attr, -1, data->core, -1, 0);
         if (events[i].fd < 0) {
            thread_die("#[%d] sys_perf_counter_open failed for counter %s: %s", data->core, events[i].name, strerror(errno));
         }
      }
      else {
         thread_die("Unknown perf type: %lu\n", events[i].type);
      }
   }

   struct perf_read_ev *last_counts = calloc(nb_events, sizeof(struct perf_read_ev));
   int logical_time = 0;
   while (1) {
      struct perf_read_ev single_count;
      uint64_t rdtsc;

      logical_time++;

      rdtscll(rdtsc);
      for (i = 0; i < nb_events; i++) {
         double percent_running = 1.;
         uint64_t value;

         if (events[i].per_node && !monitor_node_events) 
            continue;
         if (events[i].cpu_filter != -1 && data->core != events[i].cpu_filter) 
            continue;

         if(events[i].type == PERF_TYPE_RAW) {
            single_count.value = rdmsr(data->core, events[i].msr_value);
         }
         else {
            assert(read(events[i].fd, &single_count, sizeof(single_count)) == sizeof(single_count));

            uint64_t time_running = single_count.time_running - last_counts[i].time_running;
            uint64_t time_enabled = single_count.time_enabled - last_counts[i].time_enabled;
            percent_running = (double) time_running / (double) time_enabled;

         }
         value = single_count.value - last_counts[i].value;
         last_counts[i] = single_count;

         printf("%d\t%d\t%llu\t%llu\t%.3f\t%d\n", i, data->core, (long long unsigned) rdtsc, (long long unsigned) value, percent_running, logical_time);
      }

      usleep(sleep_time);
   }

   return NULL;
}

void usage (char ** argv) {
   int i;

   printf("Usage: %s [-e NAME COUNTER EXCLUDE_KERNEL EXCLUDE_USERLAND CPU_FILTER] [-ft] [-h]\n", argv[0]);
   printf("-e: hardware events\n");
   printf("\tNAME: You can give any name to the counter\n");
   printf("\tCOUNTER: Same format as raw perf events, except that it starts by 0x instead of r\n");
   printf("\tEXCLUDE_KERNEL: Do not include kernel-level samples when sety\n");
   printf("\tEXCLUDE_USER: Do not include user-level samples\n");
   printf("\tCPU_FILTER: 0=monitor on all cores, 1=monitor on 1 cpu per node, -X=monitor only on cpu X\n\n");

   printf("-s: software events\n");
   printf("\tCOUNTER: Must be a software event. Supported events are:\n");
   for (i = 0; i < PERF_COUNT_SW_MAX; i++) {
      printf("\t\t%s\n", event_symbols_sw[i].symbol); 
   } 
   printf("\tEXCLUDE_KERNEL: Do not include kernel-level samples\n");
   printf("\tEXCLUDE_USER: Do not include user-level samples\n\n");

   printf("-ft: fake threads (put threads that spinloop with low priority on all cores)\n\n");
}

void parse_options(int argc, char **argv) {
   int i = 1;
   for (;;) {
      if (i >= argc)
         break;
      if (!strcmp(argv[i], "-e")) {
         if (i + 5 >= argc)
            die("Missing argument for -e NAME COUNTER EXCLUDE_KERNEL EXCLUDE_USER CPU_FILTER\n");
                
         events = realloc(events, (nb_events + 1) * sizeof(*events));
         events[nb_events].name = strdup(argv[i + 1]);
         events[nb_events].type = PERF_TYPE_RAW;
         events[nb_events].config = hex2u64(argv[i + 2]);
         events[nb_events].exclude_kernel = atoi(argv[i + 3]);
         events[nb_events].exclude_user = atoi(argv[i + 4]);
         events[nb_events].exclude_user = atoi(argv[i + 4]);
         events[nb_events].per_node = (atoi(argv[i + 5]) == 1);
         events[nb_events].cpu_filter = (*argv[i + 5] == '-')?-(atoi(argv[i + 5])):-1;

         struct msr *msr = get_msr(events[nb_events].config, events[nb_events].cpu_filter);
         events[nb_events].msr_select = msr->select;
         events[nb_events].msr_value = msr->value;
         reserve_msr(msr->id, events[nb_events].config, events[nb_events].cpu_filter);

         nb_events++;

         i += 6;
      }
      else if (!strcmp(argv[i], "-s")) {
         int j;

         if (i + 3 >= argc)
            die("Missing argument for -e COUNTER EXCLUDE_KERNEL EXCLUDE_USER\n");
         events = realloc(events, (nb_events + 1) * sizeof(*events));
         events[nb_events].name = strdup(argv[i + 1]);
         events[nb_events].type = PERF_TYPE_SOFTWARE;

         events[nb_events].per_node = 0;
         events[nb_events].cpu_filter = -1;

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
        
         events[nb_events].config = j;
         events[nb_events].exclude_kernel = atoi(argv[i + 2]);
         events[nb_events].exclude_user = atoi(argv[i + 3]);
         nb_events++;

         i += 4;
      }
      else if (!strcmp(argv[i], "-ft")) {
         with_fake_threads = 1;
         /* see spin_loop for details */
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
}

/*
 * When there are no errors, miniprof executes an infinite loop.
 * Send a SIGTERM or SIGINT signal to terminate it.
 */
int main(int argc, char**argv) {
   int i;
   
   signal(SIGPIPE, sig_handler);
   signal(SIGTERM, sig_handler);
   signal(SIGINT, sig_handler);

   // Parse options need these to be defined...
   ncpus = get_nprocs(); 
   nnodes = numa_num_configured_nodes(); 

   // Parse options
   parse_options(argc, argv);
   if(!nb_events) {
      usage(argv);
      die("No events defined");
   }

   /* Load the kernel module for MSR access */
   if(system("sudo modprobe msr")) {};


   printf("#NB cpus :\t%d\n", ncpus);
   printf("#NB nodes :\t%d\n", nnodes);

   cores_monitoring_node_events = (int*) calloc(nnodes, sizeof(int));

   /* For each node, print which cores belong to it */
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

   uint64_t clk_speed = get_cpu_freq();

   /* Print CPU clock speed */
   printf("#Clock speed: %llu\n", (long long unsigned) clk_speed);

   /* Print list of monitored events */
   for (i = 0; i < nb_events; i++) {
      char core_str[3];
      snprintf(core_str,sizeof(core_str), "%d", events[i].cpu_filter);
      printf("#Event %d: %s (%llx) (Exclude Kernel: %s, Exclude User: %s, Per node: %s, Configured core(s): %s)\n", 
            i, 
            events[i].name, 
            (long long unsigned) events[i].config, 
            (events[i].exclude_kernel) ? "yes" : "no", 
            (events[i].exclude_user) ? "yes" : "no", 
            (events[i].per_node) ? "yes" : "no", 
            events[i].cpu_filter == -1 ? "all" : core_str);
   }

   int nb_threads = ncpus;
   printf("#Event\tCore\tTime\t\t\tSamples\t%% time enabled\tlogical time\n");

   /* 
    * Spawn 1 monitoring thread on each monitored core
    * (plus 1 spinlooping thread per core if the -ft option is enabled)
    */   
   pthread_t threads[nb_threads];
   for (i = 0; i < nb_threads; i++) {
      pdata_t *data = calloc(1, sizeof(*data));
      data->core = i;

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

   /* This place is never reached when there are no errors */
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
   stop_all_pmu();
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

/* 
 * Performs a write access to a given MSR.
 * Assumes that the (x86) msr kernel module is loaded.
 */
static int wrmsr(int cpu, uint32_t msr, uint64_t val) {
   int fd;
   char msr_file_name[64];

   sprintf(msr_file_name, "/dev/cpu/%d/msr", cpu);

   fd = open(msr_file_name, O_WRONLY);
   if(fd < 0)
      thread_die("Cannot open msr device on cpu %d\n", cpu);

   if (pwrite(fd, &val, sizeof(val), msr) != sizeof(val)) {
      if (errno == EIO) {
         thread_die("wrmsr: CPU %d cannot set MSR 0x%08"PRIx32" to 0x%016"PRIx64"\n", cpu, msr, val);
      } else {
         perror("wrmsr: pwrite");
         thread_die("Exiting");
      }
   }
   close(fd);

   return 0;
}

/* 
 * Performs a read access to a given MSR.
 * Assumes that the (x86) msr kernel module is loaded.
 */
static uint64_t rdmsr(int cpu, uint32_t msr) {
   int fd;
   uint64_t data;
   char msr_file_name[64];

   sprintf(msr_file_name, "/dev/cpu/%d/msr", cpu);

   fd = open(msr_file_name, O_RDONLY);
   if(fd < 0)
      thread_die("Cannot open msr device on cpu %d\n", cpu);

   if (pread(fd, &data, sizeof data, msr) != sizeof data) {
      if (errno == EIO) {
         thread_die("rdmsr: CPU %d cannot read MSR 0x%08"PRIx32"\n", cpu, msr);
      } else {
         perror("rdmsr: pread");
         thread_die("Exiting");
      }
   }
   close(fd);
   return data;
}

void stop_all_pmu() {
   int cpu, msr;
   for(cpu = 0; cpu < ncpus; cpu++) {
      for(msr = 0; msr < nb_events; msr++) {
         if(events[msr].type == PERF_TYPE_RAW) {
            // Stop counting event
            wrmsr(cpu, events[msr].msr_select, 0);
            // Do NOT reset value msr to avoid reading something inconsistent
            //wrmsr(cpu, events[msr].msr_value, 0); 
         }
      }
   }
}
