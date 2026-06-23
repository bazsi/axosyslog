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

#include "http-server-listener.h"
#include "http-source-config.h"
#include "mainloop-threaded-worker.h"
#include "messages.h"
#include "atomic.h"
#include "gsockaddr.h"

#include <microhttpd.h>

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* the "request entity too large" status code was spelled differently across
 * libmicrohttpd releases (and the older spellings are now deprecated) */
#if defined(MHD_HTTP_CONTENT_TOO_LARGE)
#define HTTP_STATUS_TOO_LARGE MHD_HTTP_CONTENT_TOO_LARGE
#else
#define HTTP_STATUS_TOO_LARGE 413
#endif

typedef struct _PathRegistration
{
  HTTPServerRequestHandler handler;
  HTTPServerRequestCompletion completion;
  gpointer user_data;
  gsize max_request_size;
} PathRegistration;

struct HTTPServerListener
{
  gchar *key;
  gchar *bind_addr;
  gint port;
  gint user_count;

  struct MHD_Daemon *daemon;
  struct sockaddr_storage bind_sa;
  gboolean has_bind_sa;

  GMutex lock;
  GHashTable *paths;            /* path (owned gchar *) -> PathRegistration * */

  MainLoopThreadedWorker thread;
  gboolean thread_started;
  GAtomicCounter stop_requested;
  gint wakeup_pipe[2];
};

typedef struct _RequestContext
{
  GString *body;
  gsize offset;
  gchar *path;
  GSockAddr *peer;
  gboolean too_large;
} RequestContext;

static const char *RESPONSE_OK = "";
static const char *RESPONSE_NOT_FOUND = "no source registered for this path\n";
static const char *RESPONSE_METHOD_NOT_ALLOWED = "only POST is supported\n";
static const char *RESPONSE_TOO_LARGE = "request body too large\n";

static enum MHD_Result
_send_response(struct MHD_Connection *connection, unsigned int status, const char *text)
{
  struct MHD_Response *response =
    MHD_create_response_from_buffer(strlen(text), (void *) text, MHD_RESPMEM_PERSISTENT);
  if (!response)
    return MHD_NO;

  enum MHD_Result ret = MHD_queue_response(connection, status, response);
  MHD_destroy_response(response);
  return ret;
}

static GSockAddr *
_extract_peer_addr(struct MHD_Connection *connection)
{
  const union MHD_ConnectionInfo *info =
    MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);

  if (!info || !info->client_addr)
    return NULL;

  struct sockaddr *sa = info->client_addr;
  socklen_t salen = (sa->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
  return g_sockaddr_new(sa, salen);
}

/* copy the registration for a path into *out (so it stays valid without
 * holding the lock); returns FALSE if no registration exists for the path */
static gboolean
_lookup_registration(HTTPServerListener *self, const gchar *path, PathRegistration *out)
{
  g_mutex_lock(&self->lock);
  PathRegistration *reg = g_hash_table_lookup(self->paths, path);
  if (reg)
    *out = *reg;
  g_mutex_unlock(&self->lock);
  return reg != NULL;
}

static enum MHD_Result
_handle_request(void *cls, struct MHD_Connection *connection,
                const char *url, const char *method, const char *version,
                const char *upload_data, size_t *upload_data_size, void **con_cls)
{
  HTTPServerListener *self = (HTTPServerListener *) cls;
  RequestContext *ctx = (RequestContext *) *con_cls;

  PathRegistration reg;

  if (!ctx)
    {
      if (strcmp(method, MHD_HTTP_METHOD_POST) != 0)
        return _send_response(connection, MHD_HTTP_METHOD_NOT_ALLOWED, RESPONSE_METHOD_NOT_ALLOWED);

      if (!_lookup_registration(self, url, &reg))
        return _send_response(connection, MHD_HTTP_NOT_FOUND, RESPONSE_NOT_FOUND);

      ctx = g_new0(RequestContext, 1);
      ctx->body = g_string_sized_new(1024);
      ctx->path = g_strdup(url);
      *con_cls = ctx;
      return MHD_YES;
    }

  if (*upload_data_size != 0)
    {
      if (_lookup_registration(self, ctx->path, &reg) &&
          reg.max_request_size && ctx->body->len + *upload_data_size > reg.max_request_size)
        ctx->too_large = TRUE;
      else
        g_string_append_len(ctx->body, upload_data, *upload_data_size);

      *upload_data_size = 0;
      return MHD_YES;
    }

  if (ctx->too_large)
    return _send_response(connection, HTTP_STATUS_TOO_LARGE, RESPONSE_TOO_LARGE);

  if (!_lookup_registration(self, ctx->path, &reg))
    return _send_response(connection, MHD_HTTP_NOT_FOUND, RESPONSE_NOT_FOUND);

  if (!ctx->peer)
    ctx->peer = _extract_peer_addr(connection);

  HTTPServerRequestResult result =
    reg.handler(reg.user_data, connection, ctx->body->str, ctx->body->len, &ctx->offset, ctx->peer);

  if (result == HTTP_SERVER_REQUEST_SUSPENDED)
    return MHD_YES;

  return _send_response(connection, MHD_HTTP_OK, RESPONSE_OK);
}

