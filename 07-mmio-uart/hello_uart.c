// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 阶段 7：寄存器级 UART 驱动（uart5，内部回环） =====================
 *
 * 【这是骨架】框架和注释都写好了，标 TODO 的地方由你来写。按 TODO 编号顺序写，每写完一部分就 make 一次。
 *
 * 这一阶段跟前面最大的不同：没有子系统帮你读写硬件，【你自己读写寄存器】。
 *
 * 硬件：uart5，DesignWare APB UART，跟 16550 兼容。
 *   设备树节点（rk3576-kickpi-k7.dtsi 末尾的 &uart5 覆盖）：
 *     reg = <0x0 0x2ad80000 0x0 0x100>      寄存器的物理地址，256 字节
 *     clocks / clock-names = "baudclk", "apb_pclk"
 *     reg-shift = <2>                        寄存器 n 的偏移是 n << 2（每个寄存器占 4 字节）
 *   用 16550 的内部回环（MCR 的 LOOP 位）：发送端在芯片内部直接接到接收端，不经过引脚，不用接线。
 *
 * 用户态怎么用（写完后）：
 *   echo hello > /dev/hello_uart     → write()：一个字节一个字节写进发送寄存器
 *   cat /dev/hello_uart              → read()：把接收 FIFO 里现有的字节读出来，读空了就返回 0（cat 结束）
 *   回环模式下发出去的字节立刻进了自己的接收 FIFO，所以 cat 能读回 "hello"。
 *   RK3576 的 UART FIFO 是 64 字节，一次 echo 超过 64 字节、又没人读，多出的会丢（这正好是一个观察点）。
 *
 * 用到的 16550 寄存器（编号来自 <linux/serial_reg.h>，偏移 = 编号 << 2）：
 *
 *   编号  名字           偏移   作用
 *   0     RX / TX        0x00   读：接收缓冲（取一个收到的字节）；写：发送缓冲（发一个字节）
 *   0     DLL            0x00   LCR.DLAB=1 时：波特率分频值低 8 位
 *   1     IER            0x04   中断使能（这次用轮询，写 0 全关）
 *   1     DLM            0x04   LCR.DLAB=1 时：波特率分频值高 8 位
 *   2     FCR            0x08   写：FIFO 控制（使能 FIFO、清空收发 FIFO）
 *   3     LCR            0x0c   数据格式（8N1 = UART_LCR_WLEN8）；bit7 DLAB 切换 0/1 号寄存器的含义
 *   4     MCR            0x10   UART_MCR_LOOP：内部回环
 *   5     LSR            0x14   状态：UART_LSR_DR 收到了数据；UART_LSR_THRE 发送缓冲空了，可以写下一个
 * =============================================================================================
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>			/* readl / writel */
#include <linux/clk.h>			/* devm_clk_get_enabled, clk_get_rate */
#include <linux/delay.h>		/* udelay */
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/serial_reg.h>		/* UART_LCR、UART_LSR_DR 这些寄存器编号和位定义 */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("Stage 7: register-level UART driver (uart5, internal loopback)");

#define HELLO_UART_FIFO_SIZE	64
#define HELLO_UART_REG_SHIFT	2	/* 设备树 reg-shift = <2> */

static uint baud = 115200;
module_param(baud, uint, 0444);
MODULE_PARM_DESC(baud, "baud rate");

/*
 * 调试练习（已写好参数）：insmod hello_uart.ko skip_clk=1
 * probe 里不打开时钟就去读写寄存器，观察会发生什么。
 * ⚠️ 结果可能是 oops（SError），也可能是整个系统卡死——卡死的话用串口 fiq → reboot 或者断电重启。
 */
static bool skip_clk;
module_param(skip_clk, bool, 0444);
MODULE_PARM_DESC(skip_clk, "DEBUG EXERCISE: access registers without enabling clocks");

struct hello_uart_dev {
	void __iomem *base;		/* ioremap 之后的虚拟地址（自己的东西：probe 里映射，devm 释放） */
	struct clk *baudclk;		/* 波特率时钟 */
	struct clk *pclk;		/* APB 总线时钟：CPU 访问寄存器走的那条总线 */
	struct mutex lock;		/* read 和 write 都要连续操作好几个寄存器，用 mutex 串行化 */
	struct miscdevice misc;
};

/* ============================ 寄存器读写 ============================ */

