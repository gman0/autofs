#ident "$Id: direct.c,v 1.21 2006/03/23 20:00:13 raven Exp $"
/* ----------------------------------------------------------------------- *
 *
 *  direct.c - Linux automounter direct mount handling
 *   
 *   Copyright 1997 Transmeta Corporation - All Rights Reserved
 *   Copyright 1999-2000 Jeremy Fitzhardinge <jeremy@goop.org>
 *   Copyright 2001-2005 Ian Kent <raven@themaw.net>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 675 Mass Ave, Cambridge MA 02139,
 *   USA; either version 2 of the License, or (at your option) any later
 *   version.
 *   
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 * ----------------------------------------------------------------------- */

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/poll.h>
#include <sys/mount.h>
#include <sched.h>
#include <pwd.h>
#include <grp.h>

#include "automount.h"

void cache_dump_cache(struct mapent_cache *);

extern pthread_attr_t detach_attr;

struct mnt_params {
	char *name;
	char *options;
};

pthread_key_t key_mnt_direct_params;
pthread_key_t key_mnt_offset_params;
pthread_once_t key_mnt_params_once = PTHREAD_ONCE_INIT;

static void key_mnt_params_destroy(void *arg)
{
	struct mnt_params *mp;

	mp = (struct mnt_params *) arg;
	if (mp->name)
		free(mp->name);
	if (mp->options)
		free(mp->options);
	free(mp);
	return;
}

static void key_mnt_params_init(void)
{
	int status;

	status = pthread_key_create(&key_mnt_direct_params, key_mnt_params_destroy);
	if (status)
		fatal(status);

	status = pthread_key_create(&key_mnt_offset_params, key_mnt_params_destroy);
	if (status)
		fatal(status);

	return;
}

static int autofs_init_direct(struct autofs_point *ap)
{
	int pipefd[2];

	if ((ap->state != ST_INIT)) {
		/* This can happen if an autofs process is already running*/
		error("bad state %d", ap->state);
		return -1;
	}

	ap->pipefd = ap->kpipefd = ap->ioctlfd = -1;

	/* Pipe for kernel communications */
	if (pipe(pipefd) < 0) {
		crit("failed to create commumication pipe for autofs path %s",
		     ap->path);
		return -1;
	}

	ap->pipefd = pipefd[0];
	ap->kpipefd = pipefd[1];

	/* Pipe state changes from signal handler to main loop */
	if (pipe(ap->state_pipe) < 0) {
		crit("failed create state pipe for autofs path %s", ap->path);
		close(ap->pipefd);
		close(ap->kpipefd);
		return -1;
	}
	return 0;
}

static int do_umount_autofs_direct(struct autofs_point *ap, struct mapent *me)
{
	char buf[MAX_ERR_BUF];
	int rv, left, ret;
	int status = 1;
	struct stat st;

	left = umount_multi(ap, me->key, 1);
	if (left) {
		warn("could not unmount %d dirs under %s", left, me->key);
		return -1;
	}

	if (me->ioctlfd < 0)
		me->ioctlfd = open(me->key, O_RDONLY);

	if (me->ioctlfd >= 0) {
		ioctl(me->ioctlfd, AUTOFS_IOC_ASKUMOUNT, &status);
		ioctl(me->ioctlfd, AUTOFS_IOC_CATATONIC, 0);
		close(me->ioctlfd);
	}

	if (!status) {
		rv = 1;
		goto force_umount;
	}

/*	rv = spawnl(LOG_DEBUG, PATH_UMOUNT, PATH_UMOUNT, "-n", me->key, NULL); */
	rv = umount(me->key);
	ret = stat(me->key, &st);
	if (rv != 0) {
		if (ret == -1 && errno == ENOENT) {
			error("mount point does not exist");
			return 0;
		}
		goto force_umount;
	}

	if (ret == 0 && !S_ISDIR(st.st_mode)) {
		error("mount point is not a directory");
		return 0;
	}

force_umount:
	if (rv != 0) {
		msg("forcing umount of %s", me->key);
/*		rv = spawnl(LOG_DEBUG,
			    PATH_UMOUNT, PATH_UMOUNT, "-n", "-l", me->key, NULL); */
		rv = umount2(me->key, MNT_DETACH);
	} else
		msg("umounted %s", me->key);

	if (!rv && me->dir_created) {
		if  (rmdir(me->key) == -1) {
			char *estr = strerror_r(errno, buf, MAX_ERR_BUF);
			warn("failed to remove dir %s: %s", me->key, estr);
		}
	}
	return rv;
}

