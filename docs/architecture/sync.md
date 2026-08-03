# cuteOS 同步与上下文契约

本文是阶段 1 的同步资料，按 `PLAN.md` §8.1 固化共享对象的第一版契约。
它区分当前单 hart、不可抢占基线与后续目标：A/B 实现 CPU-local context
查询、preempt 计数保护和 held-spinlock 诊断，C 收敛 scheduler 的上下文
入口与 deferred reschedule 契约；wait 和 allocator 的策略入口仍待后续阶段。

## CPU-local context

`struct cpu` 中的 `current_task` 和 `preempt_count` 仍位于 `entry.S` 依赖的
偏移；新增状态追加在其后。当前可查询的事实如下：

| 查询 | 含义 | 与其他事实的关系 |
| --- | --- | --- |
| `in_irq()` / `irq_nesting()` | 当前 CPU 是否处于 hard IRQ handler 及其嵌套深度 | 不读取硬件 IRQ bit，也不修改 `preempt_count` |
| `irqs_disabled()` | 本地硬件 IRQ 是否关闭 | IRQ-off task 仍可是真正的 task context |
| `preempt_count()` | 显式禁抢占深度 | 不编码 IRQ nesting，也不由 spinlock 自动增加 |
| `spinlock_held()` / `held_lock_depth()` | 当前 CPU 是否持有 spinlock 及深度 | 不表示锁的 owner 或其他 CPU 的状态 |
| `in_task_context()` | `current` 非空、不是 idle、且不在 hard IRQ | IRQ-off 和 preempt-disabled 仍可返回 true |

preempt 计数的合法区间是 `0..INT32_MAX`。递减时必须已经大于零，递增时
不能从 `INT32_MAX` 溢出；setter 也拒绝负值。所有构建都执行这些断言。IRQ
nesting 保持现有的独立 overflow/underflow 断言。

## Spinlock interface

`spin_lock_irqsave()` 先保存并关闭本地 IRQ，再用原子 compare-exchange
acquire 竞争锁；只有原子 acquire 成功后才登记 held lock。第一次获得当前
CPU 的 held lock 时保存原始 IRQ 状态。release 使用 release exchange 将锁置空，
随后清理 held-lock 记录；只要仍有任意 held lock，就保持 IRQ-off，直到最后一把
锁释放后才恢复最外层 acquire 前的 IRQ 状态。这个顺序保证 IRQ handler 不会在
恢复 IRQ 后看到已经释放的锁记录，也使非 LIFO 解锁不破坏 IRQ 状态。

debug 构建在 acquire 前检查当前 CPU 是否已持有同一地址以及 16 项容量；
release 前检查指定地址是否属于当前 CPU。记录集合允许非 LIFO 解锁，移除
中间项后压缩剩余地址。非 debug 构建没有地址集合，因此 membership、递归
和容量诊断不可用，但深度和 any-held 查询仍可用。

spinlock 不自动修改 `preempt_count`。持有 spinlock 的路径不得直接
`schedule()`、进入 `wait_for()`、执行可睡眠分配、睡眠或启动可睡眠 I/O；
这类入口契约由 scheduler、wait 和 allocator 各自收敛，不能由 irqsave
隐式放宽。

## 第一版锁序

锁序只记录已经确认的通用关系，不宣称所有现有调用点已经完成审计：

| 外层状态/锁 | 内层锁或操作 | 规则 |
| --- | --- | --- |
| source lock | `wait_channel.lock` | 在 source lock 内完成条件检查、事件 latch 和 watch 登记；只允许这一方向 |
| source state | 释放 source lock -> wakeup | 先发布状态并解锁，再调用 `wait_channel_wake_one/all()` 或 scheduler wakeup |
| `wait_channel.lock` | scheduler wakeup | 必须先释放 channel lock，wakeup 只是条件可能变化的通知 |
| page-cache lock | allocator/free、writeback、block I/O | 禁止在 page-cache lock 内执行；失败回滚也移到锁外 |
| scheduler lock | VFS/page-cache lock | 后续 SMP 设计中避免反向嵌套；当前 scheduler 仍是单核基线 |

锁内禁止操作包括：调度、等待、可睡眠分配、free、未知 callback、阻塞 I/O、
reclaim 和 writeback。锁内只可更新由该锁线性化的状态，或取得一个已定义的
稳定引用。错误路径必须先释放锁，再做分配、释放、I/O 或通知。

## PLAN §8.1 共享对象契约

表中的“当前”是基线事实；“目标/缺口”是后续阶段必须闭合的接口承诺。

