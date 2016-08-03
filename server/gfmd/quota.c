/*
 * $Id$
 */

#include <pthread.h>	/* db_access.h currently needs this */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <assert.h>

#include <gfarm/gfarm.h>

#include "thrsubr.h"

#include "auth.h"
#include "quota_info.h"

#include "peer.h"
#include "subr.h"
#include "rpcsubr.h"
#include "user.h"
#include "group.h"
#include "inode.h"
#include "quota.h"
#include "dirset.h"
#include "db_access.h"

#define QUOTA_NOT_CHECK_YET -1
#define is_checked(q) ((q)->space != QUOTA_NOT_CHECK_YET)

static gfarm_error_t db_state = GFARM_ERR_NO_ERROR;

/* private functions */
static void
update_softlimit(gfarm_time_t *exceedp, gfarm_time_t now, gfarm_time_t grace,
		gfarm_int64_t val, gfarm_int64_t soft)
{
	if (!quota_limit_is_valid(grace) /* disable all softlimit */ ||
	    !quota_limit_is_valid(soft) /* disable this softlimit */ ||
	    val <= soft /* not exceed */
		) {
		*exceedp = GFARM_QUOTA_INVALID;
		return;
	} else if (*exceedp >= 0)
		return; /* already exceeded */
	else if (val > soft)
		*exceedp = now; /* exceed now */
}

static void
quota_softlimit_exceed(struct quota *q)
{
	struct timeval now;

	if (!quota_limit_is_valid(q->grace_period)) {
		/* disable all softlimit */
		q->space_exceed = GFARM_QUOTA_INVALID;
		q->num_exceed = GFARM_QUOTA_INVALID;
		q->phy_space_exceed = GFARM_QUOTA_INVALID;
		q->phy_num_exceed = GFARM_QUOTA_INVALID;
		return;
	}

	/* update exceeded time of softlimit */
	gettimeofday(&now, NULL);
	update_softlimit(&q->space_exceed, now.tv_sec, q->grace_period,
			 q->space, q->space_soft);
	update_softlimit(&q->num_exceed, now.tv_sec, q->grace_period,
			 q->num, q->num_soft);
	update_softlimit(&q->phy_space_exceed, now.tv_sec, q->grace_period,
			 q->phy_space, q->phy_space_soft);
	update_softlimit(&q->phy_num_exceed, now.tv_sec, q->grace_period,
			 q->phy_num, q->phy_num_soft);
}

static void
quota_metadata_softlimit_exceed(struct quota_metadata *q)
{
	struct timeval now;

	if (!quota_limit_is_valid(q->limit.grace_period)) {
		/* disable all softlimit */
		q->exceed.space_time = GFARM_QUOTA_INVALID;
		q->exceed.num_time = GFARM_QUOTA_INVALID;
		q->exceed.phy_space_time = GFARM_QUOTA_INVALID;
		q->exceed.phy_num_time = GFARM_QUOTA_INVALID;
		return;
	}

	/* update exceeded time of softlimit */
	gettimeofday(&now, NULL);
	update_softlimit(&q->exceed.space_time, now.tv_sec,
	    q->limit.grace_period, q->usage.space,
	    q->limit.soft.space);
	update_softlimit(&q->exceed.num_time, now.tv_sec,
	    q->limit.grace_period, q->usage.num,
	    q->limit.soft.num);
	update_softlimit(&q->exceed.phy_space_time, now.tv_sec,
	    q->limit.grace_period, q->usage.phy_space,
	    q->limit.soft.phy_space);
	update_softlimit(&q->exceed.phy_num_time, now.tv_sec,
	    q->limit.grace_period, q->usage.phy_num,
	    q->limit.soft.phy_num);
}

void
quota_usage_clear(struct gfarm_quota_subject_info *usage)
{
	usage->space = 0;
	usage->num = 0;
	usage->phy_space = 0;
	usage->phy_num = 0;
}

static void
usage_tmp_clear_user(void *closure, struct user *u)
{
	quota_usage_clear(user_usage_tmp(u));
}

static void
usage_tmp_clear_group(void *closure, struct group *g)
{
	quota_usage_clear(group_usage_tmp(g));
}

static void
usage_tmp_clear(void)
{
	user_all(NULL, usage_tmp_clear_user, 0);
	group_all(NULL, usage_tmp_clear_group, 0);
}

static void
usage_to_quota(struct gfarm_quota_subject_info *src_usage, struct quota *dst)
{
	dst->space = src_usage->space;
	dst->num = src_usage->num;
	dst->phy_space = src_usage->phy_space;
	dst->phy_num = src_usage->phy_num;
}

static void
quota_update_usage_user(void *closure, struct user *u)
{
	struct quota *q = user_quota(u);
	struct gfarm_quota_subject_info *usage_tmp = user_usage_tmp(u);

	usage_to_quota(usage_tmp, q);
	quota_softlimit_exceed(q);

	if (!user_is_valid(u))
		gflog_notice(GFARM_MSG_1004294,
		    "quota_check: removed user(%s), Usage: "
		    "space=%lld, inodes=%lld, phys_space=%lld, phys_num=%lld",
		    user_name_with_invalid(u),
		    (long long)q->space, (long long)q->num,
		    (long long)q->phy_space, (long long)q->phy_num);
}

static void
quota_update_usage_group(void *closure, struct group *g)
{
	struct quota *q = group_quota(g);
	struct gfarm_quota_subject_info *usage_tmp = group_usage_tmp(g);

	usage_to_quota(usage_tmp, q);
	quota_softlimit_exceed(q);

	if (!group_is_valid(g))
		gflog_notice(GFARM_MSG_1004295,
		    "quota_check: removed group(%s), Usage: "
		    "space=%lld, inodes=%lld, phys_space=%lld, phys_num=%lld",
		    group_name_with_invalid(g),
		    (long long)q->space, (long long)q->num,
		    (long long)q->phy_space, (long long)q->phy_num);
}

static void
quota_update_usage(void)
{
	user_all(NULL, quota_update_usage_user, 0);
	group_all(NULL, quota_update_usage_group, 0);
}

