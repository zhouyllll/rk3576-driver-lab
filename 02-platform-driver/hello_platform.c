// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 整个驱动是怎么跑起来的 =====================
 *
 *  insmod hello_platform.ko [buf_size_override=N]
 *    ├─ 内核先把模块参数 buf_size_override 设好（没传就是 0）
 *    └─ module_platform_driver() 展开出来的 init 函数
 *         └─ platform_driver_register(&hello_platform_driver)
 *              把"我能驱动 compatible = zhouyllll,hello-platform 的设备"
 *              这件事告诉内核
 *                └─ 内核拿 hello_of_match 表去跟设备树里的节点逐个比对
 *                     └─ 比对上了 hello-platform 节点 → 调用 hello_platform_probe()
 *                          ├─ 读设备树属性（buf-size、label），模块参数可以覆盖 buf-size
 *                          └─ misc_register() → /dev/hello0 出现，
 *                             同时出现 sysfs 属性 /sys/class/misc/hello0/buf_size
 *
 *  用户态 open/read/write/ioctl /dev/hello0
 *    └─ misc 子系统的 misc_open() → 按 hello_fops 表调到 hello_open/hello_read/...
 *
 *  用户态 cat /sys/class/misc/hello0/buf_size
 *    └─ 内核调用 buf_size_show()
 *
 *  rmmod hello_platform
 *    └─ platform_driver_unregister()
 *         └─ 设备跟驱动解绑 → 调用 hello_platform_remove()
 *              ├─ misc_deregister() → /dev/hello0 和 sysfs 属性一起消失
 *              └─ 之后 devm 自动释放 probe 里 devm_kzalloc 的那块内存
 *
 *  三个角色：
 *    platform_device：内核根据设备树节点自动创建的"设备"，probe 的参数 pdev 就是它
 *    platform_driver：你写的"驱动"，也就是下面的 hello_platform_driver
 *    misc device    ：驱动在 probe 里注册出来的"用户态入口"，也就是 /dev/hello0
 *
 *  驱动给用户态暴露信息的几种方式，这个驱动里都用到了：
 *    read/write      ：传数据
 *    ioctl           ：发控制命令（清空、查长度）
 *    sysfs 属性       ：一个文件一个值，给人和脚本看的"正式接口"（buf_size）
 *    模块参数         ：加载时传配置（buf_size_override）
 *    pr_debug        ：调试打印，平时关闭，用 dynamic debug 按需打开
 *    debugfs         ：调试用的内部状态，没有稳定性承诺（open_count）
 * =================================================================
 */

/* 让本文件里所有 pr_err/pr_info/pr_debug 打印自动带上 "hello_platform: " 前缀，必须写在所有 #include 之前 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>		/* module_param、MODULE_* 宏 */
#include <linux/platform_device.h>	/* platform_device、platform_driver、platform_get/set_drvdata */
#include <linux/of.h>			/* 设备树：device_node、of_property_read_* */
#include <linux/fs.h>			/* file_operations、inode、file */
#include <linux/uaccess.h>		/* copy_to_user/copy_from_user、put_user */
#include <linux/miscdevice.h>		/* miscdevice、misc_register/misc_deregister */
#include <linux/slab.h>			/* kzalloc、kfree */
#include <linux/mutex.h>		/* mutex_init、mutex_lock_interruptible 等 */
#include <linux/debugfs.h>		/* debugfs_create_dir、debugfs_create_atomic_t、debugfs_remove_recursive */
#include <linux/atomic.h>		/* atomic_t、atomic_inc/atomic_dec */
#include "hello_ioctl.h"		/* HELLO_CLEAR/HELLO_GET_LEN，跟用户态测试程序共用 */

