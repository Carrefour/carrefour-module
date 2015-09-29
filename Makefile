obj-m += carrefour.o
carrefour-y := src/carrefour_main.o src/carrefour_rbtree.o src/carrefour.o src/carrefour_machine.o src/carrefour_tid_replication.o src/console.o
carrefour-y += src/carrefour_options.o src/ibs/nmi_int.o src/ibs/ibs_init.o src/ibs/ibs_main.o src/ibs/fake_ibs.o

ccflags-y := -I$(src)/include
ccflags-y += -I$(src)/src/ibs/include
ccflags-y += -Werror

KV = $(shell uname -r)
OPTERON = $(shell if [ `grep -m 1 family /proc/cpuinfo | awk '{print $$4}'` -lt 21 ]; then echo "y"; else echo "n"; fi)

KDIR := /lib/modules/$(KV)/build
PWD := $(shell pwd)

ifeq ($(KV), 3.2.1-replication+)
ccflags-y += -DLEGACY_MADV_REP
endif

ifeq ($(OPTERON), y)
ccflags-y += -DOPTERON
endif

.PHONY: tags default

default: tags
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean
	rm -f modules.order

clean_tags:
	rm cscope.*
	rm tags

mrproper: clean clean_tags

tags:
	ctags --totals `find . -name '*.[ch]'` $(KDIR)/include/linux/carrefour-hooks.h
	cscope -b -q -k -R -s. -s$(KDIR)/include/linux/carrefour-hooks.h
