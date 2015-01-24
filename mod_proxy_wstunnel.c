/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mod_proxy.h"

/* Clear all connection-based headers from the incoming headers table */
typedef struct header_dptr {
    apr_pool_t *pool;
    apr_table_t *table;
    apr_time_t time;
} header_dptr;

static int clear_conn_headers(void *data, const char *key, const char *val)
{
    apr_table_t *headers = ((header_dptr*)data)->table;
    apr_pool_t *pool = ((header_dptr*)data)->pool;
    const char *name;
    char *next = apr_pstrdup(pool, val);
    while (*next) {
        name = next;
        while (*next && !apr_isspace(*next) && (*next != ',')) {
            ++next;
        }
        while (*next && (apr_isspace(*next) || (*next == ','))) {
            *next++ = '\0';
        }
        apr_table_unset(headers, name);
    }
    return 1;
}

static void proxy_clear_connection(apr_pool_t *p, apr_table_t *headers)
{
    header_dptr x;
    x.pool = p;
    x.table = headers;
    apr_table_unset(headers, "Proxy-Connection");
    apr_table_do(clear_conn_headers, &x, headers, "Connection", NULL);
    apr_table_unset(headers, "Connection");
}

/* NOTE: From include/http_log.h */
/**
 * APLOGNO() should be used at the start of the format string passed
 * to ap_log_error() and friends. The argument must be a 5 digit decimal
 * number. It creates a tag of the form "AH02182: "
 * See docs/log-message-tags/README for details.
 */
#ifndef APLOGNO
#define APLOGNO(n)              "AH" #n ": "
#endif
/* NOTE: End copy from include/http_log.h */

/* NOTE: From server/util.c */
/**
 * Determine if a request has a request body or not.
 *
 * @param r the request_rec of the request
 * @return truth value
 */
AP_DECLARE(int) ap_request_has_body(request_rec *r)
{
    apr_off_t cl;
    char *estr;
    const char *cls;
    int has_body;

    has_body = (!r->header_only
                && (/* r->kept_body
                    || */ apr_table_get(r->headers_in, "Transfer-Encoding")
                    || ( (cls = apr_table_get(r->headers_in, "Content-Length"))
                        && (apr_strtoff(&cl, cls, &estr, 10) == APR_SUCCESS)
                        && (!*estr)
                        && (cl > 0) )
                    )
                );
    return has_body;
}
/* NOTE: End copy from server/util.c */

/* NOTE: From proxy_util.c */
PROXY_DECLARE(int) ap_proxy_pass_brigade(apr_bucket_alloc_t *bucket_alloc,
                                         request_rec *r, proxy_conn_rec *p_conn,
                                         conn_rec *origin, apr_bucket_brigade *bb,
                                         int flush)
{
    apr_status_t status;
    apr_off_t transferred;

    if (flush) {
        apr_bucket *e = apr_bucket_flush_create(bucket_alloc);
        APR_BRIGADE_INSERT_TAIL(bb, e);
    }
    apr_brigade_length(bb, 0, &transferred);
    if (transferred != -1)
        p_conn->worker->s->transferred += transferred;
    status = ap_pass_brigade(origin->output_filters, bb);
    /* Cleanup the brigade now to avoid buckets lifetime
     * issues in case of error returned below. */
    apr_brigade_cleanup(bb);
    if (status != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, status, r, APLOGNO(01084)
                      "pass request body failed to %pI (%s)",
                      p_conn->addr, p_conn->hostname);
        if (origin->aborted) {
            const char *ssl_note;

            if (((ssl_note = apr_table_get(origin->notes, "SSL_connect_rv"))
                 != NULL) && (strcmp(ssl_note, "err") == 0)) {
                return ap_proxyerror(r, HTTP_INTERNAL_SERVER_ERROR,
                                     "Error during SSL Handshake with"
                                     " remote server");
            }
            return APR_STATUS_IS_TIMEUP(status) ? HTTP_GATEWAY_TIME_OUT : HTTP_BAD_GATEWAY;
        }
        else {
            return HTTP_BAD_REQUEST;
        }
    }
    return OK;
}

