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

/*
 * $(bucket <bucket-count> value...)
 * $(bucket --weights W1,W2,... value...)
 * $(bucket --rr [--batch N] <bucket-count>)
 * $(bucket --rr [--batch N] --weights W1,W2,...)
 *
 * Hashes its value arguments and returns a bucket index in the range
 * [0, bucket-count) (or [0, number-of-weights) in the
 * --weights form). The mapping from a given set of values to a bucket
 * index is deterministic within a running configuration, so it can be used
 * to consistently route a message down one of several log paths, e.g.
 * based on $HOST.
 *
 * Options:
 *   --weights W1,W2,...  Comma separated list of non-negative integer
 *                         weights, one per bucket, replacing the plain
 *                         bucket-count argument. A bucket's share of the
 *                         input is proportional to its weight; a weight of
 *                         zero drains a bucket entirely.
 *   --rr                  Non-sticky, round robin mode: instead of hashing
 *                         value arguments (which are not accepted in this
 *                         mode), messages are dealt out to the buckets in
 *                         turn, driven by a per-thread counter that starts
 *                         from zero. Each thread cycles through the buckets
 *                         on its own, so the first message of every thread
 *                         lands in bucket 0; with --weights the cycle
 *                         visits each bucket as many times as its weight.
 *   --batch N             With --rr, keep N consecutive messages of a
 *                         thread in the same bucket before moving on to the
 *                         next one. Defaults to 100.
 *
 * NOTE: the hash function used is not part of any documented contract: the
 * mapping from values to bucket indexes may change across syslog-ng
 * versions or platforms, so it must not be relied upon to stay stable
 * across upgrades.
 */

#include "mainloop-worker.h"
#include "atomic-gssize.h"

/* upper bound on the bucket count, the number of weights and each
 * individual weight: keeps the weight sum well within guint range */
#define TF_BUCKET_MAX_BUCKETS 4096

#define TF_BUCKET_DEFAULT_BATCH_SIZE 100

/* per-thread message counter of the --rr mode, padded so that the
 * counters of different threads don't share a cache line */
typedef union _TFBucketCounter
{
  guint64 value;          /* slots owned by a single worker thread */
  atomic_gssize shared;   /* the slot shared by threads without a worker index */
  gchar __pad[64];
} TFBucketCounter;

/* one slot per worker thread index, plus a shared one at the end for
 * threads that don't have an index (e.g. the main thread) */
typedef struct _TFBucketCounters
{
  gint num_workers;
  TFBucketCounter slots[];
} TFBucketCounters;

typedef struct _TFBucketState
{
  TFSimpleFuncState super;
  guint num_buckets;
  guint *weights;         /* NULL for the plain (uniform bucket-count) form */
  guint weights_sum;

  /* --rr */
  gboolean round_robin;
  guint64 batch_size;

  /* allocated on first use, see _tf_bucket_get_counters() */
  TFBucketCounters *counters;
} TFBucketState;

static gboolean
_tf_bucket_parse_weights(const gchar *weights_str, guint **out_weights, guint *out_num_buckets,
                         guint *out_weights_sum, GError **error)
{
  gchar **tokens = g_strsplit(weights_str, ",", 0);
  guint n = g_strv_length(tokens);

  if (n == 0)
    {
      g_set_error(error, LOG_TEMPLATE_ERROR, LOG_TEMPLATE_ERROR_COMPILE,
                  "$(bucket) --weights requires a comma separated list of at least one weight");
      g_strfreev(tokens);
      return FALSE;
    }

  if (n > TF_BUCKET_MAX_BUCKETS)
    {
      g_set_error(error, LOG_TEMPLATE_ERROR, LOG_TEMPLATE_ERROR_COMPILE,
                  "$(bucket) --weights accepts at most %d weights, got %u", TF_BUCKET_MAX_BUCKETS, n);
      g_strfreev(tokens);
      return FALSE;
    }

  guint *weights = g_new0(guint, n);
  guint sum = 0;
  gboolean success = TRUE;

  for (guint i = 0; i < n; i++)
    {
      gint64 w;

      if (!parse_int64(tokens[i], &w) || w < 0 || w > TF_BUCKET_MAX_BUCKETS)
        {
          g_set_error(error, LOG_TEMPLATE_ERROR, LOG_TEMPLATE_ERROR_COMPILE,
                      "$(bucket) --weights must be a comma separated list of non-negative integers "
                      "not greater than %d, got \"%s\"", TF_BUCKET_MAX_BUCKETS, tokens[i]);
          success = FALSE;
          break;
        }
      weights[i] = (guint) w;
      sum += weights[i];
    }
  g_strfreev(tokens);

  if (success && sum == 0)
    {
      g_set_error(error, LOG_TEMPLATE_ERROR, LOG_TEMPLATE_ERROR_COMPILE,
                  "$(bucket) --weights must not all be zero");
      success = FALSE;
    }

  if (!success)
    {
      g_free(weights);
      return FALSE;
    }

  *out_weights = weights;
  *out_num_buckets = n;
  *out_weights_sum = sum;
  return TRUE;
}