/*
 * 模块信息，modinfo hello_platform.ko 能看到。
 * MODULE_LICENSE 不只是说明：内核据此判断这个模块能不能调用那些只导出给 GPL 模块的函数
 * （EXPORT_SYMBOL_GPL），不写或者写成非 GPL，内核会被标记为"被污染"（taint 标志 P），
 * 有些函数也用不了。
 * 你现在加载时看到的 "loading out-of-tree module taints kernel" 是另一回事：只要是
 * 不在内核源码树里编译的模块（out-of-tree）都会打这个标记（taint 标志 O），不影响使用。
 */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("Minimal platform driver exercise");

/*
 * ===================== 补充练习 2（上）：module_param =====================
 *
 * 用途：给驱动传"配置参数"，就像程序的命令行参数。同一个驱动，加载时传不同的值，
 * 行为就不同，不用改代码、不用重新编译。
 *
 * 这里做一个真正的配置项：用模块参数覆盖设备树里的 buf-size。
 *   insmod hello_platform.ko buf_size_override=1024   → 缓冲区变成 1024 字节
 *   insmod hello_platform.ko                          → 不传就是 0，表示"不覆盖"，还用设备树的值
 *
 * module_param(变量名, 类型, 权限) 的三个参数：
 *   - 变量名：buf_size_override，同时也是 insmod 时写的参数名
 *   - 类型  ：uint 对应 unsigned int；常用的还有 bool、int、charp（字符串）
 *   - 权限  ：决定 /sys/module/hello_platform/parameters/buf_size_override 这个文件
 *            能不能读写。这里用 0444（只读），因为这个值只在 probe 时用一次，
 *            运行中改了也不会生效，干脆不让改，免得误导人。写成 0 就不出现在 /sys 里。
 *
 * 驱动编进内核（=y）时没有 insmod，改在 bootargs 里写：hello_platform.buf_size_override=1024
 *
 * 使用它的地方在 probe 第 ② 步后面。
 */
static unsigned int buf_size_override;
module_param(buf_size_override, uint, 0444);
MODULE_PARM_DESC(buf_size_override, "override buf-size from device tree (0 = use device tree)");

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
 * 两个结构体，生命周期不同，这是这个驱动的核心设计：
 *
 *   struct hello_dev  ：每个设备一份。probe 里分配（devm），remove 之后自动释放。
 *                       存"这个设备的配置"：缓冲区多大、叫什么名字、misc 设备本体。
 *   struct hello_priv ：每次 open 一份。open 里分配（kzalloc），release 里释放。
 *                       存"这一次打开的会话数据"：缓冲区、写了多少、锁。
 *
 * 规则：资源跟着谁的生命周期走，就在谁的对称函数里释放。
 * 所以 hello_priv 不能用 devm 分配——devm 要等设备解绑才释放，每 open 一次就泄漏一块。
 */

/*
 * 设备级结构体：每个匹配上的设备节点对应一份，在 probe 里分配。
 * 01 里是一个全局变量 mydev，整个模块只有一份；这里改成每次 probe 分配，
 * 设备树里就算写了两个 hello-platform 节点，也会各自有一份，互不干扰。
 *
 * 注意 miscdev 是直接嵌在结构体里的（不是指针），open 和 sysfs 的 show 函数里都要靠
 * container_of 从 miscdev 的地址反推出整个 hello_dev，嵌进来才能这么算。
 */
struct hello_dev {
	struct miscdevice miscdev;	/* misc 设备本体，每个设备一份 */
	u32 buf_size;			/* 缓冲区大小：设备树的 buf-size，或者模块参数覆盖后的值 */
	const char *name;		/* 从设备树读到的 label，也是 /dev/ 下的名字 */
	/*
	 * 补充练习 3（debugfs）用到的两个成员，已经加好：
	 *
	 * open_count：当前有几个打开着的会话。open 时加一，release 时减一。
	 *   用 atomic_t 而不是普通的 int：多个进程可能同时 open，"读出来、加一、写回去"
	 *   这三步如果被打断，两个进程可能读到同一个旧值，结果只加了一次（这就是"竞态"）。
	 *   atomic_inc 在硬件层面保证这三步一气呵成，不会被打断。这是阶段 5 并发的预习。
	 *
	 * debugfs_dir：probe 里建的 debugfs 目录，remove 时要靠它把整个目录删掉。
	 *   struct dentry 是内核里表示"文件系统里的一个目录项"的结构体。
	 */
	atomic_t open_count;
	struct dentry *debugfs_dir;
};

