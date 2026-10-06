// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 阶段 4 第一版：GPIO 中断 + 阻塞 read + poll =====================
 *
 *  硬件：杜邦线把 GPIO3_B0（输出，hello_gpio 驱动控制）和 GPIO3_D4（输入，本驱动）短接。
 *        echo 1 > /sys/devices/platform/hello-gpio/value  →  GPIO3_D4 出现上升沿  →  中断
 *
 *  设备树（rk3576-kickpi-k7.dtsi）：
 *    hello-irq {
 *        compatible = "zhouyllll,hello-irq";
 *        irq-gpios = <&gpio3 RK_PD4 GPIO_ACTIVE_HIGH>;
 *        pinctrl-0 = <&hello_irq_pin>;    ← 下拉：没接线时引脚不悬空，不会乱触发中断
 *    };
 *
 *  insmod hello_irq.ko → hello_irq_probe()
 *    ├─ devm_gpiod_get(dev, "irq", GPIOD_IN)       拿到输入引脚
 *    ├─ gpiod_to_irq()                             引脚 → 中断号
 *    ├─ init_waitqueue_head()                      准备一个"等待队列"，read 没数据时睡在这里
 *    ├─ devm_request_irq(..., IRQF_TRIGGER_RISING) 注册中断处理函数，只在上升沿触发
 *    └─ misc_register()                            → /dev/hello_irq
 *
 *  用户态 cat /dev/hello_irq
 *    └─ hello_irq_read()
 *         └─ 没有新中断 → wait_event_interruptible() 进程睡眠（不占 CPU）
 *
 *  上升沿到来 → GIC → gpio-rockchip 的中断分发 → hello_irq_handler()（中断上下文）
 *    ├─ atomic_inc(&count)                         计数 +1
 *    └─ wake_up_interruptible(&wq)                 叫醒睡在 wq 上的进程
 *         └─ read 从 wait_event_interruptible 返回，把计数交给用户态
 *
 *  三种执行上下文（面试必问，这个驱动里全都有）：
 *    进程上下文：probe、open、read、poll——可以睡眠
 *    硬中断上下文：hello_irq_handler——不能睡眠，要尽快返回
 *    （软中断/tasklet 上下文：练习 ② 再加）
 * ===================================================================================
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>			/* of_device_id */
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>		/* devm_request_irq、irqreturn_t、IRQF_* */
#include <linux/wait.h>			/* wait_queue_head_t、wait_event_interruptible、wake_up_interruptible */
#include <linux/poll.h>			/* poll_wait、EPOLLIN */
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/delay.h>		/* msleep：只用于调试练习 */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("GPIO interrupt + blocking read + poll exercise");

/*
 * 调试练习：故意在硬中断里睡眠。默认关闭，insmod hello_irq.ko bug_sleep_in_irq=1 才会触发。
 * 预期看到 "BUG: scheduling while atomic"——硬中断上下文不能睡眠。
 */
static bool bug_sleep_in_irq;
module_param(bug_sleep_in_irq, bool, 0444);
MODULE_PARM_DESC(bug_sleep_in_irq, "DEBUG EXERCISE: call msleep() inside the hard IRQ handler");

/* 每个设备一份 */
struct hello_irq_dev {
	struct miscdevice miscdev;
	struct gpio_desc *gpiod;
	int irq;
	/*
	 * 【新写法】atomic_t：中断处理函数和 read 会同时访问 count。
	 * atomic_inc/atomic_read 是一条不可分割的操作，不会出现"读到一半被中断改了"。
	 * 只有一个整数时用它就够；要保护几个变量一起改，就得用锁（阶段 5）。
	 */
	atomic_t count;
	/*
	 * 【新写法】等待队列：一个"排队睡觉的地方"。
	 * read 没数据时把当前进程挂到这里睡眠；中断来了 wake_up 把它们叫醒。
	 */
	wait_queue_head_t wq;
};

/*
 * 每次 open 一份：记住"这个 fd 上次读到第几次中断"。
 * 这样两个进程同时 cat，每个都能看到每一次中断（跟阶段 1 的 per-open private_data 同一个思路）。
 */
struct hello_irq_priv {
	struct hello_irq_dev *hdev;
	int seen;
};

/* ============================ 中断处理函数 ============================ */

