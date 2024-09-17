/*
 * Copyright (c) 2002-2018 Balabit
 * Copyright (c) 2018 Laszlo Budai <laszlo.budai@balabit.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#ifndef TRANSPORT_STACK_H_INCLUDED
#define TRANSPORT_STACK_H_INCLUDED

#include "transport/logtransport.h"

typedef struct _LogTransportStack LogTransportStack;
typedef struct _LogTransportFactory LogTransportFactory;

typedef enum
{
  /* this is a special index for simple cases where we only use a single
   * LogTransport which never changes */
  LOG_TRANSPORT_INITIAL,
  LOG_TRANSPORT_SOCKET,
  LOG_TRANSPORT_TLS,
  LOG_TRANSPORT_HAPROXY,
  LOG_TRANSPORT_GZIP,
  LOG_TRANSPORT_NONE,
  LOG_TRANSPORT__MAX = LOG_TRANSPORT_NONE,
} LogTransportIndex;

struct _LogTransportFactory
{
  gint index;
  LogTransport *(*construct_transport)(const LogTransportFactory *self, LogTransportStack *stack);
  void (*free_fn)(LogTransportFactory *self);
};

static inline LogTransport *
log_transport_factory_construct_transport(const LogTransportFactory *self, LogTransportStack *stack)
{
  g_assert(self->construct_transport);

  LogTransport *transport = self->construct_transport(self, stack);
//  transport->name = self->id;

  return transport;
}

static inline void
log_transport_factory_free(LogTransportFactory *self)
{
  if (self->free_fn)
    self->free_fn(self);
  g_free(self);
}

void log_transport_factory_init_instance(LogTransportFactory *self, LogTransportIndex index);


struct _LogTransportStack
{
  gint active_transport;
  gint fd;
  LogTransport *transports[LOG_TRANSPORT__MAX];
  LogTransportFactory *transport_factories[LOG_TRANSPORT__MAX];
};

static inline LogTransport *
log_transport_stack_get_transport(LogTransportStack *self, gint active)
{
  if (self->transports[active])
    return self->transports[active];

  if (self->transport_factories[active])
    {
      self->transports[active] = log_transport_factory_construct_transport(self->transport_factories[active], self);
      return self->transports[active];
    }
  return NULL;
}

static inline LogTransport *
log_transport_stack_get_active(LogTransportStack *self)
{
  return log_transport_stack_get_transport(self, self->active_transport);
}

void log_transport_stack_add_factory(LogTransportStack *self, LogTransportFactory *);
void log_transport_stack_add_transport(LogTransportStack *self, gint index, LogTransport *);
gboolean log_transport_stack_switch(LogTransportStack *self, gint index);

void log_transport_stack_init(LogTransportStack *self, LogTransport *initial_transport);
void log_transport_stack_deinit(LogTransportStack *self);

#endif
