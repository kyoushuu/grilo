/*
 * Copyright (C) 2011 Igalia S.L.
 *
 * Contact: Iago Toral Quiroga <itoral@igalia.com>
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

#if !defined (_GRILO_H_INSIDE_) && !defined (GRILO_COMPILATION)
#error "Only <grilo.h> can be included directly."
#endif

#include <grl-caps.h>

#if !defined (_GRL_OPERATION_OPTIONS_H_)
#define _GRL_OPERATION_OPTIONS_H_

G_BEGIN_DECLS

typedef struct _GrlOperationOptionsPrivate GrlOperationOptionsPrivate;

typedef struct {
  GObject parent;

  /*< private >*/
  GrlOperationOptionsPrivate *priv;

  gpointer _grl_reserved[GRL_PADDING_SMALL];
} GrlOperationOptions;

typedef struct {
  GObjectClass parent;

  /*< private >*/
  gpointer _grl_reserved[GRL_PADDING];
} GrlOperationOptionsClass;

#define GRL_OPERATION_OPTIONS_TYPE (grl_operation_options_get_type ())
#define GRL_OPERATION_OPTIONS(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GRL_OPERATION_OPTIONS_TYPE, GrlOperationOptions))
#define GRL_OPERATION_OPTIONS_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), GRL_OPERATION_OPTIONS_TYPE, GrlOperationOptionsClass))
#define IS_GRL_OPERATION_OPTIONS(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GRL_OPERATION_OPTIONS_TYPE))
#define IS_GRL_OPERATION_OPTIONS_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GRL_OPERATION_OPTIONS_TYPE))
#define GRL_OPERATION_OPTIONS_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), GRL_OPERATION_OPTIONS_TYPE, GrlOperationOptionsClass))

#define GRL_COUNT_INFINITY (-1)

GType grl_operation_options_get_type (void);

GrlOperationOptions *grl_operation_options_new (GrlCaps *caps);

gboolean grl_operation_options_obey_caps (GrlOperationOptions *options,
                                          GrlCaps *caps,
                                          GrlOperationOptions **supported_options,
                                          GrlOperationOptions **unsupported_options);

GrlOperationOptions *grl_operation_options_copy (GrlOperationOptions *options);

gboolean grl_operation_options_set_skip (GrlOperationOptions *options, guint skip);
guint grl_operation_options_get_skip (GrlOperationOptions *options);

gboolean grl_operation_options_set_count (GrlOperationOptions *options, gint count);
gint grl_operation_options_get_count (GrlOperationOptions *options);

gboolean grl_operation_options_set_flags (GrlOperationOptions *options,
                                      GrlMetadataResolutionFlags flags);
GrlMetadataResolutionFlags
    grl_operation_options_get_flags (GrlOperationOptions *options);

G_END_DECLS

#endif /* _GRL_OPERATION_OPTIONS_H_ */