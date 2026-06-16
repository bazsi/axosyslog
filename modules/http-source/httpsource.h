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

#ifndef HTTPSOURCE_H
#define HTTPSOURCE_H

#include "driver.h"
#include "logsource.h"
#include "gsockaddr.h"

struct MHD_Connection;

typedef struct HTTPSourceDriver HTTPSourceDriver;

typedef enum
{
  HTTP_SD_POST_DONE,       /* the whole body was posted */
  HTTP_SD_POST_SUSPENDED,  /* the source window is full, the connection was suspended */
} HTTPSourcePostStatus;

LogDriver *http_sd_new(GlobalConfig *cfg);

void http_sd_set_port(LogDriver *s, gint port);
void http_sd_set_bind_addr(LogDriver *s, const gchar *addr);
void http_sd_set_path(LogDriver *s, const gchar *path);
void http_sd_set_max_request_size(LogDriver *s, gsize size);

/*
 * Called by the listener thread to post a request body as messages.  Posting
 * starts at *offset and the offset is advanced as lines are accepted.  If the
 * source's flow-control window fills up, the connection is suspended (via
 * MHD_suspend_connection()) and HTTP_SD_POST_SUSPENDED is returned; the
 * connection is resumed automatically once the window reopens, and the
 * listener will be called again to continue from *offset.
 */
HTTPSourcePostStatus http_sd_post_body(HTTPSourceDriver *self, struct MHD_Connection *connection,
                                       const gchar *body, gsize body_len, gsize *offset, GSockAddr *peer);

/* drop a connection from the suspended set (e.g. when the request terminates) */
void http_sd_forget_connection(HTTPSourceDriver *self, struct MHD_Connection *connection);

#endif
