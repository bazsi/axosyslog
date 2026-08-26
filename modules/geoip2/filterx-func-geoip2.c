/*
 * Copyright (c) 2026 Axoflow
 * Copyright (c) 2026 Balazs Scheidler <balazs.scheidler@axoflow.com>
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

#include "filterx-func-geoip2.h"
#include "maxminddb-helper.h"

#include "filterx/object-string.h"
#include "filterx/object-primitive.h"
#include "filterx/object-null.h"
#include "filterx/object-dict.h"
#include "filterx/object-list.h"
#include "filterx/filterx-sequence.h"
#include "filterx/filterx-eval.h"
#include "filterx/object-extractor.h"

#include "messages.h"

typedef struct FilterXFunctionGeoIP2_
{
  FilterXFunction super;
  FilterXExpr *ip;
  MMDB_s *database;
  gchar *database_path;
  gchar **entry_path;
} FilterXFunctionGeoIP2;

static FilterXObject *_entry_data_list_to_filterx(MMDB_entry_data_list_s **entry_data_list, gint *status);

static FilterXObject *
_scalar_to_filterx(MMDB_entry_data_s *entry_data, gint *status)
{
  *status = MMDB_SUCCESS;

  switch (entry_data->type)
    {
    case MMDB_DATA_TYPE_UTF8_STRING:
      return filterx_string_new(entry_data->utf8_string, entry_data->data_size);
    case MMDB_DATA_TYPE_DOUBLE:
      return filterx_double_new(entry_data->double_value);
    case MMDB_DATA_TYPE_FLOAT:
      return filterx_double_new((gdouble) entry_data->float_value);
    case MMDB_DATA_TYPE_UINT16:
      return filterx_integer_new(entry_data->uint16);
    case MMDB_DATA_TYPE_UINT32:
      return filterx_integer_new(entry_data->uint32);
    case MMDB_DATA_TYPE_INT32:
      return filterx_integer_new(entry_data->int32);
    case MMDB_DATA_TYPE_UINT64:
      return filterx_integer_new((gint64) entry_data->uint64);
    case MMDB_DATA_TYPE_BOOLEAN:
      return filterx_boolean_new(entry_data->boolean);
    default:
      *status = MMDB_INVALID_DATA_ERROR;
      return NULL;
    }
}

static FilterXObject *
_map_to_filterx(MMDB_entry_data_list_s **entry_data_list, gint *status)
{
  guint32 size = (*entry_data_list)->entry_data.data_size;
  FilterXObject *dict = filterx_dict_new();
  filterx_object_cow_prepare(&dict);

  *entry_data_list = (*entry_data_list)->next;
  for (; size && *entry_data_list; size--)
    {
      if (MMDB_DATA_TYPE_UTF8_STRING != (*entry_data_list)->entry_data.type)
        {
          *status = MMDB_INVALID_DATA_ERROR;
          goto error;
        }

      const gchar *key = (*entry_data_list)->entry_data.utf8_string;
      gsize key_len = (*entry_data_list)->entry_data.data_size;
      *entry_data_list = (*entry_data_list)->next;

      FilterXObject *value = _entry_data_list_to_filterx(entry_data_list, status);
      if (MMDB_SUCCESS != *status)
        goto error;

      FILTERX_STRING_DECLARE_ON_STACK(dict_key, key, key_len);
      gboolean ok = filterx_object_set_subscript(dict, dict_key, &value);
      FILTERX_STRING_CLEAR_FROM_STACK(dict_key);
      filterx_object_unref(value);
      if (!ok)
        {
          *status = MMDB_INVALID_DATA_ERROR;
          goto error;
        }
    }

  filterx_object_set_dirty(dict, FALSE);
  return dict;

error:
  filterx_object_unref(dict);
  return NULL;
}

static FilterXObject *
_array_to_filterx(MMDB_entry_data_list_s **entry_data_list, gint *status)
{
  guint32 size = (*entry_data_list)->entry_data.data_size;
  FilterXObject *list = filterx_list_new();
  filterx_object_cow_prepare(&list);

  *entry_data_list = (*entry_data_list)->next;
  for (; size && *entry_data_list; size--)
    {
      FilterXObject *value = _entry_data_list_to_filterx(entry_data_list, status);
      if (MMDB_SUCCESS != *status)
        goto error;

      gboolean ok = filterx_sequence_append(list, &value);
      filterx_object_unref(value);
      if (!ok)
        {
          *status = MMDB_INVALID_DATA_ERROR;
          goto error;
        }
    }

  filterx_object_set_dirty(list, FALSE);
  return list;

error:
  filterx_object_unref(list);
  return NULL;
}

static FilterXObject *
_entry_data_list_to_filterx(MMDB_entry_data_list_s **entry_data_list, gint *status)
{
  switch ((*entry_data_list)->entry_data.type)
    {
    case MMDB_DATA_TYPE_MAP:
      return _map_to_filterx(entry_data_list, status);
    case MMDB_DATA_TYPE_ARRAY:
      return _array_to_filterx(entry_data_list, status);
    default:
      break;
    }

  FilterXObject *result = _scalar_to_filterx(&(*entry_data_list)->entry_data, status);
  *entry_data_list = (*entry_data_list)->next;
  return result;
}

typedef enum
{
  GEOIP2_LOOKUP_FOUND,
  GEOIP2_LOOKUP_NOT_FOUND,
  GEOIP2_LOOKUP_ERROR,
} GeoIP2LookupResult;

static GeoIP2LookupResult
_lookup_entry_path(FilterXFunctionGeoIP2 *self, MMDB_entry_s *start, FilterXObject **out, gint *mmdb_status)
{
  MMDB_entry_data_s entry_data;

  *mmdb_status = MMDB_aget_value(start, &entry_data, (const char *const *const) self->entry_path);
  if (*mmdb_status != MMDB_SUCCESS)
    return (*mmdb_status == MMDB_LOOKUP_PATH_DOES_NOT_MATCH_DATA_ERROR)
           ? GEOIP2_LOOKUP_NOT_FOUND : GEOIP2_LOOKUP_ERROR;

  if (!entry_data.has_data)
    return GEOIP2_LOOKUP_NOT_FOUND;

  if (entry_data.type != MMDB_DATA_TYPE_MAP && entry_data.type != MMDB_DATA_TYPE_ARRAY)
    {
      *out = _scalar_to_filterx(&entry_data, mmdb_status);
      return (*mmdb_status == MMDB_SUCCESS) ? GEOIP2_LOOKUP_FOUND : GEOIP2_LOOKUP_ERROR;
    }

  MMDB_entry_s sub_entry = { .mmdb = start->mmdb, .offset = entry_data.offset };
  MMDB_entry_data_list_s *entry_data_list = NULL;
  *mmdb_status = MMDB_get_entry_data_list(&sub_entry, &entry_data_list);
  if (*mmdb_status != MMDB_SUCCESS)
    return GEOIP2_LOOKUP_ERROR;

  MMDB_entry_data_list_s *cursor = entry_data_list;
  *out = _entry_data_list_to_filterx(&cursor, mmdb_status);
  MMDB_free_entry_data_list(entry_data_list);

  return (*mmdb_status == MMDB_SUCCESS) ? GEOIP2_LOOKUP_FOUND : GEOIP2_LOOKUP_ERROR;
}

static FilterXObject *
_eval(FilterXExpr *s)
{
  FilterXFunctionGeoIP2 *self = (FilterXFunctionGeoIP2 *) s;

  FilterXObject *ip_obj = filterx_expr_eval(self->ip);
  if (!ip_obj)
    return NULL;

  FilterXObject *result = NULL;
  const gchar *ip_str;
  gsize ip_len;

  if (!filterx_object_extract_string_as_cstr_len(ip_obj, &ip_str, &ip_len))
    {
      filterx_eval_push_error_info_printf("Failed to evaluate geoip2()",
                                          "ip argument must be string, got: %s",
                                          filterx_object_get_type_name(ip_obj));
      goto exit;
    }

  int gai_error, mmdb_error;
  MMDB_lookup_result_s lookup_result = MMDB_lookup_string(self->database, ip_str, &gai_error, &mmdb_error);

  if (!lookup_result.found_entry)
    {
      if (gai_error != 0)
        msg_debug("geoip2(): getaddrinfo failed",
                  evt_tag_str("ip", ip_str),
                  evt_tag_str("gai_error", gai_strerror(gai_error)));
      else if (mmdb_error != MMDB_SUCCESS)
        msg_debug("geoip2(): maxminddb error",
                  evt_tag_str("ip", ip_str),
                  evt_tag_str("error", MMDB_strerror(mmdb_error)));

      result = filterx_null_new();
      goto exit;
    }

  gint status;
  GeoIP2LookupResult lookup_status = _lookup_entry_path(self, &lookup_result.entry, &result, &status);
  switch (lookup_status)
    {
    case GEOIP2_LOOKUP_FOUND:
      break;
    case GEOIP2_LOOKUP_NOT_FOUND:
      result = filterx_null_new();
      break;
    case GEOIP2_LOOKUP_ERROR:
      msg_debug("geoip2(): maxminddb error",
                evt_tag_str("ip", ip_str),
                evt_tag_str("error", MMDB_strerror(status)));
      result = filterx_null_new();
      break;
    }

exit:
  filterx_object_unref(ip_obj);
  return result;
}

static void
_free(FilterXExpr *s)
{
  FilterXFunctionGeoIP2 *self = (FilterXFunctionGeoIP2 *) s;

  filterx_expr_unref(self->ip);
  g_free(self->database_path);
  g_strfreev(self->entry_path);
  if (self->database)
    {
      MMDB_close(self->database);
      g_free(self->database);
    }

  filterx_function_free_method(&self->super);
}

static gboolean
_walk(FilterXExpr *s, FilterXExprWalkFunc f, gpointer user_data)
{
  FilterXFunctionGeoIP2 *self = (FilterXFunctionGeoIP2 *) s;

  return filterx_expr_visit(s, &self->ip, f, user_data);
}

static FilterXExpr *
_extract_ip_arg(FilterXFunctionArgs *args, GError **error)
{
  FilterXExpr *ip_expr = filterx_function_args_get_expr(args, 0);
  if (!ip_expr)
    {
      g_set_error(error, FILTERX_FUNCTION_ERROR, FILTERX_FUNCTION_ERROR_CTOR_FAIL,
                  "argument must be set: ip. " FILTERX_FUNC_GEOIP2_USAGE);
      return NULL;
    }

  return ip_expr;
}

static gboolean
_extract_optional_args(FilterXFunctionGeoIP2 *self, FilterXFunctionArgs *args, GError **error)
{
  gboolean exists;
  gsize len;

  const gchar *database = filterx_function_args_get_named_literal_string(args, "database", &len, &exists);
  if (exists)
    {
      if (!database)
        {
          g_set_error(error, FILTERX_FUNCTION_ERROR, FILTERX_FUNCTION_ERROR_CTOR_FAIL,
                      "database argument must be a string literal. " FILTERX_FUNC_GEOIP2_USAGE);
          return FALSE;
        }
      self->database_path = g_strdup(database);
    }
  else
    {
      self->database_path = mmdb_default_database();
    }

  if (!self->database_path)
    {
      g_set_error(error, FILTERX_FUNCTION_ERROR, FILTERX_FUNCTION_ERROR_CTOR_FAIL,
                  "database argument must be set, no default GeoIP database found. " FILTERX_FUNC_GEOIP2_USAGE);
      return FALSE;
    }

  const gchar *field = filterx_function_args_get_named_literal_string(args, "field", &len, &exists);
  if (exists && !field)
    {
      g_set_error(error, FILTERX_FUNCTION_ERROR, FILTERX_FUNCTION_ERROR_CTOR_FAIL,
                  "field argument must be a string literal. " FILTERX_FUNC_GEOIP2_USAGE);
      return FALSE;
    }

  self->entry_path = g_strsplit(exists ? field : "country.iso_code", ".", -1);

  return TRUE;
}

static gboolean
_extract_args(FilterXFunctionGeoIP2 *self, FilterXFunctionArgs *args, GError **error)
{
  gsize args_len = filterx_function_args_len(args);
  if (args_len != 1)
    {
      g_set_error(error, FILTERX_FUNCTION_ERROR, FILTERX_FUNCTION_ERROR_CTOR_FAIL,
                  "invalid number of arguments. " FILTERX_FUNC_GEOIP2_USAGE);
      return FALSE;
    }

  self->ip = _extract_ip_arg(args, error);
  if (!self->ip)
    return FALSE;

  return _extract_optional_args(self, args, error);
}

static gboolean
_open_database(FilterXFunctionGeoIP2 *self, GError **error)
{
  self->database = g_new0(MMDB_s, 1);

  if (!mmdb_open_database(self->database_path, self->database))
    {
      g_free(self->database);
      self->database = NULL;
      g_set_error(error, FILTERX_FUNCTION_ERROR, FILTERX_FUNCTION_ERROR_CTOR_FAIL,
                  "geoip2(): could not open database %s", self->database_path);
      return FALSE;
    }

  return TRUE;
}

FilterXExpr *
filterx_function_geoip2_new(FilterXFunctionArgs *args, GError **error)
{
  FilterXFunctionGeoIP2 *self = g_new0(FilterXFunctionGeoIP2, 1);

  filterx_function_init_instance(&self->super, "geoip2", FXE_READ);
  self->super.super.eval = _eval;
  self->super.super.walk_children = _walk;
  self->super.super.free_fn = _free;

  if (!_extract_args(self, args, error) ||
      !filterx_function_args_check(args, error) ||
      !_open_database(self, error))
    goto error;

  filterx_function_args_free(args);
  return &self->super.super;

error:
  filterx_function_args_free(args);
  filterx_expr_unref(&self->super.super);
  return NULL;
}

FILTERX_FUNCTION(geoip2, filterx_function_geoip2_new);
