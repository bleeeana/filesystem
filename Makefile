obj-m += filesystem.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

ccflags-y := -Wall -Wextra -DDEBUG

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(MAKE) -C userspace clean 2>/dev/null || true

userspace:
	$(MAKE) -C userspace

.PHONY: all clean userspace