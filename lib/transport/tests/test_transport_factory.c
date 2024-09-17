/*
 * Copyright (c) 2002-2018 Balabit
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

#include <criterion/criterion.h>

#include "transport/transport-stack.h"
#include "apphook.h"

typedef struct _FakeTransport FakeTransport;
typedef struct _FakeTransportFactory FakeTransportFactory;

struct _FakeTransport
{
  LogTransport super;
  gboolean constructed;
};

static gssize
_fake_read(LogTransport *s, gpointer buf, gsize count, LogTransportAuxData *auc)
{
  return count;
}

static gssize
_fake_write(LogTransport *s, const gpointer buf, gsize count)
{
  return count;
}

static void
_fake_free(LogTransport *s)
{
}

LogTransport *
_fake_transport_new(void)
{
  FakeTransport *instance = g_new0(FakeTransport, 1);

  log_transport_init_instance(&instance->super, "fake", -1);
  instance->super.free_fn = _fake_free;
  instance->super.read = _fake_read;
  instance->super.write = _fake_write;
  instance->constructed = TRUE;

  return &instance->super;
}

struct _FakeTransportFactory
{
  LogTransportFactory super;
};

static LogTransport *
_transport_factory_construct(const LogTransportFactory *s, LogTransportStack *stack)
{
  return _fake_transport_new();
}

LogTransportFactory *
_fake_transport_factory_new(void)
{
  FakeTransportFactory *instance = g_new0(FakeTransportFactory, 1);
  instance->super.construct_transport = _transport_factory_construct;
  return &instance->super;
}

TestSuite(transport_factory, .init = app_startup, .fini = app_shutdown);

Test(transport_factory, fake_transport_factory)
{
  LogTransportFactory *fake_factory = _fake_transport_factory_new();
  LogTransportStack stack;

  log_transport_stack_init(&stack);

  gint fd = 11;
  FakeTransport *fake_transport = (FakeTransport *) log_transport_factory_construct_transport(fake_factory, &stack);
  cr_expect_eq(fake_transport->constructed, TRUE);
  cr_expect_eq(fake_transport->super.read, _fake_read);
  cr_expect_eq(fake_transport->super.write, _fake_write);
  log_transport_free(&fake_transport->super);

  log_transport_factory_free(fake_factory);
  log_transport_stack_deinit(&stack);
}
