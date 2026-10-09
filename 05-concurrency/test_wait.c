/*
 * 阶段 5 练习 ②：测试 HELLO_RACE_WAIT_N
 *
 * 【这是骨架】标 TODO 的地方由你来写。
 *
 * 用法：./test_wait [N，默认 1000]
 * 做的事：调用 ioctl 等 N 次中断，测量等了多久，打印结果。
 *   use_timer=1 period_us=100 时，等 1000 次应该大约 100ms
 *   没有中断时（比如 use_timer=0 又没接线），应该在 wait_timeout_ms 后超时
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include "hello_race.h"

/* 已写好：取单调时钟的毫秒数（不受修改系统时间影响），用来测"等了多久" */
static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	__u32 n = argc > 1 ? strtoul(argv[1], NULL, 0) : 1000;
	double t0, t1;
    int fd, ret;
	/*
	 * TODO 1：只读打开 /dev/hello_race，失败 perror 后返回 1
	 *   （ioctl 不要求写权限，只读打开就能调用）
	 */
	fd = open("/dev/hello_race", O_RDWR);
	if (fd<0) {
		perror("open /dev/hello_race");
		return 1;
	}
	/*
	 * TODO 2：
	 *   double t0 = now_ms();
	 *   ret = ioctl(fd, HELLO_RACE_WAIT_N, &n);    ← 第三个参数传的是 n 的地址，驱动里 copy_from_user 读它
	 *   double t1 = now_ms();
	 *
	 *   ret == 0：打印 "waited %u interrupts in %.1f ms"
	 *   ret <  0：打印失败原因：strerror(errno) 会把 ETIMEDOUT 变成 "Connection timed out"、
	 *             EBUSY 变成 "Device or resource busy"，再打印等了多久
	 *
	 * 思考：用户态看到的 ioctl 返回值是 -1，真正的错误码在 errno 里。
	 *       驱动里 return -ETIMEDOUT，是怎么变成用户态的 ret = -1、errno = ETIMEDOUT 的？
	 *       （阶段 4 讲 /dev 和错误码时提过：libc 做了转换）
	 */
	t0 = now_ms();
	ret = ioctl(fd,HELLO_RACE_WAIT_N,&n);
	t1 = now_ms();

	if (ret == 0)
		printf("waited %u interrupts in %.1f ms\n", n, t1 - t0);
	else
		printf("ioctl failed: %s (after %.1f ms)\n", strerror(errno), t1 - t0);
	/*
	 * TODO 3：关闭 fd，返回 0
	 */
	close(fd);
	(void)n;
	(void)now_ms;
	return 0;
}
