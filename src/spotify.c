/*
 * Copyright (C) 2010, 2011 Thomas Jost
 *
 * This file is part of spop.
 *
 * spop is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * spop is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * spop. If not, see <http://www.gnu.org/licenses/>.
 *
 * Additional permission under GNU GPL version 3 section 7
 *
 * If you modify this Program, or any covered work, by linking or combining it
 * with libspotify (or a modified version of that library), containing parts
 * covered by the terms of the Libspotify Terms of Use, the licensors of this
 * Program grant you additional permission to convey the resulting work.
 */

#include <fcntl.h>
#include <glib.h>
#include <libspotify/api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "spop.h"
#include "config.h"
#include "plugin.h"
#include "queue.h"
#include "spotify.h"

/************************
 *** Global variables ***
 ************************/
static sp_playlistcontainer* g_container;
static gboolean g_container_loaded = FALSE;
static sp_playlist* g_starred_playlist = NULL;

static sp_session* g_session = NULL;

static unsigned int g_audio_time = 0;
static unsigned int g_audio_samples = 0;
static unsigned int g_audio_rate = 44100;

/* Session load/unload callbacks */
static GList* g_session_callbacks = NULL;
typedef struct {
    spop_session_callback_ptr func;
    gpointer user_data;
} session_callback;
typedef struct {
    session_callback_type type;
    gpointer data;
} session_callback_data;

/* Application key -- defined in appkey.c */
extern const uint8_t g_appkey[];
extern const size_t g_appkey_size;


/****************************
 *** Callbacks structures ***
 ****************************/
static sp_playlistcontainer_callbacks g_sp_container_callbacks = {
    NULL,
    NULL,
    NULL,
    &cb_container_loaded
};
static sp_session_callbacks g_sp_session_callbacks = {
    &cb_logged_in,
    &cb_logged_out,
    &cb_metadata_updated,
    &cb_connection_error,
    &cb_message_to_user,
    &cb_notify_main_thread,
    &cb_music_delivery,
    &cb_play_token_lost,
    &cb_log_message,
    &cb_end_of_track,
    NULL, /* streaming_error */
    NULL, /* userinfo_updated */
    NULL, /* start_playback */
    NULL, /* stop_playback */
    NULL  /* get_audio_buffer_stats */
};


/**********************
 *** Init functions ***
 **********************/
void session_init() {
    sp_error error;
    sp_session_config config;
    gchar* cache_path;

    g_debug("Creating session...");

    /* Cache path */
    cache_path = g_build_filename(g_get_user_cache_dir(), g_get_prgname(), NULL);

    /* libspotify session config */
    config.api_version = SPOTIFY_API_VERSION;
    config.cache_location = cache_path;
    config.settings_location = cache_path;
    config.application_key = g_appkey;
    config.application_key_size = g_appkey_size;
    config.user_agent = "spop " SPOP_VERSION;
    config.callbacks = &g_sp_session_callbacks;
    config.userdata = NULL;
    config.compress_playlists = FALSE;
    config.dont_save_metadata_for_playlists = FALSE;
    config.initially_unload_playlists = FALSE;

    error = sp_session_create(&config, &g_session);
    if (error != SP_ERROR_OK)
        g_error("Failed to create session: %s", sp_error_message(error));

    /* Set bitrate */
    if (config_get_bool_opt("high_bitrate", TRUE)) {
        g_debug("Setting preferred bitrate to high.");
        sp_session_preferred_bitrate(g_session, SP_BITRATE_320k);
    }
    else {
        g_debug("Setting preferred bitrate to low.");
        sp_session_preferred_bitrate(g_session, SP_BITRATE_160k);
    }
    if (config_get_bool_opt("offline_high_bitrate", TRUE)) {
        g_debug("Setting preferred offline bitrate to high.");
        sp_session_preferred_offline_bitrate(g_session, SP_BITRATE_320k, FALSE);
    }
    else {
        g_debug("Setting preferred offline bitrate to low.");
        sp_session_preferred_offline_bitrate(g_session, SP_BITRATE_160k, FALSE);
    }

    g_debug("Session created.");

}

void session_login(const char* username, const char* password) {
    g_debug("Logging in...");
    if (!g_session)
        g_error("Session is not ready.");

    sp_session_login(g_session, username, password, TRUE);
}
void session_logout() {
    g_debug("Logging out...");
    if (g_session)
        sp_session_logout(g_session);
}


