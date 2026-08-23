/*
 * kernel/exec.c - ELF 加载与 execve 替换语义
 */

#include <nuvix/elf.h>
#include <nuvix/errno.h>
#include <nuvix/buddy.h>
#include <nuvix/exec.h>
#include <nuvix/fdtable.h>
#include <nuvix/fork.h>
#include <nuvix/futex.h>
#include <nuvix/fs.h>
#include <nuvix/mm.h>
#include <nuvix/printk.h>
#include <nuvix/proc.h>
#include <nuvix/random.h>
#include <nuvix/rseq.h>
#include <nuvix/sched.h>
#include <nuvix/signal.h>
#include <nuvix/slab.h>
#include <nuvix/task.h>
#include <nuvix/syscall.h>
#include <nuvix/vfs.h>
#include <nuvix/vmalloc.h>
#include <uapi/auxvec.h>
#include <uapi/mman.h>
#include <nuvix/processor.h>
#include <nuvix/page.h>
#include <nuvix/pgtable.h>
#include <nuvix/trap.h>

struct exec_image {
	struct file *file;
	uint64_t size;
};

struct elf_phdr_table {
	Elf64_Phdr *entries;
	int count;
};

struct elf_load_layout {
	vaddr_t first_vaddr;
	vaddr_t last_end;
	bool loaded_segment;
};

struct elf_auxv {
	uintptr_t type;
	uintptr_t value;
};

static int elf_auxv_set_value(struct elf_auxv *auxv, size_t count,
			      uintptr_t type, uintptr_t value)
{
	for (size_t index = 0; index < count; index++) {
		if (auxv[index].type != type)
			continue;
		auxv[index].value = value;
		return 0;
	}

	return -ENOEXEC;
}

static int elf_flags_to_prot(uint32_t p_flags)
{
	int prot = 0;

	if (p_flags & PF_R)
		prot |= PROT_READ;
	if (p_flags & PF_W)
		prot |= PROT_WRITE;
	if (p_flags & PF_X)
		prot |= PROT_EXEC;

	return prot;
}

static int open_exec_image(const char *path, struct exec_image *image)
{
	struct path found;
	struct file *file;
	int ret;

	memset(image, 0, sizeof(*image));

	ret = path_lookupat_path(NULL, path, 0, &found);
	if (ret < 0)
		return ret;
	if (!found.dentry->d_inode || !found.dentry->d_inode->i_fop ||
	    !found.dentry->d_inode->i_fop->read) {
		path_put(&found);
		return -ENOEXEC;
	}

	file = file_alloc_path(&found, O_RDONLY, FMODE_READ);
	path_put(&found);
	if (!file)
		return -ENOMEM;

	image->file = file;
	image->size = vfs_inode_size(file->f_inode);
	return 0;
}

static void close_exec_image(struct exec_image *image)
{
	if (!image || !image->file)
		return;

	file_put(image->file);
	image->file = NULL;
	image->size = 0;
}

static int read_exec_image(struct exec_image *image, uint64_t offset, void *buf,
			   size_t len)
{
	if (!image || !image->file || !buf)
		return -EINVAL;
	if (offset > image->size || len > image->size - offset)
		return -ENOEXEC;

	image->file->f_pos = (loff_t)offset;
	ssize_t ret = vfs_read(image->file, buf, len);
	if (ret < 0)
		return (int)ret;
	if ((size_t)ret != len)
		return -ENOEXEC;

	return 0;
}

static void *stack_sp_to_kernel(void *stack, vaddr_t sp)
{
	return (uint8_t *)stack + (sp - USER_STACK_BASE);
}

static int push_user_bytes(void *stack, uintptr_t *sp, const void *src,
			   size_t len, uintptr_t *user_ptr)
{
	if (*sp < USER_STACK_BASE + len)
		return -E2BIG;

	*sp -= len;
	memcpy(stack_sp_to_kernel(stack, *sp), src, len);
	*user_ptr = *sp;
	return 0;
}