/* 每次 open 一份的私有数据，open 里分配、release 里释放 */
struct hello_priv {
	char *buf;			/* 01 里是 char buf[BUF_SIZE]，现在按 buf_size 动态分配 */
	size_t size;			/* buf 的容量，也就是 hdev->buf_size */
	size_t len;			/* 当前写入了多少（有效数据的长度） */
	/*
	 * 补充练习 3：指回所属设备的指针，已经加好成员，赋值留给你（见 hello_open 里的 TODO）。
	 *
	 * 为什么需要它：release 的参数里只有 inode 和 filp，filp->private_data 在 open 里
	 * 已经换成了 priv，不再是 miscdev 的地址，没法再用 container_of 找回 hdev。
	 * 但 release 要给 hdev->open_count 减一，所以 open 的时候就把 hdev 存进 priv 里，
	 * release 通过 priv->hdev 拿到它。
	 * "在私有数据里存一个指回上级对象的指针"是驱动里非常常见的写法。
	 */
	struct hello_dev *hdev;
	/*
	 * 保护上面三个成员。同一个 fd 可能被多个线程同时 read/write（比如 fork 之后
	 * 父子进程共用一个 fd），没有锁的话 len 和 buf 可能被改乱。
	 * 用 mutex 而不是 spinlock：copy_to_user/copy_from_user 可能睡眠（缺页时要换页），
	 * 持有 spinlock 时不能睡眠。
	 */
	struct mutex lock;
};

/*
 * open：用户态 open("/dev/hello0") 时调用。
 *
 * 调用链：open() 系统调用 → misc_open()（drivers/char/misc.c）→ hello_open()
 * 进到这里时，misc_open() 已经把 filp->private_data 设成了 &hdev->miscdev。
 *
 * 做的事：分配本次打开专用的 hello_priv，存进 filp->private_data，
 * 之后这个 fd 上的 read/write/ioctl/release 都从 private_data 拿到它。
 *
 * 返回 0 表示打开成功；返回负的错误码，用户态的 open() 就失败，errno 是对应的错误。
 */
static int hello_open(struct inode *inode, struct file *filp)
{
	/* 用 container_of 从 miscdev 的地址反推出整个 hello_dev，拿到 buf_size */
	struct hello_dev *hdev = container_of(filp->private_data, struct hello_dev, miscdev);
	struct hello_priv *priv;

	/* kzalloc 会把内存清零：len 从 0 开始（01 里用 kmalloc 没清零踩过坑） */
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->buf = kzalloc(hdev->buf_size, GFP_KERNEL);
	if (!priv->buf) {
		/* 第二次分配失败，要把第一次分配的 priv 还回去，否则就泄漏了 */
		kfree(priv);
		return -ENOMEM;
	}
	priv->size = hdev->buf_size;
	mutex_init(&priv->lock);

	/*
	 * ===================== 补充练习 2（下）：dynamic debug =====================
	 *
	 * 用途：控制调试打印要不要输出。它不负责"看"日志，看日志还是用 dmesg。
	 *
	 * pr_debug 跟 pr_info 写法一样，区别是：内核会把每一条 pr_debug 都登记下来，
	 * 每条带一个独立的开关，默认全部关闭，所以平时 dmesg 里看不到。
	 * 这个驱动里一共 3 条：这里一条，read、write 里各一条。
	 * 需要时在板子上打开（先确认 debugfs 已挂载：mount -t debugfs none /sys/kernel/debug）：
	 *   echo 'module hello_platform +p' > /sys/kernel/debug/dynamic_debug/control   打开整个模块
	 *   echo 'func hello_open +p'       > /sys/kernel/debug/dynamic_debug/control   只打开这个函数的
	 *   echo 'module hello_platform -p' > /sys/kernel/debug/dynamic_debug/control   全部关闭
	 *   grep hello_platform /sys/kernel/debug/dynamic_debug/control                  查看每条的开关状态
	 *
	 * 这里拿不到 struct device，所以用 pr_debug 而不是 dev_dbg；
	 * 文件开头的 pr_fmt 会自动加上 "hello_platform: " 前缀。
	 * %zu 对应 size_t，类型和格式符对不上，编译会报警告。
	 */
	pr_debug("open: buf_size=%zu\n", priv->size);

	/*
	 * TODO（补充练习 3）：在这里做两件事
	 *   1. 把 hdev 存进 priv->hdev，release 要用
	 *   2. 用 atomic_inc(&hdev->open_count) 给打开计数加一
	 *
	 * 为什么放在这里（函数末尾，而不是开头）：前面的两次 kzalloc 都可能失败并提前 return，
	 * 那种情况下 open 没成功，用户态拿不到 fd，之后也不会调用 release。
	 * 如果在开头就加一，失败时计数就再也减不回来了。所以只在"确定成功"之后才加一。
	 */
	priv->hdev = hdev;
	atomic_inc(&hdev->open_count);
	/* 从这里开始，private_data 换成本次 open 自己的数据，read/write/ioctl/release 拿到的都是 priv */
	filp->private_data = priv;
	return 0;
}

