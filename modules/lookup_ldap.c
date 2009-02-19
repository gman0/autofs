/*
 * lookup_ldap.c - Module for Linux automountd to access automount
 *		   maps in LDAP directories.
 *
 *   Copyright 2001-2003 Ian Kent <raven@themaw.net>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 675 Mass Ave, Cambridge MA 02139,
 *   USA; either version 2 of the License, or (at your option) any later
 *   version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <lber.h>
#include <ldap.h>

#define MODULE_LOOKUP
#include "automount.h"
#include "nsswitch.h"
#include "lookup_ldap.h"

#define MAPFMT_DEFAULT "sun"

#define MODPREFIX "lookup(ldap): "

int lookup_version = AUTOFS_LOOKUP_VERSION;	/* Required by protocol */

static struct ldap_schema common_schema[] = {
	{"nisMap", "nisMapName", "nisObject", "cn", "nisMapEntry"},
	{"automountMap", "ou", "automount", "cn", "automountInformation"},
	{"automountMap", "automountMapName", "automount", "automountKey", "automountInformation"},
};
static unsigned int common_schema_count = sizeof(common_schema)/sizeof(struct ldap_schema);

struct ldap_search_params {
	struct autofs_point *ap;
	LDAP *ldap;
	char *query, **attrs;
	struct berval *cookie;
	int morePages;
	ber_int_t totalCount;
	LDAPMessage *result;
	time_t age;
};

static LDAP *auth_init(unsigned logopt, const char *, struct lookup_context *);

#ifndef HAVE_LDAP_CREATE_PAGE_CONTROL
int ldap_create_page_control(LDAP *ldap, ber_int_t pagesize,
			     struct berval *cookie, char isCritical,
			     LDAPControl **output)
{
	BerElement *ber;
	int rc;

	if (!ldap || !output)
		return LDAP_PARAM_ERROR;

	ber = ber_alloc_t(LBER_USE_DER);
	if (!ber)
		return LDAP_NO_MEMORY;

	if (ber_printf(ber, "{io}", pagesize,
			(cookie && cookie->bv_val) ? cookie->bv_val : "",
			(cookie && cookie->bv_val) ? cookie->bv_len : 0)
				== LBER_ERROR) {
		ber_free(ber, 1);
		return LDAP_ENCODING_ERROR;
	}

	rc = ldap_create_control(LDAP_CONTROL_PAGEDRESULTS, ber, isCritical, output);

	return rc;
}
#endif /* HAVE_LDAP_CREATE_PAGE_CONTROL */

#ifndef HAVE_LDAP_PARSE_PAGE_CONTROL
int ldap_parse_page_control(LDAP *ldap, LDAPControl **controls,
			    ber_int_t *totalcount, struct berval **cookie)
{
	int i, rc;
	BerElement *theBer;
	LDAPControl *listCtrlp;

	for (i = 0; controls[i] != NULL; i++) {
		if (strcmp(controls[i]->ldctl_oid, LDAP_CONTROL_PAGEDRESULTS) == 0) {
			listCtrlp = controls[i];

			theBer = ber_init(&listCtrlp->ldctl_value);
			if (!theBer)
				return LDAP_NO_MEMORY;

			rc = ber_scanf(theBer, "{iO}", totalcount, cookie);
			if (rc == LBER_ERROR) {
				ber_free(theBer, 1);
				return LDAP_DECODING_ERROR;
			}

			ber_free(theBer, 1);
			return LDAP_SUCCESS;
		}
	}

	return LDAP_CONTROL_NOT_FOUND;
}
#endif /* HAVE_LDAP_PARSE_PAGE_CONTROL */

static void uris_mutex_lock(struct lookup_context *ctxt)
{
	int status = pthread_mutex_lock(&ctxt->uris_mutex);
	if (status)
		fatal(status);
	return;
}

static void uris_mutex_unlock(struct lookup_context *ctxt)
{
	int status = pthread_mutex_unlock(&ctxt->uris_mutex);
	if (status)
		fatal(status);
	return;
}

int bind_ldap_anonymous(unsigned logopt, LDAP *ldap, const char *uri, struct lookup_context *ctxt)
{
	int rv;

	if (ctxt->version == 2)
		rv = ldap_simple_bind_s(ldap, ctxt->base, NULL);
	else
		rv = ldap_simple_bind_s(ldap, NULL, NULL);

	if (rv != LDAP_SUCCESS) {
		if (!ctxt->uris) {
			crit(logopt, MODPREFIX
			     "Unable to bind to the LDAP server: "
			     "%s, error %s", ctxt->server ? "" : "(default)",
			     ldap_err2string(rv));
		} else {
			info(logopt, MODPREFIX "Unable to bind to the LDAP server: "
			     "%s, error %s", uri, ldap_err2string(rv));
		}
		return -1;
	}

	return 0;
}

int unbind_ldap_connection(unsigned logopt, LDAP *ldap, struct lookup_context *ctxt)
{
	int rv;

#ifdef WITH_SASL
	/*
	 * The OpenSSL library can't handle having its message and error
	 * string database loaded multiple times and segfaults if the
	 * TLS environment is not reset at the right times. As there
	 * is no ldap_stop_tls call in the openldap library we have
	 * to do the job ourselves, here and in lookup_done when the
	 * module is closed.
	 */
	if (ctxt->use_tls == LDAP_TLS_RELEASE) {
		ERR_remove_state(0);
		ctxt->use_tls = LDAP_TLS_INIT;
	}
	autofs_sasl_unbind(ctxt);
#endif

	rv = ldap_unbind_ext(ldap, NULL, NULL);
	if (rv != LDAP_SUCCESS)
		error(logopt, "unbind failed: %s", ldap_err2string(rv));

	return rv;
}

LDAP *init_ldap_connection(unsigned logopt, const char *uri, struct lookup_context *ctxt)
{
	LDAP *ldap = NULL;
	struct timeval timeout     = { ctxt->timeout, 0 };
	struct timeval net_timeout = { ctxt->network_timeout, 0 };
	int rv;

	ctxt->version = 3;

	/* Initialize the LDAP context. */
	rv = ldap_initialize(&ldap, uri);
	if (rv != LDAP_OPT_SUCCESS) {
		info(logopt, MODPREFIX
		     "couldn't initialize LDAP connection to %s",
		     uri ? uri : "default");
		return NULL;
	}

	/* Use LDAPv3 */
	rv = ldap_set_option(ldap, LDAP_OPT_PROTOCOL_VERSION, &ctxt->version);
	if (rv != LDAP_OPT_SUCCESS) {
		/* fall back to LDAPv2 */
		ldap_unbind_ext(ldap, NULL, NULL);
		rv = ldap_initialize(&ldap, uri);
		if (rv != LDAP_OPT_SUCCESS) {
			crit(logopt, MODPREFIX "couldn't initialize LDAP");
			return NULL;
		}
		ctxt->version = 2;
	}


	if (ctxt->timeout != -1) {
		/* Set synchronous call timeout */
		rv = ldap_set_option(ldap, LDAP_OPT_TIMEOUT, &timeout);
		if (rv != LDAP_OPT_SUCCESS)
			info(logopt, MODPREFIX
			     "failed to set synchronous call timeout to %d",
			     timeout.tv_sec);
	}

	/* Sane network timeout */
	rv = ldap_set_option(ldap, LDAP_OPT_NETWORK_TIMEOUT, &net_timeout);
	if (rv != LDAP_OPT_SUCCESS)
		info(logopt, MODPREFIX "failed to set connection timeout to %d",
		     net_timeout.tv_sec);

#ifdef WITH_SASL
	if (ctxt->use_tls) {
		if (ctxt->version == 2) {
			if (ctxt->tls_required) {
				error(logopt, MODPREFIX
				    "TLS required but connection is version 2");
				ldap_unbind_ext(ldap, NULL, NULL);
				return NULL;
			}
			return ldap;
		}

		rv = ldap_start_tls_s(ldap, NULL, NULL);
		if (rv != LDAP_SUCCESS) {
			unbind_ldap_connection(logopt, ldap, ctxt);
			if (ctxt->tls_required) {
				error(logopt, MODPREFIX
				      "TLS required but START_TLS failed: %s",
				      ldap_err2string(rv));
				return NULL;
			}
			ctxt->use_tls = LDAP_TLS_DONT_USE;
			ldap = init_ldap_connection(logopt, uri, ctxt);
			if (ldap)
				ctxt->use_tls = LDAP_TLS_INIT;
			return ldap;
		}
		ctxt->use_tls = LDAP_TLS_RELEASE;
	}
#endif

	return ldap;
}

