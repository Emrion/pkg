/*-
 * Copyright (c) 2011-2020 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * Copyright (c) 2014-2015 Matthew Seaman <matthew@FreeBSD.org>
 * Copyright (c) 2014 Vsevolod Stakhov <vsevolod@FreeBSD.org>
 * Copyright (c) 2023 Serenity Cyber Security, LLC
 *                    Author: Gleb Popov <arrowd@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/stat.h>

#include <errno.h>
#include <regex.h>
#include <fcntl.h>

#include <bsd_compat.h>

#include "pkg.h"
#include "private/event.h"
#include "private/pkg.h"
#include "private/pkg_abi.h"
#include "xmalloc.h"

#define TICK	100

static int load_metadata(struct pkg *pkg, const char *metadata, const char *plist,
    const char *rootdir);
static void fixup_abi(struct pkg *pkg, const char *rootdir, bool testing);
static void counter_init(const char *what, int64_t max);
static void counter_count(void);
static void counter_end(void);

extern struct pkg_ctx ctx;

static int
pkg_create_from_dir(struct pkg *pkg, const char *root,
    struct pkg_create *pc, struct packing *pkg_archive)
{
	char		 fpath[MAXPATHLEN];
	struct pkg_file	*file = NULL;
	struct pkg_dir	*dir = NULL;
	int		 ret;
	struct stat	 st;
	int64_t		 flatsize = 0;
	int64_t		 nfiles;
	const char	*relocation;
	char		*manifest;
	ucl_object_t	*obj;
	hardlinks_t	 hardlinks = vec_init();

	if (pkg_is_valid(pkg) != EPKG_OK) {
		pkg_emit_error("the package is not valid");
		return (EPKG_FATAL);
	}

	relocation = pkg_kv_get(&pkg->annotations, "relocated");
	if (relocation == NULL)
		relocation = "";
	if (ctx.pkg_rootdir != NULL)
		relocation = ctx.pkg_rootdir;

	/*
	 * Get / compute size / checksum if not provided in the manifest
	 */

	nfiles = pkghash_count(pkg->filehash);
	counter_init("file sizes/checksums", nfiles);

	while (pkg_files(pkg, &file) == EPKG_OK) {

		snprintf(fpath, sizeof(fpath), "%s%s%s", root ? root : "",
		    relocation, file->path);

		if (lstat(fpath, &st) == -1) {
			pkg_emit_error("file '%s' is missing", fpath);
			vec_free_and_free(&hardlinks, free);
			return (EPKG_FATAL);
		}

		if (!(S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))) {
			pkg_emit_error("file '%s' is not a regular file or symlink", fpath);
			vec_free_and_free(&hardlinks, free);
			return (EPKG_FATAL);
		}

		if (file->size == 0)
			file->size = (int64_t)st.st_size;

		if (st.st_nlink == 1 || !check_for_hardlink(&hardlinks, &st)) {
			flatsize += file->size;
		}

		free(file->sum);
		file->sum = pkg_checksum_generate_file(fpath,
		    PKG_HASH_TYPE_SHA256_HEX);
		if (file->sum == NULL) {
			vec_free_and_free(&hardlinks, free);
			return (EPKG_FATAL);
		}

		counter_count();
	}
	vec_free_and_free(&hardlinks, free);

	counter_end();

	pkg->flatsize = flatsize;

	if (pkg->type == PKG_OLD_FILE) {
		pkg_emit_error("Cannot create an old format package");
		return (EPKG_FATAL);
	}

	obj = pkg_emit_object(pkg, PKG_MANIFEST_EMIT_COMPACT);
	manifest = ucl_object_emit(obj, UCL_EMIT_JSON_COMPACT);
	ucl_object_unref(obj);
	packing_append_buffer(pkg_archive, manifest, "+COMPACT_MANIFEST", strlen(manifest));
	free(manifest);
	obj = pkg_emit_object(pkg, 0);
	if (pc->expand_manifest) {
		manifest = ucl_object_emit(obj, UCL_EMIT_CONFIG);
	} else {
		manifest = ucl_object_emit(obj, UCL_EMIT_JSON_COMPACT);
	}
	ucl_object_unref(obj);
	packing_append_buffer(pkg_archive, manifest, "+MANIFEST", strlen(manifest));
	free(manifest);

	counter_init("packing files", nfiles);

	while (pkg_files(pkg, &file) == EPKG_OK) {
		char dpath[MAXPATHLEN];
		const char *dp = file->path;

		if (pkg->oprefix != NULL) {
			size_t l = strlen(pkg->prefix);
			if (strncmp(file->path, pkg->prefix, l) == 0 &&
			    (file->path[l] == '/' || l == 1)) {
				snprintf(dpath, sizeof(dpath), "%s%s%s",
				    pkg->oprefix, l == 1 ? "/" : "", file->path + l);
				dp = dpath;
			}
		}

		snprintf(fpath, sizeof(fpath), "%s%s%s", root ? root : "",
		    relocation, file->path);

		ret = packing_append_file_attr(pkg_archive, fpath, dp,
		    file->uname, file->gname, file->perm, file->fflags);
		if (ctx.developer_mode && ret != EPKG_OK)
			return (ret);
		counter_count();
	}

	counter_end();

	nfiles = pkghash_count(pkg->dirhash);
	counter_init("packing directories", nfiles);

	while (pkg_dirs(pkg, &dir) == EPKG_OK) {
		snprintf(fpath, sizeof(fpath), "%s%s%s", root ? root : "",
		    relocation, dir->path);

		ret = packing_append_file_attr(pkg_archive, fpath, dir->path,
		    dir->uname, dir->gname, dir->perm, dir->fflags);
		if (ctx.developer_mode && ret != EPKG_OK)
			return (ret);
		counter_count();
	}

	counter_end();

	return (EPKG_OK);
}

