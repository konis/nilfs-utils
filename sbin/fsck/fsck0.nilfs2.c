/*
 * fsck0.nilfs2.c - correct inconsistencies of nilfs2 volume
 *
 * Licensed under GPLv2: the complete text of the GNU General Public License
 * can be found in COPYING file of the nilfs-utils package.
 *
 * Copyright (C) 2008-2011 Nippon Telegraph and Telephone Corporation.
 * Written by Ryusuke Konishi <ryusuke@osrg.net>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif	/* HAVE_CONFIG_H */

#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif	/* HAVE_SYS_TYPES_H */

#if HAVE_LINUX_TYPES_H
#include <linux/types.h>
#endif	/* HAVE_LINUX_TYPES_H */

#include <stdio.h>

#if HAVE_STDLIB_H
#include <stdlib.h>
#endif	/* HAVE_STDLIB_H */

#if HAVE_FCNTL_H
#include <fcntl.h>
#endif	/* HAVE_FCNTL_H */

#include <errno.h>

#if HAVE_UNISTD_H
#include <unistd.h>
#endif	/* HAVE_UNISTD_H */

#include <malloc.h>

#if HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif	/* HAVE_SYS_IOCTL_H */

#if HAVE_STRINGS_H
#include <strings.h>
#endif	/* HAVE_SYS_STRINGS_H */

#if HAVE_STRING_H
#include <string.h>
#endif	/* HAVE_SYS_STRING_H */

#include <endian.h>
#include <byteswap.h>

#include <stdarg.h>
#include <time.h>
#include <assert.h>

#include "../mkfs/mkfs.h"

extern __u32 crc32_le(__u32 seed, unsigned char const *data, size_t length);


#define MOUNTS			"/etc/mtab"
#define LINE_BUFFER_SIZE	256  /* Line buffer size for reading mtab */
#define MAX_SCAN_SEGMENT	50   /* Maximum number of segments which are
					tested for the latest segment search */
#define SCAN_INDICATOR_SPEED	3    /* Indicator speed (smaller value for
					higher speed) */
#define SCAN_SEGMENT_MASK	((1U << SCAN_INDICATOR_SPEED) - 1)

#define NILFS_MAX_SB_SIZE	1024 /* Maximum size of super block in bytes */
#define NILFS_SB_BLOCK_SIZE_SHIFT	10

/* fsck return codes */
#define EXIT_OK			0
#define EXIT_NONDESTRUCT	1
#define EXIT_DESTRUCT		2
#define EXIT_UNCORRECTED	4
#define EXIT_ERROR		8
#define EXIT_USAGE		16
#define EXIT_CANCEL		32
#define EXIT_LIBRARY		128

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

char *progname = NULL;

struct nilfs_log_ref {
	__u64 blocknr;   /* start blocknumber */
	__u64 seqnum;    /* sequence number */
	__u64 cno;       /* checkpoint number */
	__u64 ctime;     /* creation time */
};

static int show_version_only = 0;
static int force = 0;
static int verbose = 0;

static int devfd = -1;
static int blocksize;
static __u32 crc_seed;
static __u32 blocks_per_segment;
static __u64 first_data_block;
static __u64 nsegments;
static __u16 checkpoint_size;
static __u16 sb_bytes;

static int first_checkpoint_offset;
static int ncheckpoints_per_block;

/*
 * Generic routines
 */
void die(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	fprintf(stderr, "%s: ", progname);
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);

	if (devfd >= 0)
		close(devfd);
	exit(EXIT_ERROR);
}

static void (*nilfs_shrink)(void) = NULL;

void *nilfs_malloc(size_t size)
{
	void *p = malloc(size);
	if (!p) {
		if (nilfs_shrink)
			nilfs_shrink();
		p = malloc(size);
		if (!p)
			die("memory allocation failure");
	}
	return p;
}

static inline void *nilfs_zalloc(size_t size)
{
	void *p = nilfs_malloc(size);
	memset(p, 0, size);
	return p;
}

/*
 * Block buffer
 */
static void *block_buffer = NULL;

static void destroy_block_buffer(void)
{
	if (block_buffer) {
		free(block_buffer);
		block_buffer = NULL;
	}
}

static void init_block_buffer(void)
{
	block_buffer = nilfs_malloc(blocksize);
	atexit(destroy_block_buffer);
}

static void read_block(int fd, __u64 blocknr, void *buf,
		       unsigned long size)
{
	if (lseek64(fd, blocknr * blocksize, SEEK_SET) < 0 ||
	    read(fd, buf, size) < size)
		die("cannot read block (blocknr = %llu): %s",
		    (unsigned long long)blocknr, strerror(errno));
}