/*
 * release：这个 fd 最后一次被关闭时调用（对应用户态的 close()）。
 *
 * 注意是"最后一次"：fork 或 dup 之后，多个 fd 指向同一个打开的文件，
 * 要全部关掉才会调用 release，不是每次 close() 都调。
 * 进程退出时没关的 fd 也会被内核自动关闭，同样会调到这里，所以不会泄漏。
 *
 * 做的事：释放 open 里分配的东西，顺序跟分配时相反。
 */
static int hello_release(struct inode *inode, struct file *filp)
{
	struct hello_priv *priv = filp->private_data;

	/*
	 * TODO（补充练习 3）：用 atomic_dec 给打开计数减一，跟 open 里的 atomic_inc 成对。
	 *   - 通过 priv->hdev 拿到设备结构体
	 *   - 一定要放在 kfree(priv) 之前：priv 释放之后就不能再访问 priv->hdev 了
	 */
	atomic_dec(&priv->hdev->open_count);
	mutex_destroy(&priv->lock);	/* 跟 mutex_init 成对；开了锁调试选项时能查出"锁还被持有就销毁"的问题 */
	kfree(priv->buf);	/* 先释放 buf：priv 释放之后就不能再访问 priv->buf 了 */
	kfree(priv);
	return 0;
}

/*
 * read：用户态 read(fd, buf, count) 时调用。
 *
 * 参数：
 *   buf   ：用户态的缓冲区地址，带 __user 标注，不能直接解引用，只能用 copy_to_user
 *   count ：用户最多想读多少字节
 *   ppos  ：这个 fd 的当前偏移，读完要自己往后挪
 *
 * 返回值：实际读到的字节数；返回 0 表示 EOF（cat 读到 0 才会停）；出错返回负的错误码。
 */
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

	/* copy_to_user 返回的是"没拷成功的字节数"，非 0 就是用户给的地址有问题 */
	if (copy_to_user(buf, priv->buf + *ppos, bytes_to_read)) {
		pr_err("read: copy_to_user failed\n");
		ret = -EFAULT;
		goto out_unlock;
	}

	/*
	 * 调试打印：读了多少字节、读之前的偏移。放在 *ppos += 前面，打印的才是读之前的偏移。
	 * *ppos 是 loff_t，在不同架构上的底层类型可能不同，强转成 long long 再用 %lld，
	 * 是内核里打印 loff_t 的常见写法。
	 */
	pr_debug("read: bytes=%zu, pos=%lld\n", bytes_to_read, (long long)*ppos);
	*ppos += bytes_to_read;
	ret = bytes_to_read;
out_unlock:
	/* 所有出口都走这里解锁，避免某条错误路径忘了解锁 */
	mutex_unlock(&priv->lock);
	return ret;
}