static gboolean
_tf_bucket_parse_bucket_count(const gchar *count_str, guint *out_num_buckets, GError **error)
{
  gint64 count;

  if (!parse_int64(count_str, &count) || count < 1 || count > TF_BUCKET_MAX_BUCKETS)
    {
      g_set_error(error, LOG_TEMPLATE_ERROR, LOG_TEMPLATE_ERROR_COMPILE,
                  "$(bucket) bucket count must be a positive integer not greater than %d, got \"%s\"",
                  TF_BUCKET_MAX_BUCKETS, count_str);
      return FALSE;
    }
  *out_num_buckets = (guint) count;
  return TRUE;
}

static gboolean
_tf_bucket_parse_batch_size(const gchar *batch_size_str, guint64 *out_batch_size, GError **error)
{
  gint64 batch_size;

  if (!parse_int64(batch_size_str, &batch_size) || batch_size < 1)
    {
      g_set_error(error, LOG_TEMPLATE_ERROR, LOG_TEMPLATE_ERROR_COMPILE,
                  "$(bucket) --batch must be a positive integer, got \"%s\"", batch_size_str);
      return FALSE;
    }
  *out_batch_size = (guint64) batch_size;
  return TRUE;
}

/* $(bucket --rr [--batch N] <bucket-count>)
 * $(bucket --rr [--batch N] --weights W1,W2,...)
 *
 * NOTE: the weights (if any) are already parsed into state at this point */
static gboolean
_tf_bucket_prepare_round_robin(LogTemplateFunction *self, TFBucketState *state, LogTemplate *parent,
                               gint argc, gchar *argv[], const gchar *batch_size_str, GError **error)
{
  gint num_positional_args = state->weights ? 0 : 1;

  if (argc - 1 < num_positional_args)
    {
      g_set_error(error, LOG_TEMPLATE_ERROR, LOG_TEMPLATE_ERROR_COMPILE,
                  "$(bucket) --rr requires a bucket count or --weights, "
                  "usage: $(bucket --rr [--batch N] <bucket-count>)");
      return FALSE;
    }

  if (argc - 1 > num_positional_args)
    {
      g_set_error(error, LOG_TEMPLATE_ERROR, LOG_TEMPLATE_ERROR_COMPILE,
                  "$(bucket) --rr does not take value arguments, as the bucket is not derived from "
                  "message content, got \"%s\"", argv[num_positional_args + 1]);
      return FALSE;
    }

  if (!state->weights && !_tf_bucket_parse_bucket_count(argv[1], &state->num_buckets, error))
    return FALSE;

  state->batch_size = TF_BUCKET_DEFAULT_BATCH_SIZE;
  if (batch_size_str && !_tf_bucket_parse_batch_size(batch_size_str, &state->batch_size, error))
    return FALSE;

  state->round_robin = TRUE;

  /* no value arguments, nothing to compile: argv[0] is the function name */
  return tf_simple_func_prepare(self, state, parent, 1, argv, error);
}

static gboolean
tf_bucket_prepare(LogTemplateFunction *self, gpointer s, LogTemplate *parent, gint argc, gchar *argv[],
                  GError **error)
{
  TFBucketState *state = (TFBucketState *) s;
  GOptionContext *ctx;
  gchar *weights_str = NULL;
  gboolean round_robin = FALSE;
  gchar *batch_size_str = NULL;
  gboolean success = FALSE;
  GOptionEntry bucket_options[] =
  {
    { "weights", 0, 0, G_OPTION_ARG_STRING, &weights_str, NULL, NULL },
    { "rr", 0, 0, G_OPTION_ARG_NONE, &round_robin, NULL, NULL },
    { "batch", 0, 0, G_OPTION_ARG_STRING, &batch_size_str, NULL, NULL },
    { NULL }
  };

  ctx = g_option_context_new("bucket");
  g_option_context_set_ignore_unknown_options(ctx, FALSE);
  g_option_context_add_main_entries(ctx, bucket_options, NULL);

  if (!g_option_context_parse(ctx, &argc, &argv, error))
    {
      g_option_context_free(ctx);
      goto exit;
    }
  g_option_context_free(ctx);

  if (batch_size_str && !round_robin)
    {
      g_set_error(error, LOG_TEMPLATE_ERROR, LOG_TEMPLATE_ERROR_COMPILE,
                  "$(bucket) --batch is only valid together with --rr");
      goto exit;
    }

  if (weights_str
      && !_tf_bucket_parse_weights(weights_str, &state->weights, &state->num_buckets, &state->weights_sum, error))
    goto exit;

  if (round_robin)
    {
      success = _tf_bucket_prepare_round_robin(self, state, parent, argc, argv, batch_size_str, error);
      goto exit;
    }

  if (state->weights)
    {
      if (argc < 2)
        {
          g_set_error(error, LOG_TEMPLATE_ERROR, LOG_TEMPLATE_ERROR_COMPILE,
                      "$(bucket) requires at least one value argument");
          goto exit;
        }

      success = tf_simple_func_prepare(self, state, parent, argc, argv, error);
      goto exit;
    }

  if (argc < 3)
    {
      g_set_error(error, LOG_TEMPLATE_ERROR, LOG_TEMPLATE_ERROR_COMPILE,
                  "$(bucket) requires a bucket count and at least one value argument, "
                  "usage: $(bucket <bucket-count> value...)");
      goto exit;
    }

  if (!_tf_bucket_parse_bucket_count(argv[1], &state->num_buckets, error))
    goto exit;

  gchar **value_argv = g_new(gchar *, argc - 1);
  value_argv[0] = argv[0];
  for (gint i = 2; i < argc; i++)
    value_argv[i - 1] = argv[i];

  success = tf_simple_func_prepare(self, state, parent, argc - 1, value_argv, error);
  g_free(value_argv);

exit:
  g_free(weights_str);
  g_free(batch_size_str);
  return success;
}