int umount_autofs_direct(struct autofs_point *ap)
{
	struct mapent_cache *mc = ap->mc;
	struct mapent *me;

	close(ap->state_pipe[0]);
	close(ap->state_pipe[1]);
	if (ap->pipefd >= 0)
		close(ap->pipefd);
	if (ap->kpipefd >= 0) {
		close(ap->kpipefd);
		ap->kpipefd = -1;
	}

	cache_readlock(mc);
	me = cache_enumerate(mc, NULL);
	while (me) {
		cache_unlock(mc);
		/* TODO: check return, locking me */
		do_umount_autofs_direct(ap, me);
		cache_readlock(mc);
		me = cache_enumerate(mc, me);
	}
	cache_unlock(mc);

	return 0;
}


int do_mount_autofs_direct(struct autofs_point *ap, struct mapent *me, int now)
{
	struct mnt_params *mp;
	time_t timeout = ap->exp_timeout;
	struct stat st;
	int status, ret;

	if (is_mounted(_PROC_MOUNTS, me->key)) {
		if (ap->state != ST_READMAP)
			debug("trigger %s already mounted", me->key);
		return 0;
	}

	status = pthread_once(&key_mnt_params_once, key_mnt_params_init);
	if (status)
		fatal(status);

	mp = pthread_getspecific(key_mnt_direct_params);
	if (!mp) {
		mp = (struct mnt_params *) malloc(sizeof(struct mnt_params));
		if (!mp) {
			crit("mnt_params value create failed for direct mount %s",
				ap->path);
			return 0;
		}
		mp->name = NULL;
		mp->options = NULL;

		status = pthread_setspecific(key_mnt_direct_params, mp);
		if (status) {
			free(mp);
			fatal(status);
		}
	}

	if (!mp->name) {
		mp->name = make_mnt_name_string(ap->path);
		if (!mp->name)
			return 0;
	}

	if (!mp->options) {
		mp->options = make_options_string(ap->path, ap->kpipefd, "direct");
		if (!mp->name) {
			free(mp->name);
			mp->name = NULL;
			return 0;
		}
	}

	/* In case the directory doesn't exist, try to mkdir it */
	if (mkdir_path(me->key, 0555) < 0) {
		if (errno != EEXIST && errno != EROFS) {
			crit("failed to create mount directory %s", me->key);
			return -1;
		}
		/* If we recieve an error, and it's EEXIST or EROFS we know
		   the directory was not created. */
		me->dir_created = 0;
	} else {
		/* No errors so the directory was successfully created */
		me->dir_created = 1;
	}
/*
	ret = spawnl(LOG_DEBUG, PATH_MOUNT, PATH_MOUNT, "-t", "autofs",
			"-n", "-o", mp->options, mp->name, me->key, NULL);
	if (ret != 0) {
		crit("failed to mount autofs path %s", me->key);
		goto out_err;
	}
*/
	ret = mount(mp->name, me->key, "autofs", MS_MGC_VAL, mp->options);
	if (ret) {
		crit("failed to mount autofs path %s", me->key);
		goto out_err;
	}

	/* Root directory for ioctl()'s */
	me->ioctlfd = open(me->key, O_RDONLY);
	if (me->ioctlfd < 0) {
		crit("failed to create ioctl fd for %s", me->key);
		goto out_umount;
	}

	/* Only calculate this first time round */
	if (ap->kver.major)
		goto got_version;

	ap->kver.major = 0;
	ap->kver.minor = 0;

	/* If this ioctl() doesn't work, it is kernel version 2 */
	if (!ioctl(me->ioctlfd, AUTOFS_IOC_PROTOVER, &ap->kver.major)) {
		 /* If this ioctl() fails the kernel doesn't support direct mounts */
		 if (ioctl(me->ioctlfd, AUTOFS_IOC_PROTOSUBVER, &ap->kver.minor)) {
			ap->kver.minor = 0;
			ap->ghost = 0;
		 }
	}

	msg("using kernel protocol version %d.%02d", ap->kver.major, ap->kver.minor);

	if (ap->kver.major < 5) {
		crit("kernel does not support direct mounts");
		goto out_close;
	}

	/* Calculate the timeouts */
	ap->exp_runfreq = (timeout + CHECK_RATIO - 1) / CHECK_RATIO;

	if (timeout) {
		msg("using timeout %d seconds; freq %d secs",
			(int) ap->exp_timeout, (int) ap->exp_runfreq);
	} else {
		msg("timeouts disabled");
	}

got_version:
	ioctl(me->ioctlfd, AUTOFS_IOC_SETTIMEOUT, &timeout);

	ret = fstat(me->ioctlfd, &st);
	if (ret == -1) {
		error("failed to stat direct mount trigger %s", me->key);
		goto out_umount;
	}
/*	cache_set_ino(me, st.st_dev, st.st_ino); */
	cache_set_ino_index(ap->mc, me->key, st.st_dev, st.st_ino);

	close(me->ioctlfd);
	me->ioctlfd = -1;

	debug("mounted trigger %s", me->key);

	return 0;

out_close:
	close(me->ioctlfd);
	me->ioctlfd = -1;
out_umount:
	/* TODO: maybe force umount (-l) */
/*	spawnl(LOG_DEBUG, PATH_UMOUNT, PATH_UMOUNT, "-n", me->key, NULL); */
	umount(me->key);
out_err:
	if (me->dir_created)
		rmdir(me->key);

	return -1;
}