static int get_query_dn(unsigned logopt, LDAP *ldap, struct lookup_context *ctxt, const char *class, const char *key)
{
	char buf[MAX_ERR_BUF];
	char *query, *dn, *qdn;
	LDAPMessage *result, *e;
	struct ldap_searchdn *sdns = NULL;
	char *attrs[2];
	struct berval **value;
	int scope;
	int rv, l;

	attrs[0] = (char *) key;
	attrs[1] = NULL;

	if (!ctxt->mapname && !ctxt->base) {
		error(logopt, MODPREFIX "no master map to lookup");
		return 0;
	}

	/* Build a query string. */
	l = strlen("(objectclass=)") + strlen(class) + 1;
	if (ctxt->mapname)
		l += strlen(key) + strlen(ctxt->mapname) + strlen("(&(=))");

	query = malloc(l);
	if (query == NULL) {
		char *estr = strerror_r(errno, buf, sizeof(buf));
		crit(logopt, MODPREFIX "malloc: %s", estr);
		return NSS_STATUS_UNAVAIL;
	}

	/*
	 * If we have a master mapname construct a query using it
	 * otherwise assume the base dn will catch it.
	 */
	if (ctxt->mapname) {
		if (sprintf(query, "(&(objectclass=%s)(%s=%.*s))", class,
		     key, (int) strlen(ctxt->mapname), ctxt->mapname) >= l) {
			debug(logopt,
			      MODPREFIX "error forming query string");
			free(query);
			return 0;
		}
		scope = LDAP_SCOPE_SUBTREE;
	} else {
		if (sprintf(query, "(objectclass=%s)", class) >= l) {
			debug(logopt,
			      MODPREFIX "error forming query string");
			free(query);
			return 0;
		}
		scope = LDAP_SCOPE_SUBTREE;
	}
	query[l] = '\0';

	if (!ctxt->base) {
		sdns = defaults_get_searchdns();
		if (sdns) {
			if (ctxt->sdns)
				defaults_free_searchdns(ctxt->sdns);
			ctxt->sdns = sdns;
		}
	}

	dn = NULL;
	if (!ctxt->sdns) {
		rv = ldap_search_s(ldap, ctxt->base,
				   scope, query, attrs, 0, &result);
		if ((rv != LDAP_SUCCESS) || !result) {
			error(logopt,
			      MODPREFIX "query failed for %s: %s",
			      query, ldap_err2string(rv));
			free(query);
			return 0;
		}

		e = ldap_first_entry(ldap, result);
		if (e && (value = ldap_get_values_len(ldap, e, key))) {
			ldap_value_free_len(value);
			dn = ldap_get_dn(ldap, e);
			debug(logopt, MODPREFIX "found query dn %s", dn);
		} else {
			debug(logopt,
			      MODPREFIX "query succeeded, no matches for %s",
			      query);
			ldap_msgfree(result);
			free(query);
			return 0;
		}
	} else {
		struct ldap_searchdn *this = ctxt->sdns;

		debug(logopt, MODPREFIX "check search base list");

		result = NULL;
		while (this) {
			rv = ldap_search_s(ldap, this->basedn,
					   scope, query, attrs, 0, &result);
			if ((rv == LDAP_SUCCESS) && result) {
				debug(logopt, MODPREFIX
				      "found search base under %s",
				      this->basedn);

				e = ldap_first_entry(ldap, result);
				if (e && (value = ldap_get_values_len(ldap, e, key))) {
					ldap_value_free_len(value);
					dn = ldap_get_dn(ldap, e);
					debug(logopt, MODPREFIX "found query dn %s", dn);
					break;
				} else {
					debug(logopt,
					      MODPREFIX "query succeeded, no matches for %s",
					      query);
					ldap_msgfree(result);
					result = NULL;
				}
			} else {
				error(logopt,
				      MODPREFIX "query failed for search dn %s: %s",
				      this->basedn, ldap_err2string(rv));
			}

			this = this->next;
		}

		if (!result) {
			ldap_msgfree(result);
			error(logopt,
			      MODPREFIX "failed to find query dn under search base dns");
			free(query);
			return 0;
		}
	}

	free(query);
	qdn = strdup(dn);
	ldap_memfree(dn);
	ldap_msgfree(result);
	if (!qdn)
		return 0;

	if (ctxt->qdn)
		free(ctxt->qdn);

	ctxt->qdn = qdn;

	return 1;
}

static struct ldap_schema *alloc_common_schema(struct ldap_schema *s)
{
	struct ldap_schema *schema;
	char *mc, *ma, *ec, *ea, *va;

	mc = strdup(s->map_class);
	if (!mc)
		return NULL;

	ma = strdup(s->map_attr);
	if (!ma) {
		free(mc);
		return NULL;
	}

	ec = strdup(s->entry_class);
	if (!ec) {
		free(mc);
		free(ma);
		return NULL;
	}

	ea = strdup(s->entry_attr);
	if (!ea) {
		free(mc);
		free(ma);
		free(ec);
		return NULL;
	}

	va = strdup(s->value_attr);
	if (!va) {
		free(mc);
		free(ma);
		free(ec);
		free(ea);
		return NULL;
	}

	schema = malloc(sizeof(struct ldap_schema));
	if (!schema) {
		free(mc);
		free(ma);
		free(ec);
		free(ea);
		free(va);
		return NULL;
	}

	schema->map_class = mc;
	schema->map_attr = ma;
	schema->entry_class = ec;
	schema->entry_attr = ea;
	schema->value_attr = va;

	return schema;
}

static int find_query_dn(unsigned logopt, LDAP *ldap, struct lookup_context *ctxt)
{
	struct ldap_schema *schema;
	unsigned int i;

	if (ctxt->schema)
		return 0;

	for (i = 0; i < common_schema_count; i++) {
		const char *class = common_schema[i].map_class;
		const char *key = common_schema[i].map_attr;
		if (get_query_dn(logopt, ldap, ctxt, class, key)) {
			schema = alloc_common_schema(&common_schema[i]);
			if (!schema) {
				error(logopt, MODPREFIX "failed to allocate schema");
				return 0;
			}
			ctxt->schema = schema;
			return 1;
		}
	}

	return 0;
}

static int do_bind(unsigned logopt, LDAP *ldap, const char *uri, struct lookup_context *ctxt)
{
	char *host = NULL, *nhost;
	int rv, need_base = 1;

#ifdef WITH_SASL
	debug(logopt, MODPREFIX "auth_required: %d, sasl_mech %s",
	      ctxt->auth_required, ctxt->sasl_mech);

	if (ctxt->auth_required & (LDAP_AUTH_REQUIRED|LDAP_AUTH_AUTODETECT)) {
		rv = autofs_sasl_bind(logopt, ldap, ctxt);
		debug(logopt, MODPREFIX "autofs_sasl_bind returned %d", rv);
	} else {
		rv = bind_ldap_anonymous(logopt, ldap, uri, ctxt);
		debug(logopt, MODPREFIX "ldap anonymous bind returned %d", rv);
	}
#else
	rv = bind_ldap_anonymous(logopt, ldap, uri, ctxt);
	debug(logopt, MODPREFIX "ldap anonymous bind returned %d", rv);
#endif

	if (rv != 0)
		return 0;

	rv = ldap_get_option(ldap, LDAP_OPT_HOST_NAME, &host);
        if (rv != LDAP_SUCCESS || !host) {
		debug(logopt, "failed to get hostname for connection");
		return 0;
	}

	nhost = strdup(host);
	if (!nhost) {
		debug(logopt, "failed to alloc context for hostname");
		return 0;
	}
	ldap_memfree(host);

	if (!ctxt->cur_host) {
		ctxt->cur_host = nhost;
		/* Check if schema defined in conf first time only */
		ctxt->schema = defaults_get_schema();
	} else {
		/* If connection host has changed update */
		if (strcmp(ctxt->cur_host, nhost)) {
			free(ctxt->cur_host);
			ctxt->cur_host = nhost;
		} else {
			free(nhost);
			need_base = 0;
		}
	}

	if (!need_base)
		return 1;

	/*
	 * If the schema isn't defined in the configuration then check for
	 * presence of a map dn with a the common schema. Then calculate the
	 * base dn for searches.
	 */
	if (!ctxt->schema) {
		if (!find_query_dn(logopt, ldap, ctxt)) {
			warn(logopt,
			      MODPREFIX "failed to find valid query dn");
			return 0;
		}
	} else {
		const char *class = ctxt->schema->map_class;
		const char *key = ctxt->schema->map_attr;
		if (!get_query_dn(logopt, ldap, ctxt, class, key)) {
			error(logopt, MODPREFIX "failed to get query dn");
			return 0;
		}
	}

	return 1;
}

static LDAP *do_connect(unsigned logopt, const char *uri, struct lookup_context *ctxt)
{
	LDAP *ldap;

	ldap = init_ldap_connection(logopt, uri, ctxt);
	if (!ldap)
		return NULL;

	if (!do_bind(logopt, ldap, uri, ctxt)) {
		unbind_ldap_connection(logopt, ldap, ctxt);
		return NULL;
	}

	return ldap;
}

static LDAP *connect_to_server(unsigned logopt, const char *uri, struct lookup_context *ctxt)
{
	LDAP *ldap;

#ifdef WITH_SASL
	/*
	 * Determine which authentication mechanism to use if we require
	 * authentication.
	 */
	if (ctxt->auth_required & (LDAP_AUTH_REQUIRED|LDAP_AUTH_AUTODETECT)) {
		ldap = auth_init(logopt, uri, ctxt);
		if (!ldap && ctxt->auth_required & LDAP_AUTH_AUTODETECT)
			info(logopt,
			     "no authentication mechanisms auto detected.");
		if (!ldap) {
			error(logopt, MODPREFIX
			      "cannot initialize authentication setup");
			return NULL;
		}

		if (!do_bind(logopt, ldap, uri, ctxt)) {
			unbind_ldap_connection(logopt, ldap, ctxt);
			autofs_sasl_dispose(ctxt);
			error(logopt, MODPREFIX "cannot bind to server");
			return NULL;
		}

		return ldap;
	}
#endif

	ldap = do_connect(logopt, uri, ctxt);
	if (!ldap) {
		warn(logopt,
		     MODPREFIX "couldn't connect to server %s",
		     uri ? uri : "default");
		return NULL;
	}

	return ldap;
}

static LDAP *find_server(unsigned logopt, struct lookup_context *ctxt)
{
	LDAP *ldap = NULL;
	struct ldap_uri *this;
	struct list_head *p, *first;

	/* Try each uri in list, add connect fails to tmp list */
	uris_mutex_lock(ctxt);
	if (!ctxt->uri)
		first = ctxt->uris;
	else
		first = &ctxt->uri->list;
	uris_mutex_unlock(ctxt);
	p = first->next;
	while(p != first) {
		/* Skip list head */
		if (p == ctxt->uris) {
			p = p->next;
			continue;
		}
		this = list_entry(p, struct ldap_uri, list);
		debug(logopt, "trying server %s", this->uri);
		ldap = connect_to_server(logopt, this->uri, ctxt);
		if (ldap) {
			info(logopt, "connected to uri %s", this->uri);
			uris_mutex_lock(ctxt);
			ctxt->uri = this;
			uris_mutex_unlock(ctxt);
			break;
		}
		p = p->next;
	}

	return ldap;
}

static LDAP *do_reconnect(unsigned logopt, struct lookup_context *ctxt)
{
	struct ldap_uri *this;
	LDAP *ldap;

	if (ctxt->server || !ctxt->uris) {
		ldap = do_connect(logopt, ctxt->server, ctxt);
#ifdef WITH_SASL
		/* Dispose of the sasl authentication connection and try again. */
		if (!ldap) {
			autofs_sasl_dispose(ctxt);
			ldap = connect_to_server(logopt, ctxt->server, ctxt);
		}
#endif
		return ldap;
	}

	uris_mutex_lock(ctxt);
	this = ctxt->uri;
	uris_mutex_unlock(ctxt);
	ldap = do_connect(logopt, this->uri, ctxt);
#ifdef WITH_SASL
	/*
	 * Dispose of the sasl authentication connection and try the
	 * current server again before trying other servers in the list.
	 */
	if (!ldap) {
		autofs_sasl_dispose(ctxt);
		ldap = connect_to_server(logopt, this->uri, ctxt);
	}
#endif
	if (ldap)
		return ldap;

	/* Failed to connect, try to find a new server */

#ifdef WITH_SASL
	autofs_sasl_dispose(ctxt);
#endif

	/* Current server failed connect, try the rest */
	ldap = find_server(logopt, ctxt);
	if (!ldap)
		error(logopt, MODPREFIX "failed to find available server");

	return ldap;
}

