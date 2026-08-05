# cuteOS

cuteOS 是一个面向实验和内核机制验证的 Unix-like 宏内核，当前运行在
QEMU `virt` 的 RISC-V 64 平台上，经 OpenSBI 启动。项目以运行真实的静态
riscv64 ELF 程序为验证目标，并以 Linux riscv64 ABI 作为已声明用户可见语义
的兼容边界。

cuteOS 的长期目标不是实现一个微内核，也不是复制 Linux 的全部功能，而是：

- 保持宏内核定位，提供完整、可复用、可验证的内核机制；
- 每类策略只提供一套默认实现，保证基本运行需求和实现可理解性；
- 在机制和策略之间建立稳定、足够深入的接口，使新策略可以较低成本接入；
- 通过 SMP、多核、内核抢占和多架构目标验证通用机制，而不是让单核实现成为
  永久前提；
- 优先保证 ABI 正确性、子系统边界、并发语义和可调试性。

这里的“完整机制”指内核内部的状态机、同步、生命周期、资源管理、地址空间、
I/O 和 ABI 契约应当闭合，并不意味着当前项目承诺完整 Linux 兼容或生产级设备
覆盖。

## 设计模型

项目把内核分为五个相互约束的层次：

| 层次 | 内容 | 例子 |
| --- | --- | --- |
| 硬件机制 | ISA、异常、IRQ、timer、IPI、页表和内存序 | RISC-V trap、`sfence.vma` |
| 通用内核机制 | 生命周期、同步、状态机和共享服务 | task、wait queue、VFS、page cache |
| 策略接缝 | 可替换行为的稳定接口及其不变量 | scheduler、block dispatch |
| 默认策略 | 当前系统实际使用的一套选择规则和参数 | FIFO RR、当前分配与写回方案 |
| ABI/用户语义 | 用户程序可观察的布局、错误和时序 | syscall、signal frame、ELF |

机制层不应知道默认策略的内部字段；策略层不应绕过通用对象的生命周期和同步
契约；syscall 层只做 ABI 适配，不能因为入口位于 `syscall/` 就拥有文件系统、
内存或调度语义。

## 当前状态

当前实现是单核、内核不可抢占的 RISC-V 64 内核。未来目标与当前状态明确区分：

| 领域 | 当前状态 | 长期目标 |
| --- | --- | --- |
| CPU | 只启动 hart 0，从核启动阶段 park | SMP 多核、per-CPU 状态、IPI 和远程唤醒 |
| 调度 | per-CPU FIFO RR（固定时间片），用户返回点可按 timer 请求切换 | 通用调度机制加可替换策略，支持迁移和负载管理 |
| 内核抢占 | 未实现；内核路径不可抢占 | 完整的 preempt/IRQ/sleep 契约和内核抢占 |
| 内存 | Sv39、固定 VMA 数组、延迟缺页、复制式 fork | 共享 mm 并发、uaccess、COW、TLB shootdown 和可扩展 MM 机制 |
| 存储 | VFS、ext2、page cache、virtio-blk，主要为轮询 I/O | 并发安全的文件系统、页缓存、请求生命周期和 I/O 策略接缝 |
| 架构 | RISC-V 64 与 QEMU `virt` | architecture、platform、user ABI 分层，并验证第二架构 |

SMP、内核抢占和多架构不是可有可无的附加功能。虽然当前基线还没有实现它们，
新代码不能继续依赖“关闭本地中断等于全局互斥”“CPU 0 等于当前 CPU”或“全局
静态请求天然串行”等单核假设。

## 已有能力与边界

当前系统具备以下主要能力：

- OpenSBI 启动、QEMU `virt`、RISC-V `rv64gc`、Sv39、S-mode 和高半区内核映射；
- 静态、非 PIE、soft-float `lp64` ELF64 RISC-V 用户程序；
- 构建时生成的 ext2 根文件系统，VFS 通过 virtio-blk 接入；
- 进程和线程的部分 clone/fork、exec、exit、wait4、PID 和任务生命周期语义；
- 信号、信号帧、sigreturn、futex、rseq、定时器和时间接口的已声明子集；
- 匿名和文件映射、缺页处理、uaccess、poll/epoll 兼容入口；
- 文件、fdtable、路径、挂载、dentry、inode、pipe、page cache 和 ext2 操作；
- 静态 musl BusyBox 用户态，以及独立的用户态回归测试 rootfs；
- Linux 数值 errno、Linux riscv64 syscall 号和已声明 ABI 布局。