static gfarm_time_t
calculate_grace_period(gfarm_time_t exceeded_time,
		       gfarm_time_t grace_period,
		       gfarm_time_t now)
{
	gfarm_time_t val;

	if (!quota_limit_is_valid(grace_period) ||
	    !quota_limit_is_valid(exceeded_time))
		return (GFARM_QUOTA_INVALID); /* disable softlimit */

	val = grace_period - (now - exceeded_time);
	if (val < 0)
		return (0); /* expired */

	return (val); /* grace period until expiration */
}

void
quota_exceed_to_grace(gfarm_time_t grace_period,
	const struct gfarm_quota_subject_time *exceed,
	struct gfarm_quota_subject_time *grace)
{
	struct timeval now;

	gettimeofday(&now, NULL);

	grace->space_time = calculate_grace_period(
	    exceed->space_time, grace_period, now.tv_sec);
	grace->num_time = calculate_grace_period(
	    exceed->num_time, grace_period, now.tv_sec);
	grace->phy_space_time = calculate_grace_period(
	    exceed->phy_space_time, grace_period, now.tv_sec);
	grace->phy_num_time = calculate_grace_period(
	    exceed->phy_num_time, grace_period, now.tv_sec);
}

static void
quota_convert_1(const struct quota *q, const char *name,
		struct gfarm_quota_get_info *qi)
{
	struct timeval now;

	gettimeofday(&now, NULL);

	qi->name = (char *)name;
	qi->grace_period = q->grace_period;
	qi->space = q->space;
	qi->space_grace = calculate_grace_period(
		q->space_exceed, q->grace_period, now.tv_sec);
	qi->space_soft = q->space_soft;
	qi->space_hard = q->space_hard;
	qi->num = q->num;
	qi->num_grace = calculate_grace_period(
		q->num_exceed, q->grace_period, now.tv_sec);
	qi->num_soft = q->num_soft;
	qi->num_hard =  q->num_hard;
	qi->phy_space = q->phy_space;
	qi->phy_space_grace = calculate_grace_period(
		q->phy_space_exceed, q->grace_period, now.tv_sec);
	qi->phy_space_soft = q->phy_space_soft;
	qi->phy_space_hard = q->phy_space_hard;
	qi->phy_num = q->phy_num;
	qi->phy_num_grace = calculate_grace_period(
		q->phy_num_exceed, q->grace_period, now.tv_sec);
	qi->phy_num_soft = q->phy_num_soft;
	qi->phy_num_hard = q->phy_num_hard;
}

static void
quota_convert_2(const struct gfarm_quota_info *qi, struct quota *q)
{
	q->grace_period = qi->grace_period;
	q->space = qi->space;
	q->space_exceed = qi->space_exceed;
	q->space_soft = qi->space_soft;
	q->space_hard = qi->space_hard;
	q->num = qi->num;
	q->num_exceed = qi->num_exceed;
	q->num_soft = qi->num_soft;
	q->num_hard =  qi->num_hard;
	q->phy_space = qi->phy_space;
	q->phy_space_exceed = qi->phy_space_exceed;
	q->phy_space_soft = qi->phy_space_soft;
	q->phy_space_hard = qi->phy_space_hard;
	q->phy_num = qi->phy_num;
	q->phy_num_exceed = qi->phy_num_exceed;
	q->phy_num_soft = qi->phy_num_soft;
	q->phy_num_hard = qi->phy_num_hard;
}

static void
quota_user_remove_db(const char *name)
{
	gfarm_error_t e = db_quota_user_remove(name);

	if (e != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_1000412,
			    "db_quota_user_remove(%s) %s",
			    name, gfarm_error_string(e));
}

static void
quota_group_remove_db(const char *name)
{
	gfarm_error_t e = db_quota_group_remove(name);

	if (e != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_1000413,
			    "db_quota_group_remove(%s) %s",
			    name, gfarm_error_string(e));
}

/* public functions */
static void
quota_user_set_one_from_db(void *closure, struct gfarm_quota_info *qi)
{
	struct user *u;

	if (qi->name == NULL)
		return;
	u = user_lookup_including_invalid(qi->name);
	if (u == NULL) {
		quota_user_remove_db(qi->name);
	} else {
		struct quota *q = user_quota(u);
		quota_convert_2(qi, q);
		q->on_db = 1; /* load from db */
	}
	gfarm_quota_info_free(qi);
}

static void
quota_group_set_one_from_db(void *closure, struct gfarm_quota_info *qi)
{
	struct group *g;

	if (qi->name == NULL)
		return;
	g = group_lookup_including_invalid(qi->name);
	if (g == NULL) {
		quota_group_remove_db(qi->name);
	} else {
		struct quota *q = group_quota(g);
		quota_convert_2(qi, q);
		q->on_db = 1; /* load from db */
	}
	gfarm_quota_info_free(qi);
}

void
quota_init(void)
{
	gfarm_error_t e;

	e = db_quota_user_load(NULL, quota_user_set_one_from_db);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1000414,
			    "db_quota_user_load() %s", gfarm_error_string(e));
		db_state = e;
	}
	e = db_quota_group_load(NULL, quota_group_set_one_from_db);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1000415,
			    "db_quota_group_load() %s", gfarm_error_string(e));
		db_state = e;
	}
}

void
quota_data_init(struct quota *q)
{
	/* gfarmadm do not execute gfedquota yet */
	q->on_db = 0;
	/* disable all softlimit */
	q->grace_period = GFARM_QUOTA_INVALID;
	/* gfarmadm do not execute gfquotacheck yet */
	q->space = QUOTA_NOT_CHECK_YET;
	q->space_exceed = GFARM_QUOTA_INVALID;
	q->space_soft = GFARM_QUOTA_INVALID;
	q->space_hard = GFARM_QUOTA_INVALID;
	q->num = 0;
	q->num_exceed = GFARM_QUOTA_INVALID;
	q->num_soft = GFARM_QUOTA_INVALID;
	q->num_hard = GFARM_QUOTA_INVALID;
	q->phy_space = 0;
	q->phy_space_exceed = GFARM_QUOTA_INVALID;
	q->phy_space_soft = GFARM_QUOTA_INVALID;
	q->phy_space_hard = GFARM_QUOTA_INVALID;
	q->phy_num = 0;
	q->phy_num_exceed = GFARM_QUOTA_INVALID;
	q->phy_num_soft = GFARM_QUOTA_INVALID;
	q->phy_num_hard = GFARM_QUOTA_INVALID;
}