static inline __u64 segment_start_blocknr(unsigned long segnum)
{
	return segnum > 0 ? blocks_per_segment * segnum : first_data_block;
}

static int log_is_valid(int fd, __u64 log_start,
			struct nilfs_segment_summary *ss)
{
	__u32 crc, sum;
	int offset = sizeof(ss->ss_datasum);
	int nblocks = le32_to_cpu(ss->ss_nblocks);
	__u64 blocknr = log_start;

	if (le32_to_cpu(ss->ss_magic) != NILFS_SEGSUM_MAGIC)
		return 0;

	if (nblocks == 0 || nblocks > blocks_per_segment)
		return 0;

	sum = le32_to_cpu(ss->ss_datasum);

	read_block(fd, blocknr++, block_buffer, blocksize);
	crc = crc32_le(crc_seed, block_buffer + offset, blocksize - offset);
	while (--nblocks > 0) {
		read_block(fd, blocknr++, block_buffer, blocksize);
		crc = crc32_le(crc, block_buffer, blocksize);
	}
	return crc == sum;
}

/*
 * Routines to handle log (partial segment) list
 */
struct nilfs_list {  /* use struct list_head in kernel land */
	struct nilfs_list *prev;
	struct nilfs_list *next;
};

static inline void nilfs_list_init(struct nilfs_list *list)
{
	list->prev = list->next = list;
}

static inline int nilfs_list_empty(struct nilfs_list *list)
{
	return list->next == list;
}

static inline void nilfs_list_del(struct nilfs_list *list)
{
	struct nilfs_list *p = list->prev, *n = list->next;

	p->next = n;
	n->prev = p;
	list->prev = list->next = list;
}

static inline void nilfs_list_add(struct nilfs_list *list,
				  struct nilfs_list *item)
{
	struct nilfs_list *p = list->prev;

	item->prev = p;
	item->next = list;
	p->next = list->prev = item;
}

/* log (partial segment) information */
struct nilfs_log_info {
	struct nilfs_list list;
	__u64 log_start;  /* start blocknr */
	__u32 nblocks;
	struct nilfs_segment_summary segsum;  /* on-disk log header */
	__u16 flags;
};

static inline struct nilfs_log_info *nilfs_log_list_entry(struct nilfs_list *p)
{
	return (void *)p - offsetof(struct nilfs_log_info, list);
}

struct nilfs_log_info *new_log_info(__u64 blocknr)
{
	struct nilfs_log_info *loginfo = nilfs_zalloc(sizeof(*loginfo));

	loginfo->log_start = blocknr;
	nilfs_list_init(&loginfo->list);
	return loginfo;
}

static void dispose_log_list(struct nilfs_list *list)
{
	struct nilfs_list *p, *n;

	for (p = list->next; n = p->next, p != list; p = n) {
		nilfs_list_del(p);
		free(nilfs_log_list_entry(p));
	}
}

/*
 * Segment information
 */
struct nilfs_segment_info {
	struct nilfs_list list;
	struct nilfs_list log_list;	/* partial segment list */
	__u64 seg_start;		/* start blocknr of the segment */
	__u64 next;			/* pointer to the next segment */
	__u64 segseq;			/* sequence number of the segment */
	unsigned long segnum;		/* the number of the segment */
	int nlogs;			/* number of logs */
	int refcnt;
};

static struct nilfs_list segment_cache;

static struct nilfs_segment_info *
nilfs_segment_list_entry(struct nilfs_list *p)
{
	return (void *)p - offsetof(struct nilfs_segment_info, list);
}

struct nilfs_segment_info *new_segment_info(unsigned long segnum)
{
	struct nilfs_segment_info *seginfo;

	seginfo = nilfs_zalloc(sizeof(*seginfo));
	seginfo->segnum = segnum;
	seginfo->seg_start = segment_start_blocknr(segnum);
	seginfo->refcnt = 1;

	nilfs_list_init(&seginfo->log_list);
	nilfs_list_add(&segment_cache, &seginfo->list);
	return seginfo;
}

void destroy_segment_info(struct nilfs_segment_info *seginfo)
{
	nilfs_list_del(&seginfo->list);
	dispose_log_list(&seginfo->log_list);
	free(seginfo);
}

static inline struct nilfs_segment_info *
get_segment_info(struct nilfs_segment_info *seginfo)
{
	seginfo->refcnt++;
	return seginfo;
}