static int push_user_string(void *stack, uintptr_t *sp, const char *src,
			    uintptr_t *user_ptr)
{
	return push_user_bytes(stack, sp, src, strlen(src) + 1, user_ptr);
}

static int setup_user_stack(struct mm_struct *mm,
			    const struct exec_args_envp *args, const char *path,
			    const Elf64_Ehdr *ehdr, vaddr_t phdr_addr,
			    vaddr_t *sp_out)
{
	void *stack = vmalloc(USER_STACK_SIZE, ALLOC_NOWAIT);
	uintptr_t random_ptr;
	uintptr_t execfn_ptr;
	uint8_t random_bytes[16];
	struct elf_auxv auxv[] = {
		{AT_PHDR, phdr_addr},
		{AT_PHENT, sizeof(Elf64_Phdr)},
		{AT_PHNUM, ehdr->e_phnum},
		{AT_PAGESZ, PAGE_SIZE},
		{AT_ENTRY, ehdr->e_entry},
		{AT_UID, task_uid(current_task())},
		{AT_EUID, task_euid(current_task())},
		{AT_GID, task_gid(current_task())},
		{AT_EGID, task_egid(current_task())},
		{AT_SECURE, 0},
		{AT_RANDOM, 0},
		{AT_EXECFN, 0},
		{AT_NULL, 0},
	};

	if (!stack)
		return -ENOMEM;
	memset(stack, 0, USER_STACK_SIZE);

	uintptr_t user_argv[EXEC_MAX_ARGS + 1];
	uintptr_t user_envp[EXEC_MAX_ENVS + 1];
	uintptr_t sp = USER_STACK_TOP;
	int ret;

	for (int i = args->envc - 1; i >= 0; i--) {
		ret = push_user_string(stack, &sp, args->envp[i],
				       &user_envp[i]);
		if (ret < 0)
			goto out;
	}
	user_envp[args->envc] = 0;

	for (int i = args->argc - 1; i >= 0; i--) {
		ret = push_user_string(stack, &sp, args->argv[i],
				       &user_argv[i]);
		if (ret < 0)
			goto out;
	}
	user_argv[args->argc] = 0;
	weak_random_bytes(random_bytes, sizeof(random_bytes));
	ret = push_user_bytes(stack, &sp, random_bytes, sizeof(random_bytes),
			      &random_ptr);
	if (ret < 0)
		goto out;
	ret = push_user_string(stack, &sp, path, &execfn_ptr);
	if (ret < 0)
		goto out;
	ret = elf_auxv_set_value(auxv, ARRLEN(auxv), AT_RANDOM, random_ptr);
	if (ret < 0)
		goto out;
	ret = elf_auxv_set_value(auxv, ARRLEN(auxv), AT_EXECFN, execfn_ptr);
	if (ret < 0)
		goto out;

	sp &= ~(uintptr_t)0xf;

	size_t argv_bytes = (size_t)(args->argc + 1) * sizeof(uintptr_t);
	size_t envp_bytes = (size_t)(args->envc + 1) * sizeof(uintptr_t);
	size_t auxv_bytes = sizeof(auxv);
	size_t frame_bytes =
		sizeof(uintptr_t) + argv_bytes + envp_bytes + auxv_bytes;
	size_t padding = frame_bytes & 0xf ? 16 - (frame_bytes & 0xf) : 0;

	if (sp < USER_STACK_BASE + frame_bytes + padding)
		goto too_big;

	sp -= frame_bytes + padding;

	uint8_t *frame = stack_sp_to_kernel(stack, sp);
	uintptr_t argc = (uintptr_t)args->argc;

	memcpy(frame, &argc, sizeof(argc));
	memcpy(frame + sizeof(uintptr_t), user_argv, argv_bytes);
	memcpy(frame + sizeof(uintptr_t) + argv_bytes, user_envp, envp_bytes);
	memcpy(frame + sizeof(uintptr_t) + argv_bytes + envp_bytes, auxv,
	       auxv_bytes);

	*sp_out = sp;
	ret = mm_add_stack(mm, stack, USER_STACK_SIZE);
out:
	vfree(stack);
	return ret;

too_big:
	ret = -E2BIG;
	goto out;
}

