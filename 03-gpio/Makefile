# 独立内核模块（out-of-tree），编译目标是这棵内核树：~/rk3576-kernel
#
# 用法：
#   source ~/rk3576-uboot/scripts/env.sh   # 拿到 ARCH/CROSS_COMPILE
#   make
#
# 产出 hello_gpio.ko、hello_led.ko（这两个匹配同一个设备树节点，一次只加载一个）和 hello_pwm.ko，记得 cp 到 ~/rk3576-nfs-share/ 再去板子上 insmod。

obj-m += hello_gpio.o hello_led.o hello_pwm.o

KDIR := /home/dministrator/rk3576-kernel
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
