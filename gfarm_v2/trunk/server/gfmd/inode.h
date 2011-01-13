void inode_init(void);
void dir_entry_init(void);
void file_copy_init(void);
void symlink_init(void);
void xattr_init(void);

gfarm_uint64_t inode_total_num(void);

struct inode;

struct host;
struct user;
struct group;
struct process;
struct file_copy;
struct dead_file_copy;

void inode_for_each_file_copies(
	struct inode *,
	void (*)(struct inode *, struct file_copy *, void *),
	void *);
void inode_for_each_file_opening(
	struct inode *,
	void (*)(int, struct host *, void *),
	void *);

struct host *file_copy_host(struct file_copy *);
int file_copy_is_valid(struct file_copy *);

int inode_is_dir(struct inode *);
int inode_is_file(struct inode *);
int inode_is_symlink(struct inode *);
gfarm_ino_t inode_get_number(struct inode *);
gfarm_int64_t inode_get_gen(struct inode *);
gfarm_int64_t inode_get_nlink(struct inode *);
struct user *inode_get_user(struct inode *);
struct group *inode_get_group(struct inode *);
int inode_is_creating_file(struct inode *);
gfarm_int64_t inode_get_ncopy(struct inode *);
gfarm_int64_t inode_get_ncopy_with_dead_host(struct inode *);

gfarm_mode_t inode_get_mode(struct inode *);
gfarm_error_t inode_set_mode(struct inode *, gfarm_mode_t);
gfarm_off_t inode_get_size(struct inode *);
void inode_set_size(struct inode *, gfarm_off_t);
gfarm_error_t inode_set_owner(struct inode *, struct user *, struct group *);
struct gfarm_timespec *inode_get_atime(struct inode *);
struct gfarm_timespec *inode_get_mtime(struct inode *);
struct gfarm_timespec *inode_get_ctime(struct inode *);
void inode_set_atime(struct inode *, struct gfarm_timespec *);
void inode_set_mtime(struct inode *, struct gfarm_timespec *);
void inode_set_ctime(struct inode *, struct gfarm_timespec *);
void inode_accessed(struct inode *);
void inode_modified(struct inode *);
void inode_status_changed(struct inode *);
char *inode_get_symlink(struct inode *);
int inode_desired_dead_file_copy(gfarm_ino_t);

struct peer;
int inode_new_generation_is_pending(struct inode *);
gfarm_error_t inode_new_generation_wait_start(struct inode *, struct peer *);
gfarm_error_t inode_new_generation_done(struct inode *, struct peer *,
	gfarm_int32_t);
gfarm_error_t inode_new_generation_wait(struct inode *, struct peer *,
	gfarm_error_t (*)(struct peer *, void *, int *), void *);


gfarm_error_t inode_access(struct inode *, struct user *, int);

struct inode *inode_lookup(gfarm_ino_t);
void inode_lookup_all(void *, void (*callback)(void *, struct inode *));

gfarm_error_t inode_lookup_root(struct process *, int, struct inode **);
gfarm_error_t inode_lookup_parent(struct inode *, struct process *, int,
	struct inode **);
gfarm_error_t inode_lookup_by_name(struct inode *, char *,
	struct process *, int,
	struct inode **);
gfarm_error_t inode_create_file(struct inode *, char *,
	struct process *, int, gfarm_mode_t,
	struct inode **, int *);
gfarm_error_t inode_create_dir(struct inode *, char *,
	struct process *, gfarm_mode_t);
gfarm_error_t inode_create_symlink(struct inode *, char *,
	struct process *, char *);
gfarm_error_t inode_create_link(struct inode *, char *,
	struct process *, struct inode *);
gfarm_error_t inode_rename(struct inode *, char *, struct inode *, char *,
	struct process *);
gfarm_error_t inode_unlink(struct inode *, char *, struct process *);

void inode_dead_file_copy_added(gfarm_ino_t, gfarm_int64_t, struct host *);
gfarm_error_t inode_add_replica(struct inode *, struct host *, int);
void inode_remove_replica_completed(gfarm_ino_t, gfarm_int64_t, struct host *);
gfarm_error_t inode_remove_replica_metadata(struct inode *, struct host *,
	gfarm_int64_t);
