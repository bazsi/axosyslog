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
#include "driver.h"
#include "apphook.h"
#include "modules/http-source/http-source-signals.h"
#include <criterion/criterion.h>

/* drive one request through a plugin attached to a driver connector and return
 * the (possibly mutated) signal data */
static HTTPSourceRequestSignalData
_emit_request(LogDriverPlugin *plugin, const gchar *authorization)
{
  LogDriver driver = { 0 };
  driver.signal_slot_connector = signal_slot_connector_new();

  cr_assert(log_driver_plugin_attach(plugin, &driver));

  GHashTable *headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (authorization)
    g_hash_table_insert(headers, g_strdup("authorization"), g_strdup(authorization));

  HTTPSourceRequestSignalData data =
  {
    .method = "POST",
    .path = "/services/collector/event",
    .headers = headers,
    .rejected = FALSE,
    .response_status = 0,
    .response_body = NULL,
  };

  EMIT(driver.signal_slot_connector, signal_http_source_request, &data);

  log_driver_plugin_detach(plugin, &driver);
  g_hash_table_unref(headers);
  signal_slot_connector_free(driver.signal_slot_connector);
  return data;
}

Test(http_adapters, test_hec_auth_accepts_valid_splunk_token)
{
  LogDriverPlugin *auth = splunk_hec_auth_new();
  splunk_hec_auth_add_token(auth, "good-token");

  HTTPSourceRequestSignalData data = _emit_request(auth, "Splunk good-token");
  cr_assert_not(data.rejected);

  log_driver_plugin_free(auth);
}

Test(http_adapters, test_hec_auth_accepts_valid_bearer_token)
{
  LogDriverPlugin *auth = splunk_hec_auth_new();
  splunk_hec_auth_add_token(auth, "good-token");

  HTTPSourceRequestSignalData data = _emit_request(auth, "Bearer good-token");
  cr_assert_not(data.rejected);

  log_driver_plugin_free(auth);
}

Test(http_adapters, test_hec_auth_rejects_invalid_token)
{
  LogDriverPlugin *auth = splunk_hec_auth_new();
  splunk_hec_auth_add_token(auth, "good-token");

  HTTPSourceRequestSignalData data = _emit_request(auth, "Splunk wrong-token");
  cr_assert(data.rejected);
  cr_assert_eq(data.response_status, 401);

  log_driver_plugin_free(auth);
}

Test(http_adapters, test_hec_auth_rejects_missing_authorization)
{
  LogDriverPlugin *auth = splunk_hec_auth_new();
  splunk_hec_auth_add_token(auth, "good-token");

  HTTPSourceRequestSignalData data = _emit_request(auth, NULL);
  cr_assert(data.rejected);
  cr_assert_eq(data.response_status, 401);

  log_driver_plugin_free(auth);
}

Test(http_adapters, test_hec_auth_rejects_unknown_scheme)
{
  LogDriverPlugin *auth = splunk_hec_auth_new();
  splunk_hec_auth_add_token(auth, "good-token");

  HTTPSourceRequestSignalData data = _emit_request(auth, "Basic good-token");
  cr_assert(data.rejected);

  log_driver_plugin_free(auth);
}

Test(http_adapters, test_hec_auth_accepts_any_configured_token)
{
  LogDriverPlugin *auth = splunk_hec_auth_new();
  splunk_hec_auth_add_token(auth, "first");
  splunk_hec_auth_add_token(auth, "second");

  cr_assert_not(_emit_request(auth, "Splunk first").rejected);
  cr_assert_not(_emit_request(auth, "Splunk second").rejected);
  cr_assert(_emit_request(auth, "Splunk third").rejected);

  log_driver_plugin_free(auth);
}

static void
setup(void)
{
  app_startup();
}

static void
teardown(void)
{
  app_shutdown();
}

TestSuite(http_adapters, .init = setup, .fini = teardown);
