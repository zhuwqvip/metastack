/*****************************************************************************\
 *  licenses.c - Functions for handling cluster-wide consumable resources
 *****************************************************************************
 *  Copyright (C) 2008-2011 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette@llnl.gov>, et. al.
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "slurm/slurm_errno.h"

#include "src/common/assoc_mgr.h"
#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmctld/licenses.h"
#include "src/slurmctld/reservation.h"
#include "src/slurmctld/slurmctld.h"
#include "src/common/slurm_accounting_storage.h"

List license_list = (List) NULL;
time_t last_license_update = 0;
static pthread_mutex_t license_mutex = PTHREAD_MUTEX_INITIALIZER;
static void _pack_license(licenses_t *lic, buf_t *buffer,
			  uint16_t protocol_version);

typedef struct {
	char *name;
	slurmctld_resv_t *resv_ptr;
} bf_licenses_find_resv_t;

/* Print all licenses on a list */
static void _licenses_print(char *header, List licenses, job_record_t *job_ptr)
{
	ListIterator iter;
	licenses_t *license_entry;

	if (licenses == NULL)
		return;
	if (!(slurm_conf.debug_flags & DEBUG_FLAG_LICENSE))
		return;

	iter = list_iterator_create(licenses);
  	while ((license_entry = list_next(iter))) {
		if (!job_ptr) {
			info("licenses: %s=%s total=%u used=%u",
			     header, license_entry->name,
			     license_entry->total, license_entry->used);
		} else {
			info("licenses: %s=%s %pJ available=%u used=%u",
			     header, license_entry->name, job_ptr,
			     license_entry->total, license_entry->used);
		}
	}
	list_iterator_destroy(iter);
}

/* Free a license_t record (for use by FREE_NULL_LIST) */
extern void license_free_rec(void *x)
{
	licenses_t *license_entry = (licenses_t *) x;

	if (license_entry) {
		xfree(license_entry->name);
		xfree(license_entry);
	}
}

/* Find a license_t record by license name (for use by list_find_first) */
static int _license_find_rec(void *x, void *key)
{
	licenses_t *license_entry = (licenses_t *) x;
	char *name = (char *) key;

	if ((license_entry->name == NULL) || (name == NULL))
		return 0;
	if (xstrcmp(license_entry->name, name))
		return 0;
	return 1;
}

/* Find a license_t record by license name (for use by list_find_first) */
static int _license_find_remote_rec(void *x, void *key)
{
	licenses_t *license_entry = (licenses_t *) x;

	if (!license_entry->remote)
		return 0;
	return _license_find_rec(x, key);
}

/* Given a license string, return a list of license_t records */
static List _build_license_list(char *licenses, bool *valid)
{
	int i;
	char *end_num, *tmp_str, *token, *last;
	licenses_t *license_entry;
	List lic_list;

	*valid = true;
	if ((licenses == NULL) || (licenses[0] == '\0'))
		return NULL;

	lic_list = list_create(license_free_rec);
	tmp_str = xstrdup(licenses);
	token = strtok_r(tmp_str, ",;", &last);
	while (token && *valid) {
		int32_t num = 1;
		for (i = 0; token[i]; i++) {
			if (isspace(token[i])) {
				*valid = false;
				break;
			}

			if (token[i] == ':') {
				token[i++] = '\0';
				num = (int32_t)strtol(&token[i], &end_num, 10);
				if (*end_num != '\0')
					 *valid = false;
				break;
			}
		}
		if (num < 0 || !(*valid)) {
			*valid = false;
			break;
		}

		license_entry = list_find_first(lic_list, _license_find_rec,
						token);
		if (license_entry) {
			license_entry->total += num;
		} else {
			license_entry = xmalloc(sizeof(licenses_t));
			license_entry->name = xstrdup(token);
			license_entry->total = num;
			list_push(lic_list, license_entry);
		}
		token = strtok_r(NULL, ",;", &last);
	}
	xfree(tmp_str);

	if (*valid == false) {
		FREE_NULL_LIST(lic_list);
	}
	return lic_list;
}

/*
 * Given a list of license_t records, return a license string.
 *
 * This can be combined with _build_license_list() to eliminate duplicates
 *
 * IN license_list - list of license_t records
 *
 * RET string representation of licenses. Must be destroyed by caller.
 */
extern char *license_list_to_string(List license_list)
{
	char *sep = "";
	char *licenses = NULL;
	ListIterator iter;
	licenses_t *license_entry;

	if (!license_list)
		return licenses;

	iter = list_iterator_create(license_list);
	while ((license_entry = list_next(iter))) {
		xstrfmtcat(licenses, "%s%s:%u",
			   sep, license_entry->name, license_entry->total);
		sep = ",";
	}
	list_iterator_destroy(iter);

	return licenses;
}

/* license_mutex should be locked before calling this. */
static void _add_res_rec_2_lic_list(slurmdb_res_rec_t *rec, bool sync)
{
	licenses_t *license_entry = xmalloc(sizeof(licenses_t));

	license_entry->name = xstrdup_printf("%s@%s", rec->name, rec->server);
	license_entry->total = ((rec->count *
				 rec->clus_res_rec->percent_allowed) / 100);
	license_entry->remote = sync ? 2 : 1;

	list_push(license_list, license_entry);
	last_license_update = time(NULL);
}

