// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 整个驱动是怎么跑起来的 =====================
 *
 *  insmod hello_platform.ko
 *    └─ module_platform_driver() 展开出来的 init 函数
 *         └─ platform_driver_register(&hello_platform_driver)
 *              把"我能驱动 compatible = zhouyllll,hello-platform 的设备"
 *              这件事告诉内核
 *                └─ 内核拿 hello_of_match 表去跟设备树里的节点逐个比对
 *                     └─ 比对上了 hello-platform 节点 → 调用 hello_platform_probe()
 *                          ├─ 读设备树属性（buf-size、label）
 *                          └─ misc_register() → /dev/hello0 出现
 *
 *  用户态 open/read/write/ioctl /dev/hello0
 *    └─ misc 子系统的 misc_open() → 按 hello_fops 表调到 hello_open/hello_read/...
 *
 *  rmmod hello_platform
 *    └─ platform_driver_unregister()
 *         └─ 设备跟驱动解绑 → 调用 hello_platform_remove()
 *              ├─ misc_deregister() → /dev/hello0 消失
 *              └─ 之后 devm 自动释放 probe 里 devm_kzalloc 的那块内存
 *
 *  三个角色：
 *    platform_device：内核根据设备树节点自动创建的"设备"，probe 的参数 pdev 就是它
 *    platform_driver：你写的"驱动"，也就是下面的 hello_platform_driver
 *    misc device    ：驱动在 probe 里注册出来的"用户态入口"，也就是 /dev/hello0
 * =================================================================
 */

/* 让本文件里所有 pr_err/pr_info 打印自动带上 "hello_platform: " 前缀，必须写在所有 #include 之前 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/platform_device.h>	/* platform_device、platform_driver、platform_get/set_drvdata */
#include <linux/of.h>			/* 设备树：device_node、of_property_read_* */
#include <linux/fs.h>			/* file_operations、inode、file */
#include <linux/uaccess.h>		/* copy_to_user/copy_from_user、put_user */
#include <linux/miscdevice.h>		/* miscdevice、misc_register/misc_deregister */
#include <linux/slab.h>			/* kzalloc、kfree */
#include <linux/mutex.h>
#include "hello_ioctl.h"		/* HELLO_CLEAR/HELLO_GET_LEN，跟用户态测试程序共用 */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("Minimal platform driver exercise");

/*
 * 匹配表：告诉内核"这个驱动能驱动哪些设备"。
 * 内核会拿这里的 compatible 字符串，跟设备树里每个节点的 compatible 比，
 * 完全一致（一个字母都不能差）才会调用 probe。
 * 最后一项必须是空的 {}，内核遍历这张表时靠它知道到头了（叫 sentinel，哨兵）。
 */
static const struct of_device_id hello_of_match[] = {
	{ .compatible = "zhouyllll,hello-platform" },
	{ /* sentinel */ },
};

/*
 * 把匹配表导出到模块信息里，给 depmod/modprobe 用：将来可以做到
 * "设备树里有这个节点就自动加载这个模块"。现在手动 insmod，暂时用不上。
 */
MODULE_DEVICE_TABLE(of, hello_of_match);

/*
 * 设备级结构体：每个匹配上的设备节点对应一份，在 probe 里分配。
 * 01 里是一个全局变量 mydev，整个模块只有一份；这里改成每次 probe 分配，
 * 设备树里就算写了两个 hello-platform 节点，也会各自有一份，互不干扰。
 *
 * 注意 miscdev 是直接嵌在结构体里的（不是指针），open 里要靠
 * container_of 从 miscdev 的地址反推出整个 hello_dev，嵌进来才能这么算。
 */
struct hello_dev {
	struct miscdevice miscdev;	/* misc 设备本体，每个设备一份 */
	u32 buf_size;			/* 从设备树读到的缓冲区大小 */
	const char *name;		/* 从设备树读到的 label，也是 /dev/ 下的名字 */
};

/* 每次 open 一份的私有数据，open 里分配、release 里释放 */
struct hello_priv {
	char *buf;			/* 01 里是 char buf[BUF_SIZE]，现在按设备树的大小动态分配 */
	size_t size;			/* buf 的容量，也就是 hdev->buf_size */
	size_t len;			/* 当前写入了多少 */
	struct mutex lock;
};

