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
 */

#include <criterion/criterion.h>
#include "libtest/msg_parse_lib.h"
#include "libtest/filterx-lib.h"

#include "apphook.h"
#include "scratch-buffers.h"

#include "filterx/object-string.h"
#include "filterx/object-primitive.h"
#include "filterx/object-null.h"
#include "filterx/object-dict.h"
#include "filterx/object-list.h"
#include "filterx/expr-literal.h"
#include "filterx-func-geoip2.h"

/*
 * The origin of the database:
 * https://github.com/maxmind/MaxMind-DB/blob/ea7cd314bb55879b8cb1a059c425d53ff3b9b6cc/test-data/GeoIP2-Precision-Enterprise-Test.mmdb
 */
#define TEST_DATABASE TOP_SRCDIR "/modules/geoip2/tests/test.mmdb"
#define TEST_IP "2.125.160.216"

static FilterXExpr *
_construct(const gchar *ip, const gchar *database, const gchar *field, GError **error)
{
  GList *args = NULL;

  args = g_list_append(args, filterx_function_arg_new(NULL, filterx_literal_new(filterx_string_new(ip, -1))));

  if (database)
    args = g_list_append(args, filterx_function_arg_new("database",
                                                         filterx_literal_new(filterx_string_new(database, -1))));

  if (field)
    args = g_list_append(args, filterx_function_arg_new("field",
                                                         filterx_literal_new(filterx_string_new(field, -1))));

  GError *args_err = NULL;
  FilterXExpr *func = filterx_function_geoip2_new(filterx_function_args_new(args, &args_err), error);
  cr_assert_null(args_err);

  return func;
}

Test(filterx_func_geoip2, test_empty_args_error)
{
  GError *err = NULL;
  GError *args_err = NULL;
  FilterXExpr *func = filterx_function_geoip2_new(filterx_function_args_new(NULL, &args_err), &err);

  cr_assert_null(func);
  cr_assert_null(args_err);
  cr_assert_not_null(err);
  cr_assert(strstr(err->message, FILTERX_FUNC_GEOIP2_USAGE) != NULL);
  g_error_free(err);
}

Test(filterx_func_geoip2, test_nonexistent_database_error)
{
  GError *err = NULL;
  FilterXExpr *func = _construct(TEST_IP, "/nonexistent/geoip2-test-database.mmdb", NULL, &err);

  cr_assert_null(func);
  cr_assert_not_null(err);
  g_error_free(err);
}

Test(filterx_func_geoip2, test_field_argument_must_be_string_literal)
{
  GList *args = NULL;
  args = g_list_append(args, filterx_function_arg_new(NULL, filterx_literal_new(filterx_string_new(TEST_IP, -1))));
  args = g_list_append(args, filterx_function_arg_new("database",
                                                       filterx_literal_new(filterx_string_new(TEST_DATABASE, -1))));
  args = g_list_append(args, filterx_function_arg_new("field", filterx_literal_new(filterx_integer_new(5))));

  GError *err = NULL;
  GError *args_err = NULL;
  FilterXExpr *func = filterx_function_geoip2_new(filterx_function_args_new(args, &args_err), &err);

  cr_assert_null(func);
  cr_assert_null(args_err);
  cr_assert_not_null(err);
  cr_assert(strstr(err->message, FILTERX_FUNC_GEOIP2_USAGE) != NULL);
  g_error_free(err);
}

Test(filterx_func_geoip2, test_default_field_is_country_iso_code)
{
  GError *err = NULL;
  FilterXExpr *func = _construct(TEST_IP, TEST_DATABASE, NULL, &err);

  cr_assert_null(err);
  cr_assert_not_null(func);

  FilterXObject *obj = init_and_eval_expr(func);

  cr_assert_not_null(obj);
  assert_marshaled_object(obj, "GB", LM_VT_STRING);

  filterx_object_unref(obj);
  filterx_expr_unref(func);
}

Test(filterx_func_geoip2, test_integer_field)
{
  GError *err = NULL;
  FilterXExpr *func = _construct(TEST_IP, TEST_DATABASE, "country.geoname_id", &err);

  cr_assert_null(err);
  cr_assert_not_null(func);

  FilterXObject *obj = init_and_eval_expr(func);

  cr_assert_not_null(obj);
  assert_marshaled_object(obj, "2635167", LM_VT_INTEGER);

  filterx_object_unref(obj);
  filterx_expr_unref(func);
}

Test(filterx_func_geoip2, test_boolean_field)
{
  GError *err = NULL;
  FilterXExpr *func = _construct(TEST_IP, TEST_DATABASE, "country.is_in_european_union", &err);

  cr_assert_null(err);
  cr_assert_not_null(func);

  FilterXObject *obj = init_and_eval_expr(func);

  cr_assert_not_null(obj);
  assert_marshaled_object(obj, "true", LM_VT_BOOLEAN);

  filterx_object_unref(obj);
  filterx_expr_unref(func);
}