/* Initialize licenses on this system based upon slurm.conf */
extern int license_init(char *licenses)
{
	bool valid = true;

	last_license_update = time(NULL);

	slurm_mutex_lock(&license_mutex);
	if (license_list)
		fatal("license_list already defined");

	license_list = _build_license_list(licenses, &valid);
	if (!valid)
		fatal("Invalid configured licenses: %s", licenses);

	_licenses_print("init_license", license_list, NULL);
	slurm_mutex_unlock(&license_mutex);
	return SLURM_SUCCESS;
}

/* Update licenses on this system based upon slurm.conf.
 * Remove all previously allocated licenses */
extern int license_update(char *licenses)
{
        ListIterator iter;
        licenses_t *license_entry, *match;
        List new_list;
        bool valid = true;

        new_list = _build_license_list(licenses, &valid);
        if (!valid)
                fatal("Invalid configured licenses: %s", licenses);

        slurm_mutex_lock(&license_mutex);
        if (!license_list) {        /* no licenses before now */
                license_list = new_list;
                slurm_mutex_unlock(&license_mutex);
                return SLURM_SUCCESS;
        }

        iter = list_iterator_create(license_list);
        while ((license_entry = list_next(iter))) {
		/* Always add the remote ones, since we handle those
		   else where. */
		if (license_entry->remote) {
			list_remove(iter);
			if (!new_list)
				new_list = list_create(license_free_rec);
			license_entry->used = 0;
			list_append(new_list, license_entry);
			continue;
		}
		if (new_list)
			match = list_find_first(new_list, _license_find_rec,
						license_entry->name);
		else
			match = NULL;

                if (!match) {
                        info("license %s removed with %u in use",
                             license_entry->name, license_entry->used);
                } else {
                        if (license_entry->used > match->total) {
                                info("license %s count decreased",
                                     match->name);
                        }
                }
        }
        list_iterator_destroy(iter);

        FREE_NULL_LIST(license_list);
        license_list = new_list;
        _licenses_print("update_license", license_list, NULL);
        slurm_mutex_unlock(&license_mutex);
        return SLURM_SUCCESS;
}

extern void license_add_remote(slurmdb_res_rec_t *rec)
{
	licenses_t *license_entry;
	char *name;


	xassert(rec);
	xassert(rec->type == SLURMDB_RESOURCE_LICENSE);

	name = xstrdup_printf("%s@%s", rec->name, rec->server);

	slurm_mutex_lock(&license_mutex);
	if (!license_list) {
		/* If last_license_update then init already ran and we
		 * don't have any licenses defined in the slurm.conf
		 * so make the license_list.
		 */
		xassert(last_license_update);
		license_list = list_create(license_free_rec);
	}

	license_entry = list_find_first(
		license_list, _license_find_remote_rec, name);

	if (license_entry)
		error("license_add_remote: license %s already exists!", name);
	else
		_add_res_rec_2_lic_list(rec, 0);

	xfree(name);

	slurm_mutex_unlock(&license_mutex);
}

extern void license_update_remote(slurmdb_res_rec_t *rec)
{
	licenses_t *license_entry;
	char *name;

	xassert(rec);
	xassert(rec->clus_res_rec);
	xassert(rec->type == SLURMDB_RESOURCE_LICENSE);

	name = xstrdup_printf("%s@%s", rec->name, rec->server);

	slurm_mutex_lock(&license_mutex);
	if (!license_list) {
		/* If last_license_update then init already ran and we
		 * don't have any licenses defined in the slurm.conf
		 * so make the license_list.
		 */
		xassert(last_license_update);
		license_list = list_create(license_free_rec);
	}

	license_entry = list_find_first(
		license_list, _license_find_remote_rec, name);

	if (!license_entry) {
		debug("license_update_remote: License '%s' not found, adding",
		      name);
		_add_res_rec_2_lic_list(rec, 0);
	} else {
		license_entry->total =
			((rec->count *
			  rec->clus_res_rec->percent_allowed) / 100);
		if (license_entry->used > license_entry->total) {
			info("license %s count decreased",
			     license_entry->name);
		}
	}
	last_license_update = time(NULL);

	xfree(name);

	slurm_mutex_unlock(&license_mutex);
}

#ifdef __METASTACK_NEW_LICENSE_OCCUPIED
extern int license_update_remote2(slurm_license_info_t *rec)
{
	licenses_t *license_entry;
	char *name;
	int ret = 0;
	name = xstrdup_printf("%s@%s", rec->name, rec->server);

	slurm_mutex_lock(&license_mutex);

	if (!license_list) {
		xassert(last_license_update);
		license_list = list_create(license_free_rec);
	}
	
	license_entry = list_find_first(
		license_list, _license_find_remote_rec, name);

	if (!license_entry) {
		error("license_update_remote2: License '%s' not found.", name);
		ret = -1;
	} else {

		license_entry->occupied = (rec->occupied > license_entry->total) ? license_entry->total : rec->occupied;
	}

	last_license_update = time(NULL);

	xfree(name);

	slurm_mutex_unlock(&license_mutex);
	
	return ret;
}
#endif