/* for soft and hard */
static void
quota_limit_subject_init(struct gfarm_quota_subject_info *limit)
{
	limit->space = GFARM_QUOTA_INVALID;
	limit->num = GFARM_QUOTA_INVALID;
	limit->phy_space = GFARM_QUOTA_INVALID;
	limit->phy_num = GFARM_QUOTA_INVALID;
}

/* for limit */
static void
quota_limit_init(struct gfarm_quota_limit_info *limit)
{
	limit->grace_period = GFARM_QUOTA_INVALID; /* disable all softlimit */
	quota_limit_subject_init(&limit->soft);
	quota_limit_subject_init(&limit->hard);
}

/* for usage */
void
quota_usage_init(struct gfarm_quota_subject_info *usage)
{
	usage->space = QUOTA_NOT_CHECK_YET; /* quota_check hasn't been done */
	usage->num = 0;
	usage->phy_space = 0;
	usage->phy_num = 0;
}

/* for exceed, grace */
void
quota_subject_time_init(struct gfarm_quota_subject_time *time)
{
	time->space_time = GFARM_QUOTA_INVALID;
	time->num_time = GFARM_QUOTA_INVALID;
	time->phy_space_time = GFARM_QUOTA_INVALID;
	time->phy_num_time = GFARM_QUOTA_INVALID;
}

void
quota_metadata_init(struct quota_metadata *q)
{
	quota_limit_init(&q->limit);
	quota_usage_init(&q->usage);
	quota_subject_time_init(&q->exceed);
}

int
quota_is_checked(const struct gfarm_quota_subject_info *usage)
{
	return (is_checked(usage));
}

static inline gfarm_int64_t
int64_add(gfarm_int64_t orig, gfarm_int64_t diff)
{
	gfarm_int64_t val;

	if (diff == 0)
		return (orig);
	else if (diff > 0) {
		val = orig + diff;
		/*
		 * signed overflow causes undefined behavior in the C language
		 * standard, thus we have to use unsigned here.
		 */
		if ((gfarm_uint64_t)val < orig ||
		    (gfarm_uint64_t)val > GFARM_INT64_MAX) /* overflow */
			val = GFARM_INT64_MAX;
		return (val);
	} else { /* diff < 0 */
		val = orig + diff;
		if (val < 0)
			val = 0;
		return (val);
	}
}

#define update_file_add(q, size, ncopy)					\
	{								\
		(q)->space = int64_add((q)->space, size);		\
		(q)->num = int64_add((q)->num, 1);			\
		(q)->phy_space = int64_add((q)->phy_space, size * ncopy); \
		(q)->phy_num = int64_add((q)->phy_num, ncopy);		\
	}


static void
usage_tmp_update(struct inode *inode)
{
	gfarm_off_t size;
	gfarm_int64_t ncopy;
	struct user *u = inode_get_user(inode);
	struct group *g = inode_get_group(inode);

	if (inode_is_file(inode)) {
		size = inode_get_size(inode);
		ncopy = inode_get_ncopy_with_dead_host(inode);
	} else {
		size = 0;
		ncopy = 0;
	}

	if (u)
		update_file_add(user_usage_tmp(u), size, ncopy);
	if (g)
		update_file_add(group_usage_tmp(g), size, ncopy);
}

static void quota_check_retry_if_running(void);

void
quota_update_file_add(struct inode *inode, struct dirset *tdirset)
{
	gfarm_off_t size;
	gfarm_int64_t ncopy;
	struct user *u = inode_get_user(inode);
	struct group *g = inode_get_group(inode);

	if (inode_is_file(inode)) {
		size = inode_get_size(inode);
		ncopy = inode_get_ncopy_with_dead_host(inode);
	} else {
		size = 0;
		ncopy = 0;
	}

	if (u) {
		struct quota *uq = user_quota(u);
		if (is_checked(uq)) {
			update_file_add(uq, size, ncopy);
			quota_softlimit_exceed(uq);
		}
	}
	if (g) {
		struct quota *gq = group_quota(g);
		if (is_checked(gq)) {
			update_file_add(gq, size, ncopy);
			quota_softlimit_exceed(gq);
		}
	}
	if (tdirset != TDIRSET_IS_UNKNOWN && tdirset != TDIRSET_IS_NOT_SET) {
		struct quota_metadata *dsq = dirset_quota(tdirset);
		if (is_checked(&dsq->usage)) {
			update_file_add(&dsq->usage, size, ncopy);
			quota_metadata_softlimit_exceed(dsq);
		}
	}

	if (debug_mode) {
		gfarm_ino_t inum;
		gfarm_int64_t gen;
		char *username, *groupname;
		inum = inode_get_number(inode);
		gen = inode_get_gen(inode);
		username = user_name(u);
		groupname = group_name(g);
		gflog_debug(GFARM_MSG_1000416,
			    "<quota_add> ino=%lld(gen=%lld): "
			    "size=%lld,ncopy=%lld,user=%s,group=%s",
			    (unsigned long long)inum, (unsigned long long)gen,
			    (unsigned long long)size, (unsigned long long)ncopy,
			    username, groupname);
	}

	quota_check_retry_if_running();
}

#define update_file_resize(q, old_size, new_size, ncopy)		\
	{								\
		gfarm_int64_t diff = new_size - old_size;		\
		(q)->space = int64_add((q)->space, diff);		\
		(q)->phy_space = int64_add((q)->phy_space, diff * ncopy); \
	}

