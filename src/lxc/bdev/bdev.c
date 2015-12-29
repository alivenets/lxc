/*
 * lxc: linux Container library
 *
 * (C) Copyright IBM Corp. 2007, 2008
 *
 * Authors:
 * Daniel Lezcano <daniel.lezcano at free.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * this is all just a first shot for experiment.  If we go this route, much
 * should change.  bdev should be a directory with per-bdev file.  Things which
 * I'm doing by calling out to userspace should sometimes be done through
 * libraries like liblvm2
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <libgen.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/loop.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "bdev.h"
#include "conf.h"
#include "config.h"
#include "error.h"
#include "log.h"
#include "lxc.h"
#include "lxcbtrfs.h"
#include "lxclock.h"
#include "lxcoverlay.h"
#include "lxcrsync.h"
#include "lxczfs.h"
#include "namespace.h"
#include "parse.h"
#include "utils.h"

#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12,114,size_t)
#endif

#ifndef LO_FLAGS_AUTOCLEAR
#define LO_FLAGS_AUTOCLEAR 4
#endif

#ifndef LOOP_CTL_GET_FREE
#define LOOP_CTL_GET_FREE 0x4C82
#endif

#define DEFAULT_FS_SIZE 1073741824
#define DEFAULT_FSTYPE "ext3"

lxc_log_define(bdev, lxc);

/* the bulk of this needs to become a common helper */
char *dir_new_path(char *src, const char *oldname, const char *name,
		   const char *oldpath, const char *lxcpath)
{
	char *ret, *p, *p2;
	int l1, l2, nlen;

	nlen = strlen(src) + 1;
	l1 = strlen(oldpath);
	p = src;
	/* if src starts with oldpath, look for oldname only after
	 * that path */
	if (strncmp(src, oldpath, l1) == 0) {
		p += l1;
		nlen += (strlen(lxcpath) - l1);
	}
	l2 = strlen(oldname);
	while ((p = strstr(p, oldname)) != NULL) {
		p += l2;
		nlen += strlen(name) - l2;
	}

	ret = malloc(nlen);
	if (!ret)
		return NULL;

	p = ret;
	if (strncmp(src, oldpath, l1) == 0) {
		p += sprintf(p, "%s", lxcpath);
		src += l1;
	}

	while ((p2 = strstr(src, oldname)) != NULL) {
		strncpy(p, src, p2-src); // copy text up to oldname
		p += p2-src; // move target pointer (p)
		p += sprintf(p, "%s", name); // print new name in place of oldname
		src = p2 + l2;  // move src to end of oldname
	}
	sprintf(p, "%s", src);  // copy the rest of src
	return ret;
}

/*
 * return block size of dev->src in units of bytes
 */
static int blk_getsize(struct bdev *bdev, uint64_t *size)
{
	int fd, ret;
	char *path = bdev->src;

	if (strcmp(bdev->type, "loop") == 0)
		path = bdev->src + 5;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	ret = ioctl(fd, BLKGETSIZE64, size); // size of device in bytes
	close(fd);
	return ret;
}

/*
 * These are copied from conf.c.  However as conf.c will be moved to using
 * the callback system, they can be pulled from there eventually, so we
 * don't need to pollute utils.c with these low level functions
 */
static int find_fstype_cb(char* buffer, void *data)
{
	struct cbarg {
		const char *rootfs;
		const char *target;
		const char *options;
	} *cbarg = data;

	unsigned long mntflags;
	char *mntdata;
	char *fstype;

	/* we don't try 'nodev' entries */
	if (strstr(buffer, "nodev"))
		return 0;

	fstype = buffer;
	fstype += lxc_char_left_gc(fstype, strlen(fstype));
	fstype[lxc_char_right_gc(fstype, strlen(fstype))] = '\0';

	DEBUG("trying to mount '%s'->'%s' with fstype '%s'",
	      cbarg->rootfs, cbarg->target, fstype);

	if (parse_mntopts(cbarg->options, &mntflags, &mntdata) < 0) {
		free(mntdata);
		return 0;
	}

	if (mount(cbarg->rootfs, cbarg->target, fstype, mntflags, mntdata)) {
		DEBUG("mount failed with error: %s", strerror(errno));
		free(mntdata);
		return 0;
	}

	free(mntdata);

	INFO("mounted '%s' on '%s', with fstype '%s'",
	     cbarg->rootfs, cbarg->target, fstype);

	return 1;
}

static int mount_unknown_fs(const char *rootfs, const char *target,
			                const char *options)
{
	int i;

	struct cbarg {
		const char *rootfs;
		const char *target;
		const char *options;
	} cbarg = {
		.rootfs = rootfs,
		.target = target,
		.options = options,
	};

	/*
	 * find the filesystem type with brute force:
	 * first we check with /etc/filesystems, in case the modules
	 * are auto-loaded and fall back to the supported kernel fs
	 */
	char *fsfile[] = {
		"/etc/filesystems",
		"/proc/filesystems",
	};

	for (i = 0; i < sizeof(fsfile)/sizeof(fsfile[0]); i++) {

		int ret;

		if (access(fsfile[i], F_OK))
			continue;

		ret = lxc_file_for_each_line(fsfile[i], find_fstype_cb, &cbarg);
		if (ret < 0) {
			ERROR("failed to parse '%s'", fsfile[i]);
			return -1;
		}

		if (ret)
			return 0;
	}

	ERROR("failed to determine fs type for '%s'", rootfs);
	return -1;
}

static int do_mkfs(const char *path, const char *fstype)
{
	pid_t pid;

	if ((pid = fork()) < 0) {
		ERROR("error forking");
		return -1;
	}
	if (pid > 0)
		return wait_for_pid(pid);

	// If the file is not a block device, we don't want mkfs to ask
	// us about whether to proceed.
	if (null_stdfds() < 0)
		exit(1);
	execlp("mkfs", "mkfs", "-t", fstype, path, NULL);
	exit(1);
}

static char *linkderef(char *path, char *dest)
{
	struct stat sbuf;
	ssize_t ret;

	ret = stat(path, &sbuf);
	if (ret < 0)
		return NULL;
	if (!S_ISLNK(sbuf.st_mode))
		return path;
	ret = readlink(path, dest, MAXPATHLEN);
	if (ret < 0) {
		SYSERROR("error reading link %s", path);
		return NULL;
	} else if (ret >= MAXPATHLEN) {
		ERROR("link in %s too long", path);
		return NULL;
	}
	dest[ret] = '\0';
	return dest;
}

/*
 * Given a bdev (presumably blockdev-based), detect the fstype
 * by trying mounting (in a private mntns) it.
 * @bdev: bdev to investigate
 * @type: preallocated char* in which to write the fstype
 * @len: length of passed in char*
 * Returns length of fstype, of -1 on error
 */
static int detect_fs(struct bdev *bdev, char *type, int len)
{
	int  p[2], ret;
	size_t linelen;
	pid_t pid;
	FILE *f;
	char *sp1, *sp2, *sp3, *line = NULL;
	char *srcdev;

	if (!bdev || !bdev->src || !bdev->dest)
		return -1;

	srcdev = bdev->src;
	if (strcmp(bdev->type, "loop") == 0)
		srcdev = bdev->src + 5;

	ret = pipe(p);
	if (ret < 0)
		return -1;
	if ((pid = fork()) < 0)
		return -1;
	if (pid > 0) {
		int status;
		close(p[1]);
		memset(type, 0, len);
		ret = read(p[0], type, len-1);
		close(p[0]);
		if (ret < 0) {
			SYSERROR("error reading from pipe");
			wait(&status);
			return -1;
		} else if (ret == 0) {
			ERROR("child exited early - fstype not found");
			wait(&status);
			return -1;
		}
		wait(&status);
		type[len-1] = '\0';
		INFO("detected fstype %s for %s", type, srcdev);
		return ret;
	}

	if (unshare(CLONE_NEWNS) < 0)
		exit(1);

	if (detect_shared_rootfs()) {
		if (mount(NULL, "/", NULL, MS_SLAVE|MS_REC, NULL)) {
			SYSERROR("Failed to make / rslave");
			ERROR("Continuing...");
		}
	}

	ret = mount_unknown_fs(srcdev, bdev->dest, bdev->mntopts);
	if (ret < 0) {
		ERROR("failed mounting %s onto %s to detect fstype", srcdev, bdev->dest);
		exit(1);
	}
	// if symlink, get the real dev name
	char devpath[MAXPATHLEN];
	char *l = linkderef(srcdev, devpath);
	if (!l)
		exit(1);
	f = fopen("/proc/self/mounts", "r");
	if (!f)
		exit(1);
	while (getline(&line, &linelen, f) != -1) {
		sp1 = strchr(line, ' ');
		if (!sp1)
			exit(1);
		*sp1 = '\0';
		if (strcmp(line, l))
			continue;
		sp2 = strchr(sp1+1, ' ');
		if (!sp2)
			exit(1);
		*sp2 = '\0';
		sp3 = strchr(sp2+1, ' ');
		if (!sp3)
			exit(1);
		*sp3 = '\0';
		sp2++;
		if (write(p[1], sp2, strlen(sp2)) != strlen(sp2))
			exit(1);
		exit(0);
	}
	exit(1);
}

