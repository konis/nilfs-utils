/*
 * nilfs-diff.c - show changes between two checkpoints of nilfs2 volume
 *
 * Licensed under GPLv2: the complete text of the GNU General Public License
 * can be found in COPYING file of the nilfs-utils package.
 *
 * Copyright (C) 2011 Nippon Telegraph and Telephone Corporation.
 * Written by Ryusuke Konishi <konishi.ryusuke@lab.ntt.co.jp>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif	/* HAVE_CONFIG_H */

#include <stdio.h>

#if HAVE_STDLIB_H
#include <stdlib.h>
#endif	/* HAVE_STDLIB_H */

#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif	/* HAVE_SYS_TYPES_H */

#if HAVE_LINUX_TYPES_H
#include <linux/types.h>
#endif	/* HAVE_LINUX_TYPES_H */

#if HAVE_FCNTL_H
#include <fcntl.h>
#endif	/* HAVE_FCNTL_H */

#if HAVE_UNISTD_H
#include <unistd.h>
#endif	/* HAVE_UNISTD_H */

#if HAVE_LIMITS_H
#include <limits.h>
#endif	/* HAVE_LIMITS_H */

#if HAVE_STRING_H
#include <string.h>
#endif	/* HAVE_SYS_STRING_H */

#if HAVE_SYSLOG_H
#include <syslog.h>	/* LOG_ERR, and so forth */
#endif	/* HAVE_SYSLOG_H */

#include <sys/stat.h>
#include <setjmp.h>
#include <assert.h>
#include <stdarg.h>	/* va_start, va_end, vfprintf */
#include <errno.h>
#include <signal.h>
#include "nls.h"
#include "nilfs.h"
#include "cno.h"

#ifdef _GNU_SOURCE
#include <getopt.h>
const static struct option long_option[] = {
	{"help", no_argument, NULL, 'h'},
	{"inode", no_argument, NULL, 'i'},
	{"brief", no_argument, NULL, 'q'},
	{"stat", no_argument, NULL, 's'},
	{"verbose", no_argument, NULL, 'v'},
	{"version", no_argument, NULL, 'V'},
	{NULL, 0, NULL, 0}
};
#define NILFS_DIFF_USAGE						\
	"Usage: %s [options] [device] cno1..cno2\n"			\
	"  -h, --help\t\tdisplay this help and exit\n"			\
	"  -i, --inode\t\tprint the inode number of changes files\n"	\
	"  -q, --brief\t\toutput only whether files differ\n"		\
	"  -s, --stat\t\tshow statistical information\n"		\
	"  -v, --verbose\t\tverbose mode\n"				\
	"  -V, --version\t\tdisplay version and exit\n"
#else
#define NILFS_DIFF_USAGE						\
	"Usage: %s [-hiqsvV] [device] cno1..cno2\n"
#endif	/* _GNU_SOURCE */

/* general macros */
#ifndef min_t
#define min_t(type,x,y) \
	({ type __x = (x); type __y = (y); __x < __y ? __x : __y; })
#define max_t(type,x,y) \
	({ type __x = (x); type __y = (y); __x > __y ? __x : __y; })
#endif

/* options */
static char *progname;
static int show_version_only = 0;
static int verbose = 0;
static int show_ino = 1;
static int show_stat = 0;
static int brief = 0;

static unsigned long ndeleted = 0;
static unsigned long ncreated = 0;
static unsigned long nmodified = 0;

#define NILFS_DIFF_BUFSIZE	128
#define NILFS_DIFF_NCHANGES	512

static struct nilfs_inode_change changes[NILFS_DIFF_NCHANGES];


