// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 阶段 10：给 UART 驱动加上 runtime PM 和系统休眠 =====================
 *
 * 【这是骨架】以你阶段 7 写的 07-mmio-uart/hello_uart.c 为基础（寄存器读写、hw_init、tx_byte、read 原样搬过来，
 * 删掉了 SCR 调试打印和 skip_clk）。新的部分标了 TODO。
 *
 * 跟 hello_uart.ko / hello_uart_dma.ko 用同一个 compatible，加载前先把它们 rmmod。设备节点 /dev/hello_uart_pm。
 *
 * 要解决的问题：阶段 7 的驱动 probe 时就把两个时钟打开，直到 rmmod 才关——
 * 哪怕一整天没人用这个串口，时钟也一直开着，白白耗电。
 *
 * runtime PM（运行时电源管理）的思路：
 *   有人要用（read/write）时 → "我要用了"：pm_runtime_resume_and_get → 框架调你的 runtime_resume（开时钟）
 *   用完了                   → "我不用了"：pm_runtime_put_autosuspend → 计数归零后再等 autosuspend_delay 毫秒，
 *                             期间没人再用 → 框架调你的 runtime_suspend（关时钟）
 *   为什么要等一会儿再关：echo 一下、cat 一下，间隔很短；每次都关了又开，开关本身的开销比省下的电还多。
 *
 * 框架替你维护一个"使用计数"（usage_count）和状态（active / suspended），你只需要：
 *   1. 写两个回调：runtime_suspend（关）、runtime_resume（开）
 *   2. 在用硬件前 get、用完后 put
 *
 * 观察（不用接线，不用改设备树，普通内核就行）：
 *   cat /sys/bus/platform/devices/2ad80000.serial/power/runtime_status     active / suspended
 *   grep uart5 /sys/kernel/debug/clk/clk_summary                           时钟的 enable_cnt 0 / 1
 *   （阶段 7 案例 17 用 clk_summary 查过时钟——这次看着它随 runtime PM 自动开关）
 *
 * 系统休眠（suspend to RAM）：整个系统睡下去之前，内核挨个调每个设备的 suspend；醒来后挨个调 resume。
 *   这里直接复用 runtime PM 的回调：pm_runtime_force_suspend / pm_runtime_force_resume（已写好，见文件末尾）。
 *   测试不需要真的休眠：echo devices > /sys/power/pm_test 之后 echo mem > /sys/power/state，
 *   内核只走一遍"所有设备 suspend → 等 5 秒 → 所有设备 resume"，不真的断电。
 * =============================================================================================
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/serial_reg.h>
#include <linux/pm_runtime.h>		/* pm_runtime_* */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("Stage 10: UART driver with runtime PM");

#define HELLO_UART_FIFO_SIZE	64
#define HELLO_UART_REG_SHIFT	2

static uint baud = 115200;
module_param(baud, uint, 0444);
MODULE_PARM_DESC(baud, "baud rate");

static uint autosuspend_ms = 2000;
module_param(autosuspend_ms, uint, 0444);
MODULE_PARM_DESC(autosuspend_ms, "idle time before runtime suspend (ms)");

/*
 * 调试练习（已写好参数）：resume 时要不要重新初始化寄存器。
 * 先用默认值 0 做实验，再用 1，比较"echo 之后等 3 秒再 cat"读到什么（见 TODO 2 的思考）。
 */
static bool reinit_on_resume;
module_param(reinit_on_resume, bool, 0444);
MODULE_PARM_DESC(reinit_on_resume, "re-run hw_init in runtime_resume");

struct hello_uart_dev {
	struct device *dev;		/* 新：runtime PM 的接口都要 dev */
	void __iomem *base;
	struct clk *baudclk;
	struct clk *pclk;
	struct mutex lock;
	struct miscdevice misc;
};

/* ============================ 寄存器读写、hw_init、tx_byte（你在阶段 7 写的） ============================ */

static u32 hello_uart_read(struct hello_uart_dev *hdev, int reg)
{
	return readl(hdev->base + (reg << HELLO_UART_REG_SHIFT));
}

static void hello_uart_write(struct hello_uart_dev *hdev, int reg, u32 val)
{
	writel(val, hdev->base + (reg << HELLO_UART_REG_SHIFT));
}

