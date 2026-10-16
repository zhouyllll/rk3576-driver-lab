// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 阶段 9：自己写一个 spi_driver，替换 spidev，做 MOSI-MISO 回环 =====================
 *
 * 【这是骨架】框架和注释都写好了，标 TODO 的地方由你来写。
 *
 * 硬件：40pin 上的 SPI4（M2 引脚组），厂商在 rk3576-kickpi-k7-extend-40pin.dtsi:67 启用：
 *   CLK  = GPIO4_B0      MOSI = GPIO4_B1      MISO = GPIO4_B2      CS0 = GPIO1_D0（软件片选 cs-gpios）
 *   子节点 spidev@0（compatible = "rockchip,spidev"）→ 内核创建 spi_device "spi4.0"，被 spidev 驱动绑定，
 *   用户态通过 /dev/spidev4.0 访问。
 *   用杜邦线把 MOSI 和 MISO 短接：自己发出去的每一位，同一个时钟周期就被自己收回来。
 *
 * 设备树：不用改，也不用刷机。用 driver_override 把 spi4.0 从 spidev 手里"抢"过来：
 *   echo spi4.0     > /sys/bus/spi/drivers/spidev/unbind          先从 spidev 上解绑
 *   echo hello_spi  > /sys/bus/spi/devices/spi4.0/driver_override  【新写法】指定"这个设备只给叫 hello_spi 的驱动"
 *   insmod hello_spi.ko                                              加载时匹配上 → probe
 *   （还回去：rmmod hello_spi; echo > .../driver_override; echo spi4.0 > /sys/bus/spi/drivers/spidev/bind）
 *   driver_override 优先于 of_match_table：设了它，就只按驱动名字匹配，所以这个驱动连 compatible 都不需要。
 *
 * 跟 I2C 对比：
 *
 *   I2C（阶段 6）                          SPI（这次）
 *   struct i2c_client *client              struct spi_device *spi          ← 设备
 *   struct i2c_driver                      struct spi_driver               ← 驱动
 *   module_i2c_driver                      module_spi_driver
 *   struct i2c_msg + i2c_transfer          struct spi_transfer + spi_sync_transfer
 *   半双工：先写地址，再读（两条消息）      【全双工】：每个时钟周期同时发一位、收一位（一个 transfer 里同时有 tx_buf 和 rx_buf）
 *   有地址、有 ACK（没人应答会 -ENXIO）    没有地址（靠片选 CS 选设备）、没有 ACK——没接设备也"传输成功"，读到的是线上的电平
 *
 * 最后一行是 SPI 调试时最重要的一点：SPI 传输永远"成功"，数据对不对只能自己检查。
 * =============================================================================================
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/spi/spi.h>		/* spi_device、spi_driver、spi_transfer、spi_sync_transfer */
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/string.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("Stage 9: SPI loopback driver replacing spidev");

static uint speed_hz = 1000000;
module_param(speed_hz, uint, 0444);
MODULE_PARM_DESC(speed_hz, "SPI clock in Hz");

/*
 * 一次传输的字节数。spi-rockchip 的规则（drivers/spi/spi-rockchip.c:889 rockchip_spi_can_dma）：
 * 长度 >= FIFO 深度时用 DMA（走 dmac2，阶段 8 学的 PL330），否则 CPU 直接读写 FIFO。
 * 默认 16 → 不走 DMA；insmod 时 xfer_len=256 → 走 DMA，可以看 /proc/interrupts 里 dmac2 的计数。
 */
static uint xfer_len = 16;
module_param(xfer_len, uint, 0444);
MODULE_PARM_DESC(xfer_len, "bytes per loopback transfer (>= FIFO depth uses DMA)");

#define HELLO_SPI_MAX_LEN	4096

struct hello_spi_dev {
	struct spi_device *spi;		/* 内核给的（C 类：别人的东西） */
	u8 *tx;				/* 发送缓冲：必须是 kmalloc 来的（见 TODO 1 第 3 步） */
	u8 *rx;				/* 接收缓冲 */
	u8 seq;				/* 每次测试换一个起始值，避免"上次的数据碰巧对上" */
	struct mutex lock;		/* tx/rx 缓冲只有一份，两个进程同时 cat 要串行（阶段 8 ⑤ 的道理） */
};

/* ============================ sysfs：loopback ============================ */

