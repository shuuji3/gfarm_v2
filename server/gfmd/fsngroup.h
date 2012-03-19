/*
 * $Id$
 */

/*
 * Exported data types:
 */
struct gfarm_fsngroup_tuples_record;
typedef struct gfarm_fsngroup_tuples_record *gfarm_fsngroup_tuples_t;
struct gfarm_fsngroup_text_record;
typedef struct gfarm_fsngroup_text_record *gfarm_fsngroup_text_t;

/*
 * Exported APIs:
 */
size_t gfm_fsngroup_tuples_size(gfarm_fsngroup_tuples_t);
const char *gfm_fsngroup_tuples_hostname(gfarm_fsngroup_tuples_t, size_t);
const char *gfm_fsngroup_tuples_fsngroup(gfarm_fsngroup_tuples_t, size_t);
void gfm_fsngroup_tuples_destroy(gfarm_fsngroup_tuples_t);

size_t gfm_fsngroup_text_size(gfarm_fsngroup_text_t);
const char *gfm_fsngroup_text_line(gfarm_fsngroup_text_t, size_t);
void gfm_fsngroup_text_destroy(gfarm_fsngroup_text_t);

gfarm_fsngroup_tuples_t gfm_fsngroup_get_tuples_all_unlock(int);
gfarm_fsngroup_tuples_t gfm_fsngroup_get_tuples_all(int);

gfarm_fsngroup_tuples_t gfm_fsngroup_get_tuples_by_hostnames_unlock(
	const char **, size_t, int);
gfarm_fsngroup_tuples_t gfm_fsngroup_get_tuples_by_hostnames(
	const char **, size_t, int);

gfarm_fsngroup_tuples_t gfm_fsngroup_get_tuples_by_fsngroups_unlock(
	const char **, size_t, int);
gfarm_fsngroup_tuples_t gfm_fsngroup_get_tuples_by_fsngroups(
	const char **, size_t, int);

gfarm_fsngroup_text_t gfm_fsngroup_get_hostnames_by_fsngroup_unlock(
	const char *, int);
gfarm_fsngroup_text_t gfm_fsngroup_get_hostnames_by_fsngroup(
	const char *, int);

/*
 * Server side RPC stubs:
 */
gfarm_error_t gfm_server_fsngroup_get_all(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
gfarm_error_t gfm_server_fsngroup_get_by_hostname(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
gfarm_error_t gfm_server_fsngroup_modify(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