static int read_elf_header(struct exec_image *image, Elf64_Ehdr *ehdr)
{
	int ret;

	if (!image || !ehdr)
		return -EINVAL;
	if (image->size < sizeof(*ehdr))
		return -ENOEXEC;

	ret = read_exec_image(image, 0, ehdr, sizeof(*ehdr));
	if (ret < 0)
		return ret;

	if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
	    ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
	    ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
	    ehdr->e_ident[EI_MAG3] != ELFMAG3)
		return -ENOEXEC;
	if (ehdr->e_ident[EI_CLASS] != ELFCLASS64)
		return -ENOEXEC;
	if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
		return -ENOEXEC;
	if (ehdr->e_type != ET_EXEC || ehdr->e_machine != EM_RISCV)
		return -ENOEXEC;
	if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0)
		return -ENOEXEC;
	if (ehdr->e_phentsize != sizeof(Elf64_Phdr))
		return -ENOEXEC;

	return 0;
}

static int read_elf_phdr_table(struct exec_image *image, const Elf64_Ehdr *ehdr,
			       struct elf_phdr_table *table)
{
	uint64_t phdr_bytes;
	int ret;

	if (!image || !ehdr || !table)
		return -EINVAL;

	memset(table, 0, sizeof(*table));
	phdr_bytes = (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
	if (ehdr->e_phoff > image->size ||
	    phdr_bytes > image->size - ehdr->e_phoff)
		return -ENOEXEC;

	table->entries = kmalloc((size_t)phdr_bytes, ALLOC_NOWAIT);
	if (!table->entries)
		return -ENOMEM;

	ret = read_exec_image(image, ehdr->e_phoff, table->entries,
			      (size_t)phdr_bytes);
	if (ret < 0) {
		kfree(table->entries);
		table->entries = NULL;
		return ret;
	}

	table->count = ehdr->e_phnum;
	return 0;
}

static void free_elf_phdr_table(struct elf_phdr_table *table)
{
	if (!table || !table->entries)
		return;

	kfree(table->entries);
	table->entries = NULL;
	table->count = 0;
}

static int elf_user_phdr_addr(const Elf64_Ehdr *ehdr,
			      const struct elf_phdr_table *phdrs,
			      vaddr_t *phdr_addr)
{
	uint64_t phdr_bytes = (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr);

	for (int i = 0; i < phdrs->count; i++) {
		const Elf64_Phdr *ph = &phdrs->entries[i];
		uint64_t offset;
		uint64_t address;

		if (ph->p_type != PT_LOAD || ehdr->e_phoff < ph->p_offset)
			continue;
		offset = ehdr->e_phoff - ph->p_offset;
		if (offset > ph->p_filesz || phdr_bytes > ph->p_filesz - offset)
			continue;
		address = ph->p_vaddr + offset;
		if (address < ph->p_vaddr || address > USER_STACK_BASE ||
		    phdr_bytes > USER_STACK_BASE - address)
			return -ENOEXEC;

		*phdr_addr = (vaddr_t)address;
		return 0;
	}

	return -ENOEXEC;
}

static int validate_elf_program_headers(const struct elf_phdr_table *phdrs)
{
	for (int i = 0; i < phdrs->count; i++) {
		switch (phdrs->entries[i].p_type) {
		case PT_INTERP:
		case PT_DYNAMIC:
			return -ENOEXEC;
		default:
			break;
		}
	}

	return 0;
}

static int create_exec_mm(struct mm_struct **mm_out)
{
	struct mm_struct *mm;

	if (!mm_out)
		return -EINVAL;
	*mm_out = NULL;

	mm = mm_create_user();
	if (!mm)
		return -ENOMEM;

	*mm_out = mm;
	return 0;
}

static bool elf_phdr_loadable(const Elf64_Phdr *ph)
{
	return ph->p_type == PT_LOAD && ph->p_memsz != 0;
}

static int validate_load_segment(struct exec_image *image, const Elf64_Phdr *ph,
				 vaddr_t *seg_start, vaddr_t *seg_end)
{
	uint64_t start;
	uint64_t end;

	if (!(ph->p_flags & PF_R))
		return -ENOEXEC;
	if (ph->p_filesz > ph->p_memsz)
		return -ENOEXEC;
	if (ph->p_offset > image->size ||
	    ph->p_filesz > image->size - ph->p_offset)
		return -ENOEXEC;

	start = ph->p_vaddr;
	end = ph->p_vaddr + ph->p_memsz;
	if (end < start || end > USER_STACK_BASE)
		return -ENOEXEC;

	*seg_start = (vaddr_t)start;
	*seg_end = (vaddr_t)end;
	return 0;
}

static int copy_segment_page(struct exec_image *image, const Elf64_Phdr *ph,
			     void *page, vaddr_t va, vaddr_t seg_start)
{
	uint64_t page_file_start = va < seg_start ? 0 : va - seg_start;
	uint64_t file_off;
	uint64_t remaining;
	size_t copy_off;
	size_t copy_len;

	if (page_file_start >= ph->p_filesz)
		return 0;

	file_off = ph->p_offset + page_file_start;
	copy_off = va < seg_start ? seg_start - va : 0;
	copy_len = PAGE_SIZE - copy_off;
	remaining = ph->p_filesz - page_file_start;
	if (copy_len > remaining)
		copy_len = (size_t)remaining;

	return read_exec_image(image, file_off, (uint8_t *)page + copy_off,
			       copy_len);
}

static int map_segment_page(struct exec_image *image, struct mm_struct *mm,
			    const Elf64_Phdr *ph, vaddr_t va, vaddr_t seg_start)
{
	void *page;
	int ret;

	page = get_free_page(0, ALLOC_NOWAIT);
	if (!page)
		return -ENOMEM;
	memset(page, 0, PAGE_SIZE);

	ret = copy_segment_page(image, ph, page, va, seg_start);
	if (ret < 0) {
		free_page(page, 0);
		return ret;
	}

	ret = mm_map_page(mm, va, page, elf_flags_to_prot(ph->p_flags));
	if (ret < 0) {
		free_page(page, 0);
		return ret;
	}
	return 0;
}

static int map_load_segment(struct exec_image *image, struct mm_struct *mm,
			    const Elf64_Phdr *ph, vaddr_t seg_start,
			    vaddr_t seg_end)
{
	vaddr_t page_start = PFN_DOWN(seg_start) << PAGE_SHIFT;
	vaddr_t page_end = PFN_UP(seg_end) << PAGE_SHIFT;

	for (vaddr_t va = page_start; va < page_end; va += PAGE_SIZE) {
		int ret = map_segment_page(image, mm, ph, va, seg_start);

		if (ret < 0)
			return ret;
	}

	return 0;
}

static bool can_map_file_backed_text(const Elf64_Phdr *ph)
{
	if (!(ph->p_flags & PF_X) || (ph->p_flags & PF_W))
		return false;
	if (ph->p_filesz != ph->p_memsz)
		return false;
	return (ph->p_offset & (PAGE_SIZE - 1)) ==
	       (ph->p_vaddr & (PAGE_SIZE - 1));
}

static void record_load_bounds(struct elf_load_layout *layout, vaddr_t start,
			       vaddr_t end)
{
	if (!layout->loaded_segment || start < layout->first_vaddr)
		layout->first_vaddr = start;
	if (!layout->loaded_segment || end > layout->last_end)
		layout->last_end = end;
	layout->loaded_segment = true;
}

static int load_elf_segment(struct exec_image *image, struct mm_struct *mm,
			    const Elf64_Phdr *ph,
			    struct elf_load_layout *layout)
{
	vaddr_t seg_start;
	vaddr_t seg_end;
	int ret;

