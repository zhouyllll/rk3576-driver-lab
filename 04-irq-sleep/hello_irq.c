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
#include <linux/delay.h>		/* msleep */
#include <linux/workqueue.h>		/* work_struct、INIT_WORK、schedule_work、cancel_work_sync */
#include <linux/ktime.h>		/* ktime_get、ktime_us_delta */
#include <linux/sched.h>		/* current、task_pid_nr */
#include <linux/timer.h>		/* timer_list、timer_setup、mod_timer、del_timer_sync */
#include <linux/jiffies.h>		/* jiffies、msecs_to_jiffies */
#include <linux/spinlock.h>		/* spinlock_t、spin_lock、spin_lock_irqsave */
#include <linux/device.h>		/* DEVICE_ATTR_RW、ATTRIBUTE_GROUPS */

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

/*
 * 练习 ②：下半部选哪种。insmod hello_irq.ko bh=N
 *   0：不用下半部，全部在硬中断里做（第一版的做法）
 *   1：tasklet          —— 软中断上下文，不能睡眠
 *   2：workqueue        —— kworker 内核线程，进程上下文，可以睡眠
 *   3：线程化中断        —— 专用内核线程 irq/<virq>-hello_irq，进程上下文，可以睡眠
 */
enum {
	HELLO_BH_NONE,
	HELLO_BH_TASKLET,
	HELLO_BH_WORK,
	HELLO_BH_THREAD,
};
static int bh = HELLO_BH_NONE;
module_param(bh, int, 0444);
MODULE_PARM_DESC(bh, "bottom half: 0=none 1=tasklet 2=workqueue 3=threaded irq");

/*
 * 练习 ③：消抖。insmod hello_irq.ko debounce_ms=20
 * 0 表示不消抖。跟 bh 二选一（消抖的"下半部"就是定时器回调）。
 */
static unsigned int debounce_ms;
module_param(debounce_ms, uint, 0444);
MODULE_PARM_DESC(debounce_ms, "debounce time in ms, 0 = off (only with bh=0)");

/*
 * 练习 ③ 后半：测量相邻两次中断的间隔，配合 hello_gpio 的 hrtimer 方波测周期精度。
 * insmod hello_irq.ko measure=1，结果在 /sys/devices/platform/hello-irq/irq_stats。
 * 打开后上半部不再逐条打印（1kHz 打印会把控制台淹没），只记统计。
 */
static bool measure;
module_param(measure, bool, 0444);
MODULE_PARM_DESC(measure, "record interval statistics between interrupts (only with bh=0, no debounce)");

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

	/* ---- 练习 ② 下半部用到的 ---- */
	struct device *dev;			/* 给下半部里的 dev_info 用 */
	/*
	 * 上半部记下的时间，下半部用来算"晚了多久"。
	 * 只有上半部写、下半部读，单个变量 → READ_ONCE/WRITE_ONCE 就够（不需要原子或锁）
	 */
	ktime_t t_top;
	struct tasklet_struct tasklet;		/* bh=1 */
	struct work_struct work;		/* bh=2 */

	/* ---- 练习 ③ 消抖用到的 ---- */
	struct timer_list debounce;		/* 最后一个边沿之后 debounce_ms 到期 */
	/*
	 * 收到的原始边沿数（包括抖动），只在上半部 ++，定时器回调里读后清零。
	 * 两个执行流（硬中断、定时器软中断）可能在不同 CPU 上同时访问 → atomic_t
	 */
	atomic_t raw_edges;

	/*
	 * ---- 练习 ③ 后半：间隔统计 ----
	 * 【新写法，阶段 5 预习】spinlock：这几个字段要"一起"更新、"一起"读，
	 * 否则 sysfs 读到的可能是 n 已经 +1、sum 还没加上的半截状态。atomic_t 只能保护单个整数，管不了一组。
	 * 写的一方在硬中断里，读的一方（sysfs show）在进程上下文，所以：
	 *   硬中断里：spin_lock / spin_unlock（中断处理期间本 CPU 的中断本来就是关的）
	 *   进程上下文：spin_lock_irqsave —— 必须同时关本 CPU 的中断，否则持锁期间中断进来、
	 *   handler 再去拿同一把锁，就在同一个 CPU 上自己等自己，死锁
	 */
	spinlock_t stats_lock;
	ktime_t prev_edge;		/* 上一次中断的时间，0 = 还没有 */
	u64 n, min_ns, max_ns, sum_ns;
};

