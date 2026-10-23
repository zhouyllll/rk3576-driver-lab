// SPDX-License-Identifier: GPL-2.0
/*
 * RK3576 驱动练习总览：一个通用结构，各类驱动在对应位置替换特殊部分。
 *
 * 阅读方式：按 ①～⑧ 阅读主框架，再看附录中的总线入口和资源对照。
 * 本文件是带 C 代码片段的学习笔记，不是可直接加载的“大而全”驱动。
 * 片段放在 #if 0 内：包含互斥方案、示意变量和待实现的 lab_* 函数，
 * 不要直接去掉 #if 0 编译；实际编译、上板以各目录的独立模块为单位。
 * API 写法沿用本仓库目标内核（~/rk3576-kernel，6.1.75）；移植时核对
 * 目标内核头文件中的签名，常见变化见附录 A 末尾。
 *
 * 源码索引（不包含编译生成的 *.mod.c）：
 * 01-chardev/hello_chrdev.c       cdev、设备号、每次 open 的缓冲区、ioctl
 * 02-platform-driver/hello_platform.c
 *                               platform、设备树、misc、sysfs、debugfs
 * 03-gpio/hello_gpio.c           GPIO 描述符、自定义 sysfs、hrtimer 方波
 * 03-gpio/hello_led.c            GPIO + LED 子系统
 * 03-gpio/hello_pwm.c            PWM consumer + LED 子系统
 * 04-irq-sleep/hello_irq.c       IRQ、下半部、消抖、阻塞 read、poll、统计
 * 04-irq-sleep/hello_key.c       GPIO IRQ + input 按键
 * 05-concurrency/hello_race.c    多字段一致性、自旋锁、completion、锁顺序
 * 06-i2c/hello_i2c.c             I2C 读取 IMX415 ID
 * 07-mmio-uart/hello_uart.c      MMIO、时钟、UART FIFO 轮询、内部回环
 * 08-dma-mmap/hello_mmap.c       页分配、用户态 mmap
 * 08-dma-mmap/hello_uart_dma.c   UART TX DMA、completion、超时终止
 * 08-dma-mmap/hello_bugs.c       内存错误调试练习
 * 09-spi/hello_spi.c             SPI 全双工回环、sysfs、driver_override
 * 10-pm/hello_uart_pm.c          UART runtime PM、时钟开关、自动休眠
 *
 * 通用调用链：
 * 模块入口 → 注册 driver → 总线匹配 device → probe
 *          → 分配状态 → 获取资源 → 初始化硬件 → 注册上层接口
 *          → 用户操作 / IRQ / 定时器 → 回调 → 操作硬件或搬运数据
 *          → remove / 模块退出 → 撤销接口、停止异步活动、释放资源
 *
 * 注意两条独立维度：
 *   “设备挂在哪里”：platform / I2C / SPI，决定 probe 和驱动注册方式。
 *   “向上提供什么”：misc / input / LED / sysfs，决定用户如何使用。
 * 例如 GPIO 按键可以同时是 platform 驱动和 input 设备，二者并不冲突。
 *
 * 设备树节点复用（同一节点同一时刻只能绑定一个驱动，换驱动前先 rmmod）：
 *   zhouyllll,hello-platform  02 hello_platform
 *   zhouyllll,hello-gpio      03 hello_gpio、hello_led
 *   zhouyllll,hello-pwm       03 hello_pwm
 *   zhouyllll,hello-irq       04 hello_irq、hello_key，05 hello_race
 *   zhouyllll,hello-uart      07 hello_uart、08 hello_uart_dma、10 hello_uart_pm
 *   sony,imx415               06 hello_i2c（先 unbind 原厂 imx415 驱动）
 *   spi4.0                    09 hello_spi（无 of 表，driver_override 绑定）
 *   无设备树                  01 hello_chrdev、08 hello_mmap、hello_bugs
 * 复用 compatible 只是为了省去改设备树重刷；正式驱动一个 compatible 对应一种硬件。
 */

#if 0 /* 以下为教学片段，不参与构建。 */

/* ==================== ① 头文件、常量、模块参数 ==================== */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/completion.h>
/* 按需再加 gpio/consumer.h、input.h、leds.h、pwm.h、interrupt.h、
 * timer.h、hrtimer.h、i2c.h、spi/spi.h、io.h、clk.h、dmaengine.h、
 * dma-mapping.h、mm.h、pm_runtime.h 等，不需要的功能不用引入。 */

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver structure study notes");

static unsigned int debounce_ms = 20;
module_param(debounce_ms, uint, 0444); /* insmod 配置；sysfs 只读。 */
/* 02 的 buf_size_override、07 的 baud、09 的 xfer_len、10 的
 * autosuspend_ms 都属于配置，不是设备实例的运行状态。 */

/* ==================== ② 私有数据：先划清生命周期 ==================== */
struct lab_dev {
	struct device *dev;           /* 保存内核传来的设备，自己不 kfree。 */
	struct miscdevice misc;       /* 嵌入对象，回调可用 container_of 找回外层。 */
	struct mutex io_lock;        /* 进程上下文串行化，可以保护可睡眠操作。 */
	spinlock_t irq_lock;         /* IRQ 与进程共享的短临界区。 */
	wait_queue_head_t wq;        /* 等待“条件为真”，用于 read/poll。 */
	struct completion done;      /* 等待“一次任务完成”，例如 DMA。 */
	atomic_t events;             /* 单个计数器；不能保证多个字段一起一致。 */
	bool dead;                  /* 完整热拔插实现需要离线状态及对象引用管理。 */

