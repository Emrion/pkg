/*-
 * Copyright (c) 2011-2012 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2013 Matthew Seaman <matthew@FreeBSD.org>
 * Copyright (c) 2012-2013 Bryan Drewery <bdrewery@FreeBSD.org>
 * Copyright (c) 2016 Vsevolod Stakhov <vsevolod@FreeBSD.org>
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

#include "pkg_config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <err.h>
#include <getopt.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <xstring.h>
#include <pkghash.h>
#include <pkg.h>

#ifdef HAVE_SYS_CAPSICUM_H
#include <sys/capsicum.h>
#endif

#ifdef HAVE_CAPSICUM
#include <sys/capsicum.h>
#endif
#include "pkgcli.h"
#include <pkg/audit.h>

static const char vuln_end_lit[] = "**END**";

void
usage_upgrade(void)
{
	fprintf(stderr, "Usage: pkg upgrade [-fInFqUy] [-r reponame] [-Cgix] <pkg-name> ...\n\n");
	fprintf(stderr, "For more information see 'pkg help upgrade'.\n");
}

static void
add_to_check(pkghash *check, struct pkg *pkg)
{
	const char *uid = NULL;

	pkg_get(pkg, PKG_ATTR_UNIQUEID, &uid);
	pkghash_safe_add(check, uid, pkg, NULL);
}

static void
check_vulnerable(struct pkg_audit *audit, struct pkgdb *db, int sock)
{
	struct pkg_audit_issues	*issues;
	struct pkgdb_it	*it = NULL;
	struct pkg		*pkg = NULL;
	pkghash			*check = NULL;
	pkghash_it		hit;
	const char		*uid;
	FILE			*out;

	out = fdopen(sock, "w");
	if (out == NULL) {
		warn("unable to open stream");
		return;
	}

	if ((it = pkgdb_query(db, NULL, MATCH_ALL)) == NULL) {
		warnx("Error accessing the package database");
		pkg_audit_free(audit);
		fclose(out);
		return;
	}
	check = pkghash_new();

	while (pkgdb_it_next(it, &pkg, PKG_LOAD_BASIC|PKG_LOAD_RDEPS) == EPKG_OK) {
		if (pkg_type(pkg) == PKG_INSTALLED) {
			add_to_check(check, pkg);
			pkg = NULL;
		}
	}

	pkgdb_it_free(it);
	pkgdb_close(db);

	if (check == NULL)
	        goto out_cleanup;

	if (pkg_audit_load(audit, NULL) != EPKG_OK) {
		warn("unable to open vulnxml file");
	        goto out_cleanup;
	}

	pkg_drop_privileges();

#ifdef HAVE_CAPSICUM
#ifndef PKG_COVERAGE
	if (cap_enter() < 0 && errno != ENOSYS) {
		warn("cap_enter() failed");
		goto out_cleanup;
	}
#endif
#endif

	if (pkg_audit_process(audit) == EPKG_OK) {
		hit = pkghash_iterator(check);
		while (pkghash_next(&hit)) {
				issues = NULL;
				pkg = (struct pkg *)hit.value;
				if (pkg_audit_is_vulnerable(audit, pkg, &issues, true)) {
					pkg_get(pkg, PKG_ATTR_UNIQUEID, &uid);
					fprintf(out, "%s\n", uid);
					fflush(out);
				}
				pkg_audit_issues_free(issues);
				pkg_free(pkg);
		}

		fprintf(out, "%s\n", vuln_end_lit);
		fflush(out);
	} else {
		warnx("cannot process vulnxml");
	}

out_cleanup:
	pkg_audit_free(audit);
	pkghash_destroy(check);
	fclose(out);
}

static int
add_vulnerable_upgrades(struct pkg_jobs	*jobs, struct pkgdb *db)
{
	int 				sp[2], retcode, ret = EPKG_FATAL;
	pid_t 				cld;
	FILE				*in;
	struct pkg_audit	*audit;
	char				*line = NULL;
	size_t				linecap = 0;
	ssize_t				linelen;

	/* Fetch audit file */
	if (pkg_audit_fetch(NULL, NULL) != EPKG_OK)
		return (EXIT_FAILURE);

	/* Create socketpair to execute audit check in a detached mode */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == -1)  {
		warnx("Cannot create socketpair");

		return (EPKG_FATAL);
	}

	audit = pkg_audit_new();
	cld = fork();

	switch (cld) {
	case 0:
		close(sp[1]);
		check_vulnerable(audit, db, sp[0]);
		close(sp[0]);
		_exit(EXIT_SUCCESS);
		break;
	case -1:
		warnx("Cannot fork");
	        pkg_audit_free(audit);
		return (EPKG_FATAL);
	default:
		/* Parent code */
		close(sp[0]);
		pkg_audit_free(audit);
		in = fdopen(sp[1], "r");

		if (in == NULL) {
			warnx("Cannot create stream");
			close(sp[1]);

			return (EPKG_FATAL);
		}
		break;
	}

	while ((linelen = getline(&line, &linecap, in)) > 0) {
		if (line[linelen - 1] == '\n') {
			line[linelen - 1] = '\0';
		}

		if (STREQ(line, vuln_end_lit)) {
			ret = EPKG_OK;
			break;
		}

		if (pkg_jobs_add(jobs, MATCH_EXACT, &line, 1) == EPKG_FATAL) {
			warnx("Cannot update %s which is vulnerable", line);
			/* TODO: assume it non-fatal for now */
		}
	}

	free(line);

	fclose(in);

	while (waitpid(cld, &retcode, 0) == -1) {
		if (errno != EINTR) {
			warnx("Cannot wait");
			return (EPKG_FATAL);
		}
	}

	if (ret != EPKG_OK) {
		warn("Cannot get the complete list of vulnerable packages");
	}

	return (ret);
}