/*
 * TODO 1：读写一个 16550 寄存器。参数 reg 是寄存器【编号】（UART_LSR 这种），不是偏移。
 *
 *   【新写法】readl(addr)：从 addr 读 32 位；writel(val, addr)：往 addr 写 32 位（注意参数顺序：值在前，地址在后）
 *            addr 是 void __iomem * 类型——ioremap 返回的"I/O 内存"指针，只能用 readl/writel 这类函数访问，
 *            不能直接 *addr 解引用（sparse 会报错；有的架构上直接解引用也不对）
 *
 * 1. 偏移 = reg << HELLO_UART_REG_SHIFT
 * 2. 读：return readl(hdev->base + 偏移);
 *    写：writel(val, hdev->base + 偏移);
 *
 * 思考（面试题）：readl 和 readl_relaxed 有什么区别？
 *   提示：去看 arch/arm64/include/asm/io.h 里 readl 的定义，它比 readl_relaxed 多了什么？
 *   想一下"写完 DMA 描述符再读设备状态寄存器"的场景。
 */
/* 骨架里的 __maybe_unused：还没人调用时防止 -Werror 报"未使用"，全部写完后删掉 */
static u32 __maybe_unused hello_uart_read(struct hello_uart_dev *hdev, int reg)
{
	int offset;
	u32 value;

	offset = reg<<HELLO_UART_REG_SHIFT;
	value = readl(hdev->base + offset);

	return value;
}

static void hello_uart_write(struct hello_uart_dev *hdev, int reg, u32 val)
{
	int offset = reg << HELLO_UART_REG_SHIFT;

	writel(val, hdev->base + offset);
}

/* ============================ 硬件初始化 ============================ */

/*
 * TODO 2：初始化 UART：8N1、波特率、FIFO、内部回环、关中断。
 *
 * 1. IER 写 0：关掉所有中断（这次用轮询）
 * 2. 设置波特率：
 *      divisor = DIV_ROUND_CLOSEST(clk_get_rate(hdev->baudclk), 16 * baud)
 *        （16550 的规定：波特率 = 时钟 / (16 × divisor)）
 *        【新写法】DIV_ROUND_CLOSEST(a, b)：四舍五入的除法；clk_get_rate 返回这个时钟现在的频率（Hz）
 *      LCR 写 UART_LCR_DLAB         → 0、1 号寄存器变成 DLL、DLM
 *      DLL 写 divisor & 0xff，DLM 写 divisor >> 8
 *      LCR 写 UART_LCR_WLEN8        → 关掉 DLAB（0、1 号寄存器变回 RX/TX、IER），同时设成 8 位数据、无校验、1 停止位
 * 3. FCR 写 UART_FCR_ENABLE_FIFO | UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT：开 FIFO，并清空收发 FIFO
 * 4. MCR 写 UART_MCR_LOOP：内部回环
 * 5. dev_info 打印 baudclk 的频率和 divisor
 *
 * 注意第 2 步的顺序：DLAB 置 1 以后，写 0 号寄存器是在写 DLL，不是发数据。忘了关 DLAB，后面"发送"的字节全写进了分频寄存器。
 */
static int hello_uart_hw_init(struct hello_uart_dev *hdev)
{
	u32 divisor;
	unsigned long rate;
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

	hello_uart_write(hdev, UART_LCR, UART_LCR_DLAB);              // 打开 DLAB
	hello_uart_write(hdev, UART_DLL, divisor & 0xff);
	hello_uart_write(hdev, UART_DLM, divisor >> 8);              // 高 8 位
	hello_uart_write(hdev, UART_LCR, UART_LCR_WLEN8);              // 关掉 DLAB，设成 8N1

	hello_uart_write(hdev, UART_FCR, UART_FCR_ENABLE_FIFO | UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);              // 开 FIFO + 清空收发
	hello_uart_write(hdev, UART_MCR, UART_MCR_LOOP);              // 内部回环

	pr_info("baudclk %lu Hz, divisor %u\n", rate, divisor);

	return 0;

}

/*
 * TODO 3：发送一个字节（轮询）。成功返回 0，超时返回 -ETIMEDOUT。
 *
 * 1. 等发送缓冲空：循环读 LSR，直到 UART_LSR_THRE 置位；每次没等到就 udelay(10)，最多等 1000 次（10ms）
 *    （115200 波特率下发一个字节约 87us，10ms 足够了；等不到说明硬件有问题，不能死循环）
 * 2. 超时 → 返回 -ETIMEDOUT
 * 3. 往 TX 寄存器写这个字节，返回 0
 *
 * 【可选的新写法】上面的循环，内核有现成的宏：
 *   #include <linux/iopoll.h>
 *   readl_poll_timeout_atomic(addr, val, cond, delay_us, timeout_us)
 *     反复 val = readl(addr)，直到 cond 为真（返回 0）或超时（返回 -ETIMEDOUT），每次间隔 delay_us 微秒
 *   先手写循环，写对了再试着换成它。
 */
