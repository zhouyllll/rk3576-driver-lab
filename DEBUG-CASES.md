# 排查案例记录

格式：现象 → 排查 → 根因 → 修复 → 验证，参考
`~/embedded-interview/项目经历-openvela-RK3576-BSP适配.md` 和
`~/embedded-lab/qiuzhi/rk3576-ipc-camera/docs/DEBUG-CASES.md` 的记录方式。

## 案例 1：`01-chardev` 第一版实现的五个真实 bug

**背景**：字符设备驱动第一版实现（`open`/`release`/`read`/`write` 骨架 + `struct hello_dev`
封装）编译通过，但功能没有实测过。逐行 review 出以下问题：

1. **`hello_write` 无边界检查**：`copy_from_user(kbuf + *ppos, buf, count)` 里
   `count` 直接照抄用户传入值，没有 clamp 到 `BUF_SIZE - *ppos`——写超过缓冲区
   大小会造成内核内存越界写。**修复**：先算 `bytes_to_write = min_t(size_t, count, BUF_SIZE - *ppos)`，
   拷贝用这个值而不是 `count`。
2. **`kbuf_len` 从未被更新**：`hello_write` 完成拷贝后没有任何地方给 `kbuf_len`
   赋值，它作为静态全局变量永远是初始值 0，导致 `hello_read` 第一行
   `if (*ppos >= kbuf_len) return 0;` 永远成立——不管写了什么，读永远是 EOF。
   **修复**：拷贝成功后 `*ppos += bytes_to_write; if (*ppos > kbuf_len) kbuf_len = *ppos;`。
3. **`copy_from_user` 返回值判断方向错误**：该函数返回"未成功拷贝的字节数"
   （0 = 全部成功，正数 = 部分失败），**不会返回负数**，原代码写的
   `if (ret < 0)` 这个分支永远不会触发，等于没做错误检查。**修复**：改成 `if (ret)`。
4. **`hello_read` 函数末尾 `return 0`**：数据已经通过 `copy_to_user` 正确拷贝，
   但函数返回值（read() 系统调用的"实际读到几字节"）写死成 0，用户态看到的是
   "读到 EOF，一个字节都没有"，哪怕内核这边拷贝是成功的。`cat` 会直接不打印
   任何内容。**修复**：`return bytes_to_read;`。
5. **`hello_release` 里声明了未使用的变量**：`struct hello_dev *dev = ...` 声明了
   但函数体没有用到，内核编译把未使用变量当错误（`-Werror=unused-variable`），
   直接编译失败。**修复**：这一版没有需要在 release 里清理的资源（`kbuf` 是全局静态，
   不是 open() 里临时分配的），直接删掉这行声明。

**验证**：五处改完，`make` 编译通过，`insmod` 之后
`echo -n "hello" > /dev/hello_chrdev && cat /dev/hello_chrdev` 能读回 `hello`，
换成 12 字节的 `hello_chrdev` 也能完整读回，边界/长度/返回值全部正确。

**方法论**：内核编译对警告零容忍（`-Werror`）在这里帮了忙——编译期就拦下了
"变量声明了没用"这种低级失误；但类型系统拦不住的（`copy_from_user` 返回值语义、
`return 0` 这种"编译通过但逻辑错"的 bug），只能靠**代码走查 + 实际数据回环测试**
才能发现，光是"编译过了"不代表逻辑对。

---

## 案例 2：`insmod` 成功但找不到设备节点——查错了名字，不是驱动有问题

**现象**：`insmod hello_chrdev.ko` 没有报错，`lsmod` 显示模块 `Live`，
但 `ls /dev/hello_chrdev` 一开始查不到（后来改了代码之后又查到了），一度
怀疑是 `device_create()` 没有真正成功。

**排查**：逐步核实四个建设备的步骤：
```bash
cat /proc/devices | grep mydev   # 510 mydev —— 设备号注册成功
ls /sys/class/ | grep mydev      # mydev —— class 建出来了
```
都是成功的，说明 `alloc_chrdev_region`/`cdev_add`/`class_create`/`device_create`
全部没问题。

**根因**：`class_create()`/`device_create()` 最后一个参数（决定 `/dev/` 下
节点叫什么名字）传的字符串是 `"mydev"`，跟 `#define DEVICE_NAME "hello_chrdev"`
这个宏完全是两回事——宏只在 `alloc_chrdev_region` 里当设备号注册名用。
一开始按照"驱动源文件叫 hello_chrdev，节点应该也叫这个"的直觉去查
`/dev/hello_chrdev`，查不到，其实节点一直都在，只是叫 `/dev/mydev`。

**教训**：Linux 驱动里至少有三个"名字"，很容易搞混，要分清楚指的是哪一个：
- **模块名**（`lsmod`/`rmmod` 用）：来自源文件名，`hello_chrdev.c` → 模块名 `hello_chrdev`
- **设备号注册名**（`/proc/devices` 里显示的）：`alloc_chrdev_region()` 的
  第三个参数，纯粹给人看的标签，不影响功能
- **设备节点名**（`/dev/` 下实际文件名）：`device_create()` 的最后一个参数

三个可以完全不一样（这次的驱动就是模块名 `hello_chrdev`、注册名和节点名
都是 `mydev`），排查时不能想当然认为它们该是同一个字符串。

**衍生的另一个坑**：`rmmod mydev` 报 "No such file or directory"——因为
`rmmod` 认的是上面说的**模块名**（`hello_chrdev`），不是设备节点名
（`mydev`），同样是这三个名字混淆导致的。

---

## 案例 3：`udhcpc` 报告"拿到租约"，但网络其实不通

**现象**：`udhcpc -i eth1` 输出 "lease of 192.168.1.100 obtained"，看起来成功了，
但板子 `ping` 网关和开发机都报 "Network is unreachable"（不是超时，是路由层面
直接判定不可达）。

**排查**：`ip addr show eth1` 发现根本没有 IPv4 地址（只有内核自动配置出来的
IPv6），证实 DHCP 到手的 IP 从未被真正应用到网卡上。

