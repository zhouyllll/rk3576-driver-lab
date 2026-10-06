#!/bin/sh
# 练习 ④ 第二步：gpio-keys（只写设备树）验证 + 跟 hello_key 抢引脚
V=/sys/devices/platform/hello-gpio/value
insmod /mnt/nfs/hello_gpio.ko
echo "### gpio-keys 开机就 probe 了："
ls /sys/bus/platform/drivers/gpio-keys/
grep -A4 'hello-gpio-keys' /proc/bus/input/devices
grep gpio-124 /sys/kernel/debug/gpio
echo "### 读事件（gpio-keys，没加载任何自己的模块）"
echo 0 > $V
(sleep 2; echo 1 > $V; sleep 0.5; echo 0 > $V; sleep 0.5; for i in 1 2 3; do echo 1 > $V; echo 0 > $V; done; echo 1 > $V; sleep 0.5; echo 0 > $V) &
/mnt/nfs/test_input hello-gpio-keys 5
echo "### 这时加载 hello_key："
dmesg -c > /dev/null; insmod /mnt/nfs/hello_key.ko; echo "insmod ret=$?"; dmesg | grep hello_key
ls /sys/bus/platform/drivers/hello_key/
echo "### 解绑 gpio-keys 后再试："
rmmod hello_key
echo hello-gpio-keys > /sys/bus/platform/drivers/gpio-keys/unbind
grep -c hello-gpio-keys /proc/bus/input/devices
insmod /mnt/nfs/hello_key.ko; dmesg | grep hello_key | tail -1
grep gpio-124 /sys/kernel/debug/gpio