	if (!elf_phdr_loadable(ph))
		return 0;

	ret = validate_load_segment(image, ph, &seg_start, &seg_end);
	if (ret < 0)
		return ret;

	if (can_map_file_backed_text(ph)) {
		ret = mm_map_file_segment(mm, image->file, seg_start, seg_end,
					  elf_flags_to_prot(ph->p_flags),
					  ph->p_offset);
		if (ret < 0)
			return ret;
	} else {
		ret = map_load_segment(image, mm, ph, seg_start, seg_end);
		if (ret < 0)
			return ret;
		ret = mm_map_segment(mm, seg_start, seg_end,
				     elf_flags_to_prot(ph->p_flags));
		if (ret < 0)
			return ret;
	}

	record_load_bounds(layout, seg_start, seg_end);
	return 0;
}

static int load_elf_segments(struct exec_image *image,
			     const struct elf_phdr_table *phdrs,
			     struct mm_struct *mm,
			     struct elf_load_layout *layout)
{
	for (int i = 0; i < phdrs->count; i++) {
		int ret =
			load_elf_segment(image, mm, &phdrs->entries[i], layout);

		if (ret < 0)
			return ret;
	}

	return layout->loaded_segment ? 0 : -ENOEXEC;
}

static int finish_exec_mm(struct mm_struct *mm,
			  const struct exec_args_envp *args, const char *path,
			  const Elf64_Ehdr *ehdr, vaddr_t phdr_addr,
			  struct elf_load_layout *layout, vaddr_t *sp_out)
{
	int ret;