/*
 * 每次 open 一份：记住"这个 fd 上次读到第几次中断"。
 * 这样两个进程同时 cat，每个都能看到每一次中断（跟阶段 1 的 per-open private_data 同一个思路）。
 */
struct hello_irq_priv {
	struct hello_irq_dev *hdev;
	int seen;
};

/* ============================ 中断处理：上半部 + 下半部 ============================ */

/*
 * 真正的"处理"：不管哪种下半部，最后都调到这里。
 * 打印自己运行在谁身上、什么上下文、比上半部晚了多少，用来对比四种方式。
 *
 *   current          ：当前 CPU 上正在运行的线程（task_struct）。在中断/软中断里，它是被"借用"的那个线程
 *   preempt_count()  ：bit 8-15 软中断计数，bit 16-19 硬中断计数（见 04 笔记）
 *   in_task()        ：是不是进程上下文（不在硬中断、软中断、NMI 里）——只有这时才能睡眠
 */
static void hello_irq_do_bottom(struct hello_irq_dev *hdev, const char *how)
{
	s64 delay_us = ktime_us_delta(ktime_get(), READ_ONCE(hdev->t_top));

	dev_info(hdev->dev, "%-8s comm=%-16s pid=%-4d preempt=0x%08x in_task=%d delay=%lldus\n",
		 how, current->comm, task_pid_nr(current), preempt_count(), !!in_task(), delay_us);

	/*
	 * 进程上下文里才能睡：workqueue 和线程化中断走这里，合法。
	 * 硬中断（bh=0）和 tasklet（bh=1）会跳过——在它们里面 msleep 就是上一个调试练习的 BUG。
	 */
	if (in_task())
		/*
		 * 【新写法】usleep_range(min, max)：基于高精度定时器（hrtimer）的睡眠，适合 10us~20ms。
		 * 不用 msleep(5)：msleep 按 jiffy 计时，本内核 HZ=250，一个 jiffy 是 4ms，
		 * msleep(5) 实际会睡 8~12ms（checkpatch 会警告，见 Documentation/timers/timers-howto.rst）。
		 * 给一个范围而不是一个精确值，是为了让内核把相近的定时器合并唤醒，省电。
		 */
		usleep_range(5000, 6000);

	atomic_inc(&hdev->count);
	wake_up_interruptible(&hdev->wq);
}

/*
 * 【新写法】tasklet 回调。
 * 硬中断里 tasklet_schedule() 只是登记一下；硬中断返回时内核处理软中断，就在那时调用它。
 * 参数是 tasklet_struct 的地址，from_tasklet 就是 container_of 的包装，找回 hdev。
 */
static void hello_irq_tasklet(struct tasklet_struct *t)
{
	struct hello_irq_dev *hdev = from_tasklet(hdev, t, tasklet);

	hello_irq_do_bottom(hdev, "tasklet");
}

/*
 * 【新写法】workqueue 回调。
 * schedule_work() 把 work 挂到系统默认的工作队列上，由某个 kworker 内核线程取出来执行。
 * 参数是 work_struct 的地址，同样 container_of 找回 hdev。
 */
static void hello_irq_work(struct work_struct *work)
{
	struct hello_irq_dev *hdev = container_of(work, struct hello_irq_dev, work);

	hello_irq_do_bottom(hdev, "work");
}

/*
 * 【新写法】线程化中断的"线程函数"：上半部返回 IRQ_WAKE_THREAD 后，
 * 内核唤醒为这个中断专门创建的线程 irq/<virq>-hello_irq 来执行它。
 * dev_id 跟上半部一样，是 request 时传的 hdev。
 */
static irqreturn_t hello_irq_thread(int irq, void *dev_id)
{
	hello_irq_do_bottom(dev_id, "thread");
	return IRQ_HANDLED;
}

/*
 * 上半部（硬中断上下文）。
 *
 * 规则：
 *   - 不能睡眠：不能用 mutex、msleep、kmalloc(GFP_KERNEL)、copy_to_user……
 *   - 要快：中断处理期间，这个 CPU 上的同级中断都在等它
 *   - 返回值：IRQ_HANDLED 表示"是我的中断，处理了"；共享中断线时不是自己的要返回 IRQ_NONE；
 *     线程化中断返回 IRQ_WAKE_THREAD，表示"请唤醒我的线程"
 *
 * 上半部只做最紧急的：记下时间，然后按 bh 参数把剩下的工作交出去。
 */
