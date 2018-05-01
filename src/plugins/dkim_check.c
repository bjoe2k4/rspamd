/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/***MODULE:dkim
 * rspamd module that checks dkim records of incoming email
 *
 * Allowed options:
 * - symbol_allow (string): symbol to insert in case of allow (default: 'R_DKIM_ALLOW')
 * - symbol_reject (string): symbol to insert (default: 'R_DKIM_REJECT')
 * - symbol_tempfail (string): symbol to insert in case of temporary fail (default: 'R_DKIM_TEMPFAIL')
 * - symbol_permfail (string): symbol to insert in case of permanent failure (default: 'R_DKIM_PERMFAIL')
 * - symbol_na (string): symbol to insert in case of no signing (default: 'R_DKIM_NA')
 * - whitelist (map): map of whitelisted networks
 * - domains (map): map of domains to check
 * - strict_multiplier (number): multiplier for strict domains
 * - time_jitter (number): jitter in seconds to allow time diff while checking
 * - trusted_only (flag): check signatures only for domains in 'domains' map
 */


#include "config.h"
#include "libmime/message.h"
#include "libserver/dkim.h"
#include "libutil/hash.h"
#include "libutil/map.h"
#include "libutil/map_helpers.h"
#include "rspamd.h"
#include "utlist.h"
#include "lua/lua_common.h"
#include "libserver/mempool_vars_internal.h"

#define DEFAULT_SYMBOL_REJECT "R_DKIM_REJECT"
#define DEFAULT_SYMBOL_TEMPFAIL "R_DKIM_TEMPFAIL"
#define DEFAULT_SYMBOL_ALLOW "R_DKIM_ALLOW"
#define DEFAULT_SYMBOL_NA "R_DKIM_NA"
#define DEFAULT_SYMBOL_PERMFAIL "R_DKIM_PERMFAIL"
#define DEFAULT_CACHE_SIZE 2048
#define DEFAULT_TIME_JITTER 60
#define DEFAULT_MAX_SIGS 5

static const gchar default_sign_headers[] = ""
		"(o)from:(o)sender:(o)reply-to:(o)subject:(o)date:(o)message-id:"
		"(o)to:(o)cc:(o)mime-version:(o)content-type:(o)content-transfer-encoding:"
		"resent-to:resent-cc:resent-from:resent-sender:resent-message-id:"
		"(o)in-reply-to:(o)references:list-id:list-owner:list-unsubscribe:"
		"list-subscribe:list-post";

struct dkim_ctx {
	struct module_ctx ctx;
	const gchar *symbol_reject;
	const gchar *symbol_tempfail;
	const gchar *symbol_allow;
	const gchar *symbol_na;
	const gchar *symbol_permfail;

	rspamd_mempool_t *dkim_pool;
	struct rspamd_radix_map_helper *whitelist_ip;
	struct rspamd_hash_map_helper *dkim_domains;
	guint strict_multiplier;
	guint time_jitter;
	rspamd_lru_hash_t *dkim_hash;
	rspamd_lru_hash_t *dkim_sign_hash;
	const gchar *sign_headers;
	gint sign_condition_ref;
	guint max_sigs;
	gboolean trusted_only;
	gboolean check_local;
	gboolean check_authed;
};

struct dkim_check_result {
	rspamd_dkim_context_t *ctx;
	rspamd_dkim_key_t *key;
	struct rspamd_task *task;
	gint res;
	gint mult_allow, mult_deny;
	struct rspamd_async_watcher *w;
	struct dkim_check_result *next, *prev, *first;
};

static struct dkim_ctx *dkim_module_ctx = NULL;

static void dkim_symbol_callback (struct rspamd_task *task, void *unused);
static void dkim_sign_callback (struct rspamd_task *task, void *unused);

static gint lua_dkim_sign_handler (lua_State *L);
static gint lua_dkim_verify_handler (lua_State *L);
static gint lua_dkim_canonicalize_handler (lua_State *L);

/* Initialization */
gint dkim_module_init (struct rspamd_config *cfg, struct module_ctx **ctx);
gint dkim_module_config (struct rspamd_config *cfg);
gint dkim_module_reconfig (struct rspamd_config *cfg);

module_t dkim_module = {
	"dkim",
	dkim_module_init,
	dkim_module_config,
	dkim_module_reconfig,
	NULL,
	RSPAMD_MODULE_VER
};

static void
dkim_module_key_dtor (gpointer k)
{
	rspamd_dkim_key_t *key = k;

	rspamd_dkim_key_unref (key);
}

gint
dkim_module_init (struct rspamd_config *cfg, struct module_ctx **ctx)
{
	if (dkim_module_ctx == NULL) {
		dkim_module_ctx = g_malloc0 (sizeof (struct dkim_ctx));

		dkim_module_ctx->dkim_pool = rspamd_mempool_new (rspamd_mempool_suggest_size (), "dkim");
		dkim_module_ctx->sign_headers = default_sign_headers;
		dkim_module_ctx->sign_condition_ref = -1;
		dkim_module_ctx->max_sigs = DEFAULT_MAX_SIGS;
	}

	*ctx = (struct module_ctx *)dkim_module_ctx;

	rspamd_rcl_add_doc_by_path (cfg,
			NULL,
			"DKIM check plugin",
			"dkim",
			UCL_OBJECT,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Map of IP addresses that should be excluded from DKIM checks",
			"whitelist",
			UCL_STRING,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Symbol that is added if DKIM check is successful",
			"symbol_allow",
			UCL_STRING,
			NULL,
			0,
			DEFAULT_SYMBOL_ALLOW,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Symbol that is added if DKIM check is unsuccessful",
			"symbol_reject",
			UCL_STRING,
			NULL,
			0,
			DEFAULT_SYMBOL_REJECT,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Symbol that is added if DKIM check can't be completed (e.g. DNS failure)",
			"symbol_tempfail",
			UCL_STRING,
			NULL,
			0,
			DEFAULT_SYMBOL_TEMPFAIL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Symbol that is added if mail is not signed",
			"symbol_na",
			UCL_STRING,
			NULL,
			0,
			DEFAULT_SYMBOL_NA,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Symbol that is added if permanent failure encountered",
			"symbol_permfail",
			UCL_STRING,
			NULL,
			0,
			DEFAULT_SYMBOL_PERMFAIL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Size of DKIM keys cache",
			"dkim_cache_size",
			UCL_INT,
			NULL,
			0,
			G_STRINGIFY (DEFAULT_CACHE_SIZE),
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Allow this time difference when checking DKIM signature time validity",
			"time_jitter",
			UCL_TIME,
			NULL,
			0,
			G_STRINGIFY (DEFAULT_TIME_JITTER),
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Domains to check DKIM for (check all domains if this option is empty)",
			"domains",
			UCL_STRING,
			NULL,
			0,
			"empty",
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Map of domains that are treated as 'trusted' meaning that DKIM policy failure has more significant score",
			"trusted_domains",
			UCL_STRING,
			NULL,
			0,
			"empty",
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Multiply dkim score by this factor for trusted domains",
			"strict_multiplier",
			UCL_FLOAT,
			NULL,
			0,
			NULL,
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Check DKIM policies merely for `trusted_domains`",
			"trusted_only",
			UCL_BOOLEAN,
			NULL,
			0,
			"false",
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Lua script that tells if a message should be signed and with what params",
			"sign_condition",
			UCL_STRING,
			NULL,
			0,
			"empty",
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Obsoleted: maximum number of DKIM signatures to check",
			"max_sigs",
			UCL_INT,
			NULL,
			0,
			"n/a",
			0);
	rspamd_rcl_add_doc_by_path (cfg,
			"dkim",
			"Headers used in signing",
			"sign_headers",
			UCL_STRING,
			NULL,
			0,
			default_sign_headers,
			0);

	return 0;
}