PROXY_DECLARE(int) ap_proxy_create_hdrbrgd(apr_pool_t *p,
                                            apr_bucket_brigade *header_brigade,
                                            request_rec *r,
                                            proxy_conn_rec *p_conn,
                                            proxy_worker *worker,
                                            proxy_server_conf *conf,
                                            apr_uri_t *uri,
                                            char *url, char *server_portstr,
                                            char **old_cl_val,
                                            char **old_te_val)
{
    conn_rec *c = r->connection;
    int counter;
    char *buf;
    const apr_array_header_t *headers_in_array;
    const apr_table_entry_t *headers_in;
    apr_table_t *headers_in_copy;
    apr_bucket *e;
    int do_100_continue;
    conn_rec *origin = p_conn->connection;

    /*
     * To be compliant, we only use 100-Continue for requests with bodies.
     * We also make sure we won't be talking HTTP/1.0 as well.
     */
    do_100_continue = (worker->ping_timeout_set
                       && ap_request_has_body(r)
                       && (PROXYREQ_REVERSE == r->proxyreq)
                       && !(apr_table_get(r->subprocess_env, "force-proxy-request-1.0")));

    if (apr_table_get(r->subprocess_env, "force-proxy-request-1.0")) {
        /*
         * According to RFC 2616 8.2.3 we are not allowed to forward an
         * Expect: 100-continue to an HTTP/1.0 server. Instead we MUST return
         * a HTTP_EXPECTATION_FAILED
         */
        if (r->expecting_100) {
            return HTTP_EXPECTATION_FAILED;
        }
        buf = apr_pstrcat(p, r->method, " ", url, " HTTP/1.0" CRLF, NULL);
        p_conn->close = 1;
    } else {
        buf = apr_pstrcat(p, r->method, " ", url, " HTTP/1.1" CRLF, NULL);
    }
    if (apr_table_get(r->subprocess_env, "proxy-nokeepalive")) {
        origin->keepalive = AP_CONN_CLOSE;
        p_conn->close = 1;
    }
    ap_xlate_proto_to_ascii(buf, strlen(buf));
    e = apr_bucket_pool_create(buf, strlen(buf), p, c->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(header_brigade, e);
    if (conf->preserve_host == 0) {
        if (ap_strchr_c(uri->hostname, ':')) { /* if literal IPv6 address */
            if (uri->port_str && uri->port != DEFAULT_HTTP_PORT) {
                buf = apr_pstrcat(p, "Host: [", uri->hostname, "]:",
                                  uri->port_str, CRLF, NULL);
            } else {
                buf = apr_pstrcat(p, "Host: [", uri->hostname, "]", CRLF, NULL);
            }
        } else {
            if (uri->port_str && uri->port != DEFAULT_HTTP_PORT) {
                buf = apr_pstrcat(p, "Host: ", uri->hostname, ":",
                                  uri->port_str, CRLF, NULL);
            } else {
                buf = apr_pstrcat(p, "Host: ", uri->hostname, CRLF, NULL);
            }
        }
    }
    else {
        /* don't want to use r->hostname, as the incoming header might have a
         * port attached
         */
        const char* hostname = apr_table_get(r->headers_in,"Host");
        if (!hostname) {
            hostname =  r->server->server_hostname;
            ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r, APLOGNO(01092)
                          "no HTTP 0.9 request (with no host line) "
                          "on incoming request and preserve host set "
                          "forcing hostname to be %s for uri %s",
                          hostname, r->uri);
        }
        buf = apr_pstrcat(p, "Host: ", hostname, CRLF, NULL);
    }
    ap_xlate_proto_to_ascii(buf, strlen(buf));
    e = apr_bucket_pool_create(buf, strlen(buf), p, c->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(header_brigade, e);

    /* handle Via */
    if (conf->viaopt == via_block) {
        /* Block all outgoing Via: headers */
        apr_table_unset(r->headers_in, "Via");
    } else if (conf->viaopt != via_off) {
        const char *server_name = ap_get_server_name(r);
        /* If USE_CANONICAL_NAME_OFF was configured for the proxy virtual host,
         * then the server name returned by ap_get_server_name() is the
         * origin server name (which does make too much sense with Via: headers)
         * so we use the proxy vhost's name instead.
         */
        if (server_name == r->hostname)
            server_name = r->server->server_hostname;
        /* Create a "Via:" request header entry and merge it */
        /* Generate outgoing Via: header with/without server comment: */
        apr_table_mergen(r->headers_in, "Via",
                         (conf->viaopt == via_full)
                         ? apr_psprintf(p, "%d.%d %s%s (%s)",
                                        HTTP_VERSION_MAJOR(r->proto_num),
                                        HTTP_VERSION_MINOR(r->proto_num),
                                        server_name, server_portstr,
                                        AP_SERVER_BASEVERSION)
                         : apr_psprintf(p, "%d.%d %s%s",
                                        HTTP_VERSION_MAJOR(r->proto_num),
                                        HTTP_VERSION_MINOR(r->proto_num),
                                        server_name, server_portstr)
                         );
    }

    /* Use HTTP/1.1 100-Continue as quick "HTTP ping" test
     * to backend
     */
    if (do_100_continue) {
        const char *val;

        if (!r->expecting_100) {
            /* Don't forward any "100 Continue" response if the client is
             * not expecting it.
             */
            apr_table_setn(r->subprocess_env, "proxy-interim-response",
                                              "Suppress");
        }

        /* Add the Expect header if not already there. */
        if (((val = apr_table_get(r->headers_in, "Expect")) == NULL)
                || (strcasecmp(val, "100-Continue") != 0 /* fast path */
                    && !ap_find_token(r->pool, val, "100-Continue"))) {
            apr_table_mergen(r->headers_in, "Expect", "100-Continue");
        }
    }

    /* X-Forwarded-*: handling
     *
     * XXX Privacy Note:
     * -----------------
     *
     * These request headers are only really useful when the mod_proxy
     * is used in a reverse proxy configuration, so that useful info
     * about the client can be passed through the reverse proxy and on
     * to the backend server, which may require the information to
     * function properly.
     *
     * In a forward proxy situation, these options are a potential
     * privacy violation, as information about clients behind the proxy
     * are revealed to arbitrary servers out there on the internet.
     *
     * The HTTP/1.1 Via: header is designed for passing client
     * information through proxies to a server, and should be used in
     * a forward proxy configuation instead of X-Forwarded-*. See the
     * ProxyVia option for details.
     */
    if (/* dconf->add_forwarded_headers */ 1) {
        if (PROXYREQ_REVERSE == r->proxyreq) {
            const char *buf;

            /* Add X-Forwarded-For: so that the upstream has a chance to
             * determine, where the original request came from.
             */
            apr_table_mergen(r->headers_in, "X-Forwarded-For",
                             /* r->useragent_ip */ c->remote_ip);

            /* Add X-Forwarded-Host: so that upstream knows what the
             * original request hostname was.
             */
            if ((buf = apr_table_get(r->headers_in, "Host"))) {
                apr_table_mergen(r->headers_in, "X-Forwarded-Host", buf);
            }

            /* Add X-Forwarded-Server: so that upstream knows what the
             * name of this proxy server is (if there are more than one)
             * XXX: This duplicates Via: - do we strictly need it?
             */
            apr_table_mergen(r->headers_in, "X-Forwarded-Server",
                             r->server->server_hostname);
        }
    }

    proxy_run_fixups(r);
    /*
     * Make a copy of the headers_in table before clearing the connection
     * headers as we need the connection headers later in the http output
     * filter to prepare the correct response headers.
     *
     * Note: We need to take r->pool for apr_table_copy as the key / value
     * pairs in r->headers_in have been created out of r->pool and
     * p might be (and actually is) a longer living pool.
     * This would trigger the bad pool ancestry abort in apr_table_copy if
     * apr is compiled with APR_POOL_DEBUG.
     */
    headers_in_copy = apr_table_copy(r->pool, r->headers_in);
    proxy_clear_connection(p, headers_in_copy);
    /* send request headers */
    headers_in_array = apr_table_elts(headers_in_copy);
    headers_in = (const apr_table_entry_t *) headers_in_array->elts;
    for (counter = 0; counter < headers_in_array->nelts; counter++) {
        if (headers_in[counter].key == NULL
            || headers_in[counter].val == NULL

            /* Already sent */
            || !strcasecmp(headers_in[counter].key, "Host")

            /* Clear out hop-by-hop request headers not to send
             * RFC2616 13.5.1 says we should strip these headers
             */
            || !strcasecmp(headers_in[counter].key, "Keep-Alive")
            || !strcasecmp(headers_in[counter].key, "TE")
            || !strcasecmp(headers_in[counter].key, "Trailer")
            || !strcasecmp(headers_in[counter].key, "Upgrade")

            ) {
            continue;
        }
        /* Do we want to strip Proxy-Authorization ?
         * If we haven't used it, then NO
         * If we have used it then MAYBE: RFC2616 says we MAY propagate it.
         * So let's make it configurable by env.
         */
        if (!strcasecmp(headers_in[counter].key,"Proxy-Authorization")) {
            if (r->user != NULL) { /* we've authenticated */
                if (!apr_table_get(r->subprocess_env, "Proxy-Chain-Auth")) {
                    continue;
                }
            }
        }

        /* Skip Transfer-Encoding and Content-Length for now.
         */
        if (!strcasecmp(headers_in[counter].key, "Transfer-Encoding")) {
            *old_te_val = headers_in[counter].val;
            continue;
        }
        if (!strcasecmp(headers_in[counter].key, "Content-Length")) {
            *old_cl_val = headers_in[counter].val;
            continue;
        }

        /* for sub-requests, ignore freshness/expiry headers */
        if (r->main) {
            if (    !strcasecmp(headers_in[counter].key, "If-Match")
                || !strcasecmp(headers_in[counter].key, "If-Modified-Since")
                || !strcasecmp(headers_in[counter].key, "If-Range")
                || !strcasecmp(headers_in[counter].key, "If-Unmodified-Since")
                || !strcasecmp(headers_in[counter].key, "If-None-Match")) {
                continue;
            }
        }

        buf = apr_pstrcat(p, headers_in[counter].key, ": ",
                          headers_in[counter].val, CRLF,
                          NULL);
        ap_xlate_proto_to_ascii(buf, strlen(buf));
        e = apr_bucket_pool_create(buf, strlen(buf), p, c->bucket_alloc);
        APR_BRIGADE_INSERT_TAIL(header_brigade, e);
    }
    return OK;
}
/* NOTE: End copy from proxy_util.c */

module AP_MODULE_DECLARE_DATA proxy_wstunnel_module;

/*
 * Canonicalise http-like URLs.
 * scheme is the scheme for the URL
 * url is the URL starting with the first '/'
 * def_port is the default port for this scheme.
 */
static int proxy_wstunnel_canon(request_rec *r, char *url)
{
    char *host, *path, sport[7];
    char *search = NULL;
    const char *err;
    char *scheme;
    apr_port_t port, def_port;

    /* ap_port_of_scheme() */
    if (strncasecmp(url, "ws:", 3) == 0) {
        url += 3;
        scheme = "ws:";
        def_port = apr_uri_port_of_scheme("http");
    }
    else if (strncasecmp(url, "wss:", 4) == 0) {
        url += 4;
        scheme = "wss:";
        def_port = apr_uri_port_of_scheme("https");
    }
    else {
        return DECLINED;
    }

    port = def_port;
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "canonicalising URL %s", url);

    /*
     * do syntactic check.
     * We break the URL into host, port, path, search
     */
    err = ap_proxy_canon_netloc(r->pool, &url, NULL, NULL, &host, &port);
    if (err) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(02439) "error parsing URL %s: %s",
                      url, err);
        return HTTP_BAD_REQUEST;
    }

    /*
     * now parse path/search args, according to rfc1738:
     * process the path. With proxy-nocanon set (by
     * mod_proxy) we use the raw, unparsed uri
     */
    if (apr_table_get(r->notes, "proxy-nocanon")) {
        path = url;   /* this is the raw path */
    }
    else {
        path = ap_proxy_canonenc(r->pool, url, strlen(url), enc_path, 0,
                                 r->proxyreq);
        search = r->args;
    }
    if (path == NULL)
        return HTTP_BAD_REQUEST;

    apr_snprintf(sport, sizeof(sport), ":%d", port);

    if (ap_strchr_c(host, ':')) {
        /* if literal IPv6 address */
        host = apr_pstrcat(r->pool, "[", host, "]", NULL);
    }
    r->filename = apr_pstrcat(r->pool, "proxy:", scheme, "//", host, sport,
                              "/", path, (search) ? "?" : "",
                              (search) ? search : "", NULL);
    return OK;
}