static inline void put_segment_info(struct nilfs_segment_info *seginfo)
{
	assert(seginfo->refcnt > 0);
	seginfo->refcnt--;
}

/*
 * Segment cache
 */
void destroy_segment_cache(void)
{
	struct nilfs_list *p, *n;

	for (p = segment_cache.next; n = p->next, p != &segment_cache; p = n) {
		destroy_segment_info(nilfs_segment_list_entry(p));
	}
}

void shrink_segment_cache(void)
{
	struct nilfs_list *p, *n;
	struct nilfs_segment_info *seginfo;

	for (p = segment_cache.next; n = p->next, p != &segment_cache; p = n) {
		seginfo = nilfs_segment_list_entry(p);
		if (seginfo->refcnt == 0)
			destroy_segment_info(seginfo);
	}
}

void init_segment_cache(void)
{
	nilfs_list_init(&segment_cache);
	nilfs_shrink = shrink_segment_cache;
	atexit(destroy_segment_cache);
}

struct nilfs_segment_info *lookup_segment(unsigned long segnum)
{
	struct nilfs_segment_info *seginfo;
	struct nilfs_list *p;

	for (p = segment_cache.next; p != &segment_cache; p = p->next) {
		seginfo = nilfs_segment_list_entry(p);
		if (seginfo->segnum == segnum) {
			get_segment_info(seginfo);
			return seginfo;
		}
	}
	return NULL;
}

struct nilfs_segment_info *load_segment(int fd, unsigned long segnum)
{
	struct nilfs_segment_info *seginfo;
	struct nilfs_log_info *loginfo;
	struct nilfs_segment_summary *ss;
	__u64 blocknr, end;

	seginfo = lookup_segment(segnum);
	if (seginfo)
		return seginfo;

	seginfo = new_segment_info(segnum);
	blocknr = seginfo->seg_start;

	posix_fadvise(fd, blocknr * blocksize, blocks_per_segment * blocksize,
		      POSIX_FADV_WILLNEED);

	loginfo = new_log_info(blocknr);
	nilfs_list_add(&seginfo->log_list, &loginfo->list);

	ss = &loginfo->segsum;
	read_block(fd, blocknr, ss, sizeof(*ss));

	if (!log_is_valid(fd, blocknr, ss)) {
		put_segment_info(seginfo);
		fprintf(stderr, "empty or bad segment: "
			"segnum = %lu, blocknr = %llu\n", segnum,
			(unsigned long long)segment_start_blocknr(segnum));
		return NULL; /* no valid partial segment found */
	}

	seginfo->segseq = le64_to_cpu(ss->ss_seq);
	seginfo->next = le64_to_cpu(ss->ss_next);

	end = blocknr + blocks_per_segment;
	do {
		seginfo->nlogs++;

		loginfo->nblocks = le32_to_cpu(ss->ss_nblocks);
		loginfo->flags = le16_to_cpu(ss->ss_flags);

		blocknr += loginfo->nblocks;
		if (blocknr >= end)
			return seginfo;

		loginfo = new_log_info(blocknr);
		nilfs_list_add(&seginfo->log_list, &loginfo->list);

		ss = &loginfo->segsum;
		read_block(fd, blocknr, ss, sizeof(*ss));

	} while (log_is_valid(fd, blocknr, ss) &&
		 le64_to_cpu(ss->ss_seq) == seginfo->segseq);

	nilfs_list_del(&loginfo->list);
	free(loginfo);

	return seginfo;
}

/*
 * Operations on segment_info structure
 */
struct nilfs_log_info *lookup_log(struct nilfs_segment_info *seginfo,
				  __u64 blocknr)
{
	struct nilfs_log_info *loginfo;
	struct nilfs_list *p;

	for (p = seginfo->log_list.next; p != &seginfo->log_list;
	     p = p->next) {
		loginfo = nilfs_log_list_entry(p);
		if (loginfo->log_start == blocknr)
			return loginfo;
	}
	return NULL;
}

struct nilfs_log_info *first_log(struct nilfs_segment_info *seginfo)
{
	return nilfs_list_empty(&seginfo->log_list) ? NULL :
		nilfs_log_list_entry(seginfo->log_list.next);
}

struct nilfs_log_info *last_log(struct nilfs_segment_info *seginfo)
{
	return nilfs_list_empty(&seginfo->log_list) ? NULL :
		nilfs_log_list_entry(seginfo->log_list.prev);
}