gint
dkim_module_config (struct rspamd_config *cfg)
{
	const ucl_object_t *value;
	gint res = TRUE, cb_id = -1;
	guint cache_size, sign_cache_size;
	gboolean got_trusted = FALSE;

	/* Register global methods */
	lua_getglobal (cfg->lua_state, "rspamd_plugins");

	if (lua_type (cfg->lua_state, -1) == LUA_TTABLE) {
		lua_pushstring (cfg->lua_state, "dkim");
		lua_createtable (cfg->lua_state, 0, 1);
		/* Set methods */
		lua_pushstring (cfg->lua_state, "sign");
		lua_pushcfunction (cfg->lua_state, lua_dkim_sign_handler);
		lua_settable (cfg->lua_state, -3);
		lua_pushstring (cfg->lua_state, "verify");
		lua_pushcfunction (cfg->lua_state, lua_dkim_verify_handler);
		lua_settable (cfg->lua_state, -3);
		lua_pushstring (cfg->lua_state, "canon_header_relaxed");
		lua_pushcfunction (cfg->lua_state, lua_dkim_canonicalize_handler);
		lua_settable (cfg->lua_state, -3);
		/* Finish dkim key */
		lua_settable (cfg->lua_state, -3);
	}

	lua_pop (cfg->lua_state, 1); /* Remove global function */
	dkim_module_ctx->whitelist_ip = NULL;

	if ((value =
			rspamd_config_get_module_opt (cfg, "options", "check_local")) != NULL) {
		dkim_module_ctx->check_local = ucl_object_toboolean (value);
	}
	else {
		dkim_module_ctx->check_local = FALSE;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "options", "check_authed")) != NULL) {
		dkim_module_ctx->check_authed = ucl_object_toboolean (value);
	}
	else {
		dkim_module_ctx->check_authed = FALSE;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "symbol_reject")) != NULL) {
		dkim_module_ctx->symbol_reject = ucl_object_tostring (value);
	}
	else {
		dkim_module_ctx->symbol_reject = DEFAULT_SYMBOL_REJECT;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim",
		"symbol_tempfail")) != NULL) {
		dkim_module_ctx->symbol_tempfail = ucl_object_tostring (value);
	}
	else {
		dkim_module_ctx->symbol_tempfail = DEFAULT_SYMBOL_TEMPFAIL;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "symbol_allow")) != NULL) {
		dkim_module_ctx->symbol_allow = ucl_object_tostring (value);
	}
	else {
		dkim_module_ctx->symbol_allow = DEFAULT_SYMBOL_ALLOW;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "symbol_na")) != NULL) {
		dkim_module_ctx->symbol_na = ucl_object_tostring (value);
	}
	else {
		dkim_module_ctx->symbol_na = DEFAULT_SYMBOL_NA;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "symbol_permfail")) != NULL) {
		dkim_module_ctx->symbol_permfail = ucl_object_tostring (value);
	}
	else {
		dkim_module_ctx->symbol_permfail = DEFAULT_SYMBOL_PERMFAIL;
	}
	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim",
		"dkim_cache_size")) != NULL) {
		cache_size = ucl_object_toint (value);
	}
	else {
		cache_size = DEFAULT_CACHE_SIZE;
	}

	if ((value =
			rspamd_config_get_module_opt (cfg, "dkim",
					"sign_cache_size")) != NULL) {
		sign_cache_size = ucl_object_toint (value);
	}
	else {
		sign_cache_size = 128;
	}

	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "time_jitter")) != NULL) {
		dkim_module_ctx->time_jitter = ucl_object_todouble (value);
	}
	else {
		dkim_module_ctx->time_jitter = DEFAULT_TIME_JITTER;
	}

	if ((value =
			rspamd_config_get_module_opt (cfg, "dkim", "max_sigs")) != NULL) {
		dkim_module_ctx->max_sigs = ucl_object_toint (value);
	}

	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "whitelist")) != NULL) {

		rspamd_config_radix_from_ucl (cfg, value, "DKIM whitelist",
				&dkim_module_ctx->whitelist_ip, NULL);
		rspamd_mempool_add_destructor (dkim_module_ctx->dkim_pool,
				(rspamd_mempool_destruct_t)rspamd_map_helper_destroy_radix,
				dkim_module_ctx->whitelist_ip);

	}

	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "domains")) != NULL) {
		if (!rspamd_map_add_from_ucl (cfg, value,
			"DKIM domains", rspamd_kv_list_read, rspamd_kv_list_fin,
			(void **)&dkim_module_ctx->dkim_domains)) {
			msg_warn_config ("cannot load dkim domains list from %s",
				ucl_object_tostring (value));
		}
		else {
			rspamd_mempool_add_destructor (dkim_module_ctx->dkim_pool,
					(rspamd_mempool_destruct_t)rspamd_map_helper_destroy_hash,
					dkim_module_ctx->dkim_domains);
			got_trusted = TRUE;
		}
	}

	if (!got_trusted && (value =
			rspamd_config_get_module_opt (cfg, "dkim", "trusted_domains")) != NULL) {
		if (!rspamd_map_add_from_ucl (cfg, value,
				"DKIM domains", rspamd_kv_list_read, rspamd_kv_list_fin,
				(void **)&dkim_module_ctx->dkim_domains)) {
			msg_warn_config ("cannot load dkim domains list from %s",
					ucl_object_tostring (value));
		}
		else {
			rspamd_mempool_add_destructor (dkim_module_ctx->dkim_pool,
					(rspamd_mempool_destruct_t)rspamd_map_helper_destroy_hash,
					dkim_module_ctx->dkim_domains);
			got_trusted = TRUE;
		}
	}

	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim",
		"strict_multiplier")) != NULL) {
		dkim_module_ctx->strict_multiplier = ucl_object_toint (value);
	}
	else {
		dkim_module_ctx->strict_multiplier = 1;
	}

	if ((value =
		rspamd_config_get_module_opt (cfg, "dkim", "trusted_only")) != NULL) {
		dkim_module_ctx->trusted_only = ucl_object_toboolean (value);
	}
	else {
		dkim_module_ctx->trusted_only = FALSE;
	}

	if ((value =
			rspamd_config_get_module_opt (cfg, "dkim", "sign_headers")) != NULL) {
		dkim_module_ctx->sign_headers = ucl_object_tostring (value);
	}

	dkim_module_ctx->dkim_hash = rspamd_lru_hash_new (
			cache_size,
			g_free,
			dkim_module_key_dtor);
	dkim_module_ctx->dkim_sign_hash = rspamd_lru_hash_new (
			sign_cache_size,
			g_free,
			(GDestroyNotify)rspamd_dkim_sign_key_unref);

	if (dkim_module_ctx->trusted_only && !got_trusted) {
		msg_err_config (
			"trusted_only option is set and no trusted domains are defined; disabling dkim module completely as it is useless in this case");
	}
	else {
		if (!rspamd_config_is_module_enabled (cfg, "dkim")) {
			return TRUE;
		}

		cb_id = rspamd_symbols_cache_add_symbol (cfg->cache,
			dkim_module_ctx->symbol_reject,
			0,
			dkim_symbol_callback,
			NULL,
			SYMBOL_TYPE_NORMAL|SYMBOL_TYPE_FINE,
			-1);
		rspamd_symbols_cache_add_symbol (cfg->cache,
			dkim_module_ctx->symbol_na,
			0,
			NULL, NULL,
			SYMBOL_TYPE_VIRTUAL|SYMBOL_TYPE_FINE,
			cb_id);
		rspamd_symbols_cache_add_symbol (cfg->cache,
			dkim_module_ctx->symbol_permfail,
			0,
			NULL, NULL,
			SYMBOL_TYPE_VIRTUAL|SYMBOL_TYPE_FINE,
			cb_id);
		rspamd_symbols_cache_add_symbol (cfg->cache,
			dkim_module_ctx->symbol_tempfail,
			0,
			NULL, NULL,
			SYMBOL_TYPE_VIRTUAL|SYMBOL_TYPE_FINE,
			cb_id);
		rspamd_symbols_cache_add_symbol (cfg->cache,
			dkim_module_ctx->symbol_allow,
			0,
			NULL, NULL,
			SYMBOL_TYPE_VIRTUAL|SYMBOL_TYPE_FINE,
			cb_id);

		msg_info_config ("init internal dkim module");
#ifndef HAVE_OPENSSL
		msg_warn_config (
			"openssl is not found so dkim rsa check is disabled, only check body hash, it is NOT safe to trust these results");
#endif
	}

	if ((value = rspamd_config_get_module_opt (cfg, "dkim", "sign_condition"))
			!= NULL) {
		const gchar *lua_script;

		lua_script = ucl_object_tostring (value);

		if (lua_script) {
			if (luaL_dostring (cfg->lua_state, lua_script) != 0) {
				msg_err_config ("cannot execute lua script for dkim "
						"sign condition: %s", lua_tostring (cfg->lua_state, -1));
			}
			else {
				if (lua_type (cfg->lua_state, -1) == LUA_TFUNCTION) {
					dkim_module_ctx->sign_condition_ref = luaL_ref (cfg->lua_state,
							LUA_REGISTRYINDEX);
					rspamd_lua_add_ref_dtor (cfg->lua_state,
							dkim_module_ctx->dkim_pool,
							dkim_module_ctx->sign_condition_ref);

					rspamd_symbols_cache_add_symbol (cfg->cache,
							"DKIM_SIGN",
							0,
							dkim_sign_callback,
							NULL,
							SYMBOL_TYPE_CALLBACK|SYMBOL_TYPE_FINE,
							-1);
					msg_info_config ("init condition script for DKIM signing");

					/*
					 * Allow dkim signing to be executed only after dkim check
					 */
					if (cb_id > 0) {
						rspamd_symbols_cache_add_delayed_dependency (cfg->cache,
								"DKIM_SIGN", dkim_module_ctx->symbol_reject);
					}

					rspamd_config_add_symbol (cfg,
							"DKIM_SIGN", 0.0, "DKIM signature fake symbol",
							"dkim", RSPAMD_SYMBOL_FLAG_IGNORE, 1, 1);
					rspamd_config_add_symbol (cfg,
							"DKIM_TRACE", 0.0, "DKIM trace symbol",
							"policies", RSPAMD_SYMBOL_FLAG_IGNORE, 1, 1);

				}
				else {
					msg_err_config ("lua script must return "
							"function(task) and not %s",
							lua_typename (cfg->lua_state,
									lua_type (cfg->lua_state, -1)));
				}
			}
		}
	}

	return res;
}

