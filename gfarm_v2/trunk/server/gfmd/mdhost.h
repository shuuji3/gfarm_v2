/*
 * $Id$
 */

struct mdhost;
struct peer;
struct gfm_connection;
struct abstract_host;
struct gfarm_metadb_server;

struct abstract_host *mdhost_to_abstract_host(struct mdhost *);

void mdhost_init(void);
const char *mdhost_get_name(struct mdhost *);
int mdhost_get_port(struct mdhost *);
int mdhost_is_self(struct mdhost *);
struct mdhost *mdhost_lookup(const char *);
struct mdhost *mdhost_lookup_self(void);
void mdhost_foreach(int (*)(struct mdhost *, void *), void *);
int mdhost_self_is_master(void);
int mdhost_self_is_readonly(void);

struct peer;
gfarm_error_t gfm_server_metadb_server_get(struct peer *, int, int);
gfarm_error_t gfm_server_metadb_server_get_all(struct peer *, int, int);
gfarm_error_t gfm_server_metadb_server_set(struct peer *, int, int);
gfarm_error_t gfm_server_metadb_server_modify(struct peer *, int, int);
gfarm_error_t gfm_server_metadb_server_remove(struct peer *, int, int);

#ifdef ENABLE_METADATA_REPLICATION
struct journal_file_reader;
struct journal_file_reader *mdhost_get_journal_file_reader(struct mdhost *);
void mdhost_set_journal_file_reader(struct mdhost *,
	struct journal_file_reader *);
gfarm_uint64_t mdhost_get_last_fetch_seqnum(struct mdhost *);
void mdhost_set_last_fetch_seqnum(struct mdhost *, gfarm_uint64_t);
int mdhost_is_recieved_seqnum(struct mdhost *);
void mdhost_set_is_recieved_seqnum(struct mdhost *, int);
void mdhost_set_self_as_master(void);
int mdhost_is_sync_replication(struct mdhost *);
int mdhost_get_flags(struct mdhost *);
int mdhost_is_in_first_sync(struct mdhost *);
void mdhost_set_is_in_first_sync(struct mdhost *, int);
int mdhost_has_async_replication_target(void);
int mdhost_is_master(struct mdhost *);
void mdhost_set_is_master(struct mdhost *, int);
struct mdcluster *mdhost_get_cluster(struct mdhost *);
void mdhost_set_cluster(struct mdhost *, struct mdcluster *);
const char *mdhost_get_cluster_name(struct mdhost *);
void mdhost_activate(struct mdhost *);
struct peer *mdhost_get_peer(struct mdhost *);
void mdhost_set_peer(struct mdhost *, struct peer *, int);
struct gfm_connection *mdhost_get_connection(struct mdhost *);
void mdhost_set_connection(struct mdhost *, struct gfm_connection *);
int mdhost_is_up(struct mdhost *);
void mdhost_disconnect(struct mdhost *, struct peer *);
gfarm_error_t mdhost_enter(struct gfarm_metadb_server *, struct mdhost **);
gfarm_error_t mdhost_modify_in_cache(struct mdhost *,
	struct gfarm_metadb_server *);
gfarm_error_t mdhost_remove_in_cache(const char *);
struct mdhost *mdhost_lookup_master(void);
struct mdhost *mdhost_lookup_metadb_server(struct gfarm_metadb_server *);
int mdhost_get_count(void);
int mdhost_self_is_master_candidate(void);
#endif
