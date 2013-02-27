/*
 * Copyright (C) 2013 Collabora Ltd.
 *
 * Author: Mateu Batle Sastre <mateu.batle@collabora.com>
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
#include "../../src/grl-operation-priv.h"
#include "../../src/grl-sync-priv.h"

#include <gio/gio.h>
#include <grilo.h>
#include <stdlib.h>
#include <string.h>
#include <totem-pl-parser.h>
#include <totem-pl-parser-mini.h>

/* --------- Logging  -------- */

#define GRL_LOG_DOMAIN_DEFAULT libpls_log_domain
GRL_LOG_DOMAIN_STATIC(libpls_log_domain);

/* -------- File info ------- */

#define FILE_ATTRIBUTES                         \
  G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","    \
  G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","    \
  G_FILE_ATTRIBUTE_STANDARD_TYPE ","            \
  G_FILE_ATTRIBUTE_TIME_MODIFIED ","            \
  G_FILE_ATTRIBUTE_THUMBNAIL_PATH ","           \
  G_FILE_ATTRIBUTE_THUMBNAILING_FAILED

/* -------- Data structures ------- */

struct _GrlPlsEntry {
  gchar *uri;
  gchar *title;
  gchar *genre;
  gchar *author;
  gchar *album;
  gchar *mime;
  gchar *thumbnail;
  gint64 duration;
  GrlMedia *media;
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
  GPtrArray *valid_entries;
  GCancellable *cancellable;
};

struct OperationState {
  GrlSource *source;
  guint operation_id;
  gboolean cancelled;
  gboolean completed;
  gboolean started;
  struct _GrlPlsPrivate *priv;
};

/* -------- Prototypes ------- */

static void
grl_pls_cancel_cb (struct OperationState *op_state);

/* -------- Functions ------- */

static void
grl_pls_private_free (struct _GrlPlsPrivate *priv)
{
  if (priv->source) {
    g_object_unref (priv->source);
    priv->source = NULL;
  }

  if (priv->playlist) {
    g_object_unref (priv->playlist);
    priv->playlist = NULL;
  }

  if (priv->keys) {
    /* TODO */
    priv->keys = NULL;
  }

  if (priv->options) {
    g_object_unref (priv->options);
    priv->options = NULL;
  }

  if (priv->entries) {
    g_array_free (priv->entries, TRUE);
    priv->entries = NULL;
  }

  if (priv->valid_entries) {
    g_ptr_array_free (priv->valid_entries, TRUE);
    priv->valid_entries = NULL;
  }

  if (priv->cancellable) {
    g_object_unref (priv->cancellable);
    priv->cancellable = NULL;
  }
}

static void
grl_pls_entry_free (struct _GrlPlsEntry *entry)
{
  if (entry->uri) {
    g_free (entry->uri);
    entry->uri = NULL;
  }

  if (entry->title) {
    g_free (entry->title);
    entry->title = NULL;
  }

  if (entry->genre) {
    g_free (entry->genre);
    entry->genre = NULL;
  }

  if (entry->author) {
    g_free (entry->author);
    entry->author = NULL;
  }

  if (entry->album) {
    g_free (entry->album);
    entry->album = NULL;
  }

  if (entry->thumbnail) {
    g_free (entry->thumbnail);
    entry->thumbnail = NULL;
  }

  if (entry->mime) {
    g_free (entry->mime);
    entry->mime = NULL;
  }

  if (entry->media) {
    g_object_unref (entry->media);
    entry->media = NULL;
  }
}

static void
grl_pls_init (void)
{
  static gboolean initialized = FALSE;

  if (!initialized) {
    GRL_LOG_DOMAIN_INIT (libpls_log_domain, "pls");
    initialized = TRUE;
  }
}

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
  g_return_val_if_fail (mime, FALSE);

  if (!mime)
    return FALSE;
  if (!strcmp (mime, "inode/directory"))
    return TRUE;
  if ((filter & GRL_TYPE_FILTER_AUDIO) && mime_is_audio (mime))
    return TRUE;
  if ((filter & GRL_TYPE_FILTER_VIDEO) && mime_is_video (mime))
    return TRUE;
  if ((filter & GRL_TYPE_FILTER_IMAGE) && mime_is_image (mime))
    return TRUE;
  return FALSE;
}

