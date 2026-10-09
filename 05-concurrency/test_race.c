/*
 * 阶段 5 练习 ①：压力测试，统计撕裂读。
 *
 * 【这是骨架】标 TODO 的地方由你来写。
 *
 * 编译：make（Makefile 里已经写好了交叉编译规则）
 * 用法：./test_race [秒数，默认 5]
 *
 * 做的事：在规定时间内不停地 read /dev/hello_race，每次检查 seq 和 seq_check 是否相等。
 * 最后打印：读了多少次、撕裂读多少次、看到的 seq 从多少到多少（说明这段时间来了多少次中断）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include "hello_race.h"

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 5;
	struct hello_snapshot snap;
    unsigned long reads = 0, torn = 0;
    __u32 first_seq = 0, last_seq = 0;
    time_t start;

	/*
	 * TODO 1：打开 /dev/hello_race（只读），失败就 perror 并返回 1
	 */
	int fd = open("/dev/hello_race",O_RDWR);
	if (fd < 0) {
		perror("open /dev/hello_race");
		return 1;
	}
	/*
	 * TODO 2：循环 read，直到超过 seconds 秒
	 *
	 * 计时：用 time(NULL) 记下开始时间，每次循环比较一下就够了（不需要很精确）
	 *
	 * 每次循环：
	 *   - read(fd, &snap, sizeof(snap))，返回值不等于 sizeof(snap) 就报错退出
	 *   - 读的总次数 +1
	 *   - 如果 snap.seq != snap.seq_check：撕裂读次数 +1；
	 *     前 5 次撕裂读把 seq 和 seq_check 打印出来，看看撕成了什么样
	 *   - 记录第一次和最后一次看到的 seq
	 *
	 * 思考：如果中断根本没来（比如忘了启动方波），seq 会一直是多少？撕裂读还会出现吗？
	 */
	start = time(NULL);
	while (time(NULL) - start < seconds){

		if (read(fd,&snap,sizeof(snap))!= sizeof(snap)){
            break;
		}

		reads++;

		if (reads == 1)                       
            first_seq = snap.seq;
        last_seq = snap.seq;

        if (snap.seq != snap.seq_check) {                   
            torn++;
        
			if (torn <= 5)
            	printf("torn #%lu: seq=%u seq_check=%u\n",
            	torn, snap.seq, snap.seq_check);
        }


	}
	/*
	 * TODO 3：打印统计结果，关闭 fd
	 *   reads=xxx torn=xxx seq: 起始 -> 结束（共 N 次中断）
	 */
	printf("reads=%lu torn=%lu seq: %u -> %u (%u interrupts)\n",
             reads, torn, first_seq, last_seq, last_seq - first_seq);
    
	close(fd);
	(void)seconds;
	return 0;
}
