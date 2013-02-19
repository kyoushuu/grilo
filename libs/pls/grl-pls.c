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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "grl-pls.h"
#include "../../src/grl-sync-priv.h"

#include <gio/gio.h>
#include <grilo.h>
#include <string.h>
#include <stdlib.h>
#include <totem-pl-parser.h>

/* --------- Logging  -------- */

#define GRL_LOG_DOMAIN_DEFAULT libpls_log_domain
GRL_LOG_DOMAIN_STATIC(libpls_log_domain);

/* -------- File info ------- */

#define FILE_ATTRIBUTES                         \
  G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","    \
  G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","    \
  G_FILE_ATTRIBUTE_STANDARD_TYPE ","            \
  G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","       \
  G_FILE_ATTRIBUTE_TIME_MODIFIED ","            \
  G_FILE_ATTRIBUTE_THUMBNAIL_PATH ","           \
  G_FILE_ATTRIBUTE_THUMBNAILING_FAILED

struct _GrlPlsEntry {
  gboolean is_valid;
  gchar *uri;
  gchar *title;
  gchar *author;
  gchar *album;
  gchar *mime;
  gchar *thumbnail;
  gint64 duration;
};

struct _GrlPlsPrivate {
  GrlSource *source;
  GrlMedia *playlist;
  GList *keys;
  GrlOperationOptions *options;
  GrlSourceResultCb callback;
  gpointer userdata;
  guint operation_id;
  GArray *entries;
};

static gboolean
mime_is_video (const gchar *mime)
{
  return g_str_has_prefix (mime, "video/");
}

static gboolean
mime_is_audio (const gchar *mime)
{
  return g_str_has_prefix (mime, "audio/");
}

static gboolean
mime_is_image (const gchar *mime)
{
  return g_str_has_prefix (mime, "image/");
}

static gboolean
mime_is_media (const gchar *mime, GrlTypeFilter filter)
{
  if (!mime)
    return FALSE;
  if (!strcmp (mime, "inode/directory"))
    return TRUE;
  if (filter & GRL_TYPE_FILTER_AUDIO && mime_is_audio (mime))
    return TRUE;
  if (filter & GRL_TYPE_FILTER_VIDEO && mime_is_video (mime))
    return TRUE;
  if (filter & GRL_TYPE_FILTER_IMAGE && mime_is_image (mime))
    return TRUE;
  return FALSE;
}

gboolean
grl_pls_mime_is_playlist (const gchar *mime)
{
  return  g_str_has_prefix (mime, "audio/x-ms-asx") ||
          //g_str_has_prefix (mime, "audio/x-ms-wax") ||
          //g_str_has_prefix (mime, "video/x-ms-wvx") ||
          g_str_has_prefix (mime, "audio/mpegurl") ||
          g_str_has_prefix (mime, "audio/x-mpegurl") ||
          g_str_has_prefix (mime, "audio/x-scpls");
}

static void
grl_pls_playlist_entry_parsed_cb (TotemPlParser *parser,
                                  const gchar *uri,
                                  GHashTable *metadata,
                                  gpointer user_data)
{
  struct _GrlPlsPrivate *priv = (struct _GrlPlsPrivate *) user_data;

  struct _GrlPlsEntry entry;
  entry.uri = g_strdup (uri);
  entry.title = g_strdup (g_hash_table_lookup (metadata, TOTEM_PL_PARSER_FIELD_TITLE));
  entry.author = g_strdup (g_hash_table_lookup (metadata, TOTEM_PL_PARSER_FIELD_AUTHOR));
  entry.album = g_strdup (g_hash_table_lookup (metadata, TOTEM_PL_PARSER_FIELD_ALBUM));
  entry.thumbnail = g_strdup (g_hash_table_lookup (metadata, TOTEM_PL_PARSER_FIELD_IMAGE_URI));
  entry.mime = g_strdup (g_hash_table_lookup (metadata, TOTEM_PL_PARSER_FIELD_CONTENT_TYPE));
  entry.duration = totem_pl_parser_parse_duration(g_hash_table_lookup (metadata, TOTEM_PL_PARSER_FIELD_DURATION), FALSE);

  g_array_append_val(priv->entries, entry);
}

