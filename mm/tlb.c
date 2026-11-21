#include <nuvix/tlb.h>

void flush_tlb_all(void)
{
    local_flush_tlb_all();
#ifdef CONFIG_SMP
    arch_remote_flush_tlb_all();
#endif
}

void flush_tlb_page(vaddr_t addr)
{
    local_flush_tlb_page(addr);
#ifdef CONFIG_SMP
    arch_remote_flush_tlb_page(addr);
#endif
}

void flush_icache_all(void)
{
    arch_local_flush_icache_all();
#ifdef CONFIG_SMP
    arch_remote_flush_icache_all();
#endif
}
