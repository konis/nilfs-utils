/*
 * sb.c - NILFS super block access library
 *
 * Copyright (C) 2005-2012 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * Written by Ryusuke Konishi <konishi.ryusuke@gmail.com>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif	/* HAVE_CONFIG_H */

#include <stdio.h>

#if HAVE_STDLIB_H
#include <stdlib.h>
#endif	/* HAVE_STDLIB_H */

#if HAVE_STRING_H
#include <string.h>
#endif	/* HAVE_STRING_H */

#if HAVE_UNISTD_H
#include <unistd.h>
#endif	/* HAVE_UNISTD_H */

#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif	/* HAVE_SYS_TYPES_H */

#if HAVE_FCNTL_H
#include <fcntl.h>
#endif	/* HAVE_FCNTL_H */

#if HAVE_LIMITS_H
#include <limits.h>
#endif	/* HAVE_LIMITS_H */

#if HAVE_LINUX_TYPES_H
#include <linux/types.h>
#endif	/* HAVE_LINUX_TYPES_H */

#include <linux/fs.h>

#if HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif	/* HAVE_SYS_IOCTL_H */

#include <linux/nilfs2_ondisk.h>
#include <sys/stat.h>  /* fstat(), S_ISBLK(), S_ISREG(), etc */
#include <errno.h>
#include <assert.h>
#include "nilfs.h"
#include "compat.h"
#include "util.h"
#include "crc32.h"

#define NILFS_MAX_SB_SIZE	1024

static uint32_t nilfs_sb_check_sum(struct nilfs_super_block *sbp)
{
	uint32_t seed, crc;
	__le32 sum;

	seed = le32_to_cpu(sbp->s_crc_seed);
	sum = sbp->s_sum;
	sbp->s_sum = 0;
	crc = crc32_le(seed, (unsigned char *)sbp, le16_to_cpu(sbp->s_bytes));
	sbp->s_sum = sum;
	return crc;
}

static int nilfs_sb_is_valid(struct nilfs_super_block *sbp, int check_crc)
{
	uint32_t crc;

	if (le16_to_cpu(sbp->s_magic) != NILFS_SUPER_MAGIC)
		return 0;
	if (unlikely(le16_to_cpu(sbp->s_bytes) > NILFS_MAX_SB_SIZE))
		return 0;
	if (!check_crc)
		return 1;

	crc = nilfs_sb_check_sum(sbp);

	return crc == le32_to_cpu(sbp->s_sum);
}

static int nilfs_sb2_offset_is_too_small(struct nilfs_super_block *sbp,
					 uint64_t sb2_offset)
{
	return sb2_offset < ((le64_to_cpu(sbp->s_nsegments) *
			      le32_to_cpu(sbp->s_blocks_per_segment)) <<
			     (le32_to_cpu(sbp->s_log_block_size) + 10));
}

static int __nilfs_sb_read(int devfd, struct nilfs_super_block **sbp,
			   uint64_t *offsets)
{
	uint64_t devsize, sb2_offset;
	int invalid_fs = 0;
	ssize_t ret;
	struct stat stat;

	sbp[0] = malloc(NILFS_MAX_SB_SIZE);
	sbp[1] = malloc(NILFS_MAX_SB_SIZE);
	if (unlikely(sbp[0] == NULL || sbp[1] == NULL))
		goto failed;

	ret = fstat(devfd, &stat);
	if (unlikely(ret < 0))
		goto failed;

	if (S_ISBLK(stat.st_mode)) {
		ret = ioctl(devfd, BLKGETSIZE64, &devsize);
		if (unlikely(ret < 0))
			goto failed;
	} else if (S_ISREG(stat.st_mode)) {
		devsize = stat.st_size;
	} else {
		errno = EBADF;
		goto failed;
	}

	ret = pread(devfd, sbp[0], NILFS_MAX_SB_SIZE, NILFS_SB_OFFSET_BYTES);
	if (likely(ret == NILFS_MAX_SB_SIZE)) {
		if (nilfs_sb_is_valid(sbp[0], 0))
			goto sb1_ok;
		invalid_fs = 1;
	}

	free(sbp[0]);
	sbp[0] = NULL;
sb1_ok:

	sb2_offset = NILFS_SB2_OFFSET_BYTES(devsize);
	if (offsets) {
		offsets[0] = NILFS_SB_OFFSET_BYTES;
		offsets[1] = sb2_offset;
	}

	ret = pread(devfd, sbp[1], NILFS_MAX_SB_SIZE, sb2_offset);
	if (likely(ret == NILFS_MAX_SB_SIZE)) {
		if (nilfs_sb_is_valid(sbp[1], 0) &&
		    !nilfs_sb2_offset_is_too_small(sbp[1], sb2_offset))
			goto sb2_ok;
		invalid_fs = 1;
	}

	free(sbp[1]);
	sbp[1] = NULL;
sb2_ok:

	if (unlikely(!sbp[0] && !sbp[1])) {
		if (invalid_fs)
			errno = EINVAL;
		goto failed;
	}

	return 0;

 failed:
	free(sbp[0]);  /* free(NULL) is just ignored */
	free(sbp[1]);
	return -1;
}

struct nilfs_super_block *nilfs_sb_read(int devfd)
{
	struct nilfs_super_block *sbp[2];
	int ret;

	ret = __nilfs_sb_read(devfd, sbp, NULL);
	if (unlikely(ret < 0))
		return NULL;

	if (!sbp[0]) {
		sbp[0] = sbp[1];
		sbp[1] = NULL;
	}

	free(sbp[1]);

	return sbp[0];
}

int nilfs_sb_write(int devfd, struct nilfs_super_block *sbp, int mask)
{
	uint64_t offsets[2];
	struct nilfs_super_block *sbps[2];
	ssize_t count;
	int i, ret;
	uint32_t crc;

	assert(devfd >= 0);

	if (unlikely(sbp == NULL)) {
		errno = EINVAL;
		return -1;
	}

	ret = __nilfs_sb_read(devfd, sbps, offsets);
	if (unlikely(ret < 0))
		return -1;

	ret = 0;
	for (i = 0; i < 2; i++) {
		if (!sbps[i])
			continue;

		if (mask & NILFS_SB_LABEL)
			memcpy(sbps[i]->s_volume_name,
			       sbp->s_volume_name, sizeof(sbp->s_volume_name));

		if (mask & NILFS_SB_COMMIT_INTERVAL)
			sbps[i]->s_c_interval = sbp->s_c_interval;

		if (mask & NILFS_SB_BLOCK_MAX)
			sbps[i]->s_c_block_max = sbp->s_c_block_max;

		if (mask & NILFS_SB_UUID)
			memcpy(sbps[i]->s_uuid, sbp->s_uuid,
			       sizeof(sbp->s_uuid));

		if (mask & NILFS_SB_FEATURES) {
			sbps[i]->s_feature_compat = sbp->s_feature_compat;
			sbps[i]->s_feature_compat_ro = sbp->s_feature_compat_ro;
			sbps[i]->s_feature_incompat = sbp->s_feature_incompat;
		}

		crc = nilfs_sb_check_sum(sbps[i]);
		sbps[i]->s_sum = cpu_to_le32(crc);
		count = pwrite(devfd, sbps[i], NILFS_MAX_SB_SIZE, offsets[i]);
		if (unlikely(count < NILFS_MAX_SB_SIZE)) {
			ret = -1;
			break;
		}
	}

	free(sbps[0]);
	free(sbps[1]);

	return ret;
}
