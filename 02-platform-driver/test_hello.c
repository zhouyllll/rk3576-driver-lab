/* 用户态测试程序，跑在板子上，验证 /dev/hello0 的读写、ioctl、截断和写满行为。
 * 所有检查都在同一个 fd 上做：驱动的缓冲区是每次 open 一份的，换个 fd 就是空的。
 * 编译：${CROSS_COMPILE}gcc -static -o test_hello test_hello.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "hello_ioctl.h"

#define DEV "/dev/hello0"

static int failures;

static void check(int ok, const char *what)
{
	printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failures++;
}

static int get_len(int fd)
{
	int len = -1;

	if (ioctl(fd, HELLO_GET_LEN, &len) < 0)
		perror("ioctl HELLO_GET_LEN");
	return len;
}

int main(void)
{
	char big[600], back[16];
	ssize_t n;
	int fd;

	fd = open(DEV, O_RDWR);
	if (fd < 0) {
		perror("open " DEV);
		return 1;
	}

	/* 1. 写 5 字节，长度应为 5 */
	n = write(fd, "hello", 5);
	check(n == 5, "write 5 bytes returns 5");
	check(get_len(fd) == 5, "HELLO_GET_LEN == 5");

	/* 2. 同一个 fd 从头读回来，内容应一致（pread 指定偏移 0，不影响当前写位置） */
	memset(back, 0, sizeof(back));
	n = pread(fd, back, sizeof(back), 0);
	check(n == 5 && memcmp(back, "hello", 5) == 0, "read back \"hello\"");

	/* 3. HELLO_CLEAR 后长度应为 0 */
	check(ioctl(fd, HELLO_CLEAR) == 0, "HELLO_CLEAR succeeds");
	check(get_len(fd) == 0, "HELLO_GET_LEN == 0 after clear");
	close(fd);

	/* 4. 新 fd 写 600 字节，缓冲区只有 512（设备树 buf-size），应只收下 512 */
	fd = open(DEV, O_RDWR);
	if (fd < 0) {
		perror("open " DEV);
		return 1;
	}
	memset(big, 'a', sizeof(big));
	n = write(fd, big, sizeof(big));
	printf("       write 600 bytes -> returned %zd\n", n);
	check(n == 512, "write 600 bytes truncated to 512");
	check(get_len(fd) == 512, "HELLO_GET_LEN == 512");

	/* 5. 已经写满，再写应失败并返回 ENOSPC，而不是返回 0 */
	errno = 0;
	n = write(fd, "x", 1);
	check(n == -1 && errno == ENOSPC, "write when full fails with ENOSPC");

	/* 6. 用 pwrite 指定远超缓冲区的偏移，应被拒绝（这就是之前 space_left 下溢的场景） */
	errno = 0;
	n = pwrite(fd, "x", 1, 10000);
	check(n == -1 && errno == ENOSPC, "pwrite at offset 10000 rejected with ENOSPC");

	/* 7. 未知 ioctl 命令应返回 ENOTTY */
	errno = 0;
	check(ioctl(fd, _IO(HELLO_MAGIC, 99)) == -1 && errno == ENOTTY, "unknown ioctl -> ENOTTY");
	close(fd);

	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