rspamd_dkim_sign_key_t *
dkim_module_load_key_format (lua_State *L, struct rspamd_task *task,
		const gchar *key, gsize keylen,
		enum rspamd_dkim_sign_key_type kt)
{
	guchar h[rspamd_cryptobox_HASHBYTES],
			hex_hash[rspamd_cryptobox_HASHBYTES * 2 + 1];
	rspamd_dkim_sign_key_t *ret;
	GError *err = NULL;

	memset (hex_hash, 0, sizeof (hex_hash));
	rspamd_cryptobox_hash (h, key, keylen, NULL, 0);
	rspamd_encode_hex_buf (h, sizeof (h), hex_hash, sizeof (hex_hash));
	ret = rspamd_lru_hash_lookup (dkim_module_ctx->dkim_sign_hash,
				hex_hash, time (NULL));

	if (ret == NULL) {
			ret = rspamd_dkim_sign_key_load (key, keylen, kt, &err);

			if (ret == NULL) {
				msg_err_task ("cannot load private key: %e", err);
				g_error_free (err);

				return NULL;
			}

			rspamd_lru_hash_insert (dkim_module_ctx->dkim_sign_hash,
					g_strdup (hex_hash), ret,
					time (NULL), 0);
		}

	return ret;
}

static gint
lua_dkim_sign_handler (lua_State *L)
{
	struct rspamd_task *task = lua_check_task (L, 1);
	gint64 arc_idx = 0, expire = 0;
	enum rspamd_dkim_type sign_type = RSPAMD_DKIM_NORMAL;
	GError *err = NULL;
	GString *hdr;
	const gchar *selector = NULL, *domain = NULL, *key = NULL, *rawkey = NULL,
			*headers = NULL, *sign_type_str = NULL, *arc_cv = NULL;
	rspamd_dkim_sign_context_t *ctx;
	rspamd_dkim_sign_key_t *dkim_key;
	gsize rawlen = 0, keylen = 0;
	gboolean no_cache = FALSE;

	luaL_argcheck (L, lua_type (L, 2) == LUA_TTABLE, 2, "'table' expected");
	/*
	 * Get the following elements:
	 * - selector
	 * - domain
	 * - key
	 */
	if (!rspamd_lua_parse_table_arguments (L, 2, &err,
			"key=V;rawkey=V;*domain=S;*selector=S;no_cache=B;headers=S;"
					"sign_type=S;arc_idx=I;arc_cv=S;expire=I",
			&keylen, &key, &rawlen, &rawkey, &domain,
			&selector, &no_cache, &headers,
			&sign_type_str, &arc_idx, &arc_cv, &expire)) {
		msg_err_task ("invalid return value from sign condition: %e",
				err);
		g_error_free (err);

		lua_pushboolean (L, FALSE);
		return 1;
	}

	if (headers == NULL) {
		headers = dkim_module_ctx->sign_headers;
	}

	if (dkim_module_ctx->dkim_sign_hash == NULL) {
		dkim_module_ctx->dkim_sign_hash = rspamd_lru_hash_new (
				128,
				g_free, /* Keys are just C-strings */
				(GDestroyNotify)rspamd_dkim_sign_key_unref);
	}

#define PEM_SIG "-----BEGIN"

	if (key) {
		if (key[0] == '.' || key[0] == '/') {
			/* Likely raw path */
			dkim_key = rspamd_lru_hash_lookup (dkim_module_ctx->dkim_sign_hash,
					key, time (NULL));

			if (dkim_key == NULL) {
				dkim_key = rspamd_dkim_sign_key_load (key, strlen (key),
						RSPAMD_DKIM_SIGN_KEY_FILE, &err);

				if (dkim_key == NULL) {
					msg_err_task ("cannot load dkim key %s: %e",
							key, err);
					g_error_free (err);

					lua_pushboolean (L, FALSE);
					return 1;
				}

				rspamd_lru_hash_insert (dkim_module_ctx->dkim_sign_hash,
						g_strdup (key), dkim_key,
						time (NULL), 0);
			}
		}
		else if (keylen > sizeof (PEM_SIG) &&
				strncmp (key, PEM_SIG, sizeof (PEM_SIG) - 1) == 0) {
			/* Pem header found */
			dkim_key = dkim_module_load_key_format (L, task, key, keylen,
					RSPAMD_DKIM_SIGN_KEY_PEM);

			if (dkim_key == NULL) {
				lua_pushboolean (L, FALSE);
				return 1;
			}
		}
		else {
			dkim_key = dkim_module_load_key_format (L, task, key, keylen,
					RSPAMD_DKIM_SIGN_KEY_BASE64);

			if (dkim_key == NULL) {
				lua_pushboolean (L, FALSE);
				return 1;
			}
		}
	}
	else if (rawkey) {
		key = rawkey;
		keylen = rawlen;

		if (keylen > sizeof (PEM_SIG) &&
				strncmp (key, PEM_SIG, sizeof (PEM_SIG) - 1) == 0) {
			/* Pem header found */
			dkim_key = dkim_module_load_key_format (L, task, key, keylen,
					RSPAMD_DKIM_SIGN_KEY_PEM);

			if (dkim_key == NULL) {
				lua_pushboolean (L, FALSE);
				return 1;
			}
		}
		else {
			dkim_key = dkim_module_load_key_format (L, task, key, keylen,
					RSPAMD_DKIM_SIGN_KEY_BASE64);

			if (dkim_key == NULL) {
				lua_pushboolean (L, FALSE);
				return 1;
			}
		}
	}
	else {
		msg_err_task ("neither key nor rawkey are specified");
		lua_pushboolean (L, FALSE);

		return 1;
	}

#undef PEM_SIG

	if (sign_type_str) {
		if (strcmp (sign_type_str, "dkim") == 0) {
			sign_type = RSPAMD_DKIM_NORMAL;
		}
		else if (strcmp (sign_type_str, "arc-sign") == 0) {
			sign_type = RSPAMD_DKIM_ARC_SIG;
			if (arc_idx == 0) {
				lua_settop (L, 0);
				return luaL_error (L, "no arc idx specified");
			}
		}
		else if (strcmp (sign_type_str, "arc-seal") == 0) {
			sign_type = RSPAMD_DKIM_ARC_SEAL;
			if (arc_cv == NULL) {
				lua_settop (L, 0);
				return luaL_error (L, "no arc cv specified");
			}
			if (arc_idx == 0) {
				lua_settop (L, 0);
				return luaL_error (L, "no arc idx specified");
			}
		}
		else {
			lua_settop (L, 0);
			return luaL_error (L, "unknown sign type: %s",
					sign_type_str);
		}
	}

	ctx = rspamd_create_dkim_sign_context (task, dkim_key,
			DKIM_CANON_RELAXED, DKIM_CANON_RELAXED,
			headers, sign_type, &err);

	if (ctx == NULL) {
		msg_err_task ("cannot create sign context: %e",
				err);
		g_error_free (err);

		lua_pushboolean (L, FALSE);
		return 1;
	}

	hdr = rspamd_dkim_sign (task, selector, domain, 0,
			expire, arc_idx, arc_cv, ctx);

	if (hdr) {

		if (!no_cache) {
			rspamd_mempool_set_variable (task->task_pool, "dkim-signature",
					hdr, rspamd_gstring_free_hard);
		}

		lua_pushboolean (L, TRUE);
		lua_pushlstring (L, hdr->str, hdr->len);

		return 2;
	}


	lua_pushboolean (L, FALSE);
	lua_pushnil (L);

	return 2;
}

