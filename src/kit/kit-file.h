/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/***************************************************************************
 *
 * kit-file.h : File utilities
 *
 * Copyright (C) 2007 David Zeuthen, <david@fubar.dk>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#if !defined (KIT_COMPILATION) && !defined(_KIT_INSIDE_KIT_H)
#error "Only <kit/kit.h> can be included directly, this file may disappear or change contents."
#endif

#ifndef KIT_FILE_H
#define KIT_FILE_H

#include <sys/stat.h>
#include <kit/kit.h>
#ifdef HAVE_SOLARIS
#include <sys/types.h>
#endif

KIT_BEGIN_DECLS

kit_bool_t kit_file_get_contents (const char *path, char **out_contents, size_t *out_contents_size);
kit_bool_t kit_file_set_contents (const char *path, mode_t mode, const char *contents, size_t contents_size);

size_t _kit_get_num_fd (void);

KIT_END_DECLS

#endif /* KIT_FILE_H */