static void
operation_state_free (struct OperationState *op_state)
{
  GRL_DEBUG ("%s (%p)", __FUNCTION__, op_state);

  g_object_unref (op_state->source);
  g_free (op_state);
}

/*
 * operation_set_finished:
 *
 * Sets operation as finished (we have already emitted the last result
 * to the user).
 */
static void
operation_set_finished (guint operation_id)
{
  GRL_DEBUG ("%s (%d)", __FUNCTION__, operation_id);

  grl_operation_remove (operation_id);
}

/*
 * operation_set_completed:
 *
 * Sets the operation as completed (we have already received the last
 * result in the relay cb. If it is finsihed it is also completed).
 */
static void
operation_set_completed (guint operation_id)
{
  struct OperationState *op_state;

  GRL_DEBUG ("%s (%d)", __FUNCTION__, operation_id);

  op_state = grl_operation_get_private_data (operation_id);

  if (op_state) {
    op_state->completed = TRUE;
  }
}

/*
 * operation_is_completed:
 *
 * Checks if operation is completed (we have already received the last
 * result in the relay cb. A finished operation is also a completed
 * operation).
 */
static gboolean
operation_is_completed (guint operation_id)
{
  struct OperationState *op_state;

  op_state = grl_operation_get_private_data (operation_id);

  return !op_state || op_state->completed;
}

/*
 * operation_set_cancelled:
 *
 * Sets the operation as cancelled (a valid operation, i.e., not
 * finished, was cancelled)
 */
static void
operation_set_cancelled (guint operation_id)
{
  struct OperationState *op_state;

  GRL_DEBUG ("%s (%d)", __FUNCTION__, operation_id);

  op_state = grl_operation_get_private_data (operation_id);

  if (op_state) {
    op_state->cancelled = TRUE;
  }
}

/*
 * operation_is_cancelled:
 *
 * Checks if operation is cancelled (a valid operation that was
 * cancelled).
 */
static gboolean
operation_is_cancelled (guint operation_id)
{
  struct OperationState *op_state;

  op_state = grl_operation_get_private_data (operation_id);

  return op_state && op_state->cancelled;
}

/*
 * operation_set_ongoing:
 *
 * Sets the operation as ongoing (operation is valid, not finished, not started
 * and not cancelled)
 */
static void
operation_set_ongoing (GrlSource *source, guint operation_id, struct _GrlPlsPrivate *priv)
{
  struct OperationState *op_state;

  GRL_DEBUG ("%s (%d)", __FUNCTION__, operation_id);

  op_state = g_new0 (struct OperationState, 1);
  op_state->source = g_object_ref (source);
  op_state->operation_id = operation_id;
  op_state->priv = priv;

  grl_operation_set_private_data (operation_id,
                                  op_state,
                                  (GrlOperationCancelCb) grl_pls_cancel_cb,
                                  (GDestroyNotify) operation_state_free);
}

/*
 * operation_is_ongoing:
 *
 * Checks if operation is ongoing (operation is valid, and it is not
 * finished nor cancelled).
 */
static gboolean
operation_is_ongoing (guint operation_id)
{
  struct OperationState *op_state;

  op_state = grl_operation_get_private_data (operation_id);

  return op_state && !op_state->cancelled;
}

static void
grl_pls_cancel_cb (struct OperationState *op_state)
{
  GRL_DEBUG ("%s (%p)", __FUNCTION__, op_state);

  if (!operation_is_ongoing (op_state->operation_id)) {
    GRL_DEBUG ("Tried to cancel invalid or already cancelled operation. "
               "Skipping...");
    return;
  }

  operation_set_cancelled (op_state->operation_id);

  /* Cancel the totem playlist parsing operation */
  if (!g_cancellable_is_cancelled (op_state->priv->cancellable)) {
    g_cancellable_cancel (op_state->priv->cancellable);
  }
}

/**
 * grl_pls_mime_is_playlist:
 * @mime: mime type of the playlist
 *
 * Check if mime type corresponds to a playlist or not.
 * This is quick to determine, but it does not offer full guarantees.
 *
 * Returns: TRUE if mime type is a playlist recognized mime type
 *
 * Since: 0.2.6
 */
gboolean
grl_pls_mime_is_playlist (const gchar *mime)
{
  GRL_DEBUG ("%s (\"%s\")", __FUNCTION__, mime);

  g_return_val_if_fail (mime, FALSE);

  return g_str_has_prefix (mime, "audio/x-ms-asx") ||
         g_str_has_prefix (mime, "audio/mpegurl") ||
         g_str_has_prefix (mime, "audio/x-mpegurl") ||
         g_str_has_prefix (mime, "audio/x-scpls");
}