	/* 按功能选择成员，不要求每个驱动把以下成员全放进去。 */
	struct gpio_desc *gpio;      /* 03/04/05：GPIO consumer。 */
	int irq;
	struct timer_list debounce;  /* 04：普通消抖定时器。 */
	struct hrtimer wave;         /* 03/05：高精度周期任务。 */
	struct work_struct work;     /* 04：可睡眠下半部。 */
	struct input_dev *input;     /* 04 hello_key：input 核心拥有事件接口。 */
	struct led_classdev led;     /* 03 hello_led/hello_pwm：嵌入 LED 对象。 */
	struct pwm_device *pwm;
	struct i2c_client *client;   /* 06：总线设备由 I2C 核心创建。 */
	struct spi_device *spi;      /* 09：总线设备由 SPI 核心创建。 */
	void __iomem *base;          /* 07/08/10：CPU 访问寄存器的映射地址。 */
	phys_addr_t phys_base;       /* 寄存器物理地址，不等于 base。 */
	struct clk *pclk, *baudclk;
	struct dma_chan *txchan;
	void *dma_buf;              /* CPU 访问 DMA 缓冲区的虚拟地址。 */
	dma_addr_t dma_addr;         /* DMA 设备使用的地址，不能直接解引用。 */
};

struct lab_file {
	struct lab_dev *hdev;        /* 反向指回设备；需确保设备对象仍然存活。 */
	char *buf;
	size_t size, len;
	struct mutex lock;
	int seen;                   /* 04：这个打开实例已经读到的事件计数。 */
};
/* 设备级：probe 分配，解绑后释放；打开实例级：open 分配，release 释放。
 * 01/02 当前代码的缓冲区是每次 open 独立的：一个 shell echo 后再 cat
 * 会再次 open，不能据此读取前一个打开实例的缓冲区。
 * 需要跨 open 共享的数据，应移到 lab_dev，而不是 lab_file。
 * dup/fork 可共享同一个 struct file；“每次 open”不等于“每个进程”。
 */

/* ==================== ③ 底层操作：每类驱动的特殊部分 ==================== */

/* [03 GPIO] 逻辑电平受设备树 GPIO_ACTIVE_LOW/HIGH 影响。 */
static void lab_gpio_set(struct lab_dev *hdev, bool on)
{
	gpiod_set_value_cansleep(hdev->gpio, on); /* 进程上下文可用。 */
}
/* IRQ/普通 timer 回调使用 gpiod_get/set_value 的前提：
 * gpiod_cansleep(gpio) 为 false；I2C GPIO 扩展器应改用线程/work。 */

/* [03 LED] LED 核心处理 brightness/trigger，驱动只实现硬件动作。 */
static int lab_led_set(struct led_classdev *led, enum led_brightness value)
{
	struct lab_dev *hdev = container_of(led, struct lab_dev, led);
	gpiod_set_value_cansleep(hdev->gpio, value != LED_OFF);
	return 0; /* 配给 brightness_set_blocking，而非 brightness_set。 */
}

/* [03 PWM] 相同的 LED 上层接口，底层换成占空比控制。 */
static int lab_pwm_set(struct led_classdev *led, enum led_brightness value)
{
	struct lab_dev *hdev = container_of(led, struct lab_dev, led);
	struct pwm_state state;
	pwm_init_state(hdev->pwm, &state); /* 获取初始周期、极性等配置。 */
	pwm_set_relative_duty_cycle(&state, value, led->max_brightness);
	state.enabled = value != 0;
	return pwm_apply_state(hdev->pwm, &state); /* 可睡眠，检查返回值。 */
}

/* [06 I2C] 本项目是 7 位从机地址、16 位大端寄存器地址、8 位数据。 */
static int lab_i2c_read(struct i2c_client *client, u16 reg, u8 *value)
{
	u8 addr[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[] = {
		{ .addr = client->addr, .flags = 0, .len = 2, .buf = addr },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = 1, .buf = value },
	};
	int ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	/* 返回的是消息数量，不是读取字节数；短传输也算失败。 */
	return ret == ARRAY_SIZE(msgs) ? 0 : (ret < 0 ? ret : -EIO);
}
/* 原例读取 0x311A，期望 0xE0；这只是芯片 ID 验证，不是完整摄像头驱动。
 * 原实验依赖传感器仍上电；完整驱动还需管理电源、复位、时钟。
 */

/* [07 MMIO UART] 当前硬件寄存器间距为 4 字节，索引左移 2 位。 */
static int lab_uart_tx_byte(struct lab_dev *hdev, u8 ch)
{
	u32 status;
	int ret;
	/* 进程上下文轮询示意：必须设置超时，不能无限等待硬件。 */
	ret = readl_poll_timeout(hdev->base + (UART_LSR << 2), status,
				status & UART_LSR_THRE, 10, 10000);
	if (ret)
		return ret;
	writel(ch, hdev->base + (UART_TX << 2));
	return 0;
}
/* readl_poll_timeout 在 <linux/iopoll.h>；07 原例手写 1000 次 × udelay(10)
 * 的循环，效果相同，都是“有上限的忙等”。
 * base 必须来自 ioremap 类 API；不能把物理地址强转指针直接访问。
 * 07 的 hw_init：clk_set_rate(baudclk, 16 * baud) → 关 IER → DLAB=1 写
 * DLL/DLM 分频 → LCR=8N1（同时清 DLAB）→ FCR 开并清 FIFO → MCR_LOOP 回环。
 * 分频按 clk_get_rate 的实际值计算并检查为 0：baudclk 默认 187500 Hz 时
 * 分频为 0、发送超时（DEBUG-CASES 案例 16）。
 * skip_clk 练习：不开时钟访问寄存器在本板不 oops，读到残留值（案例 17），
 * 所以 probe 里用 SCR 写 0x5a/0xa5 再读回，验证寄存器真的可访问。
 * 原例是 misc 教学接口，不是 serial_core/TTY 驱动。
 */