gfarm_error_t inode_remove_replica_gen(struct inode *, struct host *,
	gfarm_int64_t, int);
gfarm_error_t inode_remove_replica(struct inode *, struct host *, int);
int inode_file_updated_on(struct inode *, struct host *,
	struct gfarm_timespec *, struct gfarm_timespec *);
int inode_is_updated(struct inode *, struct gfarm_timespec *);

struct file_opening;

gfarm_error_t inode_open(struct file_opening *);
void inode_close(struct file_opening *);
void inode_close_read(struct file_opening *, struct gfarm_timespec *);
int inode_file_update(struct file_opening *,
	gfarm_off_t, struct gfarm_timespec *, struct gfarm_timespec *,
	gfarm_int64_t *, gfarm_int64_t *);

gfarm_error_t inode_cksum_set(struct file_opening *,
	const char *, size_t, const char *,
	gfarm_int32_t, struct gfarm_timespec *);
gfarm_error_t inode_cksum_get(struct file_opening *,
	char **, size_t *, char **, gfarm_int32_t *);

int inode_has_replica(struct inode *, struct host *);
gfarm_error_t inode_getdirpath(struct inode *, struct process *, char **);
struct host *inode_schedule_host_for_read(struct inode *, struct host *);
struct host *inode_schedule_host_for_write(struct inode *, struct host *);
struct host *inode_writing_spool_host(struct inode *);
int inode_schedule_confirm_for_write(struct file_opening *, struct host *,
	int *);
struct peer;

/* this interface is made as a hook for a private extension */
extern gfarm_error_t (*inode_schedule_file)(struct file_opening *,
	struct peer *, gfarm_int32_t *, struct host ***);

struct file_replicating;
gfarm_error_t file_replicating_new(
	struct inode *, struct host *, struct dead_file_copy *,
	struct file_replicating **);
void file_replicating_free(struct file_replicating *);
gfarm_int64_t file_replicating_get_gen(struct file_replicating *);
gfarm_error_t inode_replicated(struct file_replicating *,
	gfarm_int32_t, gfarm_int32_t, gfarm_off_t);
gfarm_error_t inode_prepare_to_replicate(struct inode *, struct user *,
	struct host *, struct host *, gfarm_int32_t,
	struct file_replicating **);

gfarm_error_t inode_replica_list_by_name(struct inode *,
	gfarm_int32_t *, char ***);
gfarm_error_t inode_replica_info_get(struct inode *, gfarm_int32_t,
	gfarm_int32_t *, char ***, gfarm_int64_t **, gfarm_int32_t **);

gfarm_error_t inode_xattr_add(struct inode *, int, const char *,
	void *, size_t);
gfarm_error_t inode_xattr_modify(struct inode *, int, const char *,
	void *, size_t);
gfarm_error_t inode_xattr_get_cache(struct inode *, int, const char *,
	void **, size_t *);
int inode_xattr_has_xmlattrs(struct inode *);
gfarm_error_t inode_xattr_remove(struct inode *, int, const char *);
gfarm_error_t inode_xattr_list(struct inode *, int, char **, size_t *);

struct xattr_list {
	char *name;
	void *value;
	size_t size;
};
void inode_xattr_list_free(struct xattr_list *, size_t);
gfarm_error_t inode_xattr_list_get_cached_by_patterns(gfarm_ino_t,
	char **, int, struct xattr_list **, size_t *);

void inode_init_acl(void);

void inode_init_desired_number(void);
int inode_has_desired_number(struct inode *, int *);
int inode_traverse_desired_replica_number(struct inode *, int *);

/* check and repair */
void inode_nlink_check(void);

/* debug */
void dir_dump(gfarm_ino_t);
void rootdir_dump(void);


/* exported for a use from a private extension */
gfarm_error_t inode_schedule_file_default(struct file_opening *,
	struct peer *, gfarm_int32_t *, struct host ***);