/**
 * grl_pls_file_is_playlist:
 * @filename: path to file
 *
 * Check if a file identified by filename is a playlist or not.
 * This actually reads part of the file contents, so it might
 * have some impact in performance than grl_pls_mime_is_playlist.
 *
 * Returns: TRUE if a file is recognized as a playlist.
 *
 * Since: 0.2.6
 */
gboolean
grl_pls_file_is_playlist (const gchar *filename)
{
  GRL_DEBUG ("%s (\"%s\")", __FUNCTION__, filename);

  g_return_val_if_fail (filename, FALSE);

  return totem_pl_parser_can_parse_from_filename (filename, FALSE);
}

/**
 * grl_pls_media_is_playlist:
 * @media: GrlMedia
 *
 * Check if a file identified by GrlMedia object is a playlist or not.
 * This actually reads part of the file contents, so it might
 * have some impact in performance than grl_pls_mime_is_playlist.
 *
 * Returns: TRUE if a GrlMedia is recognized as a playlist.
 *
 * Since: 0.2.6
 */
gboolean
grl_pls_media_is_playlist (GrlMedia *media)
{
  const gchar *playlist_url;
  gchar *filename;
  gchar *scheme;

  GRL_DEBUG ("%s (\"%p\")", __FUNCTION__, media);

  g_return_val_if_fail (media, FALSE);

  playlist_url = grl_media_get_url (media);
  if (!playlist_url) {
    return FALSE;
  }

  scheme = g_uri_parse_scheme (playlist_url);
  if (!scheme) {
    /* we assume it is pathname */
    filename = g_strdup (scheme);
  } else if (!g_strcmp0 (scheme, "file")) {
    filename = g_filename_from_uri (playlist_url, NULL, NULL);
    g_free (scheme);
  } else {
    g_free (scheme);
    return FALSE;
  }

  return grl_pls_file_is_playlist (filename);
}

static void
grl_pls_playlist_entry_parsed_cb (TotemPlParser *parser,
                                  const gchar *uri,
                                  GHashTable *metadata,
                                  gpointer user_data)
{
  struct _GrlPlsPrivate *priv = (struct _GrlPlsPrivate *) user_data;
  struct _GrlPlsEntry entry;
  GError *_error;

  GRL_DEBUG ("%s (parser=%p, uri=\"%s\", metadata=%p, user_data=%p)",
      __FUNCTION__, parser, uri, metadata, user_data);

  g_return_if_fail (TOTEM_IS_PL_PARSER (parser));
  g_return_if_fail (uri);
  g_return_if_fail (metadata);
  g_return_if_fail (user_data);

  /* Ignore elements after operation has completed */
  if (operation_is_completed (priv->operation_id)) {
    GRL_WARNING ("Entry parsed after playlist completed for operation %d",
                 priv->operation_id);
    return;
  }

  /* Check if cancelled */
  if (operation_is_cancelled (priv->operation_id)) {
    GRL_DEBUG ("Operation was cancelled, skipping result until getting the last one");
    /* Wait for the last element */
    _error = g_error_new (GRL_CORE_ERROR,
                          GRL_CORE_ERROR_OPERATION_CANCELLED,
                          "Operation was cancelled");
    priv->callback (priv->source, priv->operation_id, NULL, 0, priv->userdata, _error);
    g_error_free (_error);
    return;
  }

  entry.uri = g_strdup (uri);
  entry.title = g_strdup (g_hash_table_lookup (metadata, TOTEM_PL_PARSER_FIELD_TITLE));
  entry.genre = g_strdup (g_hash_table_lookup (metadata, TOTEM_PL_PARSER_FIELD_GENRE));
  entry.author = g_strdup (g_hash_table_lookup (metadata, TOTEM_PL_PARSER_FIELD_AUTHOR));
  entry.album = g_strdup (g_hash_table_lookup (metadata, TOTEM_PL_PARSER_FIELD_ALBUM));
  entry.thumbnail = g_strdup (g_hash_table_lookup (metadata, TOTEM_PL_PARSER_FIELD_IMAGE_URI));
  entry.mime = g_strdup (g_hash_table_lookup (metadata, TOTEM_PL_PARSER_FIELD_CONTENT_TYPE));
  entry.duration = totem_pl_parser_parse_duration (g_hash_table_lookup (metadata, TOTEM_PL_PARSER_FIELD_DURATION), FALSE);

  GRL_DEBUG ("New playlist entry: URI=%s title=%s genre=%s author=%s album=%s thumbnail=%s mime=%s duration=%lld",
      entry.uri, entry.title, entry.genre, entry.author, entry.album, entry.thumbnail, entry.mime, entry.duration);

  g_array_append_vals(priv->entries, &entry, 1);
}

