# 独立内核模块（out-of-tree），编译目标是这棵内核树：~/rk3576-kernel
#
# 用法：
#   source ~/rk3576-uboot/scripts/env.sh   # 拿到 ARCH/CROSS_COMPILE
#   make
#
# 产出 hello_race.ko 和用户态测试程序 test_race、test_wait、test_lock，记得 cp 到 ~/rk3576-nfs-share/ 再去板子上用。
#
# 板子跑的是调试版内核（uname -r 显示 6.1.75-lockdep）时，模块必须对着调试版内核树编译：
#   make KDIR=/home/dministrator/rk3576-kernel-debug
# 两边的 .ko 不能混用：lockdep 改变了 spinlock、mutex 等结构体的大小，版本号也不一样，加载会被拒绝。

obj-m += hello_race.o

KDIR := /home/dministrator/rk3576-kernel
PWD := $(shell pwd)

all: test_race test_wait test_lock
	$(MAKE) -C $(KDIR) M=$(PWD) modules

test_race: test_race.c hello_race.h
	$(CROSS_COMPILE)gcc -static -O2 -Wall -o $@ $<

test_wait: test_wait.c hello_race.h
	$(CROSS_COMPILE)gcc -static -O2 -Wall -o $@ $<

test_lock: test_lock.c hello_race.h
	$(CROSS_COMPILE)gcc -static -O2 -Wall -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_race test_wait test_lock