int
exec_upgrade(int argc, char **argv)
{
	struct pkgdb	*db = NULL;
	struct pkg_jobs	*jobs = NULL;
	int		 retcode;
	int		 updcode;
	int		 ch;
	int		 lock_type = PKGDB_LOCK_ADVISORY;
	match_t		 match = MATCH_EXACT;
	int		 done = 0;
	int		 nbactions = 0;
	int		 scriptnoexec = 0;
	bool	rc = true;
	pkg_flags	 f = PKG_FLAG_NONE | PKG_FLAG_PKG_VERSION_TEST;
	c_charv_t	reponames = vec_init();

	struct option longopts[] = {
		{ "case-sensitive",	no_argument,		NULL,	'C' },
		{ "force",		no_argument,		NULL,	'f' },
		{ "fetch-only",		no_argument,		NULL,	'F' },
		{ "glob",		no_argument,		NULL,	'g' },
		{ "case-insensitive",	no_argument,		NULL,	'i' },
		{ "no-scripts",		no_argument,		NULL,	'I' },
		{ "script-no-exec",	no_argument,		&scriptnoexec,	1 },
		{ "dry-run",		no_argument,		NULL,	'n' },
		{ "quiet",		no_argument,		NULL,	'q' },
		{ "repository",		required_argument,	NULL,	'r' },
		{ "no-repo-update",	no_argument,		NULL,	'U' },
		{ "regex",		no_argument,		NULL,	'x' },
		{ "yes",		no_argument,		NULL,	'y' },
		{ "vulnerable",		no_argument,		NULL,		'v' },
		{ NULL,			0,			NULL,	0   },
	};

	while ((ch = getopt_long(argc, argv, "+CfFgiInqr:Uxyv", longopts, NULL)) != -1) {
		switch (ch) {
		case 'C':
			pkgdb_set_case_sensitivity(true);
			break;
		case 'f':
			f |= PKG_FLAG_FORCE;
			break;
		case 'F':
			f |= PKG_FLAG_SKIP_INSTALL;
			lock_type = PKGDB_LOCK_READONLY;
			break;
		case 'g':
			match = MATCH_GLOB;
			break;
		case 'i':
			pkgdb_set_case_sensitivity(false);
			break;
		case 'I':
			f |= PKG_FLAG_NOSCRIPT;
			break;
		case 'n':
			f |= PKG_FLAG_DRY_RUN;
			lock_type = PKGDB_LOCK_READONLY;
			dry_run = true;
			break;
		case 'q':
			quiet = true;
			break;
		case 'r':
			vec_push(&reponames, optarg);
			break;
		case 'U':
			auto_update = false;
			break;
		case 'x':
			match = MATCH_REGEX;
			break;
		case 'y':
			yes = true;
			break;
		case 'v':
			f |= PKG_FLAG_UPGRADE_VULNERABLE;
			break;
		case 0:
			if (scriptnoexec == 1)
				f |= PKG_FLAG_NOEXEC;
			break;
		default:
			usage_upgrade();
			return (EXIT_FAILURE);
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;

	if (dry_run && !auto_update)
		retcode = pkgdb_access2(PKGDB_MODE_READ,
				       PKGDB_DB_LOCAL|PKGDB_DB_REPO, &reponames);
	else
		retcode = pkgdb_access2(PKGDB_MODE_READ  |
				       PKGDB_MODE_WRITE |
				       PKGDB_MODE_CREATE,
				       PKGDB_DB_LOCAL|PKGDB_DB_REPO, &reponames);
	if (retcode == EPKG_ENOACCESS && dry_run) {
		auto_update = false;
		retcode = pkgdb_access2(PKGDB_MODE_READ,
				       PKGDB_DB_LOCAL|PKGDB_DB_REPO, &reponames);
	}

	if (retcode == EPKG_ENOACCESS) {
		warnx("Insufficient privilege to upgrade packages");
		return (EXIT_FAILURE);
	} else if (retcode != EPKG_OK)
		return (EXIT_FAILURE);
	else
		retcode = EXIT_FAILURE;

	/* first update the remote repositories if needed */
	if (auto_update &&
	    (updcode = pkgcli_update(false, false, &reponames)) != EPKG_OK)
		return (updcode);

	if (pkgdb_open_all2(&db, PKGDB_REMOTE, &reponames) != EPKG_OK)
		return (EXIT_FAILURE);

	if (pkgdb_obtain_lock(db, lock_type) != EPKG_OK) {
		pkgdb_close(db);
		warnx("Cannot get an advisory lock on a database, it is locked by another process");
		return (EXIT_FAILURE);
	}

	if (pkg_jobs_new(&jobs, PKG_JOBS_UPGRADE, db) != EPKG_OK)
		goto cleanup;

	if (reponames.len > 0 && pkg_jobs_set_repositories(jobs, &reponames) != EPKG_OK)
		goto cleanup;

	pkg_jobs_set_flags(jobs, f);

	if (argc > 0)
		if (pkg_jobs_add(jobs, match, argv, argc) == EPKG_FATAL)
				goto cleanup;

	if (f & PKG_FLAG_UPGRADE_VULNERABLE) {
		/* We need to load audit info and add packages that are vulnerable */
		if (add_vulnerable_upgrades(jobs, db) != EPKG_OK) {
			goto cleanup;
		}
	}

	if (pkg_jobs_solve(jobs) != EPKG_OK)
		goto cleanup;

	while ((nbactions = pkg_jobs_count(jobs)) > 0) {
		/* print a summary before applying the jobs */
		rc = yes;
		if (!quiet || dry_run) {
			print_jobs_summary(jobs,
				"The following %d package(s) will be affected (of %d checked):\n\n",
				nbactions, pkg_jobs_total(jobs));

			if (!dry_run) {
				rc = query_yesno(false, "\nProceed with this "
						"action? ");
			} else {
				rc = false;
			}
		}

		if (rc) {
			retcode = pkg_jobs_apply(jobs);
			done = 1;
			if (retcode == EPKG_CONFLICT) {
				printf("Conflicts with the existing packages "
				    "have been found.\nOne more solver "
				    "iteration is needed to resolve them.\n");
				continue;
			}
			else if (retcode != EPKG_OK)
				goto cleanup;
		}

		if (messages != NULL) {
			fflush(messages->fp);
			printf("%s", messages->buf);
		}
		break;
	}

	if (done == 0 && rc && !quiet)
		printf("Your packages are up to date.\n");

	if (rc)
		retcode = EXIT_SUCCESS;
	else
		retcode = EXIT_FAILURE;

cleanup:
	pkg_jobs_free(jobs);
	pkgdb_release_lock(db, lock_type);
	pkgdb_close(db);

	if (!dry_run)
		pkg_cache_full_clean();

	if (!rc && newpkgversion)
		newpkgversion = false;

	return (retcode);
}