**根因**：`udhcpc` 只负责 DHCP 协议部分（跟服务器要 IP/网关这些信息），
真正把这些信息写进网卡（`ip addr add`/`ip route add`）要靠一个外部回调脚本
（`/usr/share/udhcpc/default.script`）。我们的最小 rootfs 里从来没有放这个
脚本，所以"协议层面成功"和"接口真的配置好了"是两件不同的事，`udhcpc`
的输出信息具有误导性——"lease obtained" 只保证协议握手成功，不保证网络能用。

**修复**：手动补上 `ip addr add`/`ip route add`（临时绕过，见 `README.md`）。
永久修复（还没做）：补一个 `default.script` 加进 rootfs，或者在 `/init`
里直接写死自动配置的逻辑。

**方法论**：工具打印"成功"字样，不代表整条链路真的通了——这跟这个项目
一路走来"不要信任'看起来成功'，要看真实产物/真实连通性"的方法论是一脉相承的。

---

## 案例 4：per-open private_data 改造后，ioctl 读出来的长度是垃圾值

**背景**：把缓冲区从全局静态改成每次 `open()` 用 `kmalloc` 分配一份
`struct hello_priv`（`buf`/`len`/`lock` 都在里面），配合 `HELLO_GET_LEN`/
`HELLO_CLEAR` 两个 ioctl 测试。

**现象**：`write(fd, "hello", 5)` 之后 `ioctl(fd, HELLO_GET_LEN, &len)`
读出来是 `-1022865920` 这种离谱的数，但紧接着 `HELLO_CLEAR` 再
`HELLO_GET_LEN` 却能正确读到 `0`。

**排查**：`HELLO_CLEAR` 走的是 `priv->len = 0;`，无条件赋值，能读对；
`HELLO_GET_LEN` 读到的是 `hello_write` 里维护的 `priv->len`——问题必然出在
写完之后 `priv->len` 没被正确更新成 5。回头看 `hello_open`：

```c
priv = kmalloc(sizeof(struct hello_priv), GFP_KERNEL);   /* 没有清零！ */
```

`kmalloc` 分配到的内存是内核堆里的旧数据，不保证是 0。`hello_write` 更新
长度用的是"新位置比已记录长度更靠后才更新"这个逻辑（模仿真实文件写入语义，
不会因为写得短就缩短文件）：

```c
if (*ppos > priv->len) priv->len = *ppos;
```

`priv->len` 的垃圾初值如果刚好是个很大的数，`*ppos`（=5）永远不会比它大，
这行判断永远不成立，`priv->len` 就永远停在垃圾值上，`HELLO_GET_LEN` 用
`put_user` 把这个 64 位 `size_t` 垃圾值截成 32 位 `int` 传出来，就是那个
离谱的负数。

**修复**：`kmalloc` 换成 `kzalloc`，让 `priv` 一分配出来就是全零，
`priv->len` 从 0 起步，"只在写到更靠后位置才更新长度"这条逻辑才能按预期
工作——这行逻辑本身没错，错的是没保证初始状态干净。

**衍生的第二个坑**：改完 `kzalloc` 重新 `make`，板子上再跑还是同样的垃圾
值，跟没改一样。检查发现：新编译出的 `.ko` 确实比源码新（编译是成功的），
但 NFS 共享目录（`~/rk3576-nfs-share/hello_chrdev.ko`，板子从这里
`insmod`）里的文件还是没同步的旧版本，`md5sum` 一对比两边完全不一样。
板子上 `insmod` 加载的从头到尾都是旧代码，改了源码根本没生效。

**教训**：`make` 编译成功 ≠ 板子上跑的就是新代码，NFS 开发流程里"编译"和
"部署"是两个独立步骤，忘了同步 `.ko` 到共享目录，会看到那种"改了但好像
没生效"的诡异现象，容易误判成代码逻辑还有别的 bug，浪费时间在错误的地方
排查。以后每次改完要么记得手动 `cp` 到 NFS 目录，要么写个一键脚本把
"编译 + 同步 NFS + 提示 rmmod/insmod"串起来，省得每次忘。

---

## 案例 5：rmmod 之后 `/dev/hello0` 还在——remove 里漏了 misc_deregister

**背景**：`02-platform-driver` 第 2 步，probe 里用 `misc_register` 注册了 `/dev/hello0`。

**现象**：`insmod` 后 `/dev/hello0` 出现了，这一步正常；但 `rmmod` 之后 `ls -l /dev/hello0`
还能看到这个节点，主次设备号 `10, 120` 都在。

**排查**：看 remove 函数，里面只有一行 `dev_info` 打印，没有调用 `misc_deregister`。

**根因**：probe 里注册的 misc 设备，在 remove 里没有注销。后果比"节点没消失"严重得多：

- 内核的 misc 设备链表里，还挂着这个 `miscdevice`
- 但它所在的 `hdev` 结构体，是 `devm_kzalloc` 分配的，设备解绑时已经被自动释放了
- 它的 `fops` 指向的 `hello_fops` 和几个操作函数，都在已经卸载的模块代码里

这时候如果 `cat /dev/hello0`，内核会跳到已经不存在的代码上执行，属于典型的
**use-after-free**（释放后使用）：轻则 oops，重则死机。再 `insmod` 一次，
也可能因为链表里有一个指向已释放内存的节点而出问题。

**修复**：remove 里用 `platform_get_drvdata(pdev)` 取回 `hdev`，调用
`misc_deregister(&hdev->miscdev)`。修好后重启板子（把内核恢复到干净状态），
再测：`insmod` 后节点出现，`rmmod` 后 `ls /dev/hello0` 提示不存在。

**教训**：probe 和 remove 要**逐行配对**检查，注册了什么，就要注销什么。
devm 只会自动释放 devm 系列申请的资源，`misc_register` 这类注册动作不在其中。

---

## 案例 6：write 里无符号数下溢导致越界写，修的时候又引入了未初始化的变量

**背景**：`02-platform-driver` 第 3 步，把 01 的 write 搬过来，缓冲区大小改成按设备树的
`buf_size` 动态分配。

**问题 1：无符号数下溢**

```c
size_t space_left = priv->size - *ppos;
size_t bytes_to_write = min_t(size_t, count, space_left);
```

