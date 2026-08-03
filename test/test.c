/*
 * test/test.c - kernel self-test runner
 */

#include <kernel/irq.h>
#include <kernel/buddy.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <kernel/test.h>
#include <kernel/types.h>

#include "ktest.h"

static const struct ktest_case arch_interface_cases[] = {
	KTEST_CASE(test_arch_interface_static_contracts),
};

static const struct ktest_case bitmap_cases[] = {
	KTEST_CASE(test_bitmap),
	KTEST_CASE(test_bitmap_ranges),
	KTEST_CASE(test_bitmap_find_first_set),
	KTEST_CASE(test_bitmap_find_first_zero),
	KTEST_CASE(test_bitmap_find_next),
	KTEST_CASE(test_bitmap_weight),
	KTEST_CASE(test_bitmap_odd_bits),
};

static const struct ktest_case container_cases[] = {
	KTEST_CASE(test_kfifo_order_and_wrap),
	KTEST_CASE(test_kfifo_bulk_and_init),
	KTEST_CASE(test_klifo_order_and_bounds),
	KTEST_CASE(test_klifo_objects_and_init),
};

static const struct ktest_case printk_cases[] = {
	KTEST_CASE(test_printk_ring_read_clear_and_overwrite),
};

static const struct ktest_case hash_cases[] = {
	KTEST_CASE(test_hash_insert_lookup),
	KTEST_CASE(test_hash_collision_delete),
};

static const struct ktest_case cleanup_cases[] = {
	KTEST_CASE(test_cleanup_free_scope),
	KTEST_CASE(test_cleanup_take_ptr),
	KTEST_CASE(test_cleanup_forget_ptr),
	KTEST_CASE(test_cleanup_guard_scope),
	KTEST_CASE(test_cleanup_with_guard_block),
	KTEST_CASE(test_cleanup_class_helpers),
	KTEST_CASE(test_cleanup_kfree_scope),
};

static const struct ktest_case buddy_cases[] = {
	KTEST_CASE(test_buddy_single_page),
	KTEST_CASE(test_buddy_multi_order),
	KTEST_CASE(test_buddy_merge),
	KTEST_CASE(test_buddy_split),
	KTEST_CASE(test_buddy_over_order_preserves_free_count),
	KTEST_CASE(test_buddy_multi_order_preserves_free_count),
	KTEST_CASE(test_buddy_stress),
};

static const struct ktest_case slab_cases[] = {
	KTEST_CASE(test_slab_basic),
	KTEST_CASE(test_slab_cross_cache),
	KTEST_CASE(test_slab_stress),
	KTEST_CASE(test_slab_returns_empty_page_to_buddy),
	KTEST_CASE(test_kmalloc_large_alloc_free),
	KTEST_CASE(test_kzalloc_large_zeroes_requested_size),
	KTEST_CASE(test_kmalloc_oversize_preserves_free_count),
};

static const struct ktest_case vmalloc_cases[] = {
	KTEST_CASE(test_vmalloc_alloc_writable_pages),
	KTEST_CASE(test_vmalloc_vfree_reuses_range),
	KTEST_CASE(test_vmalloc_free_merges_adjacent_ranges),
	KTEST_CASE(test_vmalloc_mapping_failure_rolls_back),
};

static const struct ktest_case pagetable_cases[] = {
	KTEST_CASE(test_map_page_first_table_oom_rolls_back),
	KTEST_CASE(test_map_page_second_table_oom_rolls_back),
};

static const struct ktest_case vma_cases[] = {
	KTEST_CASE(test_mm_vma_merge_adjacent),
	KTEST_CASE(test_mm_vma_munmap_middle_split),
	KTEST_CASE(test_mm_vma_munmap_head_tail_trim),
	KTEST_CASE(test_mm_vma_split_enospc_preserves_layout),
	KTEST_CASE(test_mm_vma_munmap_full_table_edge_trim),
	KTEST_CASE(test_mm_dup_split_vmas),
	KTEST_CASE(test_mm_vma_mprotect_split_merge),
	KTEST_CASE(test_mm_vma_mprotect_enospc_preserves_layout),
	KTEST_CASE(test_mm_madvise_supported_hints_are_noop),
	KTEST_CASE(test_mm_move_user_pages_preserves_resident_page),
	KTEST_CASE(test_mm_msync_shared_mapping_writes_back),
	KTEST_CASE(test_mm_sparse_shared_mapping_writes_back),
};