static struct packing *
pkg_create_archive(struct pkg *pkg, struct pkg_create *pc, unsigned required_flags)
{
	char		*pkg_path = NULL;
	struct packing	*pkg_archive = NULL;

	/*
	 * Ensure that we have all the information we need
	 */
	if (pkg->type != PKG_OLD_FILE)
		assert((pkg->flags & required_flags) == required_flags);

	if (pkg_mkdirs(pc->outdir) != EPKG_OK)
		return NULL;

	if (pkg_asprintf(&pkg_path, "%S/%n-%v", pc->outdir, pkg, pkg) == -1) {
		pkg_emit_errno("pkg_asprintf", "");
		return (NULL);
	}

	if (packing_init(&pkg_archive, pkg_path, pc->format,
	    pc->compression_level, pc->compression_threads, pc->timestamp, pc->overwrite, false) != EPKG_OK) {
		pkg_archive = NULL;
	}

	free(pkg_path);

	return pkg_archive;
}

static const char * const scripts[] = {
	"+INSTALL",
	"+PRE_INSTALL",
	"+POST_INSTALL",
	"+POST_INSTALL",
	"+DEINSTALL",
	"+PRE_DEINSTALL",
	"+POST_DEINSTALL",
	"pkg-install",
	"pkg-pre-install",
	"pkg-post-install",
	"pkg-deinstall",
	"pkg-pre-deinstall",
	"pkg-post-deinstall",
	NULL
};

static const char * const lua_scripts[] = {
	"pkg-pre-install.lua",
	"pkg-post-install.lua",
	"pkg-pre-deinstall.lua",
	"pkg-post-deinstall.lua",
	NULL
};

struct pkg_create *
pkg_create_new(void)
{
	struct pkg_create *pc;

	pc = xcalloc(1, sizeof(*pc));
	pc->format = packing_format_from_string(ctx.compression_format);
	pc->compression_level = ctx.compression_level;
	pc->compression_threads = ctx.compression_threads;
	pc->timestamp = (time_t) -1;
	pc->overwrite = true;
	pc->expand_manifest = false;

	return (pc);
}

void
pkg_create_free(struct pkg_create *pc)
{
	free(pc);
}

bool
pkg_create_set_format(struct pkg_create *pc, const char *format)
{
	if (STREQ(format, "tzst"))
		pc->format = TZS;
	else if (STREQ(format, "txz"))
		pc->format = TXZ;
	else if (STREQ(format, "tbz"))
		pc->format = TBZ;
	else if (STREQ(format, "tgz"))
		pc->format = TGZ;
	else if (STREQ(format, "tar"))
		pc->format = TAR;
	else
		return (false);
	return (true);
}

void
pkg_create_set_compression_level(struct pkg_create *pc, int clevel)
{
	pc->compression_level = clevel;
}

void
pkg_create_set_compression_threads(struct pkg_create *pc, int threads)
{
	pc->compression_threads = threads;
}

void
pkg_create_set_expand_manifest(struct pkg_create *pc, bool expand)
{
	pc->expand_manifest = expand;
}

void
pkg_create_set_rootdir(struct pkg_create *pc, const char *rootdir)
{
	pc->rootdir = rootdir;
}

void
pkg_create_set_output_dir(struct pkg_create *pc, const char *outdir)
{
	pc->outdir = outdir;
}

void
pkg_create_set_timestamp(struct pkg_create *pc, time_t timestamp)
{
	pc->timestamp = timestamp;
}

void
pkg_create_set_overwrite(struct pkg_create *pc, bool overwrite)
{
	pc->overwrite = overwrite;
}