static GrlMedia*
grl_media_new_from_uri (gchar * uri)
{
  GrlMedia *media = NULL;
  gchar *filename = NULL;
  gchar *str;
  gchar *extension;
  const gchar *mime;
  GError *error = NULL;
  GFile *file = NULL;
  GFileInfo *info = NULL;

  GRL_DEBUG ("%s (uri=\"%s\")", __FUNCTION__, uri);

  g_return_val_if_fail (uri, NULL);

  str = g_uri_parse_scheme (uri);
  if (!str) {
    /* we assume it is pathname */
    filename = g_strdup (uri);
  } else if (!g_strcmp0 (str, "file")) {
    filename = g_filename_from_uri (uri, NULL, NULL);
    g_free (str);
  } else {
    GRL_WARNING ("URI found in playlist not handled, scheme=%s", str);
    g_free (str);
    goto out;
  }

  file = g_file_new_for_path (filename);
  if (!file)
    goto out;

  info = g_file_query_info (file,
                            FILE_ATTRIBUTES,
                            0,
                            NULL,
                            &error);
  if (!info) {
    if (error) {
      GRL_WARNING ("Unable to get file information error code=%d msg=%s", error->code, error->message);
      g_error_free (error);
    }
    goto out;
  }

  mime = g_file_info_get_content_type (info);
  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
    media = GRL_MEDIA (grl_media_box_new ());
  } else if (grl_pls_file_is_playlist(filename)) {
    GRL_DEBUG ("Playlist found -> returning media box");
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

  /* URL */
  str = g_file_get_uri (file);
  grl_media_set_url (media, str);
  g_free (str);

  GRL_DEBUG ("Returning media=%p", media);

out:

  if (info)
    g_object_unref (info);
  if (file)
    g_object_unref (file);
  if (filename)
    g_free (filename);

  return media;
}

static GrlMedia*
grl_media_new_from_pls_entry (struct _GrlPlsEntry *entry)
{
  GrlMedia *media;

  GRL_DEBUG ("%s (\"%p\")", __FUNCTION__, entry);

  g_return_val_if_fail (entry, NULL);

  media = grl_media_new_from_uri (entry->uri);
  if (media) {
    if (entry->duration > 0)
      grl_media_set_duration (media, entry->duration);

    if (GRL_IS_MEDIA_AUDIO(media)) {
      GrlMediaAudio *audio = GRL_MEDIA_AUDIO(media);
      if (entry->album)
        grl_media_audio_set_album (audio, entry->album);
      if (entry->author)
        grl_media_audio_set_artist (audio, entry->author);
      if (entry->genre)
        grl_media_audio_set_genre (audio, entry->genre);
    }
  }

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
  GError *error = NULL;
  guint skip;
  guint count;
  guint remaining;
  guint i;

  GRL_DEBUG ("%s (object=%p, result=%p, user_data=%p)", __FUNCTION__, object, result, user_data);

  g_return_if_fail (object);
  g_return_if_fail (result);
  g_return_if_fail (user_data);

  retval = totem_pl_parser_parse_finish (parser, result, &error);
  if (retval != TOTEM_PL_PARSER_RESULT_SUCCESS) {
    GRL_ERROR ("Playlist parsing failed, retval=%d code=%d msg=%s", retval, error->code, error->message);
    g_error_free (error);
  }

  operation_set_completed (priv->operation_id);

  /* process all entries to see which ones are valid */
  priv->valid_entries = g_ptr_array_sized_new(priv->entries->len);
  for (i = 0;i < priv->entries->len;i++) {
    struct _GrlPlsEntry *entry;
    entry = &g_array_index (priv->entries, struct _GrlPlsEntry, i);
    if (entry) {
      GrlMedia *media = grl_media_new_from_pls_entry (entry);
      if (media) {
        entry->media = media;
        g_ptr_array_add(priv->valid_entries, media);
      }
    }
  }

  if (GRL_IS_MEDIA_BOX(priv->playlist)) {
    GrlMediaBox *box = GRL_IS_MEDIA_BOX(priv->playlist);
    grl_media_box_set_childcount(box, priv->valid_entries->len);
  }

  skip = grl_operation_options_get_skip (priv->options);
  if (skip > priv->valid_entries->len)
    skip = priv->valid_entries->len;

  count = grl_operation_options_get_count (priv->options);
  if (skip + count > priv->valid_entries->len)
    count = priv->valid_entries->len - skip;

  remaining = priv->valid_entries->len - skip;
  if (remaining) {
    for (i = 0;i < count;i++) {
      GrlMedia *content = g_ptr_array_index(priv->valid_entries, skip + i);
      remaining--;
      priv->callback (priv->source,
               priv->operation_id,
               content,
               remaining,
               priv->userdata,
               NULL);
      GRL_DEBUG ("callback called source=%p id=%d content=%p remaining=%d userdata=%p",
          priv->source, priv->operation_id, content, remaining, priv->userdata);
    }
  } else {
    priv->callback (priv->source,
             priv->operation_id,
             NULL,
             0,
             priv->userdata,
             NULL);
  }

  operation_set_finished (priv->operation_id);
}