gint
dkim_module_reconfig (struct rspamd_config *cfg)
{
	struct module_ctx saved_ctx;

	saved_ctx = dkim_module_ctx->ctx;
	rspamd_mempool_delete (dkim_module_ctx->dkim_pool);

	if (dkim_module_ctx->dkim_hash) {
		rspamd_lru_hash_destroy (dkim_module_ctx->dkim_hash);
	}

	if (dkim_module_ctx->dkim_sign_hash) {
		rspamd_lru_hash_destroy (dkim_module_ctx->dkim_sign_hash);
	}

	memset (dkim_module_ctx, 0, sizeof (*dkim_module_ctx));
	dkim_module_ctx->ctx = saved_ctx;
	dkim_module_ctx->dkim_pool = rspamd_mempool_new (rspamd_mempool_suggest_size (), "dkim");
	dkim_module_ctx->sign_headers = default_sign_headers;
	dkim_module_ctx->sign_condition_ref = -1;
	dkim_module_ctx->max_sigs = DEFAULT_MAX_SIGS;

	return dkim_module_config (cfg);
}

/*
 * Parse strict value for domain in format: 'reject_multiplier:deny_multiplier'
 */
static gboolean
dkim_module_parse_strict (const gchar *value, gint *allow, gint *deny)
{
	const gchar *colon;
	gulong val;

	colon = strchr (value, ':');
	if (colon) {
		if (rspamd_strtoul (value, colon - value, &val)) {
			*deny = val;
			colon++;
			if (rspamd_strtoul (colon, strlen (colon), &val)) {
				*allow = val;
				return TRUE;
			}
		}
	}
	return FALSE;
}

