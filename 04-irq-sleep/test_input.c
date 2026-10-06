/*
 * 简化版 evtest：按名字找到 /dev/input/eventN，打印收到的每个 input_event。
 * 板上的最小 rootfs 没有 evtest，自己写一个，顺便看清楚 input_event 长什么样。
 *
 * 编译：${CROSS_COMPILE}gcc -static -o test_input test_input.c
 * 用法：./test_input [设备名，默认 hello-key] [超时秒数，默认 10]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

static const char *type_name(unsigned short type)
{
	switch (type) {
	case EV_SYN: return "EV_SYN";
	case EV_KEY: return "EV_KEY";
	case EV_REL: return "EV_REL";
	case EV_ABS: return "EV_ABS";
	default:     return "?";
	}
}

int main(int argc, char **argv)
{
	const char *want = argc > 1 ? argv[1] : "hello-key";
	int timeout_s = argc > 2 ? atoi(argv[2]) : 10;
	char path[32], name[64];
	int fd = -1;

	/* 遍历 event0..31，用 EVIOCGNAME 读设备名，找到想要的那个 */
	for (int i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && !strcmp(name, want))
			break;
		close(fd);
		fd = -1;
	}
	if (fd < 0) {
		fprintf(stderr, "no input device named '%s'\n", want);
		return 1;
	}
	printf("%s: \"%s\"，等待事件 %d 秒...\n", path, name, timeout_s);

	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	while (poll(&pfd, 1, timeout_s * 1000) > 0) {
		struct input_event ev;

		if (read(fd, &ev, sizeof(ev)) != sizeof(ev))
			break;
		printf("time %ld.%06ld  type %d (%s)  code %d  value %d%s\n",
		       (long)ev.input_event_sec, (long)ev.input_event_usec,
		       ev.type, type_name(ev.type), ev.code, ev.value,
		       ev.type == EV_KEY ? (ev.value ? "  ← 按下" : "  ← 松开") :
		       ev.type == EV_SYN ? "  -------- SYN_REPORT" : "");
		fflush(stdout);
	}
	printf("timeout\n");
	close(fd);
	return 0;
}
