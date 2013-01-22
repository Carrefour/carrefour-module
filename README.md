carrefour-module
================

This module collects ibs samples and what should be done for each page (replication, migration, interleaving).
It also has a simple runtime (carrefour.pl), but a more advanced runtime is available (https://github.com/Carrefour/carrefour-runtime).

Most of the options are available in include/carrefour_main.h, except IBS sampling frequency (src/ibs/ibs_main.c) and rbtree size (include/carrefour_rbtree.h).

If you don't have a kernel that supports replicating pages (see https://github.com/Carrefour/linux-replication), you must disable replication in include/carrefour_main.h.

Default options are those we used for the ASPLOS paper.

Notes
=====

Since it uses IBS, it will only work on AMD processors. It has only been tested on AMD Family 10h.
If your using carrefour-per-pid as a runtime, you MUST enable 'REPLICATION_PER_TID'. If your not, you MUST disable it.