struct bdev_type {
	const char *name;
	const struct bdev_ops *ops;
};

static int dir_detect(const char *path)
{
	if (strncmp(path, "dir:", 4) == 0)
		return 1; // take their word for it
	if (is_dir(path))
		return 1;
	return 0;
}

//
// XXXXXXX plain directory bind mount ops
//
static int dir_mount(struct bdev *bdev)
{
	unsigned long mntflags;
	char *mntdata;
	int ret;

	if (strcmp(bdev->type, "dir"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;

	if (parse_mntopts(bdev->mntopts, &mntflags, &mntdata) < 0) {
		free(mntdata);
		return -22;
	}

	ret = mount(bdev->src, bdev->dest, "bind", MS_BIND | MS_REC | mntflags, mntdata);
	free(mntdata);
	return ret;
}

static int dir_umount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "dir"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	return umount(bdev->dest);
}

/*
 * for a simple directory bind mount, we substitute the old container
 * name and paths for the new
 */
static int dir_clonepaths(struct bdev *orig, struct bdev *new, const char *oldname,
		const char *cname, const char *oldpath, const char *lxcpath, int snap,
		uint64_t newsize, struct lxc_conf *conf)
{
	int len, ret;

	if (snap) {
		ERROR("directories cannot be snapshotted.  Try aufs or overlayfs.");
		return -1;
	}

	if (!orig->dest || !orig->src)
		return -1;

	len = strlen(lxcpath) + strlen(cname) + strlen("rootfs") + 3;
	new->src = malloc(len);
	if (!new->src)
		return -1;
	ret = snprintf(new->src, len, "%s/%s/rootfs", lxcpath, cname);
	if (ret < 0 || ret >= len)
		return -1;
	if ((new->dest = strdup(new->src)) == NULL)
		return -1;

	return 0;
}

static int dir_destroy(struct bdev *orig)
{
	if (lxc_rmdir_onedev(orig->src, NULL) < 0)
		return -1;
	return 0;
}

static int dir_create(struct bdev *bdev, const char *dest, const char *n,
			struct bdev_specs *specs)
{
	if (specs && specs->dir)
		bdev->src = strdup(specs->dir);
	else
		bdev->src = strdup(dest);
	bdev->dest = strdup(dest);
	if (!bdev->src || !bdev->dest) {
		ERROR("Out of memory");
		return -1;
	}

	if (mkdir_p(bdev->src, 0755) < 0) {
		ERROR("Error creating %s", bdev->src);
		return -1;
	}
	if (mkdir_p(bdev->dest, 0755) < 0) {
		ERROR("Error creating %s", bdev->dest);
		return -1;
	}

	return 0;
}

static const struct bdev_ops dir_ops = {
	.detect = &dir_detect,
	.mount = &dir_mount,
	.umount = &dir_umount,
	.clone_paths = &dir_clonepaths,
	.destroy = &dir_destroy,
	.create = &dir_create,
	.can_snapshot = false,
	.can_backup = true,
};

//
// LVM ops
//

/*
 * Look at /sys/dev/block/maj:min/dm/uuid.  If it contains the hardcoded LVM
 * prefix "LVM-", then this is an lvm2 LV
 */
static int lvm_detect(const char *path)
{
	char devp[MAXPATHLEN], buf[4];
	FILE *fout;
	int ret;
	struct stat statbuf;

	if (strncmp(path, "lvm:", 4) == 0)
		return 1; // take their word for it

	ret = stat(path, &statbuf);
	if (ret != 0)
		return 0;
	if (!S_ISBLK(statbuf.st_mode))
		return 0;

	ret = snprintf(devp, MAXPATHLEN, "/sys/dev/block/%d:%d/dm/uuid",
			major(statbuf.st_rdev), minor(statbuf.st_rdev));
	if (ret < 0 || ret >= MAXPATHLEN) {
		ERROR("lvm uuid pathname too long");
		return 0;
	}
	fout = fopen(devp, "r");
	if (!fout)
		return 0;
	ret = fread(buf, 1, 4, fout);
	fclose(fout);
	if (ret != 4 || strncmp(buf, "LVM-", 4) != 0)
		return 0;
	return 1;
}

static int lvm_mount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "lvm"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	/* if we might pass in data sometime, then we'll have to enrich
	 * mount_unknown_fs */
	return mount_unknown_fs(bdev->src, bdev->dest, bdev->mntopts);
}

static int lvm_umount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "lvm"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	return umount(bdev->dest);
}

static int lvm_compare_lv_attr(const char *path, int pos, const char expected) {
	struct lxc_popen_FILE *f;
	int ret, len, status, start=0;
	char *cmd, output[12];
	const char *lvscmd = "lvs --unbuffered --noheadings -o lv_attr %s 2>/dev/null";

	len = strlen(lvscmd) + strlen(path) - 1;
	cmd = alloca(len);

	ret = snprintf(cmd, len, lvscmd, path);
	if (ret < 0 || ret >= len)
		return -1;

	f = lxc_popen(cmd);

	if (f == NULL) {
		SYSERROR("popen failed");
		return -1;
	}

	ret = fgets(output, 12, f->f) == NULL;

	status = lxc_pclose(f);

	if (ret || WEXITSTATUS(status))
		// Assume either vg or lvs do not exist, default
		// comparison to false.
		return 0;

	len = strlen(output);
	while(start < len && output[start] == ' ') start++;

	if (start + pos < len && output[start + pos] == expected)
		return 1;

	return 0;
}

static int lvm_is_thin_volume(const char *path)
{
	return lvm_compare_lv_attr(path, 6, 't');
}

static int lvm_is_thin_pool(const char *path)
{
	return lvm_compare_lv_attr(path, 0, 't');
}

/*
 * path must be '/dev/$vg/$lv', $vg must be an existing VG, and $lv must not
 * yet exist.  This function will attempt to create /dev/$vg/$lv of size
 * $size. If thinpool is specified, we'll check for it's existence and if it's
 * a valid thin pool, and if so, we'll create the requested lv from that thin
 * pool.
 */
static int do_lvm_create(const char *path, uint64_t size, const char *thinpool)
{
	int ret, pid, len;
	char sz[24], *pathdup, *vg, *lv, *tp = NULL;

	if ((pid = fork()) < 0) {
		SYSERROR("failed fork");
		return -1;
	}
	if (pid > 0)
		return wait_for_pid(pid);

	// specify bytes to lvcreate
	ret = snprintf(sz, 24, "%"PRIu64"b", size);
	if (ret < 0 || ret >= 24)
		exit(1);

	pathdup = strdup(path);
	if (!pathdup)
		exit(1);

	lv = strrchr(pathdup, '/');
	if (!lv)
		exit(1);

	*lv = '\0';
	lv++;

	vg = strrchr(pathdup, '/');
	if (!vg)
		exit(1);
	vg++;

	if (thinpool) {
		len = strlen(pathdup) + strlen(thinpool) + 2;
		tp = alloca(len);

		ret = snprintf(tp, len, "%s/%s", pathdup, thinpool);
		if (ret < 0 || ret >= len)
			exit(1);

		ret = lvm_is_thin_pool(tp);
		INFO("got %d for thin pool at path: %s", ret, tp);
		if (ret < 0)
			exit(1);

		if (!ret)
			tp = NULL;
	}

	if (!tp)
	    execlp("lvcreate", "lvcreate", "-L", sz, vg, "-n", lv, (char *)NULL);
	else
	    execlp("lvcreate", "lvcreate", "--thinpool", tp, "-V", sz, vg, "-n", lv, (char *)NULL);

	SYSERROR("execlp");
	exit(1);
}

