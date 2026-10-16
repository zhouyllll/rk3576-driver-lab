// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 阶段 8 练习 ③：用 KASAN 抓越界和释放后使用，用 kmemleak 抓内存泄漏 =====================
 *
 * 【这是骨架】每个 bug 只有两三行，标 TODO 的地方由你来写——这次是【故意】写错。
 *
 * 必须在调试内核（LOCALVERSION -kasan，开了 KASAN 和 kmemleak）上运行，编译时 KDIR 指向调试内核：
 *   make KDIR=$HOME/rk3576-kernel-debug
 * 普通内核上这些 bug 大多【不会有任何现象】——这正是需要 KASAN 的原因。
 *
 * 用法：往 /dev/hello_bugs 写一个命令触发一种 bug
 *   echo oob   > /dev/hello_bugs     堆越界写（kmalloc 16 字节，写第 16 个字节）
 *   echo uaf   > /dev/hello_bugs     释放后使用（kfree 之后再读）
 *   echo stack > /dev/hello_bugs     栈越界读（局部数组读过头）
 *   echo leak  > /dev/hello_bugs     内存泄漏（kmalloc 之后把指针丢了）
 *
 * KASAN 的原理（简化）：
 *   每 8 字节内存配 1 字节"影子内存"（shadow），记录这 8 字节里有几个能访问。
 *   编译器在每一次内存读写前插一段检查代码（所以调试内核又大又慢）：查影子，不能访问就报告。
 *   kmalloc 时把分配到的字节标成可访问、两边的"红区"（redzone）标成不可访问；kfree 时把整块标成不可访问，
 *   并且先不真正还给系统（放进隔离区 quarantine），这样释放后再访问也能抓到。
 *
 * kmemleak 的原理（简化）：
 *   记下每一次 kmalloc 分配的块；定期扫描所有内存（全局变量、栈、其他已分配的块），
 *   找"还有没有地方存着指向这个块的指针"。一个都找不到 → 这块再也没人能 kfree 了 → 报泄漏。
 * =============================================================================================
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("Stage 8: deliberate memory bugs for KASAN / kmemleak");

/*
 * 编译器很聪明：如果它在编译时就能看出"这里越界了"，会直接报警告（-Werror 下编译失败），
 * 或者把整段没用的代码优化掉（bug 就不会在运行时出现）。
 * 所以下标和长度都从这个模块参数来，编译器在编译时不知道它的值。默认 16 = 正好越界 1 个字节。
 */
static int idx = 16;
module_param(idx, int, 0644);
MODULE_PARM_DESC(idx, "index used by the oob/stack bugs (16 = one past the end)");

/*
 * TODO 1：堆越界写
 *
 * 1. char *p = kmalloc(16, GFP_KERNEL)；失败直接 return
 * 2. pr_info 打印 "oob: write p[%d]"
 * 3. p[idx] = 'x';          ← 16 字节的块，合法下标是 0~15，idx=16 写到了块外面
 * 4. kfree(p)
 *
 * 预期 KASAN 报告：BUG: KASAN: slab-out-of-bounds in hello_bugs_oob+0x...
 *                 Write of size 1 at addr ... by task sh/...
 *                 后面还会告诉你：这块内存是谁分配的（Allocated by task ... 调用栈）、
 *                 访问的地址在"16 字节的块右边第 0 个字节"
 * 思考：不开 KASAN 的普通内核上，这一行会怎样？（kmalloc(16) 实际从 kmalloc-16 的 slab 里拿，
 *      p[16] 是紧挨着的【下一个对象】的第一个字节——改了别人的数据，当时什么也不会发生）
 */
static void hello_bugs_oob(void)
{
}

/*
 * TODO 2：释放后使用（use-after-free，UAF）
 *
 * 1. char *p = kmalloc(32, GFP_KERNEL)；失败 return
 * 2. strscpy(p, "hello", 32)            【新写法】strscpy：安全版 strcpy，最多拷 size-1 个字符并保证以 '\0' 结尾
 * 3. kfree(p)
 * 4. pr_info("uaf: p[0] = %c\n", READ_ONCE(p[0]));    ← 释放之后还在读
 *    【新写法】READ_ONCE(x)：强制编译器真的去内存读一次 x（不优化掉、不合并）。
 *             这里不用它的话，编译器可能发现"p 刚释放，读它是未定义行为"而做奇怪的优化
 *
 * 预期：BUG: KASAN: use-after-free in hello_bugs_uaf+0x...
 *      还会给出【三个】调用栈：谁访问的、谁分配的（Allocated by）、谁释放的（Freed by）
 */
static void hello_bugs_uaf(void)
{
}

/*
 * TODO 3：栈越界读
 *
 * 1. char buf[16];   memset(buf, 'a', sizeof(buf));
 * 2. pr_info("stack: buf[%d] = %d\n", idx, READ_ONCE(buf[idx]));    ← 读到数组外面
 *
 * 预期：BUG: KASAN: stack-out-of-bounds in hello_bugs_stack+0x...
 *      （需要 CONFIG_KASAN_STACK=y：编译器在局部数组两边也插红区）
 */
static void hello_bugs_stack(void)
{
}

/*
 * TODO 4：内存泄漏
 *
 * 1. char *p = kmalloc(128, GFP_KERNEL)；失败 return
 * 2. snprintf(p, 128, "leaked at %llu", ktime_get_ns())      让这块内存里有点能认出来的内容
 *    （需要 #include <linux/timekeeping.h>）
 * 3. pr_info("leak: %px\n", p)
 * 4. 函数返回——p 是局部变量，返回后就没了，再也没有人知道这块内存在哪，永远没法 kfree
 *
 * kmemleak 不会马上报（它是定期扫描的，而且新分配的块要"放一会儿"才算数）。手动触发扫描：
 *   echo scan > /sys/kernel/debug/kmemleak       （要先 mount debugfs）
 *   cat /sys/kernel/debug/kmemleak
 * 预期：unreferenced object 0x... (size 128):
 *        hex dump 里能看到 "leaked at ..."；backtrace 指向 hello_bugs_leak
 *
 * 思考：如果把 p 存进一个全局变量再返回，kmemleak 还会报吗？
 */
static void hello_bugs_leak(void)
{
}

/* ============================ 命令分发（已写好） ============================ */

static ssize_t hello_bugs_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	char cmd[16] = {};
	size_t n = min_t(size_t, count, sizeof(cmd) - 1);

	if (copy_from_user(cmd, ubuf, n))
		return -EFAULT;
	strim(cmd);			/* 去掉 echo 带的换行 */

	if (!strcmp(cmd, "oob"))
		hello_bugs_oob();
	else if (!strcmp(cmd, "uaf"))
		hello_bugs_uaf();
	else if (!strcmp(cmd, "stack"))
		hello_bugs_stack();
	else if (!strcmp(cmd, "leak"))
		hello_bugs_leak();
	else
		return -EINVAL;

	return count;			/* 整条命令都"写进去了"，不能返回 0（案例 20） */
}

static const struct file_operations hello_bugs_fops = {
	.owner	= THIS_MODULE,
	.write	= hello_bugs_write,
	.llseek	= noop_llseek,
};

static struct miscdevice hello_bugs_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "hello_bugs",
	.fops	= &hello_bugs_fops,
};

module_misc_device(hello_bugs_misc);	/* 【新写法】只注册一个 misc 设备时，代替 module_init/exit 两个函数 */
