// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 阶段 5 练习 ①：亲眼看到竞争条件，再用 spinlock 修好 =====================
 *
 * 【这是骨架】框架和注释都写好了，标 TODO 的地方由你来写。
 * 先按 TODO 的编号顺序写，每写完一部分就 make 一次，确保能编译通过。
 *
 * 场景：
 *   中断（写方）每次更新一组数据 struct hello_snapshot：seq、ts_ns、level、seq_check
 *   read（读方）把这一组数据拷给用户态
 *   这组数据有 4 个字段，必须"一起写完"、"一起读到"——atomic_t 只能保护单个整数，管不了一组。
 *
 * 实验：
 *   1. use_lock=0：不加锁。hello_gpio 的 hrtimer 用 5kHz 触发中断，test_race 疯狂 read，
 *      统计 seq != seq_check 的次数（撕裂读）。预期：大于 0
 *   2. use_lock=1：加锁。同样的压力，预期：撕裂读 = 0
 *
 * 为了让竞争更容易出现，写方和读方在两个字段之间都故意 ndelay(slow_ns)，把"写了一半"的窗口拉长。
 * 真实代码里不会有这个延时，但窗口再小，只要存在，就迟早会撞上——只是你可能要跑几天才看得到。
 *
 * 硬件和设备树：复用阶段 4 的接线（33 号 GPIO3_B0 ↔ 23 号 GPIO3_D4）和 hello-irq 节点。
 * 注意 gpio-keys 开机就占着 GPIO3_D4，加载前先：
 *   echo hello-gpio-keys > /sys/bus/platform/drivers/gpio-keys/unbind
 * ===============================================================================================
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>		/* spinlock_t、spin_lock、spin_lock_irqsave */
#include <linux/delay.h>		/* ndelay：忙等，不睡眠，原子上下文里可以用 */
#include <linux/ktime.h>		/* ktime_get_ns */
#include "hello_race.h"
#include <linux/hrtimer.h>
#include <linux/completion.h>	/* 练习 ②：completion、init_completion、complete、wait_for_completion_* */
#include <linux/atomic.h>	/* 练习 ②：atomic_t、atomic_dec_if_positive、atomic_cmpxchg */
#include <linux/jiffies.h>	/* 练习 ②：msecs_to_jiffies */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("Stage 5: race condition between IRQ and read, fixed with spinlock");

/* insmod hello_race.ko use_lock=0/1 slow_ns=N */
static bool use_lock;
module_param(use_lock, bool, 0444);
MODULE_PARM_DESC(use_lock, "protect the snapshot with a spinlock");

static bool use_timer;
module_param(use_timer, bool, 0444);
MODULE_PARM_DESC(use_timer, "use an internal hrtimer (hard IRQ context) instead of the GPIO interrupt as the writer");

static unsigned int slow_ns = 1000;
module_param(slow_ns, uint, 0444);
MODULE_PARM_DESC(slow_ns, "artificial delay between field updates, widens the race window");

static unsigned int period_us = 100;
module_param(period_us, uint, 0444);
MODULE_PARM_DESC(period_us, "hrtimer period in microseconds when use_timer=1 (default 100)");

/* 练习 ②：ioctl 等待的超时时间（已写好） */
static unsigned int wait_timeout_ms = 3000;
module_param(wait_timeout_ms, uint, 0444);
MODULE_PARM_DESC(wait_timeout_ms, "timeout of HELLO_RACE_WAIT_N in ms (default 3000)");

struct hello_race_dev {
	struct miscdevice miscdev;
	struct gpio_desc *gpiod;
	int irq;

	/*
	 * 保护 snap 的锁。
	 * 写方：中断处理函数（硬中断上下文）
	 * 读方：read（进程上下文）
	 * 谁写、谁读、什么上下文、靠什么保护——阶段 4 笔记里说的习惯，在这里写清楚。
	 */
	spinlock_t lock;
	struct hrtimer timer;