static int __maybe_unused hello_uart_tx_byte(struct hello_uart_dev *hdev, u8 c)
{
	u32 value;
	int i;

	for (i = 0; i < 1000; i++) {
		value = hello_uart_read(hdev, UART_LSR);
		if (value & UART_LSR_THRE)
			break;
		udelay(10);
	}
	if (i >= 1000) {
		return -ETIMEDOUT;
	}
	/*
	*ret = readl_poll_timeout_atomic(hdev->base + (UART_LSR << HELLO_UART_REG_SHIFT),value,(value & UART_LSR_THRE),10,10000);
	*if(ret)
	*	return ret;
	*/

	hello_uart_write(hdev, UART_TX, c);
	return 0;
}

/* ============================ 文件操作 ============================ */

/*
 * file->private_data 怎么来的：misc 框架在 open 时把 private_data 设成 &hdev->misc，
 * 所以这里用 container_of 从 misc 找回 hdev（阶段 5 讲过的写法）。
 */
static struct hello_uart_dev *to_hdev(struct file *file)
{
	return container_of(file->private_data, struct hello_uart_dev, misc);
}

/*
 * TODO 4：write：把用户写的字节一个一个发出去。返回实际发出的字节数，或者负的错误码。
 *
 * 1. 一次最多处理 HELLO_UART_FIFO_SIZE 个字节（多的不管，返回值告诉用户只写了多少，
 *    echo 会自己接着写剩下的——这是 write 的标准语义"可以只写一部分"）
 * 2. u8 kbuf[HELLO_UART_FIFO_SIZE]；copy_from_user，失败返回 -EFAULT
 * 3. mutex_lock(&hdev->lock)
 * 4. 循环调用 hello_uart_tx_byte 发每个字节，出错就记下错误码、停止
 * 5. mutex_unlock
 * 6. 一个都没发出去 → 返回错误码；否则返回发出的字节数
 *
 * 为什么用 mutex 不用 spinlock：tx_byte 里有最多 10ms 的忙等……其实 udelay 不睡眠，spinlock 也能用；
 * 但持锁 10ms 关抢占太久了，而且后面还要 copy_from_user（可能睡眠）——你想想 copy 应该放在锁里还是锁外？
 */
static ssize_t hello_uart_write_fop(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct hello_uart_dev *hdev = to_hdev(file);
	size_t n = min_t(size_t, count, HELLO_UART_FIFO_SIZE);
	u8 kbuf[HELLO_UART_FIFO_SIZE];
	int ret, i;

	if (copy_from_user(kbuf, ubuf, n))
		return -EFAULT;

	mutex_lock(&hdev->lock);
	for (i = 0; i < n; i++) {
		ret = hello_uart_tx_byte(hdev, kbuf[i]);
		if (ret < 0)
			break;
	}
	mutex_unlock(&hdev->lock);

	if (i == 0)
		return ret;

	return i;
}

/*
 * TODO 5：read：把接收 FIFO 里【现有的】字节读出来，不等待。返回读到的字节数；FIFO 空就返回 0（cat 就结束了）。
 *
 * 1. 最多读 min(count, HELLO_UART_FIFO_SIZE) 个
 * 2. mutex_lock
 * 3. 循环：LSR 的 UART_LSR_DR 置位（有数据）且还没读够 → 从 RX 寄存器读一个字节放进 kbuf
 * 4. mutex_unlock
 * 5. copy_to_user（锁外），失败返回 -EFAULT；返回读到的字节数
 *
 * 注意：回环里字节从发送到进接收 FIFO 要一个字节的时间（约 87us）。echo 完立刻 cat，人手的速度远远够了；
 * 但如果在同一个程序里 write 完马上 read，最后一个字节可能还在路上——这是轮询驱动的典型问题。
 */
static ssize_t hello_uart_read_fop(struct file *file, char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct hello_uart_dev *hdev = to_hdev(file);
	int i, lsr;
	size_t to_read = (count < HELLO_UART_FIFO_SIZE) ? count : HELLO_UART_FIFO_SIZE;
	u8 kbuf[HELLO_UART_FIFO_SIZE];

	mutex_lock(&hdev->lock);
	for (i = 0; i < to_read; i++) {
		lsr = hello_uart_read(hdev, UART_LSR);
		if (!(lsr & UART_LSR_DR))
			break;
		kbuf[i] = hello_uart_read(hdev, UART_RX);
	}
	mutex_unlock(&hdev->lock);
	if (copy_to_user(ubuf, kbuf, i))
		return -EFAULT;
	return i;                        // 读到了几个字节
}

static const struct file_operations hello_uart_fops = {
	.owner	= THIS_MODULE,
	.read	= hello_uart_read_fop,
	.write	= hello_uart_write_fop,
	.llseek	= noop_llseek,
};

/* ============================ probe / remove ============================ */