/***************************
 *** Playlist management ***
 ***************************/
int playlists_len() {
    return sp_playlistcontainer_num_playlists(g_container) + 1; /* +1 for "starred" playlist */
}

sp_playlist* playlist_get(int nb) {
    if (nb == 0) {
        if (g_starred_playlist == NULL)
            g_starred_playlist = sp_session_starred_create(g_session);
        return g_starred_playlist;
    }
    else
        return sp_playlistcontainer_playlist(g_container, nb-1);
}

sp_playlist* playlist_get_from_link(sp_link* lnk) {
    return sp_playlist_create(g_session, lnk);
}

sp_playlist_type playlist_type(int nb) {
    if (nb == 0)
        return SP_PLAYLIST_TYPE_PLAYLIST;
    else
        return sp_playlistcontainer_playlist_type(g_container, nb-1);
}

gchar* playlist_folder_name(int nb) {
    sp_error error;
    gchar* name;

    if (nb == 0)
        name = g_strdup("Starred");
    else {
        gsize len = 512 * sizeof(gchar);
        name = g_malloc(len);
        error = sp_playlistcontainer_playlist_folder_name(g_container, nb-1, name, len);
        if (error != SP_ERROR_OK)
            g_error("Failed to get playlist folder name: %s", sp_error_message(error));
    }

    return name;
}

sp_playlist_offline_status playlist_get_offline_status(sp_playlist* pl) {
    return sp_playlist_get_offline_status(g_session, pl);
}

void playlist_set_offline_mode(sp_playlist* pl, gboolean mode) {
    sp_playlist_set_offline_mode(g_session, pl, mode);
}

int playlist_get_offline_download_completed(sp_playlist* pl) {
    return sp_playlist_get_offline_download_completed(g_session, pl);
}

/**********************
 * Session management *
 **********************/
void session_load(sp_track* track) {
    sp_error error;
    session_callback_data scbd;

    g_debug("Loading track.");
    
    error = sp_session_player_load(g_session, track);
    if (error != SP_ERROR_OK)
        g_error("Failed to load track: %s", sp_error_message(error));

    /* Queue some events management */
    cb_notify_main_thread(NULL);

    /* Then call callbacks */
    scbd.type = SPOP_SESSION_LOAD;
    scbd.data = track;
    g_list_foreach(g_session_callbacks, session_call_callback, &scbd);
}

void session_unload() {
    session_callback_data scbd;

    g_debug("Unloading track.");

    /* First call callbacks */
    scbd.type = SPOP_SESSION_UNLOAD;
    scbd.data = NULL;
    g_list_foreach(g_session_callbacks, session_call_callback, &scbd);

    /* Then really unload */
    sp_session_player_play(g_session, FALSE);
    g_audio_delivery_func(NULL, NULL, 0);
    sp_session_player_unload(g_session);
    cb_notify_main_thread(NULL);
    g_audio_samples = 0;
    g_audio_time = 0;
}

void session_play(gboolean play) {
    sp_session_player_play(g_session, play);

    if (!play)
        /* Force pause in the audio plugin */
        g_audio_delivery_func(NULL, NULL, 0);

    cb_notify_main_thread(NULL);
}

void session_seek(int pos) {
    sp_session_player_seek(g_session, pos*1000);
    g_audio_time = pos;
    g_audio_samples = 0;

    cb_notify_main_thread(NULL);
}

int session_play_time() {
    return g_audio_time + (g_audio_samples / g_audio_rate);
}

void session_get_offline_sync_status(sp_offline_sync_status* status, gboolean* sync_in_progress,
                                     int* tracks_to_sync, int* num_playlists, int* time_left) {
    if (status || sync_in_progress) {
        sp_offline_sync_status oss;
        gboolean sip = sp_offline_sync_get_status(g_session, &oss);
        if (status)
            *status = oss;
        if (sync_in_progress)
            *sync_in_progress = sip;
    }
    if (tracks_to_sync)
        *tracks_to_sync = sp_offline_tracks_to_sync(g_session);
    if (num_playlists)
        *num_playlists = sp_offline_num_playlists(g_session);
    if (time_left)
        *time_left = sp_offline_time_left(g_session);
}

/********************************
 * Session callbacks management *
 ********************************/
void session_call_callback(gpointer data, gpointer user_data) {
    session_callback* scb = (session_callback*) data;
    session_callback_data* scbd = (session_callback_data*) user_data;

    scb->func(scbd->type, scbd->data, scb->user_data);
}

