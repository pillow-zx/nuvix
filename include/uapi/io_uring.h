#ifndef _NUVIX_UAPI_IO_URING_H
#define _NUVIX_UAPI_IO_URING_H
#include <nuvix/types.h>

/* Linux riscv64 layouts. Unimplemented operations retain their Linux numbers.
 */

struct io_uring_sqe {
	uint8_t opcode;
        uint8_t flags;
	uint16_t ioprio;
	int32_t fd;
	uint64_t off;
        uint64_t addr;
	uint32_t len;
	union {
		uint32_t rw_flags;
                uint32_t poll32_events;
                uint32_t timeout_flags;
                uint32_t cancel_flags;
		uint32_t fsync_flags;
	};
	uint64_t user_data;
	uint16_t buf_index;
        uint16_t personality;
	uint32_t file_index;
	uint64_t addr3;
        uint64_t pad2;
};
struct io_uring_cqe {
	uint64_t user_data;
	int32_t res;
	uint32_t flags;
};
struct io_sqring_offsets {
	uint32_t head;
        uint32_t tail;
        uint32_t ring_mask;
        uint32_t ring_entries;
        uint32_t flags;
        uint32_t dropped;
        uint32_t array;
	uint32_t resv1;
	uint64_t user_addr;
};
struct io_cqring_offsets {
	uint32_t head;
        uint32_t tail;
        uint32_t ring_mask;
        uint32_t ring_entries;
        uint32_t overflow;
        uint32_t cqes;
        uint32_t flags;
	uint32_t resv1;
	uint64_t user_addr;
};
struct io_uring_params {
	uint32_t sq_entries;
	uint32_t cq_entries;
	uint32_t flags;
	uint32_t sq_thread_cpu;
	uint32_t sq_thread_idle;
	uint32_t features;
	uint32_t wq_fd;
	uint32_t resv[3];
	struct io_sqring_offsets sq_off;
	struct io_cqring_offsets cq_off;
};
struct io_uring_getevents_arg {
	uint64_t sigmask;
	uint32_t sigmask_sz;
	uint32_t pad;
	uint64_t ts;
};
struct io_uring_rsrc_register {
	uint32_t nr;
	uint32_t flags;
	uint64_t resv2;
	uint64_t data;
	uint64_t tags;
};
struct io_uring_rsrc_update2 {
	uint32_t offset;
	uint32_t resv;
	uint64_t data;
	uint64_t tags;
	uint32_t nr;
	uint32_t resv2;
};
struct io_uring_probe_op {
	uint8_t op;
	uint8_t resv;
	uint16_t flags;
	uint32_t resv2;
};
struct io_uring_probe {
	uint8_t last_op;
	uint8_t ops_len;
	uint16_t resv;
	uint32_t resv2[3];
	struct io_uring_probe_op ops[];
};

#define IORING_OP_NOP		       0
#define IORING_OP_READV		       1
#define IORING_OP_WRITEV	       2
#define IORING_OP_FSYNC		       3
#define IORING_OP_READ_FIXED	       4
#define IORING_OP_WRITE_FIXED	       5
#define IORING_OP_POLL_ADD	       6
#define IORING_OP_TIMEOUT	       11
#define IORING_OP_ASYNC_CANCEL	       14
#define IORING_OP_READ		       22
#define IORING_OP_WRITE		       23
#define IORING_OFF_SQ_RING	       0ULL
#define IORING_OFF_CQ_RING	       0x8000000ULL
#define IORING_OFF_SQES		       0x10000000ULL
#define IORING_SQ_CQ_OVERFLOW	       (1U << 1)
#define IORING_ENTER_GETEVENTS	       (1U << 0)
#define IORING_ENTER_EXT_ARG	       (1U << 3)
#define IORING_TIMEOUT_ABS	       (1U << 0)
#define IORING_FEAT_SINGLE_MMAP	       (1U << 0)
#define IORING_FEAT_NODROP	       (1U << 1)
#define IORING_FEAT_SUBMIT_STABLE      (1U << 2)
#define IORING_FEAT_FAST_POLL	       (1U << 5)
#define IORING_FEAT_EXT_ARG	       (1U << 8)
#define IORING_UNREGISTER_BUFFERS      1
#define IORING_REGISTER_PROBE	       8
#define IORING_REGISTER_BUFFERS2       15
#define IORING_REGISTER_BUFFERS_UPDATE 16
#define IO_URING_OP_SUPPORTED	       1

#endif