Test(filterx_func_geoip2, test_double_field)
{
  GError *err = NULL;
  FilterXExpr *func = _construct(TEST_IP, TEST_DATABASE, "location.latitude", &err);

  cr_assert_null(err);
  cr_assert_not_null(func);

  FilterXObject *obj = init_and_eval_expr(func);

  cr_assert_not_null(obj);
  assert_object_json_equals(obj, "51.75");

  filterx_object_unref(obj);
  filterx_expr_unref(func);
}

Test(filterx_func_geoip2, test_nested_dict_field)
{
  GError *err = NULL;
  FilterXExpr *func = _construct(TEST_IP, TEST_DATABASE, "postal", &err);

  cr_assert_null(err);
  cr_assert_not_null(func);

  FilterXObject *obj = init_and_eval_expr(func);

  cr_assert_not_null(obj);
  cr_assert(filterx_object_is_type(obj, &FILTERX_TYPE_NAME(dict)));
  assert_object_json_equals(obj, "{\"code\":\"OX1\",\"confidence\":20}");

  filterx_object_unref(obj);
  filterx_expr_unref(func);
}

Test(filterx_func_geoip2, test_array_field)
{
  GError *err = NULL;
  FilterXExpr *func = _construct(TEST_IP, TEST_DATABASE, "subdivisions", &err);

  cr_assert_null(err);
  cr_assert_not_null(func);

  FilterXObject *obj = init_and_eval_expr(func);

  cr_assert_not_null(obj);
  cr_assert(filterx_object_is_type(obj, &FILTERX_TYPE_NAME(list)));

  guint64 len;
  cr_assert(filterx_object_len(obj, &len));
  cr_assert_eq(len, 2);

  FilterXObject *index_zero = filterx_integer_new(0);
  FilterXObject *first = filterx_object_get_subscript(obj, index_zero);
  cr_assert_not_null(first);
  cr_assert(filterx_object_is_type(first, &FILTERX_TYPE_NAME(dict)));

  FilterXObject *iso_code_key = filterx_string_new("iso_code", -1);
  FilterXObject *iso_code = filterx_object_get_subscript(first, iso_code_key);
  cr_assert_not_null(iso_code);
  assert_marshaled_object(iso_code, "ENG", LM_VT_STRING);

  filterx_object_unref(iso_code);
  filterx_object_unref(iso_code_key);
  filterx_object_unref(first);
  filterx_object_unref(index_zero);
  filterx_object_unref(obj);
  filterx_expr_unref(func);
}

Test(filterx_func_geoip2, test_whole_entry)
{
  GError *err = NULL;
  FilterXExpr *func = _construct(TEST_IP, TEST_DATABASE, "", &err);

  cr_assert_null(err);
  cr_assert_not_null(func);

  FilterXObject *obj = init_and_eval_expr(func);

  cr_assert_not_null(obj);
  cr_assert(filterx_object_is_type(obj, &FILTERX_TYPE_NAME(dict)));

  FilterXObject *country_key = filterx_string_new("country", -1);
  FilterXObject *country = filterx_object_get_subscript(obj, country_key);
  cr_assert_not_null(country);
  cr_assert(filterx_object_is_type(country, &FILTERX_TYPE_NAME(dict)));

  filterx_object_unref(country);
  filterx_object_unref(country_key);
  filterx_object_unref(obj);
  filterx_expr_unref(func);
}

Test(filterx_func_geoip2, test_unknown_ip_returns_null)
{
  GError *err = NULL;
  FilterXExpr *func = _construct("127.0.0.1", TEST_DATABASE, NULL, &err);

  cr_assert_null(err);
  cr_assert_not_null(func);

  FilterXObject *obj = init_and_eval_expr(func);

  cr_assert_not_null(obj);
  cr_assert(filterx_object_is_type(obj, &FILTERX_TYPE_NAME(null)));

  filterx_object_unref(obj);
  filterx_expr_unref(func);
}

Test(filterx_func_geoip2, test_unknown_field_returns_null)
{
  GError *err = NULL;
  FilterXExpr *func = _construct(TEST_IP, TEST_DATABASE, "does.not.exist", &err);

  cr_assert_null(err);
  cr_assert_not_null(func);

  FilterXObject *obj = init_and_eval_expr(func);

  cr_assert_not_null(obj);
  cr_assert(filterx_object_is_type(obj, &FILTERX_TYPE_NAME(null)));

  filterx_object_unref(obj);
  filterx_expr_unref(func);
}

static void
setup(void)
{
  app_startup();
  init_libtest_filterx();
}

static void
teardown(void)
{
  scratch_buffers_explicit_gc();
  deinit_libtest_filterx();
  app_shutdown();
}

TestSuite(filterx_func_geoip2, .init = setup, .fini = teardown);
