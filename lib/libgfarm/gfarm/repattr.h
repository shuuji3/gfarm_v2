/*
 * $Id$
 */

#define GFARM_REPATTR_TYPE	"replicainfo"
#define GFARM_REPATTR_NAME	"gfarm." GFARM_REPATTR_TYPE

struct gfarm_repattr;
typedef struct gfarm_repattr *gfarm_repattr_t;

size_t gfarm_repattr_parse(const char *, gfarm_repattr_t **);
void gfarm_repattr_free(gfarm_repattr_t);
const char *gfarm_repattr_group(gfarm_repattr_t);
size_t gfarm_repattr_amount(gfarm_repattr_t);
char *gfarm_repattr_stringify(gfarm_repattr_t *, size_t);
size_t gfarm_repattr_reduce(const char *, gfarm_repattr_t **);

int gfarm_repattr_validate(const char *);