struct nilfs_log_info *next_log(struct nilfs_segment_info *seginfo,
				struct nilfs_log_info *loginfo)
{
	return loginfo->list.next == &seginfo->log_list ? NULL :
		nilfs_log_list_entry(loginfo->list.next);
}

struct nilfs_log_info *prev_log(struct nilfs_segment_info *seginfo,
				struct nilfs_log_info *loginfo)
{
	return loginfo->list.prev == &seginfo->log_list ? NULL :
		nilfs_log_list_entry(loginfo->list.prev);
}

struct nilfs_log_info *
lookup_last_super_root(struct nilfs_segment_info *seginfo)
{
	struct nilfs_log_info *loginfo;

	for (loginfo = last_log(seginfo); loginfo != NULL;
	     loginfo = prev_log(seginfo, loginfo)) {
		if (loginfo->flags & NILFS_SS_SR)
			return loginfo;
	}
	return NULL;
}

unsigned long log_length(struct nilfs_segment_info *seginfo)
{
	return nilfs_list_empty(&seginfo->log_list) ? 0 :
		nilfs_log_list_entry(seginfo->log_list.prev)->log_start -
		seginfo->seg_start +
		nilfs_log_list_entry(seginfo->log_list.prev)->nblocks;
}

/*
 * Routines to get latest checkpoint number
 */
static __u64 find_latest_checkpoint(int fd, __u64 cpblocknr, __u64 blkoff)
{
	struct nilfs_checkpoint *cp;
	int i, ncp;
	__u64 cno = 0;

	read_block(fd, cpblocknr, block_buffer, blocksize);
	if (blkoff == 0) {
		cp = block_buffer + first_checkpoint_offset * checkpoint_size;
		ncp = ncheckpoints_per_block - first_checkpoint_offset;
	} else {
		cp = block_buffer;
		ncp = ncheckpoints_per_block;
	}

	for (i = 0; i < ncp; i++, cp = (void *)cp + checkpoint_size) {
		if (!nilfs_checkpoint_invalid(cp) &&
		    le64_to_cpu(cp->cp_cno) > cno)
			cno = le64_to_cpu(cp->cp_cno);
	}
	return cno;
}

static void *next_ss_entry(int fd, __u64 *blocknrp,
			   unsigned *offsetp, unsigned entry_size)
{
	void *p;

	if (*offsetp + entry_size > blocksize) {
		(*blocknrp)++;
		read_block(fd, *blocknrp, block_buffer, blocksize);
		*offsetp = 0;
	}
	p = block_buffer + *offsetp;
	(*offsetp) += entry_size;
	return p;
}

static __u64 get_latest_cno(int fd, __u64 log_start)
{
	struct nilfs_segment_summary *ss;
	struct nilfs_finfo *finfo;
	__u32 nfinfo;
	__u32 nblocks, ndatablk, nnodeblk;
	__u64 ino;
	__u64 latest_cno = 0, cno;
	__u64 blocknr = log_start, fblocknr;
	unsigned offset;
	int i, j;

	read_block(fd, blocknr, block_buffer, blocksize);
	ss = block_buffer;
	nfinfo = le32_to_cpu(ss->ss_nfinfo);
	offset = le16_to_cpu(ss->ss_bytes);
	fblocknr = blocknr + DIV_ROUND_UP(le32_to_cpu(ss->ss_sumbytes),
					  blocksize);

	for (i = 0; i < nfinfo; i++) {
		finfo = next_ss_entry(fd, &blocknr, &offset, sizeof(*finfo));

		nblocks = le32_to_cpu(finfo->fi_nblocks);
		ndatablk = le32_to_cpu(finfo->fi_ndatablk);
		nnodeblk = nblocks - ndatablk;
		ino = le64_to_cpu(finfo->fi_ino);

		if (ino == NILFS_DAT_INO) {
			__le64 *blkoff;
			struct nilfs_binfo_dat *binfo_dat;

			for (j = 0; j < ndatablk; j++, fblocknr++) {
				blkoff = next_ss_entry(fd, &blocknr,
						       &offset,
						       sizeof(*blkoff));
			}
			for (j = 0; j < nnodeblk; j++, fblocknr++) {
				binfo_dat = next_ss_entry(fd, &blocknr,
							  &offset,
							  sizeof(*binfo_dat));
			}
		} else {
			struct nilfs_binfo_v *binfo_v;
			__le64 *vblocknr;

			for (j = 0; j < ndatablk; j++, fblocknr++) {
				binfo_v = next_ss_entry(fd, &blocknr,
							&offset,
							sizeof(*binfo_v));
			}
			if (ino == NILFS_CPFILE_INO && ndatablk > 0) {
				cno = find_latest_checkpoint(
					fd, fblocknr - 1,
					le64_to_cpu(binfo_v->bi_blkoff));
				if (cno > latest_cno)
					latest_cno = cno;
			}
			for (j = 0; j < nnodeblk; j++, fblocknr++) {
				vblocknr = next_ss_entry(fd, &blocknr,
							 &offset,
							 sizeof(*vblocknr));
			}
		}
	}

	return latest_cno;
}

