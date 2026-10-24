# RK3576 Linux 驱动开发练习

在 KICKPI-K7 开发板（Rockchip RK3576）上从零手写的 Linux 设备驱动，共 10 个阶段、15 个内核模块，全部上板验证。
每个阶段都配了一个「故意埋 bug，再用工具定位」的调试练习。踩过的坑记录在 [DEBUG-CASES.md](DEBUG-CASES.md)，共 22 个案例。

- 硬件：KICKPI-K7（RK3576，4×A72 + 4×A53），40pin 扩展口，cam3 上接 IMX415 摄像头模组
- 内核：Linux 6.1.75（Rockchip SDK），驱动以 out-of-tree 模块编译，通过 NFS 热加载
- 学习路线参考 Bootlin 内核驱动课程，练习都改成在这块板子的真实外设上做

## 阶段

| 阶段 | 目录 | 驱动 | 练了什么 |
|---|---|---|---|
| 1 | [01-chardev](01-chardev) | `hello_chrdev` | 字符设备、`file_operations`、`copy_to/from_user`、ioctl、mutex、每次 open 独立的 `private_data` |
| 2 | [02-platform-driver](02-platform-driver) | `hello_platform` | 设备模型、设备树匹配与自定义属性、`devm_*`、misc 设备、sysfs、debugfs、dynamic debug |
| 3 | [03-gpio](03-gpio) | `hello_gpio` `hello_led` `hello_pwm` | gpiod、leds 子系统、PWM 调光（0–255 级）、用 pinctrl debugfs 查 `-EBUSY` 的占用者 |
| 4 | [04-irq-sleep](04-irq-sleep) | `hello_irq` `hello_key` | GPIO 中断、阻塞 `read`、`poll`；tasklet、workqueue、线程化中断三种下半部；timer_list 消抖；hrtimer；input 子系统 |
| 5 | [05-concurrency](05-concurrency) | `hello_race` | 复现撕裂读，再用 `spin_lock_irqsave` 修复；completion、atomic；lockdep 抓 AB-BA 死锁 |
| 6 | [06-i2c](06-i2c) | `hello_i2c` | 解绑原厂驱动，用 `i2c_transfer`（两条消息、RESTART）读 IMX415 芯片 ID；地址写错时的 NACK |
| 7 | [07-mmio-uart](07-mmio-uart) | `hello_uart` | 寄存器级 UART（16550 兼容）：ioremap、`clk_set_rate`、波特率分频、内部回环 |
| 8 | [08-dma-mmap](08-dma-mmap) | `hello_uart_dma` `hello_mmap` `hello_bugs` | PL330 dmaengine 发送；`remap_pfn_range` 实现 mmap；KASAN、kmemleak 抓越界、UAF、泄漏 |
| 9 | [09-spi](09-spi) | `hello_spi` | 用 `driver_override` 接管 spi4.0，`spi_sync_transfer` 全双工回环 |
| 10 | [10-pm](10-pm) | `hello_uart_pm` | runtime PM：autosuspend 关时钟、系统休眠复用 runtime 回调、probe 失败时的回滚顺序 |

[driver_summary.c](driver_summary.c) 是 10 个阶段的知识总结，包括常用 API、alloc 与 free 的配对、错误回滚的写法，以及案例索引。

## 编译与加载

```bash
# 交叉编译环境：ARCH=arm64，CROSS_COMPILE=aarch64-linux-gnu-
cd 06-i2c
make KDIR=/path/to/rk3576-kernel     # 默认 KDIR 是作者本机的内核树路径

# 板子上
echo 8-0037 > /sys/bus/i2c/drivers/imx415/unbind   # 部分阶段要先解绑原厂驱动，见各文件开头的注释
insmod hello_i2c.ko
cat /sys/bus/i2c/devices/8-0037/chip_id            # 0xe0
```

每个驱动文件开头的注释里写了要用的设备树节点、接线方式和测试命令。

## 调试案例节选

| 案例 | 现象 → 根因 |
|---|---|
| 10 | 在硬中断里 `msleep`，读懂 `scheduling while atomic` 报错 |
| 16 | UART 回环超时：baudclk 默认只有 187500 Hz，算出的分频是 0 |
| 17 | 不开时钟就读 UART 寄存器，不会 oops，读到的是上一次读的残留值 |
| 18 | mmap 不检查范围，用户态读到了别人的物理页 |
| 20 | DMA 看起来超时，其实已经成功，是 write 持锁返回导致的 |
| 22 | `memset` 清错了对象，长度一大就踩坏内核栈 |

完整内容见 [DEBUG-CASES.md](DEBUG-CASES.md)。

## 许可

驱动代码按 GPL-2.0 发布，与各源文件里的 `MODULE_LICENSE` 一致。