void
quota_update_file_resize(struct inode *inode, struct dirset *tdirset,
	gfarm_off_t new_size)
{
	gfarm_off_t old_size = inode_get_size(inode);
	gfarm_int64_t ncopy = inode_get_ncopy_with_dead_host(inode);
	struct user *u = inode_get_user(inode);
	struct group *g = inode_get_group(inode);

	assert(inode_is_file(inode));

	if (u) {
		struct quota *uq = user_quota(u);
		if (is_checked(uq)) {
			update_file_resize(uq, old_size, new_size, ncopy);
			quota_softlimit_exceed(uq);
		}
	}
	if (g) {
		struct quota *gq = group_quota(g);
		if (is_checked(gq)) {
			update_file_resize(gq, old_size, new_size, ncopy);
			quota_softlimit_exceed(gq);
		}
	}
	if (tdirset != TDIRSET_IS_UNKNOWN && tdirset != TDIRSET_IS_NOT_SET) {
		struct quota_metadata *dsq = dirset_quota(tdirset);
		if (is_checked(&dsq->usage)) {
			update_file_resize(&dsq->usage,
			    old_size, new_size, ncopy);
			quota_metadata_softlimit_exceed(dsq);
		}
	}

	quota_check_retry_if_running();
}

#define update_replica_num(q, size, n)					\
	{								\
		(q)->phy_space = int64_add((q)->phy_space, size * n);	\
		(q)->phy_num = int64_add((q)->phy_num, n);		\
	}

static void
quota_update_replica_num(struct inode *inode, struct dirset *tdirset,
	gfarm_int64_t n)
{
	gfarm_off_t size = inode_get_size(inode);
	struct user *u = inode_get_user(inode);
	struct group *g = inode_get_group(inode);

	if (u) {
		struct quota *uq = user_quota(u);
		if (is_checked(uq)) {
			update_replica_num(uq, size, n);
			quota_softlimit_exceed(uq);
		}
	}
	if (g) {
		struct quota *gq = group_quota(g);
		if (is_checked(gq)) {
			update_replica_num(gq, size, n);
			quota_softlimit_exceed(gq);
		}
	}
	if (tdirset != TDIRSET_IS_UNKNOWN && tdirset != TDIRSET_IS_NOT_SET) {
		struct quota_metadata *dsq = dirset_quota(tdirset);
		if (is_checked(&dsq->usage)) {
			update_replica_num(&dsq->usage, size, n);
			quota_metadata_softlimit_exceed(dsq);
		}
	}

	quota_check_retry_if_running();
}

void
quota_update_replica_add(struct inode *inode, struct dirset *tdirset)
{
	quota_update_replica_num(inode, tdirset, 1);
}

void
quota_update_replica_remove(struct inode *inode, struct dirset *tdirset)
{
	quota_update_replica_num(inode, tdirset, -1);
}

#define update_file_remove(q, size, ncopy)				\
	{								\
		(q)->space = int64_add((q)->space, -size);		\
		(q)->num = int64_add((q)->num, -1);			\
		(q)->phy_space = int64_add((q)->phy_space, -(size * ncopy)); \
		(q)->phy_num = int64_add((q)->phy_num, -ncopy);		\
	}

void
quota_update_file_remove(struct inode *inode, struct dirset *tdirset)
{
	gfarm_off_t size;
	gfarm_int64_t ncopy;
	struct user *u = inode_get_user(inode);
	struct group *g = inode_get_group(inode);

	if (inode_is_file(inode)) {
		size = inode_get_size(inode);
		ncopy = inode_get_ncopy_with_dead_host(inode);
	} else {
		size = 0;
		ncopy = 0;
	}

	if (u) {
		struct quota *uq = user_quota(u);
		if (is_checked(uq)) {
			update_file_remove(uq, size, ncopy);
			quota_softlimit_exceed(uq);
		}
	}
	if (g) {
		struct quota *gq = group_quota(g);
		if (is_checked(gq)) {
			update_file_remove(gq, size, ncopy);
			quota_softlimit_exceed(gq);
		}
	}
	if (tdirset != TDIRSET_IS_UNKNOWN && tdirset != TDIRSET_IS_NOT_SET) {
		struct quota_metadata *dsq = dirset_quota(tdirset);
		if (is_checked(&dsq->usage)) {
			update_file_remove(&dsq->usage, size, ncopy);
			quota_metadata_softlimit_exceed(dsq);
		}
	}

	quota_check_retry_if_running();
}

void
dirquota_update_file_add(struct inode *inode, struct dirset *tdirset)
{
	gfarm_off_t size;
	gfarm_int64_t ncopy;

	if (inode_is_file(inode)) {
		size = inode_get_size(inode);
		ncopy = inode_get_ncopy_with_dead_host(inode);
	} else {
		size = 0;
		ncopy = 0;
	}

	if (tdirset != TDIRSET_IS_UNKNOWN && tdirset != TDIRSET_IS_NOT_SET) {
		struct quota_metadata *dsq = dirset_quota(tdirset);
		if (is_checked(&dsq->usage)) {
			update_file_add(&dsq->usage, size, ncopy);
			quota_metadata_softlimit_exceed(dsq);
		}
	}

	quota_check_retry_if_running();
}

void
dirquota_update_file_remove(struct inode *inode, struct dirset *tdirset)
{
	gfarm_off_t size;
	gfarm_int64_t ncopy;

	if (inode_is_file(inode)) {
		size = inode_get_size(inode);
		ncopy = inode_get_ncopy_with_dead_host(inode);
	} else {
		size = 0;
		ncopy = 0;
	}

	if (tdirset != TDIRSET_IS_UNKNOWN && tdirset != TDIRSET_IS_NOT_SET) {
		struct quota_metadata *dsq = dirset_quota(tdirset);
		if (is_checked(&dsq->usage)) {
			update_file_remove(&dsq->usage, size, ncopy);
			quota_metadata_softlimit_exceed(dsq);
		}
	}

	quota_check_retry_if_running();
}