static GrlMedia*
grl_media_from_pls_entry(struct _GrlPlsEntry *entry)
{
  GrlMedia *media = NULL;
  gchar *str;
  gchar *extension;
  const gchar *mime;
  GError *error = NULL;

  GFile *file = g_file_new_for_path (entry->uri);
  GFileInfo *info = g_file_query_info (file,
                                       FILE_ATTRIBUTES,
                                       0,
                                       NULL,
                                       &error);

  if (error) {
    return 0;
  }

  mime = g_file_info_get_content_type (info);
  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
    media = GRL_MEDIA (grl_media_box_new ());
  } else if (mime_is_video (mime)) {
    media = grl_media_video_new ();
  } else if (mime_is_audio (mime)) {
    media = grl_media_audio_new ();
  } else if (mime_is_image (mime)) {
    media = grl_media_image_new ();
  } else {
    media = grl_media_new ();
  }

  if (!GRL_IS_MEDIA_BOX (media)) {
    grl_media_set_mime (media, mime);
  }

  /* Title */
  str = g_strdup (g_file_info_get_display_name (info));

  /* Remove file extension */
  extension = g_strrstr (str, ".");
  if (extension) {
    *extension = '\0';
  }

  grl_media_set_title (media, str);
  g_free (str);

  /* Date */
  GTimeVal time;
  GDateTime *date_time;
  g_file_info_get_modification_time (info, &time);
  date_time = g_date_time_new_from_timeval_utc (&time);
  grl_media_set_modification_date (media, date_time);
  g_date_time_unref (date_time);

  /* Thumbnail */
  gboolean thumb_failed = g_file_info_get_attribute_boolean (info,
    G_FILE_ATTRIBUTE_THUMBNAILING_FAILED);
  if (!thumb_failed) {
    const gchar *thumb = g_file_info_get_attribute_byte_string (info,
      G_FILE_ATTRIBUTE_THUMBNAIL_PATH);
    if (thumb) {
      gchar *thumb_uri = g_filename_to_uri (thumb, NULL, NULL);
      if (thumb_uri) {
        grl_media_set_thumbnail (media, thumb_uri);
        g_free (thumb_uri);
      }
    }
  }

  g_object_unref (info);

  /* URL */
  str = g_file_get_uri (file);
  grl_media_set_url (media, str);
  g_free (str);

  /* Childcount */
  if (GRL_IS_MEDIA_BOX (media)) {
    //set_container_childcount (path, media, only_fast, options);
  }

  g_object_unref (file);

  return media;
}

static void
grl_pls_playlist_parse_cb (GObject *object,
                           GAsyncResult *result,
                           gpointer user_data)
{
  TotemPlParser *parser = (TotemPlParser *) object;
  TotemPlParserResult retval;
  struct _GrlPlsPrivate *priv = (struct _GrlPlsPrivate *) user_data;
  int remaining;
  int i;

  GError *error = NULL;
  retval = totem_pl_parser_parse_finish (parser, result, &error);
  if (retval != TOTEM_PL_PARSER_RESULT_SUCCESS) {
    g_error ("Playlist parsing failed: %s", error->message);
    g_error_free (error);
  }

  remaining = priv->entries->len;
  for (i = 0;i < priv->entries->len;i++) {
    struct _GrlPlsEntry *entry;
    entry = &g_array_index (priv->entries, struct _GrlPlsEntry, i);
    remaining--;
    if (entry) {
      GrlMedia *content = grl_media_from_pls_entry(entry);
      priv->callback (priv->source,
               priv->operation_id,
               content,
               remaining,
               priv->userdata,
               NULL);
    }
  }
}

static gboolean
check_options (GrlSource *source,
               GrlSupportedOps operation,
               GrlOperationOptions *options)
{
  GrlCaps *caps;

  /* FIXME: that check should be in somewhere in GrlOperationOptions */
  if (grl_operation_options_get_count (options) == 0)
    return FALSE;

  /* Check only if the source supports the operation */
  if (grl_source_supported_operations (source) & operation) {
    caps = grl_source_get_caps (source, operation);

    return grl_operation_options_obey_caps (options, caps, NULL, NULL);
  } else {
    return TRUE;
  }
}

