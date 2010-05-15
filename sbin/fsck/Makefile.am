## Makefile.am

AM_CFLAGS = -Wall
AM_CPPFLAGS = -I$(top_srcdir)/include
LDADD = -luuid $(top_builddir)/lib/libnilfsfeature.la \
	$(top_builddir)/lib/libmountchk.la \
	$(top_builddir)/lib/libcrc32.la

sbin_PROGRAMS = fsck0.nilfs2

fsck0_nilfs2_SOURCES = fsck0.nilfs2.c ../mkfs/mkfs.h