| 对象 | owner | lock / lock order | IRQ / preempt | sleep / allocation / I/O | lifetime | wakeup | error |
| --- | --- | --- | --- | --- | --- | --- | --- |
| task | task module 与当前 CPU；未来由 runqueue owner 管理 | task lifecycle、child/process 和 scheduler 状态分别线性化；source lock -> wait channel lock | 当前只允许 CPU0；task path 不应依赖 IRQ-off 等同于 owner | 退出、等待和调度路径不能持有 spinlock；资源清理由独立安全点完成 | 目标状态为 RUNNING/SLEEPING -> EXITING -> ZOMBIE -> DEAD；stack 释放前必须离开所有 CPU、runqueue、wait/callback | 状态改变后锁外唤醒，waiter 重新检查条件 | 失败保留可观察状态、撤销 registration/ref；不能释放 current 或仍被 callback 使用的 task |
| PID registry | PID module | `pid_lock` 保护 bitmap、published pointer 和 lookup ref；不与 child lock 反向嵌套 | 当前 mutex 可阻塞，不能从 hard IRQ 使用 | alloc/free/lookup 走 mutex；不在其内部执行未知 I/O | publish 后 lookup 获得 ref；unpublish 后等待 lookup ref 归还再释放 | PID attach/detach 本身不替代 task/wait wakeup | ENOSPC、重复 publish 和 stale lookup 必须保持 registry 一致 |
| mm | mm module；active CPU 记录使用者 | `mmap_lock` 与 PTE mutation/active-mm 状态；未来需定义 TLB lock/order | 当前单 hart、不可抢占；uaccess/缺页上下文仍需单独审计 | page fault 可能分配；不可在不允许的 spinlock 下 fault/reclaim/I/O | 释放前无 active user/kernel use；SMP 需 shootdown 完成 | fault/wait 的通知由调用者拥有，mm 本身不假定 wakeup 完成 | 映射失败回滚 VMA/PTE/ref；不能留下半发布页表 |
| page table page | MM/page-table module | PTE mutation 与 generation；未来按 mm/CPU 建立 shootdown 顺序 | 当前 arch helper 运行于单 hart；不能把本地 IRQ-off当成 SMP 排他 | 建表可能分配；不得在未知锁内 reclaim 或做阻塞 I/O | 所有目标 CPU 确认 shootdown 后才能回收 | shootdown completion 唤醒等待者，访问者需重新获取有效映射 | OOM/映射失败必须撤销已发布的中间页表和引用 |
| inode / dentry / file | VFS module | cache/object lock 与 fdtable、mount、page-cache 的顺序需固定；不绕过 VFS | 当前主要 task context；不能把持锁与 IRQ-off混用成通用 owner | lookup/open/read/write 的分配、阻塞 I/O 在允许的锁外路径执行 | active operation、cache ref、open-file ref 分开；摘 cache 前无 active operation | poll/readiness 在 source state 发布后锁外通知 | 失败保持 ref/offset/namespace 不变，按负 errno 返回并完成 rollback |
| page cache page | page-cache module | `page_cache_lock` 保护 identity/LRU/dirty 元数据；锁内禁止 allocator/free/I/O | 当前单 hart；未来 busy/writeback 必须定义 preempt/IRQ handoff | read-in、writeback、reclaim 在锁外；ALLOC_NOWAIT 与 ALLOC_SLEEPABLE 语义不能混用 | page ref、busy、mapping active ref、writeback 分开；非 busy 且无 writeback 才 eviction | busy/writeback 完成后锁外唤醒，访问者重新检查 page state | read/writeback/invalidate 失败保留可重试 error，不能重复发布或提前 eviction |
| timer | timer module | timer queue lock 保护 queued/running/cancelled 状态；callback 不在 queue lock 内执行 | IRQ callback 不得睡眠、分配或获取未知锁；task callback 另行标注 | arm/cancel 的分配与 wait 规则由 timer API 声明；callback I/O 必须移出锁 | `cancel-sync` 返回后不能仍访问 timer/session/task；普通 ref 不替代同步取消 | expiry 只通知条件变化；waiter/task 必须持引用再 wake | arm/cancel/expiry 错误保持队列和 callback 状态一致，可安全重试 |
| block request | block adapter；设备完成前 request context owner 明确 | request state/queue lock 与 page-cache/writeback 顺序固定；不暴露 virtio descriptor | 当前同步轮询；未来 IRQ completion 需定义 preempt/IRQ handoff | descriptor/buffer 提交后不能复用；阻塞 I/O、分配和错误回滚在锁外 | completed/cancelled 且 device 不再访问 descriptor/buffer 后才能释放 | completion 先发布 request result，再锁外唤醒 page/waiter | timeout、device error、cancel 必须只有一个 completion，保留可观察错误 |