/*
 * write：用户态 write(fd, buf, count) 时调用。参数含义跟 read 一样，方向相反。
 *
 * 语义跟普通文件一样：从当前偏移开始写，写到后面就把有效长度 len 撑大；
 * 从中间覆盖写不会让 len 变短（清空要用 HELLO_CLEAR）。
 *
 * 返回值：实际写入的字节数（可能比 count 少，缓冲区剩余空间不够时会截断）；
 * 缓冲区已满返回 -ENOSPC。不能返回 0，否则很多程序会一直重试。
 */
static ssize_t hello_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
	struct hello_priv *priv = filp->private_data;
	size_t bytes_to_write;
	int ret;

	if (mutex_lock_interruptible(&priv->lock))
		return -ERESTARTSYS;

	/*
	 * 先检查再相减：保证下面 priv->size - *ppos 不会下溢成巨大的数。
	 * 用户态可以用 pwrite 指定任意偏移，这一步是在防越界写（DEBUG-CASES 案例 6）。
	 */
	if (*ppos >= priv->size) {
		ret = -ENOSPC;
		goto out_unlock;
	}

	/* 按剩余空间截断 */
	bytes_to_write = min_t(size_t, count, priv->size - *ppos);

	/* copy_from_user 返回的是"没拷成功的字节数"，不是负数，所以判断非 0 */
	if (copy_from_user(priv->buf + *ppos, buf, bytes_to_write)) {
		pr_err("write: copy_from_user failed\n");
		ret = -EFAULT;
		goto out_unlock;
	}

	/* 调试打印：写了多少字节、写之前的偏移，写法跟 read 里一样 */
	pr_debug("write: bytes=%zu, pos=%lld\n", bytes_to_write, (long long)*ppos);
	*ppos += bytes_to_write;
	/* 写到了比原来更后面的位置，才更新有效长度 */
	if (*ppos > priv->len)
		priv->len = *ppos;
	ret = bytes_to_write;

out_unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

/*
 * ioctl：用户态 ioctl(fd, cmd, arg) 时调用，用来发"控制命令"，而不是传数据。
 * 命令号的定义和编码方式见 hello_ioctl.h。
 *
 * 参数：
 *   cmd ：命令号，用 hello_ioctl.h 里 _IO/_IOR 宏生成的 HELLO_CLEAR、HELLO_GET_LEN
 *   arg ：命令的参数。HELLO_GET_LEN 时它是用户态 int 变量的地址，所以要强转成 int __user *
 *
 * 返回值：0 表示成功；负的错误码表示失败。
 */