`*ppos` 可以超过缓冲区大小：用户态调用 `pwrite(fd, buf, n, 10000)` 就能直接指定偏移。
这时 `512 - 10000` 按无符号数计算，会下溢成一个巨大的正数，`bytes_to_write` 就等于
用户传进来的 `count`。接下来 `copy_from_user(priv->buf + 10000, ...)` 会写到 512 字节
缓冲区**外面**，覆盖别人的内核内存。这是**堆溢出**，属于安全漏洞级别的 bug。

另外，缓冲区刚好写满（`*ppos == size`）时，`bytes_to_write` 等于 0，write 返回 0，
很多程序遇到这种情况会一直重试，陷入死循环。

**问题 2：修复时引入了未初始化的变量**

第一次修复时加了 `*ppos >= priv->size` 的检查，但把 `space_left` 的赋值删掉了，
变量声明却还留着，后面仍在使用。`space_left` 成了栈上的垃圾值。
**编译没有任何报错或警告**，因为内核默认关掉了"变量可能未初始化"的警告（误报太多）。

**最终修复**：

```c
if (mutex_lock_interruptible(&priv->lock))
        return -ERESTARTSYS;

if (*ppos >= priv->size) {          /* 先检查 */
        ret = -ENOSPC;
        goto out_unlock;
}
bytes_to_write = min_t(size_t, count, priv->size - *ppos);   /* 再相减，不会下溢 */
```

检查和计算都放到了锁里面：原来的写法在加锁**之前**读取 `*ppos`，顺序也不对。

**验证**：`test_hello` 专门测了这两个场景，都通过了：

- 写满之后再写，返回 `ENOSPC`
- `pwrite` 指定偏移 10000，被拒绝并返回 `ENOSPC`

**教训**：

- 无符号数相减之前，**先检查、再相减**
- 删掉某个变量的赋值语句时，要检查这个变量后面还有没有被用到
- 这类问题编译器发现不了，只能靠代码 review 和专门针对边界的测试。
  测试程序要故意去测"异常输入"，不能只测正常路径

---

## 案例 7：设备树属性一直读不到——属性名两边没对上

**现象**：probe 里读 `label` 属性一直失败，一直走默认值 `hello`；同一个节点里的
`buf-size` 却能正常读到 512。

**排查**：

- `ls /proc/device-tree/hello-platform/` 能看到 `label`，说明设备树里确实有这个属性
- 回头看代码，读的是 `"zhouyllll,label"`

**根因**：设备树里写的是 `label`，代码里多了 `zhouyllll,` 前缀，字符串对不上。

两边为什么不一样：

- `label`、`compatible`、`status`、`reg` 这些是设备树规范定义的**通用属性**，不加前缀
- 只有驱动自定义的属性，比如 `zhouyllll,buf-size`，才加厂商前缀

写代码时把两类属性搞混了。

**修复**：代码改成读 `"label"`。

**教训**：属性名只靠两边手写保持一致，写错了编译器查不出来，运行时也不报错，
只是静默走默认值。所以读属性时**一定要检查返回值，并在没读到时打印出来**。
这次就是靠 probe 里打印的 "no label, use default"，才发现了问题。

---

## 案例 8：menuconfig 没带交叉编译器，.config 被按主机 gcc 重新计算

**背景**：补充练习 4，把驱动放进内核源码树，用 `make ARCH=arm64 menuconfig`
把新加的 `HELLO_PLATFORM` 选成 `m`。

**现象**：之后用交叉编译器 `make modules`，输出里夹着 7 行
`Error in reading or end of file.`，但模块照常编出来了。

**排查**：

- 这句话来自 `scripts/kconfig/conf.c:80`，是配置工具在"想在终端上问用户某个选项
  怎么选，但读不到输入"时打印的，说明编译时有一批配置项在 `.config` 里没有值
- 对比 `.config.old` 和 `.config`：
  - menuconfig 保存出来的版本里是 `CONFIG_CC_VERSION_TEXT="gcc (Ubuntu 13.3.0...)"`，也就是 **PC 主机上的 x86 gcc**
  - 一批依赖交叉编译器能力的选项消失了，比如 `ARM64_LSE_ATOMICS`、`ARM64_MTE`、`AS_HAS_ARMV8_*`

**根因**：内核的 Kconfig 在解析时会**实际调用编译器**，检测它支持哪些特性，
很多选项（`CC_HAS_*`、`AS_HAS_*`，以及依赖它们的功能）是根据检测结果决定能不能出现的。
执行 menuconfig 的终端没有 `source env.sh`，命令里又只写了 `ARCH=arm64`、
没写 `CROSS_COMPILE`，`make` 就用了主机上的 `gcc`，按 x86 编译器的能力把 `.config`
重新算了一遍并保存。之后换回交叉编译器编译时，这些选项又冒出来了，而且没有值，
配置工具就想问用户。

**修复**：

- 这次编译时，配置工具已经按默认值自动补上了这些选项，结果跟最初用交叉编译器生成
  `.config` 时一致
- 核对了关键配置（FIQ 调试串口、网卡、NFS、dynamic debug、debugfs、触摸屏）都没变
- 以后在内核目录下执行**任何** `make`，包括 menuconfig，都先 `source ~/rk3576-uboot/scripts/env.sh`，
  它会同时导出 `ARCH` 和 `CROSS_COMPILE`

**教训**：`.config` 不只取决于你选了什么，还取决于**用什么编译器**去解析 Kconfig。
交叉编译的项目里，只要有一条 `make` 命令漏了 `CROSS_COMPILE`，配置就可能被悄悄改掉。
这类问题的特征是：配置"看起来没动过"，但 `CONFIG_CC_VERSION_TEXT` 变了。

---

## 案例 9：控制台"卡死"，以为是硬中断里 msleep 导致的，其实是 NFS 卡住了

**背景**：阶段 4 调试练习，准备用 `insmod /mnt/nfs/hello_irq.ko bug_sleep_in_irq=1` 加载
"在硬中断里调 msleep"的版本，再 `echo 1` 触发中断，故意制造 `scheduling while atomic`。

**现象**：执行完命令后串口没有 `~ #` 提示符，敲回车只回显换行。看起来像是 msleep 把系统弄卡了。

**排查**：

