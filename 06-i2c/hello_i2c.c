// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 阶段 6：自己写一个 I2C 驱动，读 IMX415 的芯片 ID =====================
 *
 * 【这是骨架】框架和注释都写好了，标 TODO 的地方由你来写。按 TODO 编号顺序写，每写完一部分就 make 一次。
 *
 * 硬件：摄像头模组接在 cam3 口，I2C8 总线，从机地址 0x37。
 *       芯片 ID 寄存器 0x311A（16 位寄存器地址，高字节在前），值应该是 0xE0
 *       （drivers/media/i2c/imx415.c:81-82，阶段 6 第 1 步用 i2ctransfer 验证过）。
 *
 * 设备树：不用改。复用原厂的 imx415_3 节点（compatible = "sony,imx415"，rk3576-kickpi-k7-cam3.dtsi）。
 *   原厂 imx415 驱动是编进内核的，开机就绑定了这个设备，加载前先解绑：
 *     echo 8-0037 > /sys/bus/i2c/drivers/imx415/unbind
 *   解绑后再 insmod，内核发现 8-0037 没有驱动、而你的驱动能匹配，就会调你的 probe。
 *   （阶段 6 第 1 步验证过：解绑后传感器仍然上电，能直接读）
 *
 * 跟 platform 驱动对比（结构几乎一样，只是换了一条总线）：
 *
 *   platform 驱动                         I2C 驱动
 *   struct platform_device *pdev          struct i2c_client *client      ← 设备
 *   struct platform_driver                struct i2c_driver              ← 驱动
 *   probe(pdev)                           probe_new(client)
 *   int remove(pdev)                      void remove(client)            ← 6.1 里 I2C 的 remove 返回 void
 *   module_platform_driver()              module_i2c_driver()
 *   platform_set/get_drvdata()            i2c_set/get_clientdata()
 *   &pdev->dev                            &client->dev
 *
 * 新东西只有一个：怎么在 I2C 总线上读一个寄存器（TODO 1）。
 * =============================================================================================
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/i2c.h>			/* i2c_client、i2c_driver、i2c_msg、i2c_transfer、module_i2c_driver */
#include <linux/of.h>
#include <linux/device.h>		/* DEVICE_ATTR_RO、ATTRIBUTE_GROUPS */
#include <asm/unaligned.h>		/* put_unaligned_be16 */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("Stage 6: I2C driver reading IMX415 chip id");

#define IMX415_REG_CHIP_ID	0x311A
#define IMX415_CHIP_ID		0xE0

/*
 * 调试练习（已写好参数）：insmod hello_i2c.ko wrong_addr=0x38
 * 读寄存器时用这个地址代替 client->addr（0x37），模拟"设备树里地址写错了"。
 * 0x38 上没有设备，从机不应答（NACK），预期 i2c_transfer 返回 -ENXIO。
 */
static ushort wrong_addr;
module_param(wrong_addr, ushort, 0444);
MODULE_PARM_DESC(wrong_addr, "DEBUG EXERCISE: use this 7-bit address instead of the real one");

struct hello_i2c_dev {
	struct i2c_client *client;	/* 内核给的，存指针（C 类：别人的东西） */
	u8 chip_id;			/* probe 时读到的芯片 ID */
};

/* ============================ 读寄存器 ============================ */