/*
 * 【新写法】中断处理函数（硬中断上下文，hard IRQ）。
 *
 * 规则：
 *   - 不能睡眠：不能用 mutex、msleep、kmalloc(GFP_KERNEL)、copy_to_user……
 *     （阶段 4 的调试练习就是故意在这里调 msleep，看 "scheduling while atomic"）
 *   - 要快：中断处理期间，这个 CPU 上的同级中断都在等它
 *   - 返回值：IRQ_HANDLED 表示"是我的中断，处理了"；共享中断线时，不是自己的要返回 IRQ_NONE
 *
 * 这里只做两件最快的事：计数 +1，叫醒等待的进程。真正的工作（拷数据给用户）留在 read 里做。
 * dev_id 就是 devm_request_irq 最后一个参数传进来的指针。
 */
static irqreturn_t hello_irq_handler(int irq, void *dev_id)
{
	struct hello_irq_dev *hdev = dev_id;

	if (bug_sleep_in_irq)
		msleep(10);	/* 错误示范：msleep 会让出 CPU 去调度别的进程，硬中断里不允许 */

	atomic_inc(&hdev->count);
	wake_up_interruptible(&hdev->wq);
	return IRQ_HANDLED;
}

/* ============================ file_operations ============================ */

static int hello_irq_open(struct inode *inode, struct file *filp)
{
	/* misc 在进入 open 时把 private_data 设成了 &miscdev，跟阶段 2 一样用 container_of 取回 */
	struct hello_irq_dev *hdev = container_of(filp->private_data, struct hello_irq_dev, miscdev);
	struct hello_irq_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hdev = hdev;
	priv->seen = atomic_read(&hdev->count);	/* 只关心 open 之后发生的中断 */
	filp->private_data = priv;

	/*
	 * 【新写法】stream_open：声明这是一个"流"设备，没有文件位置的概念（像管道、串口）。
	 * 这样 read 不用管 *ppos，cat 读完一次会接着读，读不到就睡，来一次中断打印一行。
	 */
	return stream_open(inode, filp);
}

static int hello_irq_release(struct inode *inode, struct file *filp)
{
	kfree(filp->private_data);
	return 0;
}

/* "有没有新中断"：wait_event 和 poll 用的是同一个条件 */
static bool hello_irq_has_new(struct hello_irq_priv *priv)
{
	return atomic_read(&priv->hdev->count) != priv->seen;
}

/*
 * 阻塞 read：没有新中断就睡，有了就返回"累计第几次中断"（文本，方便 cat）。
 */
static ssize_t hello_irq_read(struct file *filp, char __user *buf, size_t size, loff_t *ppos)
{
	struct hello_irq_priv *priv = filp->private_data;
	struct hello_irq_dev *hdev = priv->hdev;
	char kbuf[16];
	int now, len;

	if (!hello_irq_has_new(priv)) {
		/* 用户用 O_NONBLOCK 打开：不睡，直接告诉他"现在没有"，让他稍后再试 */
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		/*
		 * 【新写法】wait_event_interruptible(wq, 条件)：
		 *   条件为真 → 立刻返回 0；
		 *   条件为假 → 把当前进程挂到 wq 上睡眠，被 wake_up 叫醒后重新检查条件，
		 *              还是假就接着睡（所以被"误叫醒"也没关系）；
		 *   睡眠中收到信号（比如 Ctrl-C）→ 返回 -ERESTARTSYS。
		 * 这就是"中断来了才唤醒阻塞的 read"：睡眠期间进程不占 CPU。
		 */
		if (wait_event_interruptible(hdev->wq, hello_irq_has_new(priv)))
			return -ERESTARTSYS;
	}

	now = atomic_read(&hdev->count);
	priv->seen = now;

	len = scnprintf(kbuf, sizeof(kbuf), "%d\n", now);
	if (size < len)
		return -EINVAL;
	if (copy_to_user(buf, kbuf, len))
		return -EFAULT;
	return len;
}

/*
 * 【新写法】poll：给 select/poll/epoll 用，回答"现在读会不会阻塞"。
 *
 * 两步，缺一不可：
 *   ① poll_wait(filp, &wq, wait)：把这个等待队列登记给 poll 机制。它本身不睡眠，
 *      只是告诉内核"有变化时请从这个队列叫醒调用 poll 的进程"。
 *   ② 返回当前状态：有新中断就返回 EPOLLIN（可读），没有就返回 0。
 * 内核拿到 0 会让调用 poll 的进程睡眠；中断里的 wake_up 唤醒它后，内核会再调一次这个函数。
 */
