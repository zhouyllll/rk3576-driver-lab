#!/bin/sh
# 练习 ②：四种下半部各跑 3 次中断，对比运行上下文和延迟
V=/sys/devices/platform/hello-gpio/value
grep -q hello_gpio /proc/modules || insmod /mnt/nfs/hello_gpio.ko
for m in 0 1 2 3; do
	rmmod hello_irq 2>/dev/null
	dmesg -c > /dev/null
	insmod /mnt/nfs/hello_irq.ko bh=$m
	echo 0 > $V
	for i in 1 2 3; do echo 1 > $V; sleep 0.3; echo 0 > $V; sleep 0.3; done
	echo "===== bh=$m"
	dmesg | grep hello_irq | sed 's/^\[[ 0-9.]*\] //'
	grep hello_irq /proc/interrupts
	[ $m = 3 ] && ps | grep -E 'irq/.*hello' | grep -v grep
done
echo "===== rmmod with bh=3"
rmmod hello_irq && echo rmmod-ok
