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

#ifndef SPLUNK_HEC_AUTH_H_INCLUDED
#define SPLUNK_HEC_AUTH_H_INCLUDED 1

#include "driver.h"

/*
 * A LogDriverPlugin for the http() source that authenticates incoming requests
 * the way a Splunk HTTP Event Collector does: the client must present an
 * "Authorization: Splunk <token>" (or "Bearer <token>") header carrying one of
 * the configured tokens.  Requests without a valid token are rejected with a
 * Splunk-style 401 JSON error.
 */
LogDriverPlugin *splunk_hec_auth_new(void);
void splunk_hec_auth_add_token(LogDriverPlugin *self, const gchar *token);

#endif
