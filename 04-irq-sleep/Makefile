# 独立内核模块（out-of-tree），编译目标是这棵内核树：~/rk3576-kernel
#
# 用法：
#   source ~/rk3576-uboot/scripts/env.sh   # 拿到 ARCH/CROSS_COMPILE
#   make
#
# 产出 hello_irq.ko、hello_key.ko（两者匹配同一个设备树节点，一次只加载一个）和用户态测试程序 test_poll、test_input，记得 cp 到 ~/rk3576-nfs-share/ 再去板子上用。

obj-m += hello_irq.o hello_key.o

KDIR := /home/dministrator/rk3576-kernel
PWD := $(shell pwd)

all: test_poll test_input
	$(MAKE) -C $(KDIR) M=$(PWD) modules

test_poll: test_poll.c
	$(CROSS_COMPILE)gcc -static -O2 -Wall -o $@ $<

test_input: test_input.c
	$(CROSS_COMPILE)gcc -static -O2 -Wall -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_poll test_input