static const struct ktest_case exec_mapping_cases[] = {
	KTEST_CASE(test_mm_exec_file_segment_faults_lazily),
	KTEST_CASE(test_mm_exec_file_segment_zero_fills_tail),
	KTEST_CASE(test_mm_exec_file_segment_split_keeps_offset),
	KTEST_CASE(test_mm_exec_file_segment_trim_keeps_offset),
	KTEST_CASE(test_mm_exec_file_segment_merge_requires_contiguous_offset),
};

static const struct ktest_case pid_cases[] = {
	KTEST_CASE(test_pid_basic),
	KTEST_CASE(test_pid_exhaust),
};

static const struct ktest_case task_cases[] = {
	KTEST_CASE(test_task_idle),
	KTEST_CASE(test_task_layout_contract),
	KTEST_CASE(test_cpu_boot_topology),
	KTEST_CASE(test_cpu_current_task_accessors),
	KTEST_CASE(test_task_alloc_free),
	KTEST_CASE(test_task_multiple),
	KTEST_CASE(test_task_process_tree),
	KTEST_CASE(test_task_free_null),
	KTEST_CASE(test_task_publish_lookup_lifetime),
	KTEST_CASE(test_task_try_get_lifetime),
	KTEST_CASE(test_wait4_stop_continue_events),
	KTEST_CASE(test_wait4_signal_exit_status),
};

static const struct ktest_case resource_cases[] = {
	KTEST_CASE(test_files_struct_copy_and_share),
	KTEST_CASE(test_files_struct_copy_preserves_cloexec),
	KTEST_CASE(test_fs_struct_copy_and_share),
	KTEST_CASE(test_sighand_struct_copy_and_share),
	KTEST_CASE(test_signal_struct_pending),
	KTEST_CASE(test_signal_struct_rlimits_copy),
};

static const struct ktest_case sched_cases[] = {
	KTEST_CASE(test_sched_init),
	KTEST_CASE(test_sched_enqueue_dequeue),
	KTEST_CASE(test_sched_need_resched),
	KTEST_CASE(test_sched_preempt_count_is_cpu_local),
	KTEST_CASE(test_sched_context_guards),
	KTEST_CASE(test_sched_deferred_reschedule),
	KTEST_CASE(test_sched_wakeup_refresh),
	KTEST_CASE(test_sched_boost),
};

static const struct ktest_case kthread_cases[] = {
	KTEST_CASE(test_kernel_thread_basic),
	KTEST_CASE(test_kernel_thread_ctx_setup),
};

static const struct ktest_case timer_cases[] = {
	KTEST_CASE(test_timer_mtime),
	KTEST_CASE(test_timer_mtimecmp),
	KTEST_CASE(test_timer_jiffies),
	KTEST_CASE(test_timer_constants),
	KTEST_CASE(test_mtime_deadline_helpers),
};

static const struct ktest_case ktimer_cases[] = {
	KTEST_CASE(test_ktimer_arm_cancel_remaining),
	KTEST_CASE(test_ktimer_timer_run_expired_callback),
	KTEST_CASE(test_ktimer_interval_rearms_after_expiry),
};

static const struct ktest_case waitqueue_cases[] = {
	KTEST_CASE(test_wait_for_timeout),
	KTEST_CASE(test_wait_for_event),
	KTEST_CASE(test_wait_for_spurious_retry),
	KTEST_CASE(test_wait_for_priority),
	KTEST_CASE(test_wait_for_wake_before_block),
	KTEST_CASE(test_wait_for_registration),
	KTEST_CASE(test_wait_cancel_callback),
	KTEST_CASE(test_wait_for_partial_error_cleanup),
	KTEST_CASE(test_wait_for_signal_only),
	KTEST_CASE(test_wait_for_validation),
};

static const struct ktest_case sync_cases[] = {
	KTEST_CASE(test_atomic_basic),
	KTEST_CASE(test_spinlock_irqsave),
	KTEST_CASE(test_spinlock_held_tracking),
};

static const struct ktest_case mutex_cases[] = {
	KTEST_CASE(test_mutex_blocking),
	KTEST_CASE(test_mutex_uncontended_preserves_sleep_state),
};

