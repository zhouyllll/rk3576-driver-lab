// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 阶段 8 练习 ②：UART 发送改用 DMA（PL330 + dmaengine） =====================
 *
 * 【这是骨架】以你阶段 7 写的 07-mmio-uart/hello_uart.c 为基础：寄存器读写、hw_init、tx_byte、read、
 * probe 的前半段都是你写的代码，原样搬过来了（删掉了 SCR 调试打印和 skip_clk）。
 * 新的部分标了 TODO，由你来写。
 *
 * 跟 hello_uart.ko 用同一个 compatible（zhouyllll,hello-uart），加载前先 rmmod hello_uart。
 * 设备节点叫 /dev/hello_uart_dma。
 *
 * 发送的两种方式（模块参数 use_dma 切换）：
 *
 *   use_dma=0（阶段 7 的轮询）：CPU 等 THRE → 写一个字节 → 等 THRE → 写下一个……CPU 全程在忙
 *
 *   use_dma=1（这次）：
 *     CPU：数据拷进 DMA 缓冲区 → 告诉 PL330 "从这里搬 n 字节到 UART 的 TX 寄存器" → 睡眠等待
 *     PL330：UART 的发送 FIFO 有空位就拉请求线，PL330 就搬一批（dma_burst 个字节）过去
 *     搬完：PL330 产生中断 → pl330 驱动调用你的回调 → complete() → write 被唤醒，返回
 *
 * dmaengine 的五步（几乎所有 DMA 外设驱动都是这个套路）：
 *   1. dma_request_chan(dev, "tx")            申请通道（按设备树 dma-names 找 dmas 里对应的那个）
 *   2. dmaengine_slave_config(chan, &cfg)     告诉通道：外设那头的地址、宽度、一批搬几个
 *   3. dmaengine_prep_slave_single(...)       准备一次传输，得到一个描述符 desc
 *   4. dmaengine_submit(desc)                 提交到通道的队列
 *   5. dma_async_issue_pending(chan)          真正启动
 *   （1、2 在 probe 里做一次；3、4、5 每次传输都做）
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
#include <linux/completion.h>
#include <linux/serial_reg.h>
#include <linux/dmaengine.h>		/* dma_request_chan、dmaengine_* */
#include <linux/dma-mapping.h>		/* dma_alloc_coherent、dma_addr_t */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("Stage 8: UART TX via dmaengine (uart5, internal loopback)");

#define HELLO_UART_FIFO_SIZE	64
#define HELLO_UART_REG_SHIFT	2
#define HELLO_UART_DMA_BUF_SIZE	PAGE_SIZE	/* DMA 缓冲区一页，一次 write 最多发这么多 */

/*
 * DW APB UART 的 DMA Software Ack 寄存器，偏移 0xa8 → 编号 0x2a（0x2a << 2 = 0xa8）。
 * Rockchip 原厂驱动在每次启动 TX DMA 前写 1，清掉 UART 残留的 DMA 请求
 * （drivers/tty/serial/8250/8250_dma.c:176 serial8250_tx_dma）。照抄，不是练习重点。
 */
#define DW_UART_DMASA		0x2a

static uint baud = 115200;
module_param(baud, uint, 0444);
MODULE_PARM_DESC(baud, "baud rate");

static bool use_dma = true;
module_param(use_dma, bool, 0444);
MODULE_PARM_DESC(use_dma, "send with DMA (1) or polling (0)");

static uint dma_burst = 16;
module_param(dma_burst, uint, 0444);
MODULE_PARM_DESC(dma_burst, "DMA burst length in bytes (Rockchip 8250 uses 16)");

struct hello_uart_dev {
	void __iomem *base;
	phys_addr_t phys_base;		/* 新：寄存器的【物理】地址，DMA 控制器要用（见 TODO 1） */
	struct clk *baudclk;
	struct clk *pclk;
	struct mutex lock;
	struct miscdevice misc;