static __poll_t hello_irq_poll(struct file *filp, poll_table *wait)
{
	struct hello_irq_priv *priv = filp->private_data;

	poll_wait(filp, &priv->hdev->wq, wait);
	return hello_irq_has_new(priv) ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static const struct file_operations hello_irq_fops = {
	.owner		= THIS_MODULE,
	.open		= hello_irq_open,
	.release	= hello_irq_release,
	.read		= hello_irq_read,
	.poll		= hello_irq_poll,
	.llseek		= no_llseek,
};

/* ============================ probe / remove ============================ */

static int hello_irq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hello_irq_dev *hdev;
	int ret;

	hdev = devm_kzalloc(dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev)
		return -ENOMEM;

	/* GPIOD_IN：输入。上下拉由设备树的 pinctrl-0 决定（下拉），不在这里设 */
	hdev->gpiod = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(hdev->gpiod))
		return dev_err_probe(dev, PTR_ERR(hdev->gpiod), "failed to get irq-gpios\n");
	gpiod_set_consumer_name(hdev->gpiod, "hello-irq");

	/*
	 * 【新写法】gpiod_to_irq：引脚 → Linux 中断号（virq）。
	 * GPIO 控制器本身也是一个中断控制器（rk3576.dtsi 里 gpio3 节点有 interrupt-controller），
	 * 它把 32 个引脚各映射成一个中断号。返回负数表示失败。
	 */
	hdev->irq = gpiod_to_irq(hdev->gpiod);
	if (hdev->irq < 0)
		return dev_err_probe(dev, hdev->irq, "failed to map gpio to irq\n");

	/*
	 * 中断随时可能来，handler 里要用的东西必须在 request_irq 之前全部初始化好。
	 * 顺序反了的话，request_irq 返回前中断就可能触发，handler 访问到没初始化的 wq → 崩溃。
	 */
	atomic_set(&hdev->count, 0);
	init_waitqueue_head(&hdev->wq);

	/*
	 * 【新写法】devm_request_irq(dev, 中断号, 处理函数, 标志, 名字, dev_id)
	 *   IRQF_TRIGGER_RISING：上升沿触发（还有 FALLING、BOTH 边沿，HIGH/LOW 电平）
	 *   名字 "hello_irq"：会出现在 /proc/interrupts 里
	 *   dev_id = hdev：原样传给 handler 的第二个参数
	 *   devm：解绑时自动 free_irq
	 */
	ret = devm_request_irq(dev, hdev->irq, hello_irq_handler,
			       IRQF_TRIGGER_RISING, "hello_irq", hdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq %d\n", hdev->irq);

	hdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	hdev->miscdev.name = "hello_irq";
	hdev->miscdev.fops = &hello_irq_fops;
	hdev->miscdev.parent = dev;

	platform_set_drvdata(pdev, hdev);

	/* 放最后：注册成功的瞬间 /dev/hello_irq 就能被打开 */
	ret = misc_register(&hdev->miscdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register misc device\n");

	dev_info(dev, "gpio %d -> irq %d, /dev/%s ready\n",
		 desc_to_gpio(hdev->gpiod), hdev->irq, hdev->miscdev.name);
	return 0;
}

/*
 * misc 不是 devm 的，要手动注销。之后 devm 按申请的相反顺序释放：
 * 先 free_irq（中断不会再来），再释放 hdev 内存——所以 handler 不会访问到已释放的 hdev。
 */
static int hello_irq_remove(struct platform_device *pdev)
{
	struct hello_irq_dev *hdev = platform_get_drvdata(pdev);

	misc_deregister(&hdev->miscdev);
	return 0;
}

static const struct of_device_id hello_irq_of_match[] = {
	{ .compatible = "zhouyllll,hello-irq" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hello_irq_of_match);

static struct platform_driver hello_irq_driver = {
	.probe	= hello_irq_probe,
	.remove	= hello_irq_remove,
	.driver	= {
		.name		= "hello_irq",
		.of_match_table	= hello_irq_of_match,
	},
};
module_platform_driver(hello_irq_driver);