static int proxy_wstunnel_transfer(request_rec *r, conn_rec *c_i, conn_rec *c_o,
                                     apr_bucket_brigade *bb, char *name)
{
    int rv;
#ifdef DEBUGGING
    apr_off_t len;
#endif

    do {
        apr_brigade_cleanup(bb);
        rv = ap_get_brigade(c_i->input_filters, bb, AP_MODE_READBYTES,
                            APR_NONBLOCK_READ, AP_IOBUFSIZE);
        if (rv == APR_SUCCESS) {
            if (c_o->aborted) {
                return APR_EPIPE;
            }
            if (APR_BRIGADE_EMPTY(bb)) {
                break;
            }
#ifdef DEBUGGING
            len = -1;
            apr_brigade_length(bb, 0, &len);
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(02440)
                          "read %" APR_OFF_T_FMT
                          " bytes from %s", len, name);
#endif
            rv = ap_pass_brigade(c_o->output_filters, bb);
            if (rv == APR_SUCCESS) {
                ap_fflush(c_o->output_filters, bb);
            }
            else {
                ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, APLOGNO(02441)
                              "error on %s - ap_pass_brigade",
                              name);
            }
        } else if (!APR_STATUS_IS_EAGAIN(rv) && !APR_STATUS_IS_EOF(rv)) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, rv, r, APLOGNO(02442)
                          "error on %s - ap_get_brigade",
                          name);
        }
    } while (rv == APR_SUCCESS);

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, rv, r, "wstunnel_transfer complete");

    if (APR_STATUS_IS_EAGAIN(rv)) {
        rv = APR_SUCCESS;
    }

    return rv;
}

