#define pr_fmt(fmt) "cpu: " fmt

#include <nuvix/cpu.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>
#include <nuvix/sched.h>
#include <nuvix/page.h>
#include <nuvix/layout.h>
#include <nuvix/tick.h>

enum cpu_boot_state {
    CPU_BOOT_WAITING,
    CPU_BOOT_RUNNING,
    CPU_BOOT_FAILED,
};

#define IPI_RESCHED 1

static atomic_t boot_state = ATOMIC_INIT(CPU_BOOT_WAITING);
static struct task idle_tasks[CONFIG_NR_CPUS];

static bool cpu_boot_running(void)
{
    return atomic_read_acquire(&boot_state) == CPU_BOOT_RUNNING;
}

static bool cpu_boot_failed(void)
{
    return atomic_read_acquire(&boot_state) == CPU_BOOT_FAILED;
}

void cpu_boot_fail(void)
{
    (void)atomic_cmpxchg_release(&boot_state, CPU_BOOT_WAITING,
                                 CPU_BOOT_FAILED);
}

static int prepare_idle_tasks(void)
{
    for (unsigned int cpu_id = 1; cpu_id < nr_cpus; ++cpu_id) {
        struct task *idle = &idle_tasks[cpu_id];
        struct page *stack __free(kstack) = alloc_pages(KSTACK_ORDER);

        if (!stack)
            return -ENOMEM;
        refcount_set(&idle->refcount, 1);
        INIT_LIST_HEAD(&idle->rq_node);
        INIT_LIST_HEAD(&idle->children);
        INIT_LIST_HEAD(&idle->sibling);
        idle->kstack = (void *)__va(page_to_phys(move(stack)));
        idle->preempt_count = INIT_PREEMPT_COUNT;
        sched_init_idle(cpu_id, idle);
    }
    return 0;
}

static void cpu_local_ready(void)
{
    struct cpu *cpu = &cpus[current_cpu_id()];
    int ret;

    if (cpu_boot_failed())
        halt();
    ret = cpu_local_init();
    if (ret) {
        cpu_state_set(cpu, CPU_FAILED);
        panic("CPU%u: SBI TIME setup failed (%d)\n", current_cpu_id(),
              (int)ret);
    }
    /* A timeout may have marked this CPU failed while it initialized. */
    if (cmpxchg_release(&cpu->state, CPU_STARTING, CPU_READY) != CPU_STARTING)
        halt();
}

int smp_init(void)
{
    uint64_t start, timeout;
    int ret;

    ret = prepare_idle_tasks();
    if (ret) {
        cpu_boot_fail();
        return ret;
    }
    cpu_state_set(&cpus[0], CPU_STARTING);
    cpu_local_ready();
    start = time_read();
    timeout = time_ticks_from_ms(CONFIG_SMP_BOOT_TIMEOUT_MS);
    for (unsigned int cpu_id = 1; cpu_id < nr_cpus; ++cpu_id) {
        long error;

        if (cpu_boot_failed())
            return -EIO;
        cpu_state_set(&cpus[cpu_id], CPU_STARTING);
        error = arch_start_cpu(cpu_id, cpu_rq(cpu_id)->idle);
        if (error) {
            cpu_state_set(&cpus[cpu_id], CPU_FAILED);
            cpu_boot_fail();
            pr_err("CPU%u: SBI HSM start failed (%d)\n", cpu_id, (int)error);
            return -EIO;
        }
    }

    for (unsigned int cpu_id = 1; cpu_id < nr_cpus; ++cpu_id) {
        while (cpu_state_read(&cpus[cpu_id]) != CPU_READY) {
            if (cpu_boot_failed() ||
                cpu_state_read(&cpus[cpu_id]) == CPU_FAILED) {
                cpu_boot_fail();
                return -EIO;
            }
            if (time_read() - start >= timeout) {
                (void)cmpxchg(&cpus[cpu_id].state, CPU_STARTING, CPU_FAILED);
                cpu_boot_fail();
                pr_err("CPU%u: startup timed out\n", cpu_id);
                return -ETIMEDOUT;
            }
        }
    }
    if (atomic_cmpxchg_release(&boot_state, CPU_BOOT_WAITING,
                               CPU_BOOT_RUNNING) != CPU_BOOT_WAITING)
        return -EIO;
    pr_info("all %u CPU(s) ready\n", nr_cpus);
    return 0;
}

__noreturn void smp_main(void)
{
    cpu_local_ready();
    while (!cpu_boot_running()) {
        if (cpu_boot_failed())
            halt();
    }
    sched_task();
}

void cpu_send_resched(unsigned int cpu_id)
{
    long error;

    (void)atomic_fetch_or_release(IPI_RESCHED, &cpus[cpu_id].ipi_pending);
    error = arch_send_ipi(cpu_id);
    if (error)
        panic("CPU%u: SBI IPI failed (%d)\n", cpu_id, (int)error);
}

bool cpu_handle_ipi(void)
{
    struct cpu *cpu = &cpus[current_cpu_id()];

    /* Ack first: a later send must leave its hardware notification pending. */
    arch_clear_ipi();
    return atomic_xchg_acquire(&cpu->ipi_pending, 0) & IPI_RESCHED;
}
