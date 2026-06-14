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

#include "httpsource.h"
#include "httpsource-listener.h"
#include "logthrsource/logthrsourcedrv.h"
#include "messages.h"
#include "stats/stats-cluster-key-builder.h"

#define DEFAULT_MAX_REQUEST_SIZE (8 * 1024 * 1024)

struct HTTPSourceDriver
{
  LogThreadedSourceDriver super;

  gint port;
  gchar *bind_addr;
  gchar *path;
  gsize max_request_size;

  HTTPServerListener *listener;
  gboolean is_owner;
  LogThreadedSourceWorker *worker;

  /* parking primitives for the non-owning worker thread */
  GMutex park_lock;
  GCond park_cond;
  gboolean exit_requested;
};

/* runs in the worker thread */
static void
_worker_run(LogThreadedSourceWorker *w)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) w->control;

  if (self->is_owner)
    {
      http_server_listener_drive(self->listener);
      return;
    }

  /* a non-owning worker has nothing to do itself: the owner's thread drives
   * the shared listener and posts into this source's LogSource directly */
  g_mutex_lock(&self->park_lock);
  while (!self->exit_requested)
    g_cond_wait(&self->park_cond, &self->park_lock);
  g_mutex_unlock(&self->park_lock);
}

static void
_worker_request_exit(LogThreadedSourceWorker *w)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) w->control;

  g_mutex_lock(&self->park_lock);
  self->exit_requested = TRUE;
  g_cond_signal(&self->park_cond);
  g_mutex_unlock(&self->park_lock);

  if (self->is_owner)
    http_server_listener_request_stop(self->listener);
}

static LogThreadedSourceWorker *
_construct_worker(LogThreadedSourceDriver *s, gint worker_index)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) s;

  LogThreadedSourceWorker *worker = g_new0(LogThreadedSourceWorker, 1);
  log_threaded_source_worker_init_instance(worker, s, worker_index);

  worker->run = _worker_run;
  worker->request_exit = _worker_request_exit;

  self->worker = worker;
  return worker;
}

static void
_format_stats_key(LogThreadedSourceDriver *s, StatsClusterKeyBuilder *kb)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) s;

  stats_cluster_key_builder_add_legacy_label(kb, stats_cluster_label("driver", "http"));

  gchar buf[64];
  g_snprintf(buf, sizeof(buf), "%d", self->port);
  stats_cluster_key_builder_add_legacy_label(kb, stats_cluster_label("port", buf));

  if (self->path)
    stats_cluster_key_builder_add_legacy_label(kb, stats_cluster_label("path", self->path));
}

static gboolean
_init(LogPipe *s)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) s;
  GlobalConfig *cfg = log_pipe_get_config(s);

  if (self->port <= 0 || self->port > 65535)
    {
      msg_error("http(): the port() option is mandatory and must be between 1 and 65535",
                log_pipe_location_tag(s));
      return FALSE;
    }

  if (!self->path)
    {
      msg_error("http(): the path() option is mandatory", log_pipe_location_tag(s));
      return FALSE;
    }

  if (!log_threaded_source_driver_init_method(s))
    return FALSE;

  gboolean created = FALSE;
  self->listener = http_server_listener_acquire(cfg, self->bind_addr, self->port, &created);
  if (!self->listener)
    return FALSE;

  self->is_owner = created;
  http_server_listener_register_path(self->listener, self->path, self->worker, self->max_request_size);

  return TRUE;
}

static gboolean
_deinit(LogPipe *s)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) s;

  if (self->listener)
    http_server_listener_unregister_path(self->listener, self->path, self->worker);

  gboolean result = log_threaded_source_driver_deinit_method(s);

  if (self->listener)
    {
      http_server_listener_release(self->listener);
      self->listener = NULL;
    }

  return result;
}

static void
_free(LogPipe *s)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) s;

  g_free(self->bind_addr);
  g_free(self->path);
  g_mutex_clear(&self->park_lock);
  g_cond_clear(&self->park_cond);

  log_threaded_source_driver_free_method(s);
}

void
http_sd_set_port(LogDriver *s, gint port)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) s;
  self->port = port;
}

void
http_sd_set_bind_addr(LogDriver *s, const gchar *addr)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) s;
  g_free(self->bind_addr);
  self->bind_addr = g_strdup(addr);
}

void
http_sd_set_path(LogDriver *s, const gchar *path)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) s;
  g_free(self->path);
  self->path = g_strdup(path);
}

void
http_sd_set_max_request_size(LogDriver *s, gsize size)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) s;
  self->max_request_size = size;
}

LogDriver *
http_sd_new(GlobalConfig *cfg)
{
  HTTPSourceDriver *self = g_new0(HTTPSourceDriver, 1);
  log_threaded_source_driver_init_instance(&self->super, cfg);

  self->bind_addr = g_strdup("0.0.0.0");
  self->max_request_size = DEFAULT_MAX_REQUEST_SIZE;
  g_mutex_init(&self->park_lock);
  g_cond_init(&self->park_cond);

  self->super.super.super.super.init = _init;
  self->super.super.super.super.deinit = _deinit;
  self->super.super.super.super.free_fn = _free;
  self->super.format_stats_key = _format_stats_key;
  self->super.worker_construct = _construct_worker;

  log_threaded_source_driver_set_transport_name(&self->super, "http");

  return &self->super.super.super;
}