/* Search thru the input filters and remove the reqtimeout one */
static void remove_reqtimeout(ap_filter_t *next)
{
    ap_filter_t *reqto = NULL;
    ap_filter_rec_t *filter;

    filter = ap_get_input_filter_handle("reqtimeout");
    if (!filter) {
        return;
    }

    while (next) {
        if (next->frec == filter) {
            reqto = next;
            break;
        }
        next = next->next;
    }
    if (reqto) {
        ap_remove_input_filter(reqto);
    }
}

/*
 * process the request and write the response.
 */
static int ap_proxy_wstunnel_request(apr_pool_t *p, request_rec *r,
                                proxy_conn_rec *conn,
                                proxy_worker *worker,
                                proxy_server_conf *conf,
                                apr_uri_t *uri,
                                char *url, char *server_portstr)
{
    apr_status_t rv = APR_SUCCESS;
    apr_pollset_t *pollset;
    apr_pollfd_t pollfd;
    const apr_pollfd_t *signalled;
    apr_int32_t pollcnt, pi;
    apr_int16_t pollevent;
    conn_rec *c = r->connection;
    apr_socket_t *sock = conn->sock;
    conn_rec *backconn = conn->connection;
    char *buf;
    apr_bucket_brigade *header_brigade;
    apr_bucket *e;
    char *old_cl_val = NULL;
    char *old_te_val = NULL;
    apr_bucket_brigade *bb = apr_brigade_create(p, c->bucket_alloc);
    // apr_socket_t *client_socket = ap_get_conn_socket(c);
    apr_socket_t *client_socket = ap_get_module_config(c->conn_config, &core_module);

    header_brigade = apr_brigade_create(p, backconn->bucket_alloc);

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "sending request");

    rv = ap_proxy_create_hdrbrgd(p, header_brigade, r, conn,
                                 worker, conf, uri, url, server_portstr,
                                 &old_cl_val, &old_te_val);
    if (rv != OK) {
        return rv;
    }

    buf = apr_pstrcat(p, "Upgrade: WebSocket", CRLF, "Connection: Upgrade", CRLF, CRLF, NULL);
    ap_xlate_proto_to_ascii(buf, strlen(buf));
    e = apr_bucket_pool_create(buf, strlen(buf), p, c->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(header_brigade, e);

    if ((rv = ap_proxy_pass_brigade(c->bucket_alloc, r, conn, backconn,
                                    header_brigade, 1)) != OK)
        return rv;

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "setting up poll()");

    if ((rv = apr_pollset_create(&pollset, 2, p, 0)) != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, APLOGNO(02443)
                      "error apr_pollset_create()");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