#ifdef WITH_SASL
int get_property(unsigned logopt, xmlNodePtr node, const char *prop, char **value)
{
	xmlChar *ret;
	xmlChar *property = (xmlChar *) prop;

	if (!(ret = xmlGetProp(node, property))) {
		*value = NULL;
		return 0;
	}

	if (!(*value = strdup((char *) ret))) {
		logerr(MODPREFIX "strdup failed with %d", errno);
		xmlFree(ret);
		return -1;
	}

	xmlFree(ret);
	return 0;
}

/*
 *  For plain text, login and digest-md5 authentication types, we need
 *  user and password credentials.
 */
int authtype_requires_creds(const char *authtype)
{
	if (!strncmp(authtype, "PLAIN", strlen("PLAIN")) ||
	    !strncmp(authtype, "DIGEST-MD5", strlen("DIGEST-MD5")) ||
	    !strncmp(authtype, "LOGIN", strlen("LOGIN")))
		return 1;
	return 0;
}

/*
 *  Returns:
 *    -1  --  The permission on the file are not correct or
 *            the xml document was mal-formed
 *     0  --  The file was non-existent
 *            the file was empty
 *            the file contained valid data, which was filled into 
 *            ctxt->sasl_mech, ctxt->user, and ctxt->secret
 *
 *  The idea is that a -1 return value should abort the program.  A 0
 *  return value requires more checking.  If ctxt->authtype is filled in,
 *  then no further action is necessary.  If it is not, the caller is free
 *  to then use another method to determine how to connect to the server.
 */
int parse_ldap_config(unsigned logopt, struct lookup_context *ctxt)
{
	int          ret = 0, fallback = 0;
	unsigned int auth_required = LDAP_AUTH_NOTREQUIRED;
	unsigned int tls_required = 0, use_tls = 0;
	struct stat  st;
	xmlDocPtr    doc = NULL;
	xmlNodePtr   root = NULL;
	char         *authrequired, *auth_conf, *authtype;
	char         *user = NULL, *secret = NULL;
	char         *client_princ = NULL, *client_cc = NULL;
	char	     *usetls, *tlsrequired;

	authtype = user = secret = NULL;

	auth_conf = (char *) defaults_get_auth_conf_file();
	if (!auth_conf) {
		error(logopt,
		      MODPREFIX "failed to get auth config file name.");
		return 0;
	}

	/*
	 *  Here we check that the config file exists, and that we have
	 *  permission to read it.  The XML library does not specify why a
	 *  parse happens to fail, so we have to do all of this checking
	 *  beforehand.
	 */
	memset(&st, 0, sizeof(st));
	if (stat(auth_conf, &st) == -1 || st.st_size == 0) {
		/* Auth config doesn't exist so disable TLS and auth */
		if (errno == ENOENT) {
			ctxt->auth_conf = auth_conf;
			ctxt->use_tls = LDAP_TLS_DONT_USE;
			ctxt->tls_required = LDAP_TLS_DONT_USE;
			ctxt->auth_required = LDAP_AUTH_NOTREQUIRED;
			ctxt->sasl_mech = NULL;
			ctxt->user = NULL;
			ctxt->secret = NULL;
			ctxt->client_princ = NULL;
			return 0;
		}
		error(logopt,
		      MODPREFIX "stat(2) failed with error %s.",
		      strerror(errno));
		return 0;
	}

	if (!S_ISREG(st.st_mode) ||
	    st.st_uid != 0 || st.st_gid != 0 ||
	    (st.st_mode & 0x01ff) != 0600) {
		error(logopt, MODPREFIX
		      "Configuration file %s exists, but is not usable. "
		      "Please make sure that it is owned by root, group "
		      "is root, and the mode is 0600.",
		      auth_conf);
		return -1;
	}

	xmlInitParser();
	doc = xmlParseFile(auth_conf);
	if (!doc) {
		error(logopt, MODPREFIX
		     "xmlParseFile failed for %s.", auth_conf);
		goto out;
	}

	root = xmlDocGetRootElement(doc);
	if (!root) {
		debug(logopt, MODPREFIX
		      "empty xml document (%s).", auth_conf);
		fallback = 1;
		goto out;
	}

	if (xmlStrcmp(root->name, (const xmlChar *)"autofs_ldap_sasl_conf")) {
		error(logopt, MODPREFIX
		      "The root node of the XML document %s is not "
		      "autofs_ldap_sasl_conf.", auth_conf);
		goto out;
	}

	ret = get_property(logopt, root, "usetls", &usetls);
	if (ret != 0) {
		error(logopt,
		      MODPREFIX
		      "Failed read the usetls property from "
		      "the configuration file %s.", auth_conf);
		goto out;
	}

	if (!usetls || ctxt->port == LDAPS_PORT)
		use_tls = LDAP_TLS_DONT_USE;
	else {
		if (!strcasecmp(usetls, "yes"))
			use_tls = LDAP_TLS_INIT;
		else if (!strcasecmp(usetls, "no"))
			use_tls = LDAP_TLS_DONT_USE;
		else {
			error(logopt,
			      MODPREFIX
			      "The usetls property must have value "
			      "\"yes\" or \"no\".");
			ret = -1;
			goto out;
		}
		free(usetls);
	}

	ret = get_property(logopt, root, "tlsrequired", &tlsrequired);
	if (ret != 0) {
		error(logopt,
		      MODPREFIX
		      "Failed read the tlsrequired property from "
		      "the configuration file %s.", auth_conf);
		goto out;
	}

	if (!tlsrequired)
		tls_required = LDAP_TLS_DONT_USE;
	else {
		if (!strcasecmp(tlsrequired, "yes"))
			tls_required = LDAP_TLS_REQUIRED;
		else if (!strcasecmp(tlsrequired, "no"))
			tls_required = LDAP_TLS_DONT_USE;
		else {
			error(logopt,
			      MODPREFIX
			      "The tlsrequired property must have value "
			      "\"yes\" or \"no\".");
			ret = -1;
			goto out;
		}
		free(tlsrequired);
	}

	ret = get_property(logopt, root, "authrequired", &authrequired);
	if (ret != 0) {
		error(logopt,
		      MODPREFIX
		      "Failed read the authrequired property from "
		      "the configuration file %s.", auth_conf);
		goto out;
	}

	if (!authrequired)
		auth_required = LDAP_AUTH_NOTREQUIRED;
	else {
		if (!strcasecmp(authrequired, "yes"))
			auth_required = LDAP_AUTH_REQUIRED;
		else if (!strcasecmp(authrequired, "no"))
			auth_required = LDAP_AUTH_NOTREQUIRED;
		else if (!strcasecmp(authrequired, "autodetect"))
			auth_required = LDAP_AUTH_AUTODETECT;
		else {
			error(logopt,
			      MODPREFIX
			      "The authrequired property must have value "
			      "\"yes\", \"no\" or \"autodetect\".");
			ret = -1;
			goto out;
		}
		free(authrequired);
	}

	ret = get_property(logopt, root, "authtype", &authtype);
	if (ret != 0) {
		error(logopt,
		      MODPREFIX
		      "Failed read the authtype property from the "
		      "configuration file %s.", auth_conf);
		goto out;
	}

	if (authtype && authtype_requires_creds(authtype)) {
		ret = get_property(logopt, root, "user",  &user);
		ret |= get_property(logopt, root, "secret", &secret);
		if (ret != 0 || (!user || !secret)) {
			error(logopt,
			      MODPREFIX
			      "%s authentication type requires a username "
			      "and a secret.  Please fix your configuration "
			      "in %s.", authtype, auth_conf);
			free(authtype);
			if (user)
				free(user);
			if (secret)
				free(secret);

			ret = -1;
			goto out;
		}
	}

	/*
	 * We allow the admin to specify the principal to use for the
	 * client.  The default is "autofsclient/hostname@REALM".
	 */
	(void)get_property(logopt, root, "clientprinc", &client_princ);
	(void)get_property(logopt, root, "credentialcache", &client_cc);

	ctxt->auth_conf = auth_conf;
	ctxt->use_tls = use_tls;
	ctxt->tls_required = tls_required;
	ctxt->auth_required = auth_required;
	ctxt->sasl_mech = authtype;
	if (!authtype && (auth_required & LDAP_AUTH_REQUIRED))
		ctxt->auth_required |= LDAP_AUTH_AUTODETECT;
	ctxt->user = user;
	ctxt->secret = secret;
	ctxt->client_princ = client_princ;
	ctxt->client_cc = client_cc;

	debug(logopt, MODPREFIX
	      "ldap authentication configured with the following options:");
	debug(logopt, MODPREFIX
	      "use_tls: %u, "
	      "tls_required: %u, "
	      "auth_required: %u, "
	      "sasl_mech: %s",
	      use_tls, tls_required, auth_required, authtype);
	debug(logopt, MODPREFIX
	      "user: %s, "
	      "secret: %s, "
	      "client principal: %s "
	      "credential cache: %s",
	      user, secret ? "specified" : "unspecified",
	      client_princ, client_cc);

out:
	xmlFreeDoc(doc);
	xmlCleanupParser();

	if (fallback)
		return 0;

	return ret;
}

/*
 *  Reads in the xml configuration file and parses out the relevant
 *  information.  If there is no configuration file, then we fall back to
 *  trying all supported authentication mechanisms until one works.
 *
 *  Returns ldap connection on success, with authtype, user and secret
 *  filled in as appropriate.  Returns NULL on failre.
 */
static LDAP *auth_init(unsigned logopt, const char *uri, struct lookup_context *ctxt)
{
	int ret;
	LDAP *ldap;

	ldap = init_ldap_connection(logopt, uri, ctxt);
	if (!ldap)
		return NULL;

	/*
	 *  Initialize the sasl library.  It is okay if user and secret
	 *  are NULL, here.
	 *
	 *  The autofs_sasl_init routine will figure out which mechamism
	 *  to use. If kerberos is used, it will also take care to initialize
	 *  the credential cache and the client and service principals.
	 */
	ret = autofs_sasl_init(logopt, ldap, ctxt);
	if (ret)
		return NULL;

	return ldap;
}
#endif

/*
 *  Take an input string as specified in the master map, and break it
 *  down into a server name and basedn.
 */
