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

#ifndef HTTP_SOURCE_SIGNALS_H_INCLUDED
#define HTTP_SOURCE_SIGNALS_H_INCLUDED

#include "signal-slot-connector/signal-slot-connector.h"
#include "gsockaddr.h"

/*
 * Extension point for the http() source, analogous to the signals emitted by
 * the http() destination.  The source emits signal_http_source_request once
 * per incoming request, before its body is ingested.  A slot (installed by a
 * LogDriverPlugin, e.g. an authenticator) may inspect the request and reject
 * it, optionally overriding the HTTP response that is sent back.
 */
typedef struct _HTTPSourceRequestSignalData
{
  /* input: the incoming request (read-only) */
  const gchar *method;
  const gchar *path;
  GHashTable *headers;          /* lowercased header name -> value */
  GSockAddr *peer;

  /* output: a slot may reject the request and override the response */
  gboolean rejected;
  guint response_status;        /* 0 -> the source picks a default (401) */
  const gchar *response_body;   /* borrowed; copied by the source during the emit */
} HTTPSourceRequestSignalData;

#define signal_http_source_request SIGNAL(http_source, request, HTTPSourceRequestSignalData *)

#endif
