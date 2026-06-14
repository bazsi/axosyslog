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

typedef struct HTTPServerListener HTTPServerListener;
typedef struct HTTPSourceDriver HTTPSourceDriver;

/*
 * A shared HTTP listener, keyed by (config, bind address, port).  Multiple
 * http() sources listening on the same address:port share a single listener
 * (and a single libmicrohttpd daemon), each registering its own URL path.
 *
 * The listener owns one dedicated, syslog-ng-registered thread that drives
 * libmicrohttpd in external-polling mode.  Request handlers run on that thread
 * and post messages directly into the matching source's LogSource (the
 * destination-side queue is multi-producer safe).  Flow control is applied
 * per connection using libmicrohttpd's suspend/resume facility, so a full
 * window on one URL never blocks the others sharing the port.
 *
 * acquire()/release()/register_path()/unregister_path()/start() are called
 * from the main thread during driver init/deinit. wakeup() is thread-safe.
 */
HTTPServerListener *http_server_listener_acquire(GlobalConfig *cfg, const gchar *bind_addr, gint port);
void http_server_listener_release(HTTPServerListener *self);

/* start the listener thread (idempotent); call from post_config_init */
gboolean http_server_listener_start(HTTPServerListener *self);

gboolean http_server_listener_register_path(HTTPServerListener *self, const gchar *path,
                                             HTTPSourceDriver *source, gsize max_request_size);
void http_server_listener_unregister_path(HTTPServerListener *self, const gchar *path,
                                           HTTPSourceDriver *source);

/* wake the event loop so it processes connections resumed from another thread */
void http_server_listener_wakeup(HTTPServerListener *self);

#endif