static const struct ktest_case trap_cases[] = {
	KTEST_CASE(test_trap_frame_layout),
	KTEST_CASE(test_trap_from_user),
	KTEST_CASE(test_trap_context_layout),
	KTEST_CASE(test_trap_irq_codes),
	KTEST_CASE(test_irq_nesting_context),
	KTEST_CASE(test_task_context_matrix),
	KTEST_CASE(test_trap_user_exception_classification),
	KTEST_CASE(test_signal_riscv_frame_abi),
};

static const struct ktest_case user_return_cases[] = {
	KTEST_CASE(test_user_return_work_ecall_path),
	KTEST_CASE(test_user_return_work_page_fault_path),
	KTEST_CASE(test_user_return_work_timer_path),
};

static const struct ktest_case user_trap_cases[] = {
	KTEST_CASE(test_trap_user_return_task_setup),
};

static const struct ktest_case vfs_root_cases[] = {
	KTEST_CASE(test_vfs_root_autodetect_missing_device),
	KTEST_CASE(test_vfs_root_autodetect_no_match),
	KTEST_CASE(test_vfs_root_autodetect_single_match),
	KTEST_CASE(test_vfs_root_autodetect_ambiguous_match),
	KTEST_CASE(test_vfs_root_autodetect_probe_error),
	KTEST_CASE(test_vfs_root_autodetect_skips_no_probe),
};

static const struct ktest_case page_cache_metadata_cases[] = {
	KTEST_CASE(test_page_cache_metadata_basic),
	KTEST_CASE(test_page_cache_metadata_errors),
	KTEST_CASE(test_page_cache_block_zero_writeback),
	KTEST_CASE(test_page_cache_metadata_eviction),
};

static const struct ktest_case page_cache_cases[] = {
	KTEST_CASE(test_page_cache_dirty_write_visibility),
	KTEST_CASE(test_page_cache_physical_key_identity),
	KTEST_CASE(test_page_cache_writeback_retry),
	KTEST_CASE(test_page_cache_fsync_inode_scope),
	KTEST_CASE(test_vfs_datasync_metadata_policy),
	KTEST_CASE(test_page_cache_datasync_skips_pure_inode_metadata),
	KTEST_CASE(test_page_cache_raw_alias_fsync),
	KTEST_CASE(test_page_cache_raw_alias_drop),
	KTEST_CASE(test_page_cache_pressure_eviction),
	KTEST_CASE(test_page_cache_clustered_writeback),
	KTEST_CASE(test_page_cache_indirect_reclaim_progress),
	KTEST_CASE(test_page_cache_large_offset_rejected),
};

static const struct ktest_case syscall_compat_cases[] = {
	KTEST_CASE(test_rlimit_defaults),
	KTEST_CASE(test_vfs_default_poll_masks),
	KTEST_CASE(test_vfs_poll_propagates_session_errors),
	KTEST_CASE(test_vfs_default_ioctl_enotty),
	KTEST_CASE(test_console_tty_line_discipline),
	KTEST_CASE(test_tty_signal_delivery_policy),
	KTEST_CASE(test_tty_console_job_control_policy),
	KTEST_CASE(test_tty_controlling_terminal_is_explicit),
	KTEST_CASE(test_tty_fork_inherits_controlling_terminal),
	KTEST_CASE(test_tty_clone_release_drops_attachment),
	KTEST_CASE(test_tty_process_exit_detaches_own_attachment),
	KTEST_CASE(test_tty_setsid_detaches_controlling_terminal),
	KTEST_CASE(test_tty_nonleader_detaches_only_itself),
	KTEST_CASE(test_tty_leader_detaches_entire_session),
	KTEST_CASE(test_tty_leader_release_signals_foreground_pgrp),
	KTEST_CASE(test_tty_session_leader_exit_releases_console),
	KTEST_CASE(test_tty_root_force_steals_console),
	KTEST_CASE(test_tty_force_steal_signals_old_foreground_pgrp),
	KTEST_CASE(test_tty_console_steal_requires_root_force),
	KTEST_CASE(test_tty_stale_foreground_pgrp_gets_no_signal),
	KTEST_CASE(test_tty_detach_does_not_signal_reused_foreground_pgrp),
	KTEST_CASE(test_tty_empty_foreground_pgrp_is_cleared),
	KTEST_CASE(test_signal_rt_sigsetsize_validation),
	KTEST_CASE(test_init_signal_protection),
	KTEST_CASE(test_kill_all_processes),
	KTEST_CASE(test_kill_process_groups),
	KTEST_CASE(test_shutdown_syscall_contract),
	KTEST_CASE(test_pipe2_file_alloc_failure_cleanup),
};