int mount_autofs_direct(struct autofs_point *ap)
{
	struct mapent_cache *mc = ap->mc;
	time_t now = time(NULL);
	int map;

	if (autofs_init_direct(ap))
		return -1;

	/* TODO: check map type */
	if (!lookup_nss_read_map(ap, now)) {
		error("failed to read direct map");
		return -1;
	}

	lookup_prune_cache(ap, now);

	pthread_cleanup_push(cache_lock_cleanup, mc);
	cache_readlock(mc);
	map = lookup_enumerate(ap, do_mount_autofs_direct, now);
	cache_unlock(mc);
	pthread_cleanup_pop(0);
	if (map & LKP_FAIL) {
		if (map & LKP_INDIRECT) {
			error("bad map format, found indirect, expected direct exiting");
		} else {
			error("failed to load map, exiting");
		}
		return -1;
	}
	return 0;
}

int umount_autofs_offset(struct autofs_point *ap, struct mapent *me)
{
	char buf[MAX_ERR_BUF];
	struct stat st;
	int rv = 1, ret;

	if (me->ioctlfd < 0)
		me->ioctlfd = open(me->key, O_RDONLY);

	if (me->ioctlfd >= 0) {
		int status = 1;

		ioctl(me->ioctlfd, AUTOFS_IOC_ASKUMOUNT, &status);
		if (!status) {
			if (ap->state == ST_SHUTDOWN) {
				ioctl(me->ioctlfd, AUTOFS_IOC_CATATONIC, 0);
				close(me->ioctlfd);
				goto force_umount;
			} else {
				debug("ask umount returned busy");
				return 1;
			}
		}
		ioctl(me->ioctlfd, AUTOFS_IOC_CATATONIC, 0);
		close(me->ioctlfd);
	} else {
		error("couldn't get ioctl fd for offset %s", me->key);
		goto force_umount;
	}

/*	rv = spawnl(LOG_DEBUG, PATH_UMOUNT, PATH_UMOUNT, "-n", me->key, NULL); */
	rv = umount(me->key);
	ret = stat(me->key, &st);
	if (rv != 0) {
		if (ret == -1 && errno == ENOENT) {
			error("mount point does not exist");
			return 0;
		}
		goto force_umount;
	}

	if (ret == 0 && !S_ISDIR(st.st_mode)) {
		error("mount point is not a directory");
		return 0;
	}

force_umount:
	if (rv != 0) {
		msg("forcing umount of %s", me->key);
/*		rv = spawnl(LOG_DEBUG,
			    PATH_UMOUNT, PATH_UMOUNT, "-n", "-l", me->key, NULL); */
		rv = umount2(me->key, MNT_FORCE);
	} else
		msg("umounted %s", me->key);

	if (!rv && me->dir_created) {
		if  (rmdir(me->key) == -1) {
			char *estr = strerror_r(errno, buf, MAX_ERR_BUF);
			warn("failed to remove dir %s: %s", me->key, estr);
		}
	}
	return rv;
}

