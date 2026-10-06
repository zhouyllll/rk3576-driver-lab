// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 整个驱动是怎么跑起来的 =====================
 *
 *  设备树（rk3576-kickpi-k7.dtsi）：
 *    hello-gpio {
 *        compatible = "zhouyllll,hello-gpio";
 *        led-gpios = <&gpio3 RK_PB0 GPIO_ACTIVE_HIGH>;   ← 40pin 上的 GPIO3_B0
 *    };
 *
 *  insmod hello_gpio.ko
 *    └─ compatible 匹配上 → hello_gpio_probe()
 *         └─ devm_gpiod_get(dev, "led", GPIOD_OUT_LOW)
 *              ├─ 按 "led" + "-gpios" 拼出属性名，去设备树节点里找 led-gpios
 *              ├─ 解析出 <&gpio3 RK_PB0 ...> → 找到 gpio3 控制器（gpio-rockchip.c 注册的 gpiochip）
 *              ├─ 申请这个引脚：已经被别人申请了 → 返回 -EBUSY（实验 1 要看的就是这个）
 *              └─ 申请成功 → 设成输出、初始低电平，返回一个 gpio_desc 描述符
 *
 *  用户态 echo 1 > /sys/devices/platform/hello-gpio/value
 *    └─ value_store() → gpiod_set_value_cansleep() → gpiolib → gpio-rockchip 写寄存器
 *
 *  rmmod hello_gpio
 *    └─ hello_gpio_remove()（这里什么都不用做）
 *         └─ devm 自动 gpiod_put()：引脚释放，别的驱动又可以申请了
 *
 *  跟 leds-gpio.c 对照：
 *    leds-gpio 一个节点下面挂多个子节点，每个子节点一个 LED，用 fwnode 接口遍历；
 *    这里一个节点只管一个引脚，直接 devm_gpiod_get，是它的最简版本。
 * =================================================================
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>		/* MODULE_* 宏 */
#include <linux/platform_device.h>	/* platform_device、platform_driver、platform_get/set_drvdata */
#include <linux/of.h>			/* of_device_id */
#include <linux/gpio/consumer.h>	/* gpio_desc、devm_gpiod_get、gpiod_set/get_value_cansleep */
#include <linux/device.h>		/* DEVICE_ATTR_RW、ATTRIBUTE_GROUPS、dev_err_probe */
#include <linux/kstrtox.h>		/* kstrtobool、kstrtouint */
#include <linux/hrtimer.h>		/* hrtimer：阶段 4 练习 ③ 用来产生精确方波 */
#include <linux/ktime.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("GPIO consumer driver exercise");

/*
 * 每个设备一份。现在只有一个引脚描述符，后面阶段 4 加中断时会往这里加字段。
 */
struct hello_gpio_dev {
	struct gpio_desc *led;

	/*
	 * 阶段 4 练习 ③：hrtimer 每 half_period_us 翻转一次引脚，输出方波。
	 * 杜邦线把这个引脚接到 hello_irq 的输入脚，就能用中断测 hrtimer 的周期精度。
	 */
	struct hrtimer wave_timer;
	unsigned int half_period_us;	/* 0 = 停止；只在 sysfs store（进程上下文）里写 */
	int wave_level;			/* 只在 hrtimer 回调里读写 */
};

/* ============================ sysfs 属性 ============================ */

/*
 * 读的是"逻辑值"，不是引脚上的物理电平：
 * 设备树里写 GPIO_ACTIVE_LOW 的话，物理低电平读出来是 1。翻转由 gpiolib 处理，驱动不用管。
 *
 * 为什么用 _cansleep 版本：
 * 有的 GPIO 控制器挂在 I2C/SPI 上（比如 IO 扩展芯片），访问要睡眠。
 * sysfs 的 show/store 运行在进程上下文，允许睡眠，所以用 _cansleep 版本，换成哪种控制器都能用。
 * 不带 _cansleep 的版本留给中断上下文（阶段 4 会碰到），那里不能睡眠。
 * leds-gpio.c:51 也是按 can_sleep 在两者之间选的。
 */
static ssize_t value_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hello_gpio_dev *hdev = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", gpiod_get_value_cansleep(hdev->led));
}

/*
 * kstrtobool 认 "1/0"、"y/n"、"on/off"，echo 带的换行它也能处理。
 * 返回 count 表示"这些字节我都收下了"；返回负数，echo 会报错（比如 echo abc 会看到 Invalid argument）。
 */
static ssize_t value_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct hello_gpio_dev *hdev = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(hdev->led, on);
	dev_dbg(dev, "set %d\n", on);
	return count;
}

/* 可读写：权限 0644，需要 value_show 和 value_store 两个函数 */
static DEVICE_ATTR_RW(value);

/*
 * 【新写法】hrtimer 回调：高精度定时器到期时调用。
 *
 *   - 运行在硬中断上下文（HRTIMER_MODE_REL 默认是 hard 模式），不能睡眠，
 *     所以这里只能用不带 _cansleep 的 gpiod_set_value（start 时检查过控制器不会睡）
 *   - hrtimer_forward_now：把到期时间往后推一个周期。基准是"上一次的到期时间"，
 *     不是"现在"，所以回调本身的执行耗时不会累积成周期误差
 *   - 返回 HRTIMER_RESTART 表示继续；HRTIMER_NORESTART 表示到此为止
 */
static enum hrtimer_restart hello_gpio_wave(struct hrtimer *t)
{
	struct hello_gpio_dev *hdev = container_of(t, struct hello_gpio_dev, wave_timer);

	hdev->wave_level = !hdev->wave_level;
	gpiod_set_value(hdev->led, hdev->wave_level);

