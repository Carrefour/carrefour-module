obj-m += carrefour.o
carrefour-y := src/carrefour_main.o src/carrefour_rbtree.o src/carrefour.o src/carrefour_migrate.o src/carrefour_machine.o src/carrefour_tids.o  src/carrefour_tid_replication.o
carrefour-y += src/ibs/nmi_int.o src/ibs/ibs_init.o src/ibs/ibs_main.o src/ibs/fake_ibs.o

ccflags-y := -I$(src)/include
ccflags-y += -I$(src)/src/ibs/include

KV = $(shell uname -r)

KDIR := /lib/modules/$(KV)/build
PWD := $(shell pwd)

ifeq ($(KV), 3.2.1-replication+)
ccflags-y += -DLEGACY_MADV_REP
endif

#Recheck that the kernel symbols are at the last version everytime
.PHONY: carrefour_hooks.h tags 

default: carrefour_hooks.h tags
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

carrefour_hooks.h: create_hooks.pl
	$(PWD)/create_hooks.pl $(PWD)/include

clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean
	rm -f modules.order
	rm -f include/carrefour_hooks.h

clean_tags:
	rm cscope.*
	rm tags

mrproper: clean clean_tags

tags:
	ctags --totals `find . -name '*.[ch]'`
	cscope -b -q -k -R -s.