### 当前缺口

- wait session、timer cancel-sync、exit/reaper 和 kernel stack 释放仍属于 F/G；
  当前 `wait_for()` 仍使用调用栈 session，但每个已注册 wait entry 已持有
  task lifecycle reference。
- allocator 尚未接受 `ALLOC_NOWAIT` / `ALLOC_SLEEPABLE` mode；page cache、VFS、
  scheduler 锁内分配/free/I/O 的全面审计属于 E/H。
- mm active CPU/TLB shootdown、SMP page cache busy/in-flight、block request
  异步 lifetime 和完整 lockdep 均未闭合。

## Wait、allocator 与 scheduler 的上下文补充

### Wait

`wait_for()` 的入口实际检查 non-idle task context、`preempt_count()==0` 和无
held spinlock。IRQ-off task 可以进入，但真正 WFI 前必须打开 IRQ，返回时恢复
原状态；不能仅以 `irqs_disabled()` 判断 handoff 合法。source lock 内完成
check + latch + watch，source lock 释放后才 wake。唤醒不表示条件成立，waiter
必须重新检查条件。每个成功注册的 wait entry 持有一个 task lifecycle reference；
它可能先被 wake 摘链，但只在 wait cleanup 时释放。当前 wait core 已提供
task-exit cancellation callback，futex adapter 使用它在 channel registration
清理后从 futex bucket 摘除 waiter 并释放 owner reference；这只闭合了 futex
特定的退出路径。stable session、timer cancel-sync 和通用 task exit
cancellation 仍属于 F/G。

### Allocator

目标接口显式传播 `ALLOC_NOWAIT` 或 `ALLOC_SLEEPABLE`：前者不得等待、reclaim、
writeback 或可睡眠 I/O；后者要求 task context、IRQ enabled、preempt enabled、
无 held spinlock。free API 虽没有 mode，也必须执行持锁和生命周期检查。当前
allocator 尚未改签 mode，因此本文件只冻结规则，不把普通 allocator 宣称为
hard-IRQ-safe。

### Scheduler

当前 scheduler 是单核四级 MLFQ，timer tick 通过 `sched_request()` 设置
need-resched，切换仍发生在显式安全点。唯一的立即调度入口 `schedule()` 要求
current、非 hard IRQ、无 held spinlock 和 `preempt_count == 0`；本地 IRQ 可以
enabled 或 disabled，且入口保持硬件状态。`preempt_enable()` 归零时只在 IRQ
enabled 的安全点消费 deferred request。
scheduler core 不能执行分配、等待、I/O、reclaim 或 reaping；后者由 idle loop
在调度器外执行。

## 上下文矩阵测试

`test_task_context_matrix()` 用可返回查询覆盖下表，包括
`wait_context_can_sleep()`；`test_spinlock_held_tracking()` 覆盖锁列，
`test_irq_nesting_context()` 覆盖 nesting/preempt 独立性。hard IRQ
行使用 synthetic `irq_enter()`，并分别设置硬件 IRQ bit，以确认两个事实不互相
替代。

| 测试输入 | `in_task_context()` | `in_irq()` | `irqs_disabled()` | `preempt_count()` | `spinlock_held()` |
| --- | --- | --- | --- | --- | --- |
| 普通 task、IRQ enabled、无锁 | true | false | false | 0 | false |
| 普通 task、IRQ disabled、无锁 | true | false | true | 0 | false |
| 普通 task、hard IRQ nesting | false | true | 独立查询；测试可为 false | 0 | false |
| 普通 task、preempt disabled | true | false | false | 1（或更大） | false |
| 普通 task、持 irqsave spinlock | true | false | true | 0 | true，depth=1 |
| current 为 idle | false | false | 任意 | 0 | false |
| current 为 NULL | false | false | 任意 | 0 | false |

runner 在每个 case 后统一检查 `held_lock_depth()==0`、`preempt_count()==0`
和 `irq_nesting()==0`；发现泄漏即停止后续 case，避免在被污染的 context 上
继续产生假结果。真正的 underflow/overflow、错误 unlock、递归锁、容量错误和
wait context 错误由独立 `make kpanic CASE=...` QEMU harness 触发，不并入
`make ci`。

## 验证与未闭合范围

已验证：普通 kernel build、debug-off kernel build、kernel self-test，以及
`wait-held-lock`、`wait-preempt-disabled`、`wait-hard-irq` 三个 wait context
panic case。阶段 1 整体仍未完成；secondary hart、内核抢占、stable wait
session、allocator mode、task reaper、page-cache/block 异步 lifetime、完整
lockdep 以及 F/G/H/I-2 仍按 `TODO.md` 保持未完成。