/* 骨架里的 __maybe_unused：还没人调用时防止 -Werror 报"未使用"，写完 TODO 后删掉 */
static int __maybe_unused hello_uart_hw_init(struct hello_uart_dev *hdev)
{
	unsigned long rate;
	u32 divisor;
	int ret;

	ret = clk_set_rate(hdev->baudclk, 16 * baud);
	if (ret)
		return ret;
	rate = clk_get_rate(hdev->baudclk);

	hello_uart_write(hdev, UART_IER, 0);

	divisor = DIV_ROUND_CLOSEST(rate, 16 * baud);
	if (divisor == 0) {
		pr_err("baudclk %lu Hz too slow for baud %u\n", rate, baud);
		return -EINVAL;
	}

	hello_uart_write(hdev, UART_LCR, UART_LCR_DLAB);
	hello_uart_write(hdev, UART_DLL, divisor & 0xff);
	hello_uart_write(hdev, UART_DLM, divisor >> 8);
	hello_uart_write(hdev, UART_LCR, UART_LCR_WLEN8);

	hello_uart_write(hdev, UART_FCR,
			 UART_FCR_ENABLE_FIFO | UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
	hello_uart_write(hdev, UART_MCR, UART_MCR_LOOP);

	pr_info("baudclk %lu Hz, divisor %u\n", rate, divisor);
	return 0;
}

static int hello_uart_tx_byte(struct hello_uart_dev *hdev, u8 c)
{
	int i;

	for (i = 0; i < 1000; i++) {
		if (hello_uart_read(hdev, UART_LSR) & UART_LSR_THRE)
			break;
		udelay(10);
	}
	if (i >= 1000)
		return -ETIMEDOUT;

	hello_uart_write(hdev, UART_TX, c);
	return 0;
}

/* ============================ 时钟开关 ============================ */

/*
 * TODO 1：开 / 关两个时钟。成功返回 0。
 *
 * 阶段 7 用的 devm_clk_get_enabled 是"拿到就打开、remove 时才关"，这次要能随时开关，所以：
 *   probe 里改用 devm_clk_get（只拿，不开）——已经在 probe 里改好了
 *   开：【新写法】clk_prepare_enable(clk)：成功返回 0
 *   关：【新写法】clk_disable_unprepare(clk)
 *
 * hello_uart_clk_on：
 *   1. 先开 pclk（总线时钟：CPU 访问寄存器要它），失败返回 ret
 *   2. 再开 baudclk；失败 → 【把第 1 步开的 pclk 关掉】，再返回 ret
 *      （跟 probe 的出错路径一样：失败时要撤销已经做了的事，按相反顺序）
 *   3. return 0
 * hello_uart_clk_off：按相反顺序关：先 baudclk，再 pclk
 *
 * 思考：clk_prepare_enable 是 clk_prepare + clk_enable 两步。为什么框架要拆成两步？
 *      （提示：clk_prepare 可以睡眠（比如要等 PLL 锁定），clk_enable 不能睡眠、可以在中断里调用）
 */
static int __maybe_unused hello_uart_clk_on(struct hello_uart_dev *hdev)
{
	return -ENODEV;	/* 占位，写完记得改 */
}

static void __maybe_unused hello_uart_clk_off(struct hello_uart_dev *hdev)
{
}

/* ============================ runtime PM 回调 ============================ */

/*
 * TODO 2：runtime PM 的两个回调。框架在计数归零、等够 autosuspend_ms 之后调 suspend；在有人 get 时调 resume。
 *
 * hello_uart_runtime_suspend(dev)：
 *   1. hdev = dev_get_drvdata(dev)
 *   2. hello_uart_clk_off(hdev)
 *   3. dev_info(dev, "runtime suspend\n")；return 0
 *
 * hello_uart_runtime_resume(dev)：
 *   1. hdev = dev_get_drvdata(dev)
 *   2. ret = hello_uart_clk_on(hdev)；失败返回 ret
 *   3. reinit_on_resume 为真 → ret = hello_uart_hw_init(hdev)；失败要先 clk_off 再返回 ret
 *   4. dev_info(dev, "runtime resume\n")；return 0
 *
 * 思考：resume 时到底要不要重新 hw_init？
 *   ① 先用 reinit_on_resume=0（默认）做实验：echo hello → 等 3 秒（已经 runtime suspend 了）→ cat，读到什么？
 *   ② 再用 reinit_on_resume=1：同样的操作，读到什么？
 *   ③ 关时钟时寄存器的值会丢吗？（回想阶段 7 案例 16：rmmod 只关时钟，卡在发送端的 'h' 下次还在）
 *      什么情况下 resume 必须重新初始化？（提示：如果 suspend 时关的不只是时钟，而是整个电源域呢？）
 */
static int __maybe_unused hello_uart_runtime_suspend(struct device *dev)
{
	return 0;
}

static int __maybe_unused hello_uart_runtime_resume(struct device *dev)
{
	return 0;
}

/* ============================ 文件操作 ============================ */

static struct hello_uart_dev *to_hdev(struct file *file)
{
	return container_of(file->private_data, struct hello_uart_dev, misc);
}

/*
 * TODO 3：write 和 read 在碰硬件之前"要用"，碰完之后"不用了"。
 *
 * 在 mutex_lock 之前：
 *   【新写法】ret = pm_runtime_resume_and_get(hdev->dev);
 *     usage_count + 1；如果设备是 suspended，先同步调用你的 runtime_resume，等它返回才往下走
 *     失败返回负数（这时计数已经还原了，直接 return ret，不要 put）
 * 在 mutex_unlock 之后：
 *   【新写法】pm_runtime_mark_last_busy(hdev->dev);       记下"最后一次使用"的时间，autosuspend 从这一刻开始倒计时
 *   【新写法】pm_runtime_put_autosuspend(hdev->dev);     usage_count - 1；到 0 了就开始倒计时，到时调 runtime_suspend
 *
 * 注意：get 和 put 必须成对。所有在 get 之后的 return 路径都要 put（copy_from_user 失败那条也算）——
 *      跟"加锁后的每条 return 都要解锁"是同一个规则。少一次 put → 计数永远不归零 → 永远不 suspend；
 *      多一次 put → 计数变负 → 内核会打印警告 "Runtime PM usage count underflow!"
 */
static ssize_t hello_uart_write_fop(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct hello_uart_dev *hdev = to_hdev(file);
	size_t n = min_t(size_t, count, HELLO_UART_FIFO_SIZE);
	u8 kbuf[HELLO_UART_FIFO_SIZE];
	ssize_t ret = 0;
	size_t i;

	if (copy_from_user(kbuf, ubuf, n))
		return -EFAULT;

	/* TODO 3：get */

	mutex_lock(&hdev->lock);
	for (i = 0; i < n; i++) {
		ret = hello_uart_tx_byte(hdev, kbuf[i]);
		if (ret)
			break;
	}
	mutex_unlock(&hdev->lock);

	/* TODO 3：mark_last_busy + put */

	return i ? i : ret;
}

static ssize_t hello_uart_read_fop(struct file *file, char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct hello_uart_dev *hdev = to_hdev(file);
	size_t to_read = min_t(size_t, count, HELLO_UART_FIFO_SIZE);
	u8 kbuf[HELLO_UART_FIFO_SIZE];
	size_t i;

	/* TODO 3：get */

	mutex_lock(&hdev->lock);
	for (i = 0; i < to_read; i++) {
		if (!(hello_uart_read(hdev, UART_LSR) & UART_LSR_DR))
			break;
		kbuf[i] = hello_uart_read(hdev, UART_RX);
	}
	mutex_unlock(&hdev->lock);

	/* TODO 3：mark_last_busy + put */

	if (copy_to_user(ubuf, kbuf, i))
		return -EFAULT;
	return i;
}

static const struct file_operations hello_uart_fops = {
	.owner	= THIS_MODULE,
	.read	= hello_uart_read_fop,
	.write	= hello_uart_write_fop,
	.llseek	= noop_llseek,
};

/* ============================ probe / remove ============================ */

static int hello_uart_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hello_uart_dev *hdev;
	int __maybe_unused ret;

	hdev = devm_kzalloc(dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev)
		return -ENOMEM;
	hdev->dev = dev;
	mutex_init(&hdev->lock);
	platform_set_drvdata(pdev, hdev);

	hdev->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(hdev->base))
		return PTR_ERR(hdev->base);

	/* 阶段 7 是 devm_clk_get_enabled；这次只拿不开，开关交给 runtime PM */
	hdev->pclk = devm_clk_get(dev, "apb_pclk");
	if (IS_ERR(hdev->pclk))
		return dev_err_probe(dev, PTR_ERR(hdev->pclk), "failed to get apb_pclk\n");
	hdev->baudclk = devm_clk_get(dev, "baudclk");
	if (IS_ERR(hdev->baudclk))
		return dev_err_probe(dev, PTR_ERR(hdev->baudclk), "failed to get baudclk\n");

	/*
	 * TODO 4：probe 里的 runtime PM 初始化。按顺序：
	 *
	 * 1. ret = hello_uart_clk_on(hdev)；失败返回 ret       —— probe 期间要访问寄存器，先手动开
	 * 2. ret = hello_uart_hw_init(hdev)；失败 → clk_off，返回 ret
	 * 3. 告诉框架"设备现在是开着的"，并且先占住一个计数，防止初始化到一半就被 suspend：
	 *      【新写法】pm_runtime_get_noresume(dev);        usage_count + 1，但不调 resume（本来就开着）
	 *      【新写法】pm_runtime_set_active(dev);          框架记下：状态 = active（默认它以为是 suspended）
	 *      【新写法】pm_runtime_set_autosuspend_delay(dev, autosuspend_ms);
	 *      【新写法】pm_runtime_use_autosuspend(dev);
	 *      【新写法】pm_runtime_enable(dev);              从这一刻起框架才开始管这个设备
	 * 4. 填 misc（name = "hello_uart_pm"），misc_register；
	 *    失败 → 撤销第 3 步（pm_runtime_disable、pm_runtime_set_suspended、pm_runtime_put_noidle、
	 *           pm_runtime_dont_use_autosuspend）、clk_off，返回 ret
	 * 5. pm_runtime_mark_last_busy(dev); pm_runtime_put_autosuspend(dev);
	 *      还掉第 3 步占的计数 → 计数归零 → autosuspend_ms 后自动 suspend（关时钟）
	 * 6. dev_info 打印 "registered /dev/hello_uart_pm, autosuspend %u ms"
	 *
	 * 第 3 步的顺序就是 imx415 驱动 probe 末尾的写法（drivers/media/i2c/imx415.c:3098-3100），可以对照着看。
	 */

	return -ENODEV;	/* 占位，写完记得改 */
}

