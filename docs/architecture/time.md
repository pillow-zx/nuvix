# 时间架构

## 时钟来源

RISC-V `time` CSR 是 cuteOS 的原始单调时钟。`timer_now()` 返回 QEMU
`virt` 的 10 MHz mtime tick；内核 timer、调度 tick、相对 sleep 和 timeout
只以这个时钟计算期限。

`CLOCK_MONOTONIC` 与 `CLOCK_BOOTTIME` 直接转换 mtime。当前平台没有 suspend
模型，因此两者相同；它们不会因设置 wall clock 而跳变。

## CLOCK_REALTIME

`kernel/time.c` 拥有全局 `CLOCK_REALTIME` offset。读取 realtime 时，内核在
mtime 转换结果上加上这个 offset；写入时用请求值减去当前 mtime 重新计算它。

- 只有 UID 0 可通过 `clock_settime(CLOCK_REALTIME)` 写入。
- `tv_sec >= 0`、`0 <= tv_nsec < 1e9` 是必要条件。
- 请求值不能小于当前 `CLOCK_MONOTONIC`，否则返回 `-EINVAL`。
- offset 仅存在于内存中，启动时为零，重启后丢失。系统没有 RTC、NTP、频率调整
  或持久化策略。

`clock_gettime(CLOCK_REALTIME)`、`gettimeofday` 和 VFS 自动设置的 inode
atime/mtime/ctime 都经由这个接口读取时间。显式 `utimensat` 时间戳仍按调用者
提供的值写入。

## Timer 边界

相对等待和 interval timer 始终以 mtime 计时，因而不受 wall-clock 写入影响。
cuteOS 尚未实现 `clock_settime` 后重排已注册 wall-clock absolute deadline 的
机制。因此 `clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME)` 与
`timer_settime` 的 `CLOCK_REALTIME + TIMER_ABSTIME` 组合返回 `-EINVAL`；
`FUTEX_CLOCK_REALTIME` 返回 `-ENOSYS`。它们只能在具有 wake/recompute 和
timer 重排契约后启用。

`hwclock` 与 `rtcwake` 继续禁用，直到存在 RTC 设备及匹配 ioctl。

## Kernel timer lifetime

`arch/riscv/timer.c` 只负责 `time` CSR、`stimecmp` 和架构到通用 timer 的
适配；queued timer、callback 状态和 interval 重排由 `kernel/time.c` 拥有。
ktimer queue 持有 `active` 状态，callback 从 queue 摘下后在 queue lock 外执行，
并由 `callback_running` 标记 callback completion。周期 timer 在 callback 前先
重新插入，因此 callback 可以安全地观察或取消自己的下一次 queued delivery。

```c
void ktimer_init(struct ktimer *timer, ktimer_fn_t function, void *arg);
int ktimer_arm(struct ktimer *timer, uint64_t expires, uint64_t interval);
bool ktimer_cancel(struct ktimer *timer);
int ktimer_cancel_sync(struct ktimer *timer);
```

`ktimer_cancel()` 是非阻塞的 queued cancel；它不等待已经开始的 callback。
`ktimer_cancel_sync()` 还等待 callback completion，成功返回后 owner 可以释放
callback 使用的 session、task 或参数。等待 callback 的 task 会在 timer 的
completion waiter list 中登记一个 task reference；callback 清除
`callback_running` 后摘除并唤醒这些 waiter，再由 waiter 释放自己的 reference。
它要求 non-idle task context、无 hard IRQ、无 held spinlock 且 `preempt_count == 0`；
允许 IRQ-off task handoff，并恢复调用者进入时的 IRQ 状态。callback self-cancel 返回
`-EDEADLK`，非法上下文返回 `-EINVAL`。callback 本身不能睡眠、分配或执行同步取消。

wait deadline 使用 heap 上 stable wait session 内嵌的 wait timer。session 在
`ktimer_cancel_sync()` 完成前保持 timer/session/task 所需引用，因此 wait cleanup
不会与 timeout callback 并发访问已释放的调用者栈对象。

## Task-targeted timers

`ITIMER_REAL` 和 POSIX timer 在安装目标 task 时持有 task lifecycle reference；
callback 投递信号时再取得临时 reference，完成后释放。POSIX timer table 的 slot
ownership 与 set/delete/clear 的 in-flight operation reference 分开；lookup 后释放
table lock 的操作必须持有 operation reference，slot detach 后禁止新的 lookup，直到
cancel-sync 完成才允许释放 timer storage。timer delete、exec 清理和 signal state
销毁先摘除用户可见 slot，再执行 cancel-sync，最后释放目标 reference 和两类 timer
reference。`ITIMER_REAL` 的 get/set/destroy 通过独立 operation lock 串行化，set
在取消旧 delivery 后重新采样 mtime，避免同步 callback cleanup 期间经过的时间被
从新相对期限中遗漏。