/*
 * TODO 1：读一个寄存器（16 位地址，8 位数据）。成功返回 0，失败返回负的错误码。
 *
 * 一次 I2C 读寄存器在总线上是这样的（i2ctransfer 的 "w2@0x37 0x31 0x1a r1" 就是在做这件事）：
 *
 *   START | 地址0x37+写 | 0x31 | 0x1A | RESTART | 地址0x37+读 | [读出1字节] | STOP
 *         └────── 第 1 条消息：写 2 字节 ──────┘          └─ 第 2 条消息：读 1 字节 ─┘
 *
 * 在内核里，就是准备两个 struct i2c_msg，一次 i2c_transfer 发出去：
 *
 *   【新写法】struct i2c_msg {
 *       __u16 addr;    从机地址（7 位）：一般就是 client->addr；调试练习时换成 wrong_addr
 *       __u16 flags;   0 = 写；I2C_M_RD = 读
 *       __u16 len;     这条消息传几个字节
 *       __u8  *buf;    数据放在哪：写的时候从这里取，读的时候存到这里
 *   };
 *
 * 步骤：
 * 1. u8 addr_buf[2]：把 16 位的 reg 拆成两个字节，高字节在前。
 *    【新写法】put_unaligned_be16(reg, addr_buf)：按"大端"（高字节在前）把 16 位数写进 2 字节数组
 *    （手写的话就是 addr_buf[0] = reg >> 8; addr_buf[1] = reg & 0xff;，两种都行）
 * 2. 选地址：wrong_addr 不为 0 就用它，否则用 client->addr
 * 3. 准备 struct i2c_msg msgs[2]：
 *      msgs[0]：写，len 2，buf 指向 addr_buf
 *      msgs[1]：读，flags 是 I2C_M_RD，len 1，buf 指向 val（val 已经是指针了，直接用）
 * 4. 【新写法】ret = i2c_transfer(client->adapter, msgs, 2);
 *      client->adapter：这个设备挂在哪个 I2C 控制器上（I2C8），由它负责在总线上发波形
 *      返回值：成功时是"传完了几条消息"，也就是 2；失败时是负的错误码
 * 5. ret < 0 → 原样返回；ret 不等于 2（只传了一部分）→ 返回 -EIO；否则返回 0
 *
 * 思考：两条消息为什么要放在【一次】i2c_transfer 里发，而不是调两次 i2c_transfer、一次写一次读？
 *      （提示：看上面的波形，两条消息之间是 RESTART，不是 STOP + START。中间如果有 STOP，
 *       别的驱动可能趁机插进来访问总线；有的芯片也要求必须是 RESTART）
 */
/* __maybe_unused：骨架阶段还没人调用它，写完 TODO 2（probe 里调用了它）后删掉 */
static int hello_i2c_read_reg(struct i2c_client *client, u16 reg, u8 *val)
{
	u8 addr_buf[2];
	int ret;
	struct i2c_msg msgs[2];


	put_unaligned_be16(reg, addr_buf);
	if (wrong_addr) {
		msgs[0].addr  = wrong_addr;
		msgs[1].addr  = wrong_addr;
	} else {
		msgs[0].addr  = client->addr;
		msgs[1].addr  = client->addr;
	}
	msgs[0].flags = 0;
	msgs[0].len   = 2;
	msgs[0].buf   = addr_buf;

	msgs[1].flags = I2C_M_RD;
	msgs[1].len   = 1;
	msgs[1].buf   = val;

	ret = i2c_transfer(client->adapter, msgs, 2);
	if (ret != 2) {
		if (ret < 0)
			return ret;
		return -EIO;
	}
	return 0;
}

/* ============================ sysfs：chip_id ============================ */

/*ret = hello_i2c_read_reg(IMX415_REG_CHIP_ID);
 * TODO 3：cat /sys/bus/i2c/devices/8-0037/chip_id 时，【重新】读一次芯片 ID 寄存器，打印 "0xe0"
 *
 * 1. 从 dev 找回 hello_i2c_dev（probe 里用 i2c_set_clientdata 存的；
 *    i2c_set_clientdata 内部就是 dev_set_drvdata(&client->dev, ...)，所以这里用 dev_get_drvdata(dev)）
 * 2. 调 hello_i2c_read_reg 读 IMX415_REG_CHIP_ID，失败就返回错误码（cat 会报错）
 * 3. sysfs_emit(buf, "0x%02x\n", val)
 *
 * 这个属性名叫 chip_id，所以函数名必须叫 chip_id_show（阶段 3 讲过的 DEVICE_ATTR 规则）
 */
