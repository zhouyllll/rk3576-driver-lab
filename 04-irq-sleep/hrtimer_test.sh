#!/bin/sh
# 练习 ③ 后半：hello_gpio 的 hrtimer 输出方波，hello_irq 测相邻上升沿的间隔
G=/sys/devices/platform/hello-gpio
S=/sys/devices/platform/hello-irq/irq_stats
rmmod hello_irq 2>/dev/null; rmmod hello_gpio 2>/dev/null
insmod /mnt/nfs/hello_gpio.ko
insmod /mnt/nfs/hello_irq.ko measure=1
for hp in 500 100; do
	echo 0 > $S
	echo $hp > $G/half_period_us
	sleep 3
	echo 0 > $G/half_period_us
	echo "half_period_us=$hp (预期上升沿间隔 $((hp*2*1000))ns): $(cat $S)"
done
grep hello_irq /proc/interrupts
rmmod hello_irq; rmmod hello_gpio; echo unload-ok