// TODO: list of desired keys, use grl_source_resolve ...
// TODO: support GrlOperationOptions
// TODO: called from grl_filesystem_source_browse (GrlSource *source, GrlSourceBrowseSpec *bs)
/**
 * grl_pls_browse:
 * @source: a source
 * @container: a playlist
 * @keys: (element-type GrlKeyID): the #GList of
 * #GrlKeyID<!-- -->s to request
 * @options: options wanted for that operation
 * @callback: (scope notified): the user defined callback
 * @user_data: the user data to pass in the callback
 *
 * Browse from media elements through an available list.
 *
 * This method is asynchronous.
 *
 * Returns: the operation identifier
 *
 * Since: 0.2.6
 */
guint
grl_pls_browse (GrlSource *source,
                GrlMedia *playlist,
                const GList *keys,
                GrlOperationOptions *options,
                GrlSourceResultCb callback,
                gpointer userdata)
{
  // TODO: check input parameters
  TotemPlParser *parser;
  const char *playlist_url;
  struct _GrlPlsPrivate *priv;

  g_return_val_if_fail (GRL_IS_SOURCE (source), 0);
  g_return_val_if_fail (GRL_IS_OPERATION_OPTIONS (options), 0);
  g_return_val_if_fail (callback != NULL, 0);
  g_return_val_if_fail (grl_source_supported_operations (source) &
                        GRL_OP_BROWSE, 0);
  g_return_val_if_fail (check_options (source, GRL_OP_BROWSE, options), 0);

  parser = totem_pl_parser_new ();
  if (!parser) {
    return 0;
  }

  g_object_set (parser, "recurse", FALSE, "disable-unsafe", TRUE, NULL);
  g_signal_connect (G_OBJECT (parser), "entry-parsed", G_CALLBACK (grl_pls_playlist_entry_parsed_cb), NULL);

  playlist_url = grl_media_get_url (playlist);
  if (!playlist_url) {
    return 0;
  }

  priv = g_new(struct _GrlPlsPrivate, 1);
  if (!priv) {
    return 0;
  }
  // TODO: what to copy here ?
  priv->source = source;
  priv->playlist = playlist;
  priv->keys = keys;
  priv->options = options;
  priv->callback = callback;
  priv->userdata = userdata;
  priv->operation_id = 0;
  priv->entries = g_array_new(FALSE, FALSE, sizeof(struct _GrlPlsEntry));

  // TODO: use GCancellable ?
  totem_pl_parser_parse_async (parser, playlist_url, FALSE, NULL, grl_pls_playlist_parse_cb, priv);

  g_object_unref (parser);

  // TODO: return proper operation identifier
  return 1;
}

static void
multiple_result_async_cb (GrlSource *source,
                          guint op_id,
                          GrlMedia *media,
                          guint remaining,
                          gpointer user_data,
                          const GError *error)
{
  GrlDataSync *ds = (GrlDataSync *) user_data;

  GRL_DEBUG (__FUNCTION__);

  if (error) {
    ds->error = g_error_copy (error);

    /* Free previous results */
    g_list_foreach (ds->data, (GFunc) g_object_unref, NULL);
    g_list_free (ds->data);

    ds->data = NULL;
    ds->complete = TRUE;
    return;
  }

  if (media) {
    ds->data = g_list_prepend (ds->data, media);
  }

  if (remaining == 0) {
    ds->data = g_list_reverse (ds->data);
    ds->complete = TRUE;
  }
}

/**
 * grl_pls_browse_sync:
 * @source: a source
 * @container: a playlist
 * @keys: (element-type GrlKeyID): the #GList of
 * #GrlKeyID<!-- -->s to request
 * @options: options wanted for that operation
 * @error: a #GError, or @NULL
 *
 * Browse playlist entries through an available playlist.
 *
 * This method is synchronous.
 *
 * Returns: (element-type Grl.Media) (transfer full): a #GList with #GrlMedia
 * elements. After use g_object_unref() every element and g_list_free() the
 * list.
 *
 * Since: 0.2.6
 */