extern void license_remove_remote(slurmdb_res_rec_t *rec)
{
	licenses_t *license_entry;
	ListIterator iter;
	char *name;

	xassert(rec);
	xassert(rec->type == SLURMDB_RESOURCE_LICENSE);

	slurm_mutex_lock(&license_mutex);
	if (!license_list) {
		xassert(last_license_update);
		license_list = list_create(license_free_rec);
	}

	name = xstrdup_printf("%s@%s", rec->name, rec->server);

	iter = list_iterator_create(license_list);
	while ((license_entry = list_next(iter))) {
		if (!license_entry->remote)
			continue;
		if (!xstrcmp(license_entry->name, name)) {
			info("license_remove_remote: license %s "
			     "removed with %u in use",
			     license_entry->name, license_entry->used);
			list_delete_item(iter);
			last_license_update = time(NULL);
			break;
		}
	}
	list_iterator_destroy(iter);

	if (!license_entry)
		error("license_remote_remote: License '%s' not found", name);

	xfree(name);
	slurm_mutex_unlock(&license_mutex);
}

extern void license_sync_remote(List res_list)
{
	slurmdb_res_rec_t *rec = NULL;
	licenses_t *license_entry;
	ListIterator iter;

	slurm_mutex_lock(&license_mutex);
	if (res_list && !license_list) {
		xassert(last_license_update);
		license_list = list_create(license_free_rec);
	}

	iter = list_iterator_create(license_list);
	if (res_list) {
		ListIterator iter2 = list_iterator_create(res_list);
		while ((rec = list_next(iter2))) {
			char *name;
			if (rec->type != SLURMDB_RESOURCE_LICENSE)
				continue;
			name = xstrdup_printf("%s@%s", rec->name, rec->server);
			while ((license_entry = list_next(iter))) {
				if (!license_entry->remote)
					continue;
				if (!xstrcmp(license_entry->name, name)) {
					license_entry->remote = 2;
					license_entry->total =
						((rec->count *
						  rec->clus_res_rec->
						  percent_allowed) / 100);
					if (license_entry->used >
					    license_entry->total) {
						info("license %s count "
						     "decreased",
						     license_entry->name);
					}
					last_license_update = time(NULL);
					break;
				}
			}
			xfree(name);
			if (!license_entry)
				_add_res_rec_2_lic_list(rec, 1);
			list_iterator_reset(iter);
		}
		list_iterator_destroy(iter2);
	}

	while ((license_entry = list_next(iter))) {
		if (!license_entry->remote)
			continue;
		else if (license_entry->remote == 1) {
			info("license_remove_remote: license %s "
			     "removed with %u in use",
			     license_entry->name, license_entry->used);
			list_delete_item(iter);
			last_license_update = time(NULL);
		} else if (license_entry->remote == 2)
			license_entry->remote = 1;
	}
	list_iterator_destroy(iter);

	slurm_mutex_unlock(&license_mutex);
}

/* Free memory associated with licenses on this system */
extern void license_free(void)
{
	slurm_mutex_lock(&license_mutex);
	FREE_NULL_LIST(license_list);
	slurm_mutex_unlock(&license_mutex);
}

/*
 * license_validate - Test if the required licenses are valid
 * IN licenses - required licenses
 * IN validate_configured - if true, validate that there are enough configured
 *                          licenses for the requested amount.
 * IN validate_existing - if true, validate that licenses exist, otherwise don't
 *                        return them in the final list.
 * OUT tres_req_cnt - appropriate counts for each requested gres,
 *                    since this only matters on pending jobs you can
 *                    send in NULL otherwise
 * OUT valid - true if required licenses are valid and a sufficient number
 *             are configured (though not necessarily available now)
 * RET license_list, must be destroyed by caller
 */
extern List license_validate(char *licenses, bool validate_configured,
			     bool validate_existing,
			     uint64_t *tres_req_cnt, bool *valid)
{
	ListIterator iter;
	licenses_t *license_entry, *match;
	List job_license_list;
	static bool first_run = 1;
	static slurmdb_tres_rec_t tres_req;
	int tres_pos;

	job_license_list = _build_license_list(licenses, valid);
	if (!job_license_list)
		return job_license_list;

	/* we only need to init this once */
	if (first_run) {
		first_run = 0;
		memset(&tres_req, 0, sizeof(slurmdb_tres_rec_t));
		tres_req.type = "license";
	}

	slurm_mutex_lock(&license_mutex);
	_licenses_print("request_license", job_license_list, NULL);
	iter = list_iterator_create(job_license_list);
	while ((license_entry = list_next(iter))) {
		if (license_list) {
			match = list_find_first(license_list,
						_license_find_rec,
						license_entry->name);
		} else
			match = NULL;
		if (!match) {
			debug("License name requested (%s) does not exist",
			      license_entry->name);
			if (!validate_existing) {
				list_delete_item(iter);
				continue;
			}
			*valid = false;
			break;
		} else if (validate_configured &&
			   (license_entry->total > match->total)) {
			debug("Licenses count requested higher than configured "
			      "(%s: %u > %u)",
			      match->name, license_entry->total, match->total);
			*valid = false;
			break;
		}

		if (tres_req_cnt) {
			tres_req.name = license_entry->name;
			if ((tres_pos = assoc_mgr_find_tres_pos(
				     &tres_req, false)) != -1)
				tres_req_cnt[tres_pos] =
					(uint64_t)license_entry->total;
		}
	}
	list_iterator_destroy(iter);
	slurm_mutex_unlock(&license_mutex);

	if (!(*valid)) {
		FREE_NULL_LIST(job_license_list);
	}
	return job_license_list;
}