static void
_request_completed(void *cls, struct MHD_Connection *connection,
                   void **con_cls, enum MHD_RequestTerminationCode toe)
{
  HTTPServerListener *self = (HTTPServerListener *) cls;
  RequestContext *ctx = (RequestContext *) *con_cls;
  if (!ctx)
    return;

  PathRegistration reg;
  if (ctx->path && _lookup_registration(self, ctx->path, &reg))
    reg.completion(reg.user_data, connection);

  g_string_free(ctx->body, TRUE);
  g_free(ctx->path);
  g_sockaddr_unref(ctx->peer);
  g_free(ctx);
  *con_cls = NULL;
}

static void
_drive(HTTPServerListener *self)
{
  while (!g_atomic_counter_get(&self->stop_requested))
    {
      fd_set rs, ws, es;
      FD_ZERO(&rs);
      FD_ZERO(&ws);
      FD_ZERO(&es);

      int max_fd = -1;
      if (MHD_get_fdset(self->daemon, &rs, &ws, &es, &max_fd) != MHD_YES)
        {
          msg_error("http(): failed to query libmicrohttpd socket set, stopping listener",
                    evt_tag_int("port", self->port));
          break;
        }

      int wakeup_fd = self->wakeup_pipe[0];
      FD_SET(wakeup_fd, &rs);
      if (wakeup_fd > max_fd)
        max_fd = wakeup_fd;

      struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
      MHD_UNSIGNED_LONG_LONG mhd_timeout;
      if (MHD_get_timeout(self->daemon, &mhd_timeout) == MHD_YES)
        {
          tv.tv_sec = mhd_timeout / 1000;
          tv.tv_usec = (mhd_timeout % 1000) * 1000;
        }

      int rc = select(max_fd + 1, &rs, &ws, &es, &tv);
      if (rc < 0 && errno != EINTR)
        {
          msg_error("http(): select() failed on listener, stopping",
                    evt_tag_int("port", self->port), evt_tag_error("error"));
          break;
        }

      if (FD_ISSET(wakeup_fd, &rs))
        {
          gchar buf[64];
          while (read(wakeup_fd, buf, sizeof(buf)) > 0)
            ;
        }

      MHD_run(self->daemon);
    }
}

static void
_request_stop(HTTPServerListener *self)
{
  g_atomic_counter_set(&self->stop_requested, 1);
  http_server_listener_wakeup(self);
}

void
http_server_listener_wakeup(HTTPServerListener *self)
{
  gchar c = 'x';
  if (write(self->wakeup_pipe[1], &c, 1) < 0)
    ;
}

static void
_listener_thread_run(MainLoopThreadedWorker *t)
{
  _drive((HTTPServerListener *) t->data);
}

static void
_listener_thread_request_exit(MainLoopThreadedWorker *t)
{
  _request_stop((HTTPServerListener *) t->data);
}

gboolean
http_server_listener_start(HTTPServerListener *self)
{
  if (self->thread_started)
    return TRUE;

  self->thread_started = TRUE;
  return main_loop_threaded_worker_start(&self->thread);
}