static int
hash_file(struct pkg *pkg)
{
	char hash_dest[MAXPATHLEN];
	char filename[MAXPATHLEN];

	/* Find the hash and rename the file and create a symlink */
	pkg_snprintf(filename, sizeof(filename), "%n-%v.pkg",
			pkg, pkg);
	pkg->sum = pkg_checksum_file(filename,
			PKG_HASH_TYPE_SHA256_HEX);
	pkg_snprintf(hash_dest, sizeof(hash_dest), "%n-%v-%z.pkg",
			pkg, pkg, pkg);

	pkg_debug(1, "Rename the pkg file from: %s to: %s",
			filename, hash_dest);
	if (rename(filename, hash_dest) == -1) {
		pkg_emit_errno("rename", hash_dest);
		unlink(hash_dest);
		return (EPKG_FATAL);
	}
	if (symlink(hash_dest, filename) == -1) {
		pkg_emit_errno("symlink", hash_dest);
		return (EPKG_FATAL);
	}
	return (EPKG_OK);
}

int
pkg_create_i(struct pkg_create *pc, struct pkg *pkg, bool hash)
{
	struct packing *pkg_archive = NULL;
	int ret;

	unsigned required_flags = PKG_LOAD_DEPS | PKG_LOAD_FILES |
		PKG_LOAD_CATEGORIES | PKG_LOAD_DIRS | PKG_LOAD_SCRIPTS |
		PKG_LOAD_OPTIONS | PKG_LOAD_LICENSES | PKG_LOAD_LUA_SCRIPTS;

	assert(pkg->type == PKG_INSTALLED || pkg->type == PKG_OLD_FILE);

	pkg_archive = pkg_create_archive(pkg, pc, required_flags);
	if (pkg_archive == NULL) {
		if (errno == EEXIST)
			return (EPKG_EXIST);
		pkg_emit_error("unable to create archive");
		return (EPKG_FATAL);
	}

	if ((ret = pkg_create_from_dir(pkg, NULL, pc, pkg_archive)) != EPKG_OK) {
		pkg_emit_error("package creation failed");
	}
	packing_finish(pkg_archive);

	if (hash && ret == EPKG_OK)
		ret = hash_file(pkg);

	return (ret);
}

int
pkg_create(struct pkg_create *pc, const char *metadata, const char *plist,
    bool hash)
{
	struct pkg *pkg = NULL;
	struct packing *pkg_archive = NULL;
	int ret = ENOMEM;

	pkg_debug(1, "Creating package");
	if (pkg_new(&pkg, PKG_FILE) != EPKG_OK) {
		return (EPKG_FATAL);
	}

	if (load_metadata(pkg, metadata, plist, pc->rootdir) != EPKG_OK) {
		pkg_free(pkg);
		return (EPKG_FATAL);
	}
	fixup_abi(pkg, pc->rootdir, false);

	pkg_archive = pkg_create_archive(pkg, pc, 0);
	if (pkg_archive == NULL) {
		if (errno == EEXIST) {
			pkg_emit_notice("%s-%s already packaged, skipping...\n",
			    pkg->name, pkg->version);
			pkg_free(pkg);
			return (EPKG_EXIST);
		}
		pkg_free(pkg);
		return (EPKG_FATAL);
	}

	if ((ret = pkg_create_from_dir(pkg, pc->rootdir, pc, pkg_archive)) != EPKG_OK)
		pkg_emit_error("package creation failed");

	packing_finish(pkg_archive);
	if (hash && ret == EPKG_OK)
		ret = hash_file(pkg);

	pkg_free(pkg);
	return (ret);
}

static int
pkg_load_message_from_file(int fd, struct pkg *pkg, const char *path)
{
	char *buf = NULL;
	off_t size = 0;
	int ret;
	ucl_object_t *obj;

	assert(pkg != NULL);
	assert(path != NULL);

	if (faccessat(fd, path, F_OK, 0) == -1) {
		return (EPKG_FATAL);
	}

	pkg_debug(1, "Reading message: '%s'", path);
	if ((ret = file_to_bufferat(fd, path, &buf, &size)) != EPKG_OK) {
		return (ret);
	}

	if (*buf == '[') {
		ret = pkg_message_from_str(pkg, buf, size);
		free(buf);
		return (ret);
	}
	obj = ucl_object_fromstring_common(buf, size,
	    UCL_STRING_RAW|UCL_STRING_TRIM);
	ret = pkg_message_from_ucl(pkg, obj);
	ucl_object_unref(obj);
	free(buf);

	return (ret);
}

/* TODO use file descriptor for rootdir */
static int
load_manifest(struct pkg *pkg, const char *metadata, const char *plist,
    const char *rootdir)
{
	int ret;

	ret = pkg_parse_manifest_file(pkg, metadata);

	if (ret == EPKG_OK && plist != NULL)
		ret = ports_parse_plist(pkg, plist, rootdir);
	return (ret);
}