#if 0
    apr_socket_opt_set(sock, APR_SO_NONBLOCK, 1);
    apr_socket_opt_set(sock, APR_SO_KEEPALIVE, 1);
    apr_socket_opt_set(client_socket, APR_SO_NONBLOCK, 1);
    apr_socket_opt_set(client_socket, APR_SO_KEEPALIVE, 1);
#endif

    pollfd.p = p;
    pollfd.desc_type = APR_POLL_SOCKET;
    pollfd.reqevents = APR_POLLIN | APR_POLLHUP;
    pollfd.desc.s = sock;
    pollfd.client_data = NULL;
    apr_pollset_add(pollset, &pollfd);

    pollfd.desc.s = client_socket;
    apr_pollset_add(pollset, &pollfd);

    remove_reqtimeout(c->input_filters);

    r->output_filters = c->output_filters;
    r->proto_output_filters = c->output_filters;
    r->input_filters = c->input_filters;
    r->proto_input_filters = c->input_filters;

    /* This handler should take care of the entire connection; make it so that
     * nothing else is attempted on the connection after returning. */
    c->keepalive = AP_CONN_CLOSE;

    while (1) { /* Infinite loop until error (one side closes the connection) */
        if ((rv = apr_pollset_poll(pollset, -1, &pollcnt, &signalled))
            != APR_SUCCESS) {
            if (APR_STATUS_IS_EINTR(rv)) {
                continue;
            }
            ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, APLOGNO(02444) "error apr_poll()");
            return HTTP_INTERNAL_SERVER_ERROR;
        }
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(02445)
                      "woke from poll(), i=%d", pollcnt);

        for (pi = 0; pi < pollcnt; pi++) {
            const apr_pollfd_t *cur = &signalled[pi];

            if (cur->desc.s == sock) {
                pollevent = cur->rtnevents;
                if (pollevent & (APR_POLLIN | APR_POLLHUP)) {
                    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(02446)
                                  "sock was readable");
                    rv = proxy_wstunnel_transfer(r, backconn, c, bb, "sock");
                }
                else if (pollevent & APR_POLLERR) {
                    rv = APR_EPIPE;
                    ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, r, APLOGNO(02447)
                            "error on backconn");
                }
                else {
                    rv = APR_EGENERAL;
                    ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, r, APLOGNO(02605)
                            "unknown event on backconn %d", pollevent);
                }
            }
            else if (cur->desc.s == client_socket) {
                pollevent = cur->rtnevents;
                if (pollevent & (APR_POLLIN | APR_POLLHUP)) {
                    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(02448)
                                  "client was readable");
                    rv = proxy_wstunnel_transfer(r, c, backconn, bb, "client");
                }
                else if (pollevent & APR_POLLERR) {
                    rv = APR_EPIPE;
                    c->aborted = 1;
                    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(02607)
                            "error on client conn");
                }
                else {
                    rv = APR_EGENERAL;
                    ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, r, APLOGNO(02606)
                            "unknown event on client conn %d", pollevent);
                }
            }
            else {
                rv = APR_EBADF;
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, APLOGNO(02449)
                              "unknown socket in pollset");
            }

        }
        if (rv != APR_SUCCESS) {
            break;
        }
    }

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                  "finished with poll() - cleaning up");

    return OK;
}