static void
dkim_module_check (struct dkim_check_result *res)
{
	gboolean all_done = TRUE;
	const gchar *strict_value;
	struct dkim_check_result *first, *cur = NULL;

	first = res->first;

	DL_FOREACH (first, cur) {
		if (cur->ctx == NULL) {
			continue;
		}

		if (cur->key != NULL && cur->res == -1) {
			cur->res = rspamd_dkim_check (cur->ctx, cur->key, cur->task);

			if (dkim_module_ctx->dkim_domains != NULL) {
				/* Perform strict check */
				if ((strict_value =
						rspamd_match_hash_map (dkim_module_ctx->dkim_domains,
								rspamd_dkim_get_domain (cur->ctx))) != NULL) {
					if (!dkim_module_parse_strict (strict_value, &cur->mult_allow,
							&cur->mult_deny)) {
						cur->mult_allow = dkim_module_ctx->strict_multiplier;
						cur->mult_deny = dkim_module_ctx->strict_multiplier;
					}
				}
			}
		}
	}

	DL_FOREACH (first, cur) {
		if (cur->ctx == NULL) {
			continue;
		}
		if (cur->res == -1) {
			/* Still need a key */
			all_done = FALSE;
		}
	}

	if (all_done) {
		DL_FOREACH (first, cur) {
			const gchar *symbol = NULL, *trace = NULL;
			int symbol_weight = 1;

			if (cur->ctx == NULL) {
				continue;
			}
			if (cur->res == DKIM_REJECT) {
				symbol = dkim_module_ctx->symbol_reject;
				trace = "-";
				symbol_weight = cur->mult_deny * 1.0;
			}
			else if (cur->res == DKIM_CONTINUE) {
				symbol = dkim_module_ctx->symbol_allow;
				trace = "+";
				symbol_weight = cur->mult_allow * 1.0;
			}
			else if (cur->res == DKIM_PERM_ERROR) {
				trace = "~";
				symbol = dkim_module_ctx->symbol_permfail;
			}
			else if (cur->res == DKIM_TRYAGAIN) {
				trace = "?";
				symbol = dkim_module_ctx->symbol_tempfail;
			}

			if (symbol != NULL) {
				const gchar *domain = rspamd_dkim_get_domain (cur->ctx);
				gsize tracelen;
				gchar *tracebuf;

				tracelen = strlen (domain) + 3; /* :<trace>\0 */
				tracebuf = rspamd_mempool_alloc (cur->task->task_pool,
						tracelen);
				rspamd_snprintf (tracebuf, tracelen, "%s:%s", domain, trace);

				rspamd_task_insert_result (cur->task,
						symbol,
						symbol_weight,
						domain);
				rspamd_task_insert_result (cur->task,
						"DKIM_TRACE",
						0.0,
						tracebuf);
			}
		}
		rspamd_session_watcher_pop (res->task->s, res->w);
	}
}

static void
dkim_module_key_handler (rspamd_dkim_key_t *key,
	gsize keylen,
	rspamd_dkim_context_t *ctx,
	gpointer ud,
	GError *err)
{
	struct dkim_check_result *res = ud;
	struct rspamd_task *task;

	task = res->task;

	if (key != NULL) {
		/*
		 * We actually receive key with refcount = 1, so we just assume that
		 * lru hash owns this object now
		 */
		rspamd_lru_hash_insert (dkim_module_ctx->dkim_hash,
			g_strdup (rspamd_dkim_get_dns_key (ctx)),
			key, res->task->tv.tv_sec, rspamd_dkim_key_get_ttl (key));
		/* Another ref belongs to the check context */
		 res->key = rspamd_dkim_key_ref (key);
		/* Release key when task is processed */
		rspamd_mempool_add_destructor (res->task->task_pool,
				dkim_module_key_dtor, res->key);
	}
	else {
		/* Insert tempfail symbol */
		msg_info_task ("cannot get key for domain %s: %e",
				rspamd_dkim_get_dns_key (ctx), err);

		if (err != NULL) {
			if (err->code == DKIM_SIGERROR_NOKEY) {
				res->res = DKIM_TRYAGAIN;
			}
			else {
				res->res = DKIM_PERM_ERROR;
			}
		}
	}

	if (err) {
		g_error_free (err);
	}

	dkim_module_check (res);
}