/*
 * TODO 2：cat /sys/bus/spi/devices/spi4.0/loopback 时，做一次全双工传输，检查收到的和发出的是否一致。
 *
 * 1. hdev = dev_get_drvdata(dev)；mutex_lock
 * 2. 填 tx：tx[i] = hdev->seq + i（i 从 0 到 xfer_len-1）；hdev->seq++；memset(rx, 0, xfer_len)
 * 3. 准备一个 transfer：
 *      【新结构体】struct spi_transfer t = {
 *          .tx_buf = hdev->tx,        这一次要发的数据（MOSI）
 *          .rx_buf = hdev->rx,        同时收到的数据存这里（MISO）
 *          .len    = xfer_len,        字节数（tx 和 rx 一样长——全双工，发几个就同时收几个）
 *      };
 *    【新写法】ret = spi_sync_transfer(hdev->spi, &t, 1);
 *      同步传输（会睡眠，等传完才返回）：拉低片选 → 时钟走 len×8 个周期 → 拉高片选。成功返回 0
 *      失败 → 解锁，返回 ret
 * 4. ok = !memcmp(tx, rx, xfer_len)
 * 5. 解锁之前把结果写进 buf（sysfs_emit 可以多次调用，用 sysfs_emit_at 接着往后写）：
 *      【新写法】格式符 %*ph：把一段内存按十六进制打印，"*" 对应的参数是长度（最多 64 字节）
 *      len = sysfs_emit(buf, "tx: %*ph\n", (int)min_t(uint, xfer_len, 16), hdev->tx);
 *      len += sysfs_emit_at(buf, len, "rx: %*ph\n", (int)min_t(uint, xfer_len, 16), hdev->rx);
 *      len += sysfs_emit_at(buf, len, "%s (%u bytes @ %u Hz)\n", ok ? "OK" : "MISMATCH", xfer_len, speed_hz);
 *      （只打印前 16 字节，比较的是全部）
 * 6. mutex_unlock，返回 len
 *
 * 思考：
 *   ① 不接杜邦线（MOSI、MISO 都空着）时，rx 会是什么？spi_sync_transfer 会返回错误吗？
 *   ② 为什么 tx/rx 不能是这个函数里的局部数组 u8 tx[16]？（提示：xfer_len 大时 spi-rockchip 用 DMA；
 *      阶段 8 说 DMA 地址要通过 DMA API 拿——栈上的内存能安全地交给 DMA 吗？
 *      VMAP_STACK=y 时内核栈在 vmalloc 区，物理上不连续；而且和别的局部变量挤在同一个 cache 行里）
 */
static ssize_t loopback_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return -EINVAL;	/* 占位，写完记得改 */
}
static DEVICE_ATTR_RO(loopback);

static struct attribute *hello_spi_attrs[] = {
	&dev_attr_loopback.attr,
	NULL,
};
ATTRIBUTE_GROUPS(hello_spi);

/* ============================ probe / remove ============================ */

/*
 * TODO 1：probe
 *
 * 1. devm_kzalloc 分配 hdev；hdev->spi = spi；mutex_init；spi_set_drvdata(spi, hdev)
 *    （【新写法】spi_set_drvdata：跟 platform_set_drvdata、i2c_set_clientdata 一样，内部是 dev_set_drvdata）
 * 2. xfer_len 为 0 或大于 HELLO_SPI_MAX_LEN → 返回 -EINVAL
 * 3. 分配 tx、rx 两个缓冲，各 xfer_len 字节：devm_kzalloc(&spi->dev, xfer_len, GFP_KERNEL)，失败 -ENOMEM
 *    kmalloc 来的内存在线性映射区、物理连续、按 cache 行对齐，可以安全地交给 DMA（SPI 框架内部会做流式映射）
 * 4. 配置 SPI 模式：
 *      spi->mode = SPI_MODE_0;          时钟空闲低电平、上升沿采样（CPOL=0, CPHA=0），最常见的模式
 *      spi->bits_per_word = 8;
 *      spi->max_speed_hz = speed_hz;
 *      【新写法】ret = spi_setup(spi);   把上面的设置交给 SPI 控制器驱动检查并生效；失败用 dev_err_probe 返回
 * 5. dev_info 打印：总线号 spi->controller->bus_num、片选 spi_get_chipselect(spi, 0)（这棵 6.1 内核里已经有这个函数，内部就是 spi->chip_select）、
 *    模式、速度、xfer_len
 * 6. 返回 0
 *
 * 不需要 misc、不需要 /dev：测试入口是 sysfs 的 loopback 文件，由 .dev_groups 自动创建。
 */
static int hello_spi_probe(struct spi_device *spi)
{
	return -ENODEV;	/* 占位，写完记得改 */
}

/* 6.1 里 spi_driver 的 remove 返回 void；内存都是 devm 的，sysfs 由 dev_groups 自动删 */
static void hello_spi_remove(struct spi_device *spi)
{
	dev_info(&spi->dev, "removed\n");
}

static struct spi_driver hello_spi_driver = {
	.driver = {
		.name		= "hello_spi",		/* driver_override 按这个名字匹配 */
		.dev_groups	= hello_spi_groups,	/* → /sys/bus/spi/devices/spi4.0/loopback */
	},
	.probe	= hello_spi_probe,
	.remove	= hello_spi_remove,
};
module_spi_driver(hello_spi_driver);