/*
 */
static int proxy_wstunnel_handler(request_rec *r, proxy_worker *worker,
                             proxy_server_conf *conf,
                             char *url, const char *proxyname,
                             apr_port_t proxyport)
{
    int status;
    char server_portstr[32];
    proxy_conn_rec *backend = NULL;
    char *scheme;
    int retry;
    conn_rec *c = r->connection;
    apr_pool_t *p = r->pool;
    apr_uri_t *uri;
    int is_ssl = 0;

    if (strncasecmp(url, "wss:", 4) == 0) {
        scheme = "WSS";
        is_ssl = 1;
    }
    else if (strncasecmp(url, "ws:", 3) == 0) {
        scheme = "WS";
    }
    else {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(02450) "declining URL %s", url);
        return DECLINED;
    }

    uri = apr_palloc(p, sizeof(*uri));
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(02451) "serving URL %s", url);

    /* create space for state information */
    status = ap_proxy_acquire_connection(scheme, &backend, worker,
                                         r->server);
    if (status != OK) {
        if (backend) {
            backend->close = 1;
            ap_proxy_release_connection(scheme, backend, r->server);
        }
        return status;
    }

    backend->is_ssl = is_ssl;
    backend->close = 0;

    retry = 0;
    while (retry < 2) {
        char *locurl = url;
        /* Step One: Determine Who To Connect To */
        status = ap_proxy_determine_connection(p, r, conf, worker, backend,
                                               uri, &locurl, proxyname, proxyport,
                                               server_portstr,
                                               sizeof(server_portstr));

        if (status != OK)
            break;

        /* Step Two: Make the Connection */
        if (ap_proxy_connect_backend(scheme, backend, worker, r->server)) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(02452)
                          "failed to make connection to backend: %s",
                          backend->hostname);
            status = HTTP_SERVICE_UNAVAILABLE;
            break;
        }
        /* Step Three: Create conn_rec */
        if (!backend->connection) {
            if ((status = ap_proxy_connection_create(scheme, backend,
                                                     c, r->server)) != OK)
                break;
        }

        backend->close = 1; /* must be after ap_proxy_determine_connection */


        /* Step Three: Process the Request */
        status = ap_proxy_wstunnel_request(p, r, backend, worker, conf, uri, locurl,
                                      server_portstr);
        break;
    }

    /* Do not close the socket */
    ap_proxy_release_connection(scheme, backend, r->server);
    return status;
}

static void ap_proxy_http_register_hook(apr_pool_t *p)
{
    proxy_hook_scheme_handler(proxy_wstunnel_handler, NULL, NULL, APR_HOOK_FIRST);
    proxy_hook_canon_handler(proxy_wstunnel_canon, NULL, NULL, APR_HOOK_FIRST);
}

module AP_MODULE_DECLARE_DATA proxy_wstunnel_module = {
    STANDARD20_MODULE_STUFF,
    NULL,                       /* create per-directory config structure */
    NULL,                       /* merge per-directory config structures */
    NULL,                       /* create per-server config structure */
    NULL,                       /* merge per-server config structures */
    NULL,                       /* command apr_table_t */
    ap_proxy_http_register_hook /* register hooks */
};
