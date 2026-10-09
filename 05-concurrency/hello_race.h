/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 驱动和用户态测试程序共用的"合同"：read 一次返回一个 struct hello_snapshot。
 * 这个文件是完整的，不用改。
 *
 * 一致性检查的原理：
 *   中断里先写 seq，再写 ts_ns、level，最后写 seq_check = seq。
 *   如果一次 read 拿到的快照"完整"，seq 一定等于 seq_check；
 *   如果 read 正好读到中断"写了一半"的状态，就会出现 seq != seq_check —— 这叫撕裂读（torn read）。
 */
#ifndef HELLO_RACE_H
#define HELLO_RACE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t __u32;
typedef uint64_t __u64;
#endif

struct hello_snapshot {
	__u32 seq;		/* 第几次中断 */
	__u32 level;		/* 中断时引脚电平 */
	__u64 ts_ns;		/* 中断时间，ktime_get_ns() */
	__u32 seq_check;	/* 写完其他字段后，最后写成跟 seq 一样 */
	__u32 pad;		/* 凑齐 8 字节对齐，让内核和用户态看到的结构体大小一致 */
};

#endif
