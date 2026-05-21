KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)

ccflags-y := -Wall -Wextra -DDEBUG
obj-m += filesystem.o

all: module userspace

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

userspace:
	$(MAKE) -C userspace

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(MAKE) -C userspace clean

.PHONY: all module userspace clean
