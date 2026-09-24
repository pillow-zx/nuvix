#include <arch/plic.h>
#include <arch/pgtable.h>
#include <nuvix/errno.h>
#include <nuvix/irq.h>
#include <nuvix/slab.h>
#include <nuvix/spinlock.h>
#include <nuvix/tools.h>
#include <nuvix/wait.h>

#define PLIC_PRIORITY       0x000000UL
#define PLIC_PENDING        0x001000UL
#define PLIC_ENABLE         0x002000UL
#define PLIC_ENABLE_STRIDE  0x80UL
#define PLIC_CONTEXT        0x200000UL
#define PLIC_CONTEXT_STRIDE 0x1000UL
#define PLIC_THRESHOLD      0UL
#define PLIC_CLAIM          4UL

struct plic_source {
	irq_handler_t handler;
	void *data;
	uint32_t cpu;
	bool enabled;
	bool active;
	bool removing;
};

struct plic_cpu {
	vaddr_t enable;
	vaddr_t context;
	bool ready;
};

struct plic_controller {
	vaddr_t base;
	uint32_t ndev;
	struct plic_source *sources;
	struct plic_cpu contexts[NR_CPUS];
	/* Serializes claim/publication, registration lifetime and enable-word
	 * RMW. Never held across a device callback or a blocking wait. */
	spinlock_t lock;
	struct wait_channel idle;
};

static struct plic_controller plic = {
	.lock = SPINLOCK_INIT(LOCK_RANK_IRQ, LOCK_IRQ_HARDIRQ_REACHABLE),
	.idle = WAIT_CHANNEL_INIT_RANK(plic.idle, LOCK_RANK_WAIT_CHANNEL,
				       LOCK_IRQ_HARDIRQ_REACHABLE),
};

/* The ordinary memory barriers do not order the RISC-V I/O domain. These
 * fences also order device acknowledgement before PLIC completion, and MMIO
 * accesses against the memory operations publishing registration state. */
static void plic_fence(void)
{
	asm volatile("fence iorw,iorw" ::: "memory");
}

static uint32_t plic_read(vaddr_t address)
{
	plic_fence();
	uint32_t value = MMIO_READ(uint32_t, address);
	plic_fence();
	return value;
}

static void plic_write(vaddr_t address, uint32_t value)
{
	plic_fence();
	MMIO_WRITE(uint32_t, address, value);
	plic_fence();
}

static int plic_write_warl(vaddr_t address, uint32_t value)
{
	uint32_t old = plic_read(address);

	plic_write(address, value);
	if (plic_read(address) == value)
		return 0;
	plic_write(address, old);
	return -EINVAL;
}

static int plic_check_irq(struct plic_controller *controller, unsigned irq)
{
	if (!controller->sources)
		return -ENODEV;
	return irq && irq <= controller->ndev ? 0 : -EINVAL;
}

/* Caller holds controller->lock, except during single-CPU boot initialization. */
static void plic_set_enable(struct plic_controller *controller, uint32_t cpu,
			    unsigned irq, bool enable)
{
	vaddr_t address = controller->contexts[cpu].enable + (irq / 32) * 4;
	uint32_t mask = 1u << (irq % 32);
	uint32_t value = plic_read(address);

	plic_write(address, enable ? value | mask : value & ~mask);
}

static uint32_t plic_claim(struct plic_controller *controller, uint32_t cpu)
{
	return plic_read(controller->contexts[cpu].context + PLIC_CLAIM);
}

static void plic_complete(struct plic_controller *controller, uint32_t cpu,
			  unsigned irq)
{
	plic_write(controller->contexts[cpu].context + PLIC_CLAIM, irq);
}

void plic_init(const struct plic_config *config)
{
	struct plic_controller *controller = &plic;
	BUG_ON(controller->sources || !nr_cpu_ids || !irqs_disabled());
	if (!config->ndev || config->ndev > PLIC_MAX_SOURCES ||
	    (config->regs.start & 3))
		panic("plic: invalid register layout");
	unsigned words = config->ndev / 32 + 1;
	for (uint32_t cpu = 0; cpu < nr_cpu_ids; cpu++) {
		uint32_t context = config->contexts[cpu];
		if (context >= PLIC_MAX_CONTEXTS ||
		    PLIC_CONTEXT + context * PLIC_CONTEXT_STRIDE + 8 > config->regs.size ||
		    PLIC_ENABLE + context * PLIC_ENABLE_STRIDE + words * 4 > config->regs.size)
			panic("plic: registers out of range for CPU %u", cpu);
	}

	controller->sources = kzalloc((config->ndev + 1) *
				      sizeof(*controller->sources), ALLOC_NOWAIT);
	if (!controller->sources)
		panic("plic: cannot allocate interrupt registrations");
	controller->ndev = config->ndev;
	controller->base = mmio_map(config->regs.start, config->regs.size);

	/* Only selected S-mode contexts belong to this kernel. Mask them all
	 * before programming the global source priorities. */
	for (uint32_t cpu = 0; cpu < nr_cpu_ids; cpu++) {
		uint32_t context = config->contexts[cpu];
		controller->contexts[cpu].enable = controller->base + PLIC_ENABLE +
			context * PLIC_ENABLE_STRIDE;
		controller->contexts[cpu].context = controller->base + PLIC_CONTEXT +
			context * PLIC_CONTEXT_STRIDE;
		for (unsigned word = 0; word < words; word++)
			plic_write(controller->contexts[cpu].enable + word * 4, 0);
	}
	for (unsigned irq = 1; irq <= controller->ndev; irq++) {
		if (plic_write_warl(controller->base + PLIC_PRIORITY + irq * 4, 1))
			panic("plic: source %u does not support priority 1", irq);
	}
	pr_info("plic: %u sources, %u S-mode contexts\n", controller->ndev,
		nr_cpu_ids);
}