	hrtimer_forward_now(t, ns_to_ktime((u64)READ_ONCE(hdev->half_period_us) * NSEC_PER_USEC));
	return HRTIMER_RESTART;
}

static ssize_t half_period_us_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hello_gpio_dev *hdev = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", hdev->half_period_us);
}

/*
 * echo 500 > half_period_us ：每 500us 翻转一次，方波周期 1ms（1kHz，每 1ms 一个上升沿）
 * echo 0   > half_period_us ：停止，引脚回到低电平
 */
static ssize_t half_period_us_store(struct device *dev, struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct hello_gpio_dev *hdev = dev_get_drvdata(dev);
	unsigned int us;
	int ret;

	ret = kstrtouint(buf, 0, &us);
	if (ret)
		return ret;
	if (us && us < 50)		/* 太快的话 CPU 全耗在定时器中断上 */
		return -EINVAL;
	if (us && gpiod_cansleep(hdev->led))	/* 回调里不能睡，控制器会睡就不支持 */
		return -EOPNOTSUPP;

	/* 先停：hrtimer_cancel 会等正在执行的回调跑完 */
	hrtimer_cancel(&hdev->wave_timer);
	WRITE_ONCE(hdev->half_period_us, us);
	if (us) {
		hdev->wave_level = 0;
		gpiod_set_value_cansleep(hdev->led, 0);
		hrtimer_start(&hdev->wave_timer, ns_to_ktime((u64)us * NSEC_PER_USEC), HRTIMER_MODE_REL);
	} else {
		gpiod_set_value_cansleep(hdev->led, 0);
	}
	return count;
}
static DEVICE_ATTR_RW(half_period_us);

static struct attribute *hello_gpio_attrs[] = {
	&dev_attr_value.attr,
	&dev_attr_half_period_us.attr,
	NULL,
};
ATTRIBUTE_GROUPS(hello_gpio);

/* ============================ probe / remove ============================ */

static int hello_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hello_gpio_dev *hdev;

	hdev = devm_kzalloc(dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev)
		return -ENOMEM;

	/*
	 * 第二个参数 "led" 是 con_id：gpiolib 会依次找 led-gpios、led-gpio 这两个属性名。
	 * GPIOD_OUT_LOW：申请成功后立刻设成输出、逻辑 0，避免引脚处于不确定的状态。
	 *
	 * 失败时用 dev_err_probe 而不是 dev_err：
	 *   - 它会把错误码按名字打印出来（"error -EBUSY"），不用自己查数字
	 *   - 如果是 -EPROBE_DEFER（GPIO 控制器驱动还没 probe），它不打印错误，
	 *     只记下原因，可以在 /sys/kernel/debug/devices_deferred 里看到。
	 *     内核之后会自动重试 probe，这种情况不算真正的失败
	 */
	hdev->led = devm_gpiod_get(dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(hdev->led))
		return dev_err_probe(dev, PTR_ERR(hdev->led), "failed to get led-gpios\n");

	/*
	 * 给引脚起个名字，/sys/kernel/debug/gpio 里显示的就是它（leds-gpio.c:179 也做了同样的事）。
	 * 不设的话，默认显示的是 con_id，也就是 "led"。
	 */
	gpiod_set_consumer_name(hdev->led, "hello-gpio");

	/*
	 * 要放在 return 0 之前：driver core 先调 probe，probe 成功后才创建 dev_groups 里的属性文件，
	 * 所以 value_show/value_store 被调用时 drvdata 一定已经设好了。
	 */
	/*
	 * 【新写法】hrtimer_init(定时器, 时钟源, 模式)
	 *   CLOCK_MONOTONIC：单调递增的时钟，不受修改系统时间影响
	 *   HRTIMER_MODE_REL：start 时给的是"从现在起多久"；默认 hard 模式，回调在硬中断里执行
	 * 6.1 还是 hrtimer_init + 手动设 function；更新的内核改成了 hrtimer_setup 一步完成。
	 */
	hrtimer_init(&hdev->wave_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hdev->wave_timer.function = hello_gpio_wave;

	platform_set_drvdata(pdev, hdev);
	dev_info(dev, "probed, led gpio = %d\n", desc_to_gpio(hdev->led));
	return 0;
}

/*
 * 引脚和内存都是 devm 管理的，解绑时自动释放，这里没有要手动清理的东西。
 * 留着这个函数只是为了打一行日志，方便观察绑定和解绑的时机。
 */
static int hello_gpio_remove(struct platform_device *pdev)
{
	struct hello_gpio_dev *hdev = platform_get_drvdata(pdev);

	/*
	 * 必须停掉 hrtimer：它不是 devm 管理的。不停的话，模块卸载后定时器到期，
	 * 回调函数的代码已经不在了 → 内核崩溃。
	 */
	hrtimer_cancel(&hdev->wave_timer);
	dev_info(&pdev->dev, "removed\n");
	return 0;
}

static const struct of_device_id hello_gpio_of_match[] = {
	{ .compatible = "zhouyllll,hello-gpio" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hello_gpio_of_match);

static struct platform_driver hello_gpio_driver = {
	.probe	= hello_gpio_probe,
	.remove	= hello_gpio_remove,
	.driver	= {
		.name		= "hello_gpio",
		.of_match_table	= hello_gpio_of_match,
		/*
		 * 对比 02：那边属性挂在 misc 设备上（/sys/class/misc/hello0/），
		 * 这里没有 /dev 节点，属性直接挂在 platform 设备上，
		 * 路径是 /sys/devices/platform/hello-gpio/value（目录名就是设备树节点名）。
		 */
		.dev_groups	= hello_gpio_groups,
	},
};
module_platform_driver(hello_gpio_driver);