enum quota_exceeded_type {
	QUOTA_NOT_EXCEEDED = 0,
	QUOTA_EXCEEDED_SPACE_SOFT,
	QUOTA_EXCEEDED_SPACE_HARD,
	QUOTA_EXCEEDED_NUM_SOFT,
	QUOTA_EXCEEDED_NUM_HARD,
	QUOTA_EXCEEDED_PHY_SPACE_SOFT,
	QUOTA_EXCEEDED_PHY_SPACE_HARD,
	QUOTA_EXCEEDED_PHY_NUM_SOFT,
	QUOTA_EXCEEDED_PHY_NUM_HARD,
};

static int
is_exceeded(struct timeval *nowp, struct quota *q,
	int num_file_creating, int num_replica_adding, gfarm_off_t size)
{
	int check_logical = 0, check_physical = 0;

	if (!is_checked(q))  /* quota is disabled */
		return (QUOTA_NOT_EXCEEDED);

	/* softlimit */
	if (quota_limit_is_valid(q->grace_period)) {
		if (quota_limit_is_valid(q->space_soft) &&
		    quota_limit_is_valid(q->space_exceed) &&
		    (nowp->tv_sec - q->space_exceed) > q->grace_period)
			return (QUOTA_EXCEEDED_SPACE_SOFT);
		if (quota_limit_is_valid(q->num_soft) &&
		    quota_limit_is_valid(q->num_exceed) &&
		    (nowp->tv_sec - q->num_exceed) > q->grace_period)
			return (QUOTA_EXCEEDED_NUM_SOFT);
		if (quota_limit_is_valid(q->phy_space_soft) &&
		    quota_limit_is_valid(q->phy_space_exceed) &&
		    (nowp->tv_sec - q->phy_space_exceed) > q->grace_period)
			return (QUOTA_EXCEEDED_PHY_SPACE_SOFT);
		if (quota_limit_is_valid(q->phy_num_soft) &&
		    quota_limit_is_valid(q->phy_num_exceed) &&
		    (nowp->tv_sec - q->phy_num_exceed) > q->grace_period)
			return (QUOTA_EXCEEDED_PHY_NUM_SOFT);
	}

	/* hardlimit */
	if (num_file_creating >= 1 && num_replica_adding <= 0) {
		check_logical = 1;
	} else if (num_file_creating <= 0 && num_replica_adding >= 1) {
		check_physical = 1;
	} else {
		check_logical = 1;
		check_physical = 1;
	}

	if (check_logical) {
		if ((quota_limit_is_valid(q->space_hard) &&
		    q->space + size > q->space_hard))
			return (QUOTA_EXCEEDED_SPACE_HARD);
		if (quota_limit_is_valid(q->num_hard) &&
		    q->num + num_file_creating > q->num_hard)
			return (QUOTA_EXCEEDED_NUM_HARD);
	}
	if (check_physical) {
		if (quota_limit_is_valid(q->phy_space_hard) &&
		    q->phy_space + (size * num_replica_adding) >
		    q->phy_space_hard)
			return (QUOTA_EXCEEDED_PHY_SPACE_HARD);
		if (quota_limit_is_valid(q->phy_num_hard) &&
		    q->phy_num + num_replica_adding > q->phy_num_hard)
			return (QUOTA_EXCEEDED_PHY_NUM_HARD);
	}

	return (QUOTA_NOT_EXCEEDED);
}

static int
quota_metadata_is_exceeded(struct timeval *nowp, struct quota_metadata *q,
	int num_file_creating, int num_replica_adding, gfarm_off_t size)
{
	int check_logical = 0, check_physical = 0;

	if (!is_checked(&q->usage))  /* quota is disabled */
		return (QUOTA_NOT_EXCEEDED);

	/* softlimit */
	if (quota_limit_is_valid(q->limit.grace_period)) {
		if (quota_limit_is_valid(q->limit.soft.space) &&
		    quota_limit_is_valid(q->exceed.space_time) &&
		    (nowp->tv_sec - q->exceed.space_time) >
		    q->limit.grace_period)
			return (QUOTA_EXCEEDED_SPACE_SOFT);
		if (quota_limit_is_valid(q->limit.soft.num) &&
		    quota_limit_is_valid(q->exceed.num_time) &&
		    (nowp->tv_sec - q->exceed.num_time) >
		    q->limit.grace_period)
			return (QUOTA_EXCEEDED_NUM_SOFT);
		if (quota_limit_is_valid(q->limit.soft.phy_space) &&
		    quota_limit_is_valid(q->exceed.phy_space_time) &&
		    (nowp->tv_sec - q->exceed.phy_space_time) >
		    q->limit.grace_period)
			return (QUOTA_EXCEEDED_PHY_SPACE_SOFT);
		if (quota_limit_is_valid(q->limit.soft.phy_num) &&
		    quota_limit_is_valid(q->exceed.phy_num_time) &&
		    (nowp->tv_sec - q->exceed.phy_num_time) >
		    q->limit.grace_period)
			return (QUOTA_EXCEEDED_PHY_NUM_SOFT);
	}

	/* hardlimit */
	if (num_file_creating >= 1 && num_replica_adding <= 0) {
		check_logical = 1;
	} else if (num_file_creating <= 0 && num_replica_adding >= 1) {
		check_physical = 1;
	} else {
		check_logical = 1;
		check_physical = 1;
	}

	if (check_logical) {
		if ((quota_limit_is_valid(q->limit.hard.space) &&
		    q->usage.space + size > q->limit.hard.space))
			return (QUOTA_EXCEEDED_SPACE_HARD);
		if (quota_limit_is_valid(q->limit.hard.num) &&
		    q->usage.num + num_file_creating > q->limit.hard.num)
			return (QUOTA_EXCEEDED_NUM_HARD);
	}
	if (check_physical) {
		if (quota_limit_is_valid(q->limit.hard.phy_space) &&
		    q->usage.phy_space + (size * num_replica_adding) >
		    q->limit.hard.phy_space)
			return (QUOTA_EXCEEDED_PHY_SPACE_HARD);
		if (quota_limit_is_valid(q->limit.hard.phy_num) &&
		    q->usage.phy_num + num_replica_adding >
		    q->limit.hard.phy_num)
			return (QUOTA_EXCEEDED_PHY_NUM_HARD);
	}

	return (QUOTA_NOT_EXCEEDED);
}