__u64 find_latest_cno_in_logical_segment(int fd,
					 struct nilfs_segment_info *seginfo,
					 struct nilfs_log_info *start)
{
	struct nilfs_log_info *loginfo = start ? : last_log(seginfo);
	__u64 cno, latest_cno = 0;
	__u64 seq;
	int i = 0;

	if (loginfo == NULL)
		return 0;

	get_segment_info(seginfo);
	do {
		cno = get_latest_cno(fd, loginfo->log_start);
		if (cno > latest_cno)
			latest_cno = cno;

		if (loginfo->flags & NILFS_SS_LOGBGN)
			break;

		loginfo = prev_log(seginfo, loginfo);
		if (loginfo == NULL) {
			unsigned long segnum = seginfo->segnum;

			if (++i > MAX_SCAN_SEGMENT)
				break;
			segnum = (segnum == 0) ? nsegments - 1 : segnum - 1;
			seq = seginfo->segseq;

			put_segment_info(seginfo);
			seginfo = load_segment(fd, segnum);

			if (!seginfo || seginfo->segseq != seq - 1)
				break;
			loginfo = last_log(seginfo);
		}
	} while (loginfo != NULL && !(loginfo->flags & NILFS_SS_LOGEND));

	if (seginfo)
		put_segment_info(seginfo);
	return latest_cno;
}

void print_log_message(const struct nilfs_log_ref *log_ref,
		       const char *fmt, ...)
{
	const char *cp;
	va_list args;

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	fprintf(stderr, ": blocknr = %llu\n",
		(unsigned long long)log_ref->blocknr);

	for (cp = fmt; *cp == ' '; cp++)
		fputc(' ', stderr);
	fprintf(stderr, "    segnum = %lu, seq = %llu, cno=%llu\n",
		(unsigned long)log_ref->blocknr / blocks_per_segment,
		(unsigned long long)log_ref->seqnum,
		(unsigned long long)log_ref->cno);
	if (log_ref->ctime) {
		char tmbuf[LINE_BUFFER_SIZE];
		struct tm tm;
		time_t t = (time_t)le64_to_cpu(log_ref->ctime);

		localtime_r(&t, &tm);
		strftime(tmbuf, LINE_BUFFER_SIZE, "%F %T", &tm);
		for (cp = fmt; *cp == ' '; cp++)
			fputc(' ', stderr);
		fprintf(stderr, "    creation time = %s\n", tmbuf);
	}
	va_end(args);
}

struct nilfs_log_info *
find_latest_super_root(int fd, unsigned long segnum, __u64 blocknr,
		       struct nilfs_segment_info **seginfop)
{
	struct nilfs_segment_info *seginfo;
	struct nilfs_segment_info *seginfo_sr = NULL;
		/* seginfo which has the last super root */
	struct nilfs_log_info *log_sr = NULL;
	int cont = 0, invert = 0;
	int i;

	seginfo = load_segment(fd, segnum);
	if (seginfo) {
		log_sr = lookup_last_super_root(seginfo);
		if (log_sr)
			seginfo_sr = get_segment_info(seginfo);

		if (blocknr < seginfo->seg_start + log_length(seginfo))
			cont = 1;
	}

