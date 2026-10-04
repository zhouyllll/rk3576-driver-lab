// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 练习 ②：同样的功能，改用 leds 子系统 =====================
 *
 *  hello_gpio.c 是"自己造接口"：自己定义 sysfs 文件 value、自己写 show/store。
 *  这个驱动是"接入现成框架"：只告诉 leds 子系统"怎么点亮/熄灭"，其余全由子系统提供：
 *    /sys/class/leds/hello-led/brightness      ← 读写亮度（子系统实现的 show/store）
 *    /sys/class/leds/hello-led/max_brightness
 *    /sys/class/leds/hello-led/trigger         ← 白送的功能：heartbeat、timer 闪烁……
 *
 *  设备树：直接复用 hello-gpio 节点（compatible 一样），省得重新刷机。
 *  同一个设备同一时刻只能绑定一个驱动，所以 hello_gpio.ko 和 hello_led.ko 一次只加载一个。
 *
 *  insmod hello_led.ko
 *    └─ 匹配上 hello-gpio 节点 → hello_led_probe()
 *         ├─ devm_gpiod_get(dev, "led", ...)     跟 hello_gpio.c 完全一样
 *         ├─ 填 struct led_classdev：名字、最大亮度、回调函数
 *         └─ devm_led_classdev_register()        → /sys/class/leds/hello-led/ 出现
 *
 *  echo 1 > /sys/class/leds/hello-led/brightness
 *    └─ leds 子系统的 brightness_store()（drivers/leds/led-class.c）
 *         └─ ... → led_cdev->brightness_set_blocking(led_cdev, 1)
 *              └─ hello_led_set()：container_of 找回 hello_led → gpiod_set_value_cansleep
 *
 *  echo timer > /sys/class/leds/hello-led/trigger
 *    └─ timer 触发器内部定时调用同一个 hello_led_set()，一行代码不用写 LED 就开始闪
 * ===============================================================================
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>	/* devm_gpiod_get、gpiod_set_value_cansleep */
#include <linux/leds.h>			/* led_classdev、devm_led_classdev_register、LED_OFF */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("GPIO LED via leds subsystem exercise");

/*
 * 【新写法】把子系统的结构体"嵌"在自己的结构体里（不是放指针，是整个结构体放进来）。
 *
 *   struct hello_led
 *   ┌──────────────────────────┐ ← container_of 算出来的地址
 *   │ cdev (struct led_classdev)│ ← 子系统回调时只给你这个地址
 *   │ gpiod                     │ ← 你真正需要的东西
 *   └──────────────────────────┘
 *
 * 子系统只认识 led_classdev，不知道 hello_led 的存在；回调里用 container_of
 * 从 cdev 的地址减去它在结构体里的偏移，就回到了 hello_led 的开头。
 * 跟阶段 2 里从 miscdevice 找回 hello_dev、上一节从 attr 找回 dev_attr_value 是同一个套路。
 */
struct hello_led {
	struct led_classdev cdev;
	struct gpio_desc *gpiod;
};

/*
 * 【新写法】brightness_set_blocking 回调：子系统想改亮度时调用它。
 *
 * leds 子系统有两个回调可选（include/linux/leds.h:103、109）：
 *   brightness_set          ：不能睡眠，返回 void。子系统可能在定时器（中断上下文）里直接调它
 *   brightness_set_blocking ：可以睡眠，返回 int。子系统需要在不能睡眠的地方改亮度时，
 *                             会改成丢给工作队列去调（led-core.c:276 的 schedule_work）
 * 这里用 _blocking 版本，所以里面可以放心用 gpiod_set_value_cansleep。
 * leds-gpio.c:82-87 就是按 GPIO 能不能睡眠在这两个之间选的。
 *
 * value 的范围是 0 ~ max_brightness，这里 max_brightness = 1，所以只有 0 和 1。
 * 用户写比 max_brightness 大的数，子系统会先截断成 max_brightness 再调你。
 */
static int hello_led_set(struct led_classdev *cdev, enum led_brightness value)
{
	struct hello_led *led = container_of(cdev, struct hello_led, cdev);

	gpiod_set_value_cansleep(led->gpiod, value != LED_OFF);
	return 0;
}

static int hello_led_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hello_led *led;
	int ret;

	led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	led->gpiod = devm_gpiod_get(dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(led->gpiod))
		return dev_err_probe(dev, PTR_ERR(led->gpiod), "failed to get led-gpios\n");

	/*
	 * 名字决定目录名 /sys/class/leds/<name>。
	 * 设备树节点里有 label 属性就用它（gpio-leds 的 GPIO3_B0 等名字就是这么来的），
	 * 没有就用默认名。of_property_read_string 失败时不会改 name，所以先赋默认值再读。
	 */
	led->cdev.name = "hello-led";
	of_property_read_string(dev->of_node, "label", &led->cdev.name);

	led->cdev.max_brightness = 1;			/* GPIO 只有开和关 */
	led->cdev.brightness_set_blocking = hello_led_set;

	gpiod_set_consumer_name(led->gpiod, led->cdev.name);

	/*
	 * 【新写法】注册给 leds 子系统。
	 * 成功后子系统会在 /sys/class/leds/ 下建目录和 brightness/trigger 等文件——
	 * 这些文件的 show/store 都是子系统写的（drivers/leds/led-class.c），你不用写一个 DEVICE_ATTR。
	 * devm 版本：解绑时自动注销，所以下面不需要 remove 函数。
	 * 返回 int（不是指针），所以直接判断 ret，不用 IS_ERR。
	 */
	ret = devm_led_classdev_register(dev, &led->cdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register led\n");

	dev_info(dev, "registered /sys/class/leds/%s\n", led->cdev.name);
	return 0;
}

/*
 * 故意跟 hello_gpio.c 用同一个 compatible，复用同一个设备树节点。
 * 正式的驱动不会这么做——这里只是为了练习时不用重新刷机。
 */
static const struct of_device_id hello_led_of_match[] = {
	{ .compatible = "zhouyllll,hello-gpio" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hello_led_of_match);

static struct platform_driver hello_led_driver = {
	.probe	= hello_led_probe,
	.driver	= {
		.name		= "hello_led",
		.of_match_table	= hello_led_of_match,
	},
};
module_platform_driver(hello_led_driver);