- 开发机 `ping 192.168.1.100` 能通，串口也能回显：内核和网络都活着，只是 shell 不响应
- 当时我直接下了结论："中断借用了正在执行 echo 的 shell，msleep 把它挂起了"——**没有证据，是推测**
- 串口输入 `fiq` 进入 Rockchip FIQ debugger，`reboot` 热重启（不用按 RESET）
- 重启后 `mount -t pstore pstore /sys/fs/pstore`，读 `console-ramoops-0`（上一次启动的完整控制台日志）：
  ```
  [  136.886153] hello_irq hello-irq: gpio 124 -> irq 126, /dev/hello_irq ready   ← 正常版本
  [  143.750705] hello_gpio hello-gpio: probed, led gpio = 104
  [135599.922487] mpp_rkvenc2 ...: shutdown device                              ← 37 小时后我发的 reboot
  ```
  **没有 `BUG: scheduling while atomic`，也没有第二次 insmod 的打印**——带 bug 的模块根本没加载上

**根因**：shell 卡在 `insmod /mnt/nfs/...` 读文件上。这次开机后过了 37 小时才操作，期间开发机（WSL）
大概率睡眠过或网络断过；NFS 是 `hard` 挂载，服务器连不上时读文件的进程会无限等待。
之前板子掉电重启那次，也出现过"所有碰 `/mnt/nfs` 的命令都卡住"的同样现象。

**修复**：热重启后 NFS 重新挂载，重做实验，这次 BUG 正常出现（见案例 10）。

**教训**：
- **现象一样（控制台没反应），原因可以完全不同**。先找证据（pstore、dmesg），再下结论
- 怀疑驱动之前，先排除环境：网络通不通、NFS 能不能访问（`ls /mnt/nfs` 会不会卡）
- 板子卡住时的三件工具：串口 `fiq` → FIQ debugger（`reboot`、`ps`、`bt`、`kmsg`）；
  热重启后看 pstore；`ping` 判断内核是否还活着

---

## 案例 10：故意在硬中断里 msleep——读懂 `scheduling while atomic`

**背景**：阶段 4 调试练习。`hello_irq.c` 加了模块参数 `bug_sleep_in_irq`，打开后中断处理函数里先 `msleep(10)`。
杜邦线连接 40pin 的 33 号（GPIO3_B0，输出）和 23 号（GPIO3_D4，中断输入）。

**操作**：
```sh
insmod /mnt/nfs/hello_gpio.ko
insmod /mnt/nfs/hello_irq.ko bug_sleep_in_irq=1
echo 1 > /sys/devices/platform/hello-gpio/value     # 产生上升沿
```

**现象**（串口原始记录：`04-irq-sleep/bug_sleep_in_irq.log`。1500000 波特率下串口丢了部分字符，
下面是整理后的版本：带偏移的那一行是原文，`msleep`、`handle_irq_event`、`el1_interrupt`、`cpuidle_enter_state`
这几个函数名在原文里被截断，是按上下文补全的）：

```
BUG: scheduling while atomic: swapper/0/0/0x00010000
Modules linked in: hello_irq(O) hello_gpio(O)
CPU: 0 PID: 0 Comm: swapper/0 Tainted: G           O       6.1.75 #4
Call trace:
 dump_backtrace / show_stack / dump_stack_lvl / dump_stack
 __schedule_bug
 __schedule
 schedule
 schedule_timeout
 msleep
 hello_irq_handler+0x7c/0x80 [hello_irq]
 __handle_irq_event_percpu
 handle_irq_event
 handle_edge_irq
 generic_handle_domain_irq
 rockchip_irq_demux
 generic_handle_domain_irq
 gic_handle_irq
 call_on_irq_stack
 do_interrupt_handler
 el1_interrupt / el1h_64_irq_handler / el1h_64_irq
 cpuidle_enter_state / cpuidle_enter
 do_idle
 cpu_startup_entry
 ... start_kernel
```
之后是无穷无尽的 `bad: scheduling from the idle thread!`，控制台被刷屏。

**分析**：

- `swapper/0`、PID 0：CPU0 的**空闲线程**。中断来时 CPU0 正空闲，中断"借用"的是 idle 线程
- `0x00010000` 是 preempt_count。位划分（`include/linux/preempt.h:32-39`）：bit 0-7 禁止抢占计数，
  bit 8-15 软中断计数，**bit 16-19 硬中断计数**，bit 20-23 NMI。`0x00010000` = 正在处理 1 层硬中断
- 检查在 `kernel/sched/core.c:5824` 的 `schedule_debug()`：`in_atomic_preempt_off()` 为真就调 `__schedule_bug`。
  这个检查**不依赖** `CONFIG_DEBUG_ATOMIC_SLEEP`，本内核没开那个选项也会报
- **调用栈从下往上读**就是发生顺序：idle 循环 → 中断入口 → GIC（`gic_handle_irq`）→ GPIO 控制器分发
  （`rockchip_irq_demux`）→ 两层 irq domain → 边沿触发流程（`handle_edge_irq`）→ `hello_irq_handler` → msleep → 调度器报错。
  这就是"从 GIC 到 handler 的中断路径"的实物证据
- 定位源码行：
  ```
  $ scripts/faddr2line hello_irq.ko hello_irq_handler+0x7c/0x80
  hello_irq_handler at .../04-irq-sleep/hello_irq.c:112        ← msleep(10); 那一行
  ```
- 刷屏分两个阶段（对比两份日志的调用栈）：
  - 前 10 来条 `bad: scheduling from the idle thread!` 的栈里还有 `hello_irq_handler`：msleep 内部反复调 schedule，
    idle 线程不能出运行队列（`dequeue_task_idle` 报错）
  - 之后的几百条（pstore 里 648 条，86s~105s）栈变成 `do_idle → schedule_idle → __schedule → dequeue_task_idle`，
    已经不在中断里了：msleep 超时返回、中断结束，但 idle 线程的状态被 msleep 改成了"睡眠"，
    此后 CPU0 每次进入空闲都报一次。CPU0 实际上废了，其他核还活着
- pstore 的控制台缓冲只有约 64KB，刷屏把最开头的 `BUG:` 行冲掉了；这次第一现场靠串口抓取保住