int mount_autofs_offset(struct autofs_point *ap, struct mapent *me, int is_autofs_fs)
{
	struct mnt_params *mp;
	time_t timeout = ap->exp_timeout;
	struct stat st;
	int status, ret;

	if (is_mounted(_PROC_MOUNTS, me->key)) {
		if (ap->state != ST_READMAP)
			debug("trigger %s already mounted", me->key);
		return 0;
	}

	status = pthread_once(&key_mnt_params_once, key_mnt_params_init);
	if (status)
		fatal(status);

	mp = pthread_getspecific(key_mnt_offset_params);
	if (!mp) {
		mp = (struct mnt_params *) malloc(sizeof(struct mnt_params));
		if (!mp) {
			crit("mnt_params value create failed for offset mount %s",
				me->key);
			return 0;
		}
		mp->name = NULL;
		mp->options = NULL;

		status = pthread_setspecific(key_mnt_offset_params, mp);
		if (status) {
			free(mp);
			fatal(status);
		}
	}

	if (!mp->name) {
		mp->name = make_mnt_name_string(ap->path);
		if (!mp->name)
			return 0;
	}

	if (!mp->options) {
		mp->options = make_options_string(ap->path, ap->kpipefd, "offset");
		if (!mp->name) {
			free(mp->name);
			mp->name = NULL;
			return 0;
		}
	}

	if (is_autofs_fs) {
		/* In case the directory doesn't exist, try to mkdir it */
		if (mkdir_path(me->key, 0555) < 0) {
			if (errno != EEXIST) {
				crit("failed to create mount directory %s %d",
				     me->key, errno);
				return -1;
			}
			/* 
			 * If we recieve an error, and it's EEXIST
			 * we know the directory was not created.
			 */
			me->dir_created = 0;
		} else {
			/* No errors so the directory was successfully created */
			me->dir_created = 1;
		}
	} else {
		me->dir_created = 0;

		/*
		 * We require the mount point directory to exist when
		 * installing multi-mount triggers into a host filesystem.
		 *
		 * If it doesn't exist it is not a valid part of the
		 * mount heirachy so we silently succeed here.
		 */
		if (stat(me->key, &st) == -1 && errno == ENOENT)
			return 0;
	}

	debug("calling mount -t autofs " SLOPPY "-o %s %s %s",
		mp->options, mp->name, me->key);

/*	ret = spawnl(LOG_DEBUG, PATH_MOUNT, PATH_MOUNT, "-t", "autofs",
		     "-n", SLOPPYOPT "-o", mp->options, mp->name, me->key, NULL); */
	ret = mount(mp->name, me->key, "autofs", MS_MGC_VAL, mp->options);
	if (ret) {
		crit("failed to mount autofs path %s", me->key);
		goto out_err;
	}

	if (ret != 0) {
		crit("failed to mount autofs offset trigger %s", me->key);
		goto out_err;
	}

	/* Root directory for ioctl()'s */
	me->ioctlfd = open(me->key, O_RDONLY);
	if (me->ioctlfd < 0) {
		crit("failed to create ioctl fd for %s", me->key);
		goto out_umount;
	}

	ioctl(me->ioctlfd, AUTOFS_IOC_SETTIMEOUT, &timeout);

	ret = fstat(me->ioctlfd, &st);
	if (ret == -1) {
		error("failed to stat direct mount trigger %s", me->key);
		goto out_umount;
	}

/*	cache_set_ino(me, st.st_dev, st.st_ino); */
	cache_set_ino_index(ap->mc, me->key, st.st_dev, st.st_ino);

	close(me->ioctlfd);
	me->ioctlfd = -1;

	debug("mounted trigger %s", me->key);

	return 0;

out_umount:
/*	spawnl(LOG_DEBUG, PATH_UMOUNT, PATH_UMOUNT, "-n", me->key, NULL); */
	umount(me->key);
out_err:
	if (me->dir_created)
		rmdir_path(me->key);

	return -1;
}

