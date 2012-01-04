extern char *gfarm_config_file;

/* gfsd dependent */
/* GFS dependent */
extern char *gfarm_spool_server_listen_address;
extern char *gfarm_spool_root;

enum gfarm_backend_db_type {
	GFARM_BACKEND_DB_TYPE_UNKNOWN,
	GFARM_BACKEND_DB_TYPE_LDAP,
	GFARM_BACKEND_DB_TYPE_POSTGRESQL,
	GFARM_BACKEND_DB_TYPE_LOCALFS
};

extern enum gfarm_backend_db_type gfarm_backend_db_type;

/* GFM dependent */
extern int gfarm_gfmd_connection_cache;
/* XXX FIXME these should disappear to support multiple metadata server */
extern char *gfarm_metadb_server_name;
extern int gfarm_metadb_server_port;

extern char *gfarm_metadb_admin_user;
extern char *gfarm_metadb_admin_user_gsi_dn;

extern int gfarm_metadb_stack_size;
extern int gfarm_metadb_thread_pool_size;
extern int gfarm_metadb_job_queue_length;
extern int gfarm_metadb_heartbeat_interval;
extern int gfarm_metadb_dbq_size;
#define GFARM_METADB_STACK_SIZE_DEFAULT 0 /* use OS default */
#define GFARM_METADB_THREAD_POOL_SIZE_DEFAULT	16  /* quadcore, quadsocket */
#if 0
#define GFARM_METADB_JOB_QUEUE_LENGTH_DEFAULT	160 /* THREAD_POOL * 10 */
#else /* XXX FIXME: until bcworkq is implemented */
#define GFARM_METADB_JOB_QUEUE_LENGTH_DEFAULT	16000
#endif
#define GFARM_METADB_HEARTBEAT_INTERVAL_DEFAULT 180 /* 3 min */
#define GFARM_METADB_DBQ_SIZE_DEFAULT	65536
#define GFARM_SYMLINK_LEVEL_MAX			20

/* LDAP dependent */
extern char *gfarm_ldap_server_name;
extern char *gfarm_ldap_server_port;
extern char *gfarm_ldap_base_dn;
extern char *gfarm_ldap_bind_dn;
extern char *gfarm_ldap_bind_password;
extern char *gfarm_ldap_tls;
extern char *gfarm_ldap_tls_cipher_suite;
extern char *gfarm_ldap_tls_certificate_key_file;
extern char *gfarm_ldap_tls_certificate_file;

/* PostgreSQL dependent */
extern char *gfarm_postgresql_server_name;
extern char *gfarm_postgresql_server_port;
extern char *gfarm_postgresql_dbname;
extern char *gfarm_postgresql_user;
extern char *gfarm_postgresql_password;
extern char *gfarm_postgresql_conninfo;

/* LocalFS dependent */
extern char *gfarm_localfs_datadir;

/* miscellaneous configurations */
extern int gfarm_log_level; /* syslog priority level to log */
extern int gfarm_no_file_system_node_timeout;
extern int gfarm_gfmd_reconnection_timeout;
extern int gfarm_attr_cache_limit;
extern int gfarm_attr_cache_timeout;
extern int gfarm_schedule_cache_timeout;
extern float gfarm_schedule_idle_load;
extern float gfarm_schedule_busy_load;
extern float gfarm_schedule_virtual_load;
extern int gfarm_simultaneous_replication_receivers;
extern int gfarm_gfsd_connection_cache;
extern int gfarm_record_atime;
extern int gfarm_client_file_bufsize;

extern int gf_on_demand_replication;

int gfarm_schedule_write_local_priority(void);
char *gfarm_schedule_write_target_domain(void);
gfarm_off_t gfarm_get_minimum_free_disk_space(void);
int gfarm_get_metadb_replication_enabled(void);
void gfarm_set_metadb_replication_enabled(int);
const char *gfarm_get_journal_dir(void);
int gfarm_get_journal_max_size(void);
int gfarm_get_journal_recvq_size(void);
int gfarm_get_journal_sync_file(void);
int gfarm_get_journal_sync_slave_timeout(void);
int gfarm_get_metadb_server_slave_max_size(void);
int gfarm_get_metadb_server_force_slave(void);
void gfarm_set_metadb_server_force_slave(int);

/* miscellaneous */
extern int gfarm_network_receive_timeout;
extern int gfarm_file_trace;

void gfarm_config_clear(void);
#ifdef GFARM_USE_STDIO
gfarm_error_t gfarm_config_read_file(FILE *, int *);
#endif
gfarm_error_t gfarm_init_config(void);
gfarm_error_t gfarm_free_config(void);
void gfarm_config_set_default_ports(void);
void gfarm_config_set_default_misc(void);
void gfs_display_timers(void);

int gfarm_xattr_caching_patterns_number(void);
char **gfarm_xattr_caching_patterns(void);


/* for client */
struct gfs_connection;

struct gfm_connection;
gfarm_error_t gfarm_client_process_set(struct gfs_connection *,
	struct gfm_connection *);
gfarm_error_t gfarm_client_process_reset(struct gfs_connection *,
	struct gfm_connection *);