static int lvm_snapshot(const char *orig, const char *path, uint64_t size)
{
	int ret, pid;
	char sz[24], *pathdup, *lv;

	if ((pid = fork()) < 0) {
		SYSERROR("failed fork");
		return -1;
	}
	if (pid > 0)
		return wait_for_pid(pid);

	// specify bytes to lvcreate
	ret = snprintf(sz, 24, "%"PRIu64"b", size);
	if (ret < 0 || ret >= 24)
		exit(1);

	pathdup = strdup(path);
	if (!pathdup)
		exit(1);
	lv = strrchr(pathdup, '/');
	if (!lv) {
		free(pathdup);
		exit(1);
	}
	*lv = '\0';
	lv++;

	// check if the original lv is backed by a thin pool, in which case we
	// cannot specify a size that's different from the original size.
	ret = lvm_is_thin_volume(orig);
	if (ret == -1) {
		free(pathdup);
		return -1;
	}

	if (!ret) {
		ret = execlp("lvcreate", "lvcreate", "-s", "-L", sz, "-n", lv, orig, (char *)NULL);
	} else {
		ret = execlp("lvcreate", "lvcreate", "-s", "-n", lv, orig, (char *)NULL);
	}

	free(pathdup);
	exit(1);
}

// this will return 1 for physical disks, qemu-nbd, loop, etc
// right now only lvm is a block device
static int is_blktype(struct bdev *b)
{
	if (strcmp(b->type, "lvm") == 0)
		return 1;
	return 0;
}

static int lvm_clonepaths(struct bdev *orig, struct bdev *new, const char *oldname,
		const char *cname, const char *oldpath, const char *lxcpath, int snap,
		uint64_t newsize, struct lxc_conf *conf)
{
	char fstype[100];
	uint64_t size = newsize;
	int len, ret;

	if (!orig->src || !orig->dest)
		return -1;

	if (strcmp(orig->type, "lvm")) {
		const char *vg;

		if (snap) {
			ERROR("LVM snapshot from %s backing store is not supported",
				orig->type);
			return -1;
		}
		vg = lxc_global_config_value("lxc.bdev.lvm.vg");
		len = strlen("/dev/") + strlen(vg) + strlen(cname) + 2;
		if ((new->src = malloc(len)) == NULL)
			return -1;
		ret = snprintf(new->src, len, "/dev/%s/%s", vg, cname);
		if (ret < 0 || ret >= len)
			return -1;
	} else {
		new->src = dir_new_path(orig->src, oldname, cname, oldpath, lxcpath);
		if (!new->src)
			return -1;
	}

	if (orig->mntopts) {
		new->mntopts = strdup(orig->mntopts);
		if (!new->mntopts)
			return -1;
	}

	len = strlen(lxcpath) + strlen(cname) + strlen("rootfs") + 3;
	new->dest = malloc(len);
	if (!new->dest)
		return -1;
	ret = snprintf(new->dest, len, "%s/%s/rootfs", lxcpath, cname);
	if (ret < 0 || ret >= len)
		return -1;
	if (mkdir_p(new->dest, 0755) < 0)
		return -1;

	if (is_blktype(orig)) {
		if (!newsize && blk_getsize(orig, &size) < 0) {
			ERROR("Error getting size of %s", orig->src);
			return -1;
		}
		if (detect_fs(orig, fstype, 100) < 0) {
			INFO("could not find fstype for %s, using ext3", orig->src);
			return -1;
		}
	} else {
		sprintf(fstype, "ext3");
		if (!newsize)
			size = DEFAULT_FS_SIZE;
	}

	if (snap) {
		if (lvm_snapshot(orig->src, new->src, size) < 0) {
			ERROR("could not create %s snapshot of %s", new->src, orig->src);
			return -1;
		}
	} else {
		if (do_lvm_create(new->src, size, lxc_global_config_value("lxc.bdev.lvm.thin_pool")) < 0) {
			ERROR("Error creating new lvm blockdev");
			return -1;
		}
		if (do_mkfs(new->src, fstype) < 0) {
			ERROR("Error creating filesystem type %s on %s", fstype,
				new->src);
			return -1;
		}
	}

	return 0;
}

static int lvm_destroy(struct bdev *orig)
{
	pid_t pid;

	if ((pid = fork()) < 0)
		return -1;
	if (!pid) {
		execlp("lvremove", "lvremove", "-f", orig->src, NULL);
		exit(1);
	}
	return wait_for_pid(pid);
}

static int lvm_create(struct bdev *bdev, const char *dest, const char *n,
			struct bdev_specs *specs)
{
	const char *vg, *thinpool, *fstype, *lv = n;
	uint64_t sz;
	int ret, len;

	if (!specs)
		return -1;

	vg = specs->lvm.vg;
	if (!vg)
		vg = lxc_global_config_value("lxc.bdev.lvm.vg");

	thinpool = specs->lvm.thinpool;
	if (!thinpool)
		thinpool = lxc_global_config_value("lxc.bdev.lvm.thin_pool");

	/* /dev/$vg/$lv */
	if (specs->lvm.lv)
		lv = specs->lvm.lv;

	len = strlen(vg) + strlen(lv) + 7;
	bdev->src = malloc(len);
	if (!bdev->src)
		return -1;

	ret = snprintf(bdev->src, len, "/dev/%s/%s", vg, lv);
	if (ret < 0 || ret >= len)
		return -1;

	// fssize is in bytes.
	sz = specs->fssize;
	if (!sz)
		sz = DEFAULT_FS_SIZE;

	if (do_lvm_create(bdev->src, sz, thinpool) < 0) {
		ERROR("Error creating new lvm blockdev %s size %"PRIu64" bytes", bdev->src, sz);
		return -1;
	}

	fstype = specs->fstype;
	if (!fstype)
		fstype = DEFAULT_FSTYPE;
	if (do_mkfs(bdev->src, fstype) < 0) {
		ERROR("Error creating filesystem type %s on %s", fstype,
			bdev->src);
		return -1;
	}
	if (!(bdev->dest = strdup(dest)))
		return -1;

	if (mkdir_p(bdev->dest, 0755) < 0) {
		ERROR("Error creating %s", bdev->dest);
		return -1;
	}

	return 0;
}

static const struct bdev_ops lvm_ops = {
	.detect = &lvm_detect,
	.mount = &lvm_mount,
	.umount = &lvm_umount,
	.clone_paths = &lvm_clonepaths,
	.destroy = &lvm_destroy,
	.create = &lvm_create,
	.can_snapshot = true,
	.can_backup = false,
};


/*
 *   CEPH RBD ops
 */

static int rbd_detect(const char *path)
{
	if ( memcmp(path, "/dev/rbd/", 9) == 0)
		return 1;
	return 0;
}

static int rbd_mount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "rbd"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;

	if ( !file_exists(bdev->src) ) {
		// if blkdev does not exist it should be mapped, because it is not persistent on reboot
		ERROR("Block device %s is not mapped.", bdev->src);
		return -1;
	}

	return mount_unknown_fs(bdev->src, bdev->dest, bdev->mntopts);
}

static int rbd_umount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "rbd"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	return umount(bdev->dest);
}

static int rbd_clonepaths(struct bdev *orig, struct bdev *new, const char *oldname,
		const char *cname, const char *oldpath, const char *lxcpath, int snap,
		uint64_t newsize, struct lxc_conf *conf)
{
	ERROR("rbd clonepaths not implemented");
	return -1;
}

static int rbd_destroy(struct bdev *orig)
{
	pid_t pid;
	char *rbdfullname;

	if ( file_exists(orig->src) ) {
		if ((pid = fork()) < 0)
			return -1;
		if (!pid) {
			execlp("rbd", "rbd", "unmap" , orig->src, NULL);
			exit(1);
		}
		if (wait_for_pid(pid) < 0)
			return -1;
	}

	if ((pid = fork()) < 0)
		return -1;
	if (!pid) {
		rbdfullname = alloca(strlen(orig->src) - 8);
		strcpy( rbdfullname, &orig->src[9] );
		execlp("rbd", "rbd", "rm" , rbdfullname, NULL);
		exit(1);
	}
	return wait_for_pid(pid);

}