static irqreturn_t hello_irq_handler(int irq, void *dev_id)
{
	struct hello_irq_dev *hdev = dev_id;

	if (bug_sleep_in_irq)
		msleep(20);	/* 错误示范：msleep 会让出 CPU 去调度别的进程，硬中断里不允许 */

	WRITE_ONCE(hdev->t_top, ktime_get());
	atomic_inc(&hdev->raw_edges);

	if (measure) {
		ktime_t now = hdev->t_top;

		spin_lock(&hdev->stats_lock);
		if (hdev->prev_edge) {
			u64 d = ktime_to_ns(ktime_sub(now, hdev->prev_edge));

			hdev->n++;
			hdev->sum_ns += d;
			hdev->min_ns = min(hdev->min_ns, d);
			hdev->max_ns = max(hdev->max_ns, d);
		}
		hdev->prev_edge = now;
		spin_unlock(&hdev->stats_lock);

		atomic_inc(&hdev->count);
		wake_up_interruptible(&hdev->wq);
		return IRQ_HANDLED;
	}

	if (debounce_ms) {
		/*
		 * 每个边沿都把到期时间往后推：抖动期间定时器一直到不了期。
		 * mod_timer 对还没到期的定时器调用，效果就是"重新设定到期时间"。
		 */
		mod_timer(&hdev->debounce, jiffies + msecs_to_jiffies(debounce_ms));
		return IRQ_HANDLED;
	}

	switch (bh) {
	case HELLO_BH_TASKLET:
		tasklet_schedule(&hdev->tasklet);
		return IRQ_HANDLED;
	case HELLO_BH_WORK:
		schedule_work(&hdev->work);
		return IRQ_HANDLED;
	case HELLO_BH_THREAD:
		return IRQ_WAKE_THREAD;
	default:
		hello_irq_do_bottom(hdev, "hardirq");	/* 全部在硬中断里做 */
		return IRQ_HANDLED;
	}
}

/*
 * 【新写法】配合 devm_add_action_or_reset 的清理函数：保证下半部不会在 hdev 释放后还运行。
 *   tasklet_kill    ：等已登记的 tasklet 跑完，并且不再执行
 *   cancel_work_sync：取消还没执行的 work；正在执行的，等它跑完
 */
static void hello_irq_bh_cleanup(void *data)
{
	struct hello_irq_dev *hdev = data;

	tasklet_kill(&hdev->tasklet);
	cancel_work_sync(&hdev->work);
	del_timer_sync(&hdev->debounce);	/* 取消定时器；回调正在跑的话等它跑完 */
}

/*
 * 【新写法】定时器回调：信号最后一次跳变之后 debounce_ms 都没有新的边沿，才会走到这里。
 * 运行在软中断上下文（跟 tasklet 一样），不能睡眠。
 *
 * 到期后再读一次引脚：抖动结束后稳定在高电平，才算一次有效的"按下"。
 * 读引脚用不带 _cansleep 的 gpiod_get_value：这里不能睡。gpio-rockchip 是 SoC 内部的控制器，
 * 访问不会睡眠，所以可以这样用；要是换成 I2C 扩展的 GPIO，就只能把这部分挪到 workqueue 里。
 */
static void hello_irq_debounce(struct timer_list *t)
{
	struct hello_irq_dev *hdev = from_timer(hdev, t, debounce);
	int raw = atomic_xchg(&hdev->raw_edges, 0);	/* 取出这一轮收到的边沿数，同时清零，一步完成 */

	if (gpiod_get_value(hdev->gpiod) != 1) {
		dev_info(hdev->dev, "debounce: %d raw edge(s), level low at expiry -> ignored\n", raw);
		return;
	}
	dev_info(hdev->dev, "debounce: %d raw edge(s) -> 1 event\n", raw);
	hello_irq_do_bottom(hdev, "timer");
}

/* ============================ sysfs：irq_stats ============================ */

static void hello_irq_stats_reset(struct hello_irq_dev *hdev)
{
	hdev->prev_edge = 0;
	hdev->n = 0;
	hdev->sum_ns = 0;
	hdev->min_ns = U64_MAX;
	hdev->max_ns = 0;
}

