/*
 * Copyright (c) 2026 Axoflow
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#ifndef HTTP_LISTENER_H
#define HTTP_LISTENER_H

#include "syslog-ng.h"
#include "gsockaddr.h"
#include "transport/tls-context.h"

typedef struct HTTPServerListener HTTPServerListener;

/* opaque per-connection token, passed to request handlers */
typedef struct HTTPServerConnection HTTPServerConnection;

/* Outcome of an HTTPServerRequestHandler invocation. */
typedef enum
{
  HTTP_SERVER_REQUEST_COMPLETE,    /* the whole body was processed, send the response */
  HTTP_SERVER_REQUEST_SUSPENDED,   /* processing was paused, the connection is suspended */
} HTTPServerRequestResult;

/*
 * Invoked on the listener thread once a POST to a registered path has been
 * fully received (and again after each retry).  Process the body starting at
 * *offset, advancing it as data is consumed.  To apply backpressure, suspend
 * the connection with http_server_listener_suspend_connection() and return
 * HTTP_SERVER_REQUEST_SUSPENDED; the handler is invoked again after the
 * listener is woken with http_server_listener_wakeup().
 */
typedef HTTPServerRequestResult (*HTTPServerRequestHandler)(gpointer user_data,
                                                            HTTPServerConnection *connection,
                                                            const gchar *body, gsize body_len, gsize *offset,
                                                            GSockAddr *peer);

/*
 * Invoked on the listener thread once a request has been fully received, before
 * the body is dispatched to the request handler (exactly once per request).
 * Return TRUE to let processing continue, or FALSE to reject the request: in
 * that case the response set with http_server_connection_respond() is sent
 * (defaulting to 403 if none was set) and the body handler is not called.  This
 * is the hook used to implement authentication.  May be NULL.
 */
typedef gboolean (*HTTPServerRequestValidator)(gpointer user_data, HTTPServerConnection *connection);

/* Invoked when a request terminates (whether completed or aborted). May be NULL. */
typedef void (*HTTPServerRequestCompletion)(gpointer user_data, HTTPServerConnection *connection);

/* --- per-request accessors, callable from a validator or request handler --- */

/* the request method ("POST", ...) and the routed path (query string stripped) */
const gchar *http_server_connection_get_method(HTTPServerConnection *connection);
const gchar *http_server_connection_get_path(HTTPServerConnection *connection);

/* the peer address (borrowed), or NULL */
GSockAddr *http_server_connection_get_peer(HTTPServerConnection *connection);

/* look up a request header by name (case-insensitive), or NULL if absent */
const gchar *http_server_connection_get_header(HTTPServerConnection *connection, const gchar *name);

/* the full request header set, keyed by lowercased header name -> value */
GHashTable *http_server_connection_get_headers(HTTPServerConnection *connection);

/*
 * Override the response sent for this request with the given status and body.
 * The body is copied.  Typically called from a validator that rejects the
 * request, but a request handler may also use it to customise the 2xx response.
 */
void http_server_connection_respond(HTTPServerConnection *connection, guint status, const gchar *body);

/*
 * A shared HTTP listener, keyed by (config, bind address, port).  Multiple
 * callers listening on the same address:port share a single listener, each
 * registering its own URL path with a request handler and an opaque user_data
 * pointer.
 *
 * The listener owns one dedicated, syslog-ng-registered thread that runs an
 * ivykis event loop, accepts connections and parses HTTP/1.1 requests with
 * llhttp.  Request handlers run on that thread.  Flow control is applied per
 * connection by suspending the connection (which stops reading it) and, when
 * the listener is woken, retrying the suspended connections, so a full window
 * on one URL never blocks the others sharing the port.
 *
 * acquire()/release()/register_path()/unregister_path()/start() are called
 * from the main thread during driver init/deinit.  suspend_connection() is
 * called from a request handler (on the listener thread).  wakeup() is
 * thread-safe and may be called from any thread.
 */
/*
 * Acquire (creating if needed) the shared listener for bind_addr:port.  When
 * tls_context is non-NULL the listener terminates TLS on every accepted
 * connection using it; the listener takes its own reference.  TLS is a
 * property of the socket, so it is fixed by whoever first creates the
 * listener: sources that later share the same port inherit that setting and a
 * conflicting tls() is ignored with a warning.
 */
HTTPServerListener *http_server_listener_acquire(GlobalConfig *cfg, const gchar *bind_addr, gint port,
                                                 gint connection_timeout, TLSContext *tls_context);
void http_server_listener_release(GlobalConfig *cfg, HTTPServerListener *self);

/* start the listener thread (idempotent); call from post_config_init */
gboolean http_server_listener_start(HTTPServerListener *self);

gboolean http_server_listener_register_path(HTTPServerListener *self, const gchar *path,
                                            HTTPServerRequestValidator validator,
                                            HTTPServerRequestHandler handler,
                                            HTTPServerRequestCompletion completion,
                                            gpointer user_data, gsize max_request_size);
void http_server_listener_unregister_path(HTTPServerListener *self, const gchar *path,
                                          gpointer user_data);

/* suspend a connection for flow control; call from a request handler */
void http_server_listener_suspend_connection(HTTPServerConnection *connection);

/* wake the event loop so it retries connections suspended for flow control;
 * thread-safe, callable from any thread */
void http_server_listener_wakeup(HTTPServerListener *self);

#endif
