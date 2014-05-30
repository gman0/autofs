/* ----------------------------------------------------------------------- *
 *
 *  log.c - applcation logging routines.
 *
 *   Copyright 2004 Denis Vlasenko <vda@port.imtp.ilyichevsk.odessa.ua>
 *				 - All Rights Reserved
 *   Copyright 2005 Ian Kent <raven@themaw.net> - All Rights Reserved
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 675 Mass Ave, Cambridge MA 02139,
 *   USA; either version 2 of the License, or (at your option) any later
 *   version; incorporated herein by reference.
 *
 *  This module has been adapted from patches submitted by:
 *	Denis Vlasenko <vda@port.imtp.ilyichevsk.odessa.ua>
 *	Thanks Denis.
 *
 * ----------------------------------------------------------------------- */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "automount.h"

static unsigned int syslog_open = 0;
static unsigned int logging_to_syslog = 0;

/* log notification level */
static unsigned int do_verbose = 0;		/* Verbose feedback option */
static unsigned int do_debug = 0;		/* Full debug output */

static char *prepare_attempt_prefix(const char *msg)
{
	unsigned long *attempt_id;
	char buffer[ATTEMPT_ID_SIZE + 1];
	char *prefixed_msg = NULL;

	attempt_id = pthread_getspecific(key_thread_attempt_id);
	if (attempt_id) {
		int len = sizeof(buffer) + 1 + strlen(msg) + 1;

		snprintf(buffer, ATTEMPT_ID_SIZE, "%02lx", *attempt_id);
		prefixed_msg = (char *) calloc(len, sizeof(char));
		strcpy(prefixed_msg, buffer);
		strcat(prefixed_msg, "|");
		strcat(prefixed_msg, msg);
	}

	return prefixed_msg;
}

void set_log_norm(void)
{
	do_verbose = 0;
	do_debug = 0;
	return;
}

void set_log_verbose(void)
{
	do_verbose = 1;
	return;
}

void set_log_debug(void)
{
	do_debug = 1;
	return;
}

void set_log_norm_ap(struct autofs_point *ap)
{
	ap->logopt = LOGOPT_ERROR;
	return;
}

void set_log_verbose_ap(struct autofs_point *ap)
{
	ap->logopt = LOGOPT_VERBOSE;
	return;
}

void set_log_debug_ap(struct autofs_point *ap)
{
	ap->logopt = LOGOPT_DEBUG;
	return;
}

void log_info(unsigned int logopt, const char *msg, ...)
{
	unsigned int opt_log = logopt & (LOGOPT_DEBUG | LOGOPT_VERBOSE);
	char *prefixed_msg;
	va_list ap;

	if (!do_debug && !do_verbose && !opt_log)
		return;

	prefixed_msg = prepare_attempt_prefix(msg);

	va_start(ap, msg);
	if (logging_to_syslog) {
		if (prefixed_msg)
			vsyslog(LOG_INFO, prefixed_msg, ap);
		else
			vsyslog(LOG_INFO, msg, ap);
	} else {
		if (prefixed_msg)
			vfprintf(stderr, prefixed_msg, ap);
		else
			vfprintf(stderr, msg, ap);
		fputc('\n', stderr);
	}
	va_end(ap);

	if (prefixed_msg)
		free(prefixed_msg);

	return;
}

void log_notice(unsigned int logopt, const char *msg, ...)
{
	unsigned int opt_log = logopt & (LOGOPT_DEBUG | LOGOPT_VERBOSE);
	char *prefixed_msg;
	va_list ap;

	if (!do_debug && !do_verbose && !opt_log)
		return;

	prefixed_msg = prepare_attempt_prefix(msg);

	va_start(ap, msg);
	if (logging_to_syslog) {
		if (prefixed_msg)
			vsyslog(LOG_NOTICE, prefixed_msg, ap);
		else
			vsyslog(LOG_INFO, msg, ap);
	} else {
		if (prefixed_msg)
			vfprintf(stderr, prefixed_msg, ap);
		else
			vfprintf(stderr, msg, ap);
		fputc('\n', stderr);
	}
	va_end(ap);

	if (prefixed_msg)
		free(prefixed_msg);

	return;
}