void *expire_proc_direct(void *arg)
{
	struct mnt_list *mnts, *next;
	struct expire_cond *ec;
	struct expire_args ex;
	struct autofs_point *ap;
	struct mapent_cache *mc;
	struct mapent *me;
	unsigned int now;
	unsigned int skip = 0;
	int ioctlfd = -1;
	int old_state, status;
	int ret;

	ec = (struct expire_cond *) arg;

	pthread_cleanup_push(expire_cleanup_unlock, ec);
	pthread_cleanup_push(expire_cleanup, &ex);

	status = pthread_mutex_lock(&ec->mutex);
	if (status)
		fatal(status);

	ap = ec->ap;
	mc = ap->mc;

	now = ec->when;

	ex.ap = ap;
	ex.when = ec->when;
	ex.status = 0;

	/* Get a list of real mounts and expire them if possible */
	mnts = get_mnt_list(_PROC_MOUNTS, "/", 0);
	for (next = mnts; next; next = next->next) {
		/* Consider only autofs mounts */
		if (!strstr(next->fs_type, "autofs"))
			continue;

		/* Skip submounts */
		if (strstr(next->opts, "indirect"))
			continue;

		/*
		 * All direct mounts must be present in the map
		 * entry cache.
		 */
		pthread_cleanup_push(cache_lock_cleanup, mc);
		cache_readlock(mc);
		me = cache_lookup(mc, next->path);
		if (!me)
			skip = 1;
		else if (me->ioctlfd >= 0)
			/* Real mounts have an open ioctl fd */
			ioctlfd = me->ioctlfd;
		else
			skip = 1;
		pthread_cleanup_pop(1);

		if (skip) {
			skip = 0;
			continue;
		}

		debug("send expire to trigger %s", next->path);

		/* Finally generate an expire message for the direct mount. */
		ret = ioctl(ioctlfd, AUTOFS_IOC_EXPIRE_DIRECT, &now);
		if (ret < 0 && errno != EAGAIN) {
			debug("failed to expire mount %s", next->path);
			ex.status = 1;
			goto done;
		} else
			sched_yield();
	}
done:
	free_mnt_list(mnts);

	pthread_cleanup_push(cache_lock_cleanup, mc);
	cache_readlock(mc);
	me = cache_enumerate(mc, NULL);
	while (me) {
		mnts = get_mnt_list(_PROC_MOUNTS, me->key, 0);
		for (next = mnts; next; next = next->next) {
			if (strstr(next->opts, "offset"))
				continue;
			/*
			 * If there's anything other than offset mounts
			 * we're still busy
			 */
			ex.status = 1;
			free_mnt_list(mnts);
			pthread_exit(NULL);
		}
		free_mnt_list(mnts);
		me = cache_enumerate(mc, me);
	}
	pthread_cleanup_pop(1);

	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);

	return NULL;
}

static void kernel_callback_cleanup(void *arg)
{
	struct autofs_point *ap;
	struct pending_args *mt;
	struct mapent_cache *mc;
	struct mapent *me;

	mt = (struct pending_args *) arg;
	ap = mt->ap;
	mc = ap->mc;

	if (mt->status) {
		send_ready(mt->ioctlfd, mt->wait_queue_token);
		if (mt->type == NFY_EXPIRE) {
			close(mt->ioctlfd);
			cache_writelock(mc);
			me = cache_lookup(mc, mt->name);
			if (me)
				me->ioctlfd = -1;
			cache_unlock(mc);
		}
	} else {
		send_fail(mt->ioctlfd, mt->wait_queue_token);
		if (mt->type == NFY_MOUNT) {
			close(mt->ioctlfd);
			cache_writelock(mc);
			me = cache_lookup(mc, mt->name);
			if (me)
				me->ioctlfd = -1;
			cache_unlock(mc);
		}
	}

	free(mt);
	return;
}

static void *do_expire_direct(void *arg)
{
	struct pending_args *mt;
	struct autofs_point *ap;
	int len;
	int status;

	mt = (struct pending_args *) arg;
	ap = mt->ap;

	mt->status = 1;
	pthread_cleanup_push(kernel_callback_cleanup, mt);

	len = _strlen(mt->name, KEY_MAX_LEN);
	if (!len) {
		warn("direct key path too long %s", mt->name);
		mt->status = 0;
		/* TODO: force umount ?? */
		pthread_exit(NULL);
	}

	status = do_expire(ap, mt->name, len);
	if (status)
		mt->status = 0;

	pthread_cleanup_pop(1);
	return NULL;
}

