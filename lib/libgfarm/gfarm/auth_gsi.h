void gfarm_gsi_initialize_mutex_lock(const char *);
void gfarm_gsi_initialize_mutex_unlock(const char *);

gfarm_error_t gfarm_gsi_cred_config_convert_to_name(
	enum gfarm_auth_cred_type, char *, char *, char *, gss_name_t *);

void gfarm_gsi_set_delegated_cred(gss_cred_id_t);
void gfarm_gsi_set_delegated_cred_unlocked(gss_cred_id_t);
gss_cred_id_t gfarm_gsi_get_delegated_cred(void);
gss_cred_id_t gfarm_gsi_get_delegated_cred_unlocked(void);

/* only for auth_server_gsi.c */
void gfarm_gsi_server_init_count_increment(void);
void gfarm_gsi_server_init_count_decrement(void);
