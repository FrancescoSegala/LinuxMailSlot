obj-m += LinuxMailSlots.o

all:
	clear
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

mount:
	sudo insmod LinuxMailSlots.ko
	sudo mknod /dev/testNode c 250 0
	sudo chmod 777 /dev/testNode

unmount:
	sudo rmmod LinuxMailSlots.ko
	sudo rm -f /dev/testNode

testnode:
	sudo mknod /dev/testNode c 250 0
	sudo chmod 777 /dev/testNode

remtestnode:
	sudo rm -f /dev/testNode

testwrite:
	echo -n test > /dev/testNode

testread:
	cat /dev/testNode

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

