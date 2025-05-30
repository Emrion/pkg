/*-
 * Copyright (c) 2011-2014 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * Copyright (c) 2011 Will Andrews <will@FreeBSD.org>
 * Copyright (c) 2011 Philippe Pepiot <phil@philpep.org>
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

#ifdef HAVE_CONFIG_H
#include "pkg_config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

#include <bsd_compat.h>

#include "pkg.h"
#include "private/event.h"
#include "private/pkg.h"
#include "private/pkgdb.h"
#include "private/utils.h"

#if defined(UF_NOUNLINK)
#define NOCHANGESFLAGS	(UF_IMMUTABLE | UF_APPEND | UF_NOUNLINK | SF_IMMUTABLE | SF_APPEND | SF_NOUNLINK)
#else
#define NOCHANGESFLAGS	(UF_IMMUTABLE | UF_APPEND | SF_IMMUTABLE | SF_APPEND)
#endif

int
pkg_delete(struct pkg *pkg, struct pkg *rpkg, struct pkgdb *db, int flags,
    struct triggers *t)
{
	xstring		*message = NULL;
	int		 ret, cancel = 0;
	bool		 handle_rc = false;
	const unsigned load_flags = PKG_LOAD_RDEPS|PKG_LOAD_FILES|PKG_LOAD_DIRS|
					PKG_LOAD_SCRIPTS|PKG_LOAD_ANNOTATIONS|PKG_LOAD_LUA_SCRIPTS;

	assert(pkg != NULL);
	assert(db != NULL);

	if (pkgdb_ensure_loaded(db, pkg, load_flags) != EPKG_OK)
		return (EPKG_FATAL);
	if (rpkg != NULL && pkgdb_ensure_loaded(db, rpkg, load_flags) != EPKG_OK)
		return (EPKG_FATAL);

	pkg_emit_deinstall_begin(pkg);

	/* If the package is locked */
	if (pkg->locked) {
		pkg_emit_locked(pkg);
		return (EPKG_LOCKED);
	}

	/*
	 * stop the different related services if the users do want that
	 * and that the service is running
	 */
	handle_rc = pkg_object_bool(pkg_config_get("HANDLE_RC_SCRIPTS"));
	if (handle_rc)
		pkg_start_stop_rc_scripts(pkg, PKG_RC_STOP);

	if ((flags & (PKG_DELETE_NOSCRIPT | PKG_DELETE_UPGRADE)) == 0) {
		bool noexec = ((flags & PKG_DELETE_NOEXEC) == PKG_DELETE_NOEXEC);
		pkg_open_root_fd(pkg);
		ret = pkg_lua_script_run(pkg, PKG_LUA_PRE_DEINSTALL, false);
		if (ret != EPKG_OK && ctx.developer_mode)
			return (ret);
		ret = pkg_script_run(pkg, PKG_SCRIPT_PRE_DEINSTALL, false, noexec);
		if (ret != EPKG_OK && (ctx.developer_mode || noexec))
			return (ret);
	}

	ret = pkg_delete_files(pkg, rpkg, flags, t);
	if (ret == EPKG_CANCEL)
		cancel = 1;
	else if (ret != EPKG_OK)
		return (ret);

	if ((flags & (PKG_DELETE_NOSCRIPT | PKG_DELETE_UPGRADE)) == 0) {
		bool noexec = ((flags & PKG_DELETE_NOEXEC) == PKG_DELETE_NOEXEC);
		pkg_lua_script_run(pkg, PKG_LUA_POST_DEINSTALL, false);
		ret = pkg_script_run(pkg, PKG_SCRIPT_POST_DEINSTALL, false, noexec);
		if (ret != EPKG_OK && (ctx.developer_mode || noexec))
			return (ret);
	}

	ret = pkg_delete_dirs(db, pkg, NULL);
	if (ret != EPKG_OK)
		return (ret);

	if ((flags & PKG_DELETE_UPGRADE) == 0) {
		pkg_emit_deinstall_finished(pkg);
		vec_foreach(pkg->message, i) {
			if (pkg->message.d[i]->type == PKG_MESSAGE_REMOVE) {
				if (message == NULL) {
					message = xstring_new();
					pkg_fprintf(message->fp, "Message from "
					    "%n-%v:\n", pkg, pkg);
				}
				fprintf(message->fp, "%s\n", pkg->message.d[i]->str);
			}
		}
		if (pkg_has_message(pkg) && message != NULL) {
			fflush(message->fp);
			pkg_emit_message(message->buf);
			xstring_free(message);
		}
	}

	ret = pkgdb_unregister_pkg(db, pkg->id);
	if (ret != EPKG_OK)
		return ret;

	return (cancel ? EPKG_CANCEL : ret);
}

