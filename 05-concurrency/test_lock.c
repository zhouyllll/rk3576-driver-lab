/*
 * lockdep 实验 B：依次调用 HELLO_RACE_LOCK_AB、HELLO_RACE_LOCK_BA 各一次。
 * 这个程序是完整的（不是练习重点），直接 make 就行。
 * 用法：./test_lock        然后 dmesg 看 lockdep 的报告
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "hello_race.h"

int main(void)
{
	int fd = open("/dev/hello_race", O_RDONLY);

	if (fd < 0) {
		perror("open /dev/hello_race");
		return 1;
	}
	if (ioctl(fd, HELLO_RACE_LOCK_AB))
		printf("LOCK_AB failed: %s\n", strerror(errno));
	else
		printf("LOCK_AB ok\n");
	if (ioctl(fd, HELLO_RACE_LOCK_BA))
		printf("LOCK_BA failed: %s\n", strerror(errno));
	else
		printf("LOCK_BA ok\n");
	close(fd);
	return 0;
}
