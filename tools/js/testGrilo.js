//
// testGrilo.js
// Simple program to test Grilo's introspected API.
//
// Copyright (C) 2010, Igalia S.L.
//
// Author: Eduardo Lima Mitev <elima@igalia.com>
//

const Grl = imports.gi.Grilo;
const MainLoop = imports.mainloop;

function SimplePlayList () {
    this._init ();
}

SimplePlayList.prototype = {
    _init: function () {
        Grl.grl_log_init ("*:-");
        let registry = Grl.GrlPluginRegistry.get_instance ();

        let sources = [];
        this.sources = sources;

        registry.connect ("source_added",
            function (pluginRegistry, mediaSource) {
                let ops = mediaSource.supported_operations ();
                if (ops & Grl.GrlSupportedOps.SEARCH) {
	            log ("Detected new source availabe: '" +
                         mediaSource.get_name () +
                         "' and it supports search");
                    sources.push (mediaSource);
                }
            });

        registry.connect ("source_removed",
            function (pluginRegistry, mediaSource) {
                log ("source removed");
            });

        if (registry.load_all () == false) {
            log ("Failed to load plugins.");
        }
    },

    _searchCallback: function search_cb () {
        log ("yeah");
    },

    search: function (q) {
        for each (let source in this.sources) {
            log (source.get_name () + " - " + q);
            source.search (q, [Grl.GRL_METADATA_KEY_ID], 0, 10,
                           Grl.GrlMetadataResolutionFlags.FULL |
                               Grl.GrlMetadataResolutionFlags.IDLE_RELAY,
                           this._searchCallback, source);
        }
    }
};

let playList = new SimplePlayList ();

if (ARGV[0] != null) {
    playList.search (ARGV[0]);
    MainLoop.run ("main");
}
else
    log ("Query parameter missing");