static int parse_server_string(unsigned logopt, const char *url, struct lookup_context *ctxt)
{
	char buf[MAX_ERR_BUF], *tmp = NULL, proto[9];
	const char *ptr, *name;
	int l, al_len;

	memset(proto, 0, 9);
	ptr = url;

	debug(logopt, MODPREFIX
	      "Attempting to parse LDAP information from string \"%s\".", ptr);

	ctxt->port = LDAP_PORT;
	if (!strncmp(ptr, "ldap:", 5) || !strncmp(ptr, "ldaps:", 6)) {
		if (*(ptr + 4) == 's') {
			ctxt->port = LDAPS_PORT;
			memcpy(proto, ptr, 6);
			strcat(proto, "//");
			ptr += 6;
		} else {
			memcpy(proto, ptr, 5);
			strcat(proto, "//");
			ptr += 5;
		}
	}

	if (!strncmp(ptr, "//", 2)) {
		const char *s = ptr + 2;
		const char *q = NULL;

		/* Isolate the server(s). */
		if ((q = strchr(s, '/'))) {
			l = q - s;
			if (*proto) {
				al_len = l + strlen(proto) + 2;
				tmp = malloc(al_len);
			} else {
				al_len = l + 1;
				tmp = malloc(al_len);
			}
			if (!tmp) {
				char *estr;
				estr = strerror_r(errno, buf, sizeof(buf));
				logerr(MODPREFIX "malloc: %s", estr);
				return 0;
			}
			ctxt->server = tmp;
			memset(ctxt->server, 0, al_len);
			if (*proto) {
				strcpy(ctxt->server, proto);
				memcpy(ctxt->server + strlen(proto), s, l);
				strcat(ctxt->server, "/");
			} else
				memcpy(ctxt->server, s, l);
			ptr = q + 1;
		} else {
			crit(logopt,
			     MODPREFIX "invalid LDAP map syntax %s", ptr);
			return 0;
/* TODO: why did I put this here, the parser shouldn't let this by
			l = strlen(ptr);
			tmp = malloc(l + 1);
			if (!tmp) {
				char *estr;
				estr = strerror_r(errno, buf, sizeof(buf));
				crit(logopt, MODPREFIX "malloc: %s", estr);
				return 0;
			}
			ctxt->server = tmp;
			memset(ctxt->server, 0, l + 1);
			memcpy(ctxt->server, s, l);
*/
		}
	} else if (strchr(ptr, ':') != NULL || *ptr == '[') {
		const char *q = NULL;

		/* Isolate the server. Include the port spec */
		if (*ptr != '[')
			q = strchr(ptr, ':');
		else {
			q = ++ptr;
			while (*q == ':' || isxdigit(*q))
				q++;
			if (*q != ']') {
				crit(logopt, MODPREFIX
				     "invalid LDAP map syntax %s", ptr);
				return 0;
			}
			q++;
			if (*q == ':')
				q++;
		}

		if (isdigit(*q))
			while (isdigit(*q))
				q++;

		if (*q != ':') {
			crit(logopt,
			     MODPREFIX "invalid LDAP map syntax %s", ptr);
			return 0;
		}

		l = q - ptr;
		if (*proto) {
			al_len = l + strlen(proto) + 2;
			tmp = malloc(al_len);
		} else {
			al_len = l + 1;
			tmp = malloc(al_len);
		}
		/* Isolate the server's name. */
		if (!tmp) {
			char *estr;
			estr = strerror_r(errno, buf, sizeof(buf));
			logerr(MODPREFIX "malloc: %s", estr);
			return 0;
		}
		ctxt->server = tmp;
		memset(ctxt->server, 0, al_len);
		if (*proto) {
			strcpy(ctxt->server, proto);
			memcpy(ctxt->server + strlen(proto), ptr, l);
			strcat(ctxt->server, "/");
		} else
			memcpy(ctxt->server, ptr, l);
		ptr += l + 1;
	}

	/* TODO: why did I do this - how can the map name "and" base dn be missing? */
	if (!ptr)
		goto done;

	/*
	 * For nss support we can have a map name with no
	 * type or dn info. If present a base dn must have
	 * at least an "=" and a "," to be at all functional.
	 * If a dn is given it must be fully specified or
	 * the later LDAP calls will fail.
	 */
	l = strlen(ptr);
	if ((name = strchr(ptr, '='))) {
		char *base;

		/*
		 * An '=' with no ',' means a mapname has been given so just
		 * grab it alone to keep it independent of schema otherwize
		 * we expect a full dn.
		 */
		if (!strchr(ptr, ',')) {
			char *map = strdup(name + 1);
			if (map)
				ctxt->mapname = map;
			else {
				char *estr;
				estr = strerror_r(errno, buf, sizeof(buf));
				logerr(MODPREFIX "strdup: %s", estr);
				if (ctxt->server)
					free(ctxt->server);
				return 0;
			}
			
		} else {
			base = malloc(l + 1);
			if (!base) {
				char *estr;
				estr = strerror_r(errno, buf, sizeof(buf));
				logerr(MODPREFIX "malloc: %s", estr);
				if (ctxt->server)
					free(ctxt->server);
				return 0;
			}
			ctxt->base = base;
			memset(ctxt->base, 0, l + 1);
			memcpy(ctxt->base, ptr, l);
		}
	} else {
		char *map = malloc(l + 1);
		if (!map) {
			char *estr;
			estr = strerror_r(errno, buf, sizeof(buf));
			logerr(MODPREFIX "malloc: %s", estr);
			if (ctxt->server)
				free(ctxt->server);
			return 0;
		}
		ctxt->mapname = map;
		memset(ctxt->mapname, 0, l + 1);
		memcpy(map, ptr, l);
	}

	if (!ctxt->server && *proto) {
		if (!strncmp(proto, "ldaps", 5)) {
			info(logopt, MODPREFIX
			     "server must be given to force ldaps, connection "
			     "will use LDAP client configured protocol");
		}
	}
done:
	if (ctxt->mapname)
		debug(logopt, MODPREFIX "mapname %s", ctxt->mapname);
	else
		debug(logopt, MODPREFIX "server \"%s\", base dn \"%s\"",
			ctxt->server ? ctxt->server : "(default)",
			ctxt->base);

	return 1;
}

static void free_context(struct lookup_context *ctxt)
{
	int ret;

	if (ctxt->schema) {
		free(ctxt->schema->map_class);
		free(ctxt->schema->map_attr);
		free(ctxt->schema->entry_class);
		free(ctxt->schema->entry_attr);
		free(ctxt->schema->value_attr);
		free(ctxt->schema);
	}
	if (ctxt->auth_conf)
		free(ctxt->auth_conf);
	if (ctxt->sasl_mech)
		free(ctxt->sasl_mech);
	if (ctxt->user)
		free(ctxt->user);
	if (ctxt->secret)
		free(ctxt->secret);
	if (ctxt->client_princ)
		free(ctxt->client_princ);
	if (ctxt->client_cc)
		free(ctxt->client_cc);
	if (ctxt->mapname)
		free(ctxt->mapname);
	if (ctxt->qdn)
		free(ctxt->qdn);
	if (ctxt->server)
		free(ctxt->server);
	if (ctxt->cur_host)
		free(ctxt->cur_host);
	if (ctxt->base)
		free(ctxt->base);
	if (ctxt->uris)
		defaults_free_uris(ctxt->uris);
	ret = pthread_mutex_destroy(&ctxt->uris_mutex);
	if (ret)
		fatal(ret);
	if (ctxt->sdns)
		defaults_free_searchdns(ctxt->sdns);
	free(ctxt);

	return;
}

static void validate_uris(struct list_head *list)
{
	struct list_head *next;

	next = list->next;
	while (next != list) {
		struct ldap_uri *this;

		this = list_entry(next, struct ldap_uri, list);
		next = next->next;

		/* At least we get some basic validation */
		if (!ldap_is_ldap_url(this->uri)) {
			list_del(&this->list);
			free(this->uri);
			free(this);
		}
	}

	return;			
}

/*
 * This initializes a context (persistent non-global data) for queries to
 * this module.  Return zero if we succeed.
 */
int lookup_init(const char *mapfmt, int argc, const char *const *argv, void **context)
{
	struct lookup_context *ctxt;
	char buf[MAX_ERR_BUF];
	LDAP *ldap = NULL;
	int ret;

	*context = NULL;

	/* If we can't build a context, bail. */
	ctxt = malloc(sizeof(struct lookup_context));
	if (!ctxt) {
		char *estr = strerror_r(errno, buf, sizeof(buf));
		logerr(MODPREFIX "malloc: %s", estr);
		return 1;
	}
	memset(ctxt, 0, sizeof(struct lookup_context));

	ret = pthread_mutex_init(&ctxt->uris_mutex, NULL);
	if (ret) {
		error(LOGOPT_ANY, MODPREFIX "failed to init uris mutex");
		free(ctxt);
		return 1;
	}

	/* If a map type isn't explicitly given, parse it like sun entries. */
	if (mapfmt == NULL)
		mapfmt = MAPFMT_DEFAULT;

	/*
	 * Parse out the server name and base dn, and fill them
	 * into the proper places in the lookup context structure.
	 */
	if (!parse_server_string(LOGOPT_NONE, argv[0], ctxt)) {
		error(LOGOPT_ANY, MODPREFIX "cannot parse server string");
		free_context(ctxt);
		return 1;
	}

	ctxt->timeout = defaults_get_ldap_timeout();
	ctxt->network_timeout = defaults_get_ldap_network_timeout();

	if (!ctxt->server) {
		struct list_head *uris = defaults_get_uris();
		if (uris) {
			validate_uris(uris);
			if (!list_empty(uris))
				ctxt->uris = uris;
			else {
				error(LOGOPT_ANY,
				      "no valid uris found in config list"
				      ", using default system config");
				free(uris);
			}
		}
	}

#ifdef WITH_SASL
	/*
	 *  First, check to see if a preferred authentication method was
	 *  specified by the user.  parse_ldap_config will return error
	 *  if the permissions on the file were incorrect, or if the
	 *  specified authentication type is not valid.
	 */
	ret = parse_ldap_config(LOGOPT_NONE, ctxt);
	if (ret) {
		free_context(ctxt);
		return 1;
	}

	/* Init the sasl callbacks */
	if (!autofs_sasl_client_init(LOGOPT_NONE)) {
		error(LOGOPT_ANY, "failed to init sasl client");
		free_context(ctxt);
		return 1;
	}
#endif

	if (ctxt->server || !ctxt->uris) {
		ldap = connect_to_server(LOGOPT_NONE, ctxt->server, ctxt);
		if (!ldap) {
			free_context(ctxt);
			return 1;
		}
	} else {
		ldap = find_server(LOGOPT_NONE, ctxt);
		if (!ldap) {
			free_context(ctxt);
			error(LOGOPT_ANY, MODPREFIX
			     "failed to find available server");
			return 1;
		}
	}
	unbind_ldap_connection(LOGOPT_ANY, ldap, ctxt);

	/* Open the parser, if we can. */
	ctxt->parse = open_parse(mapfmt, MODPREFIX, argc - 1, argv + 1);
	if (!ctxt->parse) {
		free_context(ctxt);
		logerr(MODPREFIX "failed to open parse context");
		return 1;
	}
	*context = ctxt;

	return 0;
}

