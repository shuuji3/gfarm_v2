struct local_peer;
struct remote_peer;
struct peer;
struct gfp_xdr;
struct abstract_host;
struct peer_watcher;

struct peer *local_peer_to_peer(struct local_peer *);

gfarm_error_t local_peer_alloc(int, struct local_peer **);
gfarm_error_t local_peer_alloc_with_connection(struct gfp_xdr *,
	struct abstract_host *, int, struct peer **);
void local_peer_authorized(struct local_peer *,
	enum gfarm_auth_id_type, char *, char *, struct sockaddr *,
	enum gfarm_auth_method, struct peer_watcher *);

/* (struct gfp_xdr_aync_peer *) == gfp_xdr_async_peer_t XXX  */
struct gfp_xdr_async_peer;
void local_peer_set_async(struct local_peer *, struct gfp_xdr_async_peer *);
void local_peer_set_readable_watcher(struct local_peer *,
	struct peer_watcher *);
void local_peer_readable_invoked(struct local_peer *);
void local_peer_watch_readable(struct local_peer *);
void peer_set_readable_watcher(struct peer *, struct peer_watcher *);

void local_peer_shutdown_all(void);
struct remote_peer *local_peer_lookup_remote(struct local_peer *,
	gfarm_int64_t);

void local_peer_init(int);

/* only for remote_peer.c */
void local_peer_add_child(struct local_peer *,
	struct remote_peer *, struct remote_peer **);
void local_peer_get_numeric_name(struct local_peer *, char *, size_t);
void local_peer_for_child_peers(struct local_peer *,
	void (*)(struct remote_peer **, void *), void *, const char *);