/*
 * TODO 5：remove，跟 probe 第 3 步相反（imx415.c:3132-3135 是同样的写法）：
 *
 * 1. misc_deregister(&hdev->misc)             先关入口
 * 2. pm_runtime_disable(dev)                  框架不再管它（之后不会再自动 suspend/resume）
 * 3. 如果设备现在还是 active（!pm_runtime_status_suspended(dev)）→ hello_uart_clk_off(hdev)
 *    （如果已经 suspended，时钟早就关了，不能再关一次——clk 的 enable 计数会变负）
 * 4. pm_runtime_set_suspended(dev)
 * 5. pm_runtime_dont_use_autosuspend(dev)
 */
static int hello_uart_remove(struct platform_device *pdev)
{
	return 0;
}

/*
 * 系统休眠直接复用 runtime PM：
 *   pm_runtime_force_suspend：设备还 active 就调你的 runtime_suspend，并记住"睡前是开着的"
 *   pm_runtime_force_resume：睡前是开着的才调 runtime_resume
 * 【新写法】DEFINE_RUNTIME_DEV_PM_OPS 一行定义出 struct dev_pm_ops，pm_ptr() 在没开 CONFIG_PM 时变成 NULL
 */
static DEFINE_RUNTIME_DEV_PM_OPS(hello_uart_pm_ops, hello_uart_runtime_suspend,
				 hello_uart_runtime_resume, NULL);

static const struct of_device_id hello_uart_of_match[] = {
	{ .compatible = "zhouyllll,hello-uart" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hello_uart_of_match);

static struct platform_driver hello_uart_driver = {
	.probe	= hello_uart_probe,
	.remove	= hello_uart_remove,
	.driver	= {
		.name		= "hello_uart_pm",
		.of_match_table	= hello_uart_of_match,
		.pm		= pm_ptr(&hello_uart_pm_ops),
	},
};
module_platform_driver(hello_uart_driver);
