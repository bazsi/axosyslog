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

#include "httpsource-listener.h"
#include "messages.h"
#include "atomic.h"
#include "gsockaddr.h"
#include "logsource.h"
#include "logmsg/logmsg.h"

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
  LogThreadedSourceWorker *worker;
  gsize max_request_size;
} PathRegistration;

struct HTTPServerListener
{
  gchar *key;
  gchar *bind_addr;
  gint port;
  gint ref_count;

  struct MHD_Daemon *daemon;
  struct sockaddr_storage bind_sa;
  gboolean has_bind_sa;

  GMutex lock;
  GHashTable *paths;            /* path (owned gchar *) -> PathRegistration * */

  GAtomicCounter stop_requested;
  gint wakeup_pipe[2];
};

static GMutex registry_lock;
static GHashTable *registry;    /* key (owned gchar *) -> HTTPServerListener * */

typedef struct _RequestContext
{
  GString *body;
  gchar *path;
  gsize max_request_size;
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

static void
_post_line(LogThreadedSourceWorker *worker, const gchar *line, gsize length, GSockAddr *peer)
{
  LogMessage *msg = log_msg_new_empty();
  log_msg_set_value(msg, LM_V_MESSAGE, line, length);

  if (peer)
    log_msg_set_saddr_ref(msg, g_sockaddr_ref(peer));

  log_threaded_source_worker_blocking_post(worker, msg);
}

/* Split the body at newlines and post each non-empty line as a separate
 * message.  A trailing CR (CRLF line endings) is stripped. */
static void
_post_body_as_lines(LogThreadedSourceWorker *worker, GString *body, GSockAddr *peer)
{
  const gchar *p = body->str;
  const gchar *end = body->str + body->len;

  while (p < end)
    {
      const gchar *nl = memchr(p, '\n', end - p);
      const gchar *line_end = nl ? nl : end;
      gsize length = line_end - p;

      if (length > 0 && p[length - 1] == '\r')
        length--;

      if (length > 0)
        _post_line(worker, p, length, peer);

      if (!nl)
        break;
      p = nl + 1;
    }
}

static enum MHD_Result
_process_request(HTTPServerListener *self, struct MHD_Connection *connection, RequestContext *ctx)
{
  if (ctx->too_large)
    return _send_response(connection, HTTP_STATUS_TOO_LARGE, RESPONSE_TOO_LARGE);

  /* By the time we process a request the worker threads are running and no
   * (un)registration happens, so holding the lock across posting is safe:
   * the owner thread is the only dispatcher and (un)registration only runs
   * before workers start / after they exit. */
  g_mutex_lock(&self->lock);
  PathRegistration *reg = g_hash_table_lookup(self->paths, ctx->path);
  if (!reg)
    {
      g_mutex_unlock(&self->lock);
      return _send_response(connection, MHD_HTTP_NOT_FOUND, RESPONSE_NOT_FOUND);
    }

  GSockAddr *peer = _extract_peer_addr(connection);
  _post_body_as_lines(reg->worker, ctx->body, peer);
  g_sockaddr_unref(peer);
  g_mutex_unlock(&self->lock);

  return _send_response(connection, MHD_HTTP_OK, RESPONSE_OK);
}

static enum MHD_Result
_handle_request(void *cls, struct MHD_Connection *connection,
                const char *url, const char *method, const char *version,
                const char *upload_data, size_t *upload_data_size, void **con_cls)
{
  HTTPServerListener *self = (HTTPServerListener *) cls;
  RequestContext *ctx = (RequestContext *) *con_cls;

  if (!ctx)
    {
      if (strcmp(method, MHD_HTTP_METHOD_POST) != 0)
        return _send_response(connection, MHD_HTTP_METHOD_NOT_ALLOWED, RESPONSE_METHOD_NOT_ALLOWED);

      g_mutex_lock(&self->lock);
      PathRegistration *reg = g_hash_table_lookup(self->paths, url);
      gsize max_request_size = reg ? reg->max_request_size : 0;
      g_mutex_unlock(&self->lock);

      if (!reg)
        return _send_response(connection, MHD_HTTP_NOT_FOUND, RESPONSE_NOT_FOUND);

      ctx = g_new0(RequestContext, 1);
      ctx->body = g_string_sized_new(1024);
      ctx->path = g_strdup(url);
      ctx->max_request_size = max_request_size;
      *con_cls = ctx;
      return MHD_YES;
    }

  if (*upload_data_size != 0)
    {
      if (ctx->max_request_size && ctx->body->len + *upload_data_size > ctx->max_request_size)
        ctx->too_large = TRUE;
      else
        g_string_append_len(ctx->body, upload_data, *upload_data_size);

      *upload_data_size = 0;
      return MHD_YES;
    }

  return _process_request(self, connection, ctx);
}

static void
_request_completed(void *cls, struct MHD_Connection *connection,
                   void **con_cls, enum MHD_RequestTerminationCode toe)
{
  RequestContext *ctx = (RequestContext *) *con_cls;
  if (!ctx)
    return;

  g_string_free(ctx->body, TRUE);
  g_free(ctx->path);
  g_free(ctx);
  *con_cls = NULL;
}

void
http_server_listener_drive(HTTPServerListener *self)
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

void
http_server_listener_request_stop(HTTPServerListener *self)
{
  g_atomic_counter_set(&self->stop_requested, 1);

  gchar c = 'x';
  if (write(self->wakeup_pipe[1], &c, 1) < 0)
    ;
}

gboolean
http_server_listener_register_path(HTTPServerListener *self, const gchar *path,
                                   LogThreadedSourceWorker *worker, gsize max_request_size)
{
  g_mutex_lock(&self->lock);

  if (g_hash_table_contains(self->paths, path))
    msg_warning("http(): a source is already registered for this path on the given address, "
                "the previous registration will be replaced",
                evt_tag_str("path", path), evt_tag_int("port", self->port));

  PathRegistration *reg = g_new0(PathRegistration, 1);
  reg->worker = worker;
  reg->max_request_size = max_request_size;
  g_hash_table_insert(self->paths, g_strdup(path), reg);

  g_mutex_unlock(&self->lock);
  return TRUE;
}

void
http_server_listener_unregister_path(HTTPServerListener *self, const gchar *path,
                                      LogThreadedSourceWorker *worker)
{
  g_mutex_lock(&self->lock);

  PathRegistration *reg = g_hash_table_lookup(self->paths, path);
  /* only remove the entry if it still points to our worker; during a reload
   * a freshly initialized source may have already taken over this path */
  if (reg && reg->worker == worker)
    g_hash_table_remove(self->paths, path);

  g_mutex_unlock(&self->lock);
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
  /* when listening on all interfaces, prefer a dual-stack (IPv4 + IPv6)
   * socket, but fall back to IPv4-only on hosts without IPv6 support */
  if (!self->has_bind_sa)
    {
      self->daemon = _start_daemon_with_flags(self, MHD_USE_DUAL_STACK);
      if (self->daemon)
        return TRUE;

      msg_debug("http(): could not open a dual-stack listener, falling back to IPv4",
                evt_tag_int("port", self->port));
    }

  self->daemon = _start_daemon_with_flags(self, MHD_NO_FLAG);
  if (!self->daemon)
    {
      msg_error("http(): failed to start HTTP listener",
                evt_tag_str("ip", self->bind_addr), evt_tag_int("port", self->port));
      return FALSE;
    }

  return TRUE;
}

static HTTPServerListener *
_listener_new(const gchar *key, const gchar *bind_addr, gint port)
{
  HTTPServerListener *self = g_new0(HTTPServerListener, 1);
  self->key = g_strdup(key);
  self->bind_addr = g_strdup(bind_addr ? bind_addr : "");
  self->port = port;
  self->ref_count = 1;
  g_mutex_init(&self->lock);
  g_atomic_counter_set(&self->stop_requested, 0);
  self->wakeup_pipe[0] = -1;
  self->wakeup_pipe[1] = -1;
  self->paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  if (pipe(self->wakeup_pipe) < 0)
    {
      msg_error("http(): failed to create wakeup pipe for listener", evt_tag_error("error"));
      goto error;
    }
  fcntl(self->wakeup_pipe[0], F_SETFL, O_NONBLOCK);
  fcntl(self->wakeup_pipe[1], F_SETFL, O_NONBLOCK);

  if (!_resolve_bind_addr(self, bind_addr))
    goto error;

  if (!_start_daemon(self))
    goto error;

  return self;

error:
  if (self->wakeup_pipe[0] >= 0)
    close(self->wakeup_pipe[0]);
  if (self->wakeup_pipe[1] >= 0)
    close(self->wakeup_pipe[1]);
  g_hash_table_unref(self->paths);
  g_mutex_clear(&self->lock);
  g_free(self->key);
  g_free(self->bind_addr);
  g_free(self);
  return NULL;
}

static void
_listener_free(HTTPServerListener *self)
{
  if (self->daemon)
    MHD_stop_daemon(self->daemon);

  if (self->wakeup_pipe[0] >= 0)
    close(self->wakeup_pipe[0]);
  if (self->wakeup_pipe[1] >= 0)
    close(self->wakeup_pipe[1]);

  g_hash_table_unref(self->paths);
  g_mutex_clear(&self->lock);
  g_free(self->key);
  g_free(self->bind_addr);
  g_free(self);
}

HTTPServerListener *
http_server_listener_acquire(GlobalConfig *cfg, const gchar *bind_addr, gint port, gboolean *created)
{
  *created = FALSE;

  gchar *key = g_strdup_printf("%p/%s:%d", (void *) cfg, bind_addr ? bind_addr : "", port);

  g_mutex_lock(&registry_lock);
  if (!registry)
    registry = g_hash_table_new(g_str_hash, g_str_equal);

  HTTPServerListener *self = g_hash_table_lookup(registry, key);
  if (self)
    {
      self->ref_count++;
      g_mutex_unlock(&registry_lock);
      g_free(key);
      return self;
    }

  self = _listener_new(key, bind_addr, port);
  if (self)
    {
      g_hash_table_insert(registry, self->key, self);
      *created = TRUE;
    }

  g_mutex_unlock(&registry_lock);
  g_free(key);
  return self;
}

void
http_server_listener_release(HTTPServerListener *self)
{
  if (!self)
    return;

  g_mutex_lock(&registry_lock);
  if (--self->ref_count == 0)
    {
      g_hash_table_remove(registry, self->key);
      g_mutex_unlock(&registry_lock);
      _listener_free(self);
      return;
    }
  g_mutex_unlock(&registry_lock);
}
