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

/**
 * SECTION:grl-pls
 * @short_description: playlist handling functions
 *
 * Existing grilo library and its plugins do not support getting information
 * from playlists. This library allow to identify playlists, and browse
 * into them exposing playlist entries as GrlMedia objects.
 *
 * The usage of this library is entirely optional, as it is not used yet in
 * any grilo plugin. It is meant to be used either from grilo plugins or
 * from applications using grilo.
 *
 * The main use case is to add browsable capabilities to playlist files
 * in the local filesystem. However, it was agreed to provide it as a library
 * so it could be reused and/or adapted for future use cases.
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

/* --------- Constants -------- */

#define GRL_DATA_PRIV_PLS_IS_PLAYLIST   "priv:pls:is_playlist"
#define GRL_DATA_PRIV_PLS_VALID_ENTRIES "priv:pls:valid_entries"

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
  gpointer user_data;
  GCancellable *cancellable;
  GArray *entries;
};

struct OperationState {
  GrlSource *source;
  guint operation_id;
  gboolean cancelled;
  gboolean completed;
  gboolean started;
  GrlSourceBrowseSpec *bs;
};

/* -------- Prototypes ------- */

static void
grl_pls_cancel_cb (struct OperationState *op_state);

/* -------- Variables ------- */

static GHashTable *operations = NULL;

/* -------- Functions ------- */

static void
grl_pls_private_free (struct _GrlPlsPrivate *priv)
{
  g_return_if_fail (priv);

  if (priv->cancellable) {
    g_object_unref (priv->cancellable);
    priv->cancellable = NULL;
  }

  g_free (priv);
}

static void
grl_source_browse_spec_free (GrlSourceBrowseSpec *spec)
{
  if (spec->source) {
    g_object_unref (spec->source);
    spec->source = NULL;
  }

  if (spec->container) {
    g_object_unref (spec->container);
    spec->container = NULL;
  }

  if (spec->keys) {
    /* TODO */
    spec->keys = NULL;
  }

  if (spec->options) {
    g_object_unref (spec->options);
    spec->options = NULL;
  }

  if (spec->user_data) {
    struct _GrlPlsPrivate *priv = (struct _GrlPlsPrivate *) spec->user_data;
    grl_pls_private_free (priv);
  }

  g_free (spec);
}