	flush_icache();
	mm_flush_remote(mm, true);

	ret = mm_finalize(mm, layout->first_vaddr, layout->last_end);
	if (ret < 0)
		return ret;
	ret = setup_user_stack(mm, args, path, ehdr, phdr_addr, sp_out);
	return ret;
}

static int load_elf_file(struct exec_image *image,
			 const struct exec_args_envp *args, const char *path,
			 struct mm_struct **mm_out, vaddr_t *entry_out,
			 vaddr_t *sp_out)
{
	Elf64_Ehdr ehdr;
	struct elf_phdr_table phdrs = {0};
	struct elf_load_layout layout = {0};
	struct mm_struct *mm = NULL;
	vaddr_t user_sp = 0;
	vaddr_t phdr_addr = 0;
	int ret;

	*mm_out = NULL;
	*entry_out = 0;
	*sp_out = 0;

	ret = read_elf_header(image, &ehdr);
	if (ret < 0)
		return ret;

	ret = read_elf_phdr_table(image, &ehdr, &phdrs);
	if (ret < 0)
		return ret;
	ret = validate_elf_program_headers(&phdrs);
	if (ret < 0)
		goto fail;
	ret = elf_user_phdr_addr(&ehdr, &phdrs, &phdr_addr);
	if (ret < 0)
		goto fail;

	ret = create_exec_mm(&mm);
	if (ret < 0)
		goto fail;
	ret = load_elf_segments(image, &phdrs, mm, &layout);
	if (ret < 0)
		goto fail;
	ret = finish_exec_mm(mm, args, path, &ehdr, phdr_addr, &layout,
			     &user_sp);
	if (ret < 0)
		goto fail;

	free_elf_phdr_table(&phdrs);
	*mm_out = mm;
	*entry_out = ehdr.e_entry;
	*sp_out = user_sp;
	return 0;

fail:
	mm_put(mm);
	free_elf_phdr_table(&phdrs);
	return ret;
}

static void install_exec_mm(struct mm_struct *mm, struct trap_frame *tf,
			    vaddr_t entry, vaddr_t sp)
{
	struct task_struct *task = current_task();
	struct proc_struct *proc = task->proc;
	struct mm_struct *oldmm;
	uintptr_t pgroot = mm_pgroot(mm);

	oldmm = proc_replace_mm(proc, mm);
	task->arch.pgroot = pgroot;
	task->arch.tf = tf;

	activate_pgroot(task->arch.pgroot);
	sched_publish_active_mm(mm);
	mm_put(oldmm);

	memset(tf, 0, sizeof(*tf));
	trap_setup_user_return(tf, entry, sp);
}

void exec_user_path(const char *path)
{
	struct exec_args_envp *args __cleanup_with(kfree) = NULL;
	struct trap_frame tf_storage;
	size_t len;
	int ret;

	args = kzalloc(sizeof(*args), ALLOC_NOWAIT);
	if (!args)
		panic("exec: failed to allocate init args");

	args->argc = 1;
	len = strnlen(path, EXEC_MAX_ARGS - 1);
	memcpy(args->argv[0], path, len);
	args->argv[0][len] = '\0';

	ret = kernel_execve(path, args, &tf_storage);
	if (ret < 0)
		panic("exec: %s failed (%d)", path, ret);

	trapret_to_user(&tf_storage);
}

int kernel_execve(const char *path, const struct exec_args_envp *args,
		  struct trap_frame *tf)
{
	struct exec_image image;
	struct task_struct *task = current_task();
	struct proc_struct *proc = task ? task->proc : NULL;
	struct mm_struct *mm;
	struct files_struct *prepared_files = NULL;
	struct sighand_struct *prepared_sighand = NULL;
	vaddr_t entry;
	vaddr_t sp;
	const struct wait_deadline deadline = wait_deadline_none();
	bool exec_started = false;
	int ret;

