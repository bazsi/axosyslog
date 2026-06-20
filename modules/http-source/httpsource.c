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
#include "http-server-listener.h"
#include "logsource.h"
#include "mainloop-worker.h"
#include "messages.h"
#include "stats/stats-cluster-key-builder.h"
#include "logmsg/logmsg.h"
#include "cfg.h"

#define DEFAULT_MAX_REQUEST_SIZE (8 * 1024 * 1024)

/*
 * A minimal LogSource: the listener thread (a registered mainloop worker
 * thread) posts messages into it directly via log_source_post().  We only
 * override wakeup(), which the flow-control/ack path calls when the window
 * reopens, to resume the connections suspended on this source.
 */
typedef struct _HTTPSource
{
  LogSource super;
  HTTPSourceDriver *owner;
} HTTPSource;

struct HTTPSourceDriver
{
  LogSrcDriver super;

  gint port;
  gchar *bind_addr;
  gchar *path;
  gsize max_request_size;

  LogSourceOptions source_options;
  HTTPServerListener *listener;
  HTTPSource *source;

  /* connections suspended because this source's flow-control window is full;
   * touched by the listener thread (suspend) and the ack path (resume) */
  GMutex backpressure_lock;
  GList *suspended_connections;
};

static void
_resume_all_connections(HTTPSourceDriver *self)
{
  g_mutex_lock(&self->backpressure_lock);
  GList *to_resume = self->suspended_connections;
  self->suspended_connections = NULL;
  g_mutex_unlock(&self->backpressure_lock);

  if (!to_resume)
    return;

  for (GList *l = to_resume; l; l = l->next)
    http_server_listener_resume_connection((struct MHD_Connection *) l->data);
  g_list_free(to_resume);

  if (self->listener)
    http_server_listener_wakeup(self->listener);
}

/* LogSource wakeup: the window reopened, resume the suspended connections */
static void
_source_wakeup(LogSource *s)
{
  HTTPSource *self = (HTTPSource *) s;
  _resume_all_connections(self->owner);
}

/*
 * HTTPServerRequestHandler: runs on the listener thread.  Posts the body line
 * by line; on a full window the connection is suspended and resumed later.
 */
static HTTPServerRequestResult
_handle_request_body(gpointer user_data, struct MHD_Connection *connection,
                     const gchar *body, gsize body_len, gsize *offset, GSockAddr *peer)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) user_data;
  LogSource *source = &self->source->super;

  while (*offset < body_len)
    {
      const gchar *p = body + *offset;
      const gchar *nl = memchr(p, '\n', body_len - *offset);
      gsize seg_len = nl ? (gsize) (nl - p) : (body_len - *offset);
      gsize advance = nl ? seg_len + 1 : seg_len;

      gsize line_len = seg_len;
      if (line_len > 0 && p[line_len - 1] == '\r')
        line_len--;

      if (line_len == 0)
        {
          *offset += advance;
          continue;
        }

      /* fast path check, then re-check under the lock to close the race with
       * a concurrent window reopen (_source_wakeup) */
      if (!log_source_free_to_send(source))
        {
          g_mutex_lock(&self->backpressure_lock);
          if (!log_source_free_to_send(source))
            {
              http_server_listener_suspend_connection(connection);
              self->suspended_connections = g_list_prepend(self->suspended_connections, connection);
              g_mutex_unlock(&self->backpressure_lock);

              /* flush what we have already posted so the destination can drain
               * it and reopen the window (otherwise the resume never comes) */
              main_loop_worker_invoke_batch_callbacks();
              return HTTP_SERVER_REQUEST_SUSPENDED;
            }
          g_mutex_unlock(&self->backpressure_lock);
        }

      LogMessage *msg = log_msg_new_empty();
      log_msg_set_value(msg, LM_V_MESSAGE, p, line_len);
      log_msg_set_value(msg, LM_V_TRANSPORT, "http", 4);
      if (peer)
        log_msg_set_saddr_ref(msg, g_sockaddr_ref(peer));
      log_source_post(source, msg);

      *offset += advance;
    }

  main_loop_worker_invoke_batch_callbacks();
  return HTTP_SERVER_REQUEST_COMPLETE;
}