	for (i = 0; i < MAX_SCAN_SEGMENT; i++) {
		struct nilfs_segment_info *seginfo2;

		/*
		 * Look into the previous segment.
		 *
		 * This code depends on the current GC policy; discontinuously
		 * allocated segments are not supported.
		 */
		if (!(i & SCAN_SEGMENT_MASK))
			fputc('.', stderr);
		segnum = (segnum == 0) ? nsegments - 1 : segnum - 1;

		seginfo2 = load_segment(fd, segnum);
		if (!seginfo2) {
			if (log_sr && cont) {
				log_sr = NULL;
				put_segment_info(seginfo_sr);
				seginfo_sr = NULL;
			}
			cont = 0;
			if (seginfo) {
				put_segment_info(seginfo);
				seginfo = NULL;
			}
			continue;
		}

		if (!seginfo) {
			seginfo = seginfo2;
			seginfo2 = NULL;

			if (log_sr)
				put_segment_info(seginfo_sr);
			log_sr = lookup_last_super_root(seginfo);
			if (log_sr)
				seginfo_sr = get_segment_info(seginfo);
			continue;
		}

		if (seginfo2->segseq + 1 != seginfo->segseq)
			cont = 0;

		if (seginfo2->segseq > seginfo->segseq) {
			invert++;
			if (log_sr) {
				log_sr = NULL;
				put_segment_info(seginfo_sr);
				seginfo_sr = NULL;
			}
		}
		if (invert && !log_sr) {
			log_sr = lookup_last_super_root(seginfo2);
			if (log_sr) {
				put_segment_info(seginfo);
				*seginfop = seginfo2;
				fputc('\n', stderr);
				return log_sr; /* latest segment was found */
			}
		}

		if (!cont && !log_sr) {
			log_sr = lookup_last_super_root(seginfo2);
			if (log_sr)
				seginfo_sr = get_segment_info(seginfo2);
		}

		put_segment_info(seginfo);
		seginfo = seginfo2;
		seginfo2 = NULL;
	}
	fputc('\n', stderr);
	if (seginfo)
		put_segment_info(seginfo);

	if (log_sr && !cont) {
		*seginfop = seginfo_sr;
		return log_sr; /* regard second-ranking candidate
				   as the latest segment */
	}
	if (seginfo_sr)
		put_segment_info(seginfo_sr);
	return NULL;
}

static void check_mount(int fd, const char *device)
{
	FILE *fp;
	char line[LINE_BUFFER_SIZE];

	fp = fopen(MOUNTS, "r");
	if (fp == NULL)
		die("cannot open %s!", MOUNTS);

	while (fgets(line, LINE_BUFFER_SIZE, fp) != NULL) {
		if (strncmp(strtok(line, " "), device, strlen(device)) == 0) {
			fclose(fp);
			die("%s is currently mounted.", device);
		}
	}
	fclose(fp);
}

static int nilfs_sb_is_valid(struct nilfs_super_block *sbp, int check_crc)
{
	__le32 sum;
	__u32 seed, crc;

	if (le16_to_cpu(sbp->s_magic) != NILFS_SUPER_MAGIC)
		return 0;
	if (le16_to_cpu(sbp->s_bytes) > NILFS_MAX_SB_SIZE)
		return 0;
	if (!check_crc)
		return 1;

	seed = le32_to_cpu(sbp->s_crc_seed);
	sum = sbp->s_sum;
	sbp->s_sum = 0;
	crc = crc32_le(seed, (unsigned char *)sbp, le16_to_cpu(sbp->s_bytes));
	sbp->s_sum = sum;
	return crc == le32_to_cpu(sum);
}

static struct nilfs_super_block *nilfs_read_super_block(int fd)
{
	struct nilfs_super_block *sbp[2];
	__u64 devsize, sb2_offset;

	sbp[0] = malloc(NILFS_MAX_SB_SIZE);
	sbp[1] = malloc(NILFS_MAX_SB_SIZE);
	if (sbp[0] == NULL || sbp[1] == NULL)
		goto failed;

	if (ioctl(fd, BLKGETSIZE64, &devsize) != 0)
		goto failed;

	if (lseek64(fd, NILFS_SB_OFFSET_BYTES, SEEK_SET) < 0 ||
	    read(fd, sbp[0], NILFS_MAX_SB_SIZE) < 0 ||
	    !nilfs_sb_is_valid(sbp[0], 0)) {
		free(sbp[0]);
		sbp[0] = NULL;
	}

	sb2_offset = NILFS_SB2_OFFSET_BYTES(devsize);
	if (lseek64(fd, sb2_offset, SEEK_SET) < 0 ||
	    read(fd, sbp[1], NILFS_MAX_SB_SIZE) < 0 ||
	    !nilfs_sb_is_valid(sbp[1], 0))
		goto sb2_failed;

	if (sb2_offset <
	    (le64_to_cpu(sbp[1]->s_nsegments) *
	     le32_to_cpu(sbp[1]->s_blocks_per_segment)) <<
	    (le32_to_cpu(sbp[1]->s_log_block_size) +
	     NILFS_SB_BLOCK_SIZE_SHIFT))
		goto sb2_failed;

 sb2_done:
	if (!sbp[0]) {
		sbp[0] = sbp[1];
		sbp[1] = NULL;
	}