static long hello_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct hello_priv *priv = filp->private_data;
	int ret = 0;

	switch (cmd) {
	case HELLO_CLEAR:
		/* 清空只是把有效长度归零，缓冲区内容不用真的擦掉 */
		if (mutex_lock_interruptible(&priv->lock))
			return -ERESTARTSYS;
		priv->len = 0;
		mutex_unlock(&priv->lock);
		pr_info("buffer cleared\n");
		break;
	case HELLO_GET_LEN:
		if (mutex_lock_interruptible(&priv->lock))
			return -ERESTARTSYS;
		/*
		 * put_user 把一个值写到用户态地址，拷多大由指针类型决定：这里是 int __user *，
		 * 所以拷 4 字节。priv->len 是 size_t（8 字节），会截断成 int；
		 * 缓冲区最大也就几十 KB，截断不会出问题，但要知道有这回事。
		 * 成功返回 0，地址非法返回 -EFAULT。
		 */
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
 * 没填的成员（比如 .llseek、.poll）就是不支持，内核会走默认行为或返回错误。
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
 * ===================== 补充练习 1：sysfs 属性 =====================
 *
 * 效果：cat /sys/class/misc/hello0/buf_size 能读出缓冲区大小。
 *
 * sysfs 的约定：一个文件只放一个值，文本格式，以换行结尾。
 * 这样 cat 能直接看懂，脚本也好解析。
 *
 * 一个 sysfs 属性由下面 4 部分组成（顺序不能乱，后面的要引用前面的）：
 *   ① show 函数      ：用户 cat 这个文件时，内核调用它，由它把值写进 buf
 *   ② DEVICE_ATTR_RO ：把 show 函数包装成一个"属性"
 *   ③ 属性数组        ：一个设备可以有多个属性，放进一个数组
 *   ④ ATTRIBUTE_GROUPS：把数组包装成 misc 设备能接收的 groups
 * 最后在 probe 里把 groups 交给 miscdev（probe 第 ③ 步）。
 */

/*
 * ① show 函数
 *
 * 函数名必须叫"属性名_show"，这里属性名是 buf_size，所以叫 buf_size_show。
 * 这是 DEVICE_ATTR_RO 宏的约定：它会按这个名字去找读函数，名字不对就编译失败。
 *
 * 参数 dev 是 misc 子系统创建出来的那个设备（/sys/class/misc/hello0），
 * 不是 pdev->dev。要从它找回 hdev，分两步：
 *   - misc.c:208 的 device_create_with_groups(..., misc, ...) 把 miscdevice 的指针
 *     存成了这个设备的"驱动私有数据"，所以 dev_get_drvdata(dev) 拿到的是 miscdev 的地址
 *   - 再用 container_of 从 miscdev 反推出整个 hdev，跟 hello_open 里的做法完全一样，
 *     只是入口从 filp->private_data 换成了 dev_get_drvdata(dev)
 */
static ssize_t buf_size_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	/* 设备私有数据里存的是 miscdev 的地址（misc.c:208），再用 container_of 找回 hdev */
	struct miscdevice *misc = dev_get_drvdata(dev);
	struct hello_dev *hdev = container_of(misc, struct hello_dev, miscdev);

	/*
	 * sysfs_emit 把格式化后的字符串写进 buf，返回写了多少字节，正好就是 show 要返回的。
	 * 用它而不是 sprintf：它会检查不超过一页（4KB），是新内核推荐的写法。
	 */
	return sysfs_emit(buf, "%u\n", hdev->buf_size);
}

/*
 * ② 定义属性
 *
 * DEVICE_ATTR_RO(buf_size) 展开后是：
 *   struct device_attribute dev_attr_buf_size = { 名字 "buf_size"，权限 0444，读函数 buf_size_show };
 * - RO 表示只读，权限是 0444（-r--r--r--）；可读写的用 DEVICE_ATTR_RW，还要再写一个 buf_size_store
 * - 生成的变量名固定是 dev_attr_ 加属性名，下面 ③ 要用到
 */
static DEVICE_ATTR_RO(buf_size);

/*
 * ③ 属性数组
 *
 * 数组里放的是每个属性的 .attr 成员的地址；最后必须以 NULL 结尾，
 * 跟 of_device_id 表的 sentinel 是同一个道理。以后加新属性，就往 NULL 前面加一行。
 */
static struct attribute *hello_attrs[] = {
	&dev_attr_buf_size.attr,
	NULL,
};

/*
 * ④ 生成 groups
 *
 * ATTRIBUTE_GROUPS(hello) 会根据 hello_attrs 生成一个叫 hello_groups 的数组，
 * 名字规则是：宏参数加 _attrs 找数组，生成宏参数加 _groups。
 * （定义在 include/linux/sysfs.h:159）
 */
ATTRIBUTE_GROUPS(hello);

/*
 * probe：设备树节点和驱动匹配上时，内核自动调用。
 * 参数 pdev 是内核根据设备树节点创建好的平台设备，节点的内容就挂在 pdev->dev.of_node 上。
 * 返回 0 表示"这个设备我接管了"；返回负数表示失败，驱动不会绑定到这个设备，
 * 而且【失败时内核不会调用 remove】，所以 probe 里失败前申请的资源要自己负责释放
 * （这里只用了 devm_ 系列，失败时会自动释放，所以可以直接 return）。
 */