**修复**：不在硬中断里睡眠。需要睡眠/耗时的工作放到下半部：线程化中断（`request_threaded_irq`）、
workqueue；tasklet 也是原子上下文，同样不能睡（阶段 4 练习 ②）。

**教训**：
- 硬中断上下文借用的是"碰巧在这个 CPU 上的线程"，它可能是 idle、可能是任意用户进程；在里面睡眠，
  受害的是那个无关的线程，症状可以离真正的 bug 很远
- 读 oops：第一行（什么 BUG、哪个进程、preempt_count）→ 调用栈从下往上 → `faddr2line` 落到源码行
- 关键信息出现在第一屏，刷屏会把它冲掉：第一时间抓串口，或者让 pstore 缓冲区大一点

---

## 案例 11：杜邦线接上了却没有中断——针脚找错，带电碰针还把板子弄重启了

**背景**：阶段 4 需要把 GPIO3_B0 和 GPIO3_D4 短接。开始时没有 K7 的引脚图，凭感觉接线。

**现象**：
- B0 输出高电平，D4 一直读 `in lo`，中断计数 0
- 带电用线头去碰"3.3V"确认 D4 位置时，板子掉电重启（`/proc/uptime` 只有 26 秒）

**排查**：
- 先排除"B0 被接到了 GND 上"：`gpio-rockchip.c:194` 的 `rockchip_gpio_get` 读的是 `ext_port`（引脚实际电平），
  B0 设高后读回 1，说明没被外部拉低
- 依次把其他 40pin GPIO（GPIO0_A5、GPIO3_D5、GPIO3_A4、GPIO3_D0）设高，D4 都不跟着变：线头不在这些 GPIO 上
- 网上搜不到 K7 的引脚图（搜到的都是 Radxa 等别的板子，不能用）；在本机找到厂商资料
  `~/openvela-rk3576/docs/KICKPI-K7（规格书+原理图+机械图）/KICKPI-K7规格书.pdf`，`pdftotext` 后得到 40pin 定义：
  **GPIO3_D4 = 23 号，GPIO3_B0 = 33 号**；33 号紧挨着 34 号 GND，35/37 是调试串口
- 把 D4 一头接 3.3V（输入脚，安全）：`in hi`、中断计数 +1，确认 23 号找对了；再改接 33 号，翻转测试通过

**根因**：一开始的针脚不对；带电挪线时线头大概率搭到了 GND 或相邻针，瞬间短路导致复位。

**教训**：
- 硬件连接以**厂商规格书/原理图**为准，不要用别的板子的图，也不要凭印象数针
- **断电接线，接好再上电**；手边有 1kΩ 电阻就串在线里，接错也只是几毫安的电流
- 用软件先"测量"：读引脚实际电平、逐个翻转已知引脚看谁跟着变，比直接上手瞎试安全得多

---

## 案例 12：开了方波，hello_race 却收不到中断——D4 被接在了 3.3V 上

**背景**：阶段 5 练习 ①，第二轮测试：hello_gpio 用 hrtimer 在 B0 上输出 5kHz 方波，经杜邦线送到 D4，hello_race 统计中断。

**现象**：`test_race` 跑 5 秒，`seq: 1 -> 1 (0 interrupts)`；`/proc/interrupts` 里 hello_race 那一行只有 1 次。

**排查**（按信号路径逐段查，先观察，不动线）：

| 段 | 命令 | 结果 |
|---|---|---|
| hrtimer 在跑吗 | `cat .../hello-gpio/half_period_us` | `100` ✅ |
| B0 在翻转吗 | 连续几次 `grep gpio-104 /sys/kernel/debug/gpio` | `lo`、`lo`、`lo`、`hi` ✅ 在变 |
| D4 跟着变吗 | 同一条命令里的 `gpio-124` | **一直 `in hi`**，B0 是 lo 时也是 hi ❌ |

**根因**：D4 配了下拉（`hello_irq_pin` 用 `pcfg_pull_down`），如果线掉了、引脚悬空，应该读到 `lo`。
一直读到 `hi`，说明 D4 接在了一个恒为高电平的地方——上次用 3.3V 确认 D4 位置后，那一头没改插到 33 号，
或者插到了紧挨着 23 号的 **25 号（VCC_3V3）**。唯一的那次中断是插线瞬间的上升沿。

**处理**：当时没法动板子，改用 `use_timer=1`（模块内 hrtimer 当写方，同样是硬中断上下文）完成实验；接线留待下次断电检查。

**教训**：
- 读到的电平本身就是线索：有下拉还读到恒高 → 接在了高电平上；恒低 → 可能是线掉了或接在了 GND
- 先观察、后动手：先拔插线，现象就没了，也就不知道原因
- debugfs 里 gpio-124 的使用者显示为 `irq`（con_id），因为没调 `gpiod_set_consumer_name`

---

## 案例 13：git worktree 检出的内核编不过——.gitignore 把源文件也忽略了

**背景**：阶段 5 lockdep 练习要一个调试版内核，为了不动日常用的内核树，用
`git worktree add ~/rk3576-kernel-debug HEAD` 另开一棵树来编译。

**现象**：几秒钟就编译失败：
```
arch/arm64/kernel/vdso/vdso.lds.S:14:10: fatal error: asm-generic/vmlinux.lds.h: No such file or directory
```
原来的 `~/rk3576-kernel` 一直能正常编译。

**排查**：
- 原树里文件存在：`ls include/asm-generic/vmlinux.lds.h` ✅
- `git ls-files include/asm-generic/vmlinux.lds.h` 没有输出：**这个文件从来没有被提交过**
- `git check-ignore -v include/asm-generic/vmlinux.lds.h` → `.gitignore:18:vmlinux.*`
- 列出所有被忽略的文件，排除编译产物（`.o`、`.cmd`、`.dtb`、生成的表和主机工具……），
  剩下的真正源文件全是 `vmlinux.*`：各架构的 `vmlinux.lds.S`、`vmlinux.lds.h`、`vmlinux.scr` 等，共 37 个

