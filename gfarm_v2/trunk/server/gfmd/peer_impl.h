struct peer;
struct local_peer;
struct remote_peer;
struct mdhost;

struct peer_ops {
	/* downcast functions */
	struct local_peer *(*peer_to_local_peer)(struct peer *);
	struct remote_peer *(*peer_to_remote_peer)(struct peer *);

	struct gfp_xdr *(*get_conn)(struct peer *);
	gfp_xdr_async_peer_t (*get_async)(struct peer *);
	gfarm_error_t (*get_port)(struct peer *, int *);
	struct mdhost *(*get_mdhost)(struct peer *);
	struct peer *(*get_parent)(struct peer *);
	int (*is_busy)(struct peer *);
	void (*notice_disconnected)(struct peer *, const char *, const char *);
	void (*shutdown)(struct peer *);
	void (*free)(struct peer *);
};

struct pending_new_generation_by_cookie {
	struct inode *inode;
	gfarm_uint64_t id;
	GFARM_HCIRCLEQ_ENTRY(pending_new_generation_by_cookie) cookie_link;
};

struct peer {
	struct peer_ops *ops;

	struct peer *next_close;
	int refcount, free_requested;

	enum gfarm_auth_id_type id_type;
	char *username, *hostname;
	struct user *user;
	struct abstract_host *host;

	struct process *process;
	int protocol_error;
	pthread_mutex_t protocol_error_mutex;

	struct protocol_state pstate;

	gfarm_int32_t fd_current, fd_saved;
	int flags;
#define PEER_FLAGS_FD_CURRENT_EXTERNALIZED	0x1
#define PEER_FLAGS_FD_SAVED_EXTERNALIZED	0x2
#define PEER_FLAGS_REMOTE_PEER_ALLOCATED	0x4 /* for local_peer */

	struct inum_path_array *findxmlattrctx;

	/* only one pending GFM_PROTO_GENERATION_UPDATED per peer is allowed */
	struct inode *pending_new_generation;
	/* GFM_PROTO_GENERATION_UPDATED_BY_COOKIE */
	GFARM_HCIRCLEQ_HEAD(pending_new_generation_by_cookie)
	     pending_new_generation_cookies;

	union {
		struct {
			/* only used by "gfrun" client */
			struct job_table_entry *jobs;
		} client;
	} u;

	/* the followings are only used for gfsd back channel */
	pthread_mutex_t replication_mutex;
	int simultaneous_replication_receivers;
	struct file_replicating replicating_inodes; /* dummy header */

	/*
	 * to support remote peer
	 */
	gfarm_int64_t peer_id;
};

void peer_construct_common(struct peer *, struct peer_ops *ops, const char *);
void peer_clear_common(struct peer *);
void peer_free_common(struct peer *, const char *);
void peer_closer_wakeup(struct peer *);
