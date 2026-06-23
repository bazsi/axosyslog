/*
 * Copyright (c) 2026 Axoflow
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
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

struct MHD_Connection;

typedef struct HTTPServerListener HTTPServerListener;

/* Outcome of an HTTPServerRequestHandler invocation. */
typedef enum
{
  HTTP_SERVER_REQUEST_COMPLETE,    /* the whole body was processed, send the response */
  HTTP_SERVER_REQUEST_SUSPENDED,   /* processing was paused, the connection is suspended */
} HTTPServerRequestResult;

/*
 * Invoked on the listener thread once a POST to a registered path has been
 * fully received (and again after each resume).  Process the body starting at
 * *offset, advancing it as data is consumed.  To apply backpressure, suspend
 * the connection with http_server_listener_suspend_connection() and return
 * HTTP_SERVER_REQUEST_SUSPENDED; the handler is invoked again once the
 * connection is resumed.
 */
typedef HTTPServerRequestResult (*HTTPServerRequestHandler)(gpointer user_data,
    struct MHD_Connection *connection, const gchar *body, gsize body_len, gsize *offset,
    GSockAddr *peer);

/* Invoked when a request terminates (whether completed or aborted). */
typedef void (*HTTPServerRequestCompletion)(gpointer user_data, struct MHD_Connection *connection);

/*
 * A shared HTTP listener, keyed by (config, bind address, port).  Multiple
 * callers listening on the same address:port share a single listener (and a
 * single libmicrohttpd daemon), each registering its own URL path with a
 * request handler and an opaque user_data pointer.
 *
 * The listener owns one dedicated, syslog-ng-registered thread that drives
 * libmicrohttpd in external-polling mode.  Request handlers run on that thread.
 * Flow control is applied per connection using libmicrohttpd's suspend/resume
 * facility (exposed via the *_connection() helpers below), so a full window on
 * one URL never blocks the others sharing the port.
 *
 * acquire()/release()/register_path()/unregister_path()/start() are called
 * from the main thread during driver init/deinit. The *_connection() and
 * wakeup() helpers are thread-safe.
 */
HTTPServerListener *http_server_listener_acquire(GlobalConfig *cfg, const gchar *bind_addr, gint port);
void http_server_listener_release(GlobalConfig *cfg, HTTPServerListener *self);

/* start the listener thread (idempotent); call from post_config_init */
gboolean http_server_listener_start(HTTPServerListener *self);

gboolean http_server_listener_register_path(HTTPServerListener *self, const gchar *path,
                                             HTTPServerRequestHandler handler,
                                             HTTPServerRequestCompletion completion,
                                             gpointer user_data, gsize max_request_size);
void http_server_listener_unregister_path(HTTPServerListener *self, const gchar *path,
                                           gpointer user_data);

/* per-connection flow control (thread-safe), for use from request handlers and
 * from the resume path */
void http_server_listener_suspend_connection(struct MHD_Connection *connection);
void http_server_listener_resume_connection(struct MHD_Connection *connection);

/* wake the event loop so it processes connections resumed from another thread */
void http_server_listener_wakeup(HTTPServerListener *self);

#endif
