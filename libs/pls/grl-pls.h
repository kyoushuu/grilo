/*
 * Copyright (C) 2013 Collabora Ltd.
 *
 * Authors: Mateu Batle Sastre <mateu.batle@collabora.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifndef _GRL_PLS_H_
#define _GRL_PLS_H_

#include <grilo.h>

gboolean grl_pls_mime_is_playlist (const gchar *mime);

gboolean grl_pls_file_is_playlist (const gchar *filename);

gboolean grl_pls_media_is_playlist (GrlMedia *media);

guint grl_pls_browse (GrlSource *source,
                      GrlMedia *container,
                      const GList *keys,
                      GrlOperationOptions *options,
                      GrlSourceResultCb callback,
                      gpointer user_data);

GList *grl_pls_browse_sync (GrlSource *source,
                            GrlMedia *container,
                            const GList *keys,
                            GrlOperationOptions *options,
                            GError **error);

#endif /* _GRL_PLS_H_ */
