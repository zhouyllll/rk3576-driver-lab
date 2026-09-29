/* 驱动（内核态）和测试程序（用户态）共用的命令号定义，只写一份，两边 #include 这个文件，
 * 避免两边各抄一遍导致数字对不上。 */
#ifndef HELLO_IOCTL_H
#define HELLO_IOCTL_H

#include <linux/ioctl.h>

#define HELLO_MAGIC 'h'
#define HELLO_CLEAR   _IO(HELLO_MAGIC, 1)
#define HELLO_GET_LEN _IOR(HELLO_MAGIC, 2, int)

#endif