/*
 * TODO 6：probe
 *
 * 1. devm_kzalloc 分配 hdev；mutex_init(&hdev->lock)
 * 2. 【新写法】hdev->base = devm_platform_ioremap_resource(pdev, 0);
 *      取设备树 reg 的第 0 段（0x2ad80000，0x100 字节），先"申请"这段物理地址（防止两个驱动映射同一段），
 *      再 ioremap 成内核虚拟地址；remove 时自动 iounmap。失败返回 ERR_PTR，用 IS_ERR / PTR_ERR 检查。
 *    思考（面试题）：为什么不能直接 readl(0x2ad80000)？为什么必须 ioremap？
 * 3. 打开时钟——skip_clk 为真时【跳过这一步】（调试练习）：
 *      【新写法】hdev->pclk = devm_clk_get_enabled(&pdev->dev, "apb_pclk");
 *        按 clock-names 找到时钟，并 prepare + enable；remove 时自动 disable + unprepare + put
 *      baudclk 同理。失败用 dev_err_probe 返回（IS_ERR 检查）
 *    skip_clk 时 hdev->baudclk 是 NULL，clk_get_rate(NULL) 返回 0，divisor 会算成 0——没关系，这个练习要看的是访问寄存器本身。
 * 4. hello_uart_hw_init(hdev)
 * 5. 填 misc：minor = MISC_DYNAMIC_MINOR，name = "hello_uart"，fops = &hello_uart_fops，parent = &pdev->dev
 * 6. platform_set_drvdata(pdev, hdev)；misc_register，失败返回错误码
 * 7. dev_info 打印"映射到的虚拟地址"（%p 会被内核哈希，想看真实值用 %px——调试用，平时别打印内核地址）
 * 8. 返回 0
 */
static int hello_uart_probe(struct platform_device *pdev)
{
	struct hello_uart_dev *hdev;
	int ret;

	hdev = devm_kzalloc(&pdev->dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev)
		return -ENOMEM;
	mutex_init(&hdev->lock);
	hdev->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(hdev->base))
		return PTR_ERR(hdev->base);

	if (!skip_clk) {
		hdev->pclk = devm_clk_get_enabled(&pdev->dev, "apb_pclk");
		if (IS_ERR(hdev->pclk))
			return dev_err_probe(&pdev->dev, PTR_ERR(hdev->pclk), "failed to get apb_pclk\n");

		hdev->baudclk = devm_clk_get_enabled(&pdev->dev, "baudclk");
		if (IS_ERR(hdev->baudclk))
			return dev_err_probe(&pdev->dev, PTR_ERR(hdev->baudclk), "failed to get baudclk\n");
	}

	dev_info(&pdev->dev, "LSR before init: 0x%02x\n", hello_uart_read(hdev, UART_LSR));
	hello_uart_write(hdev, UART_SCR, 0x5a);
	dev_info(&pdev->dev, "SCR : 0x%02x\n", hello_uart_read(hdev, UART_SCR));
	hello_uart_write(hdev, UART_SCR, 0xa5);
	dev_info(&pdev->dev, "SCR : 0x%02x\n", hello_uart_read(hdev, UART_SCR));

	ret = hello_uart_hw_init(hdev);
	if (ret)
		return ret;

	hdev->misc.minor = MISC_DYNAMIC_MINOR;
	hdev->misc.name  = "hello_uart";
	hdev->misc.fops  = &hello_uart_fops;
	hdev->misc.parent = &pdev->dev;

	platform_set_drvdata(pdev, hdev);
	ret = misc_register(&hdev->misc);
	if (ret)
		return ret;

	 dev_info(&pdev->dev, "registered /dev/%s, regs mapped at %px\n", hdev->misc.name, hdev->base);

	return 0;
}

/*
 * remove：注销 misc；时钟和映射都是 devm 的，会在 remove 返回后按申请的【相反顺序】释放：
 * 先关时钟，再 iounmap。
 * 思考：如果 misc_deregister 放在 devm 释放之后（比如也用 devm 注册 misc，但比时钟先注册），
 *       会有什么风险？（提示：时钟关了，用户还能 write 吗？）
 */
static int hello_uart_remove(struct platform_device *pdev)
{
	struct hello_uart_dev *hdev = platform_get_drvdata(pdev);

	misc_deregister(&hdev->misc);

	return 0;
}

static const struct of_device_id hello_uart_of_match[] = {
	{ .compatible = "zhouyllll,hello-uart" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hello_uart_of_match);

static struct platform_driver hello_uart_driver = {
	.probe	= hello_uart_probe,
	.remove	= hello_uart_remove,
	.driver	= {
		.name		= "hello_uart",
		.of_match_table	= hello_uart_of_match,
	},
};
module_platform_driver(hello_uart_driver);
