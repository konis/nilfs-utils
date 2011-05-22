/*
 * nilfs-revert.c - revert file in a past checkpoint of nilfs2 volume
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

#if HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif	/* HAVE_SYS_IOCTL_H */

#if HAVE_UNISTD_H
#include <unistd.h>
#endif	/* HAVE_UNISTD_H */

#if HAVE_FCNTL_H
#include <fcntl.h>	/* open */
#endif	/* HAVE_FCNTL_H */

#if HAVE_STRING_H
#include <string.h>
#endif	/* HAVE_SYS_STRING_H */

#include <sys/stat.h>
#include <assert.h>
#include <stdarg.h>	/* va_start, va_end, vfprintf */
#include <errno.h>
#include "nls.h"
#include "kern_compat.h"
#include "nilfs2_fs.h"

#ifdef _GNU_SOURCE
#include <getopt.h>
const static struct option long_option[] = {
	{"help", no_argument, NULL, 'h'},
	{"verbose", no_argument, NULL, 'v'},
	{"version", no_argument, NULL, 'V'},
	{NULL, 0, NULL, 0}
};
#define NILFS_REVERT_USAGE						\
	"Usage: %s [options] source directory\n"			\
	"  -h, --help\t\tdisplay this help and exit\n"			\
	"  -v, --verbose\t\tverbose mode\n"				\
	"  -V, --version\t\tdisplay version and exit\n"
#else
#define NILFS_REVERT_USAGE						\
	"Usage: %s [-hvV] source directory\n"
#endif	/* _GNU_SOURCE */

/* options */
static char *progname;
static int show_version_only = 0;
static int verbose = 0;

static void myprintf(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

static void nilfs_revert_usage(void)
{
	myprintf(NILFS_REVERT_USAGE, progname);
}

static void nilfs_revert_parse_options(int argc, char *argv[])
{
#ifdef _GNU_SOURCE
	int option_index;
#endif	/* _GNU_SOURCE */
	int c;

#ifdef _GNU_SOURCE
	while ((c = getopt_long(argc, argv, "hvV",
				long_option, &option_index)) >= 0) {
#else
	while ((c = getopt(argc, argv, "hvV")) >= 0) {
#endif	/* _GNU_SOURCE */
		switch (c) {
		case 'h':
			nilfs_revert_usage();
			exit(EXIT_SUCCESS);
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
	const char *source = NULL;
	const char *dir = NULL;
	__u32 arg;
	int dirfd, fd;
	int status = EXIT_FAILURE;
	int ret;

	if ((progname = strrchr(argv[0], '/')) != NULL)
		progname++;
	else
		progname = argv[0];


	nilfs_revert_parse_options(argc, argv);
	if (show_version_only) {
		myprintf(_("%s version %s\n"), progname, PACKAGE_VERSION);
		status = EXIT_SUCCESS;
		goto out;
	}

	if (optind + 2 > argc) {
		myprintf(_("Error: too few arguments\n"));
		goto out;
	}

	source = argv[optind++];
	dir = argv[optind++];

	if (optind < argc) {
		myprintf(_("Error: too many arguments.\n"));
		goto out;
	}

	dirfd = open(dir, O_RDONLY, 0);
	if (dirfd < 0) {
		myprintf(_("Error: cannot open %s\n"), dir, strerror(errno));
		goto out;
	}

	fd = open(source, O_RDONLY, 0);
	if (fd < 0) {
		myprintf(_("Error: cannot open %s\n"), source,
			 strerror(errno));
		goto out_close_dir;
	}

	arg = fd;
	ret = ioctl(dirfd, NILFS_IOCTL_REVERT, &arg);
	if (ret < 0) {
		myprintf(_("Error: revert failed: %s\n"), strerror(errno));
	} else {
		status = EXIT_SUCCESS;
	}

	close(fd);
out_close_dir:
	close(dirfd);
out:
	exit(status);
}