static void
grl_pls_entry_free (struct _GrlPlsEntry *entry)
{
  g_return_if_fail (entry);

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
grl_pls_entries_array_free (GArray *entries)
{
  g_return_if_fail (entries);

  if (entries) {
    int i;
    for (i = 0;i < entries->len;i++) {
      struct _GrlPlsEntry *entry;
      entry = &g_array_index (entries, struct _GrlPlsEntry, i);
      grl_pls_entry_free (entry);
    }
    g_array_free (entries, TRUE);
  }
}

static void
grl_pls_valid_entries_ptrarray_free (GPtrArray *valid_entries)
{
  g_return_if_fail (valid_entries);

  if (valid_entries) {
    int i;
    for (i = 0;i < valid_entries->len;i++) {
      GrlMedia *content = g_ptr_array_index (valid_entries, i);
      g_object_unref (content);
    }
    g_ptr_array_free (valid_entries, TRUE);
  }
}

static void
grl_pls_init (void)
{
  static gboolean initialized = FALSE;

  if (!initialized) {
    GRL_LOG_DOMAIN_INIT (libpls_log_domain, "pls");

    operations = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                        NULL,
                                        (GDestroyNotify) grl_source_browse_spec_free);

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
  g_return_if_fail (op_state);

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
operation_set_ongoing (GrlSource *source, guint operation_id, GrlSourceBrowseSpec *bs)
{
  struct OperationState *op_state;

  g_return_if_fail (source);

  GRL_DEBUG ("%s (%d)", __FUNCTION__, operation_id);

  op_state = g_new0 (struct OperationState, 1);
  op_state->source = g_object_ref (source);
  op_state->operation_id = operation_id;
  op_state->bs = bs;

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
  struct _GrlPlsPrivate *priv;

  g_return_if_fail (op_state);

  GRL_DEBUG ("%s (%p)", __FUNCTION__, op_state);

  if (!operation_is_ongoing (op_state->operation_id)) {
    GRL_DEBUG ("Tried to cancel invalid or already cancelled operation. "
               "Skipping...");
    return;
  }

  operation_set_cancelled (op_state->operation_id);

  /* Cancel the totem playlist parsing operation */
  priv = (struct _GrlPlsPrivate *) op_state->bs->user_data;
  if (priv && !g_cancellable_is_cancelled (priv->cancellable)) {
    g_cancellable_cancel (priv->cancellable);
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
  grl_pls_init();

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
  grl_pls_init();

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
  gpointer ptr;
  gboolean is_pls;
  gint is_pls_encoded;

  grl_pls_init();

  GRL_DEBUG ("%s (\"%p\")", __FUNCTION__, media);

  g_return_val_if_fail (media, FALSE);

  ptr = g_object_get_data (G_OBJECT (media), GRL_DATA_PRIV_PLS_IS_PLAYLIST);
  if (ptr) {
    is_pls_encoded = GPOINTER_TO_INT(ptr);
    is_pls = is_pls_encoded > 0;
    GRL_DEBUG ("%s : using cached value = %d", __FUNCTION__, is_pls);
    return is_pls;
  }

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

  is_pls = grl_pls_file_is_playlist (filename);

  /* is_pls must be encoded to differentiate between the cases:
   * - non-cached (null)
   * - not a playlist (negative)
   * - playlist (positive) */
  is_pls_encoded = is_pls ? 1 : -1;
  ptr = GINT_TO_POINTER (is_pls_encoded);
  g_object_set_data (G_OBJECT (media), GRL_DATA_PRIV_PLS_IS_PLAYLIST, ptr);
  GRL_DEBUG ("%s : caching value = %d", __FUNCTION__, is_pls);

  g_free (filename);

  return is_pls;
}

static void
grl_pls_playlist_entry_parsed_cb (TotemPlParser *parser,
                                  const gchar *uri,
                                  GHashTable *metadata,
                                  gpointer user_data)
{
  GrlSourceBrowseSpec *bs = (GrlSourceBrowseSpec *) user_data;
  struct _GrlPlsPrivate *priv;
  struct _GrlPlsEntry entry;
  GError *_error;

  GRL_DEBUG ("%s (parser=%p, uri=\"%s\", metadata=%p, user_data=%p)",
      __FUNCTION__, parser, uri, metadata, user_data);

  g_return_if_fail (TOTEM_IS_PL_PARSER (parser));
  g_return_if_fail (uri);
  g_return_if_fail (metadata);
  g_return_if_fail (user_data);
  g_return_if_fail (bs->user_data);

  priv = (struct _GrlPlsPrivate *) bs->user_data;

  /* Ignore elements after operation has completed */
  if (operation_is_completed (bs->operation_id)) {
    GRL_WARNING ("Entry parsed after playlist completed for operation %d",
                 bs->operation_id);
    return;
  }

  /* Check if cancelled */
  if (operation_is_cancelled (bs->operation_id)) {
    GRL_DEBUG ("Operation was cancelled, skipping result until getting the last one");
    /* Wait for the last element */
    _error = g_error_new (GRL_CORE_ERROR,
                          GRL_CORE_ERROR_OPERATION_CANCELLED,
                          "Operation was cancelled");
    struct _GrlPlsPrivate *priv = (struct _GrlPlsPrivate *) bs->user_data;
    bs->callback (bs->source, bs->operation_id, NULL, 0, priv->user_data, _error);
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

  if (priv->entries)
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
  GTimeVal time;
  GDateTime *date_time;
  gboolean thumb_failed;

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
  g_file_info_get_modification_time (info, &time);
  date_time = g_date_time_new_from_timeval_utc (&time);
  grl_media_set_modification_date (media, date_time);
  g_date_time_unref (date_time);

  /* Thumbnail */
  thumb_failed = g_file_info_get_attribute_boolean (info,
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

static gint
grl_pls_browse_report_error (GrlSourceBrowseSpec *bs, const gchar *message)
{
  GError *error = g_error_new (GRL_CORE_ERROR,
                               GRL_CORE_ERROR_BROWSE_FAILED,
                               "%s",
                               message);
  bs->callback (bs->source, bs->operation_id, NULL, 0, bs->user_data, error);
  g_error_free (error);

  return FALSE;
}

static gboolean
grl_pls_browse_report_results (GrlSourceBrowseSpec *bs)
{
  guint skip;
  guint count;
  guint remaining;
  GPtrArray *valid_entries;
  struct _GrlPlsPrivate *priv;

  GRL_DEBUG ("%s (bs=%p)", __FUNCTION__, bs);

  g_return_val_if_fail (bs, FALSE);
  g_return_val_if_fail (bs->container, FALSE);
  g_return_val_if_fail (bs->options, FALSE);
  g_return_val_if_fail (bs->operation_id, FALSE);
  g_return_val_if_fail (bs->user_data, FALSE);

  priv = (struct _GrlPlsPrivate *) bs->user_data;

  valid_entries = g_object_get_data (G_OBJECT (bs->container),
      GRL_DATA_PRIV_PLS_VALID_ENTRIES);
  if (valid_entries) {
    skip = grl_operation_options_get_skip (bs->options);
    if (skip > valid_entries->len)
      skip = valid_entries->len;

    count = grl_operation_options_get_count (bs->options);
    if (skip + count > valid_entries->len)
      count = valid_entries->len - skip;

    remaining = valid_entries->len - skip;
  } else {
    skip = 0;
    count = 0;
    remaining = 0;
  }

  if (remaining) {
    int i;

    for (i = 0;i < count;i++) {
      GrlMedia *content = g_ptr_array_index(valid_entries, skip + i);
      g_object_ref (content);
      remaining--;
      bs->callback (bs->source,
               bs->operation_id,
               content,
               remaining,
               priv->user_data,
               NULL);
      GRL_DEBUG ("callback called source=%p id=%d content=%p remaining=%d user_data=%p",
          bs->source, bs->operation_id, content, remaining, priv->user_data);
    }
  } else {
    bs->callback (bs->source,
             bs->operation_id,
             NULL,
             0,
             priv->user_data,
             NULL);
  }

  operation_set_finished (bs->operation_id);

  g_hash_table_remove (operations, GUINT_TO_POINTER (bs->operation_id));

  return FALSE;
}

static void
grl_pls_playlist_parse_cb (GObject *object,
                           GAsyncResult *result,
                           gpointer user_data)
{
  TotemPlParser *parser = (TotemPlParser *) object;
  TotemPlParserResult retval;
  GrlSourceBrowseSpec *bs = (GrlSourceBrowseSpec *) user_data;
  struct _GrlPlsPrivate *priv;
  GError *error = NULL;
  guint i;
  GPtrArray *valid_entries;

  GRL_DEBUG ("%s (object=%p, result=%p, user_data=%p)", __FUNCTION__, object, result, user_data);

  g_return_if_fail (object);
  g_return_if_fail (result);
  g_return_if_fail (bs);
  g_return_if_fail (bs->operation_id);
  g_return_if_fail (bs->container);
  g_return_if_fail (bs->user_data);

  priv = (struct _GrlPlsPrivate *) bs->user_data;

  retval = totem_pl_parser_parse_finish (parser, result, &error);
  if (retval != TOTEM_PL_PARSER_RESULT_SUCCESS) {
    GRL_ERROR ("Playlist parsing failed, retval=%d code=%d msg=%s", retval, error->code, error->message);
    g_error_free (error);
  }

  operation_set_completed (bs->operation_id);

  valid_entries = g_object_get_data (G_OBJECT (bs->container), GRL_DATA_PRIV_PLS_VALID_ENTRIES);

  /* process all entries to see which ones are valid */
  for (i = 0;i < priv->entries->len;i++) {
    struct _GrlPlsEntry *entry;
    entry = &g_array_index (priv->entries, struct _GrlPlsEntry, i);
    if (entry) {
      GrlMedia *media = grl_media_new_from_pls_entry (entry);
      if (media) {
        entry->media = media;
        g_ptr_array_add(valid_entries, media);
        g_object_ref(media);
      }
    }
  }

  /* at this point we can free entries, not used anymore */
  grl_pls_entries_array_free (priv->entries);
  priv->entries = NULL;

  if (GRL_IS_MEDIA_BOX (bs->container)) {
    GrlMediaBox *box = GRL_MEDIA_BOX (bs->container);
    grl_media_box_set_childcount (box, valid_entries->len);
  }

  grl_pls_browse_report_results (bs);
}

static gboolean
check_options (GrlSource *source,
               GrlSupportedOps operation,
               GrlOperationOptions *options)
{
  if (grl_operation_options_get_count (options) == 0)
    return FALSE;

  /* Check only if the source supports the operation */
  if (grl_source_supported_operations (source) & operation) {
    GrlCaps *caps;
    caps = grl_source_get_caps (source, operation);
    return grl_operation_options_obey_caps (options, caps, NULL, NULL);
  } else {
    return TRUE;
  }
}

/**
 * grl_pls_browse_by_spec:
 * @source: a source
 * @bs: a GrlSourceBrowseSpec structure with details of the browsing operation
 *
 * Browse into a playlist. The playlist entries are
 * returned via the bs->callback function as GrlMedia objects.
 * This function is more suitable to be called from plugins, which by
 * design get the GrlSourceBrowseSpec already filled in.
 *
 * The bs->playlist provided could be of any GrlMedia class,
 * as long as its URI points to a valid playlist file.
 *
 * This function is asynchronous.
 *
 * See #grl_pls_browse() and #grl_source_browse() function for additional
 * information and sample code.
 *
 * Since: 0.2.6
 */
void
grl_pls_browse_by_spec (GrlSource *source,
                        GrlSourceBrowseSpec *bs)
{
  TotemPlParser *parser;
  const char *playlist_url;
  struct _GrlPlsPrivate *priv;
  GPtrArray *valid_entries;

  grl_pls_init();

  GRL_DEBUG (__FUNCTION__);

  g_return_if_fail (GRL_IS_SOURCE (source));
  g_return_if_fail (GRL_IS_MEDIA (bs->container));
  g_return_if_fail (GRL_IS_OPERATION_OPTIONS (bs->options));
  g_return_if_fail (bs->callback != NULL);
  g_return_if_fail (grl_source_supported_operations (bs->source) &
                    GRL_OP_BROWSE);
  g_return_if_fail (check_options (source, GRL_OP_BROWSE, bs->options));

  priv = g_new0 (struct _GrlPlsPrivate, 1);
  if (!priv) {
    return;
  }

  priv->user_data = bs->user_data;
  priv->cancellable = g_cancellable_new ();
  bs->user_data = priv;

  playlist_url = grl_media_get_url (bs->container);
  if (!playlist_url) {
    GRL_DEBUG ("%s : Unable to get URL from Media %p", __FUNCTION__, bs->container);
    grl_pls_browse_report_error (bs, "Unable to get URL from Media");
    return;
  }

  if (!grl_pls_media_is_playlist (bs->container)) {
    GRL_DEBUG ("%s: URI=%s is not a playlist", __FUNCTION__, playlist_url);
    grl_pls_browse_report_error (bs, "Media is not a playlist");
    return;
  }

  /* check if we have the entries cached or not */
  valid_entries = g_object_get_data (G_OBJECT (bs->container), GRL_DATA_PRIV_PLS_VALID_ENTRIES);
  if (valid_entries) {
    GRL_DEBUG ("%s : using cached data bs=%p", __FUNCTION__, bs);
    g_idle_add ((GSourceFunc) grl_pls_browse_report_results, bs);
    return;
  }

  priv->entries = g_array_new (FALSE, FALSE, sizeof(struct _GrlPlsEntry));
  if (!priv->entries) {
    grl_pls_browse_report_error (bs, "Not enough memory");
    return;
  }

  valid_entries = g_ptr_array_sized_new(16);
  if (!valid_entries) {
    grl_pls_browse_report_error (bs, "Not enough memory");
    return;
  }

  parser = totem_pl_parser_new ();
  if (!parser) {
    grl_pls_browse_report_error (bs, "Error creating playlist parser");
    return;
  }

  g_object_set_data_full (G_OBJECT (bs->container),
      GRL_DATA_PRIV_PLS_VALID_ENTRIES,
      valid_entries,
      (GDestroyNotify) grl_pls_valid_entries_ptrarray_free);

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

  totem_pl_parser_parse_async (parser,
                               playlist_url,
                               FALSE,
                               priv->cancellable,
                               grl_pls_playlist_parse_cb,
                               priv);

  g_object_unref (parser);
}

/**
 * grl_pls_browse:
 * @source: a source
 * @playlist: a playlist
 * @keys: (element-type GrlKeyID): the #GList of
 * #GrlKeyID<!-- -->s to request
 * @options: options wanted for that operation
 * @callback: (scope notified): the user defined callback
 * @user_data: the user data to pass in the callback
 *
 * Browse into a playlist. The playlist entries are
 * returned via the @callback function as GrlMedia objects.
 * This function imitates the API and way of working of
 * #grl_source_browse.
 *
 * The @playlist provided could be of any GrlMedia class,
 * as long as its URI points to a valid playlist file.
 *
 * This function is asynchronous.
 *
 * See #grl_source_browse() function for additional information
 * and sample code.
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
  GrlSourceBrowseSpec *bs;

  grl_pls_init();

  GRL_DEBUG (__FUNCTION__);

  g_return_val_if_fail (GRL_IS_SOURCE (source), 0);
  g_return_val_if_fail (GRL_IS_MEDIA (playlist), 0);
  g_return_val_if_fail (GRL_IS_OPERATION_OPTIONS (options), 0);
  g_return_val_if_fail (callback != NULL, 0);
  g_return_val_if_fail (grl_source_supported_operations (source) &
                        GRL_OP_BROWSE, 0);
  g_return_val_if_fail (check_options (source, GRL_OP_BROWSE, options), 0);

  bs = g_new0 (GrlSourceBrowseSpec, 1);
  if (!bs) {
    return 0;
  }

  bs->source = g_object_ref (source);
  bs->container = g_object_ref (playlist);
  /* TODO: what to do with keys */
  bs->keys = NULL;
  bs->options = grl_operation_options_copy (options);
  bs->callback = callback;
  bs->user_data = userdata;
  bs->operation_id = grl_operation_generate_id ();

  g_hash_table_insert (operations, GUINT_TO_POINTER (bs->operation_id), bs);

  operation_set_ongoing (source, bs->operation_id, bs);

  grl_pls_browse_by_spec (source, bs);

  return bs->operation_id;
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
 * @playlist: a playlist
 * @keys: (element-type GrlKeyID): the #GList of
 * #GrlKeyID<!-- -->s to request
 * @options: options wanted for that operation
 * @error: a #GError, or @NULL
 *
 * Browse into a playlist. The playlist entries are
 * returned via the @callback function as GrlMedia objects.
 * This function imitates the API and way of working of
 * #grl_source_browse_sync.
 *
 * This function is synchronous.
 *
 * See #grl_source_browse_sync() function for additional information
 * and sample code.
 *
 * Returns: (element-type Grl.Media) (transfer full): a #GList with #GrlMedia
 * elements. After use g_object_unref() every element and g_list_free() the
 * list.
 *
 * Since: 0.2.6
 */
GList *
grl_pls_browse_sync (GrlSource *source,
                     GrlMedia *playlist,
                     const GList *keys,
                     GrlOperationOptions *options,
                     GError **error)
{
  GrlDataSync *ds;
  GList *result;

  grl_pls_init();

  GRL_DEBUG (__FUNCTION__);

  ds = g_slice_new0 (GrlDataSync);
  if (!ds) {
    return NULL;
  }

  if (grl_pls_browse (source,
                      playlist,
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
