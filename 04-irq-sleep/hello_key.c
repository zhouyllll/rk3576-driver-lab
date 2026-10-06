// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 练习 ④：用 input 子系统上报按键 =====================
 *
 *  hello_irq.c 自己造了 /dev/hello_irq，读出来的格式也是自己定的；
 *  这里改成接入 input 子系统：注册一个 input_dev，内核自动生成 /dev/input/eventN，
 *  上报的是统一格式的 struct input_event，evtest、Qt、Android 都能直接读。
 *
 *  设备树：复用 hello-irq 节点（compatible 一样，跟 hello_irq.ko 一次只加载一个），
 *  GPIO3_D4 当"按键"，杜邦线接 GPIO3_B0，echo 1/0 > hello-gpio/value 就是按下/松开。
 *
 *  insmod hello_key.ko → hello_key_probe()
 *    ├─ devm_gpiod_get(dev, "irq", GPIOD_IN)
 *    ├─ devm_input_allocate_device + input_set_capability(EV_KEY, KEY_PROG1)
 *    ├─ input_register_device                  → /dev/input/eventN 出现
 *    └─ devm_request_irq(双边沿)
 *
 *  按下（上升沿）/ 松开（下降沿）
 *    └─ hello_key_isr（硬中断）：mod_timer，把消抖定时器往后推
 *         └─ debounce_ms 内没有新边沿 → hello_key_debounce（软中断）
 *              ├─ 读引脚电平：1 = 按下，0 = 松开
 *              ├─ input_report_key(input, KEY_PROG1, 电平)
 *              └─ input_sync(input)            → 用户态 read 拿到事件
 *
 *  对照：内核自带的 drivers/input/keyboard/gpio_keys.c 做的就是这件事（还多了唤醒、多按键等），
 *  只写设备树就能用——练习 ④ 的第二步会对比。
 * =========================================================================
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/input.h>		/* input_dev、input_report_key、input_sync、KEY_* */
#include <linux/timer.h>
#include <linux/jiffies.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("GPIO key via input subsystem exercise");

static unsigned int debounce_ms = 20;
module_param(debounce_ms, uint, 0444);
MODULE_PARM_DESC(debounce_ms, "debounce time in ms (default 20)");

struct hello_key {
	struct input_dev *input;	/* 拿来的：devm_input_allocate_device 分配，存指针 */
	struct gpio_desc *gpiod;
	int irq;
	struct timer_list debounce;
};

/*
 * 消抖定时器到期（软中断上下文）：信号已经稳定 debounce_ms，读电平上报。
 * 跟 hello_irq 的消抖不同：这里按下和松开都要报，所以双边沿触发，到期时电平是几就报几。
 *
 * 【新写法】input_report_key(dev, code, value)：value 1 = 按下，0 = 松开（2 = 自动重复）。
 * 【新写法】input_sync(dev)：发一个 EV_SYN/SYN_REPORT，告诉用户态"这一组事件完整了"。
 *   一次触摸可能同时报 X、Y、压力好几个值，sync 之前的都算同一时刻；按键只有一个值，也要 sync。
 * 同样的状态重复上报，input 核心会自动丢掉（只有变化才产生事件），所以这里不用自己判断"变没变"。
 * input_event 内部用 spin_lock_irqsave，在软中断里调用没问题。
 */
static void hello_key_debounce(struct timer_list *t)
{
	struct hello_key *key = from_timer(key, t, debounce);
	int val = gpiod_get_value(key->gpiod);	/* 软中断里不能睡，gpio-rockchip 不会睡 */

	if (val < 0)
		return;
	input_report_key(key->input, KEY_PROG1, val);
	input_sync(key->input);
}

/* 上半部：只推定时器 */
static irqreturn_t hello_key_isr(int irq, void *dev_id)
{
	struct hello_key *key = dev_id;

	mod_timer(&key->debounce, jiffies + msecs_to_jiffies(debounce_ms));
	return IRQ_HANDLED;
}

static void hello_key_timer_cleanup(void *data)
{
	struct hello_key *key = data;

	del_timer_sync(&key->debounce);
}

static int hello_key_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hello_key *key;
	int ret;

	key = devm_kzalloc(dev, sizeof(*key), GFP_KERNEL);
	if (!key)
		return -ENOMEM;

	key->gpiod = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(key->gpiod))
		return dev_err_probe(dev, PTR_ERR(key->gpiod), "failed to get irq-gpios\n");
	gpiod_set_consumer_name(key->gpiod, "hello-key");

	key->irq = gpiod_to_irq(key->gpiod);
	if (key->irq < 0)
		return dev_err_probe(dev, key->irq, "failed to map gpio to irq\n");

	/*
	 * 【新写法】devm_input_allocate_device：分配 input_dev，并且是 devm 管理的——
	 * 用它分配的设备，input_register_device 之后解绑时也会自动注销，不用写 input_unregister_device。
	 */
	key->input = devm_input_allocate_device(dev);
	if (!key->input)
		return -ENOMEM;

	/*
	 * 这些字段在用户态能看到：
	 *   name ：EVIOCGNAME 读到的名字，/proc/bus/input/devices 里的 N: Name=
	 *   phys ：物理路径，用来区分同名设备
	 *   id   ：总线类型和厂商/产品号，BUS_HOST 表示 SoC 上直接连的
	 */
	key->input->name = "hello-key";
	key->input->phys = "hello-key/input0";
	key->input->id.bustype = BUS_HOST;

	/*
	 * 【新写法】input_set_capability：声明能上报什么事件。
	 * 没声明的 code，input_report_key 会被直接丢掉。
	 * 选 KEY_PROG1（"可编程键 1"）而不是 KEY_ENTER：避免万一系统里有键盘处理程序，真的按下了回车。
	 */
	input_set_capability(key->input, EV_KEY, KEY_PROG1);

	/* 先注册 input，再申请中断：中断一来就要上报，上报的对象得已经就绪 */
	ret = input_register_device(key->input);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register input device\n");

	timer_setup(&key->debounce, hello_key_debounce, 0);
	/* 登记在 request_irq 之前：解绑时先 free_irq、再停定时器、再注销 input */
	ret = devm_add_action_or_reset(dev, hello_key_timer_cleanup, key);
	if (ret)
		return ret;

	/* 按下（上升沿）和松开（下降沿）都要知道 */
	ret = devm_request_irq(dev, key->irq, hello_key_isr,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "hello_key", key);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq %d\n", key->irq);

	dev_info(dev, "input device '%s' ready, irq %d, debounce %ums\n",
		 key->input->name, key->irq, debounce_ms);
	return 0;
}

/* 跟 hello_irq.ko 用同一个 compatible，复用 hello-irq 节点，省得刷机。正式驱动不会这么做 */
static const struct of_device_id hello_key_of_match[] = {
	{ .compatible = "zhouyllll,hello-irq" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hello_key_of_match);

static struct platform_driver hello_key_driver = {
	.probe	= hello_key_probe,
	.driver	= {
		.name		= "hello_key",
		.of_match_table	= hello_key_of_match,
	},
};
module_platform_driver(hello_key_driver);