**根因**：厂商 `.gitignore` 写的是 `vmlinux.*`，没有限定路径，匹配了**所有目录**下以 `vmlinux.` 开头的文件；
上游写的是 `/vmlinux`、`/vmlinux.map` 这类只匹配根目录的规则。第一次"导入原厂源码"的提交因此漏掉了这些源文件。
本地目录里文件一直都在，所以日常编译从来没暴露过问题——**只有从 git 重新检出时才会出现**。

**修复**：`.gitignore` 第 18 行改成 `/vmlinux.*`，再把放出来的 37 个源文件补提交（内核仓库 `ba2920abf`）。
改完后核对新冒出来的未跟踪文件，确认全是源文件、没有误放出编译产物（`arch/arm64/kernel/vmlinux.lds` 由该目录自己的 .gitignore 管）。
调试版那棵树当时直接从原树复制了这些文件，先把内核编出来。

**教训**：
- "本地能编"不等于"仓库是完整的"。导入大型第三方源码后，用 `git worktree` 或 clone 到别处**真的编译一次**，才能证明仓库能复现
- 排查"文件明明在却说找不到"：`git ls-files` 看是否被跟踪，`git check-ignore -v` 看被哪条规则忽略
- `.gitignore` 里不带 `/` 的模式会匹配任意层级的目录，写规则时要想清楚作用范围

---

## 案例 14：调试内核开机后 lockdep 已经是关的——厂商驱动先触发了报告

**背景**：阶段 5 lockdep 练习，刷了打开 `CONFIG_PROVE_LOCKING` 的调试内核。

**现象**：开机后 `grep debug_locks /proc/lockdep_stats` 是 **0**，dmesg 里已经有 1 份报告。此时做自己的实验什么都报不出来。

**排查**：lockdep 只报第一个问题，报完就把 `debug_locks` 置 0、停止检查。把 dmesg 存到 NFS 看第一份报告：
1. `rockchip_dmcfreq`（DDR 调频）：持有 `&info->dvfs_mutex` 去拿 `cpu_hotplug_lock`（`rockchip_dmcfreq_opp_set_rate` → `cpus_read_lock`），
   而 cpufreq 初始化那条链是 `cpu_hotplug_lock → ... → policy->rwsem → notifier rwsem → dvfs_mutex` → possible circular。
   调试内核关掉 `ARM_ROCKCHIP_DMC_DEVFREQ` 后重刷
2. 又是 0：`phy-rockchip-inno-usb2` 的 `rockchip_chg_detect_work` 持有 `&rport->mutex`，和 `(wq_completion)events` 成环。
   再关掉 `PHY_ROCKCHIP_INNO_USB2`
3. 第三次开机 `debug_locks: 1`、0 份报告

**根因**：厂商驱动本身有锁顺序问题，正常内核上从未表现（需要特定时序才会真的死锁）。

**处理**：只在调试内核里关掉这两个驱动（调试内核下 USB2 口、DDR 调频不可用，不影响练习；刷机的下载模式在 U-Boot 里，不受影响）。
原始报告存在 `05-concurrency/lockdep_vendor_dmcfreq.log`、`lockdep_vendor_usb2phy.log`。

**教训**：
- 用 lockdep 前先确认 `debug_locks` 是 1；开机就是 0 的话，要先处理掉已有的问题
- lockdep 报的是"可能"，正常运行没出过事不代表没有问题——这是厂商代码里两个真实的潜在死锁
- 刷机中途被打断、板子停在 rockusb 但 Windows 看不到下载设备：在 U-Boot（1500000）里 Ctrl-C，再执行 `rockusb 0 mmc 0`，设备会重新枚举

---

## 案例 15：摄像头时好时坏——三个口都读不到 ID，重启后又好了（原因未查明）

**现象**：阶段 6 开始时，`dmesg | grep imx415` 三个口（I2C4/5/8）都是 `Unexpected sensor id(000000), ret(-5)`，
`i2cdetect -y 8` 的 0x37 是 `--`。可摄像头项目 9-27 的记录是 cam3 `Detected imx415 id 0000e0`。
NFS 上存的开机日志显示 10-06 起（ramoops）、10-09 三次调试内核开机，cam3 都失败。
之后某次重启，cam3 又 `Detected imx415 id 0000e0`，`i2cdetect` 显示 `UU`。

**排查**：
- 我们改过的引脚（GPIO3_B0/D4、GPIO2_D6）和 cam3 的引脚（供电 GPIO0_C7、I2C8、MCLK GPIO4_A1）不重叠，排除设备树改动
- 摄像头项目的记录：IMX415 的 INCK 由 SoC 从 GPIO4_A1 送出，dtsi 却写成 `fixed-clock`，Linux 不会主动打开；
  没时钟 I2C 不应答，现象和"没接模组"一样。我据此推测"9-27 是 U-Boot 碰巧开着时钟"——**但随后同一内核、同一 U-Boot 又识别成功了，推测没被证实**
- 失败的几次都是刷机后脚本复位或 FIQ debugger 重启；成功那次的重启方式、有没有碰过排线，未确认

**结论**：原因未查明。候选：热复位后模组没真正断电复位、排线接触、INCK 时钟状态。留到摄像头专精阶段。

**教训**：
- 别急着把"时好时坏"归因到最近改过的东西；先确认同样条件下能不能稳定复现
- 推测要说清楚是推测，有反例出现就更正（这次我先给了时钟的解释，下一次开机就被推翻了）
- 读芯片 ID 失败要分清：地址/接线、供电、时钟、复位状态都会表现为 NACK

---

## 案例 16：UART 回环 echo 报 "Connection timed out"——baudclk 默认只有 187500 Hz

**现象**：阶段 7 hello_uart 第一次上板，`echo hello > /dev/hello_uart` 报 `write error: Connection timed out`，`cat` 什么也读不到。
dmesg：`hello_uart: baudclk 187500 Hz, divisor 0`。

**原因**：
- `sclk_uart5` 是"选父时钟 + 8 位分频"（`clk-rk3576.c` 的 `COMPOSITE(SCLK_UART5, ...)`），uart5 以前没人用过，
  187500 = 48MHz ÷ 256，正好是分频器最大值，大概率是复位默认值
