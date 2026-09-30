/* SPDX-License-Identifier: GPL-2.0 */
/*
 * /dev/hello0 的 ioctl 命令号定义，驱动和用户态测试程序共用
 *
 * ===================== ioctl 是干什么的 =====================
 *
 * read/write 只能"传数据"。但设备经常还需要"控制命令"，比如：清空缓冲区、
 * 查询当前长度、设置波特率、打开摄像头的某个模式……这些不是数据流，
 * 硬塞进 read/write 会很别扭。ioctl 就是专门给设备发控制命令的系统调用：
 *
 *   用户态：ioctl(fd, 命令号, 参数);
 *   内核态：驱动的 hello_ioctl(filp, cmd, arg) 被调用，按 cmd 分情况处理
 *
 * 命令号就是一个整数，驱动和用户态程序必须对"哪个数字代表哪个命令"达成一致，
 * 所以这个头文件同时被两边 #include：
 *   - 驱动 hello_platform.c 用它判断 case HELLO_CLEAR / case HELLO_GET_LEN
 *   - 测试程序 test_hello.c 用它发 ioctl(fd, HELLO_CLEAR)
 * 只写一份，两边共用，就不会出现两边各抄一遍、数字对不上的问题。
 *
 * ===================== 命令号为什么要用宏生成 =====================
 *
 * 命令号不直接写成 1、2、3，而是用 _IO/_IOR/_IOW/_IOWR 宏生成，因为宏会把
 * 4 样信息编码进这一个 32 位整数（位宽见 include/uapi/asm-generic/ioctl.h）：
 *
 *   位   31-30     29-16          15-8          7-0
 *       方向(2位)  数据大小(14位)  魔数(8位)     序号(8位)
 *
 *   - 方向    ：_IO 不传数据；_IOR 内核→用户（读）；_IOW 用户→内核（写）；_IOWR 双向
 *               注意"读写"是站在用户态的角度说的
 *   - 数据大小：宏的第三个参数的 sizeof，比如 int 就是 4
 *   - 魔数    ：区分"这是哪个驱动的命令"，一般用一个字符
 *   - 序号    ：同一个驱动里的第几个命令
 *
 * 好处：
 *   - 不同驱动的命令号不容易撞车（魔数不同）
 *   - 内核和工具（比如 strace）能从命令号里看出方向和数据大小，便于检查和调试
 *   - 驱动不认识的命令号，按约定返回 -ENOTTY
 *
 * 算一下这里两个命令的实际数值（可以在测试程序里 printf("%#x") 验证）：
 *   HELLO_CLEAR   = _IO('h', 1)       = 方向 0、大小 0、魔数 0x68、序号 1 = 0x00006801
 *   HELLO_GET_LEN = _IOR('h', 2, int) = 方向 2（读）、大小 4、魔数 0x68、序号 2 = 0x80046802
 *
 * 关于魔数 'h'：内核的 Documentation/userspace-api/ioctl/ioctl-number.rst 登记了
 * 各驱动占用的魔数，'h' 其实已经被 hpet 等驱动用过了。练习里用它没问题：ioctl 是发给
 * 某个具体的 fd 的，内核只会把命令交给 /dev/hello0 的驱动，不会串到别的驱动去。
 * 但如果要把驱动提交到上游内核，就要选一个没人用的魔数，并在那个文件里登记。
 */
#ifndef HELLO_IOCTL_H	/* 头文件保护：防止同一个 .c 里被 #include 两次导致重复定义 */
#define HELLO_IOCTL_H

/*
 * _IO/_IOR 等宏的定义。<linux/ioctl.h> 属于 uapi（内核导出给用户态的头文件），
 * 内核代码和用户态程序都能 include，所以这个头文件两边都能用。
 */
#include <linux/ioctl.h>

#define HELLO_MAGIC 'h'		/* 本驱动的魔数 */

/*
 * 清空缓冲区：把有效长度归零。不带参数，所以用 _IO。
 * 用户态：ioctl(fd, HELLO_CLEAR);
 */
#define HELLO_CLEAR   _IO(HELLO_MAGIC, 1)

/*
 * 查询当前写入了多少字节：驱动把长度写到用户给的 int 变量里，
 * 数据方向是内核→用户，所以用 _IOR，第三个参数 int 表示传的是一个 int。
 * 用户态：int len; ioctl(fd, HELLO_GET_LEN, &len);
 */
#define HELLO_GET_LEN _IOR(HELLO_MAGIC, 2, int)

#endif /* HELLO_IOCTL_H */