/*
 * num_file_creating >= 1 && num_replica_adding == 0 : check logical quota
 * num_file_creating == 0 && num_replica_adding >= 1 : check physical quota
 * num_file_creating == 0 && num_replica_adding == 0 : check both
 * num_file_creating >= 1 && num_replica_adding >= 1 : check both
 */
gfarm_error_t
quota_limit_check(struct user *u, struct group *g, struct dirset *tdirset,
	int num_file_creating, int num_replica_adding, gfarm_off_t size)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	if (u && is_exceeded(&now, user_quota(u),
	    num_file_creating, num_replica_adding, size)) {
		gflog_debug(GFARM_MSG_1002051,
			 "user_quota(%s) exceeded", user_name(u));
		return (GFARM_ERR_DISK_QUOTA_EXCEEDED);
	}
	if (g && is_exceeded(&now, group_quota(g),
	    num_file_creating, num_replica_adding, size)) {
		gflog_debug(GFARM_MSG_1002052,
			 "group_quota(%s) exceeded", group_name(g));
		return (GFARM_ERR_DISK_QUOTA_EXCEEDED);
	}
	if (tdirset != TDIRSET_IS_UNKNOWN && tdirset != TDIRSET_IS_NOT_SET &&
	    quota_metadata_is_exceeded(&now, dirset_quota(tdirset),
	    num_file_creating, num_replica_adding, size)) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "dirset_quota(%s:%s) exceeded",
		    dirset_get_username(tdirset),
		    dirset_get_dirsetname(tdirset));
		return (GFARM_ERR_DISK_QUOTA_EXCEEDED);
	}

	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
dirquota_limit_check(struct dirset *tdirset,
	int num_file_creating, int num_replica_adding, gfarm_off_t size)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	if (tdirset != TDIRSET_IS_UNKNOWN && tdirset != TDIRSET_IS_NOT_SET &&
	    quota_metadata_is_exceeded(&now, dirset_quota(tdirset),
	    num_file_creating, num_replica_adding, size)) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "dirset_quota(%s:%s) exceeded",
		    dirset_get_username(tdirset),
		    dirset_get_dirsetname(tdirset));
		return (GFARM_ERR_DISK_QUOTA_EXCEEDED);
	}

	return (GFARM_ERR_NO_ERROR);
}

void
quota_user_remove(struct user *u)
{
	struct quota *q = user_quota(u);

	if (q->on_db) {
		quota_user_remove_db(user_name(u));
		q->on_db = 0;
	}
	q->space = QUOTA_NOT_CHECK_YET;
}

void
quota_group_remove(struct group *g)
{
	struct quota *q = group_quota(g);

	if (q->on_db) {
		quota_group_remove_db(group_name(g));
		q->on_db = 0;
	}
	q->space = QUOTA_NOT_CHECK_YET;
}

static pthread_mutex_t quota_check_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t quota_check_wakeup = PTHREAD_COND_INITIALIZER;
static pthread_cond_t quota_check_end = PTHREAD_COND_INITIALIZER;
static int quota_check_needed = 0;
static int quota_check_running = 0;

static const char quota_check_mutex_diag[] = "quota_check_mutex";
static const char quota_check_wakeup_diag[] = "quota_check_wakeup";
static const char quota_check_end_diag[] = "quota_check_end";

static int
quota_check_needed_locked(const char *diag)
{
	int needed;

	gfarm_mutex_lock(&quota_check_mutex, diag, quota_check_mutex_diag);
	needed = quota_check_needed;
	gfarm_mutex_unlock(&quota_check_mutex, diag, quota_check_mutex_diag);
	return (needed);
}

static void
quota_check_needed_clear(const char *diag)
{
	gfarm_mutex_lock(&quota_check_mutex, diag, quota_check_mutex_diag);
	quota_check_needed = 0;
	gfarm_mutex_unlock(&quota_check_mutex, diag, quota_check_mutex_diag);
}

static int
quota_check_main(void)
{
	static const char diag[] = "quota_check_main";
	time_t time_start, time_total;
	gfarm_ino_t inum, inum_limit, inum_target;
	struct inode *inode;
#define QUOTA_CHECK_INODE_STEP 10000

	time_start = time(NULL);

	giant_lock();
	usage_tmp_clear();
	giant_unlock();

	giant_lock();
	inum = inode_root_number();
	inum_limit = inode_table_current_size();
	inum_target = inum + QUOTA_CHECK_INODE_STEP;
	if (inum_target > inum_limit)
		inum_target = inum_limit;
	for (;; inum++) {
		if (inum >= inum_target) {
			giant_unlock();
			/* make a chance for clients */
			/* usleep(100000); */ /* for debug */
			giant_lock();
			if (inum >= inum_limit)
				break;
			inum_target = inum + QUOTA_CHECK_INODE_STEP;
			if (inum_target > inum_limit)
				inum_target = inum_limit;
		}
		if (quota_check_needed_locked(diag)) {
			giant_unlock();
			return (1); /* retry */
		}
		inode = inode_lookup(inum);
		if (inode != NULL)
			usage_tmp_update(inode);
	}
	giant_unlock();

	giant_lock();
	if (quota_check_needed_locked(diag)) {
		giant_unlock();
		return (1); /* retry */
	}
	quota_update_usage();
	giant_unlock();

	time_total = time(NULL) - time_start;
	gflog_info(GFARM_MSG_1004296,
	    "quota_check: finished, inodes=%lld, time=%lld",
	    (long long)inum_limit, (long long)time_total);

	return (0); /* finished */
}