GList *
grl_pls_browse_sync (GrlSource *source,
                     GrlMedia *container,
                     const GList *keys,
                     GrlOperationOptions *options,
                     GError **error)
{
  GrlDataSync *ds;
  GList *result;

  ds = g_slice_new0 (GrlDataSync);

  if (grl_pls_browse (source,
                      container,
                      keys,
                      options,
                      multiple_result_async_cb,
                      ds))
    grl_wait_for_async_operation_complete (ds);

  if (ds->error) {
    if (error) {
      *error = ds->error;
    } else {
      g_error_free (ds->error);
    }
  }

  result = (GList *) ds->data;
  g_slice_free (GrlDataSync, ds);

  return result;
}

#if 0
static void
produce_from_path (GrlSourceBrowseSpec *bs, const gchar *path, GrlOperationOptions *options)
{
  GDir *dir;
  GError *error = NULL;
  const gchar *entry;
  guint skip, count;
  GList *entries = NULL;
  GList *iter;

  /* Open directory */
  GRL_DEBUG ("Opening directory '%s'", path);
  dir = g_dir_open (path, 0, &error);
  if (error) {
    GRL_DEBUG ("Failed to open directory '%s': %s", path, error->message);
    bs->callback (bs->source, bs->operation_id, NULL, 0, bs->user_data, error);
    g_error_free (error);
    return;
  }

  /* Filter out media and directories */
  while ((entry = g_dir_read_name (dir)) != NULL) {
    gchar *file;
    if (strcmp (path, G_DIR_SEPARATOR_S)) {
      file = g_strconcat (path, G_DIR_SEPARATOR_S, entry, NULL);
    } else {
      file = g_strconcat (path, entry, NULL);
    }
    if (file_is_valid_content (file, FALSE, options)) {
      entries = g_list_prepend (entries, file);
    }
  }

  /* Apply skip and count */
  skip = grl_operation_options_get_skip (bs->options);
  count = grl_operation_options_get_count (bs->options);
  iter = entries;
  while (iter) {
    gboolean remove;
    GList *tmp;
    if (skip > 0)  {
      skip--;
      remove = TRUE;
    } else if (count > 0) {
      count--;
      remove = FALSE;
    } else {
      remove = TRUE;
    }
    if (remove) {
      tmp = iter;
      iter = g_list_next (iter);
      g_free (tmp->data);
      entries = g_list_delete_link (entries, tmp);
    } else {
      iter = g_list_next (iter);
    }
  }

  /* Emit results */
  if (entries) {
    /* Use the idle loop to avoid blocking for too long */
    BrowseIdleData *idle_data = g_slice_new (BrowseIdleData);
    gint global_count = grl_operation_options_get_count (bs->options);
    idle_data->spec = bs;
    idle_data->remaining = global_count - count - 1;
    idle_data->path = path;
    idle_data->entries = entries;
    idle_data->current = entries;
    idle_data->cancellable = g_cancellable_new ();
    idle_data->id = bs->operation_id;
    g_hash_table_insert (GRL_FILESYSTEM_SOURCE (bs->source)->priv->cancellables,
                         GUINT_TO_POINTER (bs->operation_id),
                         idle_data->cancellable);

    g_idle_add (browse_emit_idle, idle_data);
  } else {
    /* No results */
    bs->callback (bs->source,
      bs->operation_id,
      NULL,
      0,
      bs->user_data,
      NULL);
  }

  g_dir_close (dir);
}


static gboolean
browse_emit_idle (gpointer user_data)
{
  BrowseIdleData *idle_data;
  guint count;
  GrlFilesystemSource *fs_source;

  GRL_DEBUG ("browse_emit_idle");

  idle_data = (BrowseIdleData *) user_data;
  fs_source = GRL_FILESYSTEM_SOURCE (idle_data->spec->source);

  if (g_cancellable_is_cancelled (idle_data->cancellable)) {
    GRL_DEBUG ("Browse operation %d (\"%s\") has been cancelled",
               idle_data->id, idle_data->path);
    idle_data->spec->callback(idle_data->spec->source,
                              idle_data->id, NULL, 0,
                              idle_data->spec->user_data, NULL);
    goto finish;
  }

  count = 0;
  do {
    gchar *entry_path;
    GrlMedia *content;
    GrlOperationOptions *options = idle_data->spec->options;

    entry_path = (gchar *) idle_data->current->data;
    content = create_content (NULL,
                              entry_path,
                              grl_operation_options_get_flags (options)
                              & GRL_RESOLVE_FAST_ONLY,
                              FALSE,
                              options);
    g_free (idle_data->current->data);

    idle_data->spec->callback (idle_data->spec->source,
             idle_data->spec->operation_id,
             content,
             idle_data->remaining--,
             idle_data->spec->user_data,
             NULL);

    idle_data->current = g_list_next (idle_data->current);
    count++;
  } while (count < BROWSE_IDLE_CHUNK_SIZE && idle_data->current);

  if (!idle_data->current)
    goto finish;

  return TRUE;

finish:
    g_list_free (idle_data->entries);
    g_hash_table_remove (fs_source->priv->cancellables,
                         GUINT_TO_POINTER (idle_data->id));
    g_object_unref (idle_data->cancellable);
    g_slice_free (BrowseIdleData, idle_data);
    return FALSE;
}