gboolean session_add_callback(spop_session_callback_ptr func, gpointer user_data) {
    session_callback* scb;
    GList* cur;

    /* Is there already such a callback/data couple in the list? */
    cur = g_session_callbacks;
    while (cur != NULL) {
        scb = cur->data;
        if ((scb->func == func) && (scb->user_data == user_data))
            return FALSE;
        cur = cur->next;
    }

    /* Callback/data not in the list: add them */
    scb = g_malloc(sizeof(session_callback));
    scb->func = func;
    scb->user_data = user_data;
    g_session_callbacks = g_list_prepend(g_session_callbacks, scb);
    return TRUE;
}


/*********************
 * Tracks management *
 *********************/
GArray* tracks_get_playlist(sp_playlist* pl) {
    GArray* tracks;
    sp_track* tr;
    int i, n;

    if (!sp_playlist_is_loaded(pl))
        return NULL;

    n = sp_playlist_num_tracks(pl);
    tracks = g_array_sized_new(FALSE, FALSE, sizeof(sp_track*), n);
    if (!tracks)
        g_error("Can't allocate array of %d tracks.", n);

    for (i=0; i < n; i++) {
        tr = sp_playlist_track(pl, i);
        sp_track_add_ref(tr);
        g_array_append_val(tracks, tr);
    }

    return tracks;
}

void track_get_data(sp_track* track, gchar** name, gchar** artist, gchar** album, gchar** link, int* duration) {
    sp_artist** art = NULL;
    sp_album* alb = NULL;
    sp_link* lnk;
    int i;
    int nb_art = 0;
    const char* s;

    sp_track_add_ref(track);
    if (!sp_track_is_loaded(track)) {
        sp_track_release(track);
        return;
    }

    /* Begin loading everything */
    if (name) {
        *name = g_strdup(sp_track_name(track));
    }
    if (artist) {
        nb_art = sp_track_num_artists(track);
        art = (sp_artist**) malloc(nb_art * sizeof(sp_artist*));
        if (!art)
            g_error("Can't allocate memory.");

        for (i=0; i < nb_art; i++) {
            art[i] = sp_track_artist(track, i);
            sp_artist_add_ref(art[i]);
        }
    }
    if (album) {
        alb = sp_track_album(track);
        sp_album_add_ref(alb);
    }
    if (link) {
        GString* tmp;
        lnk = sp_link_create_from_track(track, 0);
        if (!lnk)
            g_error("Can't get URI from track.");

        tmp = g_string_sized_new(1024);
        if (sp_link_as_string(lnk, tmp->str, 1024) < 0)
            g_error("Can't render URI from link.");
        *link = tmp->str;
        g_string_free(tmp, FALSE);

        sp_link_release(lnk);
    }
    if (duration) {
        *duration = sp_track_duration(track) / 1000;
    }

    /* Now create destination strings */
    if (artist) {
        GString* tmp = g_string_new("");
        for (i=0; i < nb_art; i++) {
            if (sp_artist_is_loaded(art[i]))
                s = sp_artist_name(art[i]);
            else
                s = "[artist not loaded]";

            if (i != 0)
                g_string_append(tmp, ", ");
            g_string_append(tmp, s);
            sp_artist_release(art[i]);
        }
        *artist = tmp->str;
        g_string_free(tmp, FALSE);
    }
    if (album) {
        if (sp_album_is_loaded(alb))
            *album = g_strdup(sp_album_name(alb));
        else
            *album = g_strdup("[album not loaded]");
        sp_album_release(alb);
    }

    sp_track_release(track);
}

gboolean track_available(sp_track* track) {
    return sp_track_is_available(g_session, track);
}

sp_image* track_get_image(sp_track* track) {
    sp_album* alb = NULL;
    sp_image* img = NULL;
    const void* img_id = NULL;

    /* Get album */
    alb = sp_track_album(track);
    if (!alb)
        g_error("Can't get track album.");
    sp_album_add_ref(alb);

    if (!sp_album_is_loaded(alb))
        g_error("Album not loaded.");

    /* Get image */
    img_id = sp_album_cover(alb);
    if (!img_id) {
        /* Since the album is loaded, a NULL here indicates that there is no
           cover for this album. */
        sp_album_release(alb);
        return NULL;
    }

    img = sp_image_create(g_session, img_id);
    sp_album_release(alb);

    if (!img)
        g_error("Can't create image.");
    return img;
}