static void *
quota_check(void *arg)
{
	static const char diag[] = "quota_check";

	(void)gfarm_pthread_set_priority_minimum(diag);

	for (;;) {
		gfarm_mutex_lock(&quota_check_mutex, diag,
		    quota_check_mutex_diag);
		quota_check_running = 0;
		gfarm_cond_signal(&quota_check_end, diag, quota_check_end_diag);

		while (!quota_check_needed)
			gfarm_cond_wait(&quota_check_wakeup,
			    &quota_check_mutex, diag,
			    quota_check_wakeup_diag);

		quota_check_running = 1;
		gfarm_mutex_unlock(&quota_check_mutex, diag,
		    quota_check_mutex_diag);

		gflog_info(GFARM_MSG_1004297, "quota_check: start");
		quota_check_needed_clear(diag);
		while (quota_check_main()) {
			quota_check_needed_clear(diag);
			gflog_info(GFARM_MSG_1004298, "quota_check: retry");
		}
	}

	return (NULL);
}

static void
quota_check_wait_for_end(void)
{
	static const char diag[] = "quota_check_wait_for_end";

	gfarm_mutex_lock(&quota_check_mutex, diag, quota_check_mutex_diag);
	while (quota_check_running)
		gfarm_cond_wait(&quota_check_end,
		    &quota_check_mutex, diag, quota_check_end_diag);
	gfarm_mutex_unlock(&quota_check_mutex, diag, quota_check_mutex_diag);
}

static void
quota_check_retry_if_running(void)
{
	static const char diag[] = "quota_check_retry_if_running";

	gfarm_mutex_lock(&quota_check_mutex, diag, quota_check_mutex_diag);
	if (quota_check_running)
		quota_check_needed = 1;
	/* else: cond_wait now */
	gfarm_mutex_unlock(&quota_check_mutex, diag, quota_check_mutex_diag);
}

void
quota_check_start(void)
{
	static const char diag[] = "quota_check_start";

	gfarm_mutex_lock(&quota_check_mutex, diag, quota_check_mutex_diag);
	quota_check_needed = 1;
	quota_check_running = 1;
	gfarm_cond_signal(&quota_check_wakeup, diag, quota_check_wakeup_diag);
	gfarm_mutex_unlock(&quota_check_mutex, diag, quota_check_mutex_diag);
}

void
quota_dir_check_schedule(void)
{
	/* DQTODO */
	gflog_error(GFARM_MSG_UNFIXED,
	    "quota_dir_check_schedule: not implemented, yet");
}

void
quota_dir_check_start(void)
{
	/* DQTODO */
	gflog_error(GFARM_MSG_UNFIXED,
	    "quota_dir_check_start: not implemented, yet");
}

void
quota_check_init(void)
{
	gfarm_error_t e;

	if ((e = create_detached_thread(quota_check, NULL))
	    != GFARM_ERR_NO_ERROR)
		gflog_fatal(GFARM_MSG_1004299,
		    "create_detached_thread(quota_check_init): %s",
		    gfarm_error_string(e));

	quota_check_start();
}

/* server operations */
static gfarm_error_t
quota_get_common(struct peer *peer, int from_client, int skip, int is_group)
{
	const char *diag = is_group ?
	    "GFM_PROTO_QUOTA_GROUP_GET" : "GFM_PROTO_QUOTA_USER_GET";
	gfarm_error_t e;
	struct user *user = NULL, *peer_user = peer_get_user(peer);
	struct group *group = NULL;
	char *name;
	struct quota *q;
	struct gfarm_quota_get_info qi;

	e = gfm_server_get_request(peer, diag, "s", &name);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002053,
			"%s request failed: %s", diag, gfarm_error_string(e));
		return (e);
	}
	if (skip) {
		free(name);
		return (GFARM_ERR_NO_ERROR);
	}
	if (!from_client || peer_user == NULL) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		gflog_debug(GFARM_MSG_1002054,
			    "%s: !from_client or invalid peer_user ", diag);
		free(name);
		return (gfm_server_put_reply(peer, diag, e, ""));
	}

	if (db_state != GFARM_ERR_NO_ERROR) {
		free(name);
		gflog_debug(GFARM_MSG_1002055, "db_quota is invalid: %s",
			gfarm_error_string(db_state));
		return (gfm_server_put_reply(peer, diag, db_state, ""));
	}

	giant_lock();
	if (is_group) {
		if (strcmp(name, "") == 0)
			e = GFARM_ERR_NO_SUCH_GROUP;
		else if ((group = group_lookup(name)) == NULL) {
			if (user_is_admin(peer_user))
				e = GFARM_ERR_NO_SUCH_GROUP;
			else  /* hidden groupnames */
				e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		} else if ((!user_in_group(peer_user, group)) &&
			!user_is_admin(peer_user))
			e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		/* user_in_group() || user_is_admin() : permit */
	} else {
		if (strcmp(name, "") == 0) {
			user = peer_user; /* permit not-admin */
			free(name);
			name = strdup_log(user_name(peer_user), diag);
			if (name == NULL)
				e = GFARM_ERR_NO_MEMORY;
		} else if (strcmp(name, user_name(peer_user)) == 0)
			user = peer_user; /* permit not-admin */
		else if (!user_is_admin(peer_user))
			e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		else if ((user = user_lookup(name)) == NULL)
			e = GFARM_ERR_NO_SUCH_USER;
	}
	if (e != GFARM_ERR_NO_ERROR) {
		giant_unlock();
		gflog_debug(GFARM_MSG_1002056,
			    "%s: name=%s: %s", diag, name,
			    gfarm_error_string(e));
		free(name);
		return (gfm_server_put_reply(peer, diag, e, ""));
	}
	if (is_group)
		q = group_quota(group);
	else
		q = user_quota(user);
	if (!is_checked(q)) { /* quota is not initialized */
		giant_unlock();
		gflog_debug(GFARM_MSG_1002057,
			    "%s: %s's quota is not enabled", diag, name);
		free(name);
		e = GFARM_ERR_NO_SUCH_OBJECT;
		return (gfm_server_put_reply(peer, diag, e, ""));
	}
	quota_convert_1(q, name, &qi);
	giant_unlock();

	e = gfm_server_put_reply(
			peer, diag, e, "slllllllllllllllll",
			qi.name,
			qi.grace_period,
			qi.space,
			qi.space_grace,
			qi.space_soft,
			qi.space_hard,
			qi.num,
			qi.num_grace,
			qi.num_soft,
			qi.num_hard,
			qi.phy_space,
			qi.phy_space_grace,
			qi.phy_space_soft,
			qi.phy_space_hard,
			qi.phy_num,
			qi.phy_num_grace,
			qi.phy_num_soft,
			qi.phy_num_hard);
	free(name);
	return (e);
}

