#ifndef _NUVIX_UAPI_RSEQ_H
#define _NUVIX_UAPI_RSEQ_H

/**
 * @file rseq.h
 * @brief Linux restartable sequences UAPI structures.
 */

#define RSEQ_CPU_ID_UNINITIALIZED ((unsigned int)-1)
#define RSEQ_CPU_ID_REGISTRATION_FAILED ((unsigned int)-2)

#define RSEQ_FLAG_UNREGISTER (1 << 0)

#define RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT (1U << 0)
#define RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL  (1U << 1)
#define RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE (1U << 2)

/**
 * @struct rseq_cs
 * @brief Userspace critical-section descriptor read by the kernel.
 *
 * The structure is 32 bytes and aligned to 4 * sizeof(unsigned long), matching
 * Linux riscv64 expectations. Userspace publishes its address in struct rseq.
 *
 * @par Fields
 * - @c version: Descriptor version, currently 0.
 * - @c flags: RSEQ_CS_FLAG_* restart suppression bits.
 * - @c start_ip: First instruction of the critical section.
 * - @c post_commit_offset: Offset of post-commit instruction.
 * - @c abort_ip: Abort handler instruction pointer.
 */
struct rseq_cs {
	unsigned int version;
	unsigned int flags;
	unsigned long start_ip;
	unsigned long post_commit_offset;
	unsigned long abort_ip;
} __attribute__((aligned(4 * sizeof(unsigned long))));

/**
 * @struct rseq
 * @brief Per-thread userspace rseq area registered by the rseq syscall.
 *
 * @par Fields
 * - @c cpu_id_start: CPU id sampled before user critical path.
 * - @c cpu_id: Current CPU id; single-core nuvix writes 0.
 * - @c rseq_cs: User pointer to current struct rseq_cs.
 * - @c flags: Thread-level rseq flags.
 * - @c node_id: NUMA node id; single-node nuvix writes 0.
 * - @c mm_cid: Memory-map concurrency id compatibility field.
 * - @c end: Flexible ABI extension marker.
 */
struct rseq {
	unsigned int cpu_id_start;
	unsigned int cpu_id;
	unsigned long rseq_cs;
	unsigned int flags;
	unsigned int node_id;
	unsigned int mm_cid;
	char end[];
} __attribute__((aligned(4 * sizeof(unsigned long))));

#define RSEQ_OFFSETOF(t, d) __builtin_offsetof(t, d)

_Static_assert(sizeof(struct rseq_cs) == 32, "rseq_cs ABI size mismatch");
_Static_assert(RSEQ_OFFSETOF(struct rseq_cs, version) == 0,
	       "rseq_cs version offset mismatch");
_Static_assert(RSEQ_OFFSETOF(struct rseq_cs, flags) == 4,
	       "rseq_cs flags offset mismatch");
_Static_assert(RSEQ_OFFSETOF(struct rseq_cs, start_ip) == 8,
	       "rseq_cs start_ip offset mismatch");
_Static_assert(RSEQ_OFFSETOF(struct rseq_cs, post_commit_offset) == 16,
	       "rseq_cs post_commit_offset offset mismatch");
_Static_assert(RSEQ_OFFSETOF(struct rseq_cs, abort_ip) == 24,
	       "rseq_cs abort_ip offset mismatch");
_Static_assert(sizeof(struct rseq) == 32, "rseq ABI size mismatch");
_Static_assert(RSEQ_OFFSETOF(struct rseq, cpu_id_start) == 0,
	       "rseq cpu_id_start offset mismatch");
_Static_assert(RSEQ_OFFSETOF(struct rseq, cpu_id) == 4,
	       "rseq cpu_id offset mismatch");
_Static_assert(RSEQ_OFFSETOF(struct rseq, rseq_cs) == 8,
	       "rseq rseq_cs offset mismatch");
_Static_assert(RSEQ_OFFSETOF(struct rseq, flags) == 16,
	       "rseq flags offset mismatch");
_Static_assert(RSEQ_OFFSETOF(struct rseq, node_id) == 20,
	       "rseq node_id offset mismatch");
_Static_assert(RSEQ_OFFSETOF(struct rseq, mm_cid) == 24,
	       "rseq mm_cid offset mismatch");

#undef RSEQ_OFFSETOF

#endif
