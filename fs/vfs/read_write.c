/*
 * fs/vfs/read_write.c - VFS 读写入口
 */

#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/mutex.h>

#define SEEK_SET	  0
#define SEEK_CUR	  1
#define SEEK_END	  2
#define VFS_COPY_BUF_SIZE 256

/* Single implicit file-position mutex for the whole VFS layer.  The rank
 * contract forbids nesting two LOCK_RANK_FILE_POSITION locks, so per-file
 * f_pos locking cannot cover sendfile-style reads from one file and writes
 * to another.  One global lock keeps every implicit-position path (read,
 * write, seek, rewind, buffered copy) mutually exclusive; explicit-position
 * I/O never takes it. */
static DEFINE_MUTEX(vfs_fpos_lock, LOCK_RANK_FILE_POSITION,
		    LOCK_IRQ_TASK_ONLY);

static bool vfs_pos_implicit(const struct file *file)
{
	return file && file->f_inode && S_ISREG(file->f_inode->i_mode);
}

static bool vfs_pos_lock(const struct file *file)
{
	if (!vfs_pos_implicit(file))
		return false;
	mutex_lock(&vfs_fpos_lock);
	return true;
}

static void vfs_pos_unlock(bool locked)
{
	if (locked)
		mutex_unlock(&vfs_fpos_lock);
}

static ssize_t vfs_read_core(struct file *file, char *buf, size_t count,
			     loff_t *pos)
{
	ssize_t ret = file->f_op->read(file, buf, count, *pos);

	if (ret > 0)
		*pos += ret;
	return ret;
}

ssize_t vfs_read(struct file *file, char *buf, size_t count)
{
	ssize_t ret;
	bool locked;

	if (!file || !(file->f_mode & FMODE_READ) || !file->f_op ||
	    !file->f_op->read)
		return -EBADF;

	locked = vfs_pos_lock(file);
	ret = vfs_read_core(file, buf, count, &file->f_pos);
	vfs_pos_unlock(locked);

	return ret;
}

static ssize_t vfs_write_core(struct file *file, const char *buf, size_t count,
			      loff_t *pos)
{
	ssize_t ret;

	if ((file->f_flags & O_APPEND) && file->f_inode)
		*pos = (loff_t)file->f_inode->i_size;

	ret = file->f_op->write(file, buf, count, *pos);
	if (ret > 0)
		*pos += ret;

	return ret;
}

ssize_t vfs_write(struct file *file, const char *buf, size_t count)
{
	ssize_t ret;
	bool locked;

	if (!file || !(file->f_mode & FMODE_WRITE) || !file->f_op ||
	    !file->f_op->write)
		return -EBADF;

	locked = vfs_pos_lock(file);
	ret = vfs_write_core(file, buf, count, &file->f_pos);
	vfs_pos_unlock(locked);

	return ret;
}

ssize_t vfs_read_pos(struct file *file, char *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	if (!pos)
		return vfs_read(file, buf, count);
	if (*pos < 0)
		return -EINVAL;
	if (!file || !(file->f_mode & FMODE_READ) || !file->f_op ||
	    !file->f_op->read)
		return -EBADF;

	ret = vfs_read_core(file, buf, count, pos);
	return ret;
}

ssize_t vfs_write_pos(struct file *file, const char *buf, size_t count,
		      loff_t *pos)
{
	ssize_t ret;

	if (!pos)
		return vfs_write(file, buf, count);
	if (*pos < 0)
		return -EINVAL;
	if (!file || !(file->f_mode & FMODE_WRITE) || !file->f_op ||
	    !file->f_op->write)
		return -EBADF;

	ret = vfs_write_core(file, buf, count, pos);
	return ret;
}

void vfs_rewind_pos(struct file *file, loff_t count)
{
	bool locked;

	if (!file || count <= 0)
		return;
	locked = vfs_pos_lock(file);
	file->f_pos -= count;
	vfs_pos_unlock(locked);
}

ssize_t vfs_copy_file_buffered(struct file *out_file, struct file *in_file,
			       loff_t *in_pos, loff_t *out_pos, size_t len)
{
	char kbuf[VFS_COPY_BUF_SIZE];
	ssize_t total = 0;
	bool locked = false;

	/* One hold of the global position lock covers the implicit positions
	 * of both files; no per-file lock ordering exists anymore. */
	if (!in_pos && !out_pos && vfs_pos_implicit(in_file))
		locked = vfs_pos_lock(in_file);
	else if (!out_pos && vfs_pos_implicit(out_file))
		locked = vfs_pos_lock(out_file);

	while (len > 0) {
		loff_t input_cursor = in_pos ? *in_pos : in_file->f_pos;
		loff_t output_cursor = out_pos ? *out_pos : out_file->f_pos;
		size_t chunk = len;
		ssize_t nr_read;
		ssize_t nr_written;

		if (chunk > VFS_COPY_BUF_SIZE)
			chunk = VFS_COPY_BUF_SIZE;

		nr_read =
			in_file->f_op->read(in_file, kbuf, chunk, input_cursor);
		if (nr_read < 0) {
			if (!total)
				total = nr_read;
			break;
		}
		if (nr_read == 0)
			break;
		input_cursor += nr_read;

		if ((out_file->f_flags & O_APPEND) && out_file->f_inode)
			output_cursor = (loff_t)out_file->f_inode->i_size;
		nr_written = out_file->f_op->write(
			out_file, kbuf, (size_t)nr_read, output_cursor);
		if (nr_written < 0) {
			if (!total)
				total = nr_written;
			break;
		}
		if (nr_written == 0) {
			break;
		}
		output_cursor += nr_written;
		if (in_pos)
			*in_pos += nr_written;
		else
			in_file->f_pos += nr_written;
		if (out_pos)
			*out_pos = output_cursor;
		else
			out_file->f_pos = output_cursor;

		total += nr_written;
		len -= (size_t)nr_written;
		if (nr_written < nr_read)
			break;
	}
	vfs_pos_unlock(locked);

	return total;
}

loff_t vfs_llseek(struct file *file, loff_t offset, int whence)
{
	loff_t base;
	bool locked;

	if (!file)
		return -EBADF;

	locked = vfs_pos_lock(file);
	if (file->f_op && file->f_op->llseek) {
		loff_t result = file->f_op->llseek(file, offset, whence);

		vfs_pos_unlock(locked);
		return result;
	}

	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = file->f_pos;
		break;
	case SEEK_END:
		if (!file->f_inode) {
			vfs_pos_unlock(locked);
			return -ESPIPE;
		}
		base = (loff_t)file->f_inode->i_size;
		break;
	default:
		vfs_pos_unlock(locked);
		return -EINVAL;
	}

	if (offset < 0 && base < -offset) {
		vfs_pos_unlock(locked);
		return -EINVAL;
	}

	file->f_pos = base + offset;
	vfs_pos_unlock(locked);
	return file->f_pos;
}

int vfs_readdir(struct file *file, void *ctx, filldir_t filldir)
{
	if (!file || !file->f_op || !file->f_op->readdir)
		return -EBADF;
	if (!filldir)
		return -EINVAL;

	return file->f_op->readdir(file, ctx, filldir);
}
