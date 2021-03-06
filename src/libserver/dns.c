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

#include "config.h"
#include "dns.h"
#include "rspamd.h"
#include "utlist.h"
#include "uthash.h"
#include "rdns_event.h"

static struct rdns_upstream_elt* rspamd_dns_select_upstream (const char *name,
		size_t len, void *ups_data);
static struct rdns_upstream_elt* rspamd_dns_select_upstream_retransmit (
		const char *name,
		size_t len, void *ups_data);
static void rspamd_dns_upstream_ok (struct rdns_upstream_elt *elt,
		void *ups_data);
static void rspamd_dns_upstream_fail (struct rdns_upstream_elt *elt,
		void *ups_data);
static unsigned int rspamd_dns_upstream_count (void *ups_data);

static struct rdns_upstream_context rspamd_ups_ctx = {
		.select = rspamd_dns_select_upstream,
		.select_retransmit = rspamd_dns_select_upstream_retransmit,
		.ok = rspamd_dns_upstream_ok,
		.fail = rspamd_dns_upstream_fail,
		.count = rspamd_dns_upstream_count,
		.data = NULL
};

struct rspamd_dns_request_ud {
	struct rspamd_async_session *session;
	dns_callback_type cb;
	gpointer ud;
	rspamd_mempool_t *pool;
	struct rdns_request *req;
};

static void
rspamd_dns_fin_cb (gpointer arg)
{
	struct rspamd_dns_request_ud *reqdata = (struct rspamd_dns_request_ud *)arg;

	rdns_request_release (reqdata->req);

	if (reqdata->pool == NULL) {
		g_free (reqdata);
	}
}

static void
rspamd_dns_callback (struct rdns_reply *reply, gpointer ud)
{
	struct rspamd_dns_request_ud *reqdata = ud;

	reqdata->cb (reply, reqdata->ud);

	if (reqdata->session) {
		/*
		 * Ref event to avoid double unref by
		 * event removing
		 */
		rdns_request_retain (reply->request);
		rspamd_session_remove_event (reqdata->session, rspamd_dns_fin_cb, reqdata);
	}
	else if (reqdata->pool == NULL) {
		g_free (reqdata);
	}
}

gboolean
make_dns_request (struct rspamd_dns_resolver *resolver,
	struct rspamd_async_session *session,
	rspamd_mempool_t *pool,
	dns_callback_type cb,
	gpointer ud,
	enum rdns_request_type type,
	const char *name)
{
	struct rdns_request *req;
	struct rspamd_dns_request_ud *reqdata = NULL;

	g_assert (resolver != NULL);

	if (resolver->r == NULL) {
		return FALSE;
	}

	if (pool != NULL) {
		reqdata =
			rspamd_mempool_alloc (pool, sizeof (struct rspamd_dns_request_ud));
	}
	else {
		reqdata = g_malloc (sizeof (struct rspamd_dns_request_ud));
	}
	reqdata->pool = pool;
	reqdata->session = session;
	reqdata->cb = cb;
	reqdata->ud = ud;

	req = rdns_make_request_full (resolver->r, rspamd_dns_callback, reqdata,
			resolver->request_timeout, resolver->max_retransmits, 1, name,
			type);
	reqdata->req = req;

	if (session) {
		if (req != NULL) {
			rspamd_session_add_event (session,
					(event_finalizer_t)rspamd_dns_fin_cb,
					reqdata,
					g_quark_from_static_string ("dns resolver"));
		}
	}

	if (req == NULL) {
		if (pool == NULL) {
			g_free (reqdata);
		}
		return FALSE;
	}

	return TRUE;
}

static gboolean
make_dns_request_task_common (struct rspamd_task *task,
	dns_callback_type cb,
	gpointer ud,
	enum rdns_request_type type,
	const char *name,
	gboolean forced)
{
	gboolean ret;

	if (!forced && task->dns_requests >= task->cfg->dns_max_requests) {
		return FALSE;
	}

	ret = make_dns_request (task->resolver, task->s, task->task_pool, cb, ud,
			type, name);

	if (ret) {
		task->dns_requests ++;

		if (!forced && task->dns_requests >= task->cfg->dns_max_requests) {
			msg_info_task ("<%s> stop resolving on reaching %ud requests",
					task->message_id, task->dns_requests);
		}
	}

	return ret;
}

gboolean
make_dns_request_task (struct rspamd_task *task,
	dns_callback_type cb,
	gpointer ud,
	enum rdns_request_type type,
	const char *name)
{
	return make_dns_request_task_common (task, cb, ud, type, name, FALSE);
}

gboolean
make_dns_request_task_forced (struct rspamd_task *task,
	dns_callback_type cb,
	gpointer ud,
	enum rdns_request_type type,
	const char *name)
{
	return make_dns_request_task_common (task, cb, ud, type, name, TRUE);
}

static void rspamd_rnds_log_bridge (
		void *log_data,
		enum rdns_log_level level,
		const char *function,
		const char *format,
		va_list args)
{
	rspamd_logger_t *logger = log_data;

	rspamd_common_logv (logger, (GLogLevelFlags)level, "rdns", NULL,
			function, format, args);
}

static void
rspamd_dns_server_init (struct upstream *up, guint idx, gpointer ud)
{
	struct rspamd_dns_resolver *r = ud;
	rspamd_inet_addr_t *addr;
	void *serv;
	struct rdns_upstream_elt *elt;

	addr = rspamd_upstream_addr (up);

	if (r->cfg) {
		serv = rdns_resolver_add_server (r->r, rspamd_inet_address_to_string (addr),
				rspamd_inet_address_get_port (addr), 0, r->cfg->dns_io_per_server);
	}
	else {
		serv = rdns_resolver_add_server (r->r, rspamd_inet_address_to_string (addr),
				rspamd_inet_address_get_port (addr), 0, 8);
	}

	g_assert (serv != NULL);

	elt = g_malloc0 (sizeof (*elt));
	elt->server = serv;
	elt->lib_data = up;

	rspamd_upstream_set_data (up, elt);
}