/*
 * license_job_merge - The licenses from one job have just been merged into
 *	another job by appending one job's licenses to another, possibly
 *	including duplicate names. Reconstruct this job's licenses and
 *	license_list fields to eliminate duplicates.
 */
extern void license_job_merge(job_record_t *job_ptr)
{
	bool valid = true;

	FREE_NULL_LIST(job_ptr->license_list);
	job_ptr->license_list = _build_license_list(job_ptr->licenses, &valid);
	xfree(job_ptr->licenses);
	job_ptr->licenses = license_list_to_string(job_ptr->license_list);
}

/*
 * license_job_test - Test if the licenses required for a job are available
 * IN job_ptr - job identification
 * IN when    - time to check
 * IN reboot    - true if node reboot required to start job
 * RET: SLURM_SUCCESS, EAGAIN (not available now), SLURM_ERROR (never runnable)
 */
extern int license_job_test(job_record_t *job_ptr, time_t when, bool reboot)
{
	ListIterator iter;
	licenses_t *license_entry, *match;
	int rc = SLURM_SUCCESS, resv_licenses;

	if (!job_ptr->license_list)	/* no licenses needed */
		return rc;

	slurm_mutex_lock(&license_mutex);
	iter = list_iterator_create(job_ptr->license_list);
	while ((license_entry = list_next(iter))) {
		match = list_find_first(license_list, _license_find_rec,
			license_entry->name);
		if (!match) {
			error("could not find license %s for job %u",
			      license_entry->name, job_ptr->job_id);
			rc = SLURM_ERROR;
			break;
		} else if (license_entry->total > match->total) {
			info("job %u wants more %s licenses than configured",
			     job_ptr->job_id, match->name);
			rc = SLURM_ERROR;
			break;
#ifndef __METASTACK_NEW_LICENSE_OCCUPIED
		} else if ((license_entry->total + match->used) >
#else
		} else if ((license_entry->total + MAX(match->used, match->occupied)) >
#endif
			   match->total) {
			rc = EAGAIN;
			break;
		} else {
			/* Assume node reboot required since we have not
			 * selected the compute nodes yet */
			resv_licenses = job_test_lic_resv(job_ptr,
							  license_entry->name,
							  when, reboot);
			if ((license_entry->total + match->used +
			     resv_licenses) > match->total) {
				rc = EAGAIN;
				break;
			}
		}
	}
	list_iterator_destroy(iter);
	slurm_mutex_unlock(&license_mutex);
	return rc;
}


#ifdef __METASTACK_OPT_MAIN_SCHED_LICENSE
/*
 * sched_license_job_test - Test if the licenses required for a job are available
 * IN job_ptr - job identification
 * IN when    - time to check
 * IN reboot    - true if node reboot required to start job
 * RET: SLURM_SUCCESS, EAGAIN (not available now), SLURM_ERROR (never runnable)
 */
extern int sched_license_job_test(job_record_t *job_ptr, time_t when, bool reboot)
{
	ListIterator iter;
	licenses_t *license_entry, *match, *license_entry_dic, *license_entry_dic2;
	int rc = SLURM_SUCCESS, resv_licenses;

	if (!job_ptr->license_list)	/* no licenses needed */
		return rc;

	slurm_mutex_lock(&license_mutex);
	
	/* license decision */
	iter = list_iterator_create(job_ptr->license_list);
	while ((license_entry = list_next(iter))) {
		match = list_find_first(license_list, _license_find_rec,
			license_entry->name);
		license_entry_dic = list_find_first(queue_license_list, _license_find_rec, license_entry->name);
		if (!match) {
			error("could not find license %s for job %u",
			      license_entry->name, job_ptr->job_id);
			rc = SLURM_ERROR;
			goto fail_lic;
		} else if (license_entry->total > match->total) {
			info("job %u wants more %s licenses than configured",
			     job_ptr->job_id, match->name);
			rc = SLURM_ERROR;
			goto fail_lic;
#ifndef __METASTACK_NEW_LICENSE_OCCUPIED
		} else if ((license_entry->total + match->used) >
#else
		} else if ((license_entry->total + MAX(match->used, match->occupied)) >
#endif
			   match->total) {
			rc = EAGAIN;
			break;
		} else {
			/* Assume node reboot required since we have not
			 * selected the compute nodes yet */
			resv_licenses = job_test_lic_resv(job_ptr,
							  license_entry->name,
							  when, reboot);
			if ((license_entry->total + match->used +
			     resv_licenses) > match->total) {
				rc = EAGAIN;
				break;
			}

			/* if Dic+request+used+resv > Total break */
			if (!license_entry_dic) {
				if ((license_entry->total + match->used +
					resv_licenses) > match->total) {
					info("job %u request license + used + resv > total",job_ptr->job_id);
					rc = EAGAIN;
					break;
				}
			} else {
				if ((license_entry_dic->total + license_entry->total + match->used +
					resv_licenses) > match->total) {
					info("job %u request license + used + resv > total",job_ptr->job_id);
					rc = EAGAIN;
					break;
				}
			}
			
		}
	}

	list_iterator_reset(iter);
	/* add licenses to list */
	while ((license_entry = list_next(iter))) {
		license_entry_dic2 = list_find_first(queue_license_list, _license_find_rec, license_entry->name);
		if (license_entry_dic2) {
			license_entry_dic2->total = license_entry->total + license_entry_dic2->total;
		} else {
			license_entry_dic2 = xmalloc(sizeof(licenses_t));
			license_entry_dic2->name = xstrdup(license_entry->name);
			license_entry_dic2->total = license_entry->total;
			list_push(queue_license_list, license_entry_dic2);
		}
	}

fail_lic:
	list_iterator_destroy(iter);
	
	slurm_mutex_unlock(&license_mutex);
	return rc;
}


/*
 * license_queue_modify_dic - Job request license is removed before job start
 * IN job_ptr - job identification
 * IN queue_license_list - queue license feature list
 * RET: SLURM_SUCCESS
 */
extern int license_queue_modify_dic(job_record_t *job_ptr, List queue_license_list)
{
	ListIterator iter;
	licenses_t *license_entry, *match;
	int rc = SLURM_SUCCESS;

	slurm_mutex_lock(&license_mutex);
	iter = list_iterator_create(job_ptr->license_list);
	while ((license_entry = list_next(iter))) {
		match = list_find_first(queue_license_list, _license_find_rec,license_entry->name);
		if (!match) {
			error("could not find license %s for job %u",license_entry->name, job_ptr->job_id);
			rc = SLURM_ERROR;
			break;
		} else {
            match->total = match->total - license_entry->total;
        }
	}

	list_iterator_destroy(iter);
	slurm_mutex_unlock(&license_mutex);
	return rc;
}

#endif

/*
 * license_copy - create a copy of a license list
 * IN license_list_src - job license list to be copied
 * RET a copy of the license list
 */
extern List license_copy(List license_list_src)
{
	licenses_t *license_entry_src, *license_entry_dest;
	ListIterator iter;
	List license_list_dest = NULL;

	if (!license_list_src)
		return license_list_dest;

	license_list_dest = list_create(license_free_rec);
	iter = list_iterator_create(license_list_src);
	while ((license_entry_src = list_next(iter))) {
		license_entry_dest = xmalloc(sizeof(licenses_t));
		license_entry_dest->name = xstrdup(license_entry_src->name);
		license_entry_dest->total = license_entry_src->total;
		list_push(license_list_dest, license_entry_dest);
	}
	list_iterator_destroy(iter);
	return license_list_dest;
}

/*
 * license_job_get - Get the licenses required for a job
 * IN job_ptr - job identification
 * RET SLURM_SUCCESS or failure code
 */
extern int license_job_get(job_record_t *job_ptr)
{
	ListIterator iter;
	licenses_t *license_entry, *match;
	int rc = SLURM_SUCCESS;

	if (!job_ptr->license_list)	/* no licenses needed */
		return rc;

	last_license_update = time(NULL);

	slurm_mutex_lock(&license_mutex);
	iter = list_iterator_create(job_ptr->license_list);
	while ((license_entry = list_next(iter))) {
		match = list_find_first(license_list, _license_find_rec,
			license_entry->name);
		if (match) {
			match->used += license_entry->total;
			license_entry->used += license_entry->total;
		} else {
			error("could not find license %s for job %u",
			      license_entry->name, job_ptr->job_id);
			rc = SLURM_ERROR;
		}
	}
	list_iterator_destroy(iter);
	_licenses_print("acquire_license", license_list, job_ptr);
	slurm_mutex_unlock(&license_mutex);
	return rc;
}

/*
 * license_job_return - Return the licenses allocated to a job
 * IN job_ptr - job identification
 * RET SLURM_SUCCESS or failure code
 */
extern int license_job_return(job_record_t *job_ptr)
{
	ListIterator iter;
	licenses_t *license_entry, *match;
	int rc = SLURM_SUCCESS;

	if (!job_ptr->license_list)	/* no licenses needed */
		return rc;

	last_license_update = time(NULL);
	log_flag(TRACE_JOBS, "%s: %pJ", __func__, job_ptr);
	slurm_mutex_lock(&license_mutex);
	iter = list_iterator_create(job_ptr->license_list);
	while ((license_entry = list_next(iter))) {
		match = list_find_first(license_list, _license_find_rec,
			license_entry->name);
		if (match) {
			if (match->used >= license_entry->total)
				match->used -= license_entry->total;
			else {
				error("%s: license use count underflow for %s",
				      __func__, match->name);
				match->used = 0;
				rc = SLURM_ERROR;
			}
			license_entry->used = 0;
		} else {
			/* This can happen after a reconfiguration */
			error("%s: job returning unknown license name %s",
			      __func__, license_entry->name);
		}
	}
	list_iterator_destroy(iter);
	_licenses_print("return_license", license_list, job_ptr);
	slurm_mutex_unlock(&license_mutex);
	return rc;
}

/*
 * license_list_overlap - test if there is any overlap in licenses
 *	names found in the two lists
 */
extern bool license_list_overlap(List list_1, List list_2)
{
	ListIterator iter;
	licenses_t *license_entry;
	bool match = false;

	if (!list_1 || !list_2)
		return false;

	iter = list_iterator_create(list_1);
	while ((license_entry = list_next(iter))) {
		if (list_find_first(list_2, _license_find_rec,
				    license_entry->name)) {
			match = true;
			break;
		}
	}
	list_iterator_destroy(iter);

	return match;
}

/* pack_all_licenses()
 *
 * Return license counters to the library.
 */
extern void
get_all_license_info(char **buffer_ptr,
                     int *buffer_size,
                     uid_t uid,
                     uint16_t protocol_version)
{
	ListIterator iter;
	licenses_t *lic_entry;
	uint32_t lics_packed;
	int tmp_offset;
	buf_t *buffer;
	time_t now = time(NULL);

	debug2("%s: calling for all licenses", __func__);

	buffer_ptr[0] = NULL;
	*buffer_size = 0;

	buffer = init_buf(BUF_SIZE);

	/* write header: version and time
	 */
	lics_packed = 0;
	pack32(lics_packed, buffer);
	pack_time(now, buffer);

	slurm_mutex_lock(&license_mutex);
	if (license_list) {
		iter = list_iterator_create(license_list);
		while ((lic_entry = list_next(iter))) {
			set_reserved_license_count(lic_entry);
			/* Now encode the license data structure.
			 */
			_pack_license(lic_entry, buffer, protocol_version);
			++lics_packed;
		}
		list_iterator_destroy(iter);
	}

	slurm_mutex_unlock(&license_mutex);
	debug2("%s: processed %d licenses", __func__, lics_packed);

	/* put the real record count in the message body header
	 */
	tmp_offset = get_buf_offset(buffer);
	set_buf_offset(buffer, 0);
	pack32(lics_packed, buffer);
	set_buf_offset(buffer, tmp_offset);

	*buffer_size = get_buf_offset(buffer);
	buffer_ptr[0] = xfer_buf_data(buffer);
}

extern uint32_t get_total_license_cnt(char *name)
{
	uint32_t count = 0;
	licenses_t *lic;

	slurm_mutex_lock(&license_mutex);
	if (license_list) {
		lic = list_find_first(
			license_list, _license_find_rec, name);

		if (lic)
			count = lic->total;
	}
	slurm_mutex_unlock(&license_mutex);

	return count;
}

/* node_read should be locked before coming in here
 * returns 1 if change happened.
 */
extern char *licenses_2_tres_str(List license_list)
{
	ListIterator itr;
	slurmdb_tres_rec_t *tres_rec;
	licenses_t *license_entry;
	char *tres_str = NULL;
	static bool first_run = 1;
	static slurmdb_tres_rec_t tres_req;
	assoc_mgr_lock_t locks = { .tres = READ_LOCK };

	if (!license_list)
		return NULL;

	/* we only need to init this once */
	if (first_run) {
		first_run = 0;
		memset(&tres_req, 0, sizeof(slurmdb_tres_rec_t));
		tres_req.type = "license";
	}

	assoc_mgr_lock(&locks);
	itr = list_iterator_create(license_list);
	while ((license_entry = list_next(itr))) {
		tres_req.name = license_entry->name;
		if (!(tres_rec = assoc_mgr_find_tres_rec(&tres_req)))
			continue; /* not tracked */

		if (slurmdb_find_tres_count_in_string(
			    tres_str, tres_rec->id) != INFINITE64)
			continue; /* already handled */
		/* New license */
		xstrfmtcat(tres_str, "%s%u=%"PRIu64,
			   tres_str ? "," : "",
			   tres_rec->id, (uint64_t)license_entry->total);
	}
	list_iterator_destroy(itr);
	assoc_mgr_unlock(&locks);

	return tres_str;
}

extern void license_set_job_tres_cnt(List license_list,
				     uint64_t *tres_cnt,
				     bool locked)
{
	ListIterator itr;
	licenses_t *license_entry;
#ifdef __METASTACK_NEW_PART_PARA_SCHED
	slurmdb_tres_rec_t tres_rec;
#endif	
	int tres_pos;
	assoc_mgr_lock_t locks = { .tres = READ_LOCK };

	if (!license_list || !tres_cnt)
		return;

	if (!locked)
		assoc_mgr_lock(&locks);

#ifdef __METASTACK_NEW_PART_PARA_SCHED
	memset(&tres_rec, 0, sizeof(slurmdb_tres_rec_t));
	tres_rec.type = "license";
#endif
	itr = list_iterator_create(license_list);
	while ((license_entry = list_next(itr))) {
		tres_rec.name = license_entry->name;
		if ((tres_pos = assoc_mgr_find_tres_pos(
			     &tres_rec, locked)) != -1)
			tres_cnt[tres_pos] = (uint64_t)license_entry->total;
	}
	list_iterator_destroy(itr);

	if (!locked)
		assoc_mgr_unlock(&locks);

	return;
}

/*
 * Please update src/common/slurm_protocol_pack.c _unpack_license_info_msg() if
 * this changes.
 */
static void _pack_license(licenses_t *lic, buf_t *buffer,
			  uint16_t protocol_version)
{
#ifdef __META_PROTOCOL
    if (protocol_version >= SLURM_22_05_PROTOCOL_VERSION) {
        if (protocol_version >= META_2_0_PROTOCOL_VERSION) {
            packstr(lic->name, buffer);
            pack32(lic->total, buffer);
            pack32(lic->used, buffer);

            pack32(lic->reserved, buffer);
            pack8(lic->remote, buffer);
#ifdef __METASTACK_NEW_LICENSE_OCCUPIED
            pack32(lic->occupied, buffer);
#endif
        } else {
            packstr(lic->name, buffer);
            pack32(lic->total, buffer);
            pack32(lic->used, buffer);
#ifdef __METASTACK_NEW_LICENSE_OCCUPIED
            pack32(lic->occupied, buffer);
#endif
            pack32(lic->reserved, buffer);
            pack8(lic->remote, buffer);
        }
    } else if (protocol_version >= SLURM_21_08_PROTOCOL_VERSION) {
        packstr(lic->name, buffer);
		pack32(lic->total, buffer);
		pack32(lic->used, buffer);
		pack32(lic->reserved, buffer);
		pack8(lic->remote, buffer);
    } else if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
		packstr(lic->name, buffer);
		pack32(lic->total, buffer);
		pack32(lic->used, buffer);
#ifdef __METASTACK_NEW_LICENSE_OCCUPIED
		pack32(lic->occupied, buffer);
#endif
		pack32(lic->reserved, buffer);
		pack8(lic->remote, buffer);
	} else {
		error("%s: protocol_version %hu not supported",
		      __func__, protocol_version);
	}
#endif
}

static void _bf_license_free_rec(void *x)
{
	bf_license_t *entry = x;

	if (!entry)
		return;

	xfree(entry->name);
	xfree(entry);
}

/*
 * Will never match on a reserved license.
 */
static int _bf_licenses_find_rec(void *x, void *key)
{
	bf_license_t *license_entry = x;
	char *name = key;

	xassert(license_entry->name);
	xassert(name);

	if (license_entry->resv_ptr)
		return 0;

	if (!xstrcmp(license_entry->name, name))
		return 1;

	return 0;
}

static int _bf_licenses_find_resv(void *x, void *key)
{
	bf_license_t *license_entry = x;
	bf_licenses_find_resv_t *target = key;

	if (license_entry->resv_ptr != target->resv_ptr)
		return 0;

	if (xstrcmp(license_entry->name, target->name))
		return 0;

	return 1;
}

extern List bf_licenses_initial(bool bf_running_job_reserve)
{
	List bf_list;
	ListIterator iter;
	licenses_t *license_entry;
	bf_license_t *bf_entry;

	if (!license_list || !list_count(license_list))
		return NULL;

	bf_list = list_create(_bf_license_free_rec);

	iter = list_iterator_create(license_list);
	while ((license_entry = list_next(iter))) {
		bf_entry = xmalloc(sizeof(*bf_entry));
		bf_entry->name = xstrdup(license_entry->name);
		bf_entry->remaining = license_entry->total;

		if (!bf_running_job_reserve)
			bf_entry->remaining -= license_entry->used;

		list_push(bf_list, bf_entry);
	}
	list_iterator_destroy(iter);

	return bf_list;
}

extern char *bf_licenses_to_string(bf_licenses_t *licenses_list)
{
	char *sep = "";
	char *licenses = NULL;
	ListIterator iter;
	bf_license_t *entry;

	if (!licenses_list)
		return NULL;

	iter = list_iterator_create(licenses_list);
	while ((entry = list_next(iter))) {
		xstrfmtcat(licenses, "%s%s%s%s%s:%u",
			   (entry->resv_ptr ? "resv=" : ""),
			   (entry->resv_ptr ? entry->resv_ptr->name : ""),
			   (entry->resv_ptr ? ":" : ""),
			   sep, entry->name, entry->remaining);
		sep = ",";
	}
	list_iterator_destroy(iter);

	return licenses;
}

extern bf_licenses_t *slurm_bf_licenses_copy(bf_licenses_t *licenses_src)
{
	bf_license_t *entry_src, *entry_dest;
	ListIterator iter;
	bf_licenses_t *licenses_dest = NULL;

	if (!licenses_src)
		return NULL;

	licenses_dest = list_create(_bf_license_free_rec);

	iter = list_iterator_create(licenses_src);
	while ((entry_src = list_next(iter))) {
		entry_dest = xmalloc(sizeof(*entry_dest));
		entry_dest->name = xstrdup(entry_src->name);
		entry_dest->remaining = entry_src->remaining;
		entry_dest->resv_ptr = entry_src->resv_ptr;
		list_append(licenses_dest, entry_dest);
	}
	list_iterator_destroy(iter);

	return licenses_dest;
}

