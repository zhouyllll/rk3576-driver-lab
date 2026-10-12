// SPDX-License-Identifier: GPL-2.0
/*
 * ===================== 阶段 8 练习 ①：把一块内核缓冲区 mmap 给用户态 =====================
 *
 * 【这是骨架】框架和注释都写好了，标 TODO 的地方由你来写。按 TODO 编号顺序写，每写完一部分就 make 一次。
 *
 * 不涉及硬件，也不用设备树：模块加载时自己注册一个 misc 设备 /dev/hello_mmap。
 *
 * 要做的事：内核分配一块缓冲区（默认 2 页 = 8KB），用户态有两种方式访问它：
 *
 *   方式 A：read / write（以前一直用的）
 *     用户 buf ──copy_from_user──→ 内核缓冲区 ──copy_to_user──→ 用户 buf
 *     每次都要【拷贝】一遍数据
 *
 *   方式 B：mmap（这次的重点）
 *     把内核缓冲区所在的物理页，直接映射进用户进程的地址空间。
 *     用户拿到一个指针 p，p[0] = 'A' 写的就是内核缓冲区本身，【不拷贝】。
 *
 *   同一块物理内存，内核用一个虚拟地址访问，用户进程用另一个虚拟地址访问：
 *
 *       内核虚拟地址 hbuf ──┐
 *                           ├──→ 同一组物理页
 *       用户虚拟地址 p    ──┘
 *
 * 验证方法（test_mmap.c）：用户通过 mmap 写，再用 read 读出来——看到的就是同一份数据；反过来也一样。
 *
 * 为什么需要 mmap：摄像头一帧 1920x1080 的图像约 3MB，每秒 30 帧，如果每帧都 copy_to_user，
 * 光拷贝就要每秒 90MB。V4L2 的做法就是把帧缓冲 mmap 给用户态（摄像头专精阶段的 videobuf2）。
 * =============================================================================================
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>			/* remap_pfn_range、vm_area_struct、get_order */
#include <linux/gfp.h>			/* __get_free_pages、free_pages */
#include <linux/io.h>			/* virt_to_phys */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("Stage 8: mmap a kernel buffer to user space");

static uint buf_pages = 2;
module_param(buf_pages, uint, 0444);
MODULE_PARM_DESC(buf_pages, "buffer size in pages (rounded up to a power of 2)");

/*
 * 调试练习（已写好参数）：insmod hello_mmap.ko no_size_check=1
 * mmap 时跳过"申请的大小是否超过缓冲区"的检查。
 * 用户 mmap 比缓冲区大的区域，会把缓冲区后面【别人的】物理页也映射给用户——内核内存泄露给用户态，严重的安全漏洞。
 */
static bool no_size_check;
module_param(no_size_check, bool, 0444);
MODULE_PARM_DESC(no_size_check, "DEBUG EXERCISE: skip the mmap size check (security bug!)");

/*
 * 这个练习只有一个设备，状态直接放全局变量，不用私有结构体。
 * 骨架里的 __maybe_unused：还没人用时防止 -Werror 报"未使用"，全部写完后删掉。
 */
static unsigned long __maybe_unused hbuf;	/* 缓冲区的内核虚拟地址（__get_free_pages 返回的是 unsigned long，不是指针） */
static unsigned int __maybe_unused hbuf_order;	/* 分配了 2^order 页 */
static size_t __maybe_unused hbuf_size;	/* 字节数 = PAGE_SIZE << order */

/* ============================ read / write ============================ */

/*
 * TODO 2：read 和 write，让用户也能用"拷贝"的方式访问缓冲区，用来和 mmap 对照。
 *
 * 【新写法】这两个函数内核帮你写好了偏移、越界、拷贝的处理：
 *
 *   simple_read_from_buffer(ubuf, count, ppos, 内核缓冲区, 缓冲区大小)
 *     从内核缓冲区的 *ppos 处，最多拷 count 个字节给用户，自动推进 *ppos，读到末尾返回 0。
 *     返回拷了多少字节，失败返回 -EFAULT。
 *
 *   simple_write_to_buffer(内核缓冲区, 缓冲区大小, ppos, ubuf, count)
 *     注意参数顺序跟 read 的不一样：【目的地在前】（跟 memcpy、copy_from_user 一样）
 *
 *   hbuf 是 unsigned long，当缓冲区用要转成指针：(void *)hbuf
 *
 * 每个函数一行就够了：return simple_xxx(...);
 *
 * 以前你自己写 read 时要处理 count 和缓冲区大小取最小值、copy_to_user、返回字节数——这两个函数就是把这些包起来了。
 * 有了 ppos，cat 读到末尾会结束；也能用 pread/pwrite 在指定偏移读写（test_mmap.c 会用到）。
 */
static ssize_t hello_mmap_read(struct file *file, char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	return -EINVAL;	/* 占位，写完记得改 */
}

static ssize_t hello_mmap_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	return -EINVAL;	/* 占位，写完记得改 */
}

/* ============================ mmap ============================ */