/* [09 SPI] tx/rx 使用已分配且适合传输的缓冲，避免栈上 DMA 缓冲。 */
static int lab_spi_loopback(struct spi_device *spi, u8 *tx, u8 *rx, size_t n)
{
	struct spi_transfer t = { .tx_buf = tx, .rx_buf = rx, .len = n }; /* 其余成员为 0。 */
	int ret = spi_sync_transfer(spi, &t, 1); /* 同步接口，可睡眠。 */
	if (ret)
		return ret;
	return memcmp(tx, rx, n) ? -EIO : 0;
}
/* 原例设置 mode 0、8 bits/word、speed_hz，然后 spi_setup。
 * SPI 没有 I2C 式 ACK，控制器传输成功不等于外设数据正确；
 * 控制器超时等错误仍然会让 SPI API 返回失败。
 * 用 PIO 还是 DMA 由控制器驱动和传输条件决定，不是外设驱动强行保证。
 * 原例 tx/rx 由 devm_kzalloc 分配（xfer_len 限制 1～4096），一把 mutex 串行化
 * 共享缓冲；每次测试改变起始值 seq，避免残留数据“碰巧相等”。
 * 曾写 memset(&t, 0, xfer_len)：清错对象，长度大时踩坏内核栈；rx_buf 还指向了
 * tx（案例 22）。结构体用 = {} 初始化，memset 长度写 sizeof(对象)。
 */

/* ==================== ④ 异步事件、消抖与并发 ==================== */
static irqreturn_t lab_key_irq(int irq, void *data)
{
	struct lab_dev *hdev = data;
	/* 硬中断尽快返回：每次边沿把消抖检查推迟到最后一个边沿之后。 */
	mod_timer(&hdev->debounce, jiffies + msecs_to_jiffies(debounce_ms));
	return IRQ_HANDLED;
}

static void lab_key_debounce(struct timer_list *timer)
{
	struct lab_dev *hdev = from_timer(hdev, timer, debounce);
	int value = gpiod_get_value(hdev->gpio); /* 此处要求 GPIO 不睡眠。 */
	if (value < 0)
		return;
	input_report_key(hdev->input, KEY_PROG1, value);
	input_sync(hdev->input); /* 完成一组事件；按下 1、松开 0。 */
}

/* [04 hello_irq] 自定义事件接口可用计数器通知读者，替代上述 input 上报。 */
static void lab_publish_event(struct lab_dev *hdev)
{
	atomic_inc(&hdev->events);             /* 先更新条件，再唤醒。 */
	wake_up_interruptible(&hdev->wq);
}
/* 下半部选择：
 * 硬 IRQ：不能睡眠；共享 IRQ 还要判断是否属于本设备，否则 IRQ_NONE。
 * tasklet / timer_list：按不可睡眠上下文编写；tasklet 是原例的比较练习。
 * workqueue：普通工作队列回调可睡眠，schedule_work 提交。
 * threaded IRQ：顶半部 IRQ_WAKE_THREAD，线程处理；原例用 IRQF_ONESHOT。
 * hrtimer：03 产生方波，05 模拟周期写者；不能仅因“定时器”就假设可睡眠。
 * work/tasklet 可能合并重复调度；需要逐个保留事件时应另建计数或队列。
 * 原例 bh=0..3 切换四种方式，打印 comm/pid/preempt_count/in_task 和
 * 顶半部到下半部的延迟；只有 in_task() 为真时才 usleep_range。
 * devm 顺序：先 devm_add_action_or_reset(bh_cleanup) 再 devm_request_irq，
 * 解绑时逆序 → 先 free_irq（不再有新调度），再 tasklet_kill/cancel_work_sync/
 * del_timer_sync。顺序反了，清理后 IRQ 仍可能重新调度下半部。
 * 消抖定时器到期再读电平确认，用 atomic_xchg 取出并清零期间的原始边沿数。
 * measure=1 时顶半部记录相邻中断间隔，用 spinlock 保护 n/min/max/sum 多字段。
 */

/* [05] 多个相关字段必须在同一把锁下更新和取快照。 */
static ssize_t lab_snapshot_read(/* 示意：实际补齐 file_operations 参数 */)
{
	unsigned long flags;
	struct hello_snapshot local = {}; /* 初始化；拷给用户的结构体不能带未初始化内容。 */
	spin_lock_irqsave(&hdev->irq_lock, flags);
	local.seq = hdev->snap.seq;
	local.ts_ns = hdev->snap.ts_ns;
	local.level = hdev->snap.level;
	local.seq_check = hdev->snap.seq_check;
	spin_unlock_irqrestore(&hdev->irq_lock, flags);
	/* copy_to_user 可能缺页睡眠，必须在自旋锁外执行。 */
	return copy_to_user(ubuf, &local, sizeof(local)) ? -EFAULT : sizeof(local);
}
/* 这里 snap/ubuf 是片段占位变量。真实结构见 hello_race.c / hello_race.h。
 * hello_snapshot 用显式 __u32 pad 补齐，内核和用户态布局一致且没有隐式填充；
 * 有隐式填充的结构，“= {}”不保证填充字节为 0，应 memset 后再逐字段赋值。
 * 原例用 ndelay(slow_ns) 拉大写/读字段之间的窗口；use_lock=0 时读到
 * seq != seq_check 即为撕裂读，use_timer=1 改用 hrtimer 做写者。
 * 进程持锁时如果本 CPU IRQ 也取同一把锁，需 irqsave 避免自锁；
 * 原例 bug_noirq=1 故意在 read 里用普通 spin_lock，让 lockdep 报告。
 * IRQ 处理函数里已经关本地中断，用普通 spin_lock 即可（hello_race_update）。
 * READ_ONCE/WRITE_ONCE 不是互斥锁，也不保证多字段一致性。
 * 两把 mutex 的获取顺序要统一；原例 AB/BA 用于演示 lockdep 报告。
 */

