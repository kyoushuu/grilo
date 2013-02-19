
/*
 * Browsing in Grilo.
 * Shows the first BROWSE_CHUNK_SIZE elements of each browsable source
 *
 * XXX: No pagination yet! See grilo-test-ui. It's somewhat complicated.
 */

#include <grilo.h>

#define GRL_LOG_DOMAIN_DEFAULT  example_log_domain
GRL_LOG_DOMAIN_STATIC(example_log_domain);

#define BROWSE_CHUNK_SIZE 10

static void source_browser (gpointer data,
                            gpointer user_data);
static void element_browser (gpointer data,
                             gpointer user_data);

static void
element_browser (gpointer data,
                 gpointer user_data)
{
  GrlMedia *media = GRL_MEDIA (data);
  GrlSource *source = GRL_SOURCE (user_data);

  /* Check if we got a valid media object as some plugins may call the callback
     with a NULL media under certain circumstances (for example when they
     cannot estimate the number of remaining results and they find suddenly they
     don't have any more results to send) */
  if (!media) {
    g_debug ("Media element is NULL!");
    goto out;
  }

  const gchar *title = grl_media_get_title (media);

  /* If the media is a container (box), that means we will browse it again */
  if (GRL_IS_MEDIA_BOX (media)) {
    guint childcount = grl_media_box_get_childcount (GRL_MEDIA_BOX (media));
    g_debug ("\t Got '%s' (container with %d elements)", title, childcount);

    source_browser (source, media);
  } else {
    const gchar *url = grl_media_get_url (media);
    const gchar *mime = grl_media_get_mime (media);
    GDateTime *date = grl_media_get_modification_date (media);
    time_t rawdate = g_date_time_to_unix(date);
    g_printf ("\t Got '%s', of type '%s', ctime is '%s'\n", title, mime, ctime(&rawdate));
    g_printf ("\t\t URL: %s\n", url);
  }

out:
  g_object_unref (media);
}

static void
source_browser (gpointer data,
                gpointer user_data)
{
  GrlSource *source = GRL_SOURCE (data);
  GrlMedia *media = GRL_MEDIA (user_data);
  GList *media_elements;
  GError *error = NULL;
  GList *keys;
  GrlOperationOptions *options;
  GrlCaps *caps;

  keys = grl_metadata_key_list_new (GRL_METADATA_KEY_TITLE,
                                    GRL_METADATA_KEY_URL,
                                    GRL_METADATA_KEY_MODIFICATION_DATE,
                                    GRL_METADATA_KEY_MIME,
                                    GRL_METADATA_KEY_CHILDCOUNT,
                                    NULL);

  g_debug ("Detected new source available: '%s'",
	   grl_source_get_name (source));

  if (!(grl_source_supported_operations (source) & GRL_OP_BROWSE))
    goto out;

  g_debug ("Browsing source: %s", grl_source_get_name (source));
  /* Here is how you can browse a source, you have to provide:
     1) The source you want to browse contents from.
     2) The container object you want to browse (NULL for the root container)
     3) A list of metadata keys we are interested in.
     4) Options to control certain aspects of the browse operation.
     5) A callback that the framework will invoke for each available result
     6) User data for the callback
     It returns an operation identifier that you can use to match results
     with the corresponding request (we ignore it here) */

  caps = grl_source_get_caps (source, GRL_OP_BROWSE);
  options = grl_operation_options_new (caps);
  grl_operation_options_set_count (options, BROWSE_CHUNK_SIZE);
  grl_operation_options_set_flags (options, GRL_RESOLVE_IDLE_RELAY);
  media_elements = grl_source_browse_sync (GRL_SOURCE (source),
                                           media, keys,
                                           options,
                                           &error);
  if (!media_elements) {
    g_debug ("No elements found for source: %s!",
             grl_source_get_name (source));
    goto out;
  }

  if (error)
    g_error ("Failed to browse source: %s", error->message);

  g_list_foreach (media_elements, element_browser, source);

out:
  g_list_free (keys);
}

static void
load_plugins (void)
{
  GrlRegistry *registry;
  GrlSource *source;
  GError *error = NULL;

  registry = grl_registry_get_default ();

  /* Load plugin */
  if (!grl_registry_load_plugin_by_id (registry, "grl-filesystem", &error))
    g_error ("Failed to load plugin: %s", error->message);

  source = grl_registry_lookup_source (registry, "grl-filesystem");
  if (!source)
    g_error ("Unable to load grl-filesystem plugin");

  source_browser (source, NULL);
}

static void
config_plugins (gchar* chosen_test_path)
{
  GrlRegistry *registry;
  GrlConfig *config;

  registry = grl_registry_get_default ();

  /* Configure plugin */
  config = grl_config_new ("grl-filesystem", "Filesystem");
  grl_config_set_string (config, "base-path", chosen_test_path);
  grl_registry_add_config (registry, config, NULL);
}

gint
main (int     argc,
      gchar  *argv[])
{
  gchar *chosen_test_path;
  grl_init (&argc, &argv);
  GRL_LOG_DOMAIN_INIT (example_log_domain, "example");

  if (argc != 2) {
    g_printf ("Usage: %s <path to browse>\n", argv[0]);
    return 1;
  }

  chosen_test_path = argv[1];

  config_plugins (chosen_test_path);
  load_plugins ();

  return 0;
}