static gboolean
check_options (GrlSource *source,
               GrlSupportedOps operation,
               GrlOperationOptions *options)
{
  GrlCaps *caps;

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
  TotemPlParser *parser;
  const char *playlist_url;
  struct _GrlPlsPrivate *priv;

  grl_pls_init();

  GRL_DEBUG (__FUNCTION__);

  g_return_val_if_fail (GRL_IS_SOURCE (source), 0);
  g_return_val_if_fail (GRL_IS_MEDIA (playlist), 0);
  g_return_val_if_fail (GRL_IS_OPERATION_OPTIONS (options), 0);
  g_return_val_if_fail (callback != NULL, 0);
  g_return_val_if_fail (grl_source_supported_operations (source) &
                        GRL_OP_BROWSE, 0);
  g_return_val_if_fail (check_options (source, GRL_OP_BROWSE, options), 0);


  playlist_url = grl_media_get_url (playlist);
  if (!playlist_url) {
    GRL_WARNING ("Unable to get URL from Media");
    return 0;
  }

  if (!grl_pls_media_is_playlist (playlist)) {
    GRL_WARNING ("URI=%s is not a playlist", playlist_url);
    return 0;
  }

  parser = totem_pl_parser_new ();
  if (!parser) {
    return 0;
  }

  priv = g_new0(struct _GrlPlsPrivate, 1);
  if (!priv) {
    return 0;
  }

  /*
   * disable-unsafe: if FALSE the parser will not parse unsafe locations,
   * such as local devices and local files if the playlist isn't local.
   * This is useful if the library is parsing a playlist from a remote
   * location such as a website. */
  g_object_set (parser,
                "recurse", FALSE,
                "disable-unsafe", TRUE,
                NULL);
  g_signal_connect (G_OBJECT (parser),
                    "entry-parsed",
                    G_CALLBACK (grl_pls_playlist_entry_parsed_cb),
                    priv);
  GRL_DEBUG ("g_signal_connect, priv=%p", priv);

  priv->source = g_object_ref (source);
  priv->playlist = g_object_ref (playlist);
  /* TODO: what to do with keys */
  priv->keys = NULL;
  priv->options = grl_operation_options_copy (options);
  priv->callback = callback;
  priv->userdata = userdata;
  priv->operation_id = grl_operation_generate_id ();
  priv->entries = g_array_new (FALSE, FALSE, sizeof(struct _GrlPlsEntry));
  priv->valid_entries = NULL;
  priv->cancellable = g_cancellable_new ();

  operation_set_ongoing (source, priv->operation_id, priv);

  totem_pl_parser_parse_async (parser,
                               playlist_url,
                               FALSE,
                               priv->cancellable,
                               grl_pls_playlist_parse_cb,
                               priv);

  g_object_unref (parser);

  return priv->operation_id;
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

  grl_pls_init();

  GRL_DEBUG (__FUNCTION__);

  ds = g_slice_new0 (GrlDataSync);
  if  (!ds) {
    return NULL;
  }

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
