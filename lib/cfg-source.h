/*
 * Copyright (c) 2024 Balazs Scheidler <balazs.scheidler@axoflow.com>
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

#ifndef CFG_SOURCE_H_INCLUDED
#define CFG_SOURCE_H_INCLUDED

#include "cfg-lexer.h"

gboolean cfg_source_print_source_text(const gchar *filename, gint line, gint column, gint offset);

/* These functions are only available during parsing */
gboolean cfg_source_print_source_context(CfgLexer *lexer, CfgIncludeLevel *level, const CFG_LTYPE *yylloc);
gboolean cfg_source_extract_source_text(CfgLexer *lexer, const CFG_LTYPE *yylloc, GString *result);

#endif