	/*
	 * ---- 练习 ②：HELLO_RACE_WAIT_N（成员已经加好，用法见各处 TODO）----
	 *
	 * done       ：【新写法】completion。ioctl 在它上面等，中断里数到 0 时 complete() 它
	 * remaining  ：还要再等几次中断。ioctl（进程上下文）设成 N，中断里每来一次减 1 → 单个整数，用 atomic_t
	 * waiter_busy：0 = 没人在等，1 = 有人在等。保证同一时刻只有一个等待者（两个进程同时 ioctl，只能一个抢到）
	 */
	struct completion done;
	atomic_t remaining;
	atomic_t waiter_busy;
	struct hello_snapshot snap;	/* 被 lock 保护（use_lock=1 时） */
};

/* ============================ 写方：中断处理函数 ============================ */

/*
 * __maybe_unused：骨架阶段 probe 还没调用 request_irq，这个函数没人用，内核编译选项 -Werror 会报错。
 * 写完 TODO 3（probe 里 devm_request_irq 用到它）之后，把 __maybe_unused 删掉。
 */
static void hello_race_update(struct hello_race_dev *hdev)
{
	if (use_lock)
		spin_lock(&hdev->lock);

	hdev->snap.seq++;
	ndelay(slow_ns);

	hdev->snap.ts_ns = ktime_get_ns();
	ndelay(slow_ns);

	if (hdev->gpiod)
		hdev->snap.level = gpiod_get_value(hdev->gpiod);
	ndelay(slow_ns);

	hdev->snap.seq_check = hdev->snap.seq;

	/*
	 * TODO ②-2（写在下面解锁之后，放在锁外就行，它不涉及 snap）：
	 * 有人在等的话，把 remaining 减 1；减到 0 的那一次，complete(&hdev->done)
	 *
	 * 【新写法】atomic_dec_if_positive(&v)：只有 v > 0 时才减 1，返回"减之后的值"；v 已经是 0 时不减，返回 -1。
	 *     返回 0   → 刚好从 1 减到 0：这是第 N 次，该通知了
	 *     返回 > 0 → 还没数完
	 *     返回 -1  → 没人在等（remaining 本来就是 0），什么都不做
	 * 思考：为什么不能写成 if (atomic_read(&v) > 0) atomic_dec(&v); ？（提示：两步之间会发生什么？）
	 *
	 * 【新写法】complete(&done)：标记"完成了"，并叫醒在 done 上等的进程。不睡眠，中断里可以调用。
	 */
	if (use_lock)
		spin_unlock(&hdev->lock);

	if (atomic_dec_if_positive(&hdev->remaining) == 0)
		complete(&hdev->done);
}
static irqreturn_t hello_race_isr(int irq, void *dev_id)
{
	/*
	 * TODO 1：中断里更新快照
	 *
	 * 1. 从 dev_id 拿到 hdev（阶段 4 学的第三种"找回"方式）
	 * 2. 如果 use_lock，加锁。
	 *    思考：这里该用 spin_lock 还是 spin_lock_irqsave？
	 *    提示：硬中断处理函数执行期间，本 CPU 的中断是什么状态？
	 * 3. 按这个顺序写字段，每两步之间 ndelay(slow_ns)：
	 *      snap.seq 加 1
	 *      snap.ts_ns = ktime_get_ns()
	 *      snap.level = 引脚电平（硬中断里不能睡，用哪个 gpiod_get_value？）
	 *      snap.seq_check = snap.seq        ← 一定最后写
	 * 4. 如果加了锁，解锁（用跟第 2 步配对的那个解锁函数）
	 * 5. 返回 IRQ_HANDLED
	 *
	 * 注意：中断里不要 dev_info。5kHz 的中断，每次都打印的话控制台会被淹没，系统也会被拖垮。
	 */
	struct hello_race_dev *hdev = dev_id;

	hello_race_update(hdev);
	return IRQ_HANDLED;
}

/* ============================ 读方：file_operations ============================ */

static int hello_race_open(struct inode *inode, struct file *filp)
{
	/*
	 * 已经写好：跟阶段 4 一样，misc 把 private_data 设成了 &miscdev。
	 * 这里不需要 per-open 的数据，直接把 private_data 换成 hdev，read 里就能直接用。
	 */
	struct hello_race_dev *hdev = container_of(filp->private_data, struct hello_race_dev, miscdev);

	filp->private_data = hdev;
	return stream_open(inode, filp);
}

static ssize_t hello_race_read(struct file *filp, char __user *buf, size_t size, loff_t *ppos)
{
	/*
	 * TODO 2：把快照拷给用户态
	 *
	 * 1. 从 filp->private_data 拿到 hdev
	 * 2. 用户给的 size 必须 >= sizeof(struct hello_snapshot)，不够就返回 -EINVAL
	 * 3. 在栈上准备一个 struct hello_snapshot 局部变量 local
	 * 4. 如果 use_lock，加锁。
	 *    思考：这里该用 spin_lock 还是 spin_lock_irqsave？
	 *    提示：read 在进程上下文。持锁期间，如果中断恰好在【同一个 CPU】上进来，
	 *          中断处理函数也要拿这把锁，会发生什么？
	 *    提示 2：spin_lock_irqsave 的 flags 参数是个变量，不是指针（它是宏），先声明 unsigned long flags
	 * 5. 一个字段一个字段地拷到 local，每两个字段之间 ndelay(slow_ns)，顺序跟写方一样：
	 *      seq → ts_ns → level → seq_check
	 *    （直接 local = hdev->snap 也可以，但编译器可能一次拷完，竞争窗口太小，看不到效果）
	 * 6. 如果加了锁，解锁
	 * 7. copy_to_user(buf, &local, sizeof(local))，失败返回 -EFAULT
	 *    思考：为什么 copy_to_user 必须放在解锁【之后】，而不能在持锁时直接拷 hdev->snap？
	 *    提示：copy_to_user 碰到用户页不在内存里时会怎样？持有 spinlock 时能做那件事吗？
	 * 8. 返回 sizeof(local)
	 */
	struct hello_race_dev *hdev = filp->private_data;
	unsigned long flags;
	struct hello_snapshot local = {};

	if (size < sizeof(struct hello_snapshot))
		return -EINVAL;

	if (use_lock)
		spin_lock_irqsave(&hdev->lock, flags);

	local.seq = hdev->snap.seq;
	ndelay(slow_ns);

	local.ts_ns = hdev->snap.ts_ns;
	ndelay(slow_ns);

	local.level = hdev->snap.level;
	ndelay(slow_ns);

	local.seq_check = hdev->snap.seq_check;

	if (use_lock)
		spin_unlock_irqrestore(&hdev->lock, flags);

	if (copy_to_user(buf, &local, sizeof(local)))
		return -EFAULT;

	return sizeof(local);
}

/* 练习 ②：ioctl。下面的 fops 已经引用了它，所以骨架阶段也能编译 */
static long hello_race_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	/*
	 * TODO ②-3：实现 HELLO_RACE_WAIT_N
	 *
	 * 1. hdev 从 filp->private_data 拿（跟 read 一样）
	 * 2. cmd 不是 HELLO_RACE_WAIT_N → 返回 -ENOTTY（"不认识的 ioctl 命令"的标准返回值，阶段 1 学过）
	 * 3. 从用户那拷 N：copy_from_user(&n, (void __user *)arg, sizeof(n))，失败返回 -EFAULT
	 *    N 为 0 或大于 1000000 → 返回 -EINVAL
	 * 4. "抢座位"：atomic_cmpxchg(&hdev->waiter_busy, 0, 1)
	 *      【新写法】atomic_cmpxchg(&v, old, new)：v 等于 old 就改成 new，返回 v 原来的值。一步完成。
	 *      返回值不是 0 → 已经有人坐了 → 返回 -EBUSY
	 *    思考：为什么不能先 atomic_read 判断是 0、再 atomic_set 成 1？
	 * 5. 准备等待，【顺序很重要】：
	 *      先 reinit_completion(&hdev->done)   —— 清掉上一次"已完成"的标记
	 *      再 atomic_set(&hdev->remaining, n)  —— 这一步之后，中断就开始数了
	 *    思考：顺序反过来，N=1 时可能出什么问题？
	 *          （提示：remaining 设好的下一瞬间中断就来了、complete 了；然后你才 reinit……）
	 * 6. 等：ret = wait_for_completion_interruptible_timeout(&hdev->done, msecs_to_jiffies(wait_timeout_ms));
	 *      【新写法】返回值三种：> 0 等到了（剩余的 jiffies）；= 0 超时；< 0 被信号打断（-ERESTARTSYS）
	 *      _interruptible：能被 Ctrl-C 打断；_timeout：中断一直不来也不会永远等下去
	 *      注意 ret 要用 long 类型（返回的是 long）
	 * 7. 不管结果如何，收尾：
	 *      atomic_set(&hdev->remaining, 0)    —— 超时或被打断时，让中断别再继续数
	 *      atomic_set(&hdev->waiter_busy, 0)  —— 让出座位
	 * 8. 换算返回值：> 0 返回 0；= 0 返回 -ETIMEDOUT；< 0 原样返回
	 *
	 * 这个函数在进程上下文，可以睡眠——wait_for_completion 就是在这里睡的。它不碰 snap，不需要 hdev->lock。
	 */
	struct hello_race_dev *hdev = filp->private_data;
	__u32 n;
	long ret;

	switch (cmd) {
	case HELLO_RACE_WAIT_N:
		if (copy_from_user(&n, (void __user *)arg, sizeof(n)))
			return -EFAULT;
		if (n == 0 || n > 1000000)
			return -EINVAL;
		if (atomic_cmpxchg(&hdev->waiter_busy, 0, 1))
			return -EBUSY;
		reinit_completion(&hdev->done);
		atomic_set(&hdev->remaining, n);

		ret = wait_for_completion_interruptible_timeout(&hdev->done,
			msecs_to_jiffies(wait_timeout_ms));

		atomic_set(&hdev->remaining, 0);
		atomic_set(&hdev->waiter_busy, 0);

		if (ret > 0)
			return 0;
		if (ret == 0)
			return -ETIMEDOUT;
		return ret;
	default:
		/* 内核约定：不认识的 ioctl 命令返回 -ENOTTY */
		return -ENOTTY;
	}
	return ret;
}

static const struct file_operations hello_race_fops = {
	.owner	= THIS_MODULE,
	.open	= hello_race_open,
	.read	= hello_race_read,
	.unlocked_ioctl	= hello_race_ioctl,	/* 练习 ② */
	.llseek	= no_llseek,
};
static enum hrtimer_restart hello_race_timer_fn(struct hrtimer *t)
{
	struct hello_race_dev *hdev = container_of(t, struct hello_race_dev, timer);

	hello_race_update(hdev);
	hrtimer_forward_now(t, ns_to_ktime((u64)READ_ONCE(period_us) * NSEC_PER_USEC));

	return HRTIMER_RESTART;
}
/* ============================ probe / remove ============================ */

