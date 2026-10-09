// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 练习 ④：PWM 使用者驱动（可调亮度的 LED） =====================
 *
 *  hello_led.c 用 GPIO 只能开/关（max_brightness = 1）；
 *  这里换成 PWM：亮度 0~255 换算成占空比 0%~100%，LED 就能调亮度。
 *  对上层来说还是同一个 leds 子系统接口：/sys/class/leds/hello-pwm/brightness
 *
 *  设备树（rk3576-kickpi-k7.dtsi）：
 *    hello-pwm {
 *        compatible = "zhouyllll,hello-pwm";
 *        pwms = <&pwm2_8ch_6 0 1000000 0>;    ← 用哪个 PWM 控制器、第几路、周期(ns)、极性
 *    };
 *
 *  insmod hello_pwm.ko
 *    └─ hello_pwm_probe()
 *         ├─ devm_pwm_get(dev, NULL)          按 pwms 属性找到 PWM 控制器，申请其中一路
 *         ├─ pwm_init_state()                 用设备树里的周期、极性填好一个 pwm_state
 *         └─ devm_led_classdev_register()     → /sys/class/leds/hello-pwm/
 *
 *  echo 64 > /sys/class/leds/hello-pwm/brightness
 *    └─ hello_pwm_set(cdev, 64)
 *         ├─ 占空比 = 64/255 × 周期
 *         └─ pwm_apply_state()                → pwm-rockchip.c 写寄存器 → 引脚输出 25% 方波
 *
 *  跟 GPIO 的对照：
 *    GPIO：devm_gpiod_get + gpiod_set_value     （一个值：0/1）
 *    PWM ：devm_pwm_get   + pwm_apply_state     （一组值：周期、占空比、极性、开关）
 * ===================================================================================
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/pwm.h>			/* pwm_device、pwm_state、devm_pwm_get、pwm_apply_state */
#include <linux/leds.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("PWM consumer exercise: dimmable LED");

struct hello_pwm {
	struct led_classdev cdev;
	struct pwm_device *pwm;
};

/*
 * 【新写法】struct pwm_state + pwm_apply_state：
 *   PWM 有好几个参数要一起改，内核的做法是"整体提交"：
 *   先把当前状态拷一份出来（pwm_init_state），改好需要改的字段，再一次性 apply。
 *   不会出现"周期改了、占空比还没改"的中间状态。
 *
 *   struct pwm_state {          include/linux/pwm.h:59
 *       u64 period;             周期，纳秒
 *       u64 duty_cycle;         高电平时间，纳秒，必须 ≤ period
 *       enum pwm_polarity polarity;
 *       bool enabled;
 *   };
 *
 * pwm_apply_state 内部有 might_sleep()（drivers/pwm/core.c:558），可能睡眠，
 * 所以这里用 brightness_set_blocking——跟 hello_led.c 选 _blocking 的理由一样。
 */
static int hello_pwm_set(struct led_classdev *cdev, enum led_brightness value)
{
	struct hello_pwm *hp = container_of(cdev, struct hello_pwm, cdev);
	struct pwm_state state;

	/*
	 * 每次都从设备树的初始配置出发（周期、极性），而不是读当前硬件状态，
	 * 这样不管之前被设成什么样，结果都是确定的。
	 */
	pwm_init_state(hp->pwm, &state);

	/*
	 * pwm_set_relative_duty_cycle(&state, value, 255)：
	 *   duty_cycle = period × value / 255，帮你做了乘除和取整（include/linux/pwm.h:243）。
	 * value 为 0 时直接关掉输出，比输出 0% 占空比更省电。
	 */
	pwm_set_relative_duty_cycle(&state, value, cdev->max_brightness);
	state.enabled = value > 0;

	return pwm_apply_state(hp->pwm, &state);
}

static int hello_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hello_pwm *hp;
	int ret;

	hp = devm_kzalloc(dev, sizeof(*hp), GFP_KERNEL);
	if (!hp)
		return -ENOMEM;

	/*
	 * 【新写法】devm_pwm_get(dev, con_id)：跟 devm_gpiod_get 是同一个套路。
	 *   - 去设备树找 pwms 属性。con_id 传 NULL 表示只有一路，直接用第一个；
	 *     有多路时写 pwm-names = "a", "b"; 再用 con_id 按名字取
	 *   - 返回指针，失败时是 ERR_PTR：用 IS_ERR 判断
	 *   - 这一路如果已经被别人占着（比如在 sysfs 里 export 了没 unexport），返回 -EBUSY
	 *   - devm：解绑时自动释放
	 */
	hp->pwm = devm_pwm_get(dev, NULL);
	if (IS_ERR(hp->pwm))
		return dev_err_probe(dev, PTR_ERR(hp->pwm), "failed to get pwm\n");

	hp->cdev.name = "hello-pwm";
	of_property_read_string(dev->of_node, "label", &hp->cdev.name);
	hp->cdev.max_brightness = 255;			/* 256 级亮度 */
	hp->cdev.brightness_set_blocking = hello_pwm_set;

	/* 先把输出关掉，保证初始状态确定 */
	ret = hello_pwm_set(&hp->cdev, LED_OFF);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init pwm\n");

	/* remove 里要用 hp->pwm 关输出，所以要存起来；hello_led.c 没有 remove，就没存 */
	platform_set_drvdata(pdev, hp);

	ret = devm_led_classdev_register(dev, &hp->cdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register led\n");

	dev_info(dev, "registered /sys/class/leds/%s, period %llu ns\n",
		 hp->cdev.name, pwm_get_period(hp->pwm));
	return 0;
}

/*
 * 卸载时把 PWM 关掉。这一步其实是"保险"，不是必须的：
 *   devm 清理时会先注销 LED（它是在 devm_pwm_get 之后注册的，所以先被清理），
 *   而 led_classdev_unregister 会自动把亮度设成 LED_OFF（drivers/leds/led-class.c:437 起），
 *   调到 hello_pwm_set → enabled = false，输出就停了；之后才 pwm_put 归还 PWM。
 *   如果设了 LED_RETAIN_AT_SHUTDOWN 标志，leds 子系统就不会自动关灯，这时就要靠这里。
 * 顺序：remove 先执行，devm 资源在 remove 返回之后才释放，所以这里 PWM 还拿在手里，可以安全地关掉。
 * 也正因为 devm 按"申请的相反顺序"清理，probe 里要先 devm_pwm_get、后注册 LED：
 * 反过来的话，清理时会先归还 PWM、再注销 LED，注销时调 hello_pwm_set 用的就是已经还回去的 PWM。
 */
static int hello_pwm_remove(struct platform_device *pdev)
{
	struct hello_pwm *hp = platform_get_drvdata(pdev);

	pwm_disable(hp->pwm);
	return 0;
}

static const struct of_device_id hello_pwm_of_match[] = {
	{ .compatible = "zhouyllll,hello-pwm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hello_pwm_of_match);

static struct platform_driver hello_pwm_driver = {
	.probe	= hello_pwm_probe,
	.remove	= hello_pwm_remove,
	.driver	= {
		.name		= "hello_pwm",
		.of_match_table	= hello_pwm_of_match,
	},
};
module_platform_driver(hello_pwm_driver);