static int rbd_create(struct bdev *bdev, const char *dest, const char *n,
			struct bdev_specs *specs)
{
	const char *rbdpool, *rbdname = n, *fstype;
	uint64_t size;
	int ret, len;
	char sz[24];
	pid_t pid;

	if (!specs)
		return -1;

	rbdpool = specs->rbd.rbdpool;
	if (!rbdpool)
		rbdpool = lxc_global_config_value("lxc.bdev.rbd.rbdpool");

	if (specs->rbd.rbdname)
		rbdname = specs->rbd.rbdname;

	/* source device /dev/rbd/lxc/ctn */
	len = strlen(rbdpool) + strlen(rbdname) + 11;
	bdev->src = malloc(len);
	if (!bdev->src)
		return -1;

	ret = snprintf(bdev->src, len, "/dev/rbd/%s/%s", rbdpool, rbdname);
	if (ret < 0 || ret >= len)
		return -1;

	// fssize is in bytes.
	size = specs->fssize;
	if (!size)
		size = DEFAULT_FS_SIZE;

	// in megabytes for rbd tool
	ret = snprintf(sz, 24, "%"PRIu64, size / 1024 / 1024 );
	if (ret < 0 || ret >= 24)
		exit(1);

	if ((pid = fork()) < 0)
		return -1;
	if (!pid) {
		execlp("rbd", "rbd", "create" , "--pool", rbdpool, rbdname, "--size", sz, NULL);
		exit(1);
	}
	if (wait_for_pid(pid) < 0)
		return -1;

	if ((pid = fork()) < 0)
		return -1;
	if (!pid) {
		execlp("rbd", "rbd", "map", "--pool", rbdpool, rbdname, NULL);
		exit(1);
	}
	if (wait_for_pid(pid) < 0)
		return -1;

	fstype = specs->fstype;
	if (!fstype)
		fstype = DEFAULT_FSTYPE;

	if (do_mkfs(bdev->src, fstype) < 0) {
		ERROR("Error creating filesystem type %s on %s", fstype,
			bdev->src);
		return -1;
	}
	if (!(bdev->dest = strdup(dest)))
		return -1;

	if (mkdir_p(bdev->dest, 0755) < 0 && errno != EEXIST) {
		ERROR("Error creating %s", bdev->dest);
		return -1;
	}

	return 0;
}

static const struct bdev_ops rbd_ops = {
	.detect = &rbd_detect,
	.mount = &rbd_mount,
	.umount = &rbd_umount,
	.clone_paths = &rbd_clonepaths,
	.destroy = &rbd_destroy,
	.create = &rbd_create,
	.can_snapshot = false,
	.can_backup = false,
};

static const struct bdev_ops btrfs_ops = {
	.detect = &btrfs_detect,
	.mount = &btrfs_mount,
	.umount = &btrfs_umount,
	.clone_paths = &btrfs_clonepaths,
	.destroy = &btrfs_destroy,
	.create = &btrfs_create,
	.can_snapshot = true,
	.can_backup = true,
};

//
// loopback dev ops
//
static int loop_detect(const char *path)
{
	if (strncmp(path, "loop:", 5) == 0)
		return 1;
	return 0;
}

static int find_free_loopdev_no_control(int *retfd, char *namep)
{
	struct dirent dirent, *direntp;
	struct loop_info64 lo;
	DIR *dir;
	int fd = -1;

	dir = opendir("/dev");
	if (!dir) {
		SYSERROR("Error opening /dev");
		return -1;
	}
	while (!readdir_r(dir, &dirent, &direntp)) {

		if (!direntp)
			break;
		if (strncmp(direntp->d_name, "loop", 4) != 0)
			continue;
		fd = openat(dirfd(dir), direntp->d_name, O_RDWR);
		if (fd < 0)
			continue;
		if (ioctl(fd, LOOP_GET_STATUS64, &lo) == 0 || errno != ENXIO) {
			close(fd);
			fd = -1;
			continue;
		}
		// We can use this fd
		snprintf(namep, 100, "/dev/%s", direntp->d_name);
		break;
	}
	closedir(dir);
	if (fd == -1) {
		ERROR("No loop device found");
		return -1;
	}

	*retfd = fd;
	return 0;
}

static int find_free_loopdev(int *retfd, char *namep)
{
	int rc, fd = -1;
	int ctl = open("/dev/loop-control", O_RDWR);
	if (ctl < 0)
		return find_free_loopdev_no_control(retfd, namep);
	rc = ioctl(ctl, LOOP_CTL_GET_FREE);
	if (rc >= 0) {
		snprintf(namep, 100, "/dev/loop%d", rc);
		fd = open(namep, O_RDWR);
	}
	close(ctl);
	if (fd == -1) {
		ERROR("No loop device found");
		return -1;
	}
	*retfd = fd;
	return 0;
}

