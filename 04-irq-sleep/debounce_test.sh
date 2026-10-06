#!/bin/sh
# 练习 ③：用快速连续翻转 B0 模拟按键抖动，对比不消抖 / 消抖 20ms
V=/sys/devices/platform/hello-gpio/value
grep -q hello_gpio /proc/modules || insmod /mnt/nfs/hello_gpio.ko
cnt() { awk '/hello_irq/ {s=0; for (i=2;i<=9;i++) s+=$i; print s}' /proc/interrupts; }
bounce_press() {	# 5 个短脉冲后停在高电平：模拟"按下时抖了几下"
	for i in 1 2 3 4 5; do echo 1 > $V; echo 0 > $V; done; echo 1 > $V
}
glitch() {		# 3 个短脉冲后回到低电平：模拟干扰毛刺，不是真的按下
	for i in 1 2 3; do echo 1 > $V; echo 0 > $V; done
}
for d in 0 20; do
	rmmod hello_irq 2>/dev/null; dmesg -c > /dev/null
	echo 0 > $V; insmod /mnt/nfs/hello_irq.ko debounce_ms=$d; sleep 0.2
	echo "===== debounce_ms=$d"
	c0=$(cnt); t0=$(cut -d' ' -f1 /proc/uptime)
	bounce_press
	t1=$(cut -d' ' -f1 /proc/uptime); sleep 0.2
	echo "[按下+抖动] 突发耗时约 $(awk "BEGIN{print ($t1-$t0)*1000}")ms，硬件中断 $(( $(cnt) - c0 )) 次"
	echo 0 > $V; sleep 0.2
	c0=$(cnt); glitch; sleep 0.2
	echo "[毛刺]      硬件中断 $(( $(cnt) - c0 )) 次"
	dmesg | grep hello_irq | grep -v ready | sed 's/^\[[ 0-9.]*\] hello_irq hello-irq: //'
done
rmmod hello_irq