static ssize_t chip_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hello_i2c_dev *hdev = dev_get_drvdata(dev);
	int ret;
	u8 id;

	ret = hello_i2c_read_reg(hdev->client, IMX415_REG_CHIP_ID, &id);
	if (ret)
		return ret;
	if (!ret)
		return sysfs_emit(buf, "0x%02x\n", id);

	return 0;
}
static DEVICE_ATTR_RO(chip_id);

static struct attribute *hello_i2c_attrs[] = {
	&dev_attr_chip_id.attr,
	NULL,
};
ATTRIBUTE_GROUPS(hello_i2c);

/* ============================ probe / remove ============================ */

/*
 * TODO 2：probe。参数是 i2c_client，代表"I2C8 上地址 0x37 的那个设备"，内核根据设备树节点创建的。
 *
 * 1. devm_kzalloc 分配 hello_i2c_dev（用 &client->dev）
 * 2. hdev->client = client；i2c_set_clientdata(client, hdev)
 *    （跟 platform_set_drvdata 一样：存起来给 sysfs 的 show 用）
 * 3. 调 hello_i2c_read_reg 读 IMX415_REG_CHIP_ID
 *      失败：dev_err_probe(&client->dev, ret, "failed to read chip id\n")
 *      读到的不是 IMX415_CHIP_ID：dev_err 打印读到的值，返回 -ENODEV（"这不是我要的设备"）
 * 4. 存到 hdev->chip_id，dev_info 打印：总线号（client->adapter->nr）、地址（client->addr）、芯片 ID
 * 5. 返回 0
 *
 * 思考：probe 里为什么要检查芯片 ID？（提示：设备树写了 compatible = "sony,imx415"，
 *       就一定说明板子上真的有一颗 IMX415 吗？阶段 6 第 1 步里，cam0、cam1 两个口是什么情况？）
 */
static int hello_i2c_probe(struct i2c_client *client)
{
	struct hello_i2c_dev *hdev;
	int ret;
	u8 id;

	hdev = devm_kzalloc(&client->dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev)
		return -ENOMEM;

	hdev->client = client;
	i2c_set_clientdata(client, hdev);
	ret = hello_i2c_read_reg(client, IMX415_REG_CHIP_ID, &id);
	if (ret)
		return dev_err_probe(&client->dev, ret, "failed to read chip id\n");
	if (id != IMX415_CHIP_ID) {
		dev_err(&client->dev, "unexpected chip id 0x%02x\n", id);
		return -ENODEV;
	}

	hdev->chip_id = id;
	dev_info(&client->dev, "i2c-%d addr 0x%02x chip id 0x%02x\n", client->adapter->nr, client->addr, hdev->chip_id);

	return 0;
}

/*
 * remove：内存是 devm 的，sysfs 属性是 dev_groups 自动建/删的，这里没有要手动清理的东西。
 * 留着只为打一行日志。注意 6.1 里 I2C 的 remove 返回 void。
 */
static void hello_i2c_remove(struct i2c_client *client)
{
	dev_info(&client->dev, "removed\n");
}

/* 跟原厂驱动用同一个 compatible：原厂驱动解绑后，你的驱动才能绑上 */
static const struct of_device_id hello_i2c_of_match[] = {
	{ .compatible = "sony,imx415" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hello_i2c_of_match);

static struct i2c_driver hello_i2c_driver = {
	.probe_new	= hello_i2c_probe,	/* 6.1：只有 client 一个参数的新版 probe */
	.remove		= hello_i2c_remove,
	.driver		= {
		.name		= "hello_i2c",			/* → /sys/bus/i2c/drivers/hello_i2c */
		.of_match_table	= hello_i2c_of_match,
		.dev_groups	= hello_i2c_groups,		/* → /sys/bus/i2c/devices/8-0037/chip_id */
	},
};
module_i2c_driver(hello_i2c_driver);