/* MurmurHash3 fmix32. DJB2's multiplier 33 is 1 mod 32, so mod 2^k the raw
 * hash is the byte sum: anagrams collide, neighbouring hostnames round-robin */
static inline guint32
_tf_bucket_fmix32(guint32 h)
{
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;
  return h;
}

static guint32
_tf_bucket_hash(const gchar *data, gsize len)
{
  guint32 h = 5381;

  for (gsize i = 0; i < len; i++)
    h = (h << 5) + h + (guchar) data[i];

  return _tf_bucket_fmix32(h);
}

static guint32
_tf_bucket_hash_combine(gint argc, GString *argv[])
{
  GString *buf = scratch_buffers_alloc();

  for (gint i = 0; i < argc; i++)
    {
      /* separate values so ("ab","c") and ("a","bc") don't collide */
      if (i > 0)
        g_string_append_c(buf, '\x1e');
      g_string_append_len(buf, argv[i]->str, argv[i]->len);
    }
  return _tf_bucket_hash(buf->str, buf->len);
}

static guint
_tf_bucket_select(TFBucketState *state, guint64 hash)
{
  if (!state->weights)
    return hash % state->num_buckets;

  guint slot = hash % state->weights_sum;
  guint cumulative = 0;

  for (guint i = 0; i < state->num_buckets; i++)
    {
      cumulative += state->weights[i];
      if (slot < cumulative)
        return i;
    }
  g_assert_not_reached();
}

/* The number of worker threads is only known once the configuration is
 * initialized, which happens after the templates are compiled but before
 * the first message gets here, so the counters are sized on first use. */
static TFBucketCounters *
_tf_bucket_get_counters(TFBucketState *state)
{
  TFBucketCounters *counters = g_atomic_pointer_get(&state->counters);

  if (G_LIKELY(counters))
    return counters;

  gint num_workers = main_loop_worker_get_max_number_of_threads();
  counters = g_malloc0(sizeof(TFBucketCounters) + sizeof(TFBucketCounter) * (num_workers + 1));
  counters->num_workers = num_workers;

  if (!g_atomic_pointer_compare_and_exchange(&state->counters, NULL, counters))
    {
      /* another thread got here first */
      g_free(counters);
      counters = g_atomic_pointer_get(&state->counters);
    }
  return counters;
}

/* --rr: the per-thread counter divided by the batch size, so that
 * batch-size consecutive messages of a thread are dealt to the same bucket */
static guint64
_tf_bucket_next_round_robin_slot(TFBucketState *state)
{
  TFBucketCounters *counters = _tf_bucket_get_counters(state);
  gint thread_index = main_loop_worker_get_thread_index();
  guint64 counter;

  if (thread_index >= 0 && thread_index < counters->num_workers)
    {
      /* this slot is only ever touched by the thread owning the index */
      counter = counters->slots[thread_index].value++;
    }
  else
    {
      counter = (guint64) atomic_gssize_inc(&counters->slots[counters->num_workers].shared);
    }

  return counter / state->batch_size;
}

static void
tf_bucket_call(LogTemplateFunction *self, gpointer s, const LogTemplateInvokeArgs *args, GString *result,
               LogMessageValueType *type)
{
  TFBucketState *state = (TFBucketState *) s;
  guint64 slot;

  if (state->round_robin)
    slot = _tf_bucket_next_round_robin_slot(state);
  else
    slot = _tf_bucket_hash_combine(state->super.argc, (GString **) args->argv);

  guint bucket = _tf_bucket_select(state, slot);

  *type = LM_VT_INTEGER;
  g_string_append_printf(result, "%u", bucket);
}

static void
tf_bucket_free_state(gpointer s)
{
  TFBucketState *state = (TFBucketState *) s;

  g_free(state->weights);
  g_free(state->counters);
  tf_simple_func_free_state(&state->super);
}

TEMPLATE_FUNCTION(TFBucketState, tf_bucket, tf_bucket_prepare, tf_simple_func_eval, tf_bucket_call,
                  tf_bucket_free_state, NULL);
