/* need #include <gfarm/gfarm_config.h> to see HAVE_GETLOADAVG */

#ifndef HAVE_GETLOADAVG
int getloadavg(double *, int);
#endif

int gfsd_statfs(char *, gfarm_int32_t *,
	gfarm_off_t *, gfarm_off_t *, gfarm_off_t *,
	gfarm_off_t *, gfarm_off_t *, gfarm_off_t *);

gfarm_error_t gfsd_spool_check(int);

#define fatal_metadb_proto(msg_no, diag, proto, e) \
	fatal_metadb_proto_full(msg_no, __FILE__, __LINE__, __func__, \
	    diag, proto, e)

void fatal_metadb_proto_full(int,
	const char *, int, const char *,
	const char *, const char *, gfarm_error_t);