/* [08 UART DMA] 中断完成通知，不在回调里释放仍被使用的状态。 */
static void lab_dma_done(void *data)
{
	struct lab_dev *hdev = data;
	complete(&hdev->done);
}
static int lab_dma_send(struct lab_dev *hdev, size_t n)
{
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	/* 调用者持 io_lock，保证共享缓冲和 completion 只有一个发送者使用。 */
	reinit_completion(&hdev->done);
	desc = dmaengine_prep_slave_single(hdev->txchan, hdev->dma_addr, n,
			DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc)
		return -EIO;
	desc->callback = lab_dma_done;
	desc->callback_param = hdev;
	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie))
		return dma_submit_error(cookie);
	/* 原例此处还有 DW_UART_DMASA 写 1：硬件相关动作不能通用化省掉。 */
	dma_async_issue_pending(hdev->txchan);
	if (!wait_for_completion_timeout(&hdev->done, msecs_to_jiffies(1000))) {
		dmaengine_terminate_sync(hdev->txchan); /* 停止后才能重用/释放缓冲。 */
		return -ETIMEDOUT;
	}
	return 0;
}
/* slave_config：dst_addr 是 UART THR 的【物理】地址
 *   phys_base + (UART_TX << 2)，phys_base 来自
 *   devm_platform_get_and_ioremap_resource 返回的 res->start；
 *   dst_addr_width 1 字节，dst_maxburst 16（与 Rockchip 8250 驱动一致）。
 * CPU 写 dma_buf（虚拟地址），PL330 读 dma_addr（DMA 地址），是同一块内存。
 * copy_from_user 到共享 dma_buf 也要在 io_lock 内，否则并发 write 会互相覆盖。
 * 返回值约定要统一：本片段成功返回 0，原例 hello_uart_tx_dma 成功返回 n。
 * 曾按“0 才成功”判断 n，导致成功也报超时且持锁返回、cat 卡死（案例 20）；
 * 锁内每条 return 都要先解锁，调用者按被调函数的约定原样传递返回值。
 * DMA 完成只表示本次 DMA 搬运完成，不保证 UART 移位寄存器已发完。
 * 05 的 WAIT_N 也是 completion 用法：先初始化，再启动计数/任务，
 * 成功 >0、超时 0、可中断等待的信号错误 <0；重置前必须排除旧任务。
 */

/* ==================== ⑤ 上层接口：按设备语义选择 ==================== */

/* [01/02 misc 或 cdev] 每次打开分配会话，失败时逆序释放。 */
static int lab_open(struct inode *inode, struct file *file)
{
	/* misc_open 预先将 private_data 设置为 miscdevice 指针。 */
	struct lab_dev *hdev = container_of(file->private_data, struct lab_dev, misc);
	struct lab_file *priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->size = 256;
	priv->buf = kzalloc(priv->size, GFP_KERNEL);
	if (!priv->buf) {
		kfree(priv);
		return -ENOMEM;
	}
	priv->hdev = hdev;
	priv->seen = atomic_read(&hdev->events);
	mutex_init(&priv->lock);
	file->private_data = priv;
	return 0;
}
static int lab_release(struct inode *inode, struct file *file)
{
	struct lab_file *priv = file->private_data;
	kfree(priv->buf);
	kfree(priv);
	return 0;
}
/* misc_open 先设 private_data 再调驱动的 open；驱动没有 .open 时它保持为
 * miscdevice 指针，07/08/10 的 to_hdev() 直接 container_of 取设备。
 * 02/04 在 open 里把 private_data 换成会话 priv；05 换成 hdev 本身。
 * 换掉之后 read/write/ioctl/release 拿到的都是新值，不能再按 misc 指针解释。
 * cdev 的 open 则从 inode->i_cdev 用 container_of 找设备，不套 misc 的规则。
 * read/write：先校验负偏移、容量，再拷贝，成功后推进 *ppos；EOF 返回 0。
 * 先检查 *ppos >= size 再相减：pwrite 可指定任意偏移，size - *ppos 会下溢成
 * 巨大的无符号数（案例 6）；写满返回 -ENOSPC，不要返回 0 让用户态重试。
 * copy_to/from_user 返回“未拷贝字节数”，非零表示未全部成功，不能判断 <0。
 * 原 01 hello_write 仍有三处不应复制成模板：ret < 0 判断；加锁前就用
 * *ppos 计算 BUF_SIZE - *ppos，且未先检查越界；未知 ioctl 返回 -EINVAL。
 * 02 是按案例 6 修正后的版本，写 read/write 时以 02 为准。
 * 没有文件偏移语义的事件流（04/05）在 open 里 return stream_open()，
 * 并设 .llseek = no_llseek；07/08/10 用 noop_llseek，08 mmap 用 default_llseek。
 * 简单缓冲可用 simple_read/write_to_buffer；仍需设计并发和有效长度。
 * ioctl：与用户程序共用 hello_ioctl.h；检查用户指针，未知命令用 -ENOTTY。
 */

/* [04] 阻塞 read 的核心：循环条件由 wait_event 宏处理，不能先睡再检查。 */
static int lab_wait_event(struct file *file, struct lab_file *priv)
{
	struct lab_dev *hdev = priv->hdev;
	if (atomic_read(&hdev->events) != priv->seen)
		return 0;
	if (file->f_flags & O_NONBLOCK)
		return -EAGAIN;
	return wait_event_interruptible(hdev->wq,
		atomic_read(&hdev->events) != priv->seen || READ_ONCE(hdev->dead));
}
/* 唤醒后应检查 dead，离线返回 -ENODEV；成功 copy_to_user 后再更新 seen。
 * 同一个打开实例的并发 read 还需串行化；计数快照不能当逐事件 FIFO。
 */
