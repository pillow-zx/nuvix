/*
 * syscall/sys_exec.c - execve ABI wrapper
 */

#include <kernel/buddy.h>
#include <kernel/errno.h>
#include <kernel/exec.h>
#include <kernel/fs.h>
#include <kernel/mm.h>
#include <kernel/syscall.h>
#include <kernel/page.h>
#include <kernel/slab.h>
#include <kernel/trap.h>

static int copy_user_string(char *dst, const char *user, size_t max_len)
{
	ssize_t len = strncpy_from_user(dst, user, max_len);

	if (len == -ENAMETOOLONG)
		return -E2BIG;
	return len < 0 ? (int)len : 0;
}

static int copy_arg_array(const char *const *user_array,
			  struct exec_args_envp *args)
{
	args->argc = 0;

	if (!user_array)
		return 0;

	for (int i = 0; i < EXEC_MAX_ARGS; i++) {
		const char *user_string;

		if (copy_from_user(&user_string, &user_array[i],
				   sizeof(user_string)) != 0)
			return -EFAULT;
		if (!user_string)
			return 0;

		int ret = copy_user_string(args->argv[i], user_string,
					   EXEC_MAX_ARG_LEN);
		if (ret < 0)
			return ret;

		args->argc++;
	}

	const char *extra;
	if (copy_from_user(&extra, &user_array[EXEC_MAX_ARGS], sizeof(extra)) !=
	    0)
		return -EFAULT;
	if (extra)
		return -E2BIG;

	return 0;
}

static int copy_env_array(const char *const *user_array,
			  struct exec_args_envp *args)
{
	args->envc = 0;

	if (!user_array)
		return 0;

	for (int i = 0; i < EXEC_MAX_ENVS; i++) {
		const char *user_string;

		if (copy_from_user(&user_string, &user_array[i],
				   sizeof(user_string)) != 0)
			return -EFAULT;
		if (!user_string)
			return 0;

		int ret = copy_user_string(args->envp[i], user_string,
					   EXEC_MAX_ENV_LEN);
		if (ret < 0)
			return ret;

		args->envc++;
	}

	const char *extra;
	if (copy_from_user(&extra, &user_array[EXEC_MAX_ENVS], sizeof(extra)) !=
	    0)
		return -EFAULT;
	if (extra)
		return -E2BIG;

	return 0;
}

static int copy_exec_args(const char *const *uargv, const char *const *uenvp,
			  struct exec_args_envp *args)
{
	int ret;

	memset(args, 0, sizeof(*args));

	ret = copy_arg_array(uargv, args);
	if (ret < 0)
		return ret;

	return copy_env_array(uenvp, args);
}

/*
 * SYSCALL_SUPPORT(A): execve
 * Current: validates and copies ABI inputs, then delegates the transactional
 * image replacement and close-on-exec fd lifecycle to kernel_execve().
 */
ssize_t sys_execve(struct trap_frame *tf)
{
	const char *upath = (const char *)syscall_arg(tf, 0);
	const char *const *uargv = (const char *const *)syscall_arg(tf, 1);
	const char *const *uenvp = (const char *const *)syscall_arg(tf, 2);
	struct exec_args_envp *args __cleanup_with(kfree) = NULL;
	char *path __cleanup_with(page0) = NULL;
	ssize_t path_len;
	int ret;

	path = get_free_page(0, ALLOC_NOWAIT);
	if (!path)
		return -ENOMEM;

	path_len = strncpy_from_user(path, upath, VFS_PATH_MAX);
	if (path_len < 0)
		return (int)path_len;

	if (path_len == 0)
		return -ENOENT;

	args = kmalloc(sizeof(*args), ALLOC_NOWAIT);
	if (!args)
		return -ENOMEM;

	ret = copy_exec_args(uargv, uenvp, args);
	if (ret < 0)
		return ret;

	ret = kernel_execve(path, args, tf);

	return ret;
}