	if (sbp[1] &&
	    le64_to_cpu(sbp[1]->s_last_cno) > le64_to_cpu(sbp[0]->s_last_cno)) {
		free(sbp[0]);
		return sbp[1];
	} else if (sbp[0]) {
		free(sbp[1]);
		return sbp[0];
	}

 failed:
	free(sbp[0]);  /* free(NULL) is just ignored */
	free(sbp[1]);
	return NULL;

 sb2_failed:
	free(sbp[1]);
	sbp[1] = NULL;
	goto sb2_done;
}

static void read_sb_info(struct nilfs_super_block *sbp)
{
	char tmbuf[LINE_BUFFER_SIZE];
	struct tm tm;
	time_t t;

	fprintf(stderr, "Super-block:\n");

	crc_seed = le32_to_cpu(sbp->s_crc_seed);

	fprintf(stderr, "    revision = %d.%d\n",
		le32_to_cpu(sbp->s_rev_level),
		le16_to_cpu(sbp->s_minor_rev_level));

	blocksize = 1 << (le32_to_cpu(sbp->s_log_block_size) + 10);
	blocks_per_segment = le32_to_cpu(sbp->s_blocks_per_segment);
	first_data_block = le64_to_cpu(sbp->s_first_data_block);
	nsegments = le64_to_cpu(sbp->s_nsegments);
	checkpoint_size = le16_to_cpu(sbp->s_checkpoint_size);
	sb_bytes = le16_to_cpu(sbp->s_bytes);

	first_checkpoint_offset =
		DIV_ROUND_UP(sizeof(struct nilfs_cpfile_header),
			     checkpoint_size);
	ncheckpoints_per_block = blocksize / checkpoint_size;

	t = (time_t)le64_to_cpu(sbp->s_wtime);
	localtime_r(&t, &tm);
	strftime(tmbuf, LINE_BUFFER_SIZE, "%F %T", &tm);

	fprintf(stderr, "    blocksize = %d\n", blocksize);
	fprintf(stderr, "    write time = %s\n", tmbuf);
}

static void commit_super_block(struct nilfs_super_block *sbp,
			       const struct nilfs_log_ref *log_ref)
{
	__u32 sbsum;

	sbp->s_last_pseg = cpu_to_le64(log_ref->blocknr);
	sbp->s_last_seq = cpu_to_le64(log_ref->seqnum);
	sbp->s_last_cno = cpu_to_le64(log_ref->cno);

	sbp->s_wtime = cpu_to_le64(time(NULL));
	sbp->s_state = cpu_to_le16(le16_to_cpu(sbp->s_state) & ~NILFS_VALID_FS);

	/* fill in crc */
	sbp->s_sum = 0;
	sbsum = crc32_le(crc_seed, (unsigned char *)sbp, sb_bytes);
	sbp->s_sum = cpu_to_le32(sbsum);
}

static int nilfs_write_super_block(int fd, struct nilfs_super_block *sbp)
{
	__u64 devsize, sb2_offset;
	int ret = -1;

	if (ioctl(fd, BLKGETSIZE64, &devsize) != 0)
		return -1;

	if (lseek64(fd, NILFS_SB_OFFSET_BYTES, SEEK_SET) < 0 ||
	    write(fd, sbp, sb_bytes) < sb_bytes ||
	    fsync(fd) < 0)
		fprintf(stderr, "failed to write primary super block");
	else
		ret = 0;

	sb2_offset = NILFS_SB2_OFFSET_BYTES(devsize);
	if (sb2_offset < (__u64)nsegments * blocks_per_segment * blocksize)
		return ret;

	if (lseek64(fd, sb2_offset, SEEK_SET) < 0 ||
	    write(fd, sbp, sb_bytes) <  sb_bytes ||
	    fsync(fd) < 0)
		fprintf(stderr,
			"failed to write secondary super block");
	else
		ret = 0;

	return ret;
}

static int test_latest_log(int fd, struct nilfs_log_ref *log_ref)
{
	struct nilfs_segment_info *seginfo;
	struct nilfs_log_info *loginfo;
	unsigned long segnum;
	int ret = -1;

	/*
	 * check the log the super block points to
	 */
	segnum = log_ref->blocknr / blocks_per_segment;
	seginfo = load_segment(fd, segnum);
	if (seginfo) {
		loginfo = lookup_log(seginfo, log_ref->blocknr);
		if (loginfo && seginfo->segseq == log_ref->seqnum &&
		    loginfo->flags & NILFS_SS_SR) {
			log_ref->ctime =
				le64_to_cpu(loginfo->segsum.ss_create);
			print_log_message(log_ref,
					  "A valid log is pointed to by "
					  "superblock (No change needed)");
			ret = 0;
		}
		put_segment_info(seginfo);
	}
	return ret;
}