	/* 新：DMA 相关 */
	struct dma_chan *txchan;	/* DMA 通道（别人的东西：dmaengine 给的） */
	void *dma_buf;			/* DMA 缓冲区的 CPU 虚拟地址：CPU 往这里写数据 */
	dma_addr_t dma_buf_addr;	/* 同一块缓冲区的 DMA 地址：交给 PL330，它从这里读 */
	struct completion tx_done;	/* DMA 搬完时由回调 complete，write 在上面等 */
};

/* ============================ 寄存器读写（你在阶段 7 写的） ============================ */

static u32 hello_uart_read(struct hello_uart_dev *hdev, int reg)
{
	return readl(hdev->base + (reg << HELLO_UART_REG_SHIFT));
}

static void hello_uart_write(struct hello_uart_dev *hdev, int reg, u32 val)
{
	writel(val, hdev->base + (reg << HELLO_UART_REG_SHIFT));
}

/* ============================ 硬件初始化（你在阶段 7 写的） ============================ */

static int hello_uart_hw_init(struct hello_uart_dev *hdev)
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

/* ============================ 轮询发送（你在阶段 7 写的，use_dma=0 时用） ============================ */

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

static ssize_t hello_uart_tx_pio(struct hello_uart_dev *hdev, const u8 *buf, size_t n)
{
	int ret = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		ret = hello_uart_tx_byte(hdev, buf[i]);
		if (ret)
			break;
	}
	return i ? i : ret;
}

/* ============================ DMA 发送 ============================ */


/*
 * TODO 2：DMA 完成回调。
 *
 * PL330 搬完以后产生中断，pl330 驱动在【tasklet】（软中断上下文，阶段 4 讲过）里调用这个函数，
 * 参数 param 就是你在 TODO 3 里设的 desc->callback_param。
 *
 * 1. 把 param 转回 struct hello_uart_dev *
 * 2. complete(&hdev->tx_done)
 *
 * 思考：这里能不能调用 mutex_lock、msleep？（提示：tasklet 是什么上下文？）
 */
static void hello_uart_dma_tx_done(void *param)
{
	struct hello_uart_dev *hdev = param;

	complete(&hdev->tx_done);
}

/*
 * TODO 3：用 DMA 发送 n 个字节。调用者已经持有 hdev->lock，并且已经把数据拷进了 hdev->dma_buf。
 * 成功返回 n，失败返回负的错误码。
 *
 * 1. reinit_completion(&hdev->tx_done)              —— 每次传输前重置（阶段 5 讲过为什么要 reinit）
 * 2. 准备传输：
 *      【新写法】desc = dmaengine_prep_slave_single(hdev->txchan, hdev->dma_buf_addr, n,
 *                                                 DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
 *        参数：通道、【DMA 地址】（不是 dma_buf！）、长度、方向（内存 → 外设）、
 *              标志（DMA_PREP_INTERRUPT：搬完要产生中断、调回调；DMA_CTRL_ACK：描述符用完由 dmaengine 回收）
 *      desc 为 NULL → 返回 -EIO
 * 3. desc->callback = hello_uart_dma_tx_done;  desc->callback_param = hdev;
 * 4. 【新写法】cookie = dmaengine_submit(desc);      cookie 是 dma_cookie_t 类型，传输的"票据号"
 *      dma_submit_error(cookie) 为真 → 返回 -EIO
 * 5. hello_uart_write(hdev, DW_UART_DMASA, 1);       —— Rockchip 要求：启动前清 UART 的 DMA 请求
 * 6. 【新写法】dma_async_issue_pending(hdev->txchan); —— 真正开始搬
 * 7. 等完成：wait_for_completion_timeout(&hdev->tx_done, msecs_to_jiffies(1000))
 *      返回 0 表示超时 → 【新写法】dmaengine_terminate_sync(hdev->txchan) 把这次传输取消掉，返回 -ETIMEDOUT
 *      （不取消的话，PL330 可能过一会儿又开始搬，而 dma_buf 已经被下一次 write 改掉了）
 * 8. 返回 n
 *
 * 为什么 2 里传 dma_buf_addr 而不是 dma_buf：dma_buf 是 CPU 的虚拟地址，要经过 CPU 的 MMU 才有意义；
 * PL330 不经过 CPU 的 MMU，它要的是自己能直接用的地址（这块板子上就是物理地址）。
 */