static void
dkim_symbol_callback (struct rspamd_task *task, void *unused)
{
	GPtrArray *hlist;
	rspamd_dkim_context_t *ctx;
	rspamd_dkim_key_t *key;
	GError *err = NULL;
	struct rspamd_mime_header *rh;
	struct dkim_check_result *res = NULL, *cur;
	guint checked = 0, i, *dmarc_checks;

	/* Allow dmarc */
	dmarc_checks = rspamd_mempool_get_variable (task->task_pool,
			RSPAMD_MEMPOOL_DMARC_CHECKS);

	if (dmarc_checks) {
		(*dmarc_checks) ++;
	}
	else {
		dmarc_checks = rspamd_mempool_alloc (task->task_pool,
				sizeof (*dmarc_checks));
		*dmarc_checks = 1;
		rspamd_mempool_set_variable (task->task_pool,
				RSPAMD_MEMPOOL_DMARC_CHECKS,
				dmarc_checks, NULL);
	}

	/* First check if plugin should be enabled */
	if ((!dkim_module_ctx->check_authed && task->user != NULL)
			|| (!dkim_module_ctx->check_local &&
					rspamd_inet_address_is_local (task->from_addr, TRUE))) {
		msg_info_task ("skip DKIM checks for local networks and authorized users");
		return;
	}
	/* Check whitelist */
	if (rspamd_match_radix_map_addr (dkim_module_ctx->whitelist_ip,
			task->from_addr) != NULL) {
		msg_info_task ("skip DKIM checks for whitelisted address");
		return;
	}

	/* Now check if a message has its signature */
	hlist = rspamd_message_get_header_array (task,
			RSPAMD_DKIM_SIGNHEADER,
			FALSE);
	if (hlist != NULL && hlist->len > 0) {
		msg_debug_task ("dkim signature found");

		PTR_ARRAY_FOREACH (hlist, i, rh) {
			if (rh->decoded == NULL || rh->decoded[0] == '\0') {
				msg_info_task ("<%s> cannot load empty DKIM context",
						task->message_id);
				continue;
			}

			if (res == NULL) {
				res = rspamd_mempool_alloc0 (task->task_pool, sizeof (*res));
				res->prev = res;
				res->w = rspamd_session_get_watcher (task->s);
				cur = res;
			}
			else {
				cur = rspamd_mempool_alloc0 (task->task_pool, sizeof (*res));
			}

			cur->first = res;
			cur->res = -1;
			cur->w = res->w;
			cur->task = task;
			cur->mult_allow = 1.0;
			cur->mult_deny = 1.0;

			ctx = rspamd_create_dkim_context (rh->decoded,
					task->task_pool,
					dkim_module_ctx->time_jitter,
					RSPAMD_DKIM_NORMAL,
					&err);

			if (ctx == NULL) {
				if (err != NULL) {
					msg_info_task ("<%s> cannot parse DKIM context: %e",
							task->message_id, err);
					g_error_free (err);
					err = NULL;
				}
				else {
					msg_info_task ("<%s> cannot parse DKIM context: "
							"unknown error",
							task->message_id);
				}

				continue;
			}
			else {
				/* Get key */

				cur->ctx = ctx;

				if (dkim_module_ctx->trusted_only &&
						(dkim_module_ctx->dkim_domains == NULL ||
								rspamd_match_hash_map (dkim_module_ctx->dkim_domains,
										rspamd_dkim_get_domain (ctx)) == NULL)) {
					msg_debug_task ("skip dkim check for %s domain",
							rspamd_dkim_get_domain (ctx));

					continue;
				}

				key = rspamd_lru_hash_lookup (dkim_module_ctx->dkim_hash,
						rspamd_dkim_get_dns_key (ctx),
						task->tv.tv_sec);

				if (key != NULL) {
					cur->key = rspamd_dkim_key_ref (key);
					/* Release key when task is processed */
					rspamd_mempool_add_destructor (task->task_pool,
							dkim_module_key_dtor, cur->key);
				}
				else {
					rspamd_get_dkim_key (ctx,
							task,
							dkim_module_key_handler,
							cur);
				}
			}

			if (res != cur) {
				DL_APPEND (res, cur);
			}

			checked ++;

			if (checked > dkim_module_ctx->max_sigs) {
				msg_info_task ("message has multiple signatures but we"
						" stopped after %d checked signatures as limit"
						" is reached", checked);
				break;
			}
		}
	}
	else {
		rspamd_task_insert_result (task,
				dkim_module_ctx->symbol_na,
				1.0,
				NULL);
	}

	if (res != NULL) {
		rspamd_session_watcher_push (task->s);
		dkim_module_check (res);
	}
}