int handle_packet_expire_direct(struct autofs_point *ap, autofs_packet_expire_direct_t *pkt)
{
	struct mapent_cache *mc = ap->mc;
	struct mapent *me;
	struct pending_args *mt;
	char buf[MAX_ERR_BUF];
	pthread_t thid;
	int status = 0;

	/*
	 * This is a bit of a big deal.
	 * If we can't find the path and the map entry then
	 * we can't send a notification back to the kernel.
	 * Hang results.
	 *
	 * OTOH there is a mount so there should be a path
	 * and since it got mounted we have to trust that
	 * there is an entry in the cache.
	 */
	pthread_cleanup_push(cache_lock_cleanup, mc);
	cache_readlock(mc);
	me = cache_lookup_ino(mc, pkt->dev, pkt->ino);
	if (!me) {
		/*
		 * Shouldn't happen as we have been sent this following
		 * successful thread creation and lookup.
		 */
		crit("can't find map entry for (%lu,%lu)",
		    (unsigned long) pkt->dev, (unsigned long) pkt->ino);
		status = 1;
		goto done;
	}

	mt = malloc(sizeof(struct pending_args));
	if (!mt) {
		char *estr = strerror_r(errno, buf, MAX_ERR_BUF);
		error("malloc: %s", estr);
		send_fail(me->ioctlfd, pkt->wait_queue_token);
		status = 1;
		goto done;
	}

	mt->ap = ap;
	mt->ioctlfd = me->ioctlfd;
	/* TODO: check length here */
	strcpy(mt->name, me->key);
	mt->dev = me->dev;
	mt->type = NFY_EXPIRE;
	mt->wait_queue_token = pkt->wait_queue_token;

	debug("token %ld, name %s\n",
		  (unsigned long) pkt->wait_queue_token, mt->name);

	status = pthread_create(&thid, &detach_attr, do_expire_direct, mt);
	if (status) {
		error("expire thread create failed");
		free(mt);
		send_fail(mt->ioctlfd, pkt->wait_queue_token);
		status = 1;
	}
done:
	cache_unlock(mc);
	pthread_cleanup_pop(0);
	return status;
}

static void *do_mount_direct(void *arg)
{
	struct pending_args *mt;
	struct autofs_point *ap;
	struct passwd pw;
	struct passwd *ppw = &pw;
	struct passwd **pppw = &ppw;
	struct group gr;
	struct group *pgr = &gr;
	struct group **ppgr = &pgr;
	char *pw_tmp, *gr_tmp;
	struct thread_stdenv_vars *tsv;
	int tmplen;
	struct stat st;
	int status;

	mt = (struct pending_args *) arg;
	ap = mt->ap;

	mt->status = 0;
	pthread_cleanup_push(kernel_callback_cleanup, mt);

	status = fstat(mt->ioctlfd, &st);
	if (status == -1) {
		error("can't stat direct mount trigger %s", mt->name);
		pthread_exit(NULL);
	}
	if (!S_ISDIR(st.st_mode) || st.st_dev != mt->dev) {
		error("direct mount trigger is not valid mount point %s", mt->name);
		pthread_exit(NULL);
	}

	msg("attempting to mount entry %s", mt->name);

	/*
	 * Setup thread specific data values for macro
	 * substution in map entries during the mount.
	 * Best effort only as it must go ahead.
	 */

	tsv = malloc(sizeof(struct thread_stdenv_vars));
	if (!tsv) 
		goto cont;

	tsv->uid = mt->uid;
	tsv->gid = mt->gid;

	/* Try to get passwd info */

	tmplen = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (tmplen < 0) {
		error("failed to get buffer size for getpwuid_r");
		free(tsv);
		goto cont;
	}

	pw_tmp = malloc(tmplen + 1);
	if (!pw_tmp) {
		error("failed to malloc buffer for getpwuid_r");
		free(tsv);
		goto cont;
	}

	status = getpwuid_r(mt->uid, ppw, pw_tmp, tmplen, pppw);
	if (status) {
		error("failed to get passwd info from getpwuid_r");
		free(tsv);
		free(pw_tmp);
		goto cont;
	}

	tsv->user = strdup(pw.pw_name);
	if (!tsv->user) {
		error("failed to malloc buffer for user");
		free(tsv);
		free(pw_tmp);
		goto cont;
	}

	tsv->home = strdup(pw.pw_dir);
	if (!tsv->user) {
		error("failed to malloc buffer for home");
		free(pw_tmp);
		free(tsv->user);
		free(tsv);
		goto cont;
	}

	free(pw_tmp);

	/* Try to get group info */

	tmplen = sysconf(_SC_GETGR_R_SIZE_MAX);
	if (tmplen < 0) {
		error("failed to get buffer size for getgrgid_r");
		free(tsv->user);
		free(tsv->home);
		free(tsv);
		goto cont;
	}

	gr_tmp = malloc(tmplen + 1);
	if (!gr_tmp) {
		error("failed to malloc buffer for getgrgid_r");
		free(tsv->user);
		free(tsv->home);
		free(tsv);
		goto cont;
	}

	status = getgrgid_r(mt->gid, pgr, gr_tmp, tmplen, ppgr);
	if (status) {
		error("failed to get group info from getgrgid_r");
		free(tsv->user);
		free(tsv->home);
		free(tsv);
		free(gr_tmp);
		goto cont;
	}

	tsv->group = strdup(gr.gr_name);
	if (!tsv->group) {
		error("failed to malloc buffer for group");
		free(tsv->user);
		free(tsv->home);
		free(tsv);
		free(gr_tmp);
		goto cont;
	}

	free(gr_tmp);

	status = pthread_setspecific(key_thread_stdenv_vars, tsv);
	if (status) {
		error("failed to set stdenv thread var");
		free(tsv->group);
		free(tsv->user);
		free(tsv->home);
		free(tsv);
	}

cont:
	status = lookup_nss_mount(ap, mt->name, strlen(mt->name));
	/*
	 * Direct mounts are always a single mount. If it fails there's
	 * nothing to undo so just complain
	 */
	if (status) {
		msg("mounted %s", mt->name);
		mt->status = 1;
	} else
		error("failed to mount %s", mt->name);

	pthread_cleanup_pop(1);
	return NULL;
}