入口数量不等于完整 Linux 兼容。当前 syscall 的成熟度、最小语义、明确失败和
后续优先级以 [SYSCALL.md](SYSCALL.md) 为准。架构文档记录实现边界，代码头文件
记录精确的类型和接口。

以下能力目前不属于运行时承诺，除非路线后续明确启用：

- SMP、多核并发、内核抢占、IPI 和 TLB shootdown；
- 动态链接、`PT_INTERP`、PIE、用户 FPU/vector 上下文；
- 完整 COW fork、swap、reclaim/OOM 和生产级内存压力处理；
- 完整 Linux capability、namespace、cgroup 和安全模块；
- 广泛设备、设备树/ACPI 平台发现、DMA/IOMMU、NUMA 和生产级块调度；
- 完整实时/截止期调度、PI futex、procfs/sysfs 和全部 Linux 边缘语义。

这些边界不应被误解为可以跳过 SMP、抢占所需的基础机制。真实 spinlock、内存
序、per-CPU 状态、任务所有权、wait/wakeup、uaccess 生命周期、页表一致性、
timer callback 和设备请求生命周期属于确定目标的一部分。

## 快速开始

需要 RISC-V GCC 15+、QEMU 7.2+、`e2fsprogs`、`bc` 和 Zig 0.16+。Zig 用于
生成严格 `lp64` 的 compiler-rt builtins。可通过 `TOOLPREFIX=<prefix>` 指定
交叉工具链。

```sh
make defconfig
make qemu
```

用户态固定为静态 musl BusyBox：`/sbin/init` 作为 PID 1，读取
`/etc/inittab`，启动并监管交互式 `/bin/sh`，并提供 `kill`、`halt`、`poweroff`
和 `reboot` 以驱动完整 init 关机链路。用户态 ISA 固定为
`rv64imac_zicsr_zifencei`，禁止生成 F/D 指令；浮点需求由 Zig compiler-rt 的
soft-float builtins 处理。动态链接、PIE 和用户 FPU 上下文不属于当前运行时路线。

QEMU 启动后进入串口 shell；使用 `Ctrl-a x` 退出。常用构建和验证命令：

| 命令 | 作用 |
| --- | --- |
| `make` | 构建内核 ELF |
| `make defconfig` | 使用默认配置重新生成 `.config` |
| `make menuconfig` | 修改配置 |
| `make user` | 构建用户态 ELF |
| `make user-rootfs` | 构建交互式用户态 rootfs |
| `make user-image` | 构建包含用户态 rootfs 的 ext2 镜像 |
| `make qemu` | 构建镜像并启动 QEMU |
| `make utest-build` | 构建用户态测试 ELF 和专用 rootfs 镜像 |
| `make utest` | 从测试 rootfs 启动用户态回归套件，验证真实存储栈 |
| `make qemu-gdb` | 启动带 GDB stub 的暂停 QEMU |
| `make analyze` | 运行 GCC analyzer 和额外诊断 |
| `make clean` | 删除构建产物 |

当前默认配置使用一个 QEMU CPU。SMP 目标完成前，不要仅通过增加 QEMU CPU 数量
就把当前内核当成多核安全实现。

### 测试边界

内核不再内置 kernel self-test。ext2 磁盘格式、路径树、挂载以及驱动与文件
系统的底层集成由 `make utest` 中的用户程序验证，普通 `make qemu` 使用 ext2
rootfs 和 virtio-blk 启动。module interface 的 host 单元测试计划在不启动内核
的前提下运行。

## 代码与文档导航

- [SYSCALL.md](SYSCALL.md)：syscall 成熟度、语义边界、已知缺口和优先级。
- [AGENTS.md](AGENTS.md)：贡献、ABI、构建、测试和自动化修改规则。
- `include/kernel/`：通用内核接口、对象契约和跨子系统类型。
- `include/uapi/`：用户可见的 syscall、结构、常量和 ABI 定义。
- `scripts/filelist.mk`：内核源文件对象清单；新增源文件必须同步更新。

修改用户可见 ABI 时，必须同时检查 Linux riscv64/asm-generic 头文件、
`include/uapi/`、用户态镜像和相关测试。修改跨子系统行为时，先确认对象所有权、
锁序、IRQ/preempt 状态、睡眠规则、引用生命周期和唤醒条件，再修改调用者。