void
pkg_add_dir_to_del(struct pkg *pkg, const char *file, const char *dir)
{
	char path[MAXPATHLEN];
	char *tmp;
	size_t len, len2;

	strlcpy(path, file != NULL ? file : dir, MAXPATHLEN);

	if (file != NULL) {
		tmp = strrchr(path, '/');
		tmp[1] = '\0';
	}

	len = strlen(path);

	/* make sure to finish by a / */
	if (len > 0 && path[len - 1] != '/' && len < MAXPATHLEN) {
		path[len] = '/';
		len++;
		path[len] = '\0';
	}

	vec_foreach(pkg->dir_to_del, i) {
		len2 = strlen(pkg->dir_to_del.d[i]);
		if (len2 >= len && strncmp(path, pkg->dir_to_del.d[i], len) == 0)
			return;

		if (strncmp(path, pkg->dir_to_del.d[i], len2) == 0) {
			pkg_debug(1, "Replacing in deletion %s with %s",
			    pkg->dir_to_del.d[i], path);
			free(pkg->dir_to_del.d[i]);
			pkg->dir_to_del.d[i] = xstrdup(path);
			return;
		}
	}

	pkg_debug(1, "Adding to deletion %s", path);
	vec_push(&pkg->dir_to_del, xstrdup(path));
}

static void
rmdir_p(struct pkgdb *db, struct pkg *pkg, char *dir, const char *prefix_r)
{
	char *tmp;
	int64_t cnt;
	char fullpath[MAXPATHLEN];
	size_t len;
#ifdef HAVE_STRUCT_STAT_ST_FLAGS
	struct stat st;
#if !defined(HAVE_CHFLAGSAT)
	int fd;
#endif
#endif

	len = snprintf(fullpath, sizeof(fullpath), "/%s", dir);
	while (fullpath[len -1] == '/') {
		fullpath[len - 1] = '\0';
		len--;
	}
	if (pkgdb_is_dir_used(db, pkg, fullpath, &cnt) != EPKG_OK)
		return;

	pkg_debug(1, "Number of packages owning the directory '%s': %d",
	    fullpath, (int)cnt);
	/*
	 * At this moment the package we are removing have already been removed
	 * from the local database so if anything else is owning the directory
	 * that is another package meaning only remove the diretory is cnt == 0
	 */
	if (cnt > 0)
		return;

	if (STREQ(prefix_r, fullpath + 1))
		return;

	pkg_debug(1, "removing directory %s", fullpath);
#ifdef HAVE_STRUCT_STAT_ST_FLAGS
	if (fstatat(pkg->rootfd, dir, &st, AT_SYMLINK_NOFOLLOW) != -1) {
		if (st.st_flags & NOCHANGESFLAGS) {
#ifdef HAVE_CHFLAGSAT
			/* Disable all flags*/
			chflagsat(pkg->rootfd, dir, 0, AT_SYMLINK_NOFOLLOW);
#else
			fd = openat(pkg->rootfd, dir, O_NOFOLLOW);
			if (fd > 0) {
				fchflags(fd, 0);
				close(fd);
			}
#endif
		}
	}
#endif

	if (unlinkat(pkg->rootfd, dir, AT_REMOVEDIR) == -1) {
		if (errno != ENOTEMPTY && errno != EBUSY)
			pkg_emit_errno("unlinkat", dir);
		/* If the directory was already removed by a bogus script, continue removing parents */
		if (errno != ENOENT)
			return;
	}

	/* No recursivity for packages out of the prefix */
	if (strncmp(prefix_r, dir, strlen(prefix_r)) != 0)
		return;

	/* remove the trailing '/' */
	tmp = strrchr(dir, '/');
	if (tmp == NULL)
		return;
	if (tmp == dir)
		return;

	tmp[0] = '\0';
	tmp = strrchr(dir, '/');
	if (tmp == NULL)
		return;

	tmp[1] = '\0';

	rmdir_p(db, pkg, dir, prefix_r);
}

static void
pkg_effective_rmdir(struct pkgdb *db, struct pkg *pkg)
{
	char prefix_r[MAXPATHLEN];

	snprintf(prefix_r, sizeof(prefix_r), "%s", pkg->prefix[0] ? pkg->prefix + 1 : "");
	vec_foreach(pkg->dir_to_del, i) {
		rmdir_p(db, pkg, pkg->dir_to_del.d[i], prefix_r);
	}
}