void plic_cpu_init(void)
{
	struct plic_controller *controller = &plic;
	uint32_t cpu = current_cpu()->id;
	irq_flags_t flags;

	BUG_ON(!controller->sources || !irqs_disabled() || cpu >= nr_cpu_ids);
	spin_lock_irqsave(&controller->lock, &flags);
	BUG_ON(controller->contexts[cpu].ready);
	if (plic_write_warl(controller->contexts[cpu].context + PLIC_THRESHOLD, 0))
		panic("plic: CPU %u does not support threshold 0", cpu);
	controller->contexts[cpu].ready = true;
	spin_unlock_irqrestore(&controller->lock, flags);
}

int plic_set_threshold(uint32_t cpu, uint32_t threshold)
{
	struct plic_controller *controller = &plic;
	irq_flags_t flags;
	int ret;

	if (!controller->sources)
		return -ENODEV;
	if (cpu >= nr_cpu_ids)
		return -EINVAL;
	spin_lock_irqsave(&controller->lock, &flags);
	ret = controller->contexts[cpu].ready ?
		plic_write_warl(controller->contexts[cpu].context + PLIC_THRESHOLD,
				threshold) :
		-ENODEV;
	spin_unlock_irqrestore(&controller->lock, flags);
	return ret;
}

int irq_register(unsigned irq, uint32_t cpu, irq_handler_t handler, void *data)
{
	struct plic_controller *controller = &plic;
	irq_flags_t flags;
	int ret = plic_check_irq(controller, irq);

	if (ret)
		return ret;
	if (!handler || cpu >= nr_cpu_ids)
		return -EINVAL;
	if (!cpu_is_online(cpu))
		return -ENODEV;
	spin_lock_irqsave(&controller->lock, &flags);
	struct plic_source *source = &controller->sources[irq];
	if (source->handler) {
		ret = -EBUSY;
	} else {
		BUG_ON(!controller->contexts[cpu].ready);
		ret = plic_write_warl(controller->base + PLIC_PRIORITY + irq * 4,
				      1);
		if (!ret)
			*source = (struct plic_source){
				.handler = handler, .data = data, .cpu = cpu,
			};
	}
	spin_unlock_irqrestore(&controller->lock, flags);
	return ret;
}

int irq_enable(unsigned irq)
{
	struct plic_controller *controller = &plic;
	irq_flags_t flags;
	int ret = plic_check_irq(controller, irq);

	if (ret)
		return ret;
	spin_lock_irqsave(&controller->lock, &flags);
	struct plic_source *source = &controller->sources[irq];
	if (!source->handler) {
		ret = -ENOENT;
	} else if (source->removing) {
		ret = -EBUSY;
	} else {
		source->enabled = true;
		plic_set_enable(controller, source->cpu, irq, true);
	}
	spin_unlock_irqrestore(&controller->lock, flags);
	return ret;
}

static void plic_disable(struct plic_controller *controller, unsigned irq)
{
	struct plic_source *source = &controller->sources[irq];

	source->enabled = false;
	/* PLIC ignores completion if the context's enable bit is already
	 * clear. Keep it set until an active claim has been completed. The
	 * gateway cannot forward another request before that completion. */
	if (!source->active)
		plic_set_enable(controller, source->cpu, irq, false);
}

int irq_disable(unsigned irq)
{
	struct plic_controller *controller = &plic;
	irq_flags_t flags;
	int ret = plic_check_irq(controller, irq);

	if (ret)
		return ret;
	spin_lock_irqsave(&controller->lock, &flags);
	if (!controller->sources[irq].handler)
		ret = -ENOENT;
	else
		plic_disable(controller, irq);
	spin_unlock_irqrestore(&controller->lock, flags);
	return ret;
}