/* HTTPServerRequestCompletion: drop the connection from the suspended set */
static void
_request_completed(gpointer user_data, struct MHD_Connection *connection)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) user_data;

  g_mutex_lock(&self->backpressure_lock);
  self->suspended_connections = g_list_remove(self->suspended_connections, connection);
  g_mutex_unlock(&self->backpressure_lock);
}

static HTTPSource *
_http_source_new(GlobalConfig *cfg, HTTPSourceDriver *owner)
{
  HTTPSource *self = g_new0(HTTPSource, 1);
  log_source_init_instance(&self->super, cfg);
  self->super.wakeup = _source_wakeup;
  self->owner = owner;
  return self;
}

static void
_format_stats_kb(HTTPSourceDriver *self, StatsClusterKeyBuilder *kb)
{
  stats_cluster_key_builder_add_legacy_label(kb, stats_cluster_label("driver", "http"));

  gchar buf[64];
  g_snprintf(buf, sizeof(buf), "%d", self->port);
  stats_cluster_key_builder_add_legacy_label(kb, stats_cluster_label("port", buf));

  if (self->path)
    stats_cluster_key_builder_add_legacy_label(kb, stats_cluster_label("path", self->path));
}

static gboolean
_pre_config_init(LogPipe *s)
{
  /* reserve a thread slot for the (shared) listener thread; over-allocating
   * when several sources share a listener is harmless */
  main_loop_worker_allocate_thread_space(1);
  return TRUE;
}

static gboolean
_post_config_init(LogPipe *s)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) s;

  if (!self->listener)
    return FALSE;

  return http_server_listener_start(self->listener);
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

  if (!log_src_driver_init_method(s))
    return FALSE;

  log_source_options_init(&self->source_options, cfg, self->super.super.group);

  self->source = _http_source_new(cfg, self);

  StatsClusterKeyBuilder *kb = stats_cluster_key_builder_new();
  _format_stats_kb(self, kb);
  log_source_set_options(&self->source->super, &self->source_options, self->super.super.id, kb, TRUE,
                         self->super.super.super.expr_node);

  log_pipe_append(&self->source->super.super, &self->super.super.super);
  if (!log_pipe_init(&self->source->super.super))
    return FALSE;

  self->listener = http_server_listener_acquire(cfg, self->bind_addr, self->port);
  if (!self->listener)
    return FALSE;

  http_server_listener_register_path(self->listener, self->path,
                                     _handle_request_body, _request_completed, self,
                                     self->max_request_size);
  return TRUE;
}

static gboolean
_deinit(LogPipe *s)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) s;
  GlobalConfig *cfg = log_pipe_get_config(s);

  if (self->listener)
    http_server_listener_unregister_path(self->listener, self->path, self);

  /* unblock any connection still waiting on our window so it can finish */
  _resume_all_connections(self);

  if (self->source)
    {
      log_pipe_deinit(&self->source->super.super);
      log_pipe_unref(&self->source->super.super);
      self->source = NULL;
    }

  if (self->listener)
    {
      http_server_listener_release(cfg, self->listener);
      self->listener = NULL;
    }

  return log_src_driver_deinit_method(s);
}

static void
_free(LogPipe *s)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) s;

  if (self->source)
    {
      log_pipe_unref(&self->source->super.super);
      self->source = NULL;
    }

  g_free(self->bind_addr);
  g_free(self->path);
  g_list_free(self->suspended_connections);
  g_mutex_clear(&self->backpressure_lock);
  log_source_options_destroy(&self->source_options);

  log_src_driver_free(s);
}

LogSourceOptions *
http_sd_get_source_options(LogDriver *s)
{
  HTTPSourceDriver *self = (HTTPSourceDriver *) s;
  return &self->source_options;
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
  log_src_driver_init_instance(&self->super, cfg);

  self->bind_addr = g_strdup("0.0.0.0");
  self->max_request_size = DEFAULT_MAX_REQUEST_SIZE;
  g_mutex_init(&self->backpressure_lock);
  log_source_options_defaults(&self->source_options);

  self->super.super.super.init = _init;
  self->super.super.super.deinit = _deinit;
  self->super.super.super.free_fn = _free;
  self->super.super.super.pre_config_init = _pre_config_init;
  self->super.super.super.post_config_init = _post_config_init;

  return &self->super.super;
}