- divisor = 187500 / (16 × 115200) ≈ 0.1 → 四舍五入为 0 → 波特率发生器停转，发送端一个比特都移不出去
- 第 1 个字节写进去（THRE 起初是 1），第 2 个字节等 THRE 超时 → write 返回 1；echo 再写剩下的，第一个就超时 → `-ETIMEDOUT`

**修复**：hw_init 里先 `clk_set_rate(baudclk, 16 * baud)`，再 `clk_get_rate` 读实际值；divisor 为 0 时打印错误、返回 -EINVAL。
之后 `baudclk 1843200 Hz, divisor 1`，回环读回 hello。原厂 `8250_dw.c` 设置波特率时也是 `clk_set_rate(d->clk, 波特率 × 16)`。

**附带现象：下一次加载第一次 cat 读到 "hhello"**——多出的 `h` 是 divisor=0 那次卡在发送移位寄存器里的字节。
rmmod 只关时钟、不复位硬件，状态被"冻住"；再次 insmod，写 DLL=1 后波特率发生器恢复，`h` 在 FCR 清空 FIFO 【之后】才回环进接收 FIFO
（写寄存器几十纳秒，发一个字节 87 微秒）。正常使用后再 rmmod/insmod 不会出现——只有字节卡住过才会。

**教训**：
- 时钟打开了 ≠ 频率是对的；驱动要主动 `clk_set_rate` 自己需要的频率，设完再读回实际值
- 初始化时能发现的错误就在初始化时报错（divisor=0 检查），别拖到使用时变成莫名其妙的超时
- 打印关键中间值（时钟频率、divisor）——这次一眼就看出了问题
- 初始化不能假设硬件处于上电复位状态：前一个使用者（bootloader、上次加载、一次失败的使用）可能留下了状态。
  FCR 只清得了 FIFO，清不了移位寄存器里正在发的那个字节；要彻底干净，等 LSR.TEMT 再把 RX 读空
- write 里超时退出而不是死循环，echo 才能报错返回而不是卡死

---

## 案例 17：不开时钟访问 UART 寄存器——不 oops，读到的是"上一次读的残留值"

**实验**：`insmod hello_uart.ko skip_clk=1`，probe 里不开 `apb_pclk`/`baudclk` 就读写寄存器。
`/sys/kernel/debug/clk/clk_summary` 确认 `pclk_uart5`、`sclk_uart5` 的 enable_cnt 都是 0、硬件状态 `N`。

**结果**：

| 场景 | 读 LSR | 写 SCR=0x5a 再读 | 写 SCR=0xa5 再读 |
|---|---|---|---|
| 不开时钟（之前最后一次读的是 LSR=0x60） | 0x60 | 0x60 | 0x60 |
| 开时钟 | 0x60 | 0x5a | 0xa5 |
| 不开时钟（之前最后一次读的是 SCR=0xa5） | **0xa5** | 0xa5 | 0xa5 |

**结论**：在 RK3576 的 uart5 上，不开时钟时**写被悄悄丢掉，读到的是总线上上一次读操作留下的值**，跟读哪个寄存器无关。
不 oops、不卡死、不报错。残留值具体存在哪一级总线，软件看不出来。ROADMAP 预期的 oops 在这里没有出现（别的 SoC/外设可能 SError 或卡死）。

**排查过程**（这次的重点）：
1. 第一次 skip_clk 读 LSR 得到 0x60——是 LSR 的正常值，差点得出"关了时钟也能正常读"的结论
2. 推测"读通路不需要时钟，能读到触发器里的真实值" → 用 SCR（草稿寄存器，写什么读什么）验证 → 读到 0x60，跟 LSR 一样，**推测被推翻**
3. 新推测"读到的是上一次读的残留值" → 预测：正常加载最后一次读的是 SCR=0xa5，下一次 skip_clk 应该全读到 0xa5 → **应验**

**教训**：
- 静默失败比崩溃更危险：驱动以为配置成功，硬件什么都没做。"寄存器写了不生效"第一步查时钟（`clk_summary`）
- 读到"看起来对"的值不代表读成功了——要用能区分的实验（写一个特殊值再读回来）
- 提出假设后，设计一个能让它失败的实验；预测应验了才算数

---

## 案例 18：mmap 不检查范围——普通 mmap 读到了别人的物理页

**实验**：`insmod hello_mmap.ko no_size_check=1`（mmap 跳过范围检查），缓冲区 2 页（8192 字节），
`test_mmap 16384` 映射 4 页。

**结果**：mmap 成功（正常应 `Invalid argument`）；缓冲区后面两页几乎写满（3846/4096、3914/4096 个非零字节），开头是
`istrator/rk3576-kernel\0PIDTYPE_M` 和 `-endian -mgeneral-regs-only -mab`——PC 上的源码路径、内核枚举名、编译选项。
这三个字符串在 `hello_mmap.ko` 的调试信息（DWARF）里都有，`test_mmap` 里没有 → 很可能是 insmod 从 NFS 读 .ko 时用过的页。

**原因**：`remap_pfn_range` 不知道哪些页属于驱动，给它什么 pfn 就映射什么。范围检查是 mmap 函数里最重要的代码。

**写检查时踩过的坑**（review 出来的）：
- `vm_pgoff<<PAGE_SHIFT + size`：`+` 比 `<<` 先算 → `vm_pgoff << (12 + size)`，移位超 64 位，检查形同虚设
- `vm_pgoff + size`：页 + 字节，单位不一致；2 页缓冲区碰巧测不出，4 页缓冲区 mmap(8192, 偏移 3 页) 就放行了

**教训**：
- 移位和加减混用一律加括号；页和字节先统一单位
- 只读就泄露别人的数据；能写就能改 `struct cred` 提权。设备节点开放给普通用户（0666）时就是真实漏洞（Android 上出现过）
- 检查要覆盖起点和终点；不要依赖调用者（do_mmap）替你检查

## 案例 19：rmmod 本应失败却成功了——后台进程的 stdin 是 /dev/null

**现象**：`test_mmap &` 后台运行（停在 `getchar()` 等回车），立刻 `rmmod hello_mmap` 却成功了，跟"映射存在时卸载不掉"的预期不符。

**原因**：串口控制台上的 busybox shell 没开作业控制，后台命令的 stdin 被重定向到 /dev/null，
`getchar()` 立刻读到 EOF → munmap → 退出（之后没有打印，看不出已退出）。rmmod 时映射已经不在了。