gboolean track_get_image_data(sp_track* track, gpointer* data, gsize* len) {
    sp_image* img;
    const guchar* img_data = NULL;

    img = track_get_image(track);
    if (!img) {
        /* No cover */
        *data = NULL;
        *len = 0;
        return TRUE;
    }

    if (!sp_image_is_loaded(img))
        return FALSE;

    img_data = sp_image_data(img, len);
    if (!img_data)
        g_error("Can't read image data");

    *data = g_memdup(img_data, *len);
    sp_image_release(img);
    return TRUE;
}


/****************
 *** Browsing ***
 ****************/
sp_albumbrowse* albumbrowse_create(sp_album* album, albumbrowse_complete_cb* callback, gpointer userdata) {
    return sp_albumbrowse_create(g_session, album, callback, userdata);
}

sp_artistbrowse* artistbrowse_create(sp_artist* artist, artistbrowse_complete_cb* callback, gpointer userdata) {
    return sp_artistbrowse_create(g_session, artist, callback, userdata);
}

sp_search* search_create(const gchar* query, search_complete_cb* callback, gpointer userdata) {
    int nb_results = config_get_int_opt("search_results", 100);
    return sp_search_create(g_session, query,
                            0, nb_results, 0, nb_results, 0, nb_results,
                            callback, userdata);
}

/*************************
 *** Utility functions ***
 *************************/
gboolean container_loaded() {
    return g_container_loaded;
}


/*************************
 *** Events management ***
 *************************/
gboolean session_libspotify_event(gpointer data) {
    static guint evid = 0;
    int timeout;

    if (evid > 0)
        g_source_remove(evid);

    do {
        sp_session_process_events(g_session, &timeout);
    } while (timeout <= 1);

    /* Add next timeout */
    evid = g_timeout_add(timeout, session_libspotify_event, NULL);

    return FALSE;
}
gboolean session_next_track_event(gpointer data) {
    g_debug("Got next_track event.");
    queue_next(TRUE);

    return FALSE;
}


/******************************************
 *** Callbacks, not to be used directly ***
 ******************************************/
void cb_container_loaded(sp_playlistcontainer* pc, void* data) {
    g_debug("Container loaded.");
    g_container_loaded = TRUE;
}

void cb_logged_in(sp_session* session, sp_error error) {
    if (error != SP_ERROR_OK)
        g_warning("Login failed: %s", sp_error_message(error));
    else g_info("Logged in.");

    /* Get the playlists container */
    g_debug("Getting playlist container...");
    g_container = sp_session_playlistcontainer(g_session);
    if (!g_container)
        g_error("Could not get the playlist container.");

    /* Callback to be able to wait until it is loaded */
    sp_playlistcontainer_add_callbacks(g_container, &g_sp_container_callbacks, NULL);

    g_debug("Playlist container ready.");
}

void cb_logged_out(sp_session* session) {
    g_info("Logged out.");
}
void cb_metadata_updated(sp_session* session) {
}

void cb_connection_error(sp_session* session, sp_error error) {
    g_warning("Connection error: %s\n", sp_error_message(error));
}
void cb_message_to_user(sp_session* session, const char* message) {
    g_message("%s", message);
}
void cb_notify_main_thread(sp_session* session) {
    g_idle_add_full(G_PRIORITY_DEFAULT, session_libspotify_event, NULL, NULL);
}
int cb_music_delivery(sp_session* session, const sp_audioformat* format, const void* frames, int num_frames) {
    int n =  g_audio_delivery_func(format, frames, num_frames);

    if (format->sample_rate == g_audio_rate) {
        g_audio_samples += n;
    }
    else if (n > 0) {
        g_audio_time += g_audio_samples / g_audio_rate;
        g_audio_samples = n;
        g_audio_rate = format->sample_rate;
    }

    return n;
}
void cb_play_token_lost(sp_session* session) {
    g_warning("Play token lost.");
}
void cb_log_message(sp_session* session, const char* data) {
    gchar* c = g_strrstr(data, "\n");
    if (c)
        *c = '\0';
    g_log_libspotify("%s", data);
}
void cb_end_of_track(sp_session* session) {
    g_debug("End of track.");
    g_idle_add_full(G_PRIORITY_DEFAULT, session_next_track_event, NULL, NULL);
}