static int plic_wait_idle(struct plic_controller *controller, unsigned irq)
{
	const struct wait_deadline deadline = wait_deadline_none();
	irq_flags_t flags;

	for (;;) {
		struct wait_scope scope __wait_scope = {};
		wait_outcome_t outcome;
		int ret = wait_scope_begin(&scope, 0, &deadline);
		bool active;

		if (ret)
			return ret;
		spin_lock_irqsave(&controller->lock, &flags);
		active = controller->sources[irq].active;
		if (active)
			ret = wait_scope_prepare(&scope, &controller->idle, false);
		spin_unlock_irqrestore(&controller->lock, flags);
		if (!active || ret)
			return ret;
		ret = wait_scope_block(&scope, &outcome);
		if (ret)
			return ret;
		BUG_ON(outcome != WAIT_OUTCOME_EVENT);
	}
}

int irq_synchronize(unsigned irq)
{
	struct plic_controller *controller = &plic;
	irq_flags_t flags;
	int ret = plic_check_irq(controller, irq);

	if (ret)
		return ret;
	if (!wait_context_can_sleep())
		return -EINVAL;
	spin_lock_irqsave(&controller->lock, &flags);
	if (!controller->sources[irq].handler)
		ret = -ENOENT;
	else if (controller->sources[irq].removing)
		ret = -EBUSY;
	spin_unlock_irqrestore(&controller->lock, flags);
	return ret ? ret : plic_wait_idle(controller, irq);
}

int irq_unregister(unsigned irq)
{
	struct plic_controller *controller = &plic;
	irq_flags_t flags;
	int ret = plic_check_irq(controller, irq);

	if (ret)
		return ret;
	if (!wait_context_can_sleep())
		return -EINVAL;
	spin_lock_irqsave(&controller->lock, &flags);
	struct plic_source *source = &controller->sources[irq];
	if (!source->handler) {
		ret = -ENOENT;
	} else if (source->removing) {
		ret = -EBUSY;
	} else {
		source->removing = true;
		plic_disable(controller, irq);
	}
	spin_unlock_irqrestore(&controller->lock, flags);
	if (ret)
		return ret;

	ret = plic_wait_idle(controller, irq);
	spin_lock_irqsave(&controller->lock, &flags);
	if (!ret)
		*source = (struct plic_source){0};
	else
		source->removing = false;
	spin_unlock_irqrestore(&controller->lock, flags);
	return ret;
}

int irq_set_priority(unsigned irq, uint32_t priority)
{
	struct plic_controller *controller = &plic;
	irq_flags_t flags;
	int ret = plic_check_irq(controller, irq);

	if (ret)
		return ret;
	spin_lock_irqsave(&controller->lock, &flags);
	if (!controller->sources[irq].handler)
		ret = -ENOENT;
	else if (controller->sources[irq].removing)
		ret = -EBUSY;
	else
		ret = plic_write_warl(controller->base + PLIC_PRIORITY + irq * 4,
				      priority);
	spin_unlock_irqrestore(&controller->lock, flags);
	return ret;
}

int irq_pending(unsigned irq)
{
	struct plic_controller *controller = &plic;
	int ret = plic_check_irq(controller, irq);

	if (ret)
		return ret;
	return !!(plic_read(controller->base + PLIC_PENDING + (irq / 32) * 4) &
		  (1u << (irq % 32)));
}

void plic_handle_irq(void)
{
	struct plic_controller *controller = &plic;
	uint32_t cpu = current_cpu()->id;
	irq_flags_t flags;

	BUG_ON(!controller->sources || !in_irq() || !irqs_disabled());
	for (;;) {
		spin_lock_irqsave(&controller->lock, &flags);
		unsigned irq = plic_claim(controller, cpu);
		if (!irq) {
			spin_unlock_irqrestore(&controller->lock, flags);
			return;
		}
		if (irq > controller->ndev)
			panic("plic: CPU %u claimed invalid source %u", cpu, irq);
		struct plic_source *source = &controller->sources[irq];
		if (!source->handler || source->cpu != cpu ||
		    !source->enabled || source->removing) {
			plic_complete(controller, cpu, irq);
			plic_set_enable(controller, cpu, irq, false);
			spin_unlock_irqrestore(&controller->lock, flags);
			pr_warn("plic: masked unexpected source %u on CPU %u\n", irq, cpu);
			continue;
		}
		BUG_ON(source->active);
		source->active = true;
		irq_handler_t handler = source->handler;
		void *data = source->data;
		spin_unlock_irqrestore(&controller->lock, flags);

		handler(irq, data);
		BUG_ON(!irqs_disabled());

		spin_lock_irqsave(&controller->lock, &flags);
		plic_complete(controller, cpu, irq);
		if (!source->enabled)
			plic_set_enable(controller, cpu, irq, false);
		source->active = false;
		spin_unlock_irqrestore(&controller->lock, flags);
		wait_channel_wake_all(&controller->idle);
	}
}