static void nilfs_do_rollback(int fd, struct nilfs_log_ref *log_ref)
{
	struct nilfs_segment_info *seginfo;
	struct nilfs_log_info *loginfo;
	unsigned long segnum;

	/*
	 * check logs in the current and prior full segments.
	 */
	segnum = log_ref->blocknr / blocks_per_segment;
	loginfo = find_latest_super_root(fd, segnum, log_ref->blocknr,
					 &seginfo);
	if (!loginfo)
		die("Cannot find super root");

	log_ref->blocknr = loginfo->log_start;
	log_ref->seqnum = seginfo->segseq;
	log_ref->ctime = le64_to_cpu(loginfo->segsum.ss_create);

	if (le16_to_cpu(loginfo->segsum.ss_bytes) >= 64) {
		/* has ss_cno */
		log_ref->cno = le64_to_cpu(loginfo->segsum.ss_cno);
	} else {
		fprintf(stderr, "Searching the latest checkpoint.\n");
		log_ref->cno = find_latest_cno_in_logical_segment(fd, seginfo,
								  loginfo);
		if (log_ref->cno == 0)
			die("Cannot identify the latest checkpoint");
	}

	print_log_message(log_ref, "Selected log");
}

static void nilfs_fsck(const char *device)
{
	struct nilfs_super_block *sbp;
	struct nilfs_log_ref log_ref;
	int clean, ret;
	int c;

	if ((devfd = open(device, O_RDONLY | O_LARGEFILE)) < 0)
		die("cannot open device %s", device);

	check_mount(devfd, device);

	sbp = nilfs_read_super_block(devfd);
	if (!sbp)
		die("cannot read super block (device=%s)", device);

	read_sb_info(sbp);

	log_ref.blocknr = le64_to_cpu(sbp->s_last_pseg);
	log_ref.seqnum = le64_to_cpu(sbp->s_last_seq);
	log_ref.cno = le64_to_cpu(sbp->s_last_cno);
	log_ref.ctime = 0;
	print_log_message(&log_ref, "    indicated log");
	fputc('\n', stderr);

	if (le16_to_cpu(sbp->s_state) & NILFS_VALID_FS) {
		fprintf(stderr, "Clean FS.\n");
		clean = 1;
	} else {
		fprintf(stderr, "Unclean FS.\n");
		clean = 0;
	}

	init_block_buffer();
	init_segment_cache();

	ret = test_latest_log(devfd, &log_ref);
	if (ret < 0) {
		fprintf(stderr, "The latest log is lost. "
			"Trying rollback recovery..\n");
		clean = 0;
		nilfs_do_rollback(devfd, &log_ref);
	}
	destroy_segment_cache();
	destroy_block_buffer();

	if (!ret)
		goto out;

	/*
	 * Reopen device to update superblock
	 */
	close(devfd);
	devfd = -1;
	if ((devfd = open(device, O_RDWR | O_LARGEFILE)) < 0)
		die("cannot open device %s in read/write mode", device);

	fprintf(stderr, "Do you wish to overwrite super block (y/N)? ");
	if ((c = getchar()) == 'y' || c == 'Y') {
		commit_super_block(sbp, &log_ref);
		if (nilfs_write_super_block(devfd, sbp) < 0)
			die("couldn't update super block (device=%s)", device);
	}
 out:
	if (!clean)
		fprintf(stderr, "Recovery will complete on mount.\n");
	free(sbp);
	close(devfd);
}

static void usage(void)
{
	fprintf(stderr, "Usage: %s [-fv] device\n", progname);
	exit(EXIT_USAGE);
}

static void parse_options(int argc, char *argv[])
{
	int c;

	while ((c = getopt(argc, argv, "fvV")) != EOF) {
		switch (c) {
		case 'f':
			force = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'V':
			show_version_only = 1;
			break;
		default:
			usage();
		}
	}
	if (show_version_only)
		return;
	if (optind == argc)
		usage();
}

int main(int argc, char *argv[])
{
	char *device;

	if ((progname = strrchr(argv[0], '/')) != NULL)
		progname++;
	else
		progname = argv[0];

	parse_options(argc, argv);
	if (show_version_only) {
		fprintf(stderr, "%s version %s\n", progname, PACKAGE_VERSION);
		exit(EXIT_OK);
	}
	device = argv[optind];
	nilfs_fsck(device);

	exit(EXIT_OK);
}