/*
 * TODO 3：mmap。用户调用 mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset) 时，内核走到这里。
 *
 * 【新结构体】struct vm_area_struct *vma：内核已经在用户进程的地址空间里【划好了一段地址】，用 vma 描述它：
 *     vma->vm_start       这段用户地址从哪开始（就是将来 mmap 返回给用户的那个指针）
 *     vma->vm_end         到哪结束（不包含）
 *     vma->vm_pgoff       用户传的 offset，以【页】为单位（offset / PAGE_SIZE）
 *     vma->vm_page_prot   页的访问权限（可读、可写……），原样传给 remap_pfn_range 就行
 *   但这段地址现在还是"空的"，没有对应任何物理页——你的任务就是把缓冲区的物理页填进去。
 *
 * 步骤：
 * 1. size = vma->vm_end - vma->vm_start（用户要映射多少字节，内核已经按页对齐过了）
 * 2. 检查范围（no_size_check 为真时跳过这一步——调试练习）：
 *      用户要的是缓冲区里 [vm_pgoff 页起, 再往后 size 字节] 这一段，
 *      (vm_pgoff << PAGE_SHIFT) + size 超过 hbuf_size 就返回 -EINVAL
 *      （用 vm_pgoff 之前先确认它本身没有大于缓冲区的页数，防止左移溢出）
 * 3. 算物理页帧号（PFN，page frame number = 物理地址 >> PAGE_SHIFT）：
 *      【新写法】virt_to_phys(内核虚拟地址)：只对 kmalloc / __get_free_pages 这类"线性映射区"的地址有效，
 *               vmalloc 和 ioremap 的地址不能用它
 *      pfn = (virt_to_phys((void *)hbuf) >> PAGE_SHIFT) + vma->vm_pgoff
 * 4. 【新写法】ret = remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
 *      在用户进程的页表里，把 [vm_start, vm_start + size) 这段地址，指向从 pfn 开始的物理页
 *      成功返回 0
 * 5. pr_info 打印：用户地址 vm_start（%lx）、大小、物理地址（pfn << PAGE_SHIFT，%llx 配合 (u64) 强制转换）
 * 6. 返回 ret
 *
 * 思考：
 *   ① 为什么 mmap 只能以"页"为单位？（提示：页表的一项管多大一块内存？）
 *   ② 缓冲区为什么用 __get_free_pages 分配，而不是 vmalloc？（提示：remap_pfn_range 要求从 pfn 开始的物理页是【连续】的；
 *      vmalloc 分配的内存，虚拟地址连续，物理页呢？）
 */
static int hello_mmap_mmap(struct file *file, struct vm_area_struct *vma)
{
	return -EINVAL;	/* 占位，写完记得改 */
}

static const struct file_operations hello_mmap_fops = {
	.owner	= THIS_MODULE,
	.read	= hello_mmap_read,
	.write	= hello_mmap_write,
	.mmap	= hello_mmap_mmap,
	.llseek	= default_llseek,	/* 允许 lseek/pread/pwrite 指定偏移 */
};

static struct miscdevice __maybe_unused hello_mmap_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "hello_mmap",
	.fops	= &hello_mmap_fops,
};

/* ============================ 模块加载 / 卸载 ============================ */

/*
 * TODO 1：模块加载
 *
 * 1. 【新写法】hbuf_order = get_order(buf_pages * PAGE_SIZE);
 *      get_order(字节数)：至少需要 2 的几次方页。比如 2 页 → order 1，3 页 → order 2（4 页）
 *    hbuf_size = PAGE_SIZE << hbuf_order;
 * 2. 【新写法】hbuf = __get_free_pages(GFP_KERNEL | __GFP_ZERO, hbuf_order);
 *      一次分配 2^order 个【物理连续】的页，返回内核虚拟地址（unsigned long）；失败返回 0
 *      __GFP_ZERO：分配后清零（跟 kzalloc 的 z 一样）
 *    失败 → return -ENOMEM
 * 3. 往缓冲区开头写一句话，方便用户 mmap 后马上看到内容：
 *      snprintf((char *)hbuf, hbuf_size, "hello from kernel\n");
 * 4. misc_register(&hello_mmap_misc)；失败要先 free_pages 再返回错误码（没有 devm 了！）
 * 5. pr_info 打印：几页、内核虚拟地址（%px，配合 (void *)hbuf）、物理地址（virt_to_phys，%llx）
 * 6. return 0
 *
 * 注意：这次没有 probe、没有 pdev，所以没有 devm——申请的东西要自己在 exit 和出错路径里释放。
 */
static int __init hello_mmap_init(void)
{
	return -ENODEV;	/* 占位，写完记得改 */
}

/*
 * TODO 4：模块卸载：先 misc_deregister，再 free_pages(hbuf, hbuf_order)
 *
 * 思考：如果用户进程 mmap 了缓冲区、还没 munmap，这时 rmmod 会怎样？
 *      （提示：mmap 会让 vma 持有对 file 的引用，file 又通过 fops->owner 持有模块引用。
 *       先猜，再在板子上试：test_mmap 停在"按回车退出"时，另一个终端 rmmod）
 */
static void __exit hello_mmap_exit(void)
{
}

module_init(hello_mmap_init);
module_exit(hello_mmap_exit);