static int hello_race_probe(struct platform_device *pdev)
{
	/*
	 * TODO 3：probe。结构跟阶段 4 的 hello_irq_probe 几乎一样，可以对照着写，但别复制粘贴，自己敲一遍：
	 *
	 * 1. devm_kzalloc 分配 hdev
	 * 2. devm_gpiod_get(dev, "irq", GPIOD_IN)，失败用 dev_err_probe 返回
	 *    （设备树属性还是 irq-gpios，复用 hello-irq 节点）
	 * 3. gpiod_to_irq 拿中断号
	 * 4. spin_lock_init(&hdev->lock)
	 *    思考：为什么这一步必须在 request_irq 之前？
	 * 5. devm_request_irq：上升沿触发，名字 "hello_race"，dev_id 传 hdev
	 * 6. 填 miscdev：minor 动态、name "hello_race"、fops、parent
	 * 7. platform_set_drvdata（remove 要用）
	 * 8. misc_register，放最后
	 * 9. dev_info 打印 use_lock 和 slow_ns，方便确认加载参数
	 */
	struct hello_race_dev *hdev;
	struct device *dev = &pdev->dev;
	int ret;

	hdev = devm_kzalloc(dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev)
		return -ENOMEM;

	spin_lock_init(&hdev->lock);
	/*
	 * TODO ②-1：初始化 completion 和两个 atomic_t
	 *   init_completion(&hdev->done)；atomic_set(&hdev->remaining, 0)；atomic_set(&hdev->waiter_busy, 0)
	 * 思考：为什么也要放在中断源（request_irq / hrtimer_start）启动之前？
	 * （devm_kzalloc 已经把内存清零了，atomic_set(0) 严格说可以省，但写出来意图更清楚）
	 */

	init_completion(&hdev->done);

	//atomic_set(0)可以省,devm_kzalloc 已经把内存清零了
	atomic_set(&hdev->remaining, 0);
	atomic_set(&hdev->waiter_busy, 0);

	if (use_timer) {
		hrtimer_init(&hdev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		hdev->timer.function = hello_race_timer_fn;
	} else {
		hdev->gpiod = devm_gpiod_get(dev, "irq", GPIOD_IN);
		if (IS_ERR(hdev->gpiod))
			return dev_err_probe(dev, PTR_ERR(hdev->gpiod), "failed to get irq-gpios\n");

		hdev->irq = gpiod_to_irq(hdev->gpiod);
		if (hdev->irq < 0)
			return dev_err_probe(dev, hdev->irq, "failed to map gpio to irq\n");

		ret = devm_request_irq(dev, hdev->irq, hello_race_isr, IRQF_TRIGGER_RISING, "hello_race", hdev);
		if (ret)
			return dev_err_probe(dev, ret, "failed to request irq %d\n", hdev->irq);
	}

	hdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	hdev->miscdev.name  = "hello_race";
	hdev->miscdev.fops  = &hello_race_fops;
	hdev->miscdev.parent = dev;

	platform_set_drvdata(pdev, hdev);

	ret = misc_register(&hdev->miscdev);
	if (ret)
		return dev_err_probe(dev, ret, "fail to register miscdev\n");

	if (use_timer)
		hrtimer_start(&hdev->timer, ns_to_ktime((u64)period_us * NSEC_PER_USEC), HRTIMER_MODE_REL);
	dev_info(dev, "use_lock = %d, slow_ns = %u, use_timer = %d\n", use_lock, slow_ns, use_timer);

	return 0;
}

static int hello_race_remove(struct platform_device *pdev)
{
	/*
	 * TODO 4：remove
	 * misc 不是 devm 的，要手动注销。其余（中断、内存）devm 会按相反顺序自动释放。
	 */
	struct hello_race_dev *hdev = platform_get_drvdata(pdev);

	if (use_timer)
		hrtimer_cancel(&hdev->timer);
	misc_deregister(&hdev->miscdev);
	return 0;
}

/* 复用 hello-irq 设备树节点（跟 hello_irq.ko、hello_key.ko 一次只加载一个） */
static const struct of_device_id hello_race_of_match[] = {
	{ .compatible = "zhouyllll,hello-irq" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hello_race_of_match);

static struct platform_driver hello_race_driver = {
	.probe	= hello_race_probe,
	.remove	= hello_race_remove,
	.driver	= {
		.name		= "hello_race",
		.of_match_table	= hello_race_of_match,
	},
};
module_platform_driver(hello_race_driver);