static void
rspamd_dns_server_reorder (struct upstream *up, guint idx, gpointer ud)
{
	struct rspamd_dns_resolver *r = ud;

	rspamd_upstream_set_weight (up, rspamd_upstreams_count (r->ups) - idx + 1);
}

static bool
rspamd_dns_resolv_conf_on_server (struct rdns_resolver *resolver,
		const char *name, unsigned int port,
		int priority, unsigned int io_cnt, void *ud)
{
	struct rspamd_dns_resolver *dns_resolver = ud;

	return rspamd_upstreams_add_upstream (dns_resolver->ups, name, port,
			RSPAMD_UPSTREAM_PARSE_NAMESERVER,
			NULL);
}

struct rspamd_dns_resolver *
dns_resolver_init (rspamd_logger_t *logger,
	struct event_base *ev_base,
	struct rspamd_config *cfg)
{
	struct rspamd_dns_resolver *dns_resolver;

	dns_resolver = g_malloc0 (sizeof (struct rspamd_dns_resolver));
	dns_resolver->ev_base = ev_base;
	if (cfg != NULL) {
		dns_resolver->request_timeout = cfg->dns_timeout;
		dns_resolver->max_retransmits = cfg->dns_retransmits;
	}
	else {
		dns_resolver->request_timeout = 1;
		dns_resolver->max_retransmits = 2;
	}

	dns_resolver->r = rdns_resolver_new ();
	rdns_bind_libevent (dns_resolver->r, dns_resolver->ev_base);

	if (cfg != NULL) {
		rdns_resolver_set_log_level (dns_resolver->r, cfg->log_level);
		dns_resolver->cfg = cfg;
		rdns_resolver_set_dnssec (dns_resolver->r, cfg->enable_dnssec);

		if (cfg->nameservers == NULL) {
			/* Parse resolv.conf */
			dns_resolver->ups = rspamd_upstreams_create (cfg->ups_ctx);
			rspamd_upstreams_set_flags (dns_resolver->ups,
					RSPAMD_UPSTREAM_FLAG_NORESOLVE);
			rspamd_upstreams_set_rotation (dns_resolver->ups,
					RSPAMD_UPSTREAM_MASTER_SLAVE);

			if (!rdns_resolver_parse_resolv_conf_cb (dns_resolver->r,
					"/etc/resolv.conf",
					rspamd_dns_resolv_conf_on_server,
					dns_resolver)) {
				msg_err ("cannot parse resolv.conf and no nameservers defined, "
						"so no ways to resolve addresses");
				rdns_resolver_release (dns_resolver->r);
				dns_resolver->r = NULL;

				return dns_resolver;
			}

			/* Use normal resolv.conf rules */
			rspamd_upstreams_foreach (dns_resolver->ups, rspamd_dns_server_reorder,
					dns_resolver);
		}
		else {
			dns_resolver->ups = rspamd_upstreams_create (cfg->ups_ctx);
			rspamd_upstreams_set_flags (dns_resolver->ups,
					RSPAMD_UPSTREAM_FLAG_NORESOLVE);

			if (!rspamd_upstreams_from_ucl (dns_resolver->ups, cfg->nameservers,
					53, dns_resolver)) {
				msg_err_config ("cannot parse DNS nameservers definitions");
				rdns_resolver_release (dns_resolver->r);
				dns_resolver->r = NULL;

				return dns_resolver;
			}
		}

		rspamd_upstreams_foreach (dns_resolver->ups, rspamd_dns_server_init,
				dns_resolver);
		rdns_resolver_set_upstream_lib (dns_resolver->r, &rspamd_ups_ctx,
				dns_resolver->ups);
		cfg->dns_resolver = dns_resolver;
	}

	rdns_resolver_set_logger (dns_resolver->r, rspamd_rnds_log_bridge, logger);
	rdns_resolver_init (dns_resolver->r);

	return dns_resolver;
}


static struct rdns_upstream_elt*
rspamd_dns_select_upstream (const char *name,
		size_t len, void *ups_data)
{
	struct upstream_list *ups = ups_data;
	struct upstream *up;

	up = rspamd_upstream_get (ups, RSPAMD_UPSTREAM_ROUND_ROBIN, name, len);

	if (up) {
		msg_debug ("select %s", rspamd_upstream_name (up));

		return rspamd_upstream_get_data (up);
	}

	return NULL;
}

static struct rdns_upstream_elt*
rspamd_dns_select_upstream_retransmit (
		const char *name,
		size_t len, void *ups_data)
{
	struct upstream_list *ups = ups_data;
	struct upstream *up;

	up = rspamd_upstream_get_forced (ups, RSPAMD_UPSTREAM_RANDOM, name, len);

	if (up) {
		msg_debug ("select forced %s", rspamd_upstream_name (up));

		return rspamd_upstream_get_data (up);
	}

	return NULL;
}

static void
rspamd_dns_upstream_ok (struct rdns_upstream_elt *elt,
		void *ups_data)
{
	struct upstream *up = elt->lib_data;

	rspamd_upstream_ok (up);
}

static void
rspamd_dns_upstream_fail (struct rdns_upstream_elt *elt,
		void *ups_data)
{
	struct upstream *up = elt->lib_data;

	rspamd_upstream_fail (up);
}

static unsigned int
rspamd_dns_upstream_count (void *ups_data)
{
	struct upstream_list *ups = ups_data;

	return rspamd_upstreams_alive (ups);
}