extern void slurm_bf_licenses_deduct(bf_licenses_t *licenses,
				     job_record_t *job_ptr)
{
	licenses_t *job_entry;
	ListIterator iter;

	xassert(job_ptr);

	if (!job_ptr->license_list)
		return;

	iter = list_iterator_create(job_ptr->license_list);
	while ((job_entry = list_next(iter))) {
		bf_license_t *resv_entry = NULL, *bf_entry;
		int needed = job_entry->total;

		/*
		 * Jobs with reservations may use licenses out of the
		 * reservation, as well as global ones. Deduct from
		 * reservation first, then global as needed.
		 */
		if (job_ptr->resv_ptr) {
			bf_licenses_find_resv_t target_record = {
				.name = job_entry->name,
				.resv_ptr = job_ptr->resv_ptr,
			};

			resv_entry = list_find_first(licenses,
						     _bf_licenses_find_resv,
						     &target_record);
			if (resv_entry && (needed <= resv_entry->remaining)) {
				resv_entry->remaining -= needed;
				continue;
			} else if (resv_entry) {
				needed -= resv_entry->remaining;
				resv_entry->remaining = 0;
			}
		}

		bf_entry = list_find_first(licenses, _bf_licenses_find_rec,
					   job_entry->name);

		if (bf_entry->remaining < needed) {
			error("%s: underflow on %s", __func__, bf_entry->name);
			bf_entry->remaining = 0;
		} else {
			bf_entry->remaining -= needed;
		}
	}
	list_iterator_destroy(iter);
}

/*
 * Tranfer licenses into the control of a reservation.
 * Finds the global license, deducts the required number, then assigns those
 * to a new record locked to that reservation.
 */
extern void slurm_bf_licenses_transfer(bf_licenses_t *licenses,
				       job_record_t *job_ptr)
{
	licenses_t *resv_entry;
	ListIterator iter;

	xassert(job_ptr);

	if (!job_ptr->license_list)
		return;

	iter = list_iterator_create(job_ptr->license_list);
	while ((resv_entry = list_next(iter))) {
		bf_license_t *bf_entry, *new_entry;
		int needed = resv_entry->total, reservable;

		bf_entry = list_find_first(licenses, _bf_licenses_find_rec,
					   resv_entry->name);

		if (bf_entry->remaining < needed) {
			error("%s: underflow on %s", __func__, bf_entry->name);
			reservable = bf_entry->remaining;
			bf_entry->remaining = 0;
		} else {
			bf_entry->remaining -= needed;
			reservable = needed;
		}

		new_entry = xmalloc(sizeof(*new_entry));
		new_entry->name = xstrdup(resv_entry->name);
		new_entry->remaining = reservable;
		new_entry->resv_ptr = job_ptr->resv_ptr;

		list_push(licenses, new_entry);
	}
	list_iterator_destroy(iter);
}

extern bool slurm_bf_licenses_avail(bf_licenses_t *licenses,
				    job_record_t *job_ptr)
{
	ListIterator iter;
	licenses_t *need;
	bool avail = true;

	if (!job_ptr->license_list)
		return true;

	iter = list_iterator_create(job_ptr->license_list);
	while ((need = list_next(iter))) {
		bf_license_t *resv_entry = NULL, *bf_entry;
		int needed = need->total;

		/*
		 * Jobs with reservations may use licenses out of the
		 * reservation, as well as global ones. Deduct from
		 * reservation first, then global as needed.
		 */
		if (job_ptr->resv_ptr) {
			bf_licenses_find_resv_t target_record = {
				.name = need->name,
				.resv_ptr = job_ptr->resv_ptr,
			};

			resv_entry = list_find_first(licenses,
						     _bf_licenses_find_resv,
						     &target_record);

			if (resv_entry && (needed <= resv_entry->remaining))
				continue;
			else if (resv_entry)
				needed -= resv_entry->remaining;
		}

		bf_entry = list_find_first(licenses, _bf_licenses_find_rec,
					   need->name);

		if (bf_entry->remaining < needed) {
			avail = false;
			break;
		}
	}
	list_iterator_destroy(iter);

	return avail;
}

extern bool slurm_bf_licenses_equal(bf_licenses_t *a, bf_licenses_t *b)
{
	bf_license_t *entry_a, *entry_b;
	ListIterator iter;
	bool equivalent = true;

	iter = list_iterator_create(a);
	while ((entry_a = list_next(iter))) {
		entry_b = list_find_first(b, _bf_licenses_find_rec,
					  entry_a->name);

		if ((entry_a->remaining != entry_b->remaining) ||
		    (entry_a->resv_ptr != entry_b->resv_ptr)) {
			equivalent = false;
			break;
		}
	}
	list_iterator_destroy(iter);

	return equivalent;
}