static int loop_mount(struct bdev *bdev)
{
	int lfd, ffd = -1, ret = -1;
	struct loop_info64 lo;
	char loname[100];

	if (strcmp(bdev->type, "loop"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	if (find_free_loopdev(&lfd, loname) < 0)
		return -22;

	ffd = open(bdev->src + 5, O_RDWR);
	if (ffd < 0) {
		SYSERROR("Error opening backing file %s", bdev->src);
		goto out;
	}

	if (ioctl(lfd, LOOP_SET_FD, ffd) < 0) {
		SYSERROR("Error attaching backing file to loop dev");
		goto out;
	}
	memset(&lo, 0, sizeof(lo));
	lo.lo_flags = LO_FLAGS_AUTOCLEAR;
	if (ioctl(lfd, LOOP_SET_STATUS64, &lo) < 0) {
		SYSERROR("Error setting autoclear on loop dev");
		goto out;
	}

	ret = mount_unknown_fs(loname, bdev->dest, bdev->mntopts);
	if (ret < 0)
		ERROR("Error mounting %s", bdev->src);
	else
		bdev->lofd = lfd;

out:
	if (ffd > -1)
		close(ffd);
	if (ret < 0) {
		close(lfd);
		bdev->lofd = -1;
	}
	return ret;
}

static int loop_umount(struct bdev *bdev)
{
	int ret;

	if (strcmp(bdev->type, "loop"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	ret = umount(bdev->dest);
	if (bdev->lofd >= 0) {
		close(bdev->lofd);
		bdev->lofd = -1;
	}
	return ret;
}

static int do_loop_create(const char *path, uint64_t size, const char *fstype)
{
	int fd, ret;
	// create the new loopback file.
	fd = creat(path, S_IRUSR|S_IWUSR);
	if (fd < 0)
		return -1;
	if (lseek(fd, size, SEEK_SET) < 0) {
		SYSERROR("Error seeking to set new loop file size");
		close(fd);
		return -1;
	}
	if (write(fd, "1", 1) != 1) {
		SYSERROR("Error creating new loop file");
		close(fd);
		return -1;
	}
	ret = close(fd);
	if (ret < 0) {
		SYSERROR("Error closing new loop file");
		return -1;
	}

	// create an fs in the loopback file
	if (do_mkfs(path, fstype) < 0) {
		ERROR("Error creating filesystem type %s on %s", fstype,
			path);
		return -1;
	}

	return 0;
}

/*
 * No idea what the original blockdev will be called, but the copy will be
 * called $lxcpath/$lxcname/rootdev
 */
static int loop_clonepaths(struct bdev *orig, struct bdev *new, const char *oldname,
		const char *cname, const char *oldpath, const char *lxcpath, int snap,
		uint64_t newsize, struct lxc_conf *conf)
{
	char fstype[100];
	uint64_t size = newsize;
	int len, ret;
	char *srcdev;

	if (snap) {
		ERROR("loop devices cannot be snapshotted.");
		return -1;
	}

	if (!orig->dest || !orig->src)
		return -1;

	len = strlen(lxcpath) + strlen(cname) + strlen("rootdev") + 3;
	srcdev = alloca(len);
	ret = snprintf(srcdev, len, "%s/%s/rootdev", lxcpath, cname);
	if (ret < 0 || ret >= len)
		return -1;

	new->src = malloc(len + 5);
	if (!new->src)
		return -1;
	ret = snprintf(new->src, len + 5, "loop:%s", srcdev);
	if (ret < 0 || ret >= len + 5)
		return -1;

	new->dest = malloc(len);
	if (!new->dest)
		return -1;
	ret = snprintf(new->dest, len, "%s/%s/rootfs", lxcpath, cname);
	if (ret < 0 || ret >= len)
		return -1;

	// it's tempting to say: if orig->src == loopback and !newsize, then
	// copy the loopback file.  However, we'd have to make sure to
	// correctly keep holes!  So punt for now.

	if (is_blktype(orig)) {
		if (!newsize && blk_getsize(orig, &size) < 0) {
			ERROR("Error getting size of %s", orig->src);
			return -1;
		}
		if (detect_fs(orig, fstype, 100) < 0) {
			INFO("could not find fstype for %s, using %s", orig->src,
				DEFAULT_FSTYPE);
			return -1;
		}
	} else {
		sprintf(fstype, "%s", DEFAULT_FSTYPE);
		if (!newsize)
			size = DEFAULT_FS_SIZE;
	}
	return do_loop_create(srcdev, size, fstype);
}

static int loop_create(struct bdev *bdev, const char *dest, const char *n,
			struct bdev_specs *specs)
{
	const char *fstype;
	uint64_t sz;
	int ret, len;
	char *srcdev;

	if (!specs)
		return -1;

	// dest is passed in as $lxcpath / $lxcname / rootfs
	// srcdev will be:      $lxcpath / $lxcname / rootdev
	// src will be 'loop:$srcdev'
	len = strlen(dest) + 2;
	srcdev = alloca(len);

	ret = snprintf(srcdev, len, "%s", dest);
	if (ret < 0 || ret >= len)
		return -1;
	sprintf(srcdev + len - 4, "dev");

	bdev->src = malloc(len + 5);
	if (!bdev->src)
		return -1;
	ret = snprintf(bdev->src, len + 5, "loop:%s", srcdev);
	if (ret < 0 || ret >= len + 5)
		return -1;

	sz = specs->fssize;
	if (!sz)
		sz = DEFAULT_FS_SIZE;

	fstype = specs->fstype;
	if (!fstype)
		fstype = DEFAULT_FSTYPE;

	if (!(bdev->dest = strdup(dest)))
		return -1;

	if (mkdir_p(bdev->dest, 0755) < 0) {
		ERROR("Error creating %s", bdev->dest);
		return -1;
	}

	return do_loop_create(srcdev, sz, fstype);
}

static int loop_destroy(struct bdev *orig)
{
	return unlink(orig->src + 5);
}

static const struct bdev_ops loop_ops = {
	.detect = &loop_detect,
	.mount = &loop_mount,
	.umount = &loop_umount,
	.clone_paths = &loop_clonepaths,
	.destroy = &loop_destroy,
	.create = &loop_create,
	.can_snapshot = false,
	.can_backup = true,
};

/* overlay */
static const struct bdev_ops ovl_ops = {
	.detect = &ovl_detect,
	.mount = &ovl_mount,
	.umount = &ovl_umount,
	.clone_paths = &ovl_clonepaths,
	.destroy = &ovl_destroy,
	.create = &ovl_create,
	.can_snapshot = true,
	.can_backup = true,
};

/* zfs */
static const struct bdev_ops zfs_ops = {
	.detect = &zfs_detect,
	.mount = &zfs_mount,
	.umount = &zfs_umount,
	.clone_paths = &zfs_clonepaths,
	.destroy = &zfs_destroy,
	.create = &zfs_create,
	.can_snapshot = true,
	.can_backup = true,
};

//
// aufs ops
//

static int aufs_detect(const char *path)
{
	if (strncmp(path, "aufs:", 5) == 0)
		return 1; // take their word for it
	return 0;
}

//
// XXXXXXX plain directory bind mount ops
//
static int aufs_mount(struct bdev *bdev)
{
	char *options, *dup, *lower, *upper;
	int len;
	unsigned long mntflags;
	char *mntdata;
	int ret;
	const char *xinopath = "/dev/shm/aufs.xino";

	if (strcmp(bdev->type, "aufs"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;

	//  separately mount it first
	//  mount -t aufs -obr=${upper}=rw:${lower}=ro lower dest
	dup = alloca(strlen(bdev->src)+1);
	strcpy(dup, bdev->src);
	if (!(lower = strchr(dup, ':')))
		return -22;
	if (!(upper = strchr(++lower, ':')))
		return -22;
	*upper = '\0';
	upper++;

	if (parse_mntopts(bdev->mntopts, &mntflags, &mntdata) < 0) {
		free(mntdata);
		return -22;
	}

	// TODO We should check whether bdev->src is a blockdev, and if so
	// but for now, only support aufs of a basic directory

	// AUFS does not work on top of certain filesystems like (XFS or Btrfs)
	// so add xino=/dev/shm/aufs.xino parameter to mount options.
	// The same xino option can be specified to multiple aufs mounts, and
	// a xino file is not shared among multiple aufs mounts.
	//
	// see http://www.mail-archive.com/aufs-users@lists.sourceforge.net/msg02587.html
	//     http://www.mail-archive.com/aufs-users@lists.sourceforge.net/msg05126.html
	if (mntdata) {
		len = strlen(lower) + strlen(upper) + strlen(xinopath) + strlen("br==rw:=ro,,xino=") + strlen(mntdata) + 1;
		options = alloca(len);
		ret = snprintf(options, len, "br=%s=rw:%s=ro,%s,xino=%s", upper, lower, mntdata, xinopath);
	}
	else {
		len = strlen(lower) + strlen(upper) + strlen(xinopath) + strlen("br==rw:=ro,xino=") + 1;
		options = alloca(len);
		ret = snprintf(options, len, "br=%s=rw:%s=ro,xino=%s", upper, lower, xinopath);
	}

	if (ret < 0 || ret >= len) {
		free(mntdata);
		return -1;
	}

	ret = mount(lower, bdev->dest, "aufs", MS_MGC_VAL | mntflags, options);
	if (ret < 0)
		SYSERROR("aufs: error mounting %s onto %s options %s",
			lower, bdev->dest, options);
	else
		INFO("aufs: mounted %s onto %s options %s",
			lower, bdev->dest, options);
	return ret;
}

static int aufs_umount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "aufs"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	return umount(bdev->dest);
}

static int aufs_clonepaths(struct bdev *orig, struct bdev *new, const char *oldname,
		const char *cname, const char *oldpath, const char *lxcpath, int snap,
		uint64_t newsize, struct lxc_conf *conf)
{
	if (!snap) {
		ERROR("aufs is only for snapshot clones");
		return -22;
	}

	if (!orig->src || !orig->dest)
		return -1;

	new->dest = dir_new_path(orig->dest, oldname, cname, oldpath, lxcpath);
	if (!new->dest)
		return -1;
	if (mkdir_p(new->dest, 0755) < 0)
		return -1;

	if (am_unpriv() && chown_mapped_root(new->dest, conf) < 0)
		WARN("Failed to update ownership of %s", new->dest);

	if (strcmp(orig->type, "dir") == 0) {
		char *delta, *lastslash;
		int ret, len, lastslashidx;

		// if we have /var/lib/lxc/c2/rootfs, then delta will be
		//            /var/lib/lxc/c2/delta0
		lastslash = strrchr(new->dest, '/');
		if (!lastslash)
			return -22;
		if (strlen(lastslash) < 7)
			return -22;
		lastslash++;
		lastslashidx = lastslash - new->dest;

		delta = malloc(lastslashidx + 7);
		if (!delta)
			return -1;
		strncpy(delta, new->dest, lastslashidx+1);
		strcpy(delta+lastslashidx, "delta0");
		if ((ret = mkdir(delta, 0755)) < 0) {
			SYSERROR("error: mkdir %s", delta);
			free(delta);
			return -1;
		}
		if (am_unpriv() && chown_mapped_root(delta, conf) < 0)
			WARN("Failed to update ownership of %s", delta);

		// the src will be 'aufs:lowerdir:upperdir'
		len = strlen(delta) + strlen(orig->src) + 12;
		new->src = malloc(len);
		if (!new->src) {
			free(delta);
			return -ENOMEM;
		}
		ret = snprintf(new->src, len, "aufs:%s:%s", orig->src, delta);
		free(delta);
		if (ret < 0 || ret >= len)
			return -ENOMEM;
	} else if (strcmp(orig->type, "aufs") == 0) {
		// What exactly do we want to do here?
		// I think we want to use the original lowerdir, with a
		// private delta which is originally rsynced from the
		// original delta
		char *osrc, *odelta, *nsrc, *ndelta;
		int len, ret;
		if (!(osrc = strdup(orig->src)))
			return -22;
		nsrc = strchr(osrc, ':') + 1;
		if (nsrc != osrc + 5 || (odelta = strchr(nsrc, ':')) == NULL) {
			free(osrc);
			return -22;
		}
		*odelta = '\0';
		odelta++;
		ndelta = dir_new_path(odelta, oldname, cname, oldpath, lxcpath);
		if (!ndelta) {
			free(osrc);
			return -ENOMEM;
		}
		if ((ret = mkdir(ndelta, 0755)) < 0 && errno != EEXIST) {
			SYSERROR("error: mkdir %s", ndelta);
			free(osrc);
			free(ndelta);
			return -1;
		}
		if (am_unpriv() && chown_mapped_root(ndelta, conf) < 0)
			WARN("Failed to update ownership of %s", ndelta);

		struct rsync_data_char rdata;
		rdata.src = odelta;
		rdata.dest = ndelta;
		if (am_unpriv())
			ret = userns_exec_1(conf, rsync_delta_wrapper, &rdata);
		else
			ret = rsync_delta(&rdata);
		if (ret) {
			free(osrc);
			free(ndelta);
			ERROR("copying aufs delta");
			return -1;
		}
		len = strlen(nsrc) + strlen(ndelta) + 12;
		new->src = malloc(len);
		if (!new->src) {
			free(osrc);
			free(ndelta);
			return -ENOMEM;
		}
		ret = snprintf(new->src, len, "aufs:%s:%s", nsrc, ndelta);
		free(osrc);
		free(ndelta);
		if (ret < 0 || ret >= len)
			return -ENOMEM;
	} else {
		ERROR("aufs clone of %s container is not yet supported",
			orig->type);
		// Note, supporting this will require aufs_mount supporting
		// mounting of the underlay.  No big deal, just needs to be done.
		return -1;
	}

	return 0;
}

static int aufs_destroy(struct bdev *orig)
{
	char *upper;

	if (strncmp(orig->src, "aufs:", 5) != 0)
		return -22;
	upper = strchr(orig->src + 5, ':');
	if (!upper)
		return -22;
	upper++;
	return lxc_rmdir_onedev(upper, NULL);
}

/*
 * to say 'lxc-create -t ubuntu -n o1 -B aufs' means you want
 * $lxcpath/$lxcname/rootfs to have the created container, while all
 * changes after starting the container are written to
 * $lxcpath/$lxcname/delta0
 */
static int aufs_create(struct bdev *bdev, const char *dest, const char *n,
			struct bdev_specs *specs)
{
	char *delta;
	int ret, len = strlen(dest), newlen;

	if (len < 8 || strcmp(dest+len-7, "/rootfs") != 0)
		return -1;

	if (!(bdev->dest = strdup(dest))) {
		ERROR("Out of memory");
		return -1;
	}

	delta = alloca(strlen(dest)+1);
	strcpy(delta, dest);
	strcpy(delta+len-6, "delta0");

	if (mkdir_p(delta, 0755) < 0) {
		ERROR("Error creating %s", delta);
		return -1;
	}

	/* aufs:lower:upper */
	newlen = (2 * len) + strlen("aufs:") + 2;
	bdev->src = malloc(newlen);
	if (!bdev->src) {
		ERROR("Out of memory");
		return -1;
	}
	ret = snprintf(bdev->src, newlen, "aufs:%s:%s", dest, delta);
	if (ret < 0 || ret >= newlen)
		return -1;

	if (mkdir_p(bdev->dest, 0755) < 0) {
		ERROR("Error creating %s", bdev->dest);
		return -1;
	}

	return 0;
}

static const struct bdev_ops aufs_ops = {
	.detect = &aufs_detect,
	.mount = &aufs_mount,
	.umount = &aufs_umount,
	.clone_paths = &aufs_clonepaths,
	.destroy = &aufs_destroy,
	.create = &aufs_create,
	.can_snapshot = true,
	.can_backup = true,
};

//
// nbd dev ops
//

static int nbd_detect(const char *path)
{
	if (strncmp(path, "nbd:", 4) == 0)
		return 1;
	return 0;
}

struct nbd_attach_data {
	const char *nbd;
	const char *path;
};

static void nbd_detach(const char *path)
{
	int ret;
	pid_t pid = fork();

	if (pid < 0) {
		SYSERROR("Error forking to detach nbd");
		return;
	}
	if (pid) {
		ret = wait_for_pid(pid);
		if (ret < 0)
			ERROR("nbd disconnect returned an error");
		return;
	}
	execlp("qemu-nbd", "qemu-nbd", "-d", path, NULL);
	SYSERROR("Error executing qemu-nbd");
	exit(1);
}

static int do_attach_nbd(void *d)
{
	struct nbd_attach_data *data = d;
	const char *nbd, *path;
	pid_t pid;
	sigset_t mask;
	int sfd;
	ssize_t s;
	struct signalfd_siginfo fdsi;

	sigemptyset(&mask);
	sigaddset(&mask, SIGHUP);
	sigaddset(&mask, SIGCHLD);

	nbd = data->nbd;
	path = data->path;

	if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
		SYSERROR("Error blocking signals for nbd watcher");
		exit(1);
	}

	sfd = signalfd(-1, &mask, 0);
	if (sfd == -1) {
		SYSERROR("Error opening signalfd for nbd task");
		exit(1);
	}

	if (prctl(PR_SET_PDEATHSIG, SIGHUP, 0, 0, 0) < 0)
		SYSERROR("Error setting parent death signal for nbd watcher");

	pid = fork();
	if (pid) {
		for (;;) {
			s = read(sfd, &fdsi, sizeof(struct signalfd_siginfo));
			if (s != sizeof(struct signalfd_siginfo))
				SYSERROR("Error reading from signalfd");

			if (fdsi.ssi_signo == SIGHUP) {
				/* container has exited */
				nbd_detach(nbd);
				exit(0);
			} else if (fdsi.ssi_signo == SIGCHLD) {
				int status;
				/* If qemu-nbd fails, or is killed by a signal,
				 * then exit */
				while (waitpid(-1, &status, WNOHANG) > 0) {
					if ((WIFEXITED(status) && WEXITSTATUS(status) != 0) ||
							WIFSIGNALED(status)) {
						nbd_detach(nbd);
						exit(1);
					}
				}
			}
		}
	}

	close(sfd);
	if (sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1)
		WARN("Warning: unblocking signals for nbd watcher");

	execlp("qemu-nbd", "qemu-nbd", "-c", nbd, path, NULL);
	SYSERROR("Error executing qemu-nbd");
	exit(1);
}

static bool clone_attach_nbd(const char *nbd, const char *path)
{
	pid_t pid;
	struct nbd_attach_data data;

	data.nbd = nbd;
	data.path = path;

	pid = lxc_clone(do_attach_nbd, &data, CLONE_NEWPID);
	if (pid < 0)
		return false;
	return true;
}

static bool nbd_busy(int idx)
{
	char path[100];
	int ret;

	ret = snprintf(path, 100, "/sys/block/nbd%d/pid", idx);
	if (ret < 0 || ret >= 100)
		return true;
	return file_exists(path);
}

static bool attach_nbd(char *src, struct lxc_conf *conf)
{
	char *orig = alloca(strlen(src)+1), *p, path[50];
	int i = 0;

	strcpy(orig, src);
	/* if path is followed by a partition, drop that for now */
	p = strchr(orig, ':');
	if (p)
		*p = '\0';
	while (1) {
		sprintf(path, "/dev/nbd%d", i);
		if (!file_exists(path))
			return false;
		if (nbd_busy(i)) {
			i++;
			continue;
		}
		if (!clone_attach_nbd(path, orig))
			return false;
		conf->nbd_idx = i;
		return true;
	}
}

static bool requires_nbd(const char *path)
{
	if (strncmp(path, "nbd:", 4) == 0)
		return true;
	return false;
}

/*
 * attach_block_device returns true if all went well,
 * meaning either a block device was attached or was not
 * needed.  It returns false if something went wrong and
 * container startup should be stopped.
 */
bool attach_block_device(struct lxc_conf *conf)
{
	char *path;

	if (!conf->rootfs.path)
		return true;
	path = conf->rootfs.path;
	if (!requires_nbd(path))
		return true;
	path = strchr(path, ':');
	if (!path)
		return false;
	path++;
	if (!attach_nbd(path, conf))
		return false;
	return true;
}

void detach_nbd_idx(int idx)
{
	int ret;
	char path[50];

	ret = snprintf(path, 50, "/dev/nbd%d", idx);
	if (ret < 0 || ret >= 50)
		return;

	nbd_detach(path);
}

void detach_block_device(struct lxc_conf *conf)
{
	if (conf->nbd_idx != -1)
		detach_nbd_idx(conf->nbd_idx);
}

/*
 * Pick the partition # off the end of a nbd:file:p
 * description.  Return 1-9 for the partition id, or 0
 * for no partition.
 */
static int nbd_get_partition(const char *src)
{
	char *p = strchr(src, ':');
	if (!p)
		return 0;
	p = strchr(p+1, ':');
	if (!p)
		return 0;
	p++;
	if (*p < '1' || *p > '9')
		return 0;
	return *p - '0';
}

static bool wait_for_partition(const char *path)
{
	int count = 0;
	while (count < 5) {
		if (file_exists(path))
			return true;
		sleep(1);
		count++;
	}
	ERROR("Device %s did not show up after 5 seconds", path);
	return false;
}

static int nbd_mount(struct bdev *bdev)
{
	int ret = -1, partition;
	char path[50];

	if (strcmp(bdev->type, "nbd"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;

	/* nbd_idx should have been copied by bdev_init from the lxc_conf */
	if (bdev->nbd_idx < 0)
		return -22;
	partition = nbd_get_partition(bdev->src);
	if (partition)
		ret = snprintf(path, 50, "/dev/nbd%dp%d", bdev->nbd_idx,
				partition);
	else
		ret = snprintf(path, 50, "/dev/nbd%d", bdev->nbd_idx);
	if (ret < 0 || ret >= 50) {
		ERROR("Error setting up nbd device path");
		return ret;
	}

	/* It might take awhile for the partition files to show up */
	if (partition) {
		if (!wait_for_partition(path))
			return -2;
	}
	ret = mount_unknown_fs(path, bdev->dest, bdev->mntopts);
	if (ret < 0)
		ERROR("Error mounting %s", bdev->src);

	return ret;
}

static int nbd_create(struct bdev *bdev, const char *dest, const char *n,
			struct bdev_specs *specs)
{
	return -ENOSYS;
}

static int nbd_clonepaths(struct bdev *orig, struct bdev *new, const char *oldname,
		const char *cname, const char *oldpath, const char *lxcpath, int snap,
		uint64_t newsize, struct lxc_conf *conf)
{
	return -ENOSYS;
}

static int nbd_destroy(struct bdev *orig)
{
	return -ENOSYS;
}

static int nbd_umount(struct bdev *bdev)
{
	int ret;

	if (strcmp(bdev->type, "nbd"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	ret = umount(bdev->dest);
	return ret;
}

static const struct bdev_ops nbd_ops = {
	.detect = &nbd_detect,
	.mount = &nbd_mount,
	.umount = &nbd_umount,
	.clone_paths = &nbd_clonepaths,
	.destroy = &nbd_destroy,
	.create = &nbd_create,
	.can_snapshot = true,
	.can_backup = false,
};

static const struct bdev_type bdevs[] = {
	{.name = "zfs", .ops = &zfs_ops,},
	{.name = "lvm", .ops = &lvm_ops,},
	{.name = "rbd", .ops = &rbd_ops,},
	{.name = "btrfs", .ops = &btrfs_ops,},
	{.name = "dir", .ops = &dir_ops,},
	{.name = "aufs", .ops = &aufs_ops,},
	{.name = "overlayfs", .ops = &ovl_ops,},
	{.name = "loop", .ops = &loop_ops,},
	{.name = "nbd", .ops = &nbd_ops,},
};

static const size_t numbdevs = sizeof(bdevs) / sizeof(struct bdev_type);

void bdev_put(struct bdev *bdev)
{
	free(bdev->mntopts);
	free(bdev->src);
	free(bdev->dest);
	free(bdev);
}

struct bdev *bdev_get(const char *type)
{
	int i;
	struct bdev *bdev;

	for (i=0; i<numbdevs; i++) {
		if (strcmp(bdevs[i].name, type) == 0)
			break;
	}
	if (i == numbdevs)
		return NULL;
	bdev = malloc(sizeof(struct bdev));
	if (!bdev)
		return NULL;
	memset(bdev, 0, sizeof(struct bdev));
	bdev->ops = bdevs[i].ops;
	bdev->type = bdevs[i].name;
	return bdev;
}

static const struct bdev_type *bdev_query(const char *src)
{
	int i;
	for (i=0; i<numbdevs; i++) {
		int r;
		r = bdevs[i].ops->detect(src);
		if (r)
			break;
	}

	if (i == numbdevs)
		return NULL;
	return &bdevs[i];
}

struct bdev *bdev_init(struct lxc_conf *conf, const char *src, const char *dst, const char *mntopts)
{
	struct bdev *bdev;
	const struct bdev_type *q;

	if (!src)
		src = conf->rootfs.path;

	if (!src)
		return NULL;

	q = bdev_query(src);
	if (!q)
		return NULL;

	bdev = malloc(sizeof(struct bdev));
	if (!bdev)
		return NULL;
	memset(bdev, 0, sizeof(struct bdev));
	bdev->ops = q->ops;
	bdev->type = q->name;
	if (mntopts)
		bdev->mntopts = strdup(mntopts);
	if (src)
		bdev->src = strdup(src);
	if (dst)
		bdev->dest = strdup(dst);
	if (strcmp(bdev->type, "nbd") == 0)
		bdev->nbd_idx = conf->nbd_idx;

	return bdev;
}

bool bdev_is_dir(struct lxc_conf *conf, const char *path)
{
	struct bdev *orig = bdev_init(conf, path, NULL, NULL);
	bool ret = false;
	if (!orig)
		return ret;
	if (strcmp(orig->type, "dir") == 0)
		ret = true;
	bdev_put(orig);
	return ret;
}

bool bdev_can_backup(struct lxc_conf *conf)
{
	struct bdev *bdev = bdev_init(conf, NULL, NULL, NULL);
	bool ret;

	if (!bdev)
		return false;
	ret = bdev->ops->can_backup;
	bdev_put(bdev);
	return ret;
}

/*
 * is an unprivileged user allowed to make this kind of snapshot
 */
static bool unpriv_snap_allowed(struct bdev *b, const char *t, bool snap,
		bool maybesnap)
{
	if (!t) {
		// new type will be same as original
		// (unless snap && b->type == dir, in which case it will be
		// overlayfs -- which is also allowed)
		if (strcmp(b->type, "dir") == 0 ||
				strcmp(b->type, "aufs") == 0 ||
				strcmp(b->type, "overlayfs") == 0 ||
				strcmp(b->type, "btrfs") == 0 ||
				strcmp(b->type, "loop") == 0)
			return true;
		return false;
	}

	// unprivileged users can copy and snapshot dir, overlayfs,
	// and loop.  In particular, not zfs, btrfs, or lvm.
	if (strcmp(t, "dir") == 0 ||
		strcmp(t, "aufs") == 0 ||
		strcmp(t, "overlayfs") == 0 ||
		strcmp(t, "btrfs") == 0 ||
		strcmp(t, "loop") == 0)
		return true;
	return false;
}

/*
 * If we're not snaphotting, then bdev_copy becomes a simple case of mount
 * the original, mount the new, and rsync the contents.
 */
struct bdev *bdev_copy(struct lxc_container *c0, const char *cname,
			const char *lxcpath, const char *bdevtype,
			int flags, const char *bdevdata, uint64_t newsize,
			int *needs_rdep)
{
	struct bdev *orig, *new;
	pid_t pid;
	int ret;
	bool snap = flags & LXC_CLONE_SNAPSHOT;
	bool maybe_snap = flags & LXC_CLONE_MAYBE_SNAPSHOT;
	bool keepbdevtype = flags & LXC_CLONE_KEEPBDEVTYPE;
	const char *src = c0->lxc_conf->rootfs.path;
	const char *oldname = c0->name;
	const char *oldpath = c0->config_path;
	struct rsync_data data;

	/* if the container name doesn't show up in the rootfs path, then
	 * we don't know how to come up with a new name
	 */
	if (strstr(src, oldname) == NULL) {
		ERROR("original rootfs path %s doesn't include container name %s",
			src, oldname);
		return NULL;
	}

	orig = bdev_init(c0->lxc_conf, src, NULL, NULL);
	if (!orig) {
		ERROR("failed to detect blockdev type for %s", src);
		return NULL;
	}

	if (!orig->dest) {
		int ret;
		size_t len;
		struct stat sb;

		len = strlen(oldpath) + strlen(oldname) + strlen("/rootfs") + 2;
		orig->dest = malloc(len);
		if (!orig->dest) {
			ERROR("out of memory");
			bdev_put(orig);
			return NULL;
		}
		ret = snprintf(orig->dest, len, "%s/%s/rootfs", oldpath, oldname);
		if (ret < 0 || ret >= len) {
			ERROR("rootfs path too long");
			bdev_put(orig);
			return NULL;
		}
		ret = stat(orig->dest, &sb);
		if (ret < 0 && errno == ENOENT)
			if (mkdir_p(orig->dest, 0755) < 0)
				WARN("Error creating '%s', continuing.", orig->dest);
	}

	/*
	 * special case for snapshot - if caller requested maybe_snapshot and
	 * keepbdevtype and backing store is directory, then proceed with a copy
	 * clone rather than returning error
	 */
	if (maybe_snap && keepbdevtype && !bdevtype && !orig->ops->can_snapshot)
		snap = false;

	/*
	 * If newtype is NULL and snapshot is set, then use overlayfs
	 */
	if (!bdevtype && !keepbdevtype && snap && strcmp(orig->type , "dir") == 0)
		bdevtype = "overlayfs";

	if (am_unpriv() && !unpriv_snap_allowed(orig, bdevtype, snap, maybe_snap)) {
		ERROR("Unsupported snapshot type for unprivileged users");
		bdev_put(orig);
		return NULL;
	}

	*needs_rdep = 0;
	if (bdevtype && strcmp(orig->type, "dir") == 0 &&
			(strcmp(bdevtype, "aufs") == 0 ||
			 strcmp(bdevtype, "overlayfs") == 0)) {
		*needs_rdep = 1;
	} else if (snap && strcmp(orig->type, "lvm") == 0 &&
			!lvm_is_thin_volume(orig->src)) {
		*needs_rdep = 1;
	}

	new = bdev_get(bdevtype ? bdevtype : orig->type);
	if (!new) {
		ERROR("no such block device type: %s", bdevtype ? bdevtype : orig->type);
		bdev_put(orig);
		return NULL;
	}

	if (new->ops->clone_paths(orig, new, oldname, cname, oldpath, lxcpath,
				snap, newsize, c0->lxc_conf) < 0) {
		ERROR("failed getting pathnames for cloned storage: %s", src);
		goto err;
	}

	if (am_unpriv() && chown_mapped_root(new->src, c0->lxc_conf) < 0)
		WARN("Failed to update ownership of %s", new->dest);

	if (snap)
		return new;

	/*
	 * https://github.com/lxc/lxc/issues/131
	 * Use btrfs snapshot feature instead of rsync to restore if both orig and new are btrfs
	 */
	if (bdevtype &&
			strcmp(orig->type, "btrfs") == 0 && strcmp(new->type, "btrfs") == 0 &&
			btrfs_same_fs(orig->dest, new->dest) == 0) {
		if (btrfs_destroy(new) < 0) {
			ERROR("Error destroying %s subvolume", new->dest);
			goto err;
		}
		if (mkdir_p(new->dest, 0755) < 0) {
			ERROR("Error creating %s directory", new->dest);
			goto err;
		}
		if (btrfs_snapshot(orig->dest, new->dest) < 0) {
			ERROR("Error restoring %s to %s", orig->dest, new->dest);
			goto err;
		}
		bdev_put(orig);
		return new;
	}

	pid = fork();
	if (pid < 0) {
		SYSERROR("fork");
		goto err;
	}

	if (pid > 0) {
		int ret = wait_for_pid(pid);
		bdev_put(orig);
		if (ret < 0) {
			bdev_put(new);
			return NULL;
		}
		return new;
	}

	data.orig = orig;
	data.new = new;
	if (am_unpriv())
		ret = userns_exec_1(c0->lxc_conf, rsync_rootfs_wrapper, &data);
	else
		ret = rsync_rootfs(&data);

	exit(ret == 0 ? 0 : 1);

err:
	bdev_put(orig);
	bdev_put(new);
	return NULL;
}

static struct bdev * do_bdev_create(const char *dest, const char *type,
			const char *cname, struct bdev_specs *specs)
{

	struct bdev *bdev = bdev_get(type);
	if (!bdev) {
		return NULL;
	}

	if (bdev->ops->create(bdev, dest, cname, specs) < 0) {
		 bdev_put(bdev);
		 return NULL;
	}

	return bdev;
}

/*
 * bdev_create:
 * Create a backing store for a container.
 * If successful, return a struct bdev *, with the bdev mounted and ready
 * for use.  Before completing, the caller will need to call the
 * umount operation and bdev_put().
 * @dest: the mountpoint (i.e. /var/lib/lxc/$name/rootfs)
 * @type: the bdevtype (dir, btrfs, zfs, rbd, etc)
 * @cname: the container name
 * @specs: details about the backing store to create, like fstype
 */
struct bdev *bdev_create(const char *dest, const char *type,
			const char *cname, struct bdev_specs *specs)
{
	struct bdev *bdev;
	char *best_options[] = {"btrfs", "zfs", "lvm", "dir", "rbd", NULL};

	if (!type)
		return do_bdev_create(dest, "dir", cname, specs);

	if (strcmp(type, "best") == 0) {
		int i;
		// try for the best backing store type, according to our
		// opinionated preferences
		for (i=0; best_options[i]; i++) {
			if ((bdev = do_bdev_create(dest, best_options[i], cname, specs)))
				return bdev;
		}
		return NULL;  // 'dir' should never fail, so this shouldn't happen
	}

	// -B lvm,dir
	if (strchr(type, ',') != NULL) {
		char *dup = alloca(strlen(type)+1), *saveptr = NULL, *token;
		strcpy(dup, type);
		for (token = strtok_r(dup, ",", &saveptr); token;
				token = strtok_r(NULL, ",", &saveptr)) {
			if ((bdev = do_bdev_create(dest, token, cname, specs)))
				return bdev;
		}
	}

	return do_bdev_create(dest, type, cname, specs);
}

bool rootfs_is_blockdev(struct lxc_conf *conf)
{
	const struct bdev_type *q;
	struct stat st;
	int ret;

	if (!conf->rootfs.path || strcmp(conf->rootfs.path, "/") == 0 ||
		strlen(conf->rootfs.path) == 0)
		return false;

	ret = stat(conf->rootfs.path, &st);
	if (ret == 0 && S_ISBLK(st.st_mode))
		return true;
	q = bdev_query(conf->rootfs.path);
	if (!q)
		return false;
	if (strcmp(q->name, "lvm") == 0 ||
		strcmp(q->name, "loop") == 0 ||
		strcmp(q->name, "nbd") == 0)
		return true;
	return false;
}

bool bdev_destroy(struct lxc_conf *conf)
{
	struct bdev *r;
	bool ret = false;

	r = bdev_init(conf, conf->rootfs.path, conf->rootfs.mount, NULL);
	if (!r)
		return ret;

	if (r->ops->destroy(r) == 0)
		ret = true;
	bdev_put(r);

	return ret;
}

int bdev_destroy_wrapper(void *data)
{
	struct lxc_conf *conf = data;

	if (setgid(0) < 0) {
		ERROR("Failed to setgid to 0");
		return -1;
	}
	if (setgroups(0, NULL) < 0)
		WARN("Failed to clear groups");
	if (setuid(0) < 0) {
		ERROR("Failed to setuid to 0");
		return -1;
	}
	if (!bdev_destroy(conf))
		return -1;
	else
		return 0;
}

