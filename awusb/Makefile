KVERS?=$(shell uname -r)
obj-m := awusb.o
KDIR := /lib/modules/${KVERS}/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean
	rm -rf Module.markers module.order module.sysvers 
	
install: awusb.ko
	install -d /lib/modules/${KVERS}/kernel/extra/
	install $< /lib/modules/${KVERS}/kernel/extra/
