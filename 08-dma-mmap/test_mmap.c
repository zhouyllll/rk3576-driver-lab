// SPDX-License-Identifier: GPL-2.0
/*
 * 阶段 8 练习 ① 的用户态测试：mmap /dev/hello_mmap，验证用户和内核看到的是同一块内存。
 *
 * 【这是骨架】TODO 由你来写。
 *
 * 用法：./test_mmap [映射的字节数]       默认 8192（2 页）
 *   ./test_mmap 4096       只映射前 1 页
 *   ./test_mmap 16384      比缓冲区大，驱动应该拒绝（mmap 返回 MAP_FAILED，errno = EINVAL）
 *
 * 编译（在 PC 上交叉编译，放到 NFS 目录）：
 *   aarch64-none-linux-gnu-gcc -O2 -Wall -static -o test_mmap test_mmap.c
 *   （编译器名字以 env.sh 里的 CROSS_COMPILE 为准，后面加 gcc）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
	size_t len = argc > 1 ? strtoul(argv[1], NULL, 0) : 8192;
	char *p;
	char buf[64];
	int fd;

	fd = open("/dev/hello_mmap", O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	/*
	 * TODO A：映射
	 *
	 * 【新写法】p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	 *   NULL：让内核自己选一个用户地址
	 *   len：映射多少字节
	 *   PROT_READ | PROT_WRITE：可读可写
	 *   MAP_SHARED：写进去的内容对内核（和其他映射了它的进程）可见。
	 *               换成 MAP_PRIVATE 的话，写的时候会"写时复制"出一份私有拷贝，内核看不到
	 *   fd、0：映射哪个文件，从文件的哪个偏移开始（必须是页的整数倍）
	 *
	 * 失败时返回的是 MAP_FAILED（不是 NULL！），用 perror("mmap") 打印原因，然后关 fd 返回 1
	 */

	/* 第 1 步：直接读内核写在缓冲区开头的那句话——没有调用 read，没有拷贝 */
	printf("mapped %zu bytes at %p\n", len, p);
	printf("[user reads via mmap]  %s", p);

	/*
	 * TODO B：用户通过指针写，内核通过 read 读出来
	 *
	 * 1. strcpy(p, "hello from user via mmap\n");    —— 就是普通的写内存
	 * 2. memset(buf, 0, sizeof(buf));
	 *    pread(fd, buf, sizeof(buf) - 1, 0)          —— 走驱动的 read，从偏移 0 读
	 *    【新写法】pread(fd, buf, n, offset)：在指定偏移读，不改变文件当前位置
	 * 3. printf("[kernel read() sees]   %s", buf);
	 */

	/*
	 * TODO C：内核通过 write 写，用户通过指针看到
	 *
	 * 1. pwrite(fd, "written by write()\n", 19, 4096)  —— 走驱动的 write，写到第 2 页开头（偏移 4096）
	 *    （如果 len 小于 8192，第 2 页没映射，跳过这一步）
	 * 2. printf("[user sees via mmap]   %s", p + 4096);
	 */

	printf("press Enter to munmap and exit (try rmmod hello_mmap from another shell first)\n");
	getchar();

	/*
	 * TODO D：解除映射：munmap(p, len)，然后 close(fd)
	 */
	return 0;
}