static void myprintf(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

static void nilfs_diff_comparison_error(int err)
{
	myprintf(_("Error: failed to compare checkpoints: %s\n"),
		 strerror(err));
	if (err == ENOTTY) {
		myprintf(_("       This kernel does not support diff API.\n"));
	}
}

static void nilfs_count_ino_diff(struct nilfs_inode_change *ic)
{
	if (ic->ic_flags & NILFS_IC_CREATE)
		ncreated++;
	if (ic->ic_flags & NILFS_IC_DELETE)
		ndeleted++;
	if (!(ic->ic_flags & (NILFS_IC_CREATE | NILFS_IC_DELETE))) {
		if (ic->ic_flags || ic->ic_attr)
			nmodified++;
	}
}

static void nilfs_print_ino_diff(struct nilfs_inode_change *ic)
{
	if (ic->ic_flags & NILFS_IC_CREATE)
		printf(_("+ %llu\n"), (unsigned long long)ic->ic_ino);
	if (ic->ic_flags & NILFS_IC_DELETE)
		printf(_("- %llu\n"), (unsigned long long)ic->ic_ino);
	if (!(ic->ic_flags & (NILFS_IC_CREATE | NILFS_IC_DELETE))) {
		if (ic->ic_flags || ic->ic_attr)
			printf(_("M %llu\n"), (unsigned long long)ic->ic_ino);
	}
}

static void nilfs_print_diff_stat(void)
{
	printf(_("%lu file%s created\n"), ncreated, ncreated > 1 ? "s" : "");
	printf(_("%lu file%s deleted\n"), ndeleted, ndeleted > 1 ? "s" : "");
	printf(_("%lu file%s modified\n"), nmodified, nmodified > 1 ? "s" : "");
}

static int nilfs_do_diff(struct nilfs *nilfs, const char *device,
			 nilfs_cno_t cno1, nilfs_cno_t cno2)
{
	struct nilfs_inode_change *ic;
	int status = EXIT_FAILURE;
	ino_t ino = NILFS_ROOT_INO;
	ssize_t nc;

	do {
		nc = nilfs_compare_checkpoints(
			nilfs, cno1, cno2, NILFS_COMPARE_INODES, ino,
			changes, NILFS_DIFF_NCHANGES,
			sizeof(struct nilfs_inode_change));
		if (nc < 0) {
			nilfs_diff_comparison_error(errno);
			goto out;
		}
		if (!nc)
			break;

		for (ic = changes; ic < changes + nc; ic++) {
			if (ic->ic_ino < NILFS_USER_INO &&
			    ic->ic_ino != NILFS_ROOT_INO)
				continue;
			if (show_stat) {
				nilfs_count_ino_diff(ic);
			} else if (brief) {
				printf(_("Checkpoint %llu and %llu differ\n"),
				       (unsigned long long)cno1,
				       (unsigned long long)cno2);
				status = EXIT_SUCCESS;
				goto out;
			} else {
				nilfs_print_ino_diff(ic);
			}
		}
		ino = changes[nc - 1].ic_ino + 1;

	} while (nc == NILFS_DIFF_NCHANGES);

	status = EXIT_SUCCESS;
	if (show_stat)
		nilfs_print_diff_stat();
out:
	return status;
}

static int nilfs_diff(struct nilfs *nilfs, const char *device,
		      nilfs_cno_t cno1, nilfs_cno_t cno2)
{
	sigset_t sigset, oldset;
	int status = EXIT_FAILURE;

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGTERM);
	sigaddset(&sigset, SIGHUP);
	if (sigprocmask(SIG_BLOCK, &sigset, &oldset) < 0) {
		myprintf(_("Error: cannot block signals: %s\n"),
			   strerror(errno));
		goto out;
	}

	if (nilfs_lock_cleaner(nilfs) < 0) {
		myprintf(_("Error: cannot lock cleaner: %s\n"),
			 strerror(errno));
		goto out_unblock_signal;
	}
	status = nilfs_do_diff(nilfs, device, cno1, cno2);

	nilfs_unlock_cleaner(nilfs);

out_unblock_signal:
	sigprocmask(SIG_SETMASK, &oldset, NULL);
out:
	return status;
}

static void nilfs_diff_usage(void)
{
	myprintf(NILFS_DIFF_USAGE, progname);
}

static void nilfs_diff_parse_options(int argc, char *argv[])
{
#ifdef _GNU_SOURCE
	int option_index;
#endif	/* _GNU_SOURCE */
	int c;

#ifdef _GNU_SOURCE
	while ((c = getopt_long(argc, argv, "hiqsvV",
				long_option, &option_index)) >= 0) {
#else
	while ((c = getopt(argc, argv, "hiqsvV")) >= 0) {
#endif	/* _GNU_SOURCE */
		switch (c) {
		case 'h':
			nilfs_diff_usage();
			exit(EXIT_SUCCESS);
			break;
		case 'i':
			show_ino = 1;
			break;
		case 'q':
			brief = 1;
			break;
		case 's':
			show_stat = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'V':
			show_version_only = 1;
			break;
		default:
			myprintf(_("Error: invalid option -- %c\n"), optopt);
			exit(EXIT_FAILURE);
		}
	}
}

int main(int argc, char *argv[])
{
	char *dev = NULL, *range = NULL;
	struct nilfs *nilfs;
	struct nilfs_cpstat cpstat;
	nilfs_cno_t start, end, oldest;
	int status = EXIT_FAILURE;

	if ((progname = strrchr(argv[0], '/')) != NULL)
		progname++;
	else
		progname = argv[0];


	nilfs_diff_parse_options(argc, argv);
	if (show_version_only) {
		myprintf(_("%s version %s\n"), progname, PACKAGE_VERSION);
		status = EXIT_SUCCESS;
		goto out;
	}

	if (optind >= argc) {
		myprintf(_("Error: too few arguments\n"));
		goto out;
	} else if (optind + 1 == argc) {
		dev = NULL;
	} else {
		dev = argv[optind++];
	}

	range = argv[optind++];

	if (optind < argc) {
		myprintf(_("Error: too many arguments.\n"));
		goto out;
	}

	nilfs = nilfs_open(dev, NULL, NILFS_OPEN_RDWR | NILFS_OPEN_GCLK);
	if (nilfs == NULL) {
		myprintf(_("Error: cannot open NILFS on %s: %s\n"), dev,
			 strerror(errno));
		goto out;
	}

	if (nilfs_get_cpstat(nilfs, &cpstat) < 0) {
		myprintf(_("Error: cannot get checkpoint status: %s\n"),
			 strerror(errno));
		goto out_close;
	}

	if (nilfs_parse_cno_range(range, &start, &end, 10) < 0 ||
	    start > end || start < NILFS_CNO_MIN) {
		myprintf(_("Error: invalid checkpoint range: %s\n"), range);
		goto out_close;
	}

	if (start != end) {
		oldest = nilfs_get_oldest_cno(nilfs);
		if (start < oldest)
			start = oldest;
		if (end >= cpstat.cs_cno)
			end = cpstat.cs_cno - 2;
		if (start > end) {
			myprintf(_("Error: invalid checkpoint range: %s\n"),
				 range);
			goto out_close;
		}
	}

	status = nilfs_diff(nilfs, dev, start, end);

out_close:	
	nilfs_close(nilfs);
out:
	exit(status);
}