static void
dkim_sign_callback (struct rspamd_task *task, void *unused)
{
	lua_State *L;
	struct rspamd_task **ptask;
	gboolean sign = FALSE;
	gint err_idx;
	gint64 arc_idx = 0;
	gsize len;
	GString *tb, *hdr;
	GError *err = NULL;
	const gchar *selector = NULL, *domain = NULL, *key = NULL, *key_type = NULL,
			*sign_type_str = NULL, *lru_key, *arc_cv = NULL;
	rspamd_dkim_sign_context_t *ctx;
	rspamd_dkim_sign_key_t *dkim_key;
	enum rspamd_dkim_sign_key_type key_sign_type = RSPAMD_DKIM_SIGN_KEY_FILE;
	enum rspamd_dkim_type sign_type = RSPAMD_DKIM_NORMAL;
	guchar h[rspamd_cryptobox_HASHBYTES],
		hex_hash[rspamd_cryptobox_HASHBYTES * 2 + 1];

	if (dkim_module_ctx->sign_condition_ref != -1) {
		sign = FALSE;
		L = task->cfg->lua_state;
		lua_pushcfunction (L, &rspamd_lua_traceback);
		err_idx = lua_gettop (L);

		lua_rawgeti (L, LUA_REGISTRYINDEX, dkim_module_ctx->sign_condition_ref);
		ptask = lua_newuserdata (L, sizeof (struct rspamd_task *));
		*ptask = task;
		rspamd_lua_setclass (L, "rspamd{task}", -1);

		if (lua_pcall (L, 1, 1, err_idx) != 0) {
			tb = lua_touserdata (L, -1);
			msg_err_task ("call to user extraction script failed: %v", tb);
			g_string_free (tb, TRUE);
		}
		else {
			if (lua_istable (L, -1)) {
				/*
				 * Get the following elements:
				 * - selector
				 * - domain
				 * - key
				 */
				if (!rspamd_lua_parse_table_arguments (L, -1, &err,
						"*key=V;*domain=S;*selector=S;type=S;key_type=S;"
								"sign_type=S;arc_cv=S;arc_idx=I",
						&len, &key, &domain, &selector,
						&key_type, &key_type, &sign_type_str, &arc_cv,
						&arc_idx)) {
					msg_err_task ("invalid return value from sign condition: %e",
							err);
					g_error_free (err);

					return;
				}

				if (key_type) {
					if (strcmp (key_type, "file") == 0) {
						key_sign_type = RSPAMD_DKIM_SIGN_KEY_FILE;
					}
					else if (strcmp (key_type, "base64") == 0) {
						key_sign_type = RSPAMD_DKIM_SIGN_KEY_BASE64;
					}
					else if (strcmp (key_type, "pem") == 0) {
						key_sign_type = RSPAMD_DKIM_SIGN_KEY_PEM;
					}
					else if (strcmp (key_type, "der") == 0 ||
							strcmp (key_type, "raw") == 0) {
						key_sign_type = RSPAMD_DKIM_SIGN_KEY_DER;
					}
					else {
						lua_settop (L, 0);
						luaL_error (L, "unknown key type: %s",
								key_type);

						return;
					}
				}

				if (sign_type_str) {
					if (strcmp (sign_type_str, "dkim") == 0) {
						sign_type = RSPAMD_DKIM_NORMAL;
					}
					else if (strcmp (sign_type_str, "arc-sign") == 0) {
						sign_type = RSPAMD_DKIM_ARC_SIG;
						if (arc_idx == 0) {
							lua_settop (L, 0);
							luaL_error (L, "no arc idx specified");

							return;
						}
					}
					else if (strcmp (sign_type_str, "arc-seal") == 0) {
						sign_type = RSPAMD_DKIM_ARC_SEAL;
						if (arc_cv == NULL) {
							lua_settop (L, 0);
							luaL_error (L, "no arc cv specified");

							return;
						}
						if (arc_idx == 0) {
							lua_settop (L, 0);
							luaL_error (L, "no arc idx specified");

							return;
						}
					}
					else {
						lua_settop (L, 0);
						luaL_error (L, "unknown sign type: %s",
								sign_type_str);

						return;
					}
				}

				if (key_sign_type == RSPAMD_DKIM_SIGN_KEY_FILE) {

					dkim_key = rspamd_lru_hash_lookup (
							dkim_module_ctx->dkim_sign_hash,
							key, time (NULL));
					lru_key = key;
				}
				else {
					/* Prehash */
					memset (hex_hash, 0, sizeof (hex_hash));
					rspamd_cryptobox_hash (h, key, len, NULL, 0);
					rspamd_encode_hex_buf (h, sizeof (h),
							hex_hash, sizeof (hex_hash));
					dkim_key = rspamd_lru_hash_lookup (
							dkim_module_ctx->dkim_sign_hash,
							hex_hash, time (NULL));
					lru_key = hex_hash;
				}

				if (dkim_key == NULL) {
					dkim_key = rspamd_dkim_sign_key_load (key, len,
							key_sign_type, &err);

					if (dkim_key == NULL) {
						msg_err_task ("cannot load dkim key %s: %e",
								lru_key, err);
						g_error_free (err);

						return;
					}

					rspamd_lru_hash_insert (dkim_module_ctx->dkim_sign_hash,
							g_strdup (lru_key), dkim_key,
							time (NULL), 0);
				}
				else if (rspamd_dkim_sign_key_maybe_invalidate (dkim_key,
						key_sign_type, key, len)) {
					/*
					 * Invalidate and reload DKIM key,
					 * removal from lru cache also cleanup the key and value
					 */

					rspamd_lru_hash_remove (dkim_module_ctx->dkim_sign_hash,
							lru_key);
					dkim_key = rspamd_dkim_sign_key_load (key, len,
							key_sign_type, &err);

					if (dkim_key == NULL) {
						msg_err_task ("cannot load dkim key %s: %e",
								lru_key, err);
						g_error_free (err);

						return;
					}

					rspamd_lru_hash_insert (dkim_module_ctx->dkim_sign_hash,
							g_strdup (lru_key), dkim_key,
							time (NULL), 0);
				}

				ctx = rspamd_create_dkim_sign_context (task, dkim_key,
						DKIM_CANON_RELAXED, DKIM_CANON_RELAXED,
						dkim_module_ctx->sign_headers,
						sign_type,
						&err);

				if (ctx == NULL) {
					msg_err_task ("cannot create sign context: %e",
							err);
					g_error_free (err);

					return;
				}

				hdr = rspamd_dkim_sign (task, selector, domain, 0, 0,
						arc_idx, arc_cv,
						ctx);

				if (hdr) {
					rspamd_mempool_set_variable (task->task_pool,
							"dkim-signature",
							hdr, rspamd_gstring_free_hard);
				}

				sign = TRUE;
			}
			else {
				sign = FALSE;
			}
		}

		/* Result + error function */
		lua_settop (L, 0);

		if (!sign) {
			msg_debug_task ("skip signing as dkim condition callback returned"
					" false");
			return;
		}
	}
}

struct rspamd_dkim_lua_verify_cbdata {
	rspamd_dkim_context_t *ctx;
	struct rspamd_task *task;
	lua_State *L;
	rspamd_dkim_key_t *key;
	gint cbref;
};

static void
dkim_module_lua_push_verify_result (struct rspamd_dkim_lua_verify_cbdata *cbd,
		gint code, GError *err)
{
	struct rspamd_task **ptask, *task;
	const gchar *error_str = "unknown error";
	gboolean success = FALSE;

	task = cbd->task;

	switch (code) {
	case DKIM_CONTINUE:
		error_str = NULL;
		success = TRUE;
		break;
	case DKIM_REJECT:
		if (err) {
			error_str = err->message;
		}
		else {
			error_str = "reject";
		}
		break;
	case DKIM_TRYAGAIN:
		if (err) {
			error_str = err->message;
		}
		else {
			error_str = "tempfail";
		}
		break;
	case DKIM_NOTFOUND:
		if (err) {
			error_str = err->message;
		}
		else {
			error_str = "not found";
		}
		break;
	case DKIM_RECORD_ERROR:
		if (err) {
			error_str = err->message;
		}
		else {
			error_str = "bad record";
		}
		break;
	case DKIM_PERM_ERROR:
		if (err) {
			error_str = err->message;
		}
		else {
			error_str = "permanent error";
		}
		break;
	default:
		break;
	}

	lua_rawgeti (cbd->L, LUA_REGISTRYINDEX, cbd->cbref);
	ptask = lua_newuserdata (cbd->L, sizeof (*ptask));
	*ptask = task;
	lua_pushboolean (cbd->L, success);
	lua_pushstring (cbd->L, error_str);

	if (cbd->ctx) {
		lua_pushstring (cbd->L, rspamd_dkim_get_domain (cbd->ctx));
	}
	else {
		lua_pushnil (cbd->L);
	}

	if (lua_pcall (cbd->L, 4, 0, 0) != 0) {
		msg_err_task ("call to verify callback failed: %s",
				lua_tostring (cbd->L, -1));
		lua_pop (cbd->L, 1);
	}

	luaL_unref (cbd->L, LUA_REGISTRYINDEX, cbd->cbref);
}

