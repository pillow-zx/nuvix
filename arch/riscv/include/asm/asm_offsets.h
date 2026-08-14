#ifndef _NUVIX_ASM_OFFSETS_H
#define _NUVIX_ASM_OFFSETS_H

/*
 * arch/riscv/include/asm/asm_offsets.h - assembly-visible structure offsets
 *
 * Keep these constants synchronized with C layouts and low-level assembly.
 */

/* ---- struct trap_frame offsets ---- */
#define TF_SEPC	   (0 * 8)
#define TF_RA	   (1 * 8)
#define TF_SP	   (2 * 8)
#define TF_GP	   (3 * 8)
#define TF_TP	   (4 * 8)
#define TF_T0	   (5 * 8)
#define TF_T1	   (6 * 8)
#define TF_T2	   (7 * 8)
#define TF_S0	   (8 * 8)
#define TF_S1	   (9 * 8)
#define TF_A0	   (10 * 8)
#define TF_A1	   (11 * 8)
#define TF_A2	   (12 * 8)
#define TF_A3	   (13 * 8)
#define TF_A4	   (14 * 8)
#define TF_A5	   (15 * 8)
#define TF_A6	   (16 * 8)
#define TF_A7	   (17 * 8)
#define TF_S2	   (18 * 8)
#define TF_S3	   (19 * 8)
#define TF_S4	   (20 * 8)
#define TF_S5	   (21 * 8)
#define TF_S6	   (22 * 8)
#define TF_S7	   (23 * 8)
#define TF_S8	   (24 * 8)
#define TF_S9	   (25 * 8)
#define TF_S10	   (26 * 8)
#define TF_S11	   (27 * 8)
#define TF_T3	   (28 * 8)
#define TF_T4	   (29 * 8)
#define TF_T5	   (30 * 8)
#define TF_T6	   (31 * 8)
#define TF_SCAUSE  (32 * 8)
#define TF_STVAL   (33 * 8)
#define TF_SSTATUS (34 * 8)

#define TRAP_FRAME_SIZE (35 * 8)
#define TRAP_FRAME_ALLOC_SIZE (36 * 8)

/*
 * Temporary user-sp save slot used on U->S trap entry.
 * sscratch stores kstack_top; this slot is at kstack_top - 8, which is
 * frame_base + TRAP_SCRATCH_OFFSET after allocating trap-frame storage.
 */
#define TRAP_SCRATCH_OFFSET (TRAP_FRAME_ALLOC_SIZE - 8)

/* ---- struct context offsets ---- */
#define CTX_RA	(0 * 8)
#define CTX_SP	(1 * 8)
#define CTX_S0	(2 * 8)
#define CTX_S1	(3 * 8)
#define CTX_S2	(4 * 8)
#define CTX_S3	(5 * 8)
#define CTX_S4	(6 * 8)
#define CTX_S5	(7 * 8)
#define CTX_S6	(8 * 8)
#define CTX_S7	(9 * 8)
#define CTX_S8	(10 * 8)
#define CTX_S9	(11 * 8)
#define CTX_S10 (12 * 8)
#define CTX_S11 (13 * 8)

/*
 * Must match offsetof(struct task_struct, ...). The C side checks these in
 * include/nuvix/task.h, catching layout drift at build time.
 */
#define TASK_KSTACK 120
#define TASK_SATP   128

#define TASK_KSTACK_SIZE 1024 * 16

/* ---- struct cpu offsets ---- */
#define CPU_STATE	   8
#define CPU_CURRENT_TASK  24
#define CPU_PREEMPT_COUNT 32
#define CPU_ENTRY_SCRATCH 48

/*
 * sizeof(struct cpu), config-dependent through CONFIG_DEBUG_CONTEXT.
 * The C side checks it with a static_assert in include/nuvix/cpu.h.
 */
#if defined(CONFIG_DEBUG_CONTEXT)
#define CPU_SIZE 336
#else
#define CPU_SIZE 64
#endif

#endif
