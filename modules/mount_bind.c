/* ----------------------------------------------------------------------- *
 *   
 *  mount_bind.c      - module to mount a local filesystem if possible;
 *			otherwise create a symlink.
 *
 *   Copyright 2000 Transmeta Corporation - All Rights Reserved
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 675 Mass Ave, Cambridge MA 02139,
 *   USA; either version 2 of the License, or (at your option) any later
 *   version; incorporated herein by reference.
 *
 * ----------------------------------------------------------------------- */

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>

#define MODULE_MOUNT
#include "automount.h"

#define MODPREFIX "mount(bind): "

int mount_version = AUTOFS_MOUNT_VERSION;	/* Required by protocol */

static int bind_works = 0;

int mount_init(void **context)
{
	char tmp1[] = "/tmp/autoXXXXXX", *t1_dir;
	char tmp2[] = "/tmp/autoXXXXXX", *t2_dir;
	int err;
	struct stat st1, st2;

	t1_dir = mkdtemp(tmp1);
	t2_dir = mkdtemp(tmp2);
	if (t1_dir == NULL || t2_dir == NULL) {
		if (t1_dir)
			rmdir(t1_dir);
		if (t2_dir)
			rmdir(t2_dir);
		return 0;
	}

	if (lstat(t1_dir, &st1) == -1)
		goto out;

	err = spawn_mount(LOGOPT_NONE, "-n", "--bind", t1_dir, t2_dir, NULL);
	if (err == 0 &&
	    lstat(t2_dir, &st2) == 0 &&
	    st1.st_dev == st2.st_dev && st1.st_ino == st2.st_ino) {
		bind_works = 1;
	}

	if (spawn_umount(LOGOPT_NONE, "-n", t2_dir, NULL) != 0)
		debug(LOGOPT_ANY, MODPREFIX "umount failed for %s", t2_dir);

out:
	rmdir(t1_dir);
	rmdir(t2_dir);

	return 0;
}

int mount_reinit(void **context)
{
	return 0;
}

int mount_mount(struct autofs_point *ap, const char *root, const char *name, int name_len,
		const char *what, const char *fstype, const char *options, void *context)
{
	char fullpath[PATH_MAX];
	char buf[MAX_ERR_BUF];
	int err;
	int i, len;
	int symlnk = (*name != '/' && (ap->flags & MOUNT_FLAG_SYMLINK));
	void (*mountlog)(unsigned int, const char*, ...) = &log_debug;

	if (ap->flags & MOUNT_FLAG_REMOUNT)
		return 0;

	if (defaults_get_mount_verbose())
		mountlog = &log_info;

	/* Extract "symlink" pseudo-option which forces local filesystems
	 * to be symlinked instead of bound.
	 */
	if (*name != '/' && !symlnk && options) {
		const char *comma;
		int o_len = strlen(options) + 1;

		for (comma = options; *comma != '\0';) {
			const char *cp;
			const char *end;

			while (*comma == ',')
				comma++;

			/* Skip leading white space */
			while (*comma == ' ' || *comma == '\t')
				comma++;

			cp = comma;
			while (*comma != '\0' && *comma != ',')
				comma++;

			/* Skip trailing white space */
			end = comma - 1;
			while (*comma == ' ' || *comma == '\t')
				end--;

			o_len = end - cp + 1;
			if (_strncmp("symlink", cp, o_len) == 0)
				symlnk = 1;
		}
	}

	len = mount_fullpath(fullpath, PATH_MAX, root, 0, name);
	if (!len) {
		error(ap->logopt,
		      MODPREFIX "mount point path too long");
		return 1;
	}

	i = len;
	while (--i > 0 && fullpath[i] == '/')
		fullpath[i] = '\0';

	if (options == NULL || *options == '\0')
		options = "defaults";

	if (!strcmp(what, fullpath)) {
		debug(ap->logopt, MODPREFIX
		     "cannot mount or symlink %s to itself", fullpath);
		return 1;
	}

	if (!symlnk && bind_works) {
		int status, existed = 1;
		int flags;

		debug(ap->logopt, MODPREFIX "calling mkdir_path %s", fullpath);

		status = mkdir_path(fullpath, mp_mode);
		if (status && errno != EEXIST) {
			char *estr = strerror_r(errno, buf, MAX_ERR_BUF);
			error(ap->logopt,
			      MODPREFIX "mkdir_path %s failed: %s",
			      fullpath, estr);
			return 1;
		}

		if (!status)
			existed = 0;

		mountlog(ap->logopt, MODPREFIX
			 "calling mount --bind -o %s %s %s",
			  options, what, fullpath);

		err = spawn_bind_mount(ap->logopt, "-o",
				       options, what, fullpath, NULL);

		if (err) {
			if (ap->type != LKP_INDIRECT)
				return 1;

			if (!existed &&
			   (!(ap->flags & MOUNT_FLAG_GHOST) && name_len))
				rmdir_path(ap, fullpath, ap->dev);

			return err;
		} else {
			mountlog(ap->logopt,
			      MODPREFIX "mounted %s type %s on %s",
			      what, fstype, fullpath);
		}

		/* The bind mount has succeeded, now set the mount propagation.
		 *
		 * The default is propagation shared, change it if the master
		 * map entry has a different option specified.
		 */
		flags = MS_SLAVE;
		if (ap->flags & MOUNT_FLAG_PRIVATE)
			flags = MS_PRIVATE;
		else if (ap->flags & MOUNT_FLAG_SHARED)
			flags = MS_SHARED;

		/* Note: If the parent mount is propagation shared propagation
		 *  of child mounts (autofs offset mounts for example) back to
		 *  the target of the bind mount can happen in some cases and
		 *  must be avoided or autofs trigger mounts will deadlock.
		 */
		err = mount(NULL, fullpath, NULL, flags, NULL);
		if (err) {
			warn(ap->logopt,
			     "failed to set propagation for %s",
			     fullpath, root);
		}

		return 0;
	} else {
		char *cp;
		char basepath[PATH_MAX];
		int status;
		struct stat st;

		strcpy(basepath, fullpath);
		cp = strrchr(basepath, '/');

		if (cp != NULL && cp != basepath)
			*cp = '\0';

		if ((status = stat(fullpath, &st)) == 0) {
			if (S_ISDIR(st.st_mode))
				rmdir(fullpath);
		} else {
			debug(ap->logopt,
			      MODPREFIX "calling mkdir_path %s", basepath);
			if (mkdir_path(basepath, mp_mode) && errno != EEXIST) {
				char *estr;
				estr = strerror_r(errno, buf, MAX_ERR_BUF);
				error(ap->logopt,
				      MODPREFIX "mkdir_path %s failed: %s",
				      basepath, estr);
				return 1;
			}
		}

		if (symlink(what, fullpath) && errno != EEXIST) {
			error(ap->logopt,
			      MODPREFIX
			      "failed to create symlink %s -> %s",
			      fullpath, what);
			if ((ap->flags & MOUNT_FLAG_GHOST) && !status) {
				if (mkdir_path(fullpath, mp_mode) && errno != EEXIST) {
					char *estr = strerror_r(errno, buf, MAX_ERR_BUF);
					error(ap->logopt,
					      MODPREFIX "mkdir_path %s failed: %s",
					      fullpath, estr);
				}
			} else {
				if (ap->type == LKP_INDIRECT)
					rmdir_path(ap, fullpath, ap->dev);
			}
			return 1;
		} else {
			mountlog(ap->logopt,
			      MODPREFIX "symlinked %s -> %s", fullpath, what);
			return 0;
		}
	}
}

int mount_done(void *context)
{
	return 0;
}