#define KTEST_MODULE(module_name, module_cases)                                \
	{                                                                      \
		.name = module_name, .cases = module_cases,                    \
		.nr_cases = KTEST_ARRAY_SIZE(module_cases),                    \
	}

static const struct ktest_module core_bitmap_module =
	KTEST_MODULE("bitmap", bitmap_cases);
static const struct ktest_module core_container_module =
	KTEST_MODULE("container", container_cases);
static const struct ktest_module core_printk_module =
	KTEST_MODULE("printk", printk_cases);
static const struct ktest_module core_hash_module =
	KTEST_MODULE("hash", hash_cases);
static const struct ktest_module core_cleanup_module =
	KTEST_MODULE("cleanup", cleanup_cases);

static const struct ktest_module memory_buddy_module =
	KTEST_MODULE("buddy", buddy_cases);
static const struct ktest_module memory_slab_module =
	KTEST_MODULE("slab", slab_cases);
static const struct ktest_module memory_vmalloc_module =
	KTEST_MODULE("vmalloc", vmalloc_cases);
static const struct ktest_module memory_pagetable_module =
	KTEST_MODULE("pagetable", pagetable_cases);
static const struct ktest_module memory_vma_module =
	KTEST_MODULE("vma", vma_cases);
static const struct ktest_module memory_exec_mapping_module =
	KTEST_MODULE("exec_mapping", exec_mapping_cases);

static const struct ktest_module process_pid_module =
	KTEST_MODULE("pid", pid_cases);
static const struct ktest_module process_task_module =
	KTEST_MODULE("task", task_cases);
static const struct ktest_module process_resources_module =
	KTEST_MODULE("resources", resource_cases);
static const struct ktest_module process_sched_module =
	KTEST_MODULE("sched", sched_cases);
static const struct ktest_module process_kthread_module =
	KTEST_MODULE("kthread", kthread_cases);

static const struct ktest_module time_timer_module =
	KTEST_MODULE("timer", timer_cases);
static const struct ktest_module time_ktimer_module =
	KTEST_MODULE("ktimer", ktimer_cases);
static const struct ktest_module time_waitqueue_module =
	KTEST_MODULE("waitqueue", waitqueue_cases);
static const struct ktest_module time_sync_module =
	KTEST_MODULE("sync", sync_cases);
static const struct ktest_module time_mutex_module =
	KTEST_MODULE("mutex", mutex_cases);

static const struct ktest_module trap_arch_interface_module =
	KTEST_MODULE("arch_interface", arch_interface_cases);
static const struct ktest_module trap_trap_module =
	KTEST_MODULE("trap", trap_cases);
static const struct ktest_module trap_user_return_module =
	KTEST_MODULE("user_return", user_return_cases);
static const struct ktest_module trap_user_trap_module =
	KTEST_MODULE("user_trap", user_trap_cases);

static const struct ktest_module io_vfs_root_module =
	KTEST_MODULE("vfs_root", vfs_root_cases);
static const struct ktest_module io_page_cache_metadata_module =
	KTEST_MODULE("page_cache_metadata", page_cache_metadata_cases);
static const struct ktest_module io_page_cache_module =
	KTEST_MODULE("page_cache", page_cache_cases);

static const struct ktest_module abi_syscall_compat_module =
	KTEST_MODULE("syscall_compat", syscall_compat_cases);

static const struct ktest_module *const core_modules[] = {
	&core_bitmap_module,
	&core_container_module,
	&core_printk_module,
	&core_hash_module,
	&core_cleanup_module,
};

static const struct ktest_module *const memory_modules[] = {
	&memory_buddy_module,
	&memory_slab_module,
	&memory_vmalloc_module,
	&memory_pagetable_module,
	&memory_vma_module,
	&memory_exec_mapping_module,
};

static const struct ktest_module *const process_modules[] = {
	&process_pid_module,
	&process_task_module,
	&process_resources_module,
	&process_sched_module,
	&process_kthread_module,
};

