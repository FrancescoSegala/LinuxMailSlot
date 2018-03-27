obj-m += LinuxMailSlots.o

all:
	clear
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

mount:
	sudo insmod LinuxMailSlots.ko
	sudo mknod Node c 244 0

unmount:
	sudo rmmod LinuxMailSlots.ko
	sudo rm -f Node

testnode:
	sudo mknod /dev/testNode c 250 0
	sudo chmod 777 Node

remtestnode:
	sudo rm -f Node

testwrite:
	echo -n test > Node

testread:
	cat Node

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

