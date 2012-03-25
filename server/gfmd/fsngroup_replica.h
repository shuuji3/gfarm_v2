/*
 * $Id$
 */
struct process;
struct inode;
struct host;
struct file_copy;

typedef enum {
	FIND_UNKNOWN = 0,
	FIND_NCOPY_ONLY,
	FIND_REPLICAINFO_ONLY,
	FIND_NEAREST
} gfarm_replication_attribute_serach_t;

void gfarm_server_process_record_replication_attribute(
	struct process *, int,
	struct inode *, struct inode *,
	gfarm_replication_attribute_serach_t);
void gfarm_server_fsngroup_replicate_file(
	struct inode *, struct host *, char *, struct file_copy *);