static int hello_platform_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;	/* 设备树里 hello-platform 这个节点 */
	struct hello_dev *hdev;
	int ret;

	/*
	 * ① 给这个设备分配一份结构体。
	 * devm_kzalloc 跟 kzalloc 一样会清零，区别是这块内存"挂"在 pdev->dev 上，
	 * 设备解绑时（remove 之后，或者 probe 失败时）内核自动释放，不用自己 kfree。
	 */
	hdev = devm_kzalloc(&pdev->dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev)
		return -ENOMEM;

	/*
	 * ② 读设备树，直接存进结构体。
	 * 返回 0 表示读到了；非 0 表示属性不存在或者名字写错了，这时给默认值并打印出来，
	 * 这样属性名写错时能在 dmesg 里看到（DEBUG-CASES 案例 7 就是靠这条打印发现的）。
	 * buf-size 是自定义属性，要带厂商前缀；label 是设备树规范里的通用属性，不带前缀。
	 */
	if (of_property_read_u32(np, "zhouyllll,buf-size", &hdev->buf_size)) {
		dev_warn(&pdev->dev, "no buf-size, use default\n");
		hdev->buf_size = 1024;
	}
	if (of_property_read_string(np, "label", &hdev->name)) {
		dev_warn(&pdev->dev, "no label, use default\n");
		hdev->name = "hello";
	}

	/*
	 * 模块参数覆盖 buf-size（补充练习 2 上）。
	 * 模块参数是用户随便传的，属于"不可信的输入"：传一个 4000000000，kzalloc 会分配失败，
	 * 甚至占用大量内存。所以限制在 65536 以内，超出范围就打印警告并忽略，继续用设备树的值。
	 * 两种情况都打印出来，看 dmesg 就知道最终的值是从哪来的。
	 */
	if (buf_size_override != 0 && buf_size_override <= 65536) {
		hdev->buf_size = buf_size_override;
		dev_info(&pdev->dev, "buf-size overridden by module parameter: %u\n", hdev->buf_size);
	} else if (buf_size_override != 0) {
		dev_warn(&pdev->dev, "invalid buf-size override, ignoring\n");
	}

	/* ③ 填 miscdevice */
	hdev->miscdev.minor  = MISC_DYNAMIC_MINOR;	/* 让内核自动分配次设备号；主设备号固定是 10 */
	hdev->miscdev.name   = hdev->name;		/* 决定 /dev/ 下节点的名字 */
	hdev->miscdev.fops   = &hello_fops;		/* 用户态访问 /dev/hello0 时调哪些函数 */
	hdev->miscdev.parent = &pdev->dev;		/* 父设备：/sys/class/misc/hello0/device 链接就指向它 */
	/*
	 * 补充练习 1：把 sysfs 属性组交给 miscdev。
	 *
	 * 为什么在这里设，而不是 misc_register 之后再用 device_create_file 添加：
	 * misc_register 创建设备时会把 groups 一起创建出来（misc.c:208），设备和属性文件
	 * 同时出现。如果注册完再单独添加，设备会先出现、属性后出现，中间有一段时间窗口，
	 * 用户态程序（比如 udev）可能在属性文件出现之前就去读，结果读不到。
	 */
	hdev->miscdev.groups = hello_groups;

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

	/*
	 * ===================== 补充练习 3：debugfs =====================
	 *
	 * 目标：cat /sys/kernel/debug/hello0/open_count 能看到 /dev/hello0 当前被打开了几次。
	 *
	 * debugfs 和 sysfs 的区别：sysfs 是"正式接口"，用户态程序可能依赖它，格式不能随便改；
	 * debugfs 是给开发者调试用的，没有稳定性承诺，驱动的内部状态（计数器、寄存器值、
	 * 状态机当前状态）随便往里放。比如 /sys/kernel/debug/gpio、/sys/kernel/debug/clk/。
	 *
	 * TODO（补充练习 3）：在这里做两件事
	 *
	 * 1. 建目录：hdev->debugfs_dir = debugfs_create_dir(目录名, 父目录);
	 *    - 目录名用 hdev->name（也就是 "hello0"）。用设备名而不是模块名：设备树里写两个节点
	 *      就有 hello0、hello1 两个设备，用模块名的话两个设备会抢同一个目录，第二个建不出来
	 *    - 父目录传 NULL，表示直接建在 debugfs 的根目录（/sys/kernel/debug/）下
	 *
	 * 2. 建文件：debugfs_create_atomic_t(文件名, 权限, 所在目录, 变量的地址);
	 *    - 文件名 "open_count"，权限 0444（只读）
	 *    - 所在目录就是上一步的 hdev->debugfs_dir
	 *    - 变量的地址：&hdev->open_count
	 *    这个函数会把一个 atomic_t 变量直接变成文件，cat 时自动读出它的当前值，
	 *    不用自己写 show 函数（对比练习 1 的 sysfs，那边要自己写 buf_size_show）。
	 *
	 * 注意两点：
	 * - 不用检查这两个函数的返回值。这是内核对 debugfs 的明确规定：debugfs 只是调试辅助，
	 *   建失败了驱动照样要正常工作；而且这些函数内部已经处理好了"上一步失败"的情况。
	 *   检查 debugfs 返回值的代码，提交到上游反而会被要求删掉。
	 * - 为什么放在 misc_register 成功之后：debugfs 目录不是 devm 管理的，不会自动删除。
	 *   如果放在 misc_register 之前，misc_register 失败 return 时，就得先手动删掉目录，
	 *   否则留下一个没人管的目录。放在成功之后，失败路径就不用多写清理代码了。
	 */
	hdev->debugfs_dir = debugfs_create_dir(hdev->name, NULL);
	debugfs_create_atomic_t("open_count", 0444, hdev->debugfs_dir, &hdev->open_count);

	dev_info(&pdev->dev, "registered /dev/%s, buf-size=%u\n", hdev->name, hdev->buf_size);
	return 0;
}