int lookup_read_master(struct master *master, time_t age, void *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	unsigned int timeout = master->default_timeout;
	unsigned int logging = master->default_logging;
	unsigned int logopt = master->logopt;
	int rv, l, count;
	char buf[MAX_ERR_BUF];
	char parse_buf[PARSE_MAX_BUF];
	char *query;
	LDAPMessage *result, *e;
	char *class, *info, *entry;
	char **keyValue = NULL;
	char **values = NULL;
	char *attrs[3];
	int scope = LDAP_SCOPE_SUBTREE;
	LDAP *ldap;

	class = ctxt->schema->entry_class;
	entry = ctxt->schema->entry_attr;
	info = ctxt->schema->value_attr;

	attrs[0] = entry;
	attrs[1] = info;
	attrs[2] = NULL;

	l = strlen("(objectclass=)") + strlen(class) + 1;

	query = malloc(l);
	if (query == NULL) {
		char *estr = strerror_r(errno, buf, sizeof(buf));
		logerr(MODPREFIX "malloc: %s", estr);
		return NSS_STATUS_UNAVAIL;
	}

	if (sprintf(query, "(objectclass=%s)", class) >= l) {
		error(logopt, MODPREFIX "error forming query string");
		free(query);
		return NSS_STATUS_UNAVAIL;
	}

	/* Initialize the LDAP context. */
	ldap = do_reconnect(logopt, ctxt);
	if (!ldap) {
		free(query);
		return NSS_STATUS_UNAVAIL;
	}

	/* Look around. */
	debug(logopt,
	      MODPREFIX "searching for \"%s\" under \"%s\"", query, ctxt->qdn);

	rv = ldap_search_s(ldap, ctxt->qdn, scope, query, attrs, 0, &result);

	if ((rv != LDAP_SUCCESS) || !result) {
		error(logopt, MODPREFIX "query failed for %s: %s",
		      query, ldap_err2string(rv));
		unbind_ldap_connection(logging, ldap, ctxt);
		free(query);
		return NSS_STATUS_NOTFOUND;
	}

	e = ldap_first_entry(ldap, result);
	if (!e) {
		debug(logopt,
		      MODPREFIX "query succeeded, no matches for %s",
		      query);
		ldap_msgfree(result);
		unbind_ldap_connection(logging, ldap, ctxt);
		free(query);
		return NSS_STATUS_NOTFOUND;
	} else
		debug(logopt, MODPREFIX "examining entries");

	while (e) {
		keyValue = ldap_get_values(ldap, e, entry);

		if (!keyValue || !*keyValue) {
			e = ldap_next_entry(ldap, e);
			continue;
		}

		/*
		 * By definition keys must be unique within
		 * each map entry
		 */
		if (ldap_count_values(keyValue) > 1) {
			error(logopt,
			      MODPREFIX
			      "key %s has duplicate entries - ignoring",
			      *keyValue);
			goto next;
		}

		/*
		 * Ignore keys beginning with '+' as plus map
		 * inclusion is only valid in file maps.
		 */
		if (**keyValue == '+') {
			warn(logopt,
			     MODPREFIX
			     "ignoreing '+' map entry - not in file map");
			goto next;
		}

		values = ldap_get_values(ldap, e, info);
		if (!values || !*values) {
			debug(logopt,
			      MODPREFIX "no %s defined for %s", info, query);
			goto next;
		}

		/*
		 * We require that there be only one value per key.
		 */
		count = ldap_count_values(values);
		if (count > 1) {
			error(logopt,
			      MODPREFIX
			      "one value per key allowed in master map");
			ldap_value_free(values);
			goto next;
		}

		if (snprintf(parse_buf, sizeof(parse_buf), "%s %s",
			     *keyValue, *values) >= sizeof(parse_buf)) {
			error(logopt, MODPREFIX "map entry too long");
			ldap_value_free(values);
			goto next;
		}
		master_parse_entry(parse_buf, timeout, logging, age);
next:
		ldap_value_free(keyValue);
		e = ldap_next_entry(ldap, e);
	}

	/* Clean up. */
	ldap_msgfree(result);
	unbind_ldap_connection(logopt, ldap, ctxt);
	free(query);

	return NSS_STATUS_SUCCESS;
}

static int get_percent_decoded_len(const char *name)
{
	int escapes = 0;
	int escaped = 0;
	const char *tmp = name;
	int look_for_close = 0;

	while (*tmp) {
		if (*tmp == '%') {
			/* assume escapes aren't interpreted inside brackets */
			if (look_for_close) {
				tmp++;
				continue;
			}
			/* check for escaped % */
			if (escaped) {
				tmp++;
				escaped = 0;
				continue;
			}
			escapes++;
			tmp++;
			if (*tmp == '[') {
				escapes++;
				tmp++;
				look_for_close = 1;
			} else
				escaped = 1;
		} else if (*tmp == ']' && look_for_close) {
			escaped = 0;
			escapes++;
			tmp++;
			look_for_close = 0;
		} else {
			tmp++;
			escaped = 0;
		}
	}

	assert(strlen(name) > escapes);
	return strlen(name) - escapes;
}

/*
 * Try to catch heap corruption if our logic happens to be incorrect.
 */
static void validate_string_len(const char *orig, char *start,
				char *end, unsigned int len)
{
	debug(LOGOPT_NONE, MODPREFIX "string %s encoded as %s", orig, start);
	/* make sure we didn't overflow the allocated space */
	if (end - start > len + 1) {
		crit(LOGOPT_ANY, MODPREFIX "orig %s, len %d", orig, len);
		crit(LOGOPT_ANY, MODPREFIX "en/decoded %s, len %d", start,
		     end - start);
	}
	assert(end-start <= len + 1);
}

/*
 * Deal with encode and decode of % hack.
 * Return
 * 0 => % hack not present.
 * -1 => syntax error or alloc fail.
 * 1 transofrmed value returned.
 */
/*
 * Assumptions: %'s must be escaped by %'s.  %'s are not used to escape
 * anything else except capital letters (so you can't escape a closing
 * bracket, for example).
 */
static int decode_percent_hack(const char *name, char **key)
{
	const char *tmp;
	char *ptr, *new;
	unsigned int len;
	int escaped = 0, look_for_close = 0;

	if (!key)
		return -1;

	*key = NULL;

	len = get_percent_decoded_len(name);
	new = malloc(len + 1);
	if (!new)
		return -1;

	ptr = new;
	tmp = name;
	while (*tmp) {
		if (*tmp == '%') {
			if (escaped) {
				*ptr++ = *tmp++;
				if (!look_for_close)
					escaped = 0;
				continue;
			}
			tmp++;
			if (*tmp == '[') {
				tmp++;
				look_for_close = 1;
				escaped = 1;
			} else
				escaped = 1;
		} else if (*tmp == ']' && look_for_close) {
			tmp++;
			look_for_close = 0;
		} else {
			escaped = 0;
			*ptr++ = *tmp++;
		}
	}
	*ptr = '\0';
	*key = new;

	validate_string_len(name, new, ptr, len);
	return strlen(new);
}

/*
 * Calculate the length of a string replacing all capital letters with %letter.
 * For example:
 * Sale -> %Sale
 * SALE -> %S%A%L%E
 */
static int get_encoded_len_escaping_every_cap(const char *name)
{
	const char *tmp;
	unsigned int escapes = 0; /* number of % escape characters */

	tmp = name;
	while (*tmp) {
		/* We'll need to escape percents */
		if (*tmp == '%' || isupper(*tmp))
			escapes++;
		tmp++;
	}

	return strlen(name) + escapes;
}

/*
 * Calculate the length of a string replacing sequences (1 or more) of capital
 * letters with %[letters].  For example:
 * FOO ->  %[FOO]
 * Work -> %[W]ork
 * WorksForMe -> %[W]orks%[F]or%[M]e
 * aaBBaa -> aa%[BB]aa
 */
static int get_encoded_len_escaping_sequences(const char *name)
{
	const char *tmp;
	unsigned int escapes = 0;

	tmp = name;
	while (*tmp) {
		/* escape percents */
		if (*tmp == '%')
			escapes++;
		else if (isupper(*tmp)) {
			/* start an escape block %[...] */
			escapes += 3;  /* %[] */
			while (*tmp && isupper(*tmp))
				tmp++;
			continue;
		}
		tmp++;
	}

	return strlen(name) + escapes;
}

static void encode_individual(const char *name, char *new, unsigned int len)
{
	const char *tmp;
	char *ptr;

	ptr = new;
	tmp = name;
	while (*tmp) {
		if (*tmp == '%' || isupper(*tmp))
			*ptr++ = '%';
		*ptr++ = *tmp++;
	}
	*ptr = '\0';
	validate_string_len(name, new, ptr, len);
}

static void encode_sequence(const char *name, char *new, unsigned int len)
{
	const char *tmp;
	char *ptr;

	ptr = new;
	tmp = name;
	while (*tmp) {
		if (*tmp == '%') {
			*ptr++ = '%';
			*ptr++ = *tmp++;
		} else if (isupper(*tmp)) {
			*ptr++ = '%';
			*ptr++ = '[';
			*ptr++ = *tmp++;

			while (*tmp && isupper(*tmp)) {
				*ptr++ = *tmp;
				tmp++;
			}
			*ptr++ = ']';
		} else
			*ptr++ = *tmp++;
	}
	*ptr = '\0';
	validate_string_len(name, new, ptr, len);
}

/*
 * use_class:  1 means encode string as %[CAPITALS], 0 means encode as
 * %C%A%P%I%T%A%L%S
 */