#define set_limit(target, received)					\
	{								\
		if (quota_limit_is_valid(received))			\
			target = received;				\
		else if (received == GFARM_QUOTA_INVALID)		\
			target = GFARM_QUOTA_INVALID;			\
	}

gfarm_error_t
quota_lookup(const char *name, int is_group, struct quota **qp,
	const char *diag)
{
	gfarm_error_t e;

	if (is_group) {
		struct group *group = group_lookup(name);
		if (group == NULL) {
			e = GFARM_ERR_NO_SUCH_GROUP;
			gflog_debug(GFARM_MSG_1002061,
				    "%s: name=%s: %s",
				    diag, name, gfarm_error_string(e));
		} else {
			*qp = group_quota(group);
			e = GFARM_ERR_NO_ERROR;
		}
	} else {
		struct user *user = user_lookup(name);
		if (user == NULL) {
			e = GFARM_ERR_NO_SUCH_USER;
			gflog_debug(GFARM_MSG_1002062,
				    "%s: name=%s: %s",
				    diag, name, gfarm_error_string(e));
		} else {
			*qp = user_quota(user);
			e = GFARM_ERR_NO_ERROR;
		}
	}
	return (e);
}

static gfarm_error_t
quota_set_common(struct peer *peer, int from_client, int skip, int is_group)
{
	const char *diag = is_group ?
	    "GFM_PROTO_QUOTA_GROUP_SET" : "GFM_PROTO_QUOTA_USER_SET";
	gfarm_error_t e;
	struct gfarm_quota_set_info qi;
	struct quota *q;
	struct user *peer_user = peer_get_user(peer);

	e = gfm_server_get_request(peer, diag, "slllllllll",
				   &qi.name,
				   &qi.grace_period,
				   &qi.space_soft,
				   &qi.space_hard,
				   &qi.num_soft,
				   &qi.num_hard,
				   &qi.phy_space_soft,
				   &qi.phy_space_hard,
				   &qi.phy_num_soft,
				   &qi.phy_num_hard);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002058, "%s request failed: %s",
			diag, gfarm_error_string(e));
		return (e);
	}
	if (skip) {
		free(qi.name);
		return (GFARM_ERR_NO_ERROR);
	}

	if (db_state != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002059, "db_quota is invalid: %s",
			gfarm_error_string(db_state));
		free(qi.name);
		return (gfm_server_put_reply(peer, diag, db_state, ""));
	}

	giant_lock();
	if (!from_client || peer_user == NULL || !user_is_admin(peer_user)) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		gflog_debug(GFARM_MSG_1002060,
			    "%s: !from_client or invalid peer_user"
			    " or !user_is_admin", diag);
		goto end;
	} else if ((e = quota_lookup(qi.name, is_group, &q, diag))
	    != GFARM_ERR_NO_ERROR) {
		goto end;
	}

	/* set limits */
	set_limit(q->grace_period, qi.grace_period);
	set_limit(q->space_soft, qi.space_soft);
	set_limit(q->space_hard, qi.space_hard);
	set_limit(q->num_soft, qi.num_soft);
	set_limit(q->num_hard, qi.num_hard);
	set_limit(q->phy_space_soft, qi.phy_space_soft);
	set_limit(q->phy_space_hard, qi.phy_space_hard);
	set_limit(q->phy_num_soft, qi.phy_num_soft);
	set_limit(q->phy_num_hard, qi.phy_num_hard);

	/* check softlimit and update exceeded time */
	quota_softlimit_exceed(q);

	if (is_group)
		e = db_quota_group_set(q, qi.name);
	else
		e = db_quota_user_set(q, qi.name);
	if (e == GFARM_ERR_NO_ERROR)
		q->on_db = 1;
end:
	giant_unlock();
	free(qi.name);
	return (gfm_server_put_reply(peer, diag, e, ""));
}

gfarm_error_t
gfm_server_quota_user_get(struct peer *peer, int from_client, int skip)
{
	return (quota_get_common(peer, from_client, skip, 0));
}

gfarm_error_t
gfm_server_quota_user_set(struct peer *peer, int from_client, int skip)
{
	return (quota_set_common(peer, from_client, skip, 0));
}

gfarm_error_t
gfm_server_quota_group_get(struct peer *peer, int from_client, int skip)
{
	return (quota_get_common(peer, from_client, skip, 1));
}

gfarm_error_t
gfm_server_quota_group_set(struct peer *peer, int from_client, int skip)
{
	return (quota_set_common(peer, from_client, skip, 1));
}

gfarm_error_t
gfm_server_quota_check(struct peer *peer, int from_client, int skip)
{
	static const char diag[] = "GFM_PROTO_QUOTA_CHECK";
	gfarm_error_t e;
	struct user *peer_user = peer_get_user(peer);

	e = gfm_server_get_request(peer, diag, "");
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002063,
			"%s request failed: %s", diag, gfarm_error_string(e));
		return (e);
	}
	if (skip)
		return (GFARM_ERR_NO_ERROR);

	if (db_state != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002064, "db_quota is invalid: %s",
			gfarm_error_string(db_state));
		return (gfm_server_put_reply(peer, diag, db_state, ""));
	}

	giant_lock();
	if (!from_client || peer_user == NULL || !user_is_admin(peer_user)) {
		giant_unlock();
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		gflog_debug(GFARM_MSG_1002065,
			    "%s: !from_client or invalid peer_user"
			    " or !user_is_admin", diag);
		return (gfm_server_put_reply(peer, diag, e, ""));
	}
	giant_unlock();

	quota_check_start();
	quota_check_wait_for_end();

	return (gfm_server_put_reply(peer, diag, e, ""));
}