static GrlMedia *
create_content (GrlMedia *content,
                const gchar *path,
                gboolean only_fast,
    gboolean root_dir,
                GrlOperationOptions *options)
{
  GrlMedia *media = NULL;
  gchar *str;
  gchar *extension;
  const gchar *mime;
  GError *error = NULL;

  GFile *file = g_file_new_for_path (path);
  GFileInfo *info = g_file_query_info (file,
               FILE_ATTRIBUTES,
               0,
               NULL,
               &error);

  /* Update mode */
  if (content) {
    media = content;
  }

  if (error) {
    GRL_DEBUG ("Failed to get info for file '%s': %s", path,
               error->message);
    if (!media) {
      media = grl_media_new ();
      grl_media_set_id (media,  root_dir ? NULL : path);
    }

    /* Title */
    str = g_strdup (g_strrstr (path, G_DIR_SEPARATOR_S));
    if (!str) {
      str = g_strdup (path);
    }

    /* Remove file extension */
    extension = g_strrstr (str, ".");
    if (extension) {
      *extension = '\0';
    }

    grl_media_set_title (media, str);
    g_error_free (error);
    g_free (str);
  } else {
    mime = g_file_info_get_content_type (info);

    if (!media) {
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
  media = GRL_MEDIA (grl_media_box_new ());
      } else {
  if (mime_is_video (mime)) {
    media = grl_media_video_new ();
  } else if (mime_is_audio (mime)) {
    media = grl_media_audio_new ();
  } else if (mime_is_image (mime)) {
    media = grl_media_image_new ();
  } else {
    media = grl_media_new ();
  }
      }
      grl_media_set_id (media,  root_dir ? NULL : path);
    }

    if (!GRL_IS_MEDIA_BOX (media)) {
      grl_media_set_mime (media, mime);
    }

    /* Title */
    str = g_strdup (g_file_info_get_display_name (info));

    /* Remove file extension */
    extension = g_strrstr (str, ".");
    if (extension) {
      *extension = '\0';
    }

    grl_media_set_title (media, str);
    g_free (str);

    /* Date */
    GTimeVal time;
    GDateTime *date_time;
    g_file_info_get_modification_time (info, &time);
    date_time = g_date_time_new_from_timeval_utc (&time);
    grl_media_set_modification_date (media, date_time);
    g_date_time_unref (date_time);

    /* Thumbnail */
    gboolean thumb_failed =
      g_file_info_get_attribute_boolean (info,
                                         G_FILE_ATTRIBUTE_THUMBNAILING_FAILED);
    if (!thumb_failed) {
      const gchar *thumb =
        g_file_info_get_attribute_byte_string (info,
                                               G_FILE_ATTRIBUTE_THUMBNAIL_PATH);
      if (thumb) {
  gchar *thumb_uri = g_filename_to_uri (thumb, NULL, NULL);
  if (thumb_uri) {
    grl_media_set_thumbnail (media, thumb_uri);
    g_free (thumb_uri);
  }
      }
    }

    g_object_unref (info);
  }

  /* URL */
  str = g_file_get_uri (file);
  grl_media_set_url (media, str);
  g_free (str);

  /* Childcount */
  if (GRL_IS_MEDIA_BOX (media)) {
    set_container_childcount (path, media, only_fast, options);
  }

  g_object_unref (file);

  return media;
}
#endif
