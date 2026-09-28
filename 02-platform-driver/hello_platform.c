/*
 * platform_driver 骨架 —— 由你自己实现，这里只搭接口。
 *
 * 目标：写一个最简单的 platform_driver，跟设备树里一个节点绑定上，
 * 绑定成功时内核会调用你的 probe()，卸载模块时调用 remove()。
 *
 * 配套的设备树节点已经加在 rk3576-kickpi-k7.dtsi 里了（在 hello_platform
 * 节点，compatible = "zhouyllll,hello-platform"），已经刷进板子、重启验证过：
 *   cat /proc/device-tree/hello-platform/compatible
 * 能读到 "zhouyllll,hello-platform"，说明设备树这边没问题，剩下的活是你的。
 *
 * platform_driver 和字符设备（01-chardev）的关系：
 *   字符设备负责"怎么把自己暴露成 /dev/xxx 给用户态用"
 *   platform_driver 负责"怎么让内核知道该在什么时候创建/销毁你的设备实例"
 * 这次先练"绑定机制"本身，probe/remove 里先只打印一行日志证明被调用到了；
 * 后面可以把 01-chardev 那套字符设备的注册逻辑搬进 probe() 里，
 * 变成"设备树驱动了字符设备的生命周期"，两条线才算真正接上。
 *
 * 要做的事：
 *   1. 定义一张 struct of_device_id 表，里面至少一条：
 *        { .compatible = "zhouyllll,hello-platform" }
 *      数组最后一项必须是空的 {} 结尾（内核靠这个知道表到哪结束）。
 *      表定义完之后紧跟着写一行：
 *        MODULE_DEVICE_TABLE(of, 你的表名);
 *      这行是给 depmod/modprobe 用的（生成模块跟 compatible 字符串的映射，
 *      支持热插拔自动加载），先照抄语法，不用深究热插拔那一套。
 *
 *   2. 定义 hello_platform_probe(struct platform_device *pdev)：
 *      设备树节点匹配上、且状态是 okay 时，内核会自动调用这个函数。
 *      这次先只打印一行 printk（比如带上 pdev->name），返回 0 表示成功。
 *
 *   3. 定义 hello_platform_remove(struct platform_device *pdev)：
 *      对应卸载时机（rmmod，或者设备被移除）。同样先打印一行，返回 0。
 *      （这个内核版本 .remove 签名还是返回 int，不是新版本那个返回 void
 *      的 .remove_new——如果你之后翻资料看到 void 版本，那是更新内核的写法，
 *      这里先按 int 来。）
 *
 *   4. 定义 struct platform_driver，把上面几样串起来：
 *        .probe = hello_platform_probe,
 *        .remove = hello_platform_remove,
 *        .driver.name = "hello_platform",     // 出现在 /sys/bus/platform/drivers/ 下的名字
 *        .driver.of_match_table = 你的 of_device_id 表,
 *
 *   5. 注册/注销这个 platform_driver——两种写法，选一种：
 *      a) 手动写 module_init/module_exit，里面调用
 *         platform_driver_register(&你的driver) / platform_driver_unregister(...)
 *         （跟 01-chardev 里手写 hello_init/hello_exit 是一个套路，
 *         便于对照理解"注册"这个动作本身）
 *      b) 用 module_platform_driver(你的driver) 这一个宏，它会自动展开成
 *         标准的 module_init/module_exit，省得手写——推荐用这个，但要先
 *         用 a) 的方式理解清楚这个宏在背后做了什么，不要直接跳过原理。
 *
 * 测试：
 *   insmod hello_platform.ko
 *   dmesg | tail                          # 看 probe 里的打印
 *   ls /sys/bus/platform/drivers/hello_platform/   # 应该能看到绑定上的设备
 *   rmmod hello_platform
 *   dmesg | tail                          # 看 remove 里的打印
 *
 * 部署流程别忘了 DEBUG-CASES.md 案例4 踩过的坑：编译成功不等于板子上跑的
 * 是新代码，insmod 前先 rmmod 旧的、.ko 记得同步到 NFS 共享目录。
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouylll");
MODULE_DESCRIPTION("Minimal platform driver exercise");

/* TODO: struct of_device_id 表 + MODULE_DEVICE_TABLE */

/* TODO: hello_platform_probe */

/* TODO: hello_platform_remove */

/* TODO: struct platform_driver */

/* TODO: 注册/注销（module_init+module_exit，或者 module_platform_driver） */