static void
dkim_module_lua_on_key (rspamd_dkim_key_t *key,
		gsize keylen,
		rspamd_dkim_context_t *ctx,
		gpointer ud,
		GError *err)
{
	struct rspamd_dkim_lua_verify_cbdata *cbd = ud;
	struct rspamd_task *task;
	gint ret;

	task = cbd->task;

	if (key != NULL) {
		/*
		 * We actually receive key with refcount = 1, so we just assume that
		 * lru hash owns this object now
		 */
		rspamd_lru_hash_insert (dkim_module_ctx->dkim_hash,
				g_strdup (rspamd_dkim_get_dns_key (ctx)),
				key, cbd->task->tv.tv_sec, rspamd_dkim_key_get_ttl (key));
		/* Another ref belongs to the check context */
		cbd->key = rspamd_dkim_key_ref (key);
		/* Release key when task is processed */
		rspamd_mempool_add_destructor (cbd->task->task_pool,
				dkim_module_key_dtor, cbd->key);
	}
	else {
		/* Insert tempfail symbol */
		msg_info_task ("cannot get key for domain %s: %e",
				rspamd_dkim_get_dns_key (ctx), err);

		if (err != NULL) {
			if (err->code == DKIM_SIGERROR_NOKEY) {
				dkim_module_lua_push_verify_result (cbd, DKIM_TRYAGAIN, err);
			}
			else {
				dkim_module_lua_push_verify_result (cbd, DKIM_PERM_ERROR, err);
			}
		}
		else {
			dkim_module_lua_push_verify_result (cbd, DKIM_TRYAGAIN, NULL);
		}

		if (err) {
			g_error_free (err);
		}

		return;
	}

	ret = rspamd_dkim_check (cbd->ctx, cbd->key, cbd->task);
	dkim_module_lua_push_verify_result (cbd, ret, NULL);
}

static gint
lua_dkim_verify_handler (lua_State *L)
{
	struct rspamd_task *task = lua_check_task (L, 1);
	const gchar *sig = luaL_checkstring (L, 2);
	rspamd_dkim_context_t *ctx;
	struct rspamd_dkim_lua_verify_cbdata *cbd;
	rspamd_dkim_key_t *key;
	gint ret;
	GError *err = NULL;
	const gchar *type_str = NULL;
	enum rspamd_dkim_type type = RSPAMD_DKIM_NORMAL;

	if (task && sig && lua_isfunction (L, 3)) {
		if (lua_isstring (L, 4)) {
			type_str = lua_tostring (L, 4);

			if (type_str) {
				if (strcmp (type_str, "dkim") == 0) {
					type = RSPAMD_DKIM_NORMAL;
				}
				else if (strcmp (type_str, "arc-sign") == 0) {
					type = RSPAMD_DKIM_ARC_SIG;
				}
				else if (strcmp (type_str, "arc-seal") == 0) {
					type = RSPAMD_DKIM_ARC_SEAL;
				}
				else {
					lua_settop (L, 0);
					return luaL_error (L, "unknown sign type: %s",
							type_str);
				}
			}
		}

		ctx = rspamd_create_dkim_context (sig,
				task->task_pool,
				dkim_module_ctx->time_jitter,
				type,
				&err);

		if (ctx == NULL) {
			lua_pushboolean (L, false);

			if (err) {
				lua_pushstring (L, err->message);
				g_error_free (err);
			}
			else {
				lua_pushstring (L, "unknown error");
			}

			return 2;
		}

		cbd = rspamd_mempool_alloc (task->task_pool, sizeof (*cbd));
		cbd->L = L;
		cbd->task = task;
		lua_pushvalue (L, 3);
		cbd->cbref = luaL_ref (L, LUA_REGISTRYINDEX);
		cbd->ctx = ctx;
		cbd->key = NULL;

		key = rspamd_lru_hash_lookup (dkim_module_ctx->dkim_hash,
				rspamd_dkim_get_dns_key (ctx),
				task->tv.tv_sec);

		if (key != NULL) {
			cbd->key = rspamd_dkim_key_ref (key);
			/* Release key when task is processed */
			rspamd_mempool_add_destructor (task->task_pool,
					dkim_module_key_dtor, cbd->key);
			ret = rspamd_dkim_check (cbd->ctx, cbd->key, cbd->task);
			dkim_module_lua_push_verify_result (cbd, ret, NULL);
		}
		else {
			rspamd_get_dkim_key (ctx,
					task,
					dkim_module_lua_on_key,
					cbd);
		}
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	lua_pushboolean (L, TRUE);
	lua_pushnil (L);

	return 2;
}

static gint
lua_dkim_canonicalize_handler (lua_State *L)
{
	gsize nlen, vlen;
	const gchar *hname = luaL_checklstring (L, 1, &nlen),
		*hvalue = luaL_checklstring (L, 2, &vlen);
	static gchar st_buf[8192];
	gchar *buf;
	guint inlen;
	gboolean allocated = FALSE;
	goffset r;

	if (hname && hvalue && nlen > 0) {
		inlen = nlen + vlen + sizeof (":" CRLF);

		if (inlen > sizeof (st_buf)) {
			buf = g_malloc (inlen);
			allocated = TRUE;
		}
		else {
			/* Faster */
			buf = st_buf;
		}

		r = rspamd_dkim_canonize_header_relaxed_str (hname, hvalue, buf, inlen);

		if (r == -1) {
			lua_pushnil (L);
		}
		else {
			lua_pushlstring (L, buf, r);
		}

		if (allocated) {
			g_free (buf);
		}
	}
	else {
		return luaL_error (L, "invalid arguments");
	}

	return 1;
}
