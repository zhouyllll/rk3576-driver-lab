# 独立内核模块（out-of-tree），编译目标是这棵内核树：~/rk3576-kernel
# 不需要重编整个内核，只编这一个 .ko，几秒钟的事。
#
# 用法：
#   source ~/rk3576-uboot/scripts/env.sh   # 拿到 ARCH/CROSS_COMPILE
#   make
#
# 产出 hello_chrdev.ko，需要手动塞进 rootfs（还没有网络，没法直接 scp/insmod 远程加载）。

obj-m += hello_chrdev.o

KDIR := /home/dministrator/rk3576-kernel
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