	if (!path || !args || !tf || !task || !proc)
		return -EINVAL;

	ret = open_exec_image(path, &image);
	if (ret < 0)
		return ret;

	ret = load_elf_file(&image, args, path, &mm, &entry, &sp);
	close_exec_image(&image);
	if (ret < 0)
		return ret;

	ret = proc_exec_begin(proc, task);
	if (ret < 0) {
		mm_put(mm);
		return ret;
	}
	exec_started = true;

	ret = files_prepare_exec(proc, &prepared_files);
	if (ret < 0)
		goto abort_exec;
	ret = sig_exec_prepare(task, &prepared_sighand);
	if (ret < 0)
		goto abort_exec;
	ret = proc_exec_request_siblings(proc, task);
	if (ret < 0)
		goto abort_exec;

	for (;;) {
		struct task_wait *wait = &task->wait;
		wait_outcome_t outcome;

		ret = wait_start(wait, 0, &deadline);
		if (ret < 0)
			goto abort_exec;
		ret = proc_exec_wait(proc, wait);
		if (ret < 0) {
			wait_finish(wait);
			goto abort_exec;
		}
		if (ret > 0) {
			wait_finish(wait);
			break;
		}
		ret = wait_block(wait, &outcome);
		wait_finish(wait);
		if (ret < 0)
			goto abort_exec;
		BUG_ON(outcome != WAIT_OUTCOME_EVENT);
	}

	/* After this point every operation is a non-failing commit operation. */
	BUG_ON(proc_exec_adopt_pid(proc, task) < 0);
	files_close_on_exec(prepared_files);
	files_commit_exec(proc, prepared_files);
	prepared_files = NULL;
	sig_exec_commit(task, prepared_sighand);
	prepared_sighand = NULL;

	install_exec_mm(mm, tf, entry, sp);
	proc_mark_user_process(proc);
	restart_clear(task);
	rseq_execve(task);
	/* Thread-local registrations point into the old address space: drop
	 * them with the old mm instead of writing into released memory on a
	 * later exit.  No wake is published for exec. */
	{
		irq_flags_t flags;

		spin_lock_irqsave(&task->wait.lock, &flags);
		task_set_robust_list(task, NULL, 0);
		spin_unlock_irqrestore(&task->wait.lock, flags);
	}
	task_set_clear_child_tid(task, NULL);

	kernel_clone_complete_vfork(task);
	proc_exec_end(proc, task);

	return 0;

abort_exec:
	sig_exec_abort(prepared_sighand);
	files_abort_exec(prepared_files);
	if (exec_started)
		proc_exec_end(proc, task);
	mm_put(mm);
	return ret;
}