gboolean
http_server_listener_register_path(HTTPServerListener *self, const gchar *path,
                                   HTTPServerRequestHandler handler,
                                   HTTPServerRequestCompletion completion,
                                   gpointer user_data, gsize max_request_size)
{
  g_mutex_lock(&self->lock);

  if (g_hash_table_contains(self->paths, path))
    msg_warning("http(): a handler is already registered for this path on the given address, "
                "the previous registration will be replaced",
                evt_tag_str("path", path), evt_tag_int("port", self->port));

  PathRegistration *reg = g_new0(PathRegistration, 1);
  reg->handler = handler;
  reg->completion = completion;
  reg->user_data = user_data;
  reg->max_request_size = max_request_size;
  g_hash_table_insert(self->paths, g_strdup(path), reg);

  g_mutex_unlock(&self->lock);
  return TRUE;
}

void
http_server_listener_unregister_path(HTTPServerListener *self, const gchar *path,
                                      gpointer user_data)
{
  g_mutex_lock(&self->lock);

  PathRegistration *reg = g_hash_table_lookup(self->paths, path);
  /* only remove the entry if it still belongs to us; during a reload a freshly
   * initialized registration may have already taken over this path */
  if (reg && reg->user_data == user_data)
    g_hash_table_remove(self->paths, path);

  g_mutex_unlock(&self->lock);
}

void
http_server_listener_suspend_connection(struct MHD_Connection *connection)
{
  MHD_suspend_connection(connection);
}

void
http_server_listener_resume_connection(struct MHD_Connection *connection)
{
  MHD_resume_connection(connection);
}

static gboolean
_resolve_bind_addr(HTTPServerListener *self, const gchar *bind_addr)
{
  if (!bind_addr || !bind_addr[0] ||
      strcmp(bind_addr, "0.0.0.0") == 0 || strcmp(bind_addr, "::") == 0)
    {
      self->has_bind_sa = FALSE;
      return TRUE;
    }

  memset(&self->bind_sa, 0, sizeof(self->bind_sa));

  struct sockaddr_in *sin = (struct sockaddr_in *) &self->bind_sa;
  if (inet_pton(AF_INET, bind_addr, &sin->sin_addr) == 1)
    {
      sin->sin_family = AF_INET;
      sin->sin_port = htons((guint16) self->port);
      self->has_bind_sa = TRUE;
      return TRUE;
    }

  struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) &self->bind_sa;
  if (inet_pton(AF_INET6, bind_addr, &sin6->sin6_addr) == 1)
    {
      sin6->sin6_family = AF_INET6;
      sin6->sin6_port = htons((guint16) self->port);
      self->has_bind_sa = TRUE;
      return TRUE;
    }

  msg_error("http(): could not parse bind address, expected a numeric IPv4 or IPv6 address",
            evt_tag_str("ip", bind_addr));
  return FALSE;
}

static struct MHD_Daemon *
_start_daemon_with_flags(HTTPServerListener *self, unsigned int flags)
{
  if (self->has_bind_sa)
    return MHD_start_daemon(flags, (guint16) self->port, NULL, NULL,
                            _handle_request, self,
                            MHD_OPTION_NOTIFY_COMPLETED, _request_completed, self,
                            MHD_OPTION_LISTENING_ADDRESS_REUSE, (unsigned int) 1,
                            MHD_OPTION_SOCK_ADDR, (struct sockaddr *) &self->bind_sa,
                            MHD_OPTION_END);

  return MHD_start_daemon(flags, (guint16) self->port, NULL, NULL,
                          _handle_request, self,
                          MHD_OPTION_NOTIFY_COMPLETED, _request_completed, self,
                          MHD_OPTION_LISTENING_ADDRESS_REUSE, (unsigned int) 1,
                          MHD_OPTION_END);
}

static gboolean
_start_daemon(HTTPServerListener *self)
{
  /* external-polling mode (we drive MHD_run from our own thread) with
   * suspend/resume enabled for per-connection flow control */
  unsigned int base_flags = MHD_ALLOW_SUSPEND_RESUME;

  if (!self->has_bind_sa)
    {
      self->daemon = _start_daemon_with_flags(self, base_flags | MHD_USE_DUAL_STACK);
      if (self->daemon)
        return TRUE;

      msg_debug("http(): could not open a dual-stack listener, falling back to IPv4",
                evt_tag_int("port", self->port));
    }

  self->daemon = _start_daemon_with_flags(self, base_flags);
  if (!self->daemon)
    {
      msg_error("http(): failed to start HTTP listener",
                evt_tag_str("ip", self->bind_addr), evt_tag_int("port", self->port));
      return FALSE;
    }

  return TRUE;
}

