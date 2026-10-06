/*
 * 用 poll 等 /dev/hello_irq 的中断，带超时。
 * 编译：${CROSS_COMPILE}gcc -static -o test_poll test_poll.c
 * 用法：./test_poll [超时秒数，默认 5]
 *
 * 跟 cat 的区别：cat 用阻塞 read，没中断就一直睡；
 * poll 可以设超时，也可以同时等多个 fd——这就是驱动要实现 .poll 的原因。
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int timeout_s = argc > 1 ? atoi(argv[1]) : 5;
	struct pollfd pfd;
	char buf[16];
	ssize_t n;

	pfd.fd = open("/dev/hello_irq", O_RDONLY);
	if (pfd.fd < 0) {
		perror("open /dev/hello_irq");
		return 1;
	}
	pfd.events = POLLIN;

	for (;;) {
		int ret = poll(&pfd, 1, timeout_s * 1000);

		if (ret < 0) {
			perror("poll");
			break;
		}
		if (ret == 0) {
			printf("timeout: no interrupt in %d s\n", timeout_s);
			break;
		}
		n = read(pfd.fd, buf, sizeof(buf) - 1);
		if (n < 0) {
			perror("read");
			break;
		}
		buf[n] = '\0';
		printf("interrupt count: %s", buf);
	}

	close(pfd.fd);
	return 0;
}