static ssize_t hello_uart_tx_dma(struct hello_uart_dev *hdev, size_t n)
{
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	unsigned long timeout;

	reinit_completion(&hdev->tx_done);

	desc = dmaengine_prep_slave_single(hdev->txchan, hdev->dma_buf_addr, n,
		DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc)
		return -EIO;

	desc->callback = hello_uart_dma_tx_done;
	desc->callback_param = hdev;

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie))
		return -EIO;

	hello_uart_write(hdev, DW_UART_DMASA, 1);
	dma_async_issue_pending(hdev->txchan);
	timeout = wait_for_completion_timeout(&hdev->tx_done, msecs_to_jiffies(1000));
	if (timeout == 0) {
		dmaengine_terminate_sync(hdev->txchan);
		return -ETIMEDOUT;
	}

	return n;
}

/* ============================ 文件操作 ============================ */

static struct hello_uart_dev *to_hdev(struct file *file)
{
	return container_of(file->private_data, struct hello_uart_dev, misc);
}

/*
 * TODO 4：write。
 *
 * use_dma=0：跟阶段 7 一样——拷到局部数组 kbuf（锁外），加锁，hello_uart_tx_pio，解锁。（已写好）
 * use_dma=1：
 *   1. n = min(count, HELLO_UART_DMA_BUF_SIZE)
 *   2. mutex_lock
 *   3. copy_from_user(hdev->dma_buf, ubuf, n)：【直接】拷进 DMA 缓冲区；失败 → 解锁，返回 -EFAULT
 *   4. ret = hello_uart_tx_dma(hdev, n)
 *   5. mutex_unlock，返回 ret
 *
 * 思考：阶段 7 说 copy_from_user 要放在锁外，这里为什么放进了锁里？
 *      （提示：dma_buf 只有一块，是所有 write 共用的。两个进程同时 write，在锁外各自往 dma_buf 拷数据会怎样？
 *       copy_from_user 可能睡眠，持 mutex 睡眠可以吗？持 spinlock 呢？）
 */
static ssize_t hello_uart_write_fop(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct hello_uart_dev *hdev = to_hdev(file);
	u8 kbuf[HELLO_UART_FIFO_SIZE];
	size_t n;
	ssize_t ret;

	if (!use_dma) {
		n = min_t(size_t, count, HELLO_UART_FIFO_SIZE);
		if (copy_from_user(kbuf, ubuf, n))
			return -EFAULT;
		mutex_lock(&hdev->lock);
		ret = hello_uart_tx_pio(hdev, kbuf, n);
		mutex_unlock(&hdev->lock);
		return ret;
	}
	n = min_t(size_t, count, HELLO_UART_DMA_BUF_SIZE);
	mutex_lock(&hdev->lock);
	if (copy_from_user(hdev->dma_buf, ubuf, n)) {
		mutex_unlock(&hdev->lock);
		return -EFAULT;
	}
	ret = hello_uart_tx_dma(hdev, n);
	mutex_unlock(&hdev->lock);

	return ret;
}

