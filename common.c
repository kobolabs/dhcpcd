/* 
 * dhcpcd - DHCP client daemon
 * Copyright 2006-2008 Roy Marples <roy@marples.name>
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "logger.h"

/* Handy routine to read very long lines in text files.
 * This means we read the whole line and avoid any nasty buffer overflows. */
char *get_line (FILE *fp)
{
	char *line = NULL;
	char *p;
	size_t len = 0;
	size_t last = 0;

	if (feof (fp))
		return (NULL);

	do {
		len += BUFSIZ;
		line = xrealloc (line, sizeof (char) * len);
		p = line + last;
		memset (p, 0, BUFSIZ);
		fgets (p, BUFSIZ, fp);
		last += strlen (p);
	} while (! feof (fp) && line[last - 1] != '\n');

	/* Trim the trailing newline */
	if (*line && line[--last] == '\n')
		line[last] = '\0';

	return (line);
}

/* OK, this should be in dhcpcd.c
 * It's here to make dhcpcd more readable */
#ifndef HAVE_SRANDOMDEV
void srandomdev (void)
{
	int fd;
	unsigned long seed;

	fd = open ("/dev/urandom", 0);
	if (fd == -1 || read (fd,  &seed, sizeof (seed)) == -1) {
		logger (LOG_WARNING, "Could not read from /dev/urandom: %s",
			strerror (errno));
		seed = time (0);
	}
	if (fd >= 0)
		close(fd);

	srandom (seed);
}
#endif

/* strlcpy is nice, shame glibc does not define it */
#ifndef HAVE_STRLCPY
size_t strlcpy (char *dst, const char *src, size_t size)
{
	const char *s = src;
	size_t n = size;

	if (n && --n)
		do {
			if (! (*dst++ = *src++))
				break;
		} while (--n);

	if (! n) {
		if (size)
			*dst = '\0';
		while (*src++);
	}

	return (src - s - 1);
}
#endif

/* Close our fd's */
int close_fds (void)
{
	int fd;

	if ((fd = open ("/dev/null", O_RDWR)) == -1) {
		logger (LOG_ERR, "open `/dev/null': %s", strerror (errno));
		return (-1);
	}

	dup2 (fd, fileno (stdin));
	dup2 (fd, fileno (stdout));
	dup2 (fd, fileno (stderr));
	if (fd > 2)
		close (fd);
	return (0);
}

int close_on_exec (int fd)
{
	int flags;

	if ((flags = fcntl (fd, F_GETFD, 0)) == -1
	    || fcntl (fd, F_SETFD, flags | FD_CLOEXEC) == -1)
	{
		logger (LOG_ERR, "fcntl: %s", strerror (errno));
		return (-1);
	}
	return (0);
}

/* Handy function to get the time.
 * We only care about time advancements, not the actual time itself
 * Which is why we use CLOCK_MONOTONIC, but it is not available on all
 * platforms.
 */
int get_time (struct timeval *tp)
{
#if defined(_POSIX_MONOTONIC_CLOCK) && defined(CLOCK_MONOTONIC)
	struct timespec ts;
	static clockid_t posix_clock;
	static int posix_clock_set = 0;

	if (! posix_clock_set) {
		if (sysconf (_SC_MONOTONIC_CLOCK) >= 0)
			posix_clock = CLOCK_MONOTONIC;
		else
			posix_clock = CLOCK_REALTIME;
		posix_clock_set = 1;
	}

	if (clock_gettime (posix_clock, &ts) == -1) {
		logger (LOG_ERR, "clock_gettime: %s", strerror (errno));
		return (-1);
	}

	tp->tv_sec = ts.tv_sec;
	tp->tv_usec = ts.tv_nsec / 1000;
	return (0);
#else
	if (gettimeofday (tp, NULL) == -1) {
		logger (LOG_ERR, "gettimeofday: %s", strerror (errno));
		return (-1);
	}
	return (0);
#endif
}

time_t uptime (void)
{
	struct timeval tp;

	if (get_time (&tp) == -1)
		return (-1);

	return (tp.tv_sec);
}

void writepid (int fd, pid_t pid)
{
	char spid[16];
	if (ftruncate (fd, (off_t) 0) == -1) {
		logger (LOG_ERR, "ftruncate: %s", strerror (errno));
	} else {
		ssize_t len;
		snprintf (spid, sizeof (spid), "%u", pid);
		len = pwrite (fd, spid, strlen (spid), (off_t) 0);
		if (len != (ssize_t) strlen (spid))
			logger (LOG_ERR, "pwrite: %s", strerror (errno));
	}
}

void *xmalloc (size_t s)
{
	void *value = malloc (s);

	if (value)
		return (value);

	logger (LOG_ERR, "memory exhausted");

	exit (EXIT_FAILURE);
	/* NOTREACHED */
}

void *xzalloc (size_t s)
{
	void *value = xmalloc (s);
	memset (value, 0, s);
	return (value);
}

void *xrealloc (void *ptr, size_t s)
{
	void *value = realloc (ptr, s);

	if (value)
		return (value);

	logger (LOG_ERR, "memory exhausted");
	exit (EXIT_FAILURE);
	/* NOTREACHED */
}

char *xstrdup (const char *str)
{
	char *value;

	if (! str)
		return (NULL);

	if ((value = strdup (str)))
		return (value);

	logger (LOG_ERR, "memory exhausted");
	exit (EXIT_FAILURE);
	/* NOTREACHED */
}