static int encode_percent_hack(const char *name, char **key, unsigned int use_class)
{
	unsigned int len = 0;

	if (!key)
		return -1;

	if (use_class)
		len = get_encoded_len_escaping_sequences(name);
	else
		len = get_encoded_len_escaping_every_cap(name);

	/* If there is no escaping to be done, return 0 */
	if (len == strlen(name))
		return 0;

	*key = malloc(len + 1);
	if (!*key)
		return -1;

	if (use_class)
		encode_sequence(name, *key, len);
	else
		encode_individual(name, *key, len);

	if (strlen(*key) != len)
		crit(LOGOPT_ANY, MODPREFIX "encoded key length mismatch: key "
		     "%s len %d strlen %d", *key, len, strlen(*key));

	return strlen(*key);
}

static int do_paged_query(struct ldap_search_params *sp, struct lookup_context *ctxt)
{
	struct autofs_point *ap = sp->ap;
	LDAPControl *pageControl=NULL, *controls[2] = { NULL, NULL };
	LDAPControl **returnedControls = NULL;
	static ber_int_t pageSize = 1000;
	static char pagingCriticality = 'T';
	int rv, scope = LDAP_SCOPE_SUBTREE;

	if (sp->morePages == TRUE)
		goto do_paged;

	rv = ldap_search_s(sp->ldap, ctxt->qdn, scope, sp->query, sp->attrs, 0, &sp->result);
	if ((rv != LDAP_SUCCESS) || !sp->result) {
		/*
 		 * Check for Size Limit exceeded and force run through loop
		 * and requery using page control.
 		 */
		if (rv == LDAP_SIZELIMIT_EXCEEDED)
			sp->morePages = TRUE;
		else {
			debug(ap->logopt,
			      MODPREFIX "query failed for %s: %s",
			      sp->query, ldap_err2string(rv));
			return rv;
		}
	}
	return rv;

do_paged:
	/* we need to use page controls so requery LDAP */
	debug(ap->logopt, MODPREFIX "geting page of results");

	rv = ldap_create_page_control(sp->ldap, pageSize, sp->cookie,
				      pagingCriticality, &pageControl);
	if (rv != LDAP_SUCCESS) {
		warn(ap->logopt, MODPREFIX "failed to create page control");
		return rv;
	}

	/* Insert the control into a list to be passed to the search. */
	controls[0] = pageControl;

	/* Search for entries in the directory using the parmeters. */
	rv = ldap_search_ext_s(sp->ldap,
			       ctxt->qdn, scope, sp->query, sp->attrs,
			       0, controls, NULL, NULL, 0, &sp->result);
	if ((rv != LDAP_SUCCESS) && (rv != LDAP_PARTIAL_RESULTS)) {
		debug(ap->logopt,
		      MODPREFIX "query failed for %s: %s",
		      sp->query, ldap_err2string(rv));
		ldap_control_free(pageControl);
		return rv;
	}

	/* Parse the results to retrieve the contols being returned. */
	rv = ldap_parse_result(sp->ldap, sp->result,
			       NULL, NULL, NULL, NULL,
			       &returnedControls, FALSE);
	if (sp->cookie != NULL) {
		ber_bvfree(sp->cookie);
		sp->cookie = NULL;
	}

	/*
	 * Parse the page control returned to get the cookie and
	 * determine whether there are more pages.
	 */
	rv = ldap_parse_page_control(sp->ldap,
				     returnedControls, &sp->totalCount,
				     &sp->cookie);
	if (sp->cookie && sp->cookie->bv_val && strlen(sp->cookie->bv_val))
		sp->morePages = TRUE;
	else
		sp->morePages = FALSE;

	/* Cleanup the controls used. */
	if (returnedControls)
		ldap_controls_free(returnedControls);

	ldap_control_free(pageControl);

	return rv;
}

static int do_get_entries(struct ldap_search_params *sp, struct map_source *source, struct lookup_context *ctxt)
{
	struct autofs_point *ap = sp->ap;
	struct mapent_cache *mc = source->mc;
	char buf[MAX_ERR_BUF];
	struct berval **bvKey;
	struct berval **bvValues;
	LDAPMessage *e;
	char *class, *info, *entry;
	int rv, ret;
	int i, count;

	class = ctxt->schema->entry_class;
	entry = ctxt->schema->entry_attr;
	info = ctxt->schema->value_attr;

	e = ldap_first_entry(sp->ldap, sp->result);
	if (!e) {
		debug(ap->logopt,
		      MODPREFIX "query succeeded, no matches for %s",
		      sp->query);
		ret = ldap_parse_result(sp->ldap, sp->result,
					&rv, NULL, NULL, NULL, NULL, 0);
		if (ret == LDAP_SUCCESS)
			return rv;
		else
			return LDAP_OPERATIONS_ERROR;
	} else
		debug(ap->logopt, MODPREFIX "examining entries");

	while (e) {
		char *mapent = NULL;
		size_t mapent_len = 0;
		char *k_val;
		ber_len_t k_len;
		char *s_key;

		bvKey = ldap_get_values_len(sp->ldap, e, entry);
		if (!bvKey || !*bvKey) {
			e = ldap_next_entry(sp->ldap, e);
			if (!e) {
				debug(ap->logopt, MODPREFIX
				      "failed to get next entry for query %s",
				      sp->query);
				ret = ldap_parse_result(sp->ldap,
							sp->result, &rv,
							NULL, NULL, NULL, NULL, 0);
				if (ret == LDAP_SUCCESS)
					return rv;
				else
					return LDAP_OPERATIONS_ERROR;
			}
			continue;
		}

		/*
		 * By definition keys should be unique within each map entry,
		 * but as always there are exceptions.
		 */
		k_val = NULL;
		k_len = 0;

		/*
		 * Keys should be unique so, in general, there shouldn't be
		 * more than one attribute value. We make an exception for
		 * wildcard entries as people may have values for '*' or
		 * '/' for compaibility reasons. We use the '/' as the
		 * wildcard in LDAP but allow '*' as well to allow for
		 * people using older schemas that allow '*' as a key
		 * value. Another case where there can be multiple key
		 * values is when people have used the "%" hack to specify
		 * case matching ctriteria in a case insensitive attribute.
		 */
		count = ldap_count_values_len(bvKey);
		if (count > 1) {
			unsigned int i;

			/* Check for the "/" and "*" and use as "/" if found */
			for (i = 0; i < count; i++) {
				bvKey[i]->bv_val[bvKey[i]->bv_len] = '\0';

				/*
				 * If multiple entries are present they could
				 * be the result of people using the "%" hack so
				 * ignore them.
				 */
				if (strchr(bvKey[i]->bv_val, '%'))
					continue;

				/* check for wildcard */
				if (bvKey[i]->bv_len == 1 &&
				    (*bvKey[i]->bv_val == '/' ||
				     *bvKey[i]->bv_val == '*')) {
					/* always use '/' internally */
					*bvKey[i]->bv_val = '/';
					k_val = bvKey[i]->bv_val;
					k_len = 1;
					break;
				}

				/*
				 * We have a result from LDAP so this is a
				 * valid entry. Set the result to the LDAP
				 * key that isn't a wildcard and doesn't have
				 * any "%" hack values present. This should be
				 * the case insensitive match string for the
				 * nis schema, the default value.
				 */
				k_val = bvKey[i]->bv_val;
				k_len = bvKey[i]->bv_len;

				break;
			}

			if (!k_val) {
				error(ap->logopt,
				      MODPREFIX "invalid entry %.*s - ignoring",
				      bvKey[0]->bv_len, bvKey[0]->bv_val);
				goto next;
			}
		} else {
			/* Check for the "*" and use as "/" if found */
			if (bvKey[0]->bv_len == 1 && *bvKey[0]->bv_val == '*')
				*bvKey[0]->bv_val = '/';
			k_val = bvKey[0]->bv_val;
			k_len = bvKey[0]->bv_len;
		}

		/*
		 * Ignore keys beginning with '+' as plus map
		 * inclusion is only valid in file maps.
		 */
		if (*k_val == '+') {
			warn(ap->logopt,
			     MODPREFIX
			     "ignoreing '+' map entry - not in file map");
			goto next;
		}

		bvValues = ldap_get_values_len(sp->ldap, e, info);
		if (!bvValues || !*bvValues) {
			debug(ap->logopt,
			      MODPREFIX "no %s defined for %s", info, sp->query);
			goto next;
		}

		/*
		 * We expect that there will be only one value because
		 * questions of order of returned value entries but we
		 * accumulate values to support simple multi-mounts.
		 *
		 * If the ordering of a mount spec with another containing
		 * options or the actual order of entries causes problems
		 * it won't be supported. Perhaps someone can instruct us
		 * how to force an ordering.
		 */
		count = ldap_count_values_len(bvValues);
		for (i = 0; i < count; i++) {
			char *v_val = bvValues[i]->bv_val;
			ber_len_t v_len = bvValues[i]->bv_len;

			if (!mapent) {
				mapent = malloc(v_len + 1);
				if (!mapent) {
					char *estr;
					estr = strerror_r(errno, buf, sizeof(buf));
					logerr(MODPREFIX "malloc: %s", estr);
					ldap_value_free_len(bvValues);
					goto next;
				}
				strncpy(mapent, v_val, v_len);
				mapent[v_len] = '\0';
				mapent_len = v_len;
			} else {
				int new_size = mapent_len + v_len + 2;
				char *new_me;
				new_me = realloc(mapent, new_size);
				if (new_me) {
					mapent = new_me;
					strcat(mapent, " ");
					strncat(mapent, v_val, v_len);
					mapent[new_size] = '\0';
					mapent_len = new_size;
				} else {
					char *estr;
					estr = strerror_r(errno, buf, sizeof(buf));
					logerr(MODPREFIX "realloc: %s", estr);
				}
			}
		}
		ldap_value_free_len(bvValues);

		if (*k_val == '/' && k_len == 1) {
			if (ap->type == LKP_DIRECT)
				goto next;
			*k_val = '*';
		}

		if (strcasecmp(class, "nisObject")) {
			s_key = sanitize_path(k_val, k_len, ap->type, ap->logopt);
			if (!s_key)
				goto next;
		} else {
			char *dec_key;
			int dec_len = decode_percent_hack(k_val, &dec_key);

			if (dec_len < 0) {
				crit(ap->logopt,
				     "could not use percent hack to decode key %s",
				     k_val);
				goto next;
			}

			if (dec_len == 0)
				s_key = sanitize_path(k_val, k_len, ap->type, ap->logopt);
			else {
				s_key = sanitize_path(dec_key, dec_len, ap->type, ap->logopt);
				free(dec_key);
			}
			if (!s_key)
				goto next;
		}

		cache_writelock(mc);
		cache_update(mc, source, s_key, mapent, sp->age);
		cache_unlock(mc);

		free(s_key);
next:
		if (mapent) {
			free(mapent);
			mapent = NULL;
		}

		ldap_value_free_len(bvKey);
		e = ldap_next_entry(sp->ldap, e);
		if (!e) {
			debug(ap->logopt, MODPREFIX
			      "failed to get next entry for query %s",
			      sp->query);
			ret = ldap_parse_result(sp->ldap,
						sp->result, &rv,
						NULL, NULL, NULL, NULL, 0);
			if (ret == LDAP_SUCCESS)
				return rv;
			else
				return LDAP_OPERATIONS_ERROR;
		}
	}

	return LDAP_SUCCESS;
}