static __poll_t lab_poll(struct file *file, poll_table *wait)
{
	struct lab_file *priv = file->private_data;
	poll_wait(file, &priv->hdev->wq, wait); /* 注册等待，不在这里主动睡眠。 */
	if (READ_ONCE(priv->hdev->dead))
		return EPOLLHUP | EPOLLERR;
	return atomic_read(&priv->hdev->events) != priv->seen ?
		EPOLLIN | EPOLLRDNORM : 0;
}

/* [08 mmap] 仅适用于该例 __get_free_pages 得到的连续普通页。 */
static int lab_mmap(struct file *file, struct vm_area_struct *vma)
{
	size_t size = vma->vm_end - vma->vm_start;
	unsigned long offset;
	/* 先按页检查，再移位；用减法限制长度，避免 offset + size 溢出。 */
	if (vma->vm_pgoff >= (hbuf_size >> PAGE_SHIFT))
		return -EINVAL;
	offset = vma->vm_pgoff << PAGE_SHIFT;
	if (size > hbuf_size - offset)
		return -EINVAL;
	return remap_pfn_range(vma, vma->vm_start,
		(virt_to_phys((void *)hbuf) >> PAGE_SHIFT) + vma->vm_pgoff,
		size, vma->vm_page_prot);
}
/* hbuf/hbuf_size 见 hello_mmap.c；不要对任意 vmalloc/DMA 地址套 virt_to_phys。
 * DMA coherent 缓冲映射应选适合该 DMA 分配方式的 API，例如 dma_mmap_coherent。
 * mmap 不提供同步：用户与内核共享访问仍需协议；映射存活期间内存不能释放。
 * remap_pfn_range 不知道哪些页属于驱动：no_size_check=1 时普通用户能读到
 * 相邻物理页的内容（案例 18）。原例最终写法 ((vm_pgoff << PAGE_SHIFT) + size)
 * > hbuf_size 已修正了移位优先级和页/字节单位混用；上面的写法再防加法溢出。
 * VMA 持有 struct file 引用，close(fd) 后映射仍在时 rmmod 返回 -EWOULDBLOCK，
 * munmap 或进程退出后才能卸载（案例 19）。
 */

static const struct file_operations lab_fops = {
	.owner = THIS_MODULE,
	.open = lab_open,
	.release = lab_release,
	/* .read/.write/.unlocked_ioctl/.poll/.mmap 按需要选择，
	 * 不能把相互不兼容的缓冲、事件流和 mmap 语义机械拼在一起。 */
};

/* [02/03/06/09 sysfs] 一个属性对应一项状态或控制。 */
static ssize_t value_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct lab_dev *hdev = dev_get_drvdata(dev);
	int value = gpiod_get_value_cansleep(hdev->gpio);
	return value < 0 ? value : sysfs_emit(buf, "%d\n", value);
}
static ssize_t value_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct lab_dev *hdev = dev_get_drvdata(dev);
	bool value;
	int ret = kstrtobool(buf, &value);
	if (ret)
		return ret;
	gpiod_set_value_cansleep(hdev->gpio, value);
	return count;
}
static DEVICE_ATTR_RW(value);
static struct attribute *lab_attrs[] = { &dev_attr_value.attr, NULL };
ATTRIBUTE_GROUPS(lab); /* 在 driver.dev_groups 中引用 lab_groups。 */
/* 属性挂在哪个 struct device 上，dev_get_drvdata 就取哪个设备的 drvdata：
 *   03 value/half_period_us、04 irq_stats：driver.dev_groups → platform 设备，
 *       drvdata 是 platform_set_drvdata 存的 hdev。
 *   06 chip_id、09 loopback：driver.dev_groups → i2c_client / spi_device。
 *   02 buf_size：miscdev.groups → /sys/class/misc/<name>/，drvdata 是
 *       misc_register 存的 miscdevice 指针，要 container_of 回 hello_dev。
 * show 里访问硬件失败应返回负错误码，不要返回 0 让用户读到空内容（06 修正过）。
 * 每次 cat 都会重新执行 show：06 的读 ID、09 的一次 SPI 回环都是真实总线访问。
 * debugfs 是调试接口，02 用于 open_count；移除时 debugfs_remove_recursive。
 * pr_debug/dev_dbg 可通过 dynamic debug 控制，不替代正式用户接口。
 */

/* ==================== ⑥ probe：公共顺序中的可替换位置 ==================== */
static int lab_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct lab_dev *hdev;
	int ret;

	/* A. 分配设备状态；先初始化所有可能被回调访问的成员。 */
	hdev = devm_kzalloc(dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev)
		return -ENOMEM;
	hdev->dev = dev;
	mutex_init(&hdev->io_lock);
	spin_lock_init(&hdev->irq_lock);
	init_waitqueue_head(&hdev->wq);
	init_completion(&hdev->done);
	atomic_set(&hdev->events, 0);
	platform_set_drvdata(pdev, hdev);

	/* B. 读取设备树配置：02 的 buf-size、label；检查大小及参数覆盖。 */
	/* of_property_read_u32(dev->of_node, "buf-size", &size); */

	/* C. 获取硬件资源：以下分支按目标设备选择，不是全部执行。 */
	/* GPIO：devm_gpiod_get(dev, "led", GPIOD_OUT_LOW)，匹配 led-gpios。
	 * 按键：devm_gpiod_get(dev, "irq", GPIOD_IN)，再 gpiod_to_irq。
	 * PWM ：devm_pwm_get(dev, NULL)，匹配 pwms。
	 * MMIO：devm_platform_ioremap_resource(pdev, 0)，匹配 reg。
	 * 时钟：07/08 用 devm_clk_get_enabled；10 用 devm_clk_get 后由 PM 开关。
	 * DMA ：dma_request_chan(dev, "tx")，匹配 dma-names/dmas。
	 *       slave_config 设置外设地址、总线宽度、burst；分配 coherent 缓冲。
	 *       原 08 使用 txchan->device->dev 分配/释放，须保持配对设备一致。
	 * 错误：分配失败 NULL → -ENOMEM；ERR_PTR → IS_ERR/PTR_ERR；
	 *       保留 -EPROBE_DEFER，供应者尚未就绪时允许内核重试。
	 */

	/* D. 初始化硬件，登记回滚，再启用异步来源。
	 * 07/08：打开时钟后配置 UART；DMA 回调前先 init_completion。
	 * 04 按键的具体顺序见下面独立片段。
	 */

	/* E. 最后公布可被用户访问的 misc 接口；名字需考虑多实例唯一性。 */
	hdev->misc.minor = MISC_DYNAMIC_MINOR;
	hdev->misc.name = "lab_example";
	hdev->misc.fops = &lab_fops;
	hdev->misc.parent = dev;
	ret = misc_register(&hdev->misc);
	if (ret)
		return dev_err_probe(dev, ret, "misc registration failed\n");
	return 0;
}

