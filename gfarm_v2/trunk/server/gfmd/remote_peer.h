struct peer *remote_peer_to_peer(struct remote_peer *);

gfarm_error_t remote_peer_alloc(struct peer *, gfarm_int64_t,
	gfarm_int32_t, char *, char *, int, int, int);
void remote_peer_free_simply(struct remote_peer *);
gfarm_error_t remote_peer_free_by_id(struct peer *, gfarm_int64_t);
void remote_peer_for_each_sibling(struct remote_peer *,
	void (*)(struct remote_peer *));
struct remote_peer *remote_peer_id_lookup_from_siblings(struct remote_peer *,
	gfarm_int64_t);