/* cat irq_stats → "间隔个数 最小 最大 平均"，单位 ns */
static ssize_t irq_stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hello_irq_dev *hdev = dev_get_drvdata(dev);
	unsigned long flags;
	u64 n, mn, mx, sum;

	spin_lock_irqsave(&hdev->stats_lock, flags);	/* 拷一份快照，持锁时间越短越好 */
	n = hdev->n;
	mn = hdev->min_ns;
	mx = hdev->max_ns;
	sum = hdev->sum_ns;
	spin_unlock_irqrestore(&hdev->stats_lock, flags);

	if (!n)
		return sysfs_emit(buf, "n=0\n");
	return sysfs_emit(buf, "n=%llu min=%lluns max=%lluns avg=%lluns\n", n, mn, mx, div64_u64(sum, n));
}

/* echo 0 > irq_stats → 清零重新统计 */
static ssize_t irq_stats_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct hello_irq_dev *hdev = dev_get_drvdata(dev);
	unsigned long flags;

	spin_lock_irqsave(&hdev->stats_lock, flags);
	hello_irq_stats_reset(hdev);
	spin_unlock_irqrestore(&hdev->stats_lock, flags);
	return count;
}
static DEVICE_ATTR_RW(irq_stats);

static struct attribute *hello_irq_attrs[] = {
	&dev_attr_irq_stats.attr,
	NULL,
};
ATTRIBUTE_GROUPS(hello_irq);

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
	hdev->dev = dev;
	tasklet_setup(&hdev->tasklet, hello_irq_tasklet);
	INIT_WORK(&hdev->work, hello_irq_work);
	timer_setup(&hdev->debounce, hello_irq_debounce, 0);
	atomic_set(&hdev->raw_edges, 0);
	spin_lock_init(&hdev->stats_lock);
	hello_irq_stats_reset(hdev);

	if (bh < HELLO_BH_NONE || bh > HELLO_BH_THREAD)
		return dev_err_probe(dev, -EINVAL, "invalid bh=%d (0..3)\n", bh);
	if (debounce_ms && bh != HELLO_BH_NONE)
		return dev_err_probe(dev, -EINVAL, "debounce_ms only works with bh=0\n");
	if (measure && (bh != HELLO_BH_NONE || debounce_ms))
		return dev_err_probe(dev, -EINVAL, "measure only works with bh=0 and no debounce\n");

	/*
	 * 【新写法】devm_add_action_or_reset：把自己的清理函数登记进 devm 链表。
	 * devm 按"登记的相反顺序"释放，这里登记在 request_irq 之前，所以解绑时：
	 *   先 free_irq（中断不会再来，也就不会再 schedule 下半部）
	 *   再 hello_irq_bh_cleanup（把已经登记的 tasklet/work 收尾）
	 *   最后释放 hdev 内存
	 * 顺序反了的话：work 收尾之后中断又来一次、又 schedule_work，hdev 释放后 work 才跑 → use-after-free。
	 * _or_reset：登记失败时立刻调用一次清理函数，所以失败直接 return 即可。
	 */
	ret = devm_add_action_or_reset(dev, hello_irq_bh_cleanup, hdev);
	if (ret)
		return ret;

	/*
	 * 【新写法】devm_request_irq(dev, 中断号, 处理函数, 标志, 名字, dev_id)
	 *   IRQF_TRIGGER_RISING：上升沿触发（还有 FALLING、BOTH 边沿，HIGH/LOW 电平）
	 *   名字 "hello_irq"：会出现在 /proc/interrupts 里
	 *   dev_id = hdev：原样传给 handler 的第二个参数
	 *   devm：解绑时自动 free_irq
	 */
	if (bh == HELLO_BH_THREAD)
		/*
		 * 【新写法】线程化中断：多一个参数 thread_fn。
		 * IRQF_ONESHOT：线程函数跑完之前，这根中断线保持屏蔽。
		 * 电平触发的设备必须加（不屏蔽的话，线程还没来得及清掉硬件的中断源，电平还在，中断会一直重进）；
		 * 上半部传 NULL 时内核也强制要求加。
		 */
		ret = devm_request_threaded_irq(dev, hdev->irq, hello_irq_handler, hello_irq_thread,
						IRQF_TRIGGER_RISING | IRQF_ONESHOT, "hello_irq", hdev);
	else
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

	dev_info(dev, "gpio %d -> irq %d, /dev/%s ready, bh=%d debounce_ms=%u\n",
		 desc_to_gpio(hdev->gpiod), hdev->irq, hdev->miscdev.name, bh, debounce_ms);
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
		.dev_groups	= hello_irq_groups,	/* → /sys/devices/platform/hello-irq/irq_stats */
	},
};
module_platform_driver(hello_irq_driver);