/* [04 input 替换 E，并在请求 IRQ 前完成注册] */
static int lab_register_key(struct lab_dev *hdev)
{
	int ret;
	hdev->input = devm_input_allocate_device(hdev->dev);
	if (!hdev->input)
		return -ENOMEM;
	hdev->input->name = "lab-key";
	hdev->input->id.bustype = BUS_HOST;
	input_set_capability(hdev->input, EV_KEY, KEY_PROG1);
	set_bit(EV_REP, hdev->input->evbit); /* 可选自动重复：是 EV_REP，不是 KEY_REP。 */
	/* EV_KEY/EV_REP 是 evbit 中的事件类型；KEY_PROG1 是 keybit 中的键码。
	 * 自动重复由 input 核心产生 EV_KEY、KEY_PROG1、value=2。
	 * 原 hello_key.c 尚未启用 EV_REP，这里是结合前面讨论补充的写法。 */
	ret = input_register_device(hdev->input);
	if (ret)
		return ret;
	timer_setup(&hdev->debounce, lab_key_debounce, 0);
	/* lab_timer_cleanup 需实现为 del_timer_sync(&hdev->debounce)。 */
	ret = devm_add_action_or_reset(hdev->dev, lab_timer_cleanup, hdev);
	if (ret)
		return ret;
	return devm_request_irq(hdev->dev, hdev->irq, lab_key_irq,
		IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "lab-key", hdev);
}
/* 逆序释放：free_irq → del_timer_sync → 注销 input → 释放 GPIO/状态。
 * [03 LED 替换 E] 设置 name、max_brightness、brightness_set_blocking，
 * 再 devm_led_classdev_register；GPIO LED 最大亮度 1，PWM 原例 255。
 */

/* ==================== ⑦ 电源管理：包住每次硬件访问 ==================== */
static int lab_pm_io(struct lab_dev *hdev)
{
	int ret = pm_runtime_resume_and_get(hdev->dev);
	if (ret < 0)
		return ret; /* 获取失败不能再做配对 put。 */
	mutex_lock(&hdev->io_lock);
	ret = lab_do_hardware_io(hdev); /* 示意：UART 寄存器读写。 */
	mutex_unlock(&hdev->io_lock);
	pm_runtime_mark_last_busy(hdev->dev);
	pm_runtime_put_autosuspend(hdev->dev); /* 所有成功 get 的路径都要 put。 */
	return ret;
}
/* 10 的 probe：获取时钟 → 开时钟/初始化 → get_noresume 保持引用
 * → set_active → 设置 autosuspend_delay → use_autosuspend → enable
 * → 注册 misc → mark_last_busy → put_autosuspend。
 * runtime_suspend：确认硬件可休眠后关时钟。
 * runtime_resume：开时钟，必要时恢复丢失的寄存器；失败回滚已开启的时钟。
 * 原例用 reinit_on_resume 对比；完整 UART 还需判断发送真正结束、接收需求。
 * probe 中 misc_register 失败的回滚：pm_runtime_disable → set_suspended
 *   → put_noidle（抵消 get_noresume）→ dont_use_autosuspend → 关时钟。
 * remove：misc_deregister → pm_runtime_disable → 若未处于 suspended 则关时钟
 *   → set_suspended → dont_use_autosuspend。时钟不是 devm_clk_get_enabled，
 *   开关次数必须由驱动自己配平。
 * read/write：先 pm_runtime_resume_and_get，再拿 io_lock；mark_last_busy +
 *   put_autosuspend 在解锁之后。copy_from_user 放在 get 之前，失败无需 put。
 * dev_pm_ops 挂在 driver.pm；runtime PM 不等于系统 suspend/resume。
 * 10 用 DEFINE_RUNTIME_DEV_PM_OPS(ops, suspend, resume, NULL)：系统休眠回调是
 *   pm_runtime_force_suspend/resume，复用同一对 runtime 回调；已经
 *   runtime suspended 的设备，系统休眠时不会重复关时钟。
 * pm_ptr() 在 CONFIG_PM 关闭时得到 NULL，配合 DEFINE_* 宏不会留下未使用警告。
 * 避免 PM resume 回调与调用者重复获取同一 mutex 形成死锁。
 */

