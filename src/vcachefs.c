/*
 * vcachefs.c - Userspace video caching filesystem 
 *
 * Copyright 2008 Paul Betts <paul.betts@gmail.com>
 *
 *
 * License:
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this package; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <glib.h>

#include "vcachefs.h"


/* Utility Routines */

static struct vcachefs_mount* get_current_mountinfo(void)
{
	/* NOTE: This function *only* works inside a FUSE callout */
	struct fuse_context* ctx = fuse_get_context();
	return (ctx ? ctx->private_data : NULL);
}

static struct vcachefs_fdentry* fdentry_new(void)
{
	struct vcachefs_fdentry* ret = g_new0(struct vcachefs_fdentry, 1);
	ret->refcnt = 1;
	return ret;
}

static struct vcachefs_fdentry* fdentry_ref(struct vcachefs_fdentry* obj)
{
	g_atomic_int_inc(&obj->refcnt);
	return obj;
}

static void fdentry_unref(struct vcachefs_fdentry* obj)
{
	if(g_atomic_int_dec_and_test(&obj->refcnt))
		g_free(obj);
}

static struct vcachefs_fdentry* fdentry_from_fd(uint fd)
{
	struct vcachefs_fdentry* ret = NULL;
	struct vcachefs_mount* mount_obj = get_current_mountinfo();

	g_static_rw_lock_reader_lock(&mount_obj->fd_table_rwlock);
	ret = g_hash_table_lookup(mount_obj->fd_table, (gconstpointer)fd);
	g_static_rw_lock_reader_unlock(&mount_obj->fd_table_rwlock);

	return (ret ? fdentry_ref(ret) : NULL);
}


/*
 * FUSE callouts
 */

static void* vcachefs_init(struct fuse_conn_info *conn)
{
	struct vcachefs_mount* mount_object = g_new0(struct vcachefs_mount, 1);
	mount_object->source_path = "/etc"; 		/* XXX: Obviously dumb */

	/* Create the file descriptor table */
	mount_object->fd_table = g_hash_table_new(g_int_hash, g_int_equal);
	g_static_rw_lock_init(&mount_object->fd_table_rwlock);

	return mount_object;
}

static void trash_fdtable_item(gpointer key, gpointer val, gpointer dontcare) 
{ 
	fdentry_unref((struct vcachefs_fdentry*)val);
}

static void vcachefs_destroy(void *mount_object_ptr)
{
	struct vcachefs_mount* mount_object = mount_object_ptr;

	/* XXX: We need to make sure no one is using this before we trash it */
	g_hash_table_foreach(mount_object->fd_table, trash_fdtable_item, NULL);
	g_hash_table_destroy(mount_object->fd_table);
	mount_object->fd_table = NULL;
	g_free(mount_object);
}

static int vcachefs_getattr(const char *path, struct stat *stbuf)
{
	int ret = 0; 
	const struct vcachefs_mount* mount_obj = get_current_mountinfo();

	if(path == NULL || strlen(path) == 0)
		return -ENOENT;

	if(strcmp(path, "/") == 0)
		return stat(mount_obj->source_path, stbuf);

	gchar* full_path = g_strdup_printf("%s/%s", mount_obj->source_path, &path[1]);
	ret = stat((char *)full_path, stbuf);
	g_free(full_path);

	return ret;
}

static int vcachefs_open(const char *path, struct fuse_file_info *fi)
{
	struct vcachefs_mount* mount_obj = get_current_mountinfo();

	if(path == NULL || strlen(path) == 0)
		return -ENOENT;

	gchar* full_path = g_strdup_printf("%s/%s", mount_obj->source_path, &path[1]);

	int source_fd = open(full_path, fi->flags);
	if(source_fd <= 0) 
		return source_fd;

	/* Open succeeded - time to create a fdentry */
	struct vcachefs_fdentry* fde = fdentry_new();
	g_static_rw_lock_writer_lock(&mount_obj->fd_table_rwlock);
	fde->source_fd = source_fd;
	fi->fh = fde->fd = mount_obj->next_fd;
	mount_obj->next_fd++;
	g_hash_table_insert((gpointer)mount_obj->fd_table, fde->fd, fde);
	g_static_rw_lock_writer_unlock(&mount_obj->fd_table_rwlock);

	return 0;
}

static int vcachefs_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	int ret = 0;
	struct vcachefs_fdentry* fde = fdentry_from_fd(fi->fh);
	if(!fde)
		return -ENOENT;

	if(fde->source_offset != offset) {
		int tmp = lseek(fde->source_fd, offset, SEEK_SET);
		if (tmp < 0) {
			ret = tmp;
			goto out;
		}
	}

	ret = read(fde->source_fd, buf, size);
	if (ret >= 0)
		fde->source_offset = offset + ret;

out:
	fdentry_unref(fde);
	return size;
}


static int vcachefs_statfs(const char *path, struct statvfs *stat)
{
	struct vcachefs_mount* mount_obj = get_current_mountinfo();
	return statvfs(mount_obj->source_path, stat);
}


static int vcachefs_release(const char *path, struct fuse_file_info *info)
{
	/* Remove the entry from the fd table */
	struct vcachefs_mount* mount_obj = get_current_mountinfo();
	g_static_rw_lock_writer_lock(&mount_obj->fd_table_rwlock);
	struct vcachefs_fdentry* fde = g_hash_table_lookup(mount_obj->fd_table, (gpointer)info->fh);
	g_hash_table_remove(mount_obj->fd_table, (gpointer)info->fh);
	g_static_rw_lock_writer_unlock(&mount_obj->fd_table_rwlock);

	if(!fde)
		return -ENOENT;

	fdentry_unref(fde);

	return 0;
}


static int vcachefs_access(const char *path, int amode)
{
	int ret = 0; 
	const struct vcachefs_mount* mount_obj = get_current_mountinfo();

	if(path == NULL || strlen(path) == 0)
		return -ENOENT;

	if(strcmp(path, "/") == 0)
		return access(mount_obj->source_path, amode);

	gchar* full_path = g_strdup_printf("%s/%s", mount_obj->source_path, &path[1]);
	ret = access((char *)full_path, amode);
	g_free(full_path);

	return ret;

}

static int vcachefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi)
{
	/*
	(void) offset;
	(void) fi;

	if(strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	filler(buf, vcachefs_path + 1, NULL, 0);
	*/

	return 0;
}


/*
 * Main
 */

static struct fuse_operations vcachefs_oper = {
	.getattr	= vcachefs_getattr,
	/*.readlink 	= vcachefs_readlink, */
	.open 		= vcachefs_open,
	.read		= vcachefs_read,
	.statfs 	= vcachefs_statfs,
	/* TODO: do we need flush? */
	.release 	= vcachefs_release,
	.init 		= vcachefs_init,
	.destroy 	= vcachefs_destroy,
	.access 	= vcachefs_access,

	/* TODO: implement these later
	.getxattr 	= vcachefs_getxattr,
	.listxattr 	= vcachefs_listxattr,
	.opendir 	= vcachefs_opendir, */
	.readdir	= vcachefs_readdir,
	/*.releasedir 	= vcachefs_releasedir,
	.fsyncdir 	= vcachefs_fsyncdir, */
};

int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &vcachefs_oper, NULL);
}