static int read_one_map(struct autofs_point *ap,
			struct lookup_context *ctxt,
			time_t age, int *result_ldap)
{
	struct map_source *source;
	struct ldap_search_params sp;
	char buf[MAX_ERR_BUF];
	char *class, *info, *entry;
	char *attrs[3];
	int rv, l;

	source = ap->entry->current;
	ap->entry->current = NULL;
	master_source_current_signal(ap->entry);

	sp.ap = ap;
	sp.age = age;

	class = ctxt->schema->entry_class;
	entry = ctxt->schema->entry_attr;
	info = ctxt->schema->value_attr;

	attrs[0] = entry;
	attrs[1] = info;
	attrs[2] = NULL;
	sp.attrs = attrs;

	/* Build a query string. */
	l = strlen("(objectclass=)") + strlen(class) + 1;

	sp.query = malloc(l);
	if (sp.query == NULL) {
		char *estr = strerror_r(errno, buf, sizeof(buf));
		logerr(MODPREFIX "malloc: %s", estr);
		return NSS_STATUS_UNAVAIL;
	}

	if (sprintf(sp.query, "(objectclass=%s)", class) >= l) {
		error(ap->logopt, MODPREFIX "error forming query string");
		free(sp.query);
		return NSS_STATUS_UNAVAIL;
	}

	/* Initialize the LDAP context. */
	sp.ldap = do_reconnect(ap->logopt, ctxt);
	if (!sp.ldap) {
		free(sp.query);
		return NSS_STATUS_UNAVAIL;
	}

	/* Look around. */
	debug(ap->logopt,
	      MODPREFIX "searching for \"%s\" under \"%s\"", sp.query, ctxt->qdn);

	sp.cookie = NULL;
	sp.morePages = FALSE;
	sp.totalCount = 0;
	sp.result = NULL;

	do {
		rv = do_paged_query(&sp, ctxt);
		if (rv == LDAP_SIZELIMIT_EXCEEDED)
		{
			debug(ap->logopt, MODPREFIX "result size exceed");
			if (sp.result)
				ldap_msgfree(sp.result);

			continue;
		}

		if (rv != LDAP_SUCCESS || !sp.result) {
			unbind_ldap_connection(ap->logopt, sp.ldap, ctxt);
			*result_ldap = rv;
			free(sp.query);
			return NSS_STATUS_UNAVAIL;
		}

		rv = do_get_entries(&sp, source, ctxt);
		if (rv != LDAP_SUCCESS) {
			ldap_msgfree(sp.result);
			unbind_ldap_connection(ap->logopt, sp.ldap, ctxt);
			*result_ldap = rv;
			free(sp.query);
			return NSS_STATUS_NOTFOUND;
		}
		ldap_msgfree(sp.result);
	} while (sp.morePages == TRUE);

	debug(ap->logopt, MODPREFIX "done updating map");

	unbind_ldap_connection(ap->logopt, sp.ldap, ctxt);

	source->age = age;
	free(sp.query);

	return NSS_STATUS_SUCCESS;
}

int lookup_read_map(struct autofs_point *ap, time_t age, void *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	int rv = LDAP_SUCCESS;
	int ret, cur_state;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cur_state);
	ret = read_one_map(ap, ctxt, age, &rv);
	if (ret != NSS_STATUS_SUCCESS) {
		switch (rv) {
		case LDAP_SIZELIMIT_EXCEEDED:
			crit(ap->logopt, MODPREFIX
			     "Unable to download entire LDAP map for: %s",
			     ap->path);
		case LDAP_UNWILLING_TO_PERFORM:
			pthread_setcancelstate(cur_state, NULL);
			return NSS_STATUS_UNAVAIL;
		}
	}
	pthread_setcancelstate(cur_state, NULL);

	return ret;
}

static int lookup_one(struct autofs_point *ap,
		char *qKey, int qKey_len, struct lookup_context *ctxt)
{
	struct map_source *source;
	struct mapent_cache *mc;
	int rv, i, l, ql, count;
	char buf[MAX_ERR_BUF];
	time_t age = time(NULL);
	char *query;
	LDAPMessage *result, *e;
	char *class, *info, *entry;
	char *enc_key1, *enc_key2;
	int enc_len1 = 0, enc_len2 = 0;
	struct berval **bvKey;
	struct berval **bvValues;
	char *attrs[3];
	int scope = LDAP_SCOPE_SUBTREE;
	LDAP *ldap;
	struct mapent *we;
	unsigned int wild = 0;
	int ret = CHE_MISSING;

	source = ap->entry->current;
	ap->entry->current = NULL;
	master_source_current_signal(ap->entry);

	mc = source->mc;

	if (ctxt == NULL) {
		crit(ap->logopt, MODPREFIX "context was NULL");
		return CHE_FAIL;
	}

	class = ctxt->schema->entry_class;
	entry = ctxt->schema->entry_attr;
	info = ctxt->schema->value_attr;

	attrs[0] = entry;
	attrs[1] = info;
	attrs[2] = NULL;

	if (*qKey == '*' && qKey_len == 1)
		*qKey = '/';
	else if (!strcasecmp(class, "nisObject")) {
		enc_len1 = encode_percent_hack(qKey, &enc_key1, 0);
		if (enc_len1 < 0) {
			crit(ap->logopt,
			     "could not use percent hack encode key %s",
			     qKey);
			return CHE_FAIL;
		}
		if (enc_len1 != 0) {
			enc_len2 = encode_percent_hack(qKey, &enc_key2, 1);
			if (enc_len2 < 0) {
				crit(ap->logopt,
				     "could not use percent hack encode key %s",
				     qKey);
				return CHE_FAIL;
			}
		}
	}

	/* Build a query string. */
	l = strlen(class) + 3*strlen(entry) + strlen(qKey) + 35;
	if (enc_len1)
		l += 2*strlen(entry) + enc_len1 + enc_len2 + 6;

	query = malloc(l);
	if (query == NULL) {
		char *estr = strerror_r(errno, buf, sizeof(buf));
		crit(ap->logopt, MODPREFIX "malloc: %s", estr);
		if (enc_len1) {
			free(enc_key1);
			free(enc_key2);
		}
		free(query);
		return CHE_FAIL;
	}

	/*
	 * Look for an entry in class under ctxt-base
	 * whose entry is equal to qKey.
	 */
	if (!enc_len1) {
		ql = sprintf(query,
			"(&(objectclass=%s)(|(%s=%s)(%s=/)(%s=\\2A)))",
			class, entry, qKey, entry, entry);
	} else {
		if (enc_len2) {
			ql = sprintf(query,
				"(&(objectclass=%s)"
				"(|(%s=%s)(%s=%s)(%s=%s)(%s=/)(%s=\\2A)))",
				class, entry, qKey,
				entry, enc_key1, entry, enc_key2, entry, entry);
			free(enc_key1);
			free(enc_key2);
		} else {
			ql = sprintf(query,
				"(&(objectclass=%s)"
				"(|(%s=%s)(%s=%s)(%s=/)(%s=\\2A)))",
				class, entry, qKey, entry, enc_key1, entry, entry);
			free(enc_key1);
		}
	}
	if (ql >= l) {
		error(ap->logopt,
		      MODPREFIX "error forming query string");
		free(query);
		return CHE_FAIL;
	}

	/* Initialize the LDAP context. */
	ldap = do_reconnect(ap->logopt, ctxt);
	if (!ldap) {
		free(query);
		return CHE_UNAVAIL;
	}

	debug(ap->logopt,
	      MODPREFIX "searching for \"%s\" under \"%s\"", query, ctxt->qdn);

	rv = ldap_search_s(ldap, ctxt->qdn, scope, query, attrs, 0, &result);

	if ((rv != LDAP_SUCCESS) || !result) {
		crit(ap->logopt, MODPREFIX "query failed for %s", query);
		unbind_ldap_connection(ap->logopt, ldap, ctxt);
		free(query);
		return CHE_FAIL;
	}

	debug(ap->logopt,
	      MODPREFIX "getting first entry for %s=\"%s\"", entry, qKey);

	e = ldap_first_entry(ldap, result);
	if (!e) {
		debug(ap->logopt,
		     MODPREFIX "got answer, but no entry for %s", query);
		ldap_msgfree(result);
		unbind_ldap_connection(ap->logopt, ldap, ctxt);
		free(query);
		return CHE_MISSING;
	}

	while (e) {
		char *mapent = NULL;
		size_t mapent_len = 0;
		char *k_val;
		ber_len_t k_len;
		char *s_key;

		bvKey = ldap_get_values_len(ldap, e, entry);
		if (!bvKey || !*bvKey) {
			e = ldap_next_entry(ldap, e);
			continue;
		}

		/*
		 * By definition keys should be unique within each map entry,
		 * but as always there are exceptions.
		 */
		k_val = NULL;
		k_len = 0;

		/*
		 * Keys must be unique so, in general, there shouldn't be
		 * more than one attribute value. We make an exception for
		 * wildcard entries as people may have values for '*' or
		 * '/' for compaibility reasons. We use the '/' as the
		 * wildcard in LDAP but allow '*' as well to allow for
		 * people using older schemas that allow '*' as a key
		 * value. Another case where there can be multiple key
		 * values is when people have used the "%" hack to specify
		 * case matching ctriteria in a caase insensitive attribute.
		 */
		count = ldap_count_values_len(bvKey);
		if (count > 1) {
			unsigned int i;

			/* Check for the "/" and "*" and use as "/" if found */
			for (i = 0; i < count; i++) {
				bvKey[i]->bv_val[bvKey[i]->bv_len] = '\0';

				/*
				 * If multiple entries are present they could
				 * be the result of people using the "%" hack so
				 * ignore them.
				 */
				if (strchr(bvKey[i]->bv_val, '%'))
					continue;

				/* check for wildcard */
				if (bvKey[i]->bv_len == 1 &&
				    (*bvKey[i]->bv_val == '/' ||
				     *bvKey[i]->bv_val == '*')) {
					/* always use '/' internally */
					*bvKey[i]->bv_val = '/';
					k_val = bvKey[i]->bv_val;
					k_len = 1;
					break;
				}

				/*
				 * The key was matched by LDAP so this is a
				 * valid entry. Set the result key to the
				 * lookup key to provide the mixed case
				 * matching provided by the "%" hack.
				 */
				k_val = qKey;
				k_len = strlen(qKey);

				break;
			}

			if (!k_val) {
				error(ap->logopt,
					MODPREFIX "no valid key found for %.*s",
					qKey_len, qKey);
				ret = CHE_FAIL;
				goto next;
			}
		} else {
			/* Check for the "*" and use as "/" if found */
			if (bvKey[0]->bv_len == 1 && *bvKey[0]->bv_val == '*')
				*bvKey[0]->bv_val = '/';
			k_val = bvKey[0]->bv_val;
			k_len = bvKey[0]->bv_len;
		}

		debug(ap->logopt, MODPREFIX "examining first entry");

		bvValues = ldap_get_values_len(ldap, e, info);
		if (!bvValues || !*bvValues) {
			debug(ap->logopt,
			      MODPREFIX "no %s defined for %s", info, query);
			goto next;
		}

		count = ldap_count_values_len(bvValues);
		for (i = 0; i < count; i++) {
			char *v_val = bvValues[i]->bv_val;
			ber_len_t v_len = bvValues[i]->bv_len;

			if (!mapent) {
				mapent = malloc(v_len + 1);
				if (!mapent) {
					char *estr;
					estr = strerror_r(errno, buf, sizeof(buf));
					logerr(MODPREFIX "malloc: %s", estr);
					ldap_value_free_len(bvValues);
					goto next;
				}
				strncpy(mapent, v_val, v_len);
				mapent[v_len] = '\0';
				mapent_len = v_len;
			} else {
				int new_size = mapent_len + v_len + 2;
				char *new_me;
				new_me = realloc(mapent, new_size);
				if (new_me) {
					mapent = new_me;
					strcat(mapent, " ");
					strncat(mapent, v_val, v_len);
					mapent[new_size] = '\0';
					mapent_len = new_size;
				} else {
					char *estr;
					estr = strerror_r(errno, buf, sizeof(buf));
					logerr(MODPREFIX "realloc: %s", estr);
				}
			}
		}
		ldap_value_free_len(bvValues);

		if (*k_val == '/' && k_len == 1) {
			if (ap->type == LKP_DIRECT)
				goto next;
			wild = 1;
			cache_writelock(mc);
			cache_update(mc, source, "*", mapent, age);
			cache_unlock(mc);
			goto next;
		}

		if (strcasecmp(class, "nisObject")) {
			s_key = sanitize_path(k_val, k_len, ap->type, ap->logopt);
			if (!s_key)
				goto next;
		} else {
			char *dec_key;
			int dec_len = decode_percent_hack(k_val, &dec_key);

			if (dec_len < 0) {
				crit(ap->logopt,
				     "could not use percent hack to decode key %s",
				     k_val);
				goto next;
			}

			if (dec_len == 0)
				s_key = sanitize_path(k_val, k_len, ap->type, ap->logopt);
			else {
				s_key = sanitize_path(dec_key, dec_len, ap->type, ap->logopt);
				free(dec_key);
			}
			if (!s_key)
				goto next;
		}

		cache_writelock(mc);
		ret = cache_update(mc, source, s_key, mapent, age);
		cache_unlock(mc);

		free(s_key);
next:
		if (mapent) {
			free(mapent);
			mapent = NULL;
		}

		ldap_value_free_len(bvKey);
		e = ldap_next_entry(ldap, e);
	}

	ldap_msgfree(result);
	unbind_ldap_connection(ap->logopt, ldap, ctxt);

	/* Failed to find wild entry, update cache if needed */
	pthread_cleanup_push(cache_lock_cleanup, mc);
	cache_writelock(mc);
	we = cache_lookup_distinct(mc, "*");
	if (we) {
		/* Wildcard entry existed and is now gone */
		if (we->source == source && !wild) {
			cache_delete(mc, "*");
			source->stale = 1;
		}
	} else {
		/* Wildcard not in map but now is */
		if (wild)
			source->stale = 1;
	}
	/* Not found in the map but found in the cache */
	if (ret == CHE_MISSING) {
		struct mapent *exists = cache_lookup_distinct(mc, qKey);
		if (exists && exists->source == source) {
			if (exists->mapent) {
				free(exists->mapent);
				exists->mapent = NULL;
				source->stale = 1;
				exists->status = 0;
			}
		}
	}
	pthread_cleanup_pop(1);
	free(query);

	return ret;
}