/* read：你在阶段 7 写的，没改。回环里 DMA 发出去的字节一样会进接收 FIFO */
static ssize_t hello_uart_read_fop(struct file *file, char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct hello_uart_dev *hdev = to_hdev(file);
	size_t to_read = min_t(size_t, count, HELLO_UART_FIFO_SIZE);
	u8 kbuf[HELLO_UART_FIFO_SIZE];
	size_t i;

	mutex_lock(&hdev->lock);
	for (i = 0; i < to_read; i++) {
		if (!(hello_uart_read(hdev, UART_LSR) & UART_LSR_DR))
			break;
		kbuf[i] = hello_uart_read(hdev, UART_RX);
	}
	mutex_unlock(&hdev->lock);

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

/* ============================ DMA 资源的申请和释放 ============================ */

/*
 * 释放 DMA 资源。由 devm_add_action_or_reset 注册（TODO 1 第 5 步），remove 后自动调用。
 * 顺序跟申请相反：先还缓冲区，再还通道。（已写好）
 */
static void hello_uart_dma_release(void *data)
{
	struct hello_uart_dev *hdev = data;

	dmaengine_terminate_sync(hdev->txchan);
	dma_free_coherent(hdev->txchan->device->dev, HELLO_UART_DMA_BUF_SIZE,
			  hdev->dma_buf, hdev->dma_buf_addr);
	dma_release_channel(hdev->txchan);
}

/*
 * TODO 1：申请 DMA 资源（probe 调用）。成功返回 0。
 *
 * 1. 申请通道：
 *      【新写法】hdev->txchan = dma_request_chan(dev, "tx");
 *        按设备树 dma-names 里 "tx" 的位置，找到 dmas 里对应的 <&dmac1 11>，向 pl330 驱动要一个通道。
 *        失败返回 ERR_PTR（IS_ERR 检查）→ return dev_err_probe(dev, PTR_ERR(...), "no tx dma channel\n")
 *        （pl330 驱动还没加载时会返回 -EPROBE_DEFER，dev_err_probe 会安静地处理）
 *
 * 2. 配置通道：
 *      【新结构体】struct dma_slave_config cfg = {};
 *        cfg.direction      = DMA_MEM_TO_DEV;                        内存 → 外设
 *        cfg.dst_addr       = hdev->phys_base + (UART_TX << HELLO_UART_REG_SHIFT);
 *                             外设那头的地址：TX 寄存器的【物理地址】（不是 hdev->base！为什么？见下面思考 ①）
 *        cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;             每次往 TX 寄存器写 1 个字节
 *        cfg.dst_maxburst   = dma_burst;                             UART 拉一次请求，PL330 连着搬几个
 *      ret = dmaengine_slave_config(hdev->txchan, &cfg);
 *      失败 → dma_release_channel(hdev->txchan)，返回 ret       （这时还没有 devm 帮你释放，要手动）
 *
 * 3. 分配 DMA 缓冲区（一致性映射）：
 *      【新写法】hdev->dma_buf = dma_alloc_coherent(hdev->txchan->device->dev, HELLO_UART_DMA_BUF_SIZE,
 *                                                   &hdev->dma_buf_addr, GFP_KERNEL);
 *        返回 CPU 用的虚拟地址；DMA 用的地址通过第三个参数（指针！）带出来——两个地址，同一块内存。
 *        第一个参数是【DMA 控制器】的 device（txchan->device->dev），不是 UART 的：
 *          真正去读这块内存的是 PL330，地址要按 PL330 能访问的范围来分配（思考 ②）
 *      NULL → dma_release_channel，返回 -ENOMEM
 *
 * 4. init_completion(&hdev->tx_done)
 *
 * 5. return devm_add_action_or_reset(dev, hello_uart_dma_release, hdev);
 *      把释放函数挂到 UART 设备的 devm 上（阶段 2 学过），remove 后自动调用；注册失败时会立即调用一次释放。
 *
 * 6. 返回前 dev_info 打印：dma_buf（%px）、dma_buf_addr（%pad，【新写法】这个格式要传 &hdev->dma_buf_addr）、
 *    dma_burst、PL330 设备名 dev_name(hdev->txchan->device->dev)
 *    （注意 5 是 return，打印要放在 5 前面）
 *
 * 思考：
 *   ① cfg.dst_addr 为什么必须是物理地址，不能是 hdev->base（ioremap 后的虚拟地址）？
 *   ② 为什么分配缓冲区用 DMA 控制器的 device，不用 UART 的 &pdev->dev？
 *   ③ dma_alloc_coherent 和第 1 小节 mmap 用的 __get_free_pages 都能分配物理连续的内存，区别是什么？
 *      （提示：CPU 写进 dma_buf 的数据，可能还停在 CPU 的 cache 里，没写到内存——PL330 从内存读，会读到什么？
 *       "coherent" 就是为了解决这个问题。）
 */
static int hello_uart_dma_init(struct hello_uart_dev *hdev, struct device *dev)
{
	struct dma_slave_config cfg = {};
	int ret;

	hdev->txchan = dma_request_chan(dev, "tx");
	if (IS_ERR(hdev->txchan))
		return dev_err_probe(dev, PTR_ERR(hdev->txchan), "no tx dma channel\n");

	cfg.direction      = DMA_MEM_TO_DEV;
	cfg.dst_addr       = hdev->phys_base + (UART_TX << HELLO_UART_REG_SHIFT);
	cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	cfg.dst_maxburst   = dma_burst;

	ret = dmaengine_slave_config(hdev->txchan, &cfg);
	if (ret) {
		dma_release_channel(hdev->txchan);
		return ret;
	}

	hdev->dma_buf = dma_alloc_coherent(hdev->txchan->device->dev, HELLO_UART_DMA_BUF_SIZE,
		&hdev->dma_buf_addr, GFP_KERNEL);
	if (!hdev->dma_buf) {
		dma_release_channel(hdev->txchan);
		return -ENOMEM;
	}

	init_completion(&hdev->tx_done);
	dev_info(dev, "dma_buf=%px, dma_buf_addr=%pad, dma_burst=%u, pl330 dev=%s\n",
			hdev->dma_buf, &hdev->dma_buf_addr, dma_burst,
			dev_name(hdev->txchan->device->dev));

	return devm_add_action_or_reset(dev, hello_uart_dma_release, hdev);

}

/* ============================ probe / remove ============================ */

static int hello_uart_probe(struct platform_device *pdev)
{
	struct hello_uart_dev *hdev;
	struct resource *res;
	int ret;

	hdev = devm_kzalloc(&pdev->dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev)
		return -ENOMEM;
	mutex_init(&hdev->lock);

	/*
	 * 【新写法】跟阶段 7 的 devm_platform_ioremap_resource 一样，多带出一个 res：
	 * res->start 就是设备树 reg 里的物理地址 0x2ad80000。DMA 要用物理地址（TODO 1 思考 ①）。
	 */
	hdev->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(hdev->base))
		return PTR_ERR(hdev->base);
	hdev->phys_base = res->start;

	hdev->pclk = devm_clk_get_enabled(&pdev->dev, "apb_pclk");
	if (IS_ERR(hdev->pclk))
		return dev_err_probe(&pdev->dev, PTR_ERR(hdev->pclk), "failed to get apb_pclk\n");
	hdev->baudclk = devm_clk_get_enabled(&pdev->dev, "baudclk");
	if (IS_ERR(hdev->baudclk))
		return dev_err_probe(&pdev->dev, PTR_ERR(hdev->baudclk), "failed to get baudclk\n");

	ret = hello_uart_hw_init(hdev);
	if (ret)
		return ret;

	/*
	 * TODO 5：use_dma 为真时调用 hello_uart_dma_init(hdev, &pdev->dev)，失败返回 ret。
	 * 放在 misc_register 之前：/dev 节点一出现用户就可能 write，DMA 必须已经准备好。
	 */
	if (use_dma) {
		ret = hello_uart_dma_init(hdev, &pdev->dev);
		if (ret)
			return ret;
	}

	hdev->misc.minor = MISC_DYNAMIC_MINOR;
	hdev->misc.name = "hello_uart_dma";
	hdev->misc.fops = &hello_uart_fops;
	hdev->misc.parent = &pdev->dev;

	platform_set_drvdata(pdev, hdev);
	ret = misc_register(&hdev->misc);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "registered /dev/%s, phys 0x%llx, use_dma %d\n",
		 hdev->misc.name, (u64)hdev->phys_base, use_dma);
	return 0;
}

/*
 * remove：先注销 misc（关入口），返回后 devm 逆序释放：DMA 资源（hello_uart_dma_release）→ 时钟 → 映射。
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
		.name		= "hello_uart_dma",
		.of_match_table	= hello_uart_of_match,
	},
};
module_platform_driver(hello_uart_driver);