/* TODO use file descriptor for rootdir */
static int
load_metadata(struct pkg *pkg, const char *metadata, const char *plist,
    const char *rootdir)
{
	regex_t preg;
	regmatch_t pmatch[2];
	size_t size;
	int fd, i;

	/* Let's see if we have a directory or a manifest */
	if ((fd = open(metadata, O_DIRECTORY|O_CLOEXEC)) == -1) {
		if (errno == ENOTDIR)
			return (load_manifest(pkg, metadata, plist, rootdir));
		pkg_emit_errno("open", metadata);
		return (EPKG_FATAL);
	}

	if ((pkg_parse_manifest_fileat(fd, pkg, "+MANIFEST")) != EPKG_OK) {
		pkg_emit_error("Error parsing %s/+MANIFEST", metadata);
		close(fd);
		return (EPKG_FATAL);
	}
	/* ensure the uid is properly */
	free(pkg->uid);
	pkg->uid = xstrdup(pkg->name);

	pkg_load_message_from_file(fd, pkg, "+DISPLAY");
	if (pkg->desc == NULL)
		pkg_set_from_fileat(fd, pkg, PKG_ATTR_DESC, "+DESC", false);

	for (i = 0; scripts[i] != NULL; i++) {
		if (faccessat(fd, scripts[i], F_OK, 0) == 0)
			pkg_addscript_fileat(fd, pkg, scripts[i]);
	}

	for (i = 0; lua_scripts[i] != NULL; i++) {
		if (faccessat(fd, lua_scripts[i], F_OK, 0) == 0)
			pkg_addluascript_fileat(fd, pkg, lua_scripts[i]);
	}

	if (plist != NULL && ports_parse_plist(pkg, plist, rootdir) != EPKG_OK) {
		return (EPKG_FATAL);
	}
	close(fd);

	if (pkg->www == NULL) {
		if (pkg->desc == NULL) {
			pkg_emit_error("No www or desc defined in manifest");
			return (EPKG_FATAL);
		}
		regcomp(&preg, "^WWW:[[:space:]]*(.*)$",
		    REG_EXTENDED|REG_ICASE|REG_NEWLINE);
		if (regexec(&preg, pkg->desc, 2, pmatch, 0) == 0) {
			size = pmatch[1].rm_eo - pmatch[1].rm_so;
			pkg->www = xstrndup(&pkg->desc[pmatch[1].rm_so], size);
		} else {
			pkg->www = xstrdup("UNKNOWN");
		}
		regfree(&preg);
	}

	return (EPKG_OK);
}

static void
fixup_abi(struct pkg *pkg, const char *rootdir, bool testing)
{
	bool defaultarch = false;

	/* if no arch autodetermine it */
	if (pkg->abi == NULL) {
		if (ctx.abi.os == PKG_OS_FREEBSD) {
			char *str_osversion;
			xasprintf(&str_osversion, "%d", pkg_abi_get_freebsd_osversion(&ctx.abi));
			pkg_kv_add(&pkg->annotations, "FreeBSD_version", str_osversion, "annotation");
		}
		pkg->abi = pkg_abi_to_string(&ctx.abi);
		defaultarch = true;
	}

	if (!testing)
		pkg_analyse_files(NULL, pkg, rootdir);

	if (ctx.developer_mode)
		suggest_arch(pkg, defaultarch);
}

int
pkg_load_metadata(struct pkg *pkg, const char *mfile, const char *md_dir,
    const char *plist, const char *rootdir, bool testing)
{
	int ret;

	ret = load_metadata(pkg, md_dir != NULL ? md_dir: mfile, plist, rootdir);
	if (ret != EPKG_OK)
		return (ret);

	fixup_abi(pkg, rootdir, testing);
	return (ret);
}

static int64_t	count;
static int64_t  maxcount;
static const char *what;

static int magnitude(int64_t num)
{
	int oom;

	if (num == 0)
		return (1);
	if (num < 0)
		num = -num;

	for (oom = 1; num >= 10; oom++)
		num /= 10;

	return (oom);
}

static void
counter_init(const char *count_what, int64_t max)
{
	count = 0;
	what = count_what;
	maxcount = max;
	pkg_emit_progress_start("%-20s%*s[%jd]", what,
	    6 - magnitude(maxcount), " ", (intmax_t)maxcount);

	return;
}

static void
counter_count(void)
{
	count++;

	if (count % TICK == 0)
		pkg_emit_progress_tick(count, maxcount);

	return;
}

static void
counter_end(void)
{
	pkg_emit_progress_tick(count, maxcount);
	return;
}
