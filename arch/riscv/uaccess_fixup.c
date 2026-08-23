/*
 * arch/riscv/uaccess_fixup.c - user-access exception fixups
 */

#include <asm/trap.h>
#include <arch/trap.h>
#include <arch/uaccess.h>
#include <nuvix/errno.h>

struct riscv_uaccess_exception_entry {
	uintptr_t fault;
	uintptr_t fixup;
};

static_assert(sizeof(struct riscv_uaccess_exception_entry) ==
	      2 * sizeof(uintptr_t),
	      "uaccess exception entry must contain two addresses");

extern const struct riscv_uaccess_exception_entry __start___ex_table[];
extern const struct riscv_uaccess_exception_entry __stop___ex_table[];

static bool uaccess_fault_cause(uintptr_t cause)
{
	cause &= ~SCAUSE_IRQ_FLAG;

	return cause == EXC_LOAD_PAGE_FAULT ||
	       cause == EXC_STORE_PAGE_FAULT ||
	       cause == EXC_LOAD_ACCESS ||
	       cause == EXC_STORE_ACCESS;
}

bool riscv_uaccess_fixup(struct trap_frame *tf)
{
	const struct riscv_uaccess_exception_entry *entry;

	if (trap_frame_from_user(tf) || !uaccess_fault_cause(tf->scause))
		return false;

	for (entry = __start___ex_table; entry < __stop___ex_table; entry++) {
		if (entry->fault != tf->sepc)
			continue;

		tf->sepc = entry->fixup;
		tf->a0 = (uintptr_t)-EFAULT;
		return true;
	}

	return false;
}