/*
 * remove：设备和驱动解绑时调用（rmmod 就会触发），做的是 probe 的反操作。
 * probe 里注册了 misc 设备，这里就必须注销——漏了的话 misc 子系统里会留下一个
 * 指向已释放内存、已卸载代码的节点，之后访问 /dev/hello0 就是 use-after-free
 * （DEBUG-CASES 案例 5）。
 * devm_kzalloc 分配的 hdev 不用手动 kfree，remove 返回后内核会自动释放。
 *
 * 6.1 内核的 .remove 返回 int，但返回值会被忽略；更新的内核改成了返回 void 的 .remove_new。
 */
static int hello_platform_remove(struct platform_device *pdev)
{
	/* probe 第 ④ 步存进去的指针，在这里取回来，拿到的是同一个 hdev */
	struct hello_dev *hdev = platform_get_drvdata(pdev);

	/*
	 * TODO（补充练习 3）：用 debugfs_remove_recursive(hdev->debugfs_dir) 删掉整个 debugfs 目录。
	 *   - recursive 表示连同目录下的文件一起删，不用一个个删
	 *   - 放在 misc_deregister 之前：probe 里是先 misc_register、后建 debugfs，
	 *     remove 就按相反的顺序，先删 debugfs、后注销 misc。
	 *     "按创建的相反顺序销毁"是 remove、exit、错误处理路径的通用规则
	 *   - 这一步漏掉的话：rmmod 之后 /sys/kernel/debug/hello0/ 还在，
	 *     里面的 open_count 指向已经被 devm 释放的 hdev，cat 它就是 use-after-free，
	 *     跟案例 5 漏了 misc_deregister 是同一类问题
	 */
	debugfs_remove_recursive(hdev->debugfs_dir);
	/* 注销 misc 设备，/dev/hello0 就消失了 */
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
 * （定义在 include/linux/platform_device.h:274，底层是 include/linux/device/driver.h:262 的 module_driver）
 */
module_platform_driver(hello_platform_driver);
