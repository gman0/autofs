/* ----------------------------------------------------------------------- *
 *   
 *  mount_ext2.c - module for Linux automountd to mount ext2 filesystems
 *                 after running fsck on them.
 *
 *   Copyright 1998 Transmeta Corporation - All Rights Reserved
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
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>

#define MODULE_MOUNT
#include "automount.h"

#define MODPREFIX "mount(ext2): "

int mount_version = AUTOFS_MOUNT_VERSION;	/* Required by protocol */

int mount_init(void **context)
{
	return 0;
}

int mount_mount(struct autofs_point *ap, const char *root, const char *name, int name_len,
		const char *what, const char *fstype, const char *options, void *context)
{
	char *fullpath;
	char buf[MAX_ERR_BUF];
	const char *p, *p1;
	int err, ro = 0;
	const char *fsck_prog;
	int rlen, status, existed = 1;

	/* Root offset of multi-mount */
	if (*name == '/' && name_len == 1) {
		rlen = strlen(root);
		name_len = 0;
	/* Direct mount name is absolute path so don't use root */
	} else if (*name == '/')
		rlen = 0;
	else
		rlen = strlen(root);

	fullpath = alloca(rlen + name_len + 2);
	if (!fullpath) {
		char *estr = strerror_r(errno, buf, MAX_ERR_BUF);
		logerr(MODPREFIX "alloca: %s", estr);
		return 1;
	}

	if (name_len) {
		if (rlen)
			sprintf(fullpath, "%s/%s", root, name);
		else
			sprintf(fullpath, "%s", name);
	} else
		sprintf(fullpath, "%s", root);

	debug(ap->logopt, MODPREFIX "calling mkdir_path %s", fullpath);

	status = mkdir_path(fullpath, 0555);
	if (status && errno != EEXIST) {
		char *estr = strerror_r(errno, buf, MAX_ERR_BUF);
		error(ap->logopt,
		      MODPREFIX "mkdir_path %s failed: %s", fullpath, estr);
		return 1;
	}

	if (!status)
		existed = 0;

	if (is_mounted(_PATH_MOUNTED, fullpath, MNTS_REAL)) {
		error(ap->logopt,
		      MODPREFIX "warning: %s is already mounted", fullpath);
		return 0;
	}

	if (options && options[0]) {
		for (p = options; (p1 = strchr(p, ',')); p = p1)
			if (!strncmp(p, "ro", p1 - p) && ++p1 - p == sizeof("ro"))
				ro = 1;
		if (!strcmp(p, "ro"))
			ro = 1;
	}

#ifdef HAVE_E3FSCK
	if (!strcmp(fstype,"ext3") || !strcmp(fstype,"auto"))
		fsck_prog = PATH_E3FSCK;
	else
		fsck_prog = PATH_E2FSCK;
#else
	fsck_prog = PATH_E2FSCK;
#endif
	if (ro) {
		debug(ap->logopt,
		      MODPREFIX "calling %s -n %s", fsck_prog, what);
		err = spawnl(ap->logopt, fsck_prog, fsck_prog, "-n", what, NULL);
	} else {
		debug(ap->logopt,
		      MODPREFIX "calling %s -p %s", fsck_prog, what);
		err = spawnl(ap->logopt, fsck_prog, fsck_prog, "-p", what, NULL);
	}

	/*
	 * spawnl returns the error code, left shifted by 8 bits.  We are
	 * interested in the following error bits from the fsck program:
	 *    2 - File system errors corrected, system should be rebooted
	 *    4 - File system errors left uncorrected
	 */
	if ((err >> 8) & 6) {
		error(ap->logopt,
		      MODPREFIX "%s: filesystem needs repair, won't mount",
		      what);
		return 1;
	}

	if (options) {
		debug(ap->logopt,
		      MODPREFIX "calling mount -t %s " SLOPPY "-o %s %s %s",
		      fstype, options, what, fullpath);
		err = spawn_mount(ap->logopt, "-t", fstype,
			     SLOPPYOPT "-o", options, what, fullpath, NULL);
	} else {
		debug(ap->logopt,
		      MODPREFIX "calling mount -t %s %s %s",
		      fstype, what, fullpath);
		err = spawn_mount(ap->logopt, "-t", fstype, what, fullpath, NULL);
	}

	if (err) {
		info(ap->logopt, MODPREFIX "failed to mount %s (type %s) on %s",
		    what, fstype, fullpath);

		if (ap->type != LKP_INDIRECT)
			return 1;

		if ((!ap->ghost && name_len) || !existed)
			rmdir_path(ap, fullpath, ap->dev);

		return 1;
	} else {
		info(ap->logopt,
		      MODPREFIX "mounted %s type %s on %s",
		      what, fstype, fullpath);
		return 0;
	}
}

int mount_done(void *context)
{
	return 0;
}
