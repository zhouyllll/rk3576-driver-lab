/*
 * 字符设备驱动骨架 —— 由你自己实现，这里只搭接口。
 *
 * 目标：一个最经典的入门练习——内核态一块固定大小的缓冲区，
 *   write(fd, buf, len)  把用户态数据拷进这块缓冲区
 *   read(fd, buf, len)   把缓冲区内容拷回用户态
 * 效果：`echo hello > /dev/hello_chrdev` 再 `cat /dev/hello_chrdev` 应该能读回 "hello"。
 *
 * 要做的事（对照上面聊的四个步骤，在 hello_init/hello_exit 里填）：
 *   1. alloc_chrdev_region(&devno, 0, 1, "hello_chrdev") 动态申请设备号
 *   2. cdev_init(&hello_cdev, &fops) 把下面的 fops 跟 cdev 关联
 *      cdev_add(&hello_cdev, devno, 1) 注册进内核
 *   3. class_create(...) + device_create(...) 让 /dev/hello_chrdev 自动出现
 *      （四个参数：class, parent, devno, 设备名，具体签名去查，每个内核版本
 *      略有差异，6.1 这个版本的 class_create 只需要一个 name 参数）
 *   4. hello_exit 里要把这些全部反着销毁一遍：device_destroy -> class_destroy
 *      -> cdev_del -> unregister_chrdev_region，顺序反了会 oops
 *
 * file_operations 的四个回调（在下面填逻辑）：
 *   - hello_open/hello_release：先打印一行 printk 确认被调用到就行，不用做别的
 *   - hello_write：用 copy_from_user() 把用户态 buf 拷进内核态缓冲区，
 *     注意边界检查（缓冲区大小固定，len 超了要截断，不能直接照抄 len 拷）
 *   - hello_read：用 copy_to_user() 把内核态缓冲区拷给用户态，
 *     注意 read 到末尾要返回 0（表示 EOF），不然 cat 会死循环——这是这个练习里
 *     最容易犯的错，标准做法是用 *ppos（文件偏移量）判断读到哪了
 *
 * 测试：
 *   insmod hello_chrdev.ko
 *   dmesg | tail          # 看 init 打印的设备号
 *   ls -l /dev/hello_chrdev
 *   echo -n "hello" > /dev/hello_chrdev
 *   cat /dev/hello_chrdev
 *   rmmod hello_chrdev
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/init.h>

#define DEVICE_NAME "hello_chrdev"
#define BUF_SIZE 256

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("Minimal character device driver exercise");

static char kbuf[BUF_SIZE];
static size_t kbuf_len;

struct hello_dev {
    dev_t            devid;
    int              major;
    int              minor;
    struct cdev      cdev;
    struct class    *class;
    struct device   *device;
};

static struct hello_dev mydev;

static int hello_open(struct inode *inode, struct file *filp)
{
	/* TODO */
	filp->private_data = &mydev;     /* 关键：把设备结构体传给后续 read/write */
	return 0;
}

static int hello_release(struct inode *inode, struct file *filp)
{
	/* TODO */
	//struct hello_dev *dev = (struct hello_dev*)filp->private_data;
	return 0;
}

static ssize_t hello_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
	size_t bytes_to_read;
	
	/* 计算实际要读的字节数，不能超过缓冲区长度 */
	if (*ppos >= kbuf_len) {
		return 0; // 已经读到末尾，返回 EOF
	}

	bytes_to_read = min_t(size_t, count, kbuf_len - *ppos);

	if (copy_to_user(buf, kbuf + *ppos, bytes_to_read)) {
		printk("kernel read failed!\r\n");
		return -EFAULT;
	}

	*ppos += bytes_to_read; // 更新文件偏移量
	if (*ppos > kbuf_len) *ppos = kbuf_len; // 防止越界

	return bytes_to_read;
}

static ssize_t hello_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
	/* TODO */
	int ret;
	size_t space_left = BUF_SIZE - *ppos;
	size_t bytes_to_write = min_t(size_t, count, space_left);
    ret = copy_from_user(kbuf + *ppos, buf, bytes_to_write);
    if(ret < 0) {
        printk("kernel write failed!\r\n");
        return -EFAULT;
    }

	return bytes_to_write;
}

static const struct file_operations hello_fops = {
	.owner   = THIS_MODULE,
	.open    = hello_open,
	.release = hello_release,
	.read    = hello_read,
	.write   = hello_write,
};

static int __init hello_init(void){
	int ret = 0;
	mydev.major = 0;

	/*1.申请设备号 */
	if(mydev.major){
		mydev.devid = MKDEV(mydev.major, 0);
		ret = register_chrdev_region(mydev.devid,1,DEVICE_NAME);
	}else{
		ret = alloc_chrdev_region(&mydev.devid,0,1,DEVICE_NAME);
		mydev.major = MAJOR(mydev.devid);
		mydev.minor = MINOR(mydev.devid);
	}
	if(ret<0) goto fail_devid;

	/*2.注册cdev */
	mydev.cdev.owner = THIS_MODULE;
	cdev_init(&mydev.cdev,&hello_fops);
	ret = cdev_add(&mydev.cdev,mydev.devid,1);
	if(ret<0) goto fail_cdev;

	/*3.创class */
	mydev.class = class_create(THIS_MODULE, DEVICE_NAME);
	if(IS_ERR(mydev.class)) {ret = PTR_ERR(mydev.class); goto fail_class;}

	/*4.创/dev/mydev */
	mydev.device = device_create(mydev.class,NULL,mydev.devid,NULL,DEVICE_NAME);
	if(IS_ERR(mydev.device)) {ret = PTR_ERR(mydev.device); goto fail_device;}

	return 0;
fail_device: class_destroy(mydev.class);
fail_class: cdev_del(&mydev.cdev);
fail_cdev: unregister_chrdev_region(mydev.devid,1);
fail_devid: return ret;

}

static void __exit hello_exit(void)
{
	/* TODO */
	device_destroy(mydev.class, mydev.devid);
	class_destroy(mydev.class);
	cdev_del(&mydev.cdev);
	unregister_chrdev_region(mydev.devid,1);
}

module_init(hello_init);
module_exit(hello_exit);

