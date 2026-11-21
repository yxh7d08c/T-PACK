obj-m += tdcall_module.o

# 为了在宿主机上编译，通常使用宿主机的内核源码或者通过传入 KDIR 变量
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean