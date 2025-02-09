/*****************************************************************************\
 *  cred.c - Slurm job and sbcast credential functions
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Portions Copyright (C) 2015 SchedMD <https://www.schedmd.com>.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>.
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

#include "config.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/time.h>

#include "slurm/slurm_errno.h"
#include "src/common/bitstring.h"
#include "src/common/group_cache.h"
#include "src/common/io_hdr.h"
#include "src/common/job_resources.h"
#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/plugin.h"
#include "src/common/plugrack.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_pack.h"
#include "src/common/slurm_time.h"
#include "src/common/uid.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/interfaces/cred.h"
#include "src/interfaces/gres.h"

#define MAX_TIME 0x7fffffff

typedef struct {
	int   (*cred_sign)		(char *buffer, int buf_size,
					 char **signature);
	int   (*cred_verify_sign)	(char *buffer, uint32_t buf_size,
					 char *signature);
	const char *(*cred_str_error)	(int);
} slurm_cred_ops_t;

/*
 * These strings must be in the same order as the fields declared
 * for slurm_cred_ops_t.
 */
static const char *syms[] = {
	"cred_p_sign",
	"cred_p_verify_sign",
	"cred_p_str_error",
};

struct sbcast_cache {
	time_t       expire;	/* Time that the cred was created	*/
	uint32_t value;		/* Hash of credential signature */
};

static slurm_cred_ops_t ops;
static plugin_context_t *g_context = NULL;
static pthread_mutex_t g_context_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t cred_restart_time = (time_t) 0;
static list_t *sbcast_cache_list = NULL;
static int cred_expire = DEFAULT_EXPIRATION_WINDOW;
static bool enable_nss_slurm = false;
static bool enable_send_gids = true;

/*
 * Static prototypes:
 */

static slurm_cred_t *_slurm_cred_alloc(bool alloc_arg);

static int _cred_sign(slurm_cred_t *cred);
static void _cred_verify_signature(slurm_cred_t *cred);

static void _pack_cred(slurm_cred_arg_t *cred, buf_t *buffer,
		       uint16_t protocol_version);

/* Initialize the plugin. */
extern int cred_g_init(void)
{
	char *tok;
	char    *plugin_type = "cred";
	int	retval = SLURM_SUCCESS;

	/*					 123456789012 */
	if ((tok = xstrstr(slurm_conf.authinfo, "cred_expire="))) {
		cred_expire = atoi(tok + 12);
		if (cred_expire < 5) {
			error("AuthInfo=cred_expire=%d invalid", cred_expire);
			cred_expire = DEFAULT_EXPIRATION_WINDOW;
		}
	}

	if (xstrcasestr(slurm_conf.launch_params, "enable_nss_slurm"))
		enable_nss_slurm = true;
	else if (xstrcasestr(slurm_conf.launch_params, "disable_send_gids"))
		enable_send_gids = false;

	slurm_mutex_lock( &g_context_lock );
	if (cred_restart_time == (time_t) 0)
		cred_restart_time = time(NULL);
	if ( g_context )
		goto done;

	g_context = plugin_context_create(plugin_type,
					  slurm_conf.cred_type,
					  (void **) &ops, syms, sizeof(syms));

	if (!g_context) {
		error("cannot create %s context for %s",
		      plugin_type, slurm_conf.cred_type);
		retval = SLURM_ERROR;
		goto done;
	}
	sbcast_cache_list = list_create(xfree_ptr);

done:
	slurm_mutex_unlock( &g_context_lock );

	return(retval);
}

/* Terminate the plugin and release all memory. */
extern int cred_g_fini(void)
{
	int rc;

	if (!g_context)
		return SLURM_SUCCESS;

	FREE_NULL_LIST(sbcast_cache_list);
	rc = plugin_context_destroy(g_context);
	g_context = NULL;
	return rc;
}

/* Fill in user information based on what options are enabled. */
static int _fill_cred_gids(slurm_cred_arg_t *arg)
{
	struct passwd pwd, *result;
	char buffer[PW_BUF_SIZE];
	int rc;

	if (!enable_nss_slurm && !enable_send_gids)
		return SLURM_SUCCESS;

	xassert(arg);

	rc = slurm_getpwuid_r(arg->uid, &pwd, buffer, PW_BUF_SIZE, &result);
	if (rc || !result) {
		if (!result && !rc)
			error("%s: getpwuid_r(%u): no record found",
			      __func__, arg->uid);
		else
			error("%s: getpwuid_r(%u): %s",
			      __func__, arg->uid, slurm_strerror(rc));
		return SLURM_ERROR;
	}

	arg->pw_name = xstrdup(result->pw_name);
	arg->pw_gecos = xstrdup(result->pw_gecos);
	arg->pw_dir = xstrdup(result->pw_dir);
	arg->pw_shell = xstrdup(result->pw_shell);

	arg->ngids = group_cache_lookup(arg->uid, arg->gid,
					arg->pw_name, &arg->gids);

	if (enable_nss_slurm) {
		if (arg->ngids) {
			arg->gr_names = xcalloc(arg->ngids, sizeof(char *));
			for (int i = 0; i < arg->ngids; i++)
				arg->gr_names[i] = gid_to_string(arg->gids[i]);
		}
	}

	return SLURM_SUCCESS;
}

static void _release_cred_gids(slurm_cred_arg_t *arg)
{
	if (!enable_nss_slurm && !enable_send_gids)
		return;

	xfree(arg->pw_name);
	xfree(arg->pw_gecos);
	xfree(arg->pw_dir);
	xfree(arg->pw_shell);
	xfree(arg->gids);

	if (arg->gr_names) {
		for (int i = 0; i < arg->ngids; i++)
			xfree(arg->gr_names[i]);
		xfree(arg->gr_names);
	}
	arg->ngids = 0;
}

extern int cred_expiration(void)
{
	return cred_expire;
}

extern slurm_cred_t *slurm_cred_create(slurm_cred_arg_t *arg, bool sign_it,
				       uint16_t protocol_version)
{
	slurm_cred_t *cred = NULL;
	int i = 0, sock_recs = 0;

	xassert(arg != NULL);
	xassert(g_context);

	if (arg->uid == SLURM_AUTH_NOBODY) {
		error("%s: refusing to create job %u credential for invalid user nobody",
		      __func__, arg->step_id.job_id);
		goto fail;
	}

	if (arg->gid == SLURM_AUTH_NOBODY) {
		error("%s: refusing to create job %u credential for invalid group nobody",
		      __func__, arg->step_id.job_id);
		goto fail;
	}

	cred = _slurm_cred_alloc(false);
	xassert(cred->magic == CRED_MAGIC);

	if (arg->sock_core_rep_count) {
		for (i = 0; i < arg->job_nhosts; i++) {
			sock_recs += arg->sock_core_rep_count[i];
			if (sock_recs >= arg->job_nhosts)
				break;
		}
		i++;
	}
	arg->core_array_size = i;

	if (_fill_cred_gids(arg) != SLURM_SUCCESS)
		goto fail;

	cred->buffer = init_buf(4096);
	cred->buf_version = protocol_version;

	_pack_cred(arg, cred->buffer, protocol_version);

	if (sign_it && _cred_sign(cred) < 0) {
		goto fail;
	}

	/* Release any values populated through _fill_cred_gids(). */
	_release_cred_gids(arg);

	return cred;

fail:
	slurm_cred_destroy(cred);
	return NULL;
}

extern slurm_cred_t *slurm_cred_faker(slurm_cred_arg_t *arg)
{
	slurm_cred_t *cred = NULL;

	/*
	 * Force this on to ensure pw_name, ngid, gids are all populated.
	 */
	enable_send_gids = true;

	cred = slurm_cred_create(arg, true, SLURM_PROTOCOL_VERSION);

	return cred;
}

extern void slurm_cred_free_args(slurm_cred_arg_t *arg)
{
	if (!arg)
		return;

	xfree(arg->pw_name);
	xfree(arg->pw_gecos);
	xfree(arg->pw_dir);
	xfree(arg->pw_shell);
	xfree(arg->gids);
	for (int i = 0; arg->gr_names && i < arg->ngids; i++)
		xfree(arg->gr_names[i]);
	xfree(arg->gr_names);
	FREE_NULL_BITMAP(arg->job_core_bitmap);
	FREE_NULL_BITMAP(arg->step_core_bitmap);
	xfree(arg->cores_per_socket);
	xfree(arg->cpu_array);
	xfree(arg->cpu_array_reps);
	FREE_NULL_LIST(arg->job_gres_list);
	FREE_NULL_LIST(arg->step_gres_list);
	xfree(arg->step_hostlist);
	xfree(arg->job_account);
	xfree(arg->job_alias_list);
	xfree(arg->job_comment);
	xfree(arg->job_constraints);
	xfree(arg->job_licenses);
	xfree(arg->job_hostlist);
	xfree(arg->sock_core_rep_count);
	xfree(arg->sockets_per_node);
	xfree(arg->job_mem_alloc);
	xfree(arg->job_mem_alloc_rep_count);
	xfree(arg->job_partition);
	xfree(arg->job_reservation);
	xfree(arg->job_std_err);
	xfree(arg->job_std_in);
	xfree(arg->job_std_out);
	xfree(arg->step_mem_alloc);
	xfree(arg->step_mem_alloc_rep_count);

	xfree(arg);
}

extern void slurm_cred_unlock_args(slurm_cred_t *cred)
{
	slurm_rwlock_unlock(&cred->mutex);
}

/*
 * Caller *must* release lock.
 */
extern slurm_cred_arg_t *slurm_cred_get_args(slurm_cred_t *cred)
{
	xassert(cred != NULL);

	slurm_rwlock_rdlock(&cred->mutex);
	return cred->arg;
}

extern void *slurm_cred_get(slurm_cred_t *cred,
			    cred_data_enum_t cred_data_type)
{
	void *rc = NULL;

	xassert(cred != NULL);

	slurm_rwlock_rdlock(&cred->mutex);

	if (!cred->arg) {
		slurm_rwlock_unlock(&cred->mutex);
		return NULL;
	}

	switch (cred_data_type) {
	case CRED_DATA_JOB_GRES_LIST:
		rc = (void *) cred->arg->job_gres_list;
		break;
	case CRED_DATA_JOB_ALIAS_LIST:
		rc = (void *) cred->arg->job_alias_list;
		break;
	case CRED_DATA_STEP_GRES_LIST:
		rc = (void *) cred->arg->step_gres_list;
		break;
	default:
		error("%s: Invalid arg type requested (%d)", __func__,
		      cred_data_type);

	}
	slurm_rwlock_unlock(&cred->mutex);

	return rc;
}

/*
 * Returns NULL on error.
 *
 * On success, returns a pointer to the arg structure within the credential.
 * Caller *must* release the lock.
 */
extern slurm_cred_arg_t *slurm_cred_verify(slurm_cred_t *cred)
{
	time_t now = time(NULL);
	int errnum;

	xassert(cred != NULL);

	xassert(g_context);

	slurm_rwlock_rdlock(&cred->mutex);
	xassert(cred->magic == CRED_MAGIC);

	/* NOTE: the verification checks that the credential was
	 * created by SlurmUser or root */
	if (!cred->verified) {
		slurm_seterrno(ESLURMD_INVALID_JOB_CREDENTIAL);
		goto error;
	}

	if (now > (cred->ctime + cred_expire)) {
		slurm_seterrno(ESLURMD_CREDENTIAL_EXPIRED);
		goto error;
	}

	/* coverity[missing_unlock] */
	return cred->arg;

error:
	errnum = slurm_get_errno();
	slurm_rwlock_unlock(&cred->mutex);
	slurm_seterrno(errnum);
	return NULL;
}


extern void slurm_cred_destroy(slurm_cred_t *cred)
{
	if (cred == NULL)
		return;

	xassert(cred->magic == CRED_MAGIC);

	slurm_rwlock_wrlock(&cred->mutex);
	slurm_cred_free_args(cred->arg);
	FREE_NULL_BUFFER(cred->buffer);
	xfree(cred->signature);
	cred->magic = ~CRED_MAGIC;
	slurm_rwlock_unlock(&cred->mutex);
	slurm_rwlock_destroy(&cred->mutex);

	xfree(cred);
}

extern char *slurm_cred_get_signature(slurm_cred_t *cred)
{
	char *sig = NULL;

	xassert(cred);

	slurm_rwlock_rdlock(&cred->mutex);
	sig = xstrdup(cred->signature);
	slurm_rwlock_unlock(&cred->mutex);

	return sig;
}

extern void slurm_cred_get_mem(slurm_cred_t *credential, char *node_name,
			       const char *func_name,
			       uint64_t *job_mem_limit,
			       uint64_t *step_mem_limit)
{
	slurm_cred_arg_t *cred = credential->arg;
	int rep_idx = -1;
	int node_id = -1;

	/*
	 * Batch steps only have the job_hostlist set and will always be 0 here.
	 */
	if (cred->step_id.step_id == SLURM_BATCH_SCRIPT) {
		rep_idx = 0;
	} else if ((node_id =
		    nodelist_find(cred->job_hostlist, node_name)) >= 0) {
		rep_idx = slurm_get_rep_count_inx(cred->job_mem_alloc_rep_count,
					          cred->job_mem_alloc_size,
						  node_id);

	} else {
		error("Unable to find %s in job hostlist: `%s'",
		      node_name, cred->job_hostlist);
	}

	if (rep_idx < 0)
		error("%s: node_id=%d, not found in job_mem_alloc_rep_count requested job memory not reset.",
		      func_name, node_id);
	else
		*job_mem_limit = cred->job_mem_alloc[rep_idx];

	if (!step_mem_limit) {
		log_flag(CPU_BIND, "%s: Memory extracted from credential for %ps job_mem_limit= %"PRIu64,
			 func_name, &cred->step_id, *job_mem_limit);
		return;
	}

	if (cred->step_mem_alloc) {
		rep_idx = -1;
		if ((node_id =
		     nodelist_find(cred->step_hostlist, node_name)) >= 0) {
			rep_idx = slurm_get_rep_count_inx(
						cred->step_mem_alloc_rep_count,
						cred->step_mem_alloc_size,
						node_id);
		} else {
			error("Unable to find %s in step hostlist: `%s'",
			      node_name, cred->step_hostlist);
		}
		if (rep_idx < 0)
			error("%s: node_id=%d, not found in step_mem_alloc_rep_count",
			      func_name, node_id);
		else
			*step_mem_limit = cred->step_mem_alloc[rep_idx];
	}

	/*
	 * If we are not set or we were sent 0 go with the job_mem_limit value.
	 */
	if (!(*step_mem_limit))
		*step_mem_limit = *job_mem_limit;

	log_flag(CPU_BIND, "Memory extracted from credential for %ps job_mem_limit=%"PRIu64" step_mem_limit=%"PRIu64,
		 &cred->step_id, *job_mem_limit, *step_mem_limit);
}

/* Convert bitmap to string representation with brackets removed */
static char *_core_format(bitstr_t *core_bitmap)
{
	char str[1024], *bracket_ptr;

	bit_fmt(str, sizeof(str), core_bitmap);
	if (str[0] != '[')
		return xstrdup(str);

	/* strip off brackets */
	bracket_ptr = strchr(str, ']');
	if (bracket_ptr)
		bracket_ptr[0] = '\0';
	return xstrdup(str+1);
}

/*
 * Retrieve the set of cores that were allocated to the job and step then
 * format them in the List Format (e.g., "0-2,7,12-14"). Also return
 * job and step's memory limit.
 *
 * NOTE: caller must xfree the returned strings.
 */
extern void format_core_allocs(slurm_cred_t *credential, char *node_name,
			       uint16_t cpus, char **job_alloc_cores,
			       char **step_alloc_cores, uint64_t *job_mem_limit,
			       uint64_t *step_mem_limit)
{
	slurm_cred_arg_t *cred = credential->arg;
	bitstr_t	*job_core_bitmap, *step_core_bitmap;
	hostlist_t *hset = NULL;
	int		host_index = -1;
	uint32_t	i, j, i_first_bit=0, i_last_bit=0;

	xassert(cred);
	xassert(job_alloc_cores);
	xassert(step_alloc_cores);
	if (!(hset = hostlist_create(cred->job_hostlist))) {
		error("Unable to create job hostlist: `%s'",
		      cred->job_hostlist);
		return;
	}
#ifdef HAVE_FRONT_END
	host_index = 0;
#else
	host_index = hostlist_find(hset, node_name);
#endif
	if ((host_index < 0) || (host_index >= cred->job_nhosts)) {
		error("Invalid host_index %d for job %u",
		      host_index, cred->step_id.job_id);
		error("Host %s not in hostlist %s",
		      node_name, cred->job_hostlist);
		hostlist_destroy(hset);
		return;
	}
	host_index++;	/* change from 0-origin to 1-origin */
	for (i=0; host_index; i++) {
		if (host_index > cred->sock_core_rep_count[i]) {
			i_first_bit += cred->sockets_per_node[i] *
				cred->cores_per_socket[i] *
				cred->sock_core_rep_count[i];
			host_index -= cred->sock_core_rep_count[i];
		} else {
			i_first_bit += cred->sockets_per_node[i] *
				cred->cores_per_socket[i] *
				(host_index - 1);
			i_last_bit = i_first_bit +
				cred->sockets_per_node[i] *
				cred->cores_per_socket[i];
			break;
		}
	}

	job_core_bitmap  = bit_alloc(i_last_bit - i_first_bit);
	step_core_bitmap = bit_alloc(i_last_bit - i_first_bit);
	for (i = i_first_bit, j = 0; i < i_last_bit; i++, j++) {
		if (bit_test(cred->job_core_bitmap, i))
			bit_set(job_core_bitmap, j);
		if (bit_test(cred->step_core_bitmap, i))
			bit_set(step_core_bitmap, j);
	}

	/* Scale CPU count, same as slurmd/req.c:_get_ncpus() */
	if (i_last_bit <= i_first_bit)
		error("step credential has no CPUs selected");
	else {
		uint32_t i = cpus / (i_last_bit - i_first_bit);
		if (i > 1)
			debug2("scaling CPU count by factor of %d (%u/(%u-%u)",
			       i, cpus, i_last_bit, i_first_bit);
	}

	slurm_cred_get_mem(credential, node_name, __func__, job_mem_limit,
			   step_mem_limit);

	*job_alloc_cores  = _core_format(job_core_bitmap);
	*step_alloc_cores = _core_format(step_core_bitmap);
	FREE_NULL_BITMAP(job_core_bitmap);
	FREE_NULL_BITMAP(step_core_bitmap);
	hostlist_destroy(hset);
}

/*
 * Retrieve the job and step generic resources (gres) allocate to this job
 * on this node.
 *
 * NOTE: Caller must destroy the returned lists
 */
extern void get_cred_gres(slurm_cred_t *credential, char *node_name,
			  list_t **job_gres_list, list_t **step_gres_list)
{
	slurm_cred_arg_t *cred = credential->arg;
	hostlist_t *hset = NULL;
	int		host_index = -1;

	xassert(cred);
	xassert(job_gres_list);
	xassert(step_gres_list);

	FREE_NULL_LIST(*job_gres_list);
	FREE_NULL_LIST(*step_gres_list);
	if ((cred->job_gres_list == NULL) && (cred->step_gres_list == NULL))
		return;

	if (!(hset = hostlist_create(cred->job_hostlist))) {
		error("Unable to create job hostlist: `%s'",
		      cred->job_hostlist);
		return;
	}
#ifdef HAVE_FRONT_END
	host_index = 0;
#else
	host_index = hostlist_find(hset, node_name);
#endif
	hostlist_destroy(hset);
	if ((host_index < 0) || (host_index >= cred->job_nhosts)) {
		error("Invalid host_index %d for job %u",
		      host_index, cred->step_id.job_id);
		error("Host %s not in credential hostlist %s",
		      node_name, cred->job_hostlist);
		return;
	}

	*job_gres_list = gres_job_state_extract(cred->job_gres_list,
						host_index);
	*step_gres_list = gres_step_state_extract(cred->step_gres_list,
						  host_index);
	return;
}

extern void slurm_cred_pack(slurm_cred_t *cred, buf_t *buffer,
			    uint16_t protocol_version)
{
	xassert(cred != NULL);
	xassert(cred->magic == CRED_MAGIC);

	slurm_rwlock_rdlock(&cred->mutex);

	xassert(cred->buffer);
	xassert(cred->buf_version == protocol_version);
	packbuf(cred->buffer, buffer);
	packstr(cred->signature, buffer);

	slurm_rwlock_unlock(&cred->mutex);
}

extern slurm_cred_t *slurm_cred_unpack(buf_t *buffer, uint16_t protocol_version)
{
	uint32_t u32_ngids, len;
	slurm_cred_t *credential = NULL;
	/*
	 * The slightly confusing name is to avoid changing the entire unpack
	 * blocks below during refactor.
	 */
	slurm_cred_arg_t *cred = NULL;
	char *bit_fmt_str = NULL;
	uint32_t tot_core_cnt;
	uint32_t cred_start, cred_len;

	xassert(buffer != NULL);

	/* Save current buffer position here, use it later to verify cred. */
	cred_start = get_buf_offset(buffer);

	credential = _slurm_cred_alloc(true);
	cred = credential->arg;
	if (protocol_version >= SLURM_23_11_PROTOCOL_VERSION) {
		if (unpack_step_id_members(&cred->step_id, buffer,
					   protocol_version) != SLURM_SUCCESS)
			goto unpack_error;
		safe_unpack32(&cred->uid, buffer);
		if (cred->uid == SLURM_AUTH_NOBODY) {
			error("%s: refusing to unpack credential for invalid user nobody",
			      __func__);
			goto unpack_error;
		}

		safe_unpack32(&cred->gid, buffer);
		if (cred->gid == SLURM_AUTH_NOBODY) {
			error("%s: refusing to unpack credential for invalid group nobody",
			      __func__);
			goto unpack_error;
		}

		safe_unpackstr(&cred->pw_name, buffer);
		safe_unpackstr(&cred->pw_gecos, buffer);
		safe_unpackstr(&cred->pw_dir, buffer);
		safe_unpackstr(&cred->pw_shell, buffer);
		safe_unpack32_array(&cred->gids, &u32_ngids, buffer);
		cred->ngids = u32_ngids;
		safe_unpackstr_array(&cred->gr_names, &u32_ngids, buffer);
		if (u32_ngids && cred->ngids != u32_ngids) {
			error("%s: mismatch on gr_names array, %u != %u",
			      __func__, u32_ngids, cred->ngids);
			goto unpack_error;
		}
		if (gres_job_state_unpack(&cred->job_gres_list, buffer,
					  cred->step_id.job_id,
					  protocol_version)
		    != SLURM_SUCCESS)
			goto unpack_error;
		if (gres_step_state_unpack(&cred->step_gres_list,
					   buffer, &cred->step_id,
					   protocol_version)
		    != SLURM_SUCCESS) {
			goto unpack_error;
		}
		safe_unpack16(&cred->job_core_spec, buffer);
		safe_unpackstr(&cred->job_account, buffer);
		safe_unpackstr(&cred->job_alias_list, buffer);
		safe_unpackstr(&cred->job_comment, buffer);
		safe_unpackstr(&cred->job_constraints, buffer);
		safe_unpack_time(&cred->job_end_time, buffer);
		safe_unpackstr(&cred->job_extra, buffer);
		safe_unpack16(&cred->job_oversubscribe, buffer);
		safe_unpackstr(&cred->job_partition, buffer);
		safe_unpackstr(&cred->job_reservation, buffer);
		safe_unpack16(&cred->job_restart_cnt, buffer);
		safe_unpack_time(&cred->job_start_time, buffer);
		safe_unpackstr(&cred->job_std_err, buffer);
		safe_unpackstr(&cred->job_std_in, buffer);
		safe_unpackstr(&cred->job_std_out, buffer);
		safe_unpackstr(&cred->step_hostlist, buffer);
		safe_unpack16(&cred->x11, buffer);
		safe_unpack_time(&credential->ctime, buffer);
		safe_unpack32(&tot_core_cnt, buffer);
		unpack_bit_str_hex(&cred->job_core_bitmap, buffer);
		unpack_bit_str_hex(&cred->step_core_bitmap, buffer);
		safe_unpack16(&cred->core_array_size, buffer);
		if (cred->core_array_size) {
			safe_unpack16_array(&cred->cores_per_socket, &len,
					    buffer);
			if (len != cred->core_array_size)
				goto unpack_error;
			safe_unpack16_array(&cred->sockets_per_node, &len,
					    buffer);
			if (len != cred->core_array_size)
				goto unpack_error;
			safe_unpack32_array(&cred->sock_core_rep_count, &len,
					    buffer);
			if (len != cred->core_array_size)
				goto unpack_error;
		}
		safe_unpack32(&cred->cpu_array_count, buffer);
		if (cred->cpu_array_count) {
			safe_unpack16_array(&cred->cpu_array, &len, buffer);
			if (len != cred->cpu_array_count)
				goto unpack_error;
			safe_unpack32_array(&cred->cpu_array_reps, &len,
					    buffer);
			if (len != cred->cpu_array_count)
				goto unpack_error;
		}
		safe_unpack32(&cred->job_nhosts, buffer);
		safe_unpack32(&cred->job_ntasks, buffer);
		safe_unpackstr(&cred->job_hostlist, buffer);
		safe_unpackstr(&cred->job_licenses, buffer);

		safe_unpack32(&cred->job_mem_alloc_size, buffer);
		if (cred->job_mem_alloc_size) {
			safe_unpack64_array(&cred->job_mem_alloc, &len, buffer);
			if (len != cred->job_mem_alloc_size)
				goto unpack_error;

			safe_unpack32_array(&cred->job_mem_alloc_rep_count,
					    &len, buffer);
			if (len != cred->job_mem_alloc_size)
				goto unpack_error;

		}

		safe_unpack32(&cred->step_mem_alloc_size, buffer);
		if (cred->step_mem_alloc_size) {
			safe_unpack64_array(&cred->step_mem_alloc, &len,
					    buffer);
			if (len != cred->step_mem_alloc_size)
				goto unpack_error;

			safe_unpack32_array(&cred->step_mem_alloc_rep_count,
					    &len, buffer);
			if (len != cred->step_mem_alloc_size)
				goto unpack_error;
		}

		safe_unpackstr(&cred->selinux_context, buffer);

		cred_len = get_buf_offset(buffer) - cred_start;
		/* signature must come after the end of the signed portion */
		safe_unpackstr(&credential->signature, buffer);
	} else if (protocol_version >= SLURM_23_02_PROTOCOL_VERSION) {
		if (unpack_step_id_members(&cred->step_id, buffer,
					   protocol_version) != SLURM_SUCCESS)
			goto unpack_error;
		safe_unpack32(&cred->uid, buffer);
		if (cred->uid == SLURM_AUTH_NOBODY) {
			error("%s: refusing to unpack credential for invalid user nobody",
			      __func__);
			goto unpack_error;
		}

		safe_unpack32(&cred->gid, buffer);
		if (cred->gid == SLURM_AUTH_NOBODY) {
			error("%s: refusing to unpack credential for invalid group nobody",
			      __func__);
			goto unpack_error;
		}

		safe_unpackstr(&cred->pw_name, buffer);
		safe_unpackstr(&cred->pw_gecos, buffer);
		safe_unpackstr(&cred->pw_dir, buffer);
		safe_unpackstr(&cred->pw_shell, buffer);
		safe_unpack32_array(&cred->gids, &u32_ngids, buffer);
		cred->ngids = u32_ngids;
		safe_unpackstr_array(&cred->gr_names, &u32_ngids, buffer);
		if (u32_ngids && cred->ngids != u32_ngids) {
			error("%s: mismatch on gr_names array, %u != %u",
			      __func__, u32_ngids, cred->ngids);
			goto unpack_error;
		}
		if (gres_job_state_unpack(&cred->job_gres_list, buffer,
					  cred->step_id.job_id,
					  protocol_version)
		    != SLURM_SUCCESS)
			goto unpack_error;
		if (gres_step_state_unpack(&cred->step_gres_list,
					   buffer, &cred->step_id,
					   protocol_version)
		    != SLURM_SUCCESS) {
			goto unpack_error;
		}
		safe_unpack16(&cred->job_core_spec, buffer);
		safe_unpackstr(&cred->job_account, buffer);
		safe_unpackstr(&cred->job_alias_list, buffer);
		safe_unpackstr(&cred->job_comment, buffer);
		safe_unpackstr(&cred->job_constraints, buffer);
		safe_unpack_time(&cred->job_end_time, buffer);
		safe_unpackstr(&cred->job_extra, buffer);
		safe_unpack16(&cred->job_oversubscribe, buffer);
		safe_unpackstr(&cred->job_partition, buffer);
		safe_unpackstr(&cred->job_reservation, buffer);
		safe_unpack16(&cred->job_restart_cnt, buffer);
		safe_unpack_time(&cred->job_start_time, buffer);
		safe_unpackstr(&cred->job_std_err, buffer);
		safe_unpackstr(&cred->job_std_in, buffer);
		safe_unpackstr(&cred->job_std_out, buffer);
		safe_unpackstr(&cred->step_hostlist, buffer);
		safe_unpack16(&cred->x11, buffer);
		safe_unpack_time(&credential->ctime, buffer);
		safe_unpack32(&tot_core_cnt, buffer);
		unpack_bit_str_hex(&cred->job_core_bitmap, buffer);
		unpack_bit_str_hex(&cred->step_core_bitmap, buffer);
		safe_unpack16(&cred->core_array_size, buffer);
		if (cred->core_array_size) {
			safe_unpack16_array(&cred->cores_per_socket, &len,
					    buffer);
			if (len != cred->core_array_size)
				goto unpack_error;
			safe_unpack16_array(&cred->sockets_per_node, &len,
					    buffer);
			if (len != cred->core_array_size)
				goto unpack_error;
			safe_unpack32_array(&cred->sock_core_rep_count, &len,
					    buffer);
			if (len != cred->core_array_size)
				goto unpack_error;
		}
		safe_unpack32(&cred->cpu_array_count, buffer);
		if (cred->cpu_array_count) {
			safe_unpack16_array(&cred->cpu_array, &len, buffer);
			if (len != cred->cpu_array_count)
				goto unpack_error;
			safe_unpack32_array(&cred->cpu_array_reps, &len,
					    buffer);
			if (len != cred->cpu_array_count)
				goto unpack_error;
		}
		safe_unpack32(&cred->job_nhosts, buffer);
		safe_unpack32(&cred->job_ntasks, buffer);
		safe_unpackstr(&cred->job_hostlist, buffer);
		safe_unpackstr(&cred->job_licenses, buffer);

		safe_unpack32(&cred->job_mem_alloc_size, buffer);
		if (cred->job_mem_alloc_size) {
			safe_unpack64_array(&cred->job_mem_alloc, &len, buffer);
			if (len != cred->job_mem_alloc_size)
				goto unpack_error;

			safe_unpack32_array(&cred->job_mem_alloc_rep_count,
					    &len, buffer);
			if (len != cred->job_mem_alloc_size)
				goto unpack_error;

		}

		safe_unpack32(&cred->step_mem_alloc_size, buffer);
		if (cred->step_mem_alloc_size) {
			safe_unpack64_array(&cred->step_mem_alloc, &len,
					    buffer);
			if (len != cred->step_mem_alloc_size)
				goto unpack_error;

			safe_unpack32_array(&cred->step_mem_alloc_rep_count,
					    &len, buffer);
			if (len != cred->step_mem_alloc_size)
				goto unpack_error;
		}

		safe_unpackstr(&cred->selinux_context, buffer);

		cred_len = get_buf_offset(buffer) - cred_start;
		/* signature must come after the end of the signed portion */
		safe_unpackstr(&credential->signature, buffer);
	} else if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
		if (unpack_step_id_members(&cred->step_id, buffer,
					   protocol_version) != SLURM_SUCCESS)
			goto unpack_error;
		safe_unpack32(&cred->uid, buffer);
		if (cred->uid == SLURM_AUTH_NOBODY) {
			error("%s: refusing to unpack credential for invalid user nobody",
			      __func__);
			goto unpack_error;
		}

		safe_unpack32(&cred->gid, buffer);
		if (cred->gid == SLURM_AUTH_NOBODY) {
			error("%s: refusing to unpack credential for invalid group nobody",
			      __func__);
			goto unpack_error;
		}
		safe_unpackstr(&cred->pw_name, buffer);
		safe_unpackstr(&cred->pw_gecos, buffer);
		safe_unpackstr(&cred->pw_dir, buffer);
		safe_unpackstr(&cred->pw_shell, buffer);
		safe_unpack32_array(&cred->gids, &u32_ngids, buffer);
		cred->ngids = u32_ngids;
		safe_unpackstr_array(&cred->gr_names, &u32_ngids, buffer);
		if (u32_ngids && cred->ngids != u32_ngids) {
			error("%s: mismatch on gr_names array, %u != %u",
			      __func__, u32_ngids, cred->ngids);
			goto unpack_error;
		}
		if (gres_job_state_unpack(&cred->job_gres_list, buffer,
					  cred->step_id.job_id,
					  protocol_version)
		    != SLURM_SUCCESS)
			goto unpack_error;
		if (gres_step_state_unpack(&cred->step_gres_list,
					   buffer, &cred->step_id,
					   protocol_version)
		    != SLURM_SUCCESS) {
			goto unpack_error;
		}
		safe_unpack16(&cred->job_core_spec, buffer);
		safe_unpackstr(&cred->job_account, buffer);
		safe_unpackstr(&cred->job_alias_list, buffer);
		safe_unpackstr(&cred->job_comment, buffer);
		safe_unpackstr(&cred->job_constraints, buffer);
		safe_unpackstr(&cred->job_partition, buffer);
		safe_unpackstr(&cred->job_reservation, buffer);
		safe_unpack16(&cred->job_restart_cnt, buffer);
		safe_unpackstr(&cred->job_std_err, buffer);
		safe_unpackstr(&cred->job_std_in, buffer);
		safe_unpackstr(&cred->job_std_out, buffer);
		safe_unpackstr(&cred->step_hostlist, buffer);
		safe_unpack16(&cred->x11, buffer);
		safe_unpack_time(&credential->ctime, buffer);
		safe_unpack32(&tot_core_cnt, buffer);
		unpack_bit_str_hex(&cred->job_core_bitmap, buffer);
		unpack_bit_str_hex(&cred->step_core_bitmap, buffer);
		safe_unpack16(&cred->core_array_size, buffer);
		if (cred->core_array_size) {
			safe_unpack16_array(&cred->cores_per_socket, &len,
					    buffer);
			if (len != cred->core_array_size)
				goto unpack_error;
			safe_unpack16_array(&cred->sockets_per_node, &len,
					    buffer);
			if (len != cred->core_array_size)
				goto unpack_error;
			safe_unpack32_array(&cred->sock_core_rep_count, &len,
					    buffer);
			if (len != cred->core_array_size)
				goto unpack_error;
		}
		safe_unpack32(&cred->cpu_array_count, buffer);
		if (cred->cpu_array_count) {
			safe_unpack16_array(&cred->cpu_array, &len, buffer);
			if (len != cred->cpu_array_count)
				goto unpack_error;
			safe_unpack32_array(&cred->cpu_array_reps, &len,
					    buffer);
			if (len != cred->cpu_array_count)
				goto unpack_error;
		}
		safe_unpack32(&cred->job_nhosts, buffer);
		safe_unpack32(&cred->job_ntasks, buffer);
		safe_unpackstr(&cred->job_hostlist, buffer);

		safe_unpack32(&cred->job_mem_alloc_size, buffer);
		if (cred->job_mem_alloc_size) {
			safe_unpack64_array(&cred->job_mem_alloc, &len, buffer);
			if (len != cred->job_mem_alloc_size)
				goto unpack_error;

			safe_unpack32_array(&cred->job_mem_alloc_rep_count,
					    &len, buffer);
			if (len != cred->job_mem_alloc_size)
				goto unpack_error;

		}

		safe_unpack32(&cred->step_mem_alloc_size, buffer);
		if (cred->step_mem_alloc_size) {
			safe_unpack64_array(&cred->step_mem_alloc, &len,
					    buffer);
			if (len != cred->step_mem_alloc_size)
				goto unpack_error;

			safe_unpack32_array(&cred->step_mem_alloc_rep_count,
					    &len, buffer);
			if (len != cred->step_mem_alloc_size)
				goto unpack_error;
		}

		safe_unpackstr(&cred->selinux_context, buffer);
		cred_len = get_buf_offset(buffer) - cred_start;
		/* signature must come after the end of the signed portion */
		safe_unpackstr(&credential->signature, buffer);
	} else {
		error("slurm_cred_unpack: protocol_version"
		      " %hu not supported", protocol_version);
		goto unpack_error;
	}

	/*
	 * Both srun and slurmd will unpack the credential just to pack it
	 * again. Hold onto a buffer with the pre-packed representation.
	 */
	if (!running_in_slurmstepd()) {
		credential->buffer = init_buf(cred_len);
		credential->buf_version = protocol_version;
		memcpy(credential->buffer->head,
		       get_buf_data(buffer) + cred_start,
		       cred_len);
		credential->buffer->processed = cred_len;
	}

	/*
	 * Using the saved position, verify the credential.
	 * This avoids needing to re-pack the entire thing just to
	 * cross-check that the signature matches up later.
	 * (Only done in slurmd.)
	 */
	if (credential->signature && running_in_slurmd())
		_cred_verify_signature(credential);

	return credential;

unpack_error:
	xfree(bit_fmt_str);
	slurm_cred_destroy(credential);
	return NULL;
}

static slurm_cred_t *_slurm_cred_alloc(bool alloc_arg)
{
	slurm_cred_t *cred = xmalloc(sizeof(*cred));
	/* Contents initialized to zero */

	slurm_rwlock_init(&cred->mutex);

	if (alloc_arg) {
		cred->arg = xmalloc(sizeof(slurm_cred_arg_t));
		cred->arg->uid = SLURM_AUTH_NOBODY;
		cred->arg->gid = SLURM_AUTH_NOBODY;
	}

	cred->verified = false;

	cred->magic = CRED_MAGIC;

	return cred;
}

static int _cred_sign(slurm_cred_t *cred)
{
	int rc;

	rc = (*(ops.cred_sign))(get_buf_data(cred->buffer),
				get_buf_offset(cred->buffer),
				&cred->signature);

	if (rc) {
		error("Credential sign: %s",
		      (*(ops.cred_str_error))(rc));
		return SLURM_ERROR;
	}
	return SLURM_SUCCESS;
}

static void _cred_verify_signature(slurm_cred_t *cred)
{
	int rc;
	void *start = get_buf_data(cred->buffer);
	uint32_t len = get_buf_offset(cred->buffer);

	rc = (*(ops.cred_verify_sign))(start, len, cred->signature);

	if (rc) {
		error("Credential signature check: %s",
		      (*(ops.cred_str_error))(rc));
		return;
	}

	cred->verified = true;
}


static void _pack_cred(slurm_cred_arg_t *cred, buf_t *buffer,
		       uint16_t protocol_version)
{
	uint32_t tot_core_cnt = 0;
	/*
	 * The gr_names array is optional. If the array exists the length
	 * must match that of the gids array.
	 */
	uint32_t gr_names_cnt = (cred->gr_names) ? cred->ngids : 0;
	time_t ctime = time(NULL);

	if (protocol_version >= SLURM_23_11_PROTOCOL_VERSION) {
		pack_step_id(&cred->step_id, buffer, protocol_version);
		pack32(cred->uid, buffer);
		pack32(cred->gid, buffer);
		packstr(cred->pw_name, buffer);
		packstr(cred->pw_gecos, buffer);
		packstr(cred->pw_dir, buffer);
		packstr(cred->pw_shell, buffer);
		pack32_array(cred->gids, cred->ngids, buffer);
		packstr_array(cred->gr_names, gr_names_cnt, buffer);

		(void) gres_job_state_pack(cred->job_gres_list, buffer,
					   cred->step_id.job_id, false,
					   protocol_version);
		gres_step_state_pack(cred->step_gres_list, buffer,
				     &cred->step_id, protocol_version);
		pack16(cred->job_core_spec, buffer);
		packstr(cred->job_account, buffer);
		packstr(cred->job_alias_list, buffer);
		packstr(cred->job_comment, buffer);
		packstr(cred->job_constraints, buffer);
		pack_time(cred->job_end_time, buffer);
		packstr(cred->job_extra, buffer);
		pack16(cred->job_oversubscribe, buffer);
		packstr(cred->job_partition, buffer);
		packstr(cred->job_reservation, buffer);
		pack16(cred->job_restart_cnt, buffer);
		pack_time(cred->job_start_time, buffer);
		packstr(cred->job_std_err, buffer);
		packstr(cred->job_std_in, buffer);
		packstr(cred->job_std_out, buffer);
		packstr(cred->step_hostlist, buffer);
		pack16(cred->x11, buffer);
		pack_time(ctime, buffer);

		if (cred->job_core_bitmap)
			tot_core_cnt = bit_size(cred->job_core_bitmap);
		pack32(tot_core_cnt, buffer);
		pack_bit_str_hex(cred->job_core_bitmap, buffer);
		pack_bit_str_hex(cred->step_core_bitmap, buffer);
		pack16(cred->core_array_size, buffer);
		if (cred->core_array_size) {
			pack16_array(cred->cores_per_socket,
				     cred->core_array_size,
				     buffer);
			pack16_array(cred->sockets_per_node,
				     cred->core_array_size,
				     buffer);
			pack32_array(cred->sock_core_rep_count,
				     cred->core_array_size,
				     buffer);
		}
		pack32(cred->cpu_array_count, buffer);
		if (cred->cpu_array_count) {
			pack16_array(cred->cpu_array,
				     cred->cpu_array_count,
				     buffer);
			pack32_array(cred->cpu_array_reps,
				     cred->cpu_array_count,
				     buffer);
		}
		pack32(cred->job_nhosts, buffer);
		pack32(cred->job_ntasks, buffer);
		packstr(cred->job_hostlist, buffer);
		packstr(cred->job_licenses, buffer);
		pack32(cred->job_mem_alloc_size, buffer);
		if (cred->job_mem_alloc_size) {
			pack64_array(cred->job_mem_alloc,
				     cred->job_mem_alloc_size,
				     buffer);
			pack32_array(cred->job_mem_alloc_rep_count,
				     cred->job_mem_alloc_size,
				     buffer);
		}
		pack32(cred->step_mem_alloc_size, buffer);
		if (cred->step_mem_alloc_size) {
			pack64_array(cred->step_mem_alloc,
				     cred->step_mem_alloc_size,
				     buffer);
			pack32_array(cred->step_mem_alloc_rep_count,
				     cred->step_mem_alloc_size,
				     buffer);
		}
		packstr(cred->selinux_context, buffer);
	} else if (protocol_version >= SLURM_23_02_PROTOCOL_VERSION) {
		pack_step_id(&cred->step_id, buffer, protocol_version);
		pack32(cred->uid, buffer);
		pack32(cred->gid, buffer);
		packstr(cred->pw_name, buffer);
		packstr(cred->pw_gecos, buffer);
		packstr(cred->pw_dir, buffer);
		packstr(cred->pw_shell, buffer);
		pack32_array(cred->gids, cred->ngids, buffer);
		packstr_array(cred->gr_names, gr_names_cnt, buffer);

		(void) gres_job_state_pack(cred->job_gres_list, buffer,
					   cred->step_id.job_id, false,
					   protocol_version);
		gres_step_state_pack(cred->step_gres_list, buffer,
				     &cred->step_id, protocol_version);
		pack16(cred->job_core_spec, buffer);
		packstr(cred->job_account, buffer);
		packstr(cred->job_alias_list, buffer);
		packstr(cred->job_comment, buffer);
		packstr(cred->job_constraints, buffer);
		pack_time(cred->job_end_time, buffer);
		packstr(cred->job_extra, buffer);
		pack16(cred->job_oversubscribe, buffer);
		packstr(cred->job_partition, buffer);
		packstr(cred->job_reservation, buffer);
		pack16(cred->job_restart_cnt, buffer);
		pack_time(cred->job_start_time, buffer);
		packstr(cred->job_std_err, buffer);
		packstr(cred->job_std_in, buffer);
		packstr(cred->job_std_out, buffer);
		packstr(cred->step_hostlist, buffer);
		pack16(cred->x11, buffer);
		pack_time(ctime, buffer);

		if (cred->job_core_bitmap)
			tot_core_cnt = bit_size(cred->job_core_bitmap);
		pack32(tot_core_cnt, buffer);
		pack_bit_str_hex(cred->job_core_bitmap, buffer);
		pack_bit_str_hex(cred->step_core_bitmap, buffer);
		pack16(cred->core_array_size, buffer);
		if (cred->core_array_size) {
			pack16_array(cred->cores_per_socket,
				     cred->core_array_size,
				     buffer);
			pack16_array(cred->sockets_per_node,
				     cred->core_array_size,
				     buffer);
			pack32_array(cred->sock_core_rep_count,
				     cred->core_array_size,
				     buffer);
		}
		pack32(cred->cpu_array_count, buffer);
		if (cred->cpu_array_count) {
			pack16_array(cred->cpu_array,
				     cred->cpu_array_count,
				     buffer);
			pack32_array(cred->cpu_array_reps,
				     cred->cpu_array_count,
				     buffer);
		}
		pack32(cred->job_nhosts, buffer);
		pack32(cred->job_ntasks, buffer);
		packstr(cred->job_hostlist, buffer);
		packstr(cred->job_licenses, buffer);
		pack32(cred->job_mem_alloc_size, buffer);
		if (cred->job_mem_alloc_size) {
			pack64_array(cred->job_mem_alloc,
				     cred->job_mem_alloc_size,
				     buffer);
			pack32_array(cred->job_mem_alloc_rep_count,
				     cred->job_mem_alloc_size,
				     buffer);
		}
		pack32(cred->step_mem_alloc_size, buffer);
		if (cred->step_mem_alloc_size) {
			pack64_array(cred->step_mem_alloc,
				     cred->step_mem_alloc_size,
				     buffer);
			pack32_array(cred->step_mem_alloc_rep_count,
				     cred->step_mem_alloc_size,
				     buffer);
		}
		packstr(cred->selinux_context, buffer);
	} else if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
		pack_step_id(&cred->step_id, buffer, protocol_version);
		pack32(cred->uid, buffer);
		pack32(cred->gid, buffer);
		packstr(cred->pw_name, buffer);
		packstr(cred->pw_gecos, buffer);
		packstr(cred->pw_dir, buffer);
		packstr(cred->pw_shell, buffer);
		pack32_array(cred->gids, cred->ngids, buffer);
		packstr_array(cred->gr_names, gr_names_cnt, buffer);

		(void) gres_job_state_pack(cred->job_gres_list, buffer,
					   cred->step_id.job_id, false,
					   protocol_version);
		gres_step_state_pack(cred->step_gres_list, buffer,
				     &cred->step_id, protocol_version);
		pack16(cred->job_core_spec, buffer);
		packstr(cred->job_account, buffer);
		packstr(cred->job_alias_list, buffer);
		packstr(cred->job_comment, buffer);
		packstr(cred->job_constraints, buffer);
		packstr(cred->job_partition, buffer);
		packstr(cred->job_reservation, buffer);
		pack16(cred->job_restart_cnt, buffer);
		packstr(cred->job_std_err, buffer);
		packstr(cred->job_std_in, buffer);
		packstr(cred->job_std_out, buffer);
		packstr(cred->step_hostlist, buffer);
		pack16(cred->x11, buffer);
		pack_time(ctime, buffer);

		if (cred->job_core_bitmap)
			tot_core_cnt = bit_size(cred->job_core_bitmap);
		pack32(tot_core_cnt, buffer);
		pack_bit_str_hex(cred->job_core_bitmap, buffer);
		pack_bit_str_hex(cred->step_core_bitmap, buffer);
		pack16(cred->core_array_size, buffer);
		if (cred->core_array_size) {
			pack16_array(cred->cores_per_socket,
				     cred->core_array_size,
				     buffer);
			pack16_array(cred->sockets_per_node,
				     cred->core_array_size,
				     buffer);
			pack32_array(cred->sock_core_rep_count,
				     cred->core_array_size,
				     buffer);
		}
		pack32(cred->cpu_array_count, buffer);
		if (cred->cpu_array_count) {
			pack16_array(cred->cpu_array,
				     cred->cpu_array_count,
				     buffer);
			pack32_array(cred->cpu_array_reps,
				     cred->cpu_array_count,
				     buffer);
		}
		pack32(cred->job_nhosts, buffer);
		pack32(cred->job_ntasks, buffer);
		packstr(cred->job_hostlist, buffer);
		pack32(cred->job_mem_alloc_size, buffer);
		if (cred->job_mem_alloc_size) {
			pack64_array(cred->job_mem_alloc,
				     cred->job_mem_alloc_size,
				     buffer);
			pack32_array(cred->job_mem_alloc_rep_count,
				     cred->job_mem_alloc_size,
				     buffer);
		}
		pack32(cred->step_mem_alloc_size, buffer);
		if (cred->step_mem_alloc_size) {
			pack64_array(cred->step_mem_alloc,
				     cred->step_mem_alloc_size,
				     buffer);
			pack32_array(cred->step_mem_alloc_rep_count,
				     cred->step_mem_alloc_size,
				     buffer);
		}
		packstr(cred->selinux_context, buffer);
	}
}

/*****************************************************************************\
 *****************       SBCAST CREDENTIAL FUNCTIONS        ******************
\*****************************************************************************/

/* Pack sbcast credential without the digital signature */
static void _pack_sbcast_cred(sbcast_cred_t *sbcast_cred, buf_t *buffer,
			      uint16_t protocol_version)
{
	if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
		pack_time(sbcast_cred->ctime, buffer);
		pack_time(sbcast_cred->expiration, buffer);
		pack32(sbcast_cred->jobid, buffer);
		pack32(sbcast_cred->het_job_id, buffer);
		pack32(sbcast_cred->step_id, buffer);
		pack32(sbcast_cred->uid, buffer);
		pack32(sbcast_cred->gid, buffer);
		packstr(sbcast_cred->user_name, buffer);
		pack32_array(sbcast_cred->gids, sbcast_cred->ngids, buffer);
		packstr(sbcast_cred->nodes, buffer);
	}
}

/* Create an sbcast credential for the specified job and nodes
 *	including digital signature.
 * RET the sbcast credential or NULL on error */
extern sbcast_cred_t *create_sbcast_cred(sbcast_cred_arg_t *arg,
					 uint16_t protocol_version)
{
	buf_t *buffer;
	int rc;
	sbcast_cred_t *sbcast_cred;

	xassert(g_context);

	sbcast_cred = xmalloc(sizeof(struct sbcast_cred));
	sbcast_cred->ctime = time(NULL);
	sbcast_cred->expiration = arg->expiration;
	sbcast_cred->jobid = arg->job_id;
	sbcast_cred->het_job_id = arg->het_job_id;
	sbcast_cred->step_id = arg->step_id;
	sbcast_cred->uid = arg->uid;
	sbcast_cred->gid = arg->gid;
	sbcast_cred->user_name = xstrdup(arg->user_name);
	sbcast_cred->ngids = arg->ngids;
	sbcast_cred->gids = copy_gids(arg->ngids, arg->gids);
	sbcast_cred->nodes = xstrdup(arg->nodes);

	if (enable_send_gids) {
		/* this may still be null, in which case slurmd will handle */
		sbcast_cred->user_name = uid_to_string_or_null(arg->uid);
		/* lookup and send extended gids list */
		sbcast_cred->ngids = group_cache_lookup(arg->uid, arg->gid,
							sbcast_cred->user_name,
							&sbcast_cred->gids);
	}

	buffer = init_buf(4096);
	_pack_sbcast_cred(sbcast_cred, buffer, protocol_version);
	rc = (*(ops.cred_sign))(get_buf_data(buffer), get_buf_offset(buffer),
				&sbcast_cred->signature);
	FREE_NULL_BUFFER(buffer);

	if (rc) {
		error("sbcast_cred sign: %s",
		      (*(ops.cred_str_error))(rc));
		delete_sbcast_cred(sbcast_cred);
		return NULL;
	}

	return sbcast_cred;
}

/* Delete an sbcast credential created using create_sbcast_cred() or
 *	unpack_sbcast_cred() */
extern void delete_sbcast_cred(sbcast_cred_t *sbcast_cred)
{
	if (!sbcast_cred)
		return;

	xfree(sbcast_cred->gids);
	xfree(sbcast_cred->user_name);
	xfree(sbcast_cred->nodes);
	xfree(sbcast_cred->signature);
	xfree(sbcast_cred);
}

static uint32_t _sbcast_cache_hash(char *signature)
{
	uint32_t hash = 0;
	int len = strlen(signature);

	/* Using two bytes at a time gives us a larger number
	 * and reduces the possibility of a duplicate value */
	for (int i = 0; i < len; i += 2) {
		hash += (signature[i] << 8) + signature[i + 1];
	}

	return hash;
}

static void _sbcast_cache_add(sbcast_cred_t *sbcast_cred)
{
	struct sbcast_cache *new_cache_rec;

	new_cache_rec = xmalloc(sizeof(struct sbcast_cache));
	new_cache_rec->expire = sbcast_cred->expiration;
	new_cache_rec->value = _sbcast_cache_hash(sbcast_cred->signature);
	list_append(sbcast_cache_list, new_cache_rec);
}

/* Extract contents of an sbcast credential verifying the digital signature.
 * NOTE: We can only perform the full credential validation once with
 *	Munge without generating a credential replay error, so we only
 *	verify the credential for block one of the executable file. All other
 *	blocks or shared object files must have a recent signature on file
 *	(in our cache) or the slurmd must have recently been restarted.
 * RET 0 on success, -1 on error */
extern sbcast_cred_arg_t *extract_sbcast_cred(sbcast_cred_t *sbcast_cred,
					      uint16_t block_no, uint16_t flags,
					      uint16_t protocol_version)
{
	sbcast_cred_arg_t *arg;
	struct sbcast_cache *next_cache_rec;
	int rc;
	time_t now = time(NULL);
	buf_t *buffer;

	xassert(g_context);

	if (now > sbcast_cred->expiration)
		return NULL;

	if (block_no == 1 && !(flags & FILE_BCAST_SO)) {
		buffer = init_buf(4096);
		_pack_sbcast_cred(sbcast_cred, buffer, protocol_version);
		/* NOTE: the verification checks that the credential was
		 * created by SlurmUser or root */
		rc = (*(ops.cred_verify_sign))(get_buf_data(buffer),
					       get_buf_offset(buffer),
					       sbcast_cred->signature);
		FREE_NULL_BUFFER(buffer);

		if (rc) {
			error("sbcast_cred verify: %s",
			      (*(ops.cred_str_error))(rc));
			return NULL;
		}
		_sbcast_cache_add(sbcast_cred);

	} else {
		char *err_str = NULL;
		bool cache_match_found = false;
		list_itr_t *sbcast_iter;
		uint32_t sig_num = _sbcast_cache_hash(sbcast_cred->signature);

		sbcast_iter = list_iterator_create(sbcast_cache_list);
		while ((next_cache_rec = list_next(sbcast_iter))) {
			if ((next_cache_rec->expire == sbcast_cred->expiration) &&
			    (next_cache_rec->value  == sig_num)) {
				cache_match_found = true;
				break;
			}
			if (next_cache_rec->expire <= now)
				list_delete_item(sbcast_iter);
		}
		list_iterator_destroy(sbcast_iter);

		if (!cache_match_found) {
			error("sbcast_cred verify: signature not in cache");
			if ((now - cred_restart_time) > 60)
				return NULL;	/* restarted >60 secs ago */
			buffer = init_buf(4096);
			_pack_sbcast_cred(sbcast_cred, buffer,
					  protocol_version);
			rc = (*(ops.cred_verify_sign))(get_buf_data(buffer),
						       get_buf_offset(buffer),
						       sbcast_cred->signature);
			FREE_NULL_BUFFER(buffer);
			if (rc)
				err_str = (char *)(*(ops.cred_str_error))(rc);
			if (err_str && xstrcmp(err_str, "Credential replayed")){
				error("sbcast_cred verify: %s", err_str);
				return NULL;
			}
			info("sbcast_cred verify: signature revalidated");
			_sbcast_cache_add(sbcast_cred);
		}
	}

	if (sbcast_cred->uid == SLURM_AUTH_NOBODY) {
		error("%s: refusing to create bcast credential for invalid user nobody",
		      __func__);
		return NULL;
	}

	if (sbcast_cred->gid == SLURM_AUTH_NOBODY) {
		error("%s: refusing to create bcast credential for invalid group nobody",
		      __func__);
		return NULL;
	}

	arg = xmalloc(sizeof(sbcast_cred_arg_t));
	arg->job_id = sbcast_cred->jobid;
	arg->step_id = sbcast_cred->step_id;
	arg->uid = sbcast_cred->uid;
	arg->gid = sbcast_cred->gid;
	arg->user_name = xstrdup(sbcast_cred->user_name);
	arg->ngids = sbcast_cred->ngids;
	arg->gids = copy_gids(sbcast_cred->ngids, sbcast_cred->gids);
	arg->nodes = xstrdup(sbcast_cred->nodes);
	return arg;
}

/* Pack an sbcast credential into a buffer including the digital signature */
extern void pack_sbcast_cred(sbcast_cred_t *sbcast_cred, buf_t *buffer,
			     uint16_t protocol_version)
{
	xassert(sbcast_cred);

	_pack_sbcast_cred(sbcast_cred, buffer, protocol_version);
	packstr(sbcast_cred->signature, buffer);
}

/* Unpack an sbcast credential into a buffer including the digital signature */
extern sbcast_cred_t *unpack_sbcast_cred(buf_t *buffer,
					 uint16_t protocol_version)
{
	sbcast_cred_t *sbcast_cred = xmalloc(sizeof(*sbcast_cred));

	if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
		safe_unpack_time(&sbcast_cred->ctime, buffer);
		safe_unpack_time(&sbcast_cred->expiration, buffer);
		safe_unpack32(&sbcast_cred->jobid, buffer);
		safe_unpack32(&sbcast_cred->het_job_id, buffer);
		safe_unpack32(&sbcast_cred->step_id, buffer);
		safe_unpack32(&sbcast_cred->uid, buffer);
		safe_unpack32(&sbcast_cred->gid, buffer);
		safe_unpackstr(&sbcast_cred->user_name, buffer);
		safe_unpack32_array(&sbcast_cred->gids, &sbcast_cred->ngids,
				    buffer);
		safe_unpackstr(&sbcast_cred->nodes, buffer);

		/* "signature" must be last */
		safe_unpackstr(&sbcast_cred->signature, buffer);
		if (!sbcast_cred->signature)
			goto unpack_error;
	} else
		goto unpack_error;

	return sbcast_cred;

unpack_error:
	delete_sbcast_cred(sbcast_cred);
	return NULL;
}

extern void print_sbcast_cred(sbcast_cred_t *sbcast_cred)
{
	info("Sbcast_cred: JobId   %u", sbcast_cred->jobid);
	info("Sbcast_cred: StepId  %u", sbcast_cred->step_id);
	info("Sbcast_cred: Nodes   %s", sbcast_cred->nodes);
	info("Sbcast_cred: ctime   %s", slurm_ctime2(&sbcast_cred->ctime));
	info("Sbcast_cred: Expire  %s", slurm_ctime2(&sbcast_cred->expiration));
}

extern void sbcast_cred_arg_free(sbcast_cred_arg_t *arg)
{
	if (!arg)
		return;

	xfree(arg->gids);
	xfree(arg->nodes);
	xfree(arg->user_name);
	xfree(arg);
}