static int hello_open(struct inode *inode, struct file *filp)
{
	/*
	 * 进到这里时，misc_open() 已经把 private_data 设成了 &hdev->miscdev，
	 * 用 container_of 从成员地址反推出整个 hello_dev
	 */
	struct hello_dev *hdev = container_of(filp->private_data, struct hello_dev, miscdev);
	struct hello_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->buf = kzalloc(hdev->buf_size, GFP_KERNEL);
	if (!priv->buf) {
		kfree(priv);
		return -ENOMEM;
	}
	priv->size = hdev->buf_size;
	mutex_init(&priv->lock);

	/* 从这里开始，private_data 换成本次 open 自己的数据，read/write/ioctl/release 拿到的都是 priv */
	filp->private_data = priv;
	return 0;
}

static int hello_release(struct inode *inode, struct file *filp)
{
	struct hello_priv *priv = filp->private_data;

	mutex_destroy(&priv->lock);
	kfree(priv->buf);	/* 先释放 buf：priv 释放之后就不能再访问 priv->buf 了 */
	kfree(priv);
	return 0;
}

static ssize_t hello_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
	struct hello_priv *priv = filp->private_data;
	size_t bytes_to_read;
	int ret;

	/* 被信号打断时直接返回，内核会决定要不要重启这次系统调用 */
	if (mutex_lock_interruptible(&priv->lock))
		return -ERESTARTSYS;

	/* 已经读到末尾，返回 0 表示 EOF */
	if (*ppos >= priv->len) {
		ret = 0;
		goto out_unlock;
	}

	/* 实际要读的字节数不能超过剩下的有效数据 */
	bytes_to_read = min_t(size_t, count, priv->len - *ppos);

	if (copy_to_user(buf, priv->buf + *ppos, bytes_to_read)) {
		pr_err("read: copy_to_user failed\n");
		ret = -EFAULT;
		goto out_unlock;
	}

	*ppos += bytes_to_read;
	ret = bytes_to_read;
out_unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static ssize_t hello_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
	struct hello_priv *priv = filp->private_data;
	size_t bytes_to_write;
	int ret;

	if (mutex_lock_interruptible(&priv->lock))
		return -ERESTARTSYS;

	/* 先检查再相减：保证下面 priv->size - *ppos 不会下溢成巨大的数 */
	if (*ppos >= priv->size) {
		ret = -ENOSPC;
		goto out_unlock;
	}

	bytes_to_write = min_t(size_t, count, priv->size - *ppos);

	/* copy_from_user 返回的是"没拷成功的字节数"，不是负数，所以判断非 0 */
	if (copy_from_user(priv->buf + *ppos, buf, bytes_to_write)) {
		pr_err("write: copy_from_user failed\n");
		ret = -EFAULT;
		goto out_unlock;
	}

	*ppos += bytes_to_write;
	if (*ppos > priv->len)
		priv->len = *ppos;
	ret = bytes_to_write;

out_unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static long hello_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct hello_priv *priv = filp->private_data;
	int ret = 0;

	switch (cmd) {
	case HELLO_CLEAR:
		if (mutex_lock_interruptible(&priv->lock))
			return -ERESTARTSYS;
		priv->len = 0;
		mutex_unlock(&priv->lock);
		pr_info("buffer cleared\n");
		break;
	case HELLO_GET_LEN:
		if (mutex_lock_interruptible(&priv->lock))
			return -ERESTARTSYS;
		ret = put_user(priv->len, (int __user *)arg);
		mutex_unlock(&priv->lock);
		if (ret)
			pr_err("HELLO_GET_LEN: put_user failed\n");
		break;
	default:
		/* 内核约定：不认识的 ioctl 命令返回 -ENOTTY */
		return -ENOTTY;
	}
	return ret;
}

/*
 * 操作函数表：把用户态的系统调用和上面的函数对应起来。
 * 用户态 open("/dev/hello0") → 内核调 .open 指向的 hello_open，read → hello_read，依此类推。
 * .owner = THIS_MODULE：有人打开着 /dev/hello0 时，模块的引用计数会加一，
 * 这时候 rmmod 会被拒绝，防止模块代码被卸载了还有人在调用它。
 */
