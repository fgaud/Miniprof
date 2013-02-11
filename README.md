Miniprof
========

Miniprof is a profiler that periodically dumps the values of specified hardware counters. To that purpose, it uses the perf interface provided by the Linux kernel.

The 'msr' branch directly initialises the msr register, without using the perf interface. As a results, it supports architectures that are not yet supported by perf but it lacks fancy features such as multiplexing.

IMPORTANT NOTES
===============

* Miniprof has only been tested on AMD multicore architectures, although it
should work on other architectures as well
* Miniprof assumes that there is one memory bank per-die. This assumption should
be removed in a future release