static const struct ktest_module *const time_sync_modules[] = {
	&time_timer_module,
	&time_ktimer_module,
	&time_waitqueue_module,
	&time_sync_module,
	&time_mutex_module,
};

static const struct ktest_module *const trap_modules[] = {
	&trap_arch_interface_module,
	&trap_trap_module,
	&trap_user_return_module,
	&trap_user_trap_module,
};

static const struct ktest_module *const io_modules[] = {
	&io_vfs_root_module,
	&io_page_cache_metadata_module,
	&io_page_cache_module,
};

static const struct ktest_module *const abi_modules[] = {
	&abi_syscall_compat_module,
};

#define KTEST_SUBSYSTEM(subsystem_name, subsystem_modules)                     \
	{                                                                      \
		.name = subsystem_name, .modules = subsystem_modules,          \
		.nr_modules = KTEST_ARRAY_SIZE(subsystem_modules),             \
	}

static const struct ktest_subsystem ktest_subsystems[] = {
	KTEST_SUBSYSTEM("core", core_modules),
	KTEST_SUBSYSTEM("memory", memory_modules),
	KTEST_SUBSYSTEM("process", process_modules),
	KTEST_SUBSYSTEM("time-sync", time_sync_modules),
	KTEST_SUBSYSTEM("trap", trap_modules),
	KTEST_SUBSYSTEM("io", io_modules),
	KTEST_SUBSYSTEM("abi", abi_modules),
};

#define KTEST_CONTEXT_LEAK (-2)

static bool ktest_context_is_clean(const char *name)
{
	bool clean = true;

	if (spinlock_held()) {
		pr_err("    [LEAK] %s left held locks (depth=%u)\n", name,
		       lock_depth());
		clean = false;
	}
	if (preempt_count() != 0) {
		pr_err("    [LEAK] %s left preempt_count=%d\n", name,
		       preempt_count());
		clean = false;
	}
	if (irq_nesting() != 0) {
		pr_err("    [LEAK] %s left irq_nesting=%u\n", name,
		       irq_nesting());
		clean = false;
	}
	return clean;
}

static int ktest_run_module(const struct ktest_subsystem *subsystem,
			    const struct ktest_module *module,
			    struct ktest_summary *summary)
{
	uint32_t failed_cases = 0;

	pr_info("[KTEST] module %s/%s\n", subsystem->name, module->name);

	for (uint32_t i = 0; i < module->nr_cases; i++) {
		const struct ktest_case *test = &module->cases[i];
		int ret;

		summary->cases++;
		local_irq_disable();
		ret = test->run();
		local_irq_disable();
		if (!ktest_context_is_clean(test->name)) {
			failed_cases++;
			summary->failed_cases++;
			pr_err("[FAIL] context leak after %s; stopping ktests\n",
			       test->name);
			summary->modules++;
			summary->failed_modules++;
			return KTEST_CONTEXT_LEAK;
		}
		buddy_test_validate();
		if (ret < 0) {
			failed_cases++;
			summary->failed_cases++;
			pr_err("    [FAIL] %s/%s/%s returned %d\n",
			       subsystem->name, module->name, test->name, ret);
		}
	}

	summary->modules++;
	if (failed_cases > 0) {
		summary->failed_modules++;
		pr_err("[FAIL] %s\n", module->name);
		return -1;
	}

	pr_info("[PASS] %s\n", module->name);
	return 0;
}

int kernel_test_run(struct ktest_summary *summary)
{
	struct ktest_summary result = { 0 };
	irq_flags_t flags;

	pr_info("\n");
	pr_info("========================================\n");
	pr_info("        CuteOS Kernel Self-Test         \n");
	pr_info("========================================\n");

	flags = local_irq_save();
	for (uint32_t i = 0; i < KTEST_ARRAY_SIZE(ktest_subsystems); i++) {
		const struct ktest_subsystem *subsystem = &ktest_subsystems[i];

		result.subsystems++;
		pr_info("[KTEST] subsystem %s\n", subsystem->name);
		for (uint32_t j = 0; j < subsystem->nr_modules; j++) {
			if (ktest_run_module(subsystem, subsystem->modules[j],
					 &result) == KTEST_CONTEXT_LEAK)
				goto done;
		}
	}

done:
	if (summary)
		*summary = result;

	local_irq_restore(flags);
	return result.failed_cases ? -1 : 0;
}