void
pkg_delete_file(struct pkg *pkg, struct pkg_file *file)
{
	const char *path;
	const char *prefix_rel;
	size_t len;
#ifdef HAVE_STRUCT_STAT_ST_FLAGS
	struct stat st;
#if !defined(HAVE_CHFLAGSAT)
	int fd;
#endif
#endif
	pkg_open_root_fd(pkg);

	path = file->path;
	path++;

	prefix_rel = pkg->prefix;
	prefix_rel++;
	len = strlen(prefix_rel);
	while (len > 0 && prefix_rel[len - 1] == '/')
		len--;

#ifdef HAVE_STRUCT_STAT_ST_FLAGS
	if (fstatat(pkg->rootfd, path, &st, AT_SYMLINK_NOFOLLOW) != -1) {
		if (st.st_flags & NOCHANGESFLAGS) {
#ifdef HAVE_CHFLAGSAT
			chflagsat(pkg->rootfd, path,
			    st.st_flags & ~NOCHANGESFLAGS,
			    AT_SYMLINK_NOFOLLOW);
#else
			fd = openat(pkg->rootfd, path, O_NOFOLLOW);
			if (fd > 0) {
				fchflags(fd, st.st_flags & ~NOCHANGESFLAGS);
				close(fd);
			}
#endif
		}
	}
#endif
	pkg_debug(1, "Deleting file: '%s'", path);
	if (unlinkat(pkg->rootfd, path, 0) == -1) {
		if (errno == ENOENT)
			pkg_emit_file_missing(pkg, file);
		else
			pkg_emit_errno("unlinkat", path);
		return;
	}

	/* do not bother about directories not in prefix */
	if ((strncmp(prefix_rel, path, len) == 0) && path[len] == '/')
		pkg_add_dir_to_del(pkg, path, NULL);
}

/*
 * Handle a special case: the package is to be upgraded but is being deleted
 * temporarily to handle a file path conflict.  In this situation we shouldn't
 * remove configuration files.  For now, keep them if the replacement package
 * contains a configuration file at the same path.
 *
 * Note, this currently doesn't handle the case where a configuration file
 * participates in the conflict, i.e., it moves from one package to another.
 */
static bool
pkg_delete_skip_config(struct pkg *pkg, struct pkg *rpkg, struct pkg_file *file,
    int flags)
{
	if ((flags & PKG_DELETE_UPGRADE) == 0)
		return (false);
	if (pkghash_get(pkg->config_files_hash, file->path) == NULL)
		return (false);
	if (pkghash_get(rpkg->config_files_hash, file->path) == NULL)
		return (false);
	return (true);
}

int
pkg_delete_files(struct pkg *pkg, struct pkg *rpkg, int flags,
    struct triggers *t)
{
	struct pkg_file	*file = NULL;

	int		nfiles, cur_file = 0;
	int		retcode = EPKG_OK;

	nfiles = pkghash_count(pkg->filehash);
	if (nfiles == 0)
		return (EPKG_OK);

	pkg_emit_delete_files_begin(pkg);
	pkg_emit_progress_start(NULL);

	while (pkg_files(pkg, &file) == EPKG_OK) {
		if (pkg_delete_skip_config(pkg, rpkg, file, flags))
			continue;
		append_touched_file(file->path);
		if (pkg_emit_progress_tick(cur_file++, nfiles))
			retcode = EPKG_CANCEL;
		trigger_is_it_a_cleanup(t, file->path);
		pkg_delete_file(pkg, file);
	}

	pkg_emit_progress_tick(nfiles, nfiles);
	pkg_emit_delete_files_finished(pkg);

	return (retcode);
}

void
pkg_delete_dir(struct pkg *pkg, struct pkg_dir *dir)
{
	const char *path;
	const char *prefix_rel;
	size_t len;

	pkg_open_root_fd(pkg);

	path = dir->path;
	/* remove the first / */
	path++;

	prefix_rel = pkg->prefix;
	prefix_rel++;
	len = strlen(prefix_rel);
	while (len > 0 && prefix_rel[len - 1] == '/')
		len--;

	if ((strncmp(prefix_rel, path, len) == 0) && path[len] == '/') {
		pkg_add_dir_to_del(pkg, NULL, path);
	} else {
		vec_push(&pkg->dir_to_del, xstrdup(path));
	}
}

int
pkg_delete_dirs(__unused struct pkgdb *db, struct pkg *pkg, struct pkg *new)
{
	struct pkg_dir	*dir = NULL;

	while (pkg_dirs(pkg, &dir) == EPKG_OK) {
		if (new != NULL && !pkg_has_dir(new, dir->path))
			continue;
		pkg_delete_dir(pkg, dir);
	}

	pkg_effective_rmdir(db, pkg);

	return (EPKG_OK);
}
