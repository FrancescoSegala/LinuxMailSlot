#
# Makefile for LinuxMailSlots module
#

obj-m += LinuxMailSlots.o

all:
	clear
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

mount:
	sudo insmod LinuxMailSlots.ko

node:
	sudo mknod Node c 244 0

rmnode:
	sudo rm -f Node

unmount:
	sudo rmmod LinuxMailSlots.ko
	sudo rm -f Node

testnode:
	sudo mknod testNode c 244 0
	sudo chmod 777 testNode

remtestnode:
	sudo rm -f testNode

write:
	echo -n test > Node

read:
	cat Node

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