static int check_map_indirect(struct autofs_point *ap,
			      char *key, int key_len,
			      struct lookup_context *ctxt)
{
	struct map_source *source;
	struct mapent_cache *mc;
	struct mapent *me;
	time_t now = time(NULL);
	time_t t_last_read;
	int ret, cur_state;

	source = ap->entry->current;
	ap->entry->current = NULL;
	master_source_current_signal(ap->entry);

	mc = source->mc;

	master_source_current_wait(ap->entry);
	ap->entry->current = source;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cur_state);
	ret = lookup_one(ap, key, key_len, ctxt);
	if (ret == CHE_FAIL) {
		pthread_setcancelstate(cur_state, NULL);
		return NSS_STATUS_NOTFOUND;
	} else if (ret == CHE_UNAVAIL) {
		/*
		 * If the server is down and the entry exists in the cache
		 * and belongs to this map return success and use the entry.
		 */
		struct mapent *exists = cache_lookup(mc, key);
		if (exists && exists->source == source) {
			pthread_setcancelstate(cur_state, NULL);
			return NSS_STATUS_SUCCESS;
		}

		warn(ap->logopt,
		     MODPREFIX "lookup for %s failed: connection failed", key);

		return NSS_STATUS_UNAVAIL;
	}
	pthread_setcancelstate(cur_state, NULL);

	/*
	 * Check for map change and update as needed for
	 * following cache lookup.
	 */
	cache_readlock(mc);
	t_last_read = ap->exp_runfreq + 1;
	me = cache_lookup_first(mc);
	while (me) {
		if (me->source == source) {
			t_last_read = now - me->age;
			break;
		}
		me = cache_lookup_next(mc, me);
	}
	cache_unlock(mc);

	if (t_last_read > ap->exp_runfreq && ret & CHE_UPDATED)
		source->stale = 1;

	cache_readlock(mc);
	me = cache_lookup_distinct(mc, "*");
	if (ret == CHE_MISSING && (!me || me->source != source)) {
		cache_unlock(mc);
		return NSS_STATUS_NOTFOUND;
	}
	cache_unlock(mc);

	return NSS_STATUS_SUCCESS;
}

int lookup_mount(struct autofs_point *ap, const char *name, int name_len, void *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	struct map_source *source;
	struct mapent_cache *mc;
	struct mapent *me;
	char key[KEY_MAX_LEN + 1];
	int key_len;
	char *mapent = NULL;
	char mapent_buf[MAPENT_MAX_LEN + 1];
	int status = 0;
	int ret = 1;

	source = ap->entry->current;
	ap->entry->current = NULL;
	master_source_current_signal(ap->entry);

	mc = source->mc;

	debug(ap->logopt, MODPREFIX "looking up %s", name);

	key_len = snprintf(key, KEY_MAX_LEN + 1, "%s", name);
	if (key_len > KEY_MAX_LEN)
		return NSS_STATUS_NOTFOUND;

	/* Check if we recorded a mount fail for this key anywhere */
	me = lookup_source_mapent(ap, key, LKP_DISTINCT);
	if (me) {
		if (me->status >= time(NULL)) {
			cache_unlock(me->mc);
			return NSS_STATUS_NOTFOUND;
		}

		/* Negative timeout expired for non-existent entry. */
		if (!me->mapent)
			cache_delete(me->mc, key);

		cache_unlock(me->mc);
	}

        /*
	 * We can't check the direct mount map as if it's not in
	 * the map cache already we never get a mount lookup, so
	 * we never know about it.
	 */
	if (ap->type == LKP_INDIRECT && *key != '/') {
		char *lkp_key;

		cache_readlock(mc);
		me = cache_lookup_distinct(mc, key);
		if (me && me->multi)
			lkp_key = strdup(me->multi->key);
		else
			lkp_key = strdup(key);
		cache_unlock(mc);

		if (!lkp_key)
			return NSS_STATUS_UNKNOWN;

		master_source_current_wait(ap->entry);
		ap->entry->current = source;

		status = check_map_indirect(ap, lkp_key, strlen(lkp_key), ctxt);
		free(lkp_key);
		if (status)
			return status;
	}

	cache_readlock(mc);
	me = cache_lookup(mc, key);
	/* Stale mapent => check for entry in alternate source or wildcard */
	if (me && !me->mapent) {
		while ((me = cache_lookup_key_next(me)))
			if (me->source == source)
				break;
		if (!me)
			me = cache_lookup_distinct(mc, "*");
	}
	if (me && (me->source == source || *me->key == '/')) {
		strcpy(mapent_buf, me->mapent);
		mapent = mapent_buf;
	}
	cache_unlock(mc);

	if (!mapent)
		return NSS_STATUS_TRYAGAIN;

	master_source_current_wait(ap->entry);
	ap->entry->current = source;

	debug(ap->logopt, MODPREFIX "%s -> %s", key, mapent);
	ret = ctxt->parse->parse_mount(ap, key, key_len,
				       mapent, ctxt->parse->context);
	if (ret) {
		time_t now = time(NULL);
		int rv = CHE_OK;

		/* Record the the mount fail in the cache */
		cache_writelock(mc);
		me = cache_lookup_distinct(mc, key);
		if (!me)
			rv = cache_update(mc, source, key, NULL, now);
		if (rv != CHE_FAIL) {
			me = cache_lookup_distinct(mc, key);
			me->status = now + ap->negative_timeout;
		}
		cache_unlock(mc);
		return NSS_STATUS_TRYAGAIN;
	}

	return NSS_STATUS_SUCCESS;
}

/*
 * This destroys a context for queries to this module.  It releases the parser
 * structure (unloading the module) and frees the memory used by the context.
 */
int lookup_done(void *context)
{
	struct lookup_context *ctxt = (struct lookup_context *) context;
	int rv = close_parse(ctxt->parse);
#ifdef WITH_SASL
	autofs_sasl_dispose(ctxt);
	autofs_sasl_done();
#endif
	free_context(ctxt);
	return rv;
}