void log_warn(unsigned int logopt, const char *msg, ...)
{
	unsigned int opt_log = logopt & (LOGOPT_DEBUG | LOGOPT_VERBOSE);
	char *prefixed_msg;
	va_list ap;

	if (!do_debug && !do_verbose && !opt_log)
		return;

	prefixed_msg = prepare_attempt_prefix(msg);

	va_start(ap, msg);
	if (logging_to_syslog) {
		if (prefixed_msg)
			vsyslog(LOG_WARNING, prefixed_msg, ap);
		else
			vsyslog(LOG_INFO, msg, ap);
	} else {
		if (prefixed_msg)
			vfprintf(stderr, prefixed_msg, ap);
		else
			vfprintf(stderr, msg, ap);
		fputc('\n', stderr);
	}
	va_end(ap);

	if (prefixed_msg)
		free(prefixed_msg);

	return;
}

void log_error(unsigned logopt, const char *msg, ...)
{
	char *prefixed_msg;
	va_list ap;

	prefixed_msg = prepare_attempt_prefix(msg);

	va_start(ap, msg);
	if (logging_to_syslog) {
		if (prefixed_msg)
			vsyslog(LOG_ERR, prefixed_msg, ap);
		else
			vsyslog(LOG_INFO, msg, ap);
	} else {
		if (prefixed_msg)
			vfprintf(stderr, prefixed_msg, ap);
		else
			vfprintf(stderr, msg, ap);
		fputc('\n', stderr);
	}
	va_end(ap);

	if (prefixed_msg)
		free(prefixed_msg);

	return;
}

void log_crit(unsigned logopt, const char *msg, ...)
{
	char *prefixed_msg;
	va_list ap;

	prefixed_msg = prepare_attempt_prefix(msg);

	va_start(ap, msg);
	if (logging_to_syslog) {
		if (prefixed_msg)
			vsyslog(LOG_CRIT, prefixed_msg, ap);
		else
			vsyslog(LOG_INFO, msg, ap);
	} else {
		if (prefixed_msg)
			vfprintf(stderr, prefixed_msg, ap);
		else
			vfprintf(stderr, msg, ap);
		fputc('\n', stderr);
	}
	va_end(ap);

	if (prefixed_msg)
		free(prefixed_msg);

	return;
}

void log_debug(unsigned int logopt, const char *msg, ...)
{
	unsigned int opt_log = logopt & LOGOPT_DEBUG;
	char *prefixed_msg;
	va_list ap;

	if (!do_debug && !opt_log)
		return;

	prefixed_msg = prepare_attempt_prefix(msg);

	va_start(ap, msg);
	if (logging_to_syslog) {
		if (prefixed_msg)
			vsyslog(LOG_WARNING, prefixed_msg, ap);
		else
			vsyslog(LOG_INFO, msg, ap);
	} else {
		if (prefixed_msg)
			vfprintf(stderr, prefixed_msg, ap);
		else
			vfprintf(stderr, msg, ap);
		fputc('\n', stderr);
	}
	va_end(ap);

	if (prefixed_msg)
		free(prefixed_msg);

	return;
}

void logmsg(const char *msg, ...)
{
	char *prefixed_msg;
	va_list ap;

	prefixed_msg = prepare_attempt_prefix(msg);

	va_start(ap, msg);
	if (logging_to_syslog) {
		if (prefixed_msg)
			vsyslog(LOG_CRIT, prefixed_msg, ap);
		else
			vsyslog(LOG_INFO, msg, ap);
	} else {
		if (prefixed_msg)
			vfprintf(stderr, prefixed_msg, ap);
		else
			vfprintf(stderr, msg, ap);
		fputc('\n', stderr);
	}
	va_end(ap);

	if (prefixed_msg)
		free(prefixed_msg);

	return;
}

void open_log(void)
{
	if (!syslog_open) {
		syslog_open = 1;
		openlog("automount", LOG_PID, LOG_DAEMON);
	}

	logging_to_syslog = 1;
	return;
}

void log_to_syslog(void)
{
	char buf[MAX_ERR_BUF];
	int nullfd;

	open_log();

	/* Redirect all our file descriptors to /dev/null */
	nullfd = open("/dev/null", O_RDWR);
	if (nullfd < 0) {
		char *estr = strerror_r(errno, buf, MAX_ERR_BUF);
		fprintf(stderr, "cannot open /dev/null: %s", estr);
		exit(1);
	}

	if (dup2(nullfd, STDIN_FILENO) < 0 ||
	    dup2(nullfd, STDOUT_FILENO) < 0 ||
	    dup2(nullfd, STDERR_FILENO) < 0) {
		char *estr = strerror_r(errno, buf, MAX_ERR_BUF);
		fprintf(stderr,
			"redirecting file descriptors failed: %s", estr);
		exit(1);
	}

	if (nullfd > 2)
		close(nullfd);

	return;
}

void log_to_stderr(void)
{
	if (syslog_open) {
		syslog_open = 0;
		closelog();
	}

	logging_to_syslog = 0;

	return;
}