/* ==================== ⑧ remove：停止回调后才能释放其依赖 ==================== */
static int lab_remove(struct platform_device *pdev)
{
	struct lab_dev *hdev = platform_get_drvdata(pdev);
	/* 以下仅是生命周期顺序提示，完整热拔插需自行补全引用/并发管理。 */
	misc_deregister(&hdev->misc); /* 阻止新 open，不会撤销已经打开的 fd。 */
	WRITE_ONCE(hdev->dead, true);
	wake_up_interruptible(&hdev->wq);
	/* 停硬件事件源/释放 IRQ，等待在途处理结束；再同步停 timer/work/DMA。
	 * 若用 devm 清理，按依赖关系安排登记顺序，不要手工重复释放。
	 * 不要持有回调需要的锁调用 cancel_work_sync/del_timer_sync 等待它。
	 * PM disable 后按实际电源状态关时钟；debugfs 删除；归还手动资源。
	 */
	return 0; /* 返回后 devm 按申请顺序的逆序释放。 */
}
/* misc_register 不是 devm：remove 漏了 misc_deregister，rmmod 后 /dev 节点
 * 还在，再打开就会调用已卸载的代码（案例 5）。手动注册的接口必须手动注销。
 * devm 只管理设备解绑时的资源释放，不自动处理存活的 fd/VMA。
 * .owner=THIS_MODULE 防止常规卸载时模块代码被释放，却不阻止 sysfs unbind：
 * 打开的 fd、mmap 的 VMA 都持有模块引用，rmmod 会失败（案例 19），
 * 但 echo <dev> > /sys/bus/platform/drivers/<drv>/unbind 仍会调用 remove。
 * 完整热拔插要让旧 fd 返回离线错误，并通过引用计数等方式延迟释放状态；
 * 不能把本文件 devm 分配 + open 保存指针的片段当作完整热拔插方案。
 */

/* ==================== 附录 A：模块入口，按总线选择一种 ==================== */
static const struct of_device_id lab_of_match[] = {
	{ .compatible = "example,lab-device" }, /* 示意；实际必须匹配设备树。 */
	{ }
};
MODULE_DEVICE_TABLE(of, lab_of_match);
static struct platform_driver lab_driver = {
	.probe = lab_probe,
	.remove = lab_remove, /* 本项目写法；其他内核版本可能要求 void 返回值。 */
	.driver = {
		.name = "lab_example",
		.of_match_table = lab_of_match,
		/* .dev_groups = lab_groups,  仅在获取了 GPIO 等所需资源时注册。 */
		/* .pm = pm_ptr(&lab_pm_ops), 仅 PM 版本需要。 */
	},
};
module_platform_driver(lab_driver);

/* 换成 I2C 时（06），替换上面的入口，而不是额外加第二个 init：
 *   probe(struct i2c_client *client)，设备为 &client->dev；
 *   i2c_set/get_clientdata；struct i2c_driver；module_i2c_driver。
 *   本仓库使用 .probe_new，匹配 sony,imx415；版本迁移核对字段。
 *   6.1 中 i2c_driver.remove 返回 void，与 platform 的 int 不同。
 *   msgs 的 addr 用 client->addr；wrong_addr 练习换成不存在的地址看 NACK。
 * 换成 SPI 时（09）：
 *   probe(struct spi_device *spi)，设备为 &spi->dev；
 *   spi_set/get_drvdata；struct spi_driver；module_spi_driver。
 *   6.1 中 spi_driver.remove 同样返回 void。
 *   本例无 of 表，依赖 driver_override=hello_spi 绑定已有 spi4.0：
 *     echo spi4.0 > /sys/bus/spi/drivers/spidev/unbind
 *     echo hello_spi > /sys/bus/spi/devices/spi4.0/driver_override
 *     insmod hello_spi.ko          （注册驱动时匹配上 → probe）
 *   还原：rmmod；echo > .../driver_override；echo spi4.0 > .../spidev/bind。
 *   driver_override 优先于 of 表，只按 driver.name 匹配，.name 必须一致。
 * I2C 同理先解绑：echo 8-0037 > /sys/bus/i2c/drivers/imx415/unbind，
 *   再 insmod，hello_i2c 以同一 compatible 匹配 8-0037（I2C8，地址 0x37）。
 * 不依赖硬件时（01/08 mmap/08 bugs）：
 *   module_init/module_exit，或 module_misc_device；不必造 platform probe。
 *   01 手工 cdev：alloc_chrdev_region → cdev_init/cdev_add
 *               → class_create → device_create。
 *   退出：device_destroy → class_destroy → cdev_del → unregister_chrdev_region。
 *   本仓库 01 实际 class_create(THIS_MODULE, name)；不要照搬其过时注释。
 *   每一步失败只回滚已经成功的步骤。
 *   08 hello_bugs 用 module_misc_device(misc)：只注册一个 misc 时代替 init/exit。
 *
 * 跨版本常见变化（以目标内核头文件为准，下面是升级时最常碰到的几处）：
 *   class_create(THIS_MODULE, name)   → 6.4 起只有 name 一个参数
 *   i2c_driver.probe_new              → 6.3 起 .probe 即单参数版，6.6 删除 probe_new
 *   platform_driver.remove 返回 int   → 6.11 起返回 void
 *   pwm_apply_state                   → 6.8 起 pwm_apply_might_sleep
 *   no_llseek                         → 6.12 删除，不设 .llseek 即可
 *   hrtimer_init + .function          → 6.13 起 hrtimer_setup
 *   del_timer_sync / from_timer       → timer_delete_sync / timer_container_of
 */

