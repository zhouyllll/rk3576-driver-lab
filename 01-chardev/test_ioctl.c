/* 用户态测试程序，跑在板子上，验证 hello_chrdev 的 ioctl 接口。
 * 这是测试工具，不是驱动逻辑本身——你要学的东西都在 hello_chrdev.c 里。
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "hello_ioctl.h"

int main(void)
{
	int fd, len;

	fd = open("/dev/hello_chrdev", O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	/* 1. 写入数据 */
	write(fd, "hello", 5);

	/* 2. HELLO_GET_LEN：应该读到 5 */
	len = -1;
	if (ioctl(fd, HELLO_GET_LEN, &len) < 0) {
		perror("ioctl HELLO_GET_LEN");
		close(fd);
		return 1;
	}
	printf("写入后长度 = %d（期望 5）\n", len);

	/* 3. HELLO_CLEAR：清空缓冲区 */
	if (ioctl(fd, HELLO_CLEAR) < 0) {
		perror("ioctl HELLO_CLEAR");
		close(fd);
		return 1;
	}

	/* 4. 再查一次长度：应该变成 0 */
	len = -1;
	ioctl(fd, HELLO_GET_LEN, &len);
	printf("清空后长度 = %d（期望 0）\n", len);

	close(fd);
	return 0;
}
