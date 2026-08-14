/*
 * fs/vfs/read_write.c - VFS 读写入口
 */

#include <nuvix/errno.h>
#include <nuvix/fs.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define VFS_COPY_BUF_SIZE 256

ssize_t vfs_read(struct file *file, char *buf, size_t count)
{
	if (!file || !(file->f_mode & FMODE_READ) || !file->f_op ||
	    !file->f_op->read)
		return -EBADF;

	ssize_t ret = file->f_op->read(file, buf, count);
	if (ret > 0)
		file->f_pos += ret;

	return ret;
}

ssize_t vfs_write(struct file *file, const char *buf, size_t count)
{
	if (!file || !(file->f_mode & FMODE_WRITE) || !file->f_op ||
	    !file->f_op->write)
		return -EBADF;

	if ((file->f_flags & O_APPEND) && file->f_inode)
		file->f_pos = (loff_t)file->f_inode->i_size;

	ssize_t ret = file->f_op->write(file, buf, count);
	if (ret > 0)
		file->f_pos += ret;

	return ret;
}

ssize_t vfs_read_pos(struct file *file, char *buf, size_t count, loff_t *pos)
{
	loff_t old_pos;
	ssize_t ret;

	if (!pos)
		return vfs_read(file, buf, count);
	if (*pos < 0)
		return -EINVAL;
	if (!file)
		return -EBADF;

	old_pos = file->f_pos;
	file->f_pos = *pos;
	ret = vfs_read(file, buf, count);
	if (ret > 0)
		*pos = file->f_pos;
	file->f_pos = old_pos;
	return ret;
}

ssize_t vfs_write_pos(struct file *file, const char *buf, size_t count,
		      loff_t *pos)
{
	loff_t old_pos;
	ssize_t ret;

	if (!pos)
		return vfs_write(file, buf, count);
	if (*pos < 0)
		return -EINVAL;
	if (!file)
		return -EBADF;

	old_pos = file->f_pos;
	file->f_pos = *pos;
	ret = vfs_write(file, buf, count);
	if (ret > 0)
		*pos = file->f_pos;
	file->f_pos = old_pos;
	return ret;
}

void vfs_rewind_pos(struct file *file, loff_t count)
{
	if (!file || count <= 0)
		return;
	file->f_pos -= count;
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
				in_file->f_pos -= nr_read;
			return total ? total : nr_written;
		}
		if (nr_written == 0) {
			if (in_pos)
				*in_pos = old_in_pos;
			else
				in_file->f_pos -= nr_read;
			break;
		}

		if (in_pos && nr_written < nr_read)
			*in_pos -= nr_read - nr_written;
		else if (!in_pos && nr_written < nr_read)
			in_file->f_pos -= nr_read - nr_written;

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

	if (!file)
		return -EBADF;

	if (file->f_op && file->f_op->llseek)
		return file->f_op->llseek(file, offset, whence);

	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = file->f_pos;
		break;
	case SEEK_END:
		if (!file->f_inode)
			return -ESPIPE;
		base = (loff_t)file->f_inode->i_size;
		break;
	default:
		return -EINVAL;
	}

	if (offset < 0 && base < -offset)
		return -EINVAL;

	file->f_pos = base + offset;
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
