/*
 * fs/vfs/read_write.c - VFS 读写入口
 */

#include <nuvix/errno.h>
#include <nuvix/fs.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define VFS_COPY_BUF_SIZE 256

static bool vfs_pos_locked(const struct file *file)
{
	return file && file->f_inode && S_ISREG(file->f_inode->i_mode);
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

	if (!file || !(file->f_mode & FMODE_READ) || !file->f_op ||
	    !file->f_op->read)
		return -EBADF;

	if (vfs_pos_locked(file))
		mutex_lock(&file->f_lock);
	ret = vfs_read_core(file, buf, count, &file->f_pos);
	if (vfs_pos_locked(file))
		mutex_unlock(&file->f_lock);

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

	if (!file || !(file->f_mode & FMODE_WRITE) || !file->f_op ||
	    !file->f_op->write)
		return -EBADF;

	if (vfs_pos_locked(file))
		mutex_lock(&file->f_lock);
	ret = vfs_write_core(file, buf, count, &file->f_pos);
	if (vfs_pos_locked(file))
		mutex_unlock(&file->f_lock);

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
	if (!file || count <= 0)
		return;
	if (vfs_pos_locked(file))
		mutex_lock(&file->f_lock);
	file->f_pos -= count;
	if (vfs_pos_locked(file))
		mutex_unlock(&file->f_lock);
}

ssize_t vfs_copy_file_buffered(struct file *out_file, struct file *in_file,
			       loff_t *in_pos, loff_t *out_pos, size_t len)
{
	char kbuf[VFS_COPY_BUF_SIZE];
	ssize_t total = 0;

	while (len > 0) {
		loff_t old_in_pos = in_pos ? *in_pos : 0;
		size_t chunk = len;
		ssize_t nr_read;
		ssize_t nr_written;

		if (chunk > VFS_COPY_BUF_SIZE)
			chunk = VFS_COPY_BUF_SIZE;

		nr_read = vfs_read_pos(in_file, kbuf, chunk, in_pos);
		if (nr_read < 0)
			return total ? total : nr_read;
		if (nr_read == 0)
			break;

		nr_written = vfs_write_pos(out_file, kbuf, (size_t)nr_read,
					   out_pos);
		if (nr_written < 0) {
			if (in_pos)
				*in_pos = old_in_pos;
			else
				vfs_rewind_pos(in_file, nr_read);
			return total ? total : nr_written;
		}
		if (nr_written == 0) {
			if (in_pos)
				*in_pos = old_in_pos;
			else
				vfs_rewind_pos(in_file, nr_read);
			break;
		}

		if (in_pos && nr_written < nr_read)
			*in_pos -= nr_read - nr_written;
		else if (!in_pos && nr_written < nr_read)
			vfs_rewind_pos(in_file, nr_read - nr_written);

		total += nr_written;
		len -= (size_t)nr_written;
		if (nr_written < nr_read)
			break;
	}

	return total;
}

loff_t vfs_llseek(struct file *file, loff_t offset, int whence)
{
	loff_t base;
	bool locked;

	if (!file)
		return -EBADF;

	locked = vfs_pos_locked(file);
	if (file->f_op && file->f_op->llseek) {
		loff_t result;

		if (locked)
			mutex_lock(&file->f_lock);
		result = file->f_op->llseek(file, offset, whence);
		if (locked)
			mutex_unlock(&file->f_lock);
		return result;
	}

	if (locked)
		mutex_lock(&file->f_lock);
	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = file->f_pos;
		break;
	case SEEK_END:
		if (!file->f_inode) {
			if (locked)
				mutex_unlock(&file->f_lock);
			return -ESPIPE;
		}
		base = (loff_t)file->f_inode->i_size;
		break;
	default:
		if (locked)
			mutex_unlock(&file->f_lock);
		return -EINVAL;
	}

	if (offset < 0 && base < -offset) {
		if (locked)
			mutex_unlock(&file->f_lock);
		return -EINVAL;
	}

	file->f_pos = base + offset;
	if (locked)
		mutex_unlock(&file->f_lock);
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
