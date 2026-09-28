# 独立内核模块（out-of-tree），编译目标是这棵内核树：~/rk3576-kernel
#
# 用法：
#   source ~/rk3576-uboot/scripts/env.sh   # 拿到 ARCH/CROSS_COMPILE
#   make
#
# 产出 hello_platform.ko，记得 cp 到 ~/rk3576-nfs-share/ 再去板子上 insmod——
# 忘了同步这一步是 01-chardev 踩过的坑，见 ../DEBUG-CASES.md 案例4。

obj-m += hello_platform.o

KDIR := /home/dministrator/rk3576-kernel
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
