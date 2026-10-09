# 独立内核模块（out-of-tree），编译目标是这棵内核树：~/rk3576-kernel
#
# 用法：
#   source ~/rk3576-uboot/scripts/env.sh   # 拿到 ARCH/CROSS_COMPILE
#   make
#
# 产出 hello_race.ko 和用户态测试程序 test_race、test_wait，记得 cp 到 ~/rk3576-nfs-share/ 再去板子上用。

obj-m += hello_race.o

KDIR := /home/dministrator/rk3576-kernel
PWD := $(shell pwd)

all: test_race test_wait
	$(MAKE) -C $(KDIR) M=$(PWD) modules

test_race: test_race.c hello_race.h
	$(CROSS_COMPILE)gcc -static -O2 -Wall -o $@ $<

test_wait: test_wait.c hello_race.h
	$(CROSS_COMPILE)gcc -static -O2 -Wall -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_race test_wait