**验证**：改成 `sleep 1000 | test_mmap &`（stdin 是不会结束的管道），`ps` 确认进程还在，
rmmod 报 `Resource temporarily unavailable`（-EWOULDBLOCK = EAGAIN）。

**引用链**：open → `misc.c:113 fops_get` → `try_module_get`（模块引用 +1）；mmap → `mm/mmap.c:2762 vma->vm_file = get_file(file)`；
即使 close(fd)，vma 还持有 file → 模块引用不掉 → `kernel/module/main.c` `try_stop_module` 返回 -EWOULDBLOCK。munmap/进程退出后才能卸载。

**教训**：实验结果跟预期不符时，先确认实验前提（进程真的还在吗？），再怀疑理论。

---

## 案例 20：DMA 发送"超时"、随后 cat 卡死——其实 DMA 成功了，是 write 持锁返回

**现象**（阶段 8 练习 ②，hello_uart_dma）：
1. 第一版：`head -c 64 ... > /dev/hello_uart_dma` 卡住不返回，Ctrl-C 才退出
2. 第二版：write 报 `Connection timed out`，dmac1 中断计数 0 → 1；接着 `cat /dev/hello_uart_dma` 卡死，Ctrl-C 无效

**排查**：
- 第一版：板子上跑的 .ko 比源码旧（`ls -la --time-style` 对比 .c / .ko / NFS 上的 .ko 时间），那版 write 是 `return 0`。
  write 返回 0 = "一个字节都没写进去"，用户态写函数会重试 → 每次都用 DMA 发一遍、再返回 0 → 死循环
- 第二版：FIQ debugger 抓现场
  - `sysrq w`：`cat state:D ... mutex_lock ← hello_uart_read_fop` —— cat 在等锁
  - `sysrq l` / `pc`：所有 CPU 都在 `cpuidle_enter_state` —— 排除总线卡死（我先猜的是总线卡死，被现场推翻）
  - 代码：`ret = hello_uart_tx_dma(hdev, n); if (ret) return -ETIMEDOUT; mutex_unlock(...)`
    tx_dma 的约定是成功返回 n（非 0），所以**成功也走进 if**：报假超时，并且持锁返回。中断 0 → 1 正是 DMA 完成的中断

**修复**：`ret = hello_uart_tx_dma(hdev, n); mutex_unlock(&hdev->lock); return ret;`

**教训**：
- write 返回 0 会让用户态无限重试；返回值是写给调用者看的汇报
- 混用两种返回值约定（"0 成功" vs "n 成功"）就会出这种错；调用者按被调函数的约定用返回值，最好原样传递
- 加锁和解锁之间的每条 return 都要解锁
- 卡住时先 `sysrq w`（谁在等什么）+ `sysrq l`（CPU 在忙还是在等），再下结论
- 改完代码确认重新 make、重新拷贝：对比 .c / .ko / NFS 上 .ko 的时间

## 案例 21：/dev 下残留的普通文件挡住了设备节点——除了中断计数，一切"正常"

**现象**：重启后忘了 insmod 就执行 `head -c 64 ... > /dev/hello_uart_dma`，`ret=0`，cat 读回 64 个 A，但 dmac1 中断计数一直是 0。
之后 insmod 了，dmesg 照样 `registered /dev/hello_uart_dma`，可中断计数还是 0。

**原因**：
- 节点不存在时，shell 的 `>` 在 /dev（devtmpfs，可以建普通文件）下**新建了一个普通文件**，读写都进了这个文件
- insmod 后 `misc_register` → `device_add` → `devtmpfs_create_node(dev)`：`handle_create` 里 `kern_path_create` 返回 -EEXIST，
  但 `drivers/base/core.c:3635` **不检查返回值**——设备注册成功和 /dev 有没有节点是两回事（节点可能由 devtmpfs/udev/mdev 建，也可以手动 mknod）
- 卸载时 devtmpfs 只删自己建的节点（`i_private = &thread` 标记），所以那个普通文件一直留着

**确认**：`ls -l /dev/hello_uart_dma` 第一个字符是 `-` 不是 `c`；`cat /sys/class/misc/hello_uart_dma/dev`（10:120）是驱动实际注册的设备号。
**修复**：rmmod → `rm /dev/hello_uart_dma` → insmod → `ls -l` 确认 `crw-------`。

**教训**：
- 往设备节点读写前先 `ls -l` 确认是 `c`，尤其是重启后
- insmod 成功、dmesg 正常、write 返回 0、读回数据都是假象；**要有一个只有功能真正工作才会变化的独立指标**（这里是 DMA 中断计数）

---

## 案例 22：memset(&t, 0, xfer_len)——清错了对象，长度一大就踩坏内核栈（review 发现）

**代码**（阶段 9 hello_spi 的 loopback_show，写的时候想清 rx 缓冲区）：
```c
struct spi_transfer t;          // 栈上，一百多字节
memset(&t, 0, xfer_len);        // 长度用的是 rx 缓冲区的长度
t.rx_buf = hdev->tx;            // 同时还把接收指到了发送缓冲区
```

**后果**：
- xfer_len=16：只清了 t 的前 16 字节，speed_hz / bits_per_word / delay / cs_change 等成员是栈上的垃圾值，传输行为不可预测
- xfer_len=256：清零范围超出 t，把栈上相邻的局部变量、返回地址清成 0 → 破坏内核栈（KASAN 会报 stack-out-of-bounds；本板 KASAN 内核放不下，没能演示）
- rx_buf 指向 tx：收到的数据覆盖 tx，rx 永远是全 0，接了线也 MISMATCH

**修复**：`struct spi_transfer t = {};`（声明时整体清零，没赋值的成员都是 0）+ `memset(hdev->rx, 0, xfer_len);` + `t.rx_buf = hdev->rx;`

**教训**：
- 结构体用 `= {}` 初始化；memset 的长度和第一个参数对应，习惯写 `memset(&x, 0, sizeof(x))`
- 编译器和 checkpatch 都发现不了"清错对象"；长度可调的参数（xfer_len）要想一想它最大时会怎样
