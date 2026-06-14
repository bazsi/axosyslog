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

#ifndef HTTPSOURCE_LISTENER_H
#define HTTPSOURCE_LISTENER_H

#include "syslog-ng.h"
#include "logthrsource/logthrsourcedrv.h"

typedef struct HTTPServerListener HTTPServerListener;

/*
 * A shared HTTP listener, keyed by (config, bind address, port).  Multiple
 * http() sources that listen on the same address:port share a single
 * listener (and a single underlying libmicrohttpd daemon), each registering
 * its own URL path.
 *
 * The lifetime is reference counted.  The first source to acquire a given
 * address:port creates the daemon (and becomes the "owner" that drives the
 * event loop in its worker thread, see http_server_listener_drive()).  The
 * daemon is torn down when the last reference is released.
 *
 * acquire()/release()/register_path()/unregister_path() must be called from
 * the main thread (during driver init/deinit).
 */
HTTPServerListener *http_server_listener_acquire(GlobalConfig *cfg, const gchar *bind_addr, gint port,
                                                 gboolean *created);
void http_server_listener_release(HTTPServerListener *self);

gboolean http_server_listener_register_path(HTTPServerListener *self, const gchar *path,
                                             LogThreadedSourceWorker *worker, gsize max_request_size);
void http_server_listener_unregister_path(HTTPServerListener *self, const gchar *path,
                                           LogThreadedSourceWorker *worker);

/* drive the libmicrohttpd event loop until http_server_listener_request_stop()
 * is called; must run in a syslog-ng worker thread (the owner's run loop) */
void http_server_listener_drive(HTTPServerListener *self);
void http_server_listener_request_stop(HTTPServerListener *self);

#endif