static gboolean
_listener_start(HTTPServerListener *self)
{
  if (pipe(self->wakeup_pipe) < 0)
    {
      msg_error("http(): failed to create wakeup pipe for listener", evt_tag_error("error"));
      goto error;
    }
  fcntl(self->wakeup_pipe[0], F_SETFL, O_NONBLOCK);
  fcntl(self->wakeup_pipe[1], F_SETFL, O_NONBLOCK);

  if (!_resolve_bind_addr(self, self->bind_addr))
    goto error;

  if (!_start_daemon(self))
    goto error;

  return TRUE;

error:
  if (self->wakeup_pipe[0] >= 0)
    close(self->wakeup_pipe[0]);
  if (self->wakeup_pipe[1] >= 0)
    close(self->wakeup_pipe[1]);
  self->wakeup_pipe[0] = -1;
  self->wakeup_pipe[1] = -1;
  return FALSE;
}

static void
_listener_stop(HTTPServerListener *self)
{
  if (self->thread_started)
    {
      _request_stop(self);
      main_loop_threaded_worker_clear(&self->thread);
    }

  if (self->daemon)
    MHD_stop_daemon(self->daemon);

  if (self->wakeup_pipe[0] >= 0)
    close(self->wakeup_pipe[0]);
  if (self->wakeup_pipe[1] >= 0)
    close(self->wakeup_pipe[1]);
  self->wakeup_pipe[0] = -1;
  self->wakeup_pipe[1] = -1;
}

static HTTPServerListener *
_listener_new(const gchar *key, const gchar *bind_addr, gint port)
{
  HTTPServerListener *self = g_new0(HTTPServerListener, 1);

  self->user_count = 1;
  self->key = g_strdup(key);
  self->bind_addr = g_strdup(bind_addr ? bind_addr : "");
  self->port = port;
  g_mutex_init(&self->lock);
  g_atomic_counter_set(&self->stop_requested, 0);
  self->wakeup_pipe[0] = -1;
  self->wakeup_pipe[1] = -1;
  self->paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  main_loop_threaded_worker_init(&self->thread, MLW_THREADED_INPUT_WORKER, self);
  self->thread.run = _listener_thread_run;
  self->thread.request_exit = _listener_thread_request_exit;
  return self;
}

static void
_listener_free(HTTPServerListener *self)
{
  g_assert(self->wakeup_pipe[0] == -1);
  g_hash_table_unref(self->paths);
  g_mutex_clear(&self->lock);
  g_free(self->key);
  g_free(self->bind_addr);
  g_free(self);
}

/* GDestroyNotify for the registry */
static void
_registry_destroy_listener(gpointer p)
{
  HTTPServerListener *self = (HTTPServerListener *) p;
  _listener_stop(self);
  _listener_free(self);
}

static GHashTable *
_get_registry(GlobalConfig *cfg)
{
  HTTPSourceConfig *hsc = http_source_config_get(cfg);
  if (!hsc->listeners)
    hsc->listeners = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, _registry_destroy_listener);
  return hsc->listeners;
}

HTTPServerListener *
http_server_listener_acquire(GlobalConfig *cfg, const gchar *bind_addr, gint port)
{
  GHashTable *registry = _get_registry(cfg);
  gchar *key = g_strdup_printf("%s:%d", bind_addr ? bind_addr : "", port);

  HTTPServerListener *listener = g_hash_table_lookup(registry, key);
  if (listener)
    {
      listener->user_count++;
      g_free(key);
      return listener;
    }

  listener = _listener_new(key, bind_addr, port);
  if (_listener_start(listener))
    g_hash_table_insert(registry, listener->key, listener);
  else
    {
      _listener_free(listener);
      listener = NULL;
    }

  g_free(key);
  return listener;
}

void
http_server_listener_release(GlobalConfig *cfg, HTTPServerListener *listener)
{
  GHashTable *registry = _get_registry(cfg);
  /* the last reference removes the listener from the registry, which destroys
   * it (stop + free) via the hash table's value-destroy function */
  if (--listener->user_count == 0)
    g_hash_table_remove(registry, listener->key);
}