/* ==================== 附录 B：申请与释放速查 ====================
 * kzalloc                         → kfree
 * devm_kzalloc                    → 解绑自动释放（对象不能被旧 fd 继续访问）
 * misc_register                   → misc_deregister
 * devm_gpiod_get / devm_pwm_get    → devm 自动归还
 * devm_led_classdev_register      → devm 自动注销
 * devm_input_allocate_device
 *   + input_register_device       → managed input 生命周期自动处理
 * devm_request_irq                → devm free_irq，但不替你取消已排队 work
 * timer_setup/mod_timer           → del_timer_sync（先防止其他路径重新入队）
 * hrtimer_init/start              → hrtimer_cancel
 * INIT_WORK/schedule_work         → cancel_work_sync
 * tasklet_setup/schedule          → tasklet_kill
 * dma_request_chan               → dma_release_channel（先 terminate_sync）
 * dma_alloc_coherent              → dma_free_coherent（设备、大小、地址配对）
 * __get_free_pages                → free_pages（同一个 order，映射已结束）
 * clk_prepare_enable             → clk_disable_unprepare
 * devm_clk_get_enabled           → 解绑时自动 disable_unprepare + put
 * devm_platform_ioremap_resource → 解绑时自动 iounmap + 释放内存区域
 * mutex_init                     → mutex_destroy（调试选项下能查出锁未释放）
 * alloc_chrdev_region / cdev_add
 *   / class_create / device_create → 逆序 device_destroy … unregister_chrdev_region
 * pm_runtime_enable               → pm_runtime_disable
 * pm_runtime_get_noresume         → pm_runtime_put_noidle / put_autosuspend
 * pm_runtime_use_autosuspend      → pm_runtime_dont_use_autosuspend
 * pm_runtime_resume_and_get       → pm_runtime_put_autosuspend 等配对 put
 * debugfs_create_*               → debugfs_remove_recursive
 *
 * devm_add_action_or_reset：登记自定义清理；登记失败时立即执行该清理。
 * 成功 probe 后的 remove 和 probe 中途失败的回滚，都要覆盖异步资源。
 */

/* ==================== 附录 C：测试入口与教学故障 ====================
 * 01 test_ioctl.c：同一打开实例上的 ioctl/缓冲操作。
 * 02 test_hello.c：platform + misc 的数据/控制接口。
 * 04 test_poll.c / test_input.c：等待事件与读取标准 input_event。
 * 05 test_race.c / test_wait.c / test_lock.c：快照、完成等待、锁顺序。
 * 08 test_mmap.c：映射与 read/write 观察同一份内存。
 * 03/06/09：通过 sysfs 检查电平/亮度、chip_id、loopback。
 * 07/08 UART/10：内部回环、PIO/DMA、空闲后的 runtime PM 行为。
 *
 * hello_bugs.c 是故障练习入口，不是通用内存分配范本。
 * 当前工作区 oob/uaf/stack 函数体为空；leak 函数分配后未释放（故意的），
 * 但未检查分配失败，也还没有 pr_info("%px") 打印地址。
 * 不要把“有命令入口”理解成所有故障都已实现。
 * KASAN 用于检查越界/释放后使用，kmemleak 用于疑似泄漏，lockdep 检查锁。
 * 这些工具要在单独的调试内核上用（make KDIR=$HOME/rk3576-kernel-debug）；
 * 普通内核上越界/UAF 多数没有任何现象，这正是需要 KASAN 的原因。
 * 故障开关一览（都是 insmod 参数，默认关闭）：
 *   04 bug_sleep_in_irq  硬中断里 msleep → scheduling while atomic（案例 10）
 *   05 bug_noirq         read 中不关中断取 IRQ 也用的锁 → lockdep 报告
 *   05 LOCK_AB/LOCK_BA   两个 ioctl 相反顺序取两把 mutex → lockdep 报告
 *   06 wrong_addr        I2C 地址错 → NACK、i2c_transfer 返回负错误码
 *   07 skip_clk          不开时钟访问寄存器 → 读到残留值（案例 17）
 *   08 no_size_check     mmap 不查范围 → 读到别的物理页（案例 18）
 *   08 hello_bugs idx    oob/stack 用的下标，从参数来避免编译期被发现
 * 这些参数用于制造问题，不是新驱动的默认配置。
 */

/* ==================== 附录 D：DEBUG-CASES.md 案例索引 ====================
 * 驱动代码类
 *   1  01 第一版五个 bug：无边界检查、len 未更新、copy_* 返回值、return 0
 *   4  kmalloc 未清零，per-open 的 len 是垃圾值 → 用 kzalloc
 *   5  remove 漏 misc_deregister，节点残留、指向已释放内存
 *   6  size - *ppos 无符号下溢越界写；修复时又引入未初始化变量
 *   7  设备树属性名两边没对上，of_property_read_* 一直失败
 *   10 硬中断里 msleep，读懂 scheduling while atomic
 *   18 mmap 不查范围泄露别人的物理页；移位优先级、页/字节单位混用
 *   20 返回值约定混用：成功也报超时且持锁返回
 *   22 memset 清错对象、长度用错，踩坏内核栈
 * 硬件 / 时钟类
 *   11 杜邦线针脚找错；12 输入脚被接到 3.3V，收不到中断
 *   15 摄像头时好时坏（原因未查明）
 *   16 baudclk 默认 187500 Hz，分频为 0；17 不开时钟读到残留值
 * 环境 / 工具类
 *   2 查错了节点名；3 udhcpc 不配置网卡；8 menuconfig 未带交叉编译器
 *   9 NFS 卡住误判为驱动卡死；13 .gitignore 忽略了内核源文件
 *   14 厂商驱动开机先触发 lockdep，之后 lockdep 已关闭
 *   19 后台进程 stdin 是 /dev/null，实验前提不成立
 *   21 /dev 下残留普通文件挡住设备节点
 * 通用方法：现象 → 排查 → 根因 → 修复 → 验证；
 *   改完确认 .c / .ko / NFS 上 .ko 的时间一致；
 *   找一个“只有功能真正工作才会变化”的独立指标（如 DMA 中断计数）；
 *   卡住先 sysrq w（谁在等什么）+ sysrq l（CPU 在做什么），再下结论。
 */

#endif /* 教学片段结束。 */