int handle_packet_missing_direct(struct autofs_point *ap, autofs_packet_missing_direct_t *pkt)
{
	pthread_t thid;
	struct pending_args *mt;
	struct mapent_cache *mc = ap->mc;
	struct mapent *me;
	char buf[MAX_ERR_BUF];
	int status = 0;
	int ioctlfd;

	pthread_cleanup_push(cache_lock_cleanup, mc);
	cache_readlock(mc);
	me = cache_lookup_ino(mc, pkt->dev, pkt->ino);
	if (!me) {
		/*
		 * Shouldn't happen as the kernel is telling us
		 * someone has walked on our mount point.
		 */
		crit("can't find map entry for (%lu,%lu)",
		    (unsigned long) pkt->dev, (unsigned long) pkt->ino);
		status = 1;
		goto done;
	}

	ioctlfd = open(me->key, O_RDONLY);
	if (ioctlfd < 0) {
		crit("failed to create ioctl fd for %s", me->key);
		/* TODO:  how do we clear wait q in kernel ?? */
		status = 1;
		goto done;
	}
	me->ioctlfd = ioctlfd;

	debug("token %ld, name %s, request pid %u\n",
		  (unsigned long) pkt->wait_queue_token, me->key, pkt->pid);

	/* Ignore packet if we're trying to shut down */
	if (ap->state == ST_SHUTDOWN_PENDING ||
	    ap->state == ST_SHUTDOWN_FORCE ||
	    ap->state == ST_SHUTDOWN) {
		send_fail(me->ioctlfd, pkt->wait_queue_token);
		close(me->ioctlfd);
		me->ioctlfd = -1;
		status = 1;
		goto done;
	}

	mt = malloc(sizeof(struct pending_args));
	if (!mt) {
		char *estr = strerror_r(errno, buf, MAX_ERR_BUF);
		error("malloc: %s", estr);
		send_fail(me->ioctlfd, pkt->wait_queue_token);
		close(me->ioctlfd);
		me->ioctlfd = -1;
		goto done;
	}

	mt->ap = ap;
	mt->ioctlfd = me->ioctlfd;
	/* TODO: check length here */
	strcpy(mt->name, me->key);
	mt->dev = me->dev;
	mt->type = NFY_MOUNT;
	mt->uid = pkt->uid;
	mt->gid = pkt->gid;
	mt->wait_queue_token = pkt->wait_queue_token;

	status = pthread_create(&thid, &detach_attr, do_mount_direct, mt);
	if (status) {
		error("missing mount thread create failed");
		free(mt);
		send_fail(me->ioctlfd, pkt->wait_queue_token);
		close(me->ioctlfd);
		me->ioctlfd = -1;
		status = 1;
	}
done:
	cache_unlock(mc);
	pthread_cleanup_pop(0);
	return status;
}