static const struct file_operations hello_fops = {
	.owner		= THIS_MODULE,
	.open		= hello_open,
	.release	= hello_release,
	.read		= hello_read,
	.write		= hello_write,
	.unlocked_ioctl	= hello_ioctl,
};

/*
 * probe：设备树节点和驱动匹配上时，内核自动调用。
 * 参数 pdev 是内核根据设备树节点创建好的平台设备，节点的内容就挂在 pdev->dev.of_node 上。
 * 返回 0 表示"这个设备我接管了"；返回负数表示失败，驱动不会绑定到这个设备，
 * 而且【失败时内核不会调用 remove】，所以 probe 里失败前申请的资源要自己负责释放
 * （这里只用了 devm_ 系列，失败时会自动释放，所以可以直接 return）。
 */
static int hello_platform_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct hello_dev *hdev;
	int ret;

	/* ① 给这个设备分配一份结构体 */
	hdev = devm_kzalloc(&pdev->dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev)
		return -ENOMEM;

	/* ② 读设备树，直接存进结构体 */
	if (of_property_read_u32(np, "zhouyllll,buf-size", &hdev->buf_size)) {
		dev_warn(&pdev->dev, "no buf-size, use default\n");
		hdev->buf_size = 1024;
	}
	if (of_property_read_string(np, "label", &hdev->name)) {
		dev_warn(&pdev->dev, "no label, use default\n");
		hdev->name = "hello";
	}

	/* ③ 填 miscdevice */
	hdev->miscdev.minor  = MISC_DYNAMIC_MINOR;
	hdev->miscdev.name   = hdev->name;
	hdev->miscdev.fops   = &hello_fops;
	hdev->miscdev.parent = &pdev->dev;

	/* ④ 把结构体挂到 pdev 上，remove 时要取回来 */
	platform_set_drvdata(pdev, hdev);

	/*
	 * ⑤ 最后才注册，注册成功的那一刻 /dev/hello0 就出现了，用户态马上就能 open，
	 *    所以前面 ①～④ 必须全部准备好。失败时直接返回：前面只用了 devm_kzalloc，
	 *    内存会自动释放，没有别的需要手动撤销的东西。
	 */
	ret = misc_register(&hdev->miscdev);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "registered /dev/%s, buf-size=%u\n", hdev->name, hdev->buf_size);
	return 0;
}

/*
 * remove：设备和驱动解绑时调用（rmmod 就会触发），做的是 probe 的反操作。
 * probe 里注册了 misc 设备，这里就必须注销——漏了的话 misc 子系统里会留下一个
 * 指向已释放内存、已卸载代码的节点，之后访问 /dev/hello0 就是 use-after-free。
 * devm_kzalloc 分配的 hdev 不用手动 kfree，remove 返回后内核会自动释放。
 */
static int hello_platform_remove(struct platform_device *pdev)
{
	/* probe 第 ④ 步存进去的指针，在这里取回来，拿到的是同一个 hdev */
	struct hello_dev *hdev = platform_get_drvdata(pdev);

	misc_deregister(&hdev->miscdev);
	dev_info(&pdev->dev, "unregistered /dev/%s\n", hdev->name);
	return 0;
}

/*
 * 驱动本体：把匹配表、probe、remove 打包在一起交给内核。
 * .driver.name 决定了 /sys/bus/platform/drivers/ 下目录的名字。
 */
static struct platform_driver hello_platform_driver = {
	.probe	= hello_platform_probe,
	.remove	= hello_platform_remove,
	.driver	= {
		.name		= "hello_platform",
		.of_match_table	= hello_of_match,
	},
};

/*
 * 这一个宏等价于手写：
 *   static int __init xxx_init(void) { return platform_driver_register(&hello_platform_driver); }
 *   static void __exit xxx_exit(void) { platform_driver_unregister(&hello_platform_driver); }
 *   module_init(xxx_init);
 *   module_exit(xxx_exit);
 * insmod 时注册驱动（进而触发 probe），rmmod 时注销驱动（进而触发 remove）。
 */
module_platform_driver(hello_platform_driver);
