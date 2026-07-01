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

#include "splunk-hec-auth.h"
#include "modules/http-source/http-source-signals.h"

typedef struct _SplunkHECAuth
{
  LogDriverPlugin super;
  GList *tokens;                /* accepted tokens (owned gchar *) */
} SplunkHECAuth;

/* Splunk HEC error responses (see the HEC REST API documentation) */
#define HEC_ERR_TOKEN_REQUIRED "{\"text\":\"Token is required\",\"code\":2}"
#define HEC_ERR_INVALID_TOKEN  "{\"text\":\"Invalid token\",\"code\":4}"

/* strip the "Splunk " / "Bearer " scheme prefix and return the bare token, or
 * NULL if the header does not carry a recognised bearer scheme */
static const gchar *
_extract_token(const gchar *authorization)
{
  static const gchar *schemes[] = { "Splunk ", "Bearer ", NULL };

  for (gint i = 0; schemes[i]; i++)
    {
      gsize len = strlen(schemes[i]);
      if (g_ascii_strncasecmp(authorization, schemes[i], len) == 0)
        return authorization + len;
    }
  return NULL;
}

static gboolean
_token_is_accepted(SplunkHECAuth *self, const gchar *token)
{
  for (GList *l = self->tokens; l; l = l->next)
    {
      /* constant-time-ish compare is overkill here; a plain compare is fine */
      if (strcmp((const gchar *) l->data, token) == 0)
        return TRUE;
    }
  return FALSE;
}

static void
_on_http_source_request(gpointer object, HTTPSourceRequestSignalData *data)
{
  SplunkHECAuth *self = (SplunkHECAuth *) object;

  const gchar *authorization = g_hash_table_lookup(data->headers, "authorization");
  if (!authorization)
    {
      data->rejected = TRUE;
      data->response_status = 401;
      data->response_body = HEC_ERR_TOKEN_REQUIRED;
      return;
    }

  const gchar *token = _extract_token(authorization);
  if (!token || !_token_is_accepted(self, token))
    {
      data->rejected = TRUE;
      data->response_status = 401;
      data->response_body = HEC_ERR_INVALID_TOKEN;
    }
}

static gboolean
_attach(LogDriverPlugin *s, LogDriver *driver)
{
  SplunkHECAuth *self = (SplunkHECAuth *) s;

  if (!self->tokens)
    {
      msg_error("hec-auth(): at least one token() must be configured");
      return FALSE;
    }

  CONNECT(driver->signal_slot_connector, signal_http_source_request, _on_http_source_request, self);
  return TRUE;
}

static void
_detach(LogDriverPlugin *s, LogDriver *driver)
{
  SplunkHECAuth *self = (SplunkHECAuth *) s;

  DISCONNECT(driver->signal_slot_connector, signal_http_source_request, _on_http_source_request, self);
}

static void
_free(LogDriverPlugin *s)
{
  SplunkHECAuth *self = (SplunkHECAuth *) s;

  g_list_free_full(self->tokens, g_free);
  log_driver_plugin_free_method(s);
}

void
splunk_hec_auth_add_token(LogDriverPlugin *s, const gchar *token)
{
  SplunkHECAuth *self = (SplunkHECAuth *) s;
  self->tokens = g_list_append(self->tokens, g_strdup(token));
}

LogDriverPlugin *
splunk_hec_auth_new(void)
{
  SplunkHECAuth *self = g_new0(SplunkHECAuth, 1);

  log_driver_plugin_init_instance(&self->super, "splunk-hec-auth");
  self->super.attach = _attach;
  self->super.detach = _detach;
  self->super.free_fn = _free;
  return &self->super;
}
