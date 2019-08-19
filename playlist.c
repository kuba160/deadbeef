/*
  This file is part of Deadbeef Player source code
  http://deadbeef.sourceforge.net

  playlist management

  Copyright (C) 2009-2013 Alexey Yakovenko

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Alexey Yakovenko waker@users.sourceforge.net
*/
#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif
#ifdef HAVE_ALLOCA_H
#  include <alloca.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fnmatch.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>
#include <sys/time.h>
#ifndef __linux__
#define _POSIX_C_SOURCE 1
#endif
#include <limits.h>
#include <errno.h>
#include <math.h>
#include "gettext.h"
#include "playlist.h"
#include "streamer.h"
#include "messagepump.h"
#include "plugins.h"
#include "junklib.h"
#include "vfs.h"
#include "conf.h"
#include "utf8.h"
#include "common.h"
#include "threading.h"
#include "metacache.h"
#include "volume.h"
#include "pltmeta.h"
#include "escape.h"
#include "strdupa.h"
#include "tf.h"
#include "playqueue.h"

#include "cueutil.h"

// disable custom title function, until we have new title formatting (0.7)
#define DISABLE_CUSTOM_TITLE

#define DISABLE_LOCKING 0
#define DEBUG_LOCKING 0
#define DETECT_PL_LOCK_RC 0

#if DETECT_PL_LOCK_RC
#include <pthread.h>
#endif

// file format revision history
// 1.1->1.2 changelog:
//    added flags field
// 1.0->1.1 changelog:
//    added sample-accurate seek positions for sub-tracks
// 1.1->1.2 changelog:
//    added flags field
// 1.2->1.3 changelog:
//    removed legacy data used for compat with 0.4.4
//    note: ddb-0.5.0 should keep using 1.2 playlist format
//    1.3 support is designed for transition to ddb-0.6.0
#define PLAYLIST_MAJOR_VER 1
#define PLAYLIST_MINOR_VER 2

#if (PLAYLIST_MINOR_VER<2)
#error writing playlists in format <1.2 is not supported
#endif

#define min(x,y) ((x)<(y)?(x):(y))

static int playlists_count = 0;
static playlist_t *playlists_head = NULL;
static playlist_t *playlist = NULL; // current playlist
static int plt_loading = 0; // disable sending event about playlist switch, config regen, etc

#if !DISABLE_LOCKING
static uintptr_t mutex;
#endif

#define LOCK {pl_lock();}
#define UNLOCK {pl_unlock();}

// used at startup to prevent crashes
static playlist_t dummy_playlist = {
    .refc = 1
};

static int pl_order = -1; // mirrors "playback.order" config variable

static int no_remove_notify;

static playlist_t *addfiles_playlist; // current playlist for adding files/folders; set in pl_add_files_begin

int conf_cue_prefer_embedded = 0;

typedef struct ddb_fileadd_listener_s {
    int id;
    int (*callback)(ddb_fileadd_data_t *data, void *user_data);
    void *user_data;
    struct ddb_fileadd_listener_s *next;
} ddb_fileadd_listener_t;

typedef struct ddb_fileadd_filter_s {
    int id;
    int (*callback)(ddb_file_found_data_t *data, void *user_data);
    void *user_data;
    struct ddb_fileadd_filter_s *next;
} ddb_fileadd_filter_t;

static ddb_fileadd_listener_t *file_add_listeners;
static ddb_fileadd_filter_t *file_add_filters;

typedef struct ddb_fileadd_beginend_listener_s {
    int id;
    void (*callback_begin)(ddb_fileadd_data_t *data, void *user_data);
    void (*callback_end)(ddb_fileadd_data_t *data, void *user_data);
    void *user_data;
    struct ddb_fileadd_beginend_listener_s *next;
} ddb_fileadd_beginend_listener_t;

static ddb_fileadd_beginend_listener_t *file_add_beginend_listeners;

void
pl_set_order (int order) {
    int prev_order = pl_order;

    if (pl_order != order) {
        pl_order = order;
        for (playlist_t *plt = playlists_head; plt; plt = plt->next) {
            plt_reshuffle (plt, NULL, NULL);
        }
    }

    streamer_notify_order_changed (prev_order, pl_order);
}

int
pl_get_order (void) {
    return pl_order;
}

int
pl_init (void) {
    if (playlist) {
        return 0; // avoid double init
    }
    playlist = &dummy_playlist;
#if !DISABLE_LOCKING
    mutex = mutex_create ();
#endif
    return 0;
}

void
pl_free (void) {
    LOCK;
    playqueue_clear ();
    plt_loading = 1;
    while (playlists_head) {

        for (playItem_t *it = playlists_head->head[PL_MAIN]; it; it = it->next[PL_MAIN]) {
            if (it->_refc > 1) {
                fprintf (stderr, "\033[0;31mWARNING: playitem %p %s(%s) has refc=%d at delete time\033[37;0m\n", it, pl_find_meta_raw (it, ":URI"), pl_find_meta_raw (it, "track"), it->_refc);
            }
        }

        //fprintf (stderr, "\033[0;31mplt refc %d\033[37;0m\n", playlists_head->refc);

        plt_remove (0);
    }
    plt_loading = 0;
    UNLOCK;
#if !DISABLE_LOCKING
    if (mutex) {
        mutex_free (mutex);
        mutex = 0;
    }
#endif
    playlist = NULL;
}

#if DEBUG_LOCKING
volatile int pl_lock_cnt = 0;
#endif
#if DETECT_PL_LOCK_RC
static pthread_t tids[1000];
static int ntids = 0;
pthread_t pl_lock_tid = 0;
#endif
void
pl_lock (void) {
#if !DISABLE_LOCKING
    mutex_lock (mutex);
#if DETECT_PL_LOCK_RC
    pl_lock_tid = pthread_self ();
    tids[ntids++] = pl_lock_tid;
#endif

#if DEBUG_LOCKING
    pl_lock_cnt++;
    printf ("pcnt: %d\n", pl_lock_cnt);
#endif
#endif
}

void
pl_unlock (void) {
#if !DISABLE_LOCKING
#if DETECT_PL_LOCK_RC
    if (ntids > 0) {
        ntids--;
    }
    if (ntids > 0) {
        pl_lock_tid = tids[ntids-1];
    }
    else {
        pl_lock_tid = 0;
    }
#endif
    mutex_unlock (mutex);
#if DEBUG_LOCKING
    pl_lock_cnt--;
    printf ("pcnt: %d\n", pl_lock_cnt);
#endif
#endif
}

static void
pl_item_free (playItem_t *it);

static void
plt_gen_conf (void) {
    if (plt_loading) {
        return;
    }
    LOCK;
    int cnt = plt_get_count ();
    int i;
    conf_remove_items ("playlist.tab.");

    playlist_t *p = playlists_head;
    for (i = 0; i < cnt; i++, p = p->next) {
        char s[100];
        snprintf (s, sizeof (s), "playlist.tab.%05d", i);
        conf_set_str (s, p->title);
        snprintf (s, sizeof (s), "playlist.cursor.%d", i);
        conf_set_int (s, p->current_row[PL_MAIN]);
        snprintf (s, sizeof (s), "playlist.scroll.%d", i);
        conf_set_int (s, p->scroll);
    }
    UNLOCK;
}

playlist_t *
plt_get_list (void) {
    return playlists_head;
}

playlist_t *
plt_get_curr (void) {
    LOCK;
    if (!playlist) {
        playlist = playlists_head;
    }
    playlist_t *plt = playlist;
    if (plt) {
        plt_ref (plt);
        assert (plt->refc > 1);
    }
    UNLOCK;
    return plt;
}

playlist_t *
plt_get_for_idx (int idx) {
    LOCK;
    playlist_t *p = playlists_head;
    for (int i = 0; p && i <= idx; i++, p = p->next) {
        if (i == idx) {
            plt_ref (p);
            UNLOCK;
            return p;
        }
    }
    UNLOCK;
    return NULL;
}

void
plt_ref (playlist_t *plt) {
    LOCK;
    plt->refc++;
    UNLOCK;
}

void
plt_unref (playlist_t *plt) {
    LOCK;
    assert (plt->refc > 0);
    plt->refc--;
    if (plt->refc < 0) {
        trace ("\033[0;31mplaylist: bad refcount on playlist %p (%s)\033[37;0m\n", plt, plt->title);
    }
    if (plt->refc <= 0) {
        plt_free (plt);
    }
    UNLOCK;
}

int
plt_get_count (void) {
    return playlists_count;
}

playItem_t *
plt_get_head (int plt) {
    playlist_t *p = playlists_head;
    for (int i = 0; p && i <= plt; i++, p = p->next) {
        if (i == plt) {
            if (p->head[PL_MAIN]) {
                pl_item_ref (p->head[PL_MAIN]);
            }
            return p->head[PL_MAIN];
        }
    }
    return NULL;
}

int
plt_get_sel_count (int plt) {
    playlist_t *p = playlists_head;
    for (int i = 0; p && i <= plt; i++, p = p->next) {
        if (i == plt) {
            return plt_getselcount (p);
        }
    }
    return 0;
}

playlist_t *
plt_alloc (const char *title) {
    playlist_t *plt = malloc (sizeof (playlist_t));
    memset (plt, 0, sizeof (playlist_t));
    plt->refc = 1;
    plt->title = strdup (title);
    return plt;
}

int
plt_add (int before, const char *title) {
    assert (before >= 0);
    playlist_t *plt = plt_alloc (title);
    plt_modified (plt);

    LOCK;
    playlist_t *p_before = NULL;
    playlist_t *p_after = playlists_head;

    int i;
    for (i = 0; i < before; i++) {
        if (i >= before+1) {
            break;
        }
        p_before = p_after;
        if (p_after) {
            p_after = p_after->next;
        }
        else {
            p_after = playlists_head;
        }
    }

    if (p_before) {
        p_before->next = plt;
    }
    else {
        playlists_head = plt;
    }
    plt->next = p_after;
    playlists_count++;

    if (!playlist) {
        playlist = plt;
        if (!plt_loading) {
            // shift files
            for (int i = playlists_count-1; i >= before+1; i--) {
                char path1[PATH_MAX];
                char path2[PATH_MAX];
                if (snprintf (path1, sizeof (path1), "%s/playlists/%d.dbpl", dbconfdir, i) > sizeof (path1)) {
                    fprintf (stderr, "error: failed to make path string for playlist file\n");
                    continue;
                }
                if (snprintf (path2, sizeof (path2), "%s/playlists/%d.dbpl", dbconfdir, i+1) > sizeof (path2)) {
                    fprintf (stderr, "error: failed to make path string for playlist file\n");
                    continue;
                }
                int err = rename (path1, path2);
                if (err != 0) {
                    fprintf (stderr, "playlist rename failed: %s\n", strerror (errno));
                }
            }
        }
    }
    UNLOCK;

    plt_gen_conf ();
    if (!plt_loading) {
        plt_save_n (before);
        conf_save ();
        messagepump_push (DB_EV_PLAYLISTSWITCHED, 0, 0, 0);
        messagepump_push (DB_EV_PLAYLISTCHANGED, 0, DDB_PLAYLIST_CHANGE_CREATED, 0);
    }
    return before;
}

// NOTE: caller must ensure that configuration is saved after that call
void
plt_remove (int plt) {
    int i;
    assert (plt >= 0 && plt < playlists_count);
    LOCK;

    // find playlist and notify streamer
    playlist_t *p = playlists_head;
    playlist_t *prev = NULL;
    for (i = 0; p && i < plt; i++) {
        prev = p;
        p = p->next;
    }

    if (!p) {
        UNLOCK;
        return;
    }

    streamer_notify_playlist_deleted (p);
    if (!plt_loading) {
        // move files (will decrease number of files by 1)
        for (int i = plt+1; i < playlists_count; i++) {
            char path1[PATH_MAX];
            char path2[PATH_MAX];
            if (snprintf (path1, sizeof (path1), "%s/playlists/%d.dbpl", dbconfdir, i-1) > sizeof (path1)) {
                fprintf (stderr, "error: failed to make path string for playlist file\n");
                continue;
            }
            if (snprintf (path2, sizeof (path2), "%s/playlists/%d.dbpl", dbconfdir, i) > sizeof (path2)) {
                fprintf (stderr, "error: failed to make path string for playlist file\n");
                continue;
            }
            int err = rename (path2, path1);
            if (err != 0) {
                fprintf (stderr, "playlist rename failed: %s\n", strerror (errno));
            }
        }
    }

    if (!plt_loading && (playlists_head && !playlists_head->next)) {
        pl_clear ();
        free (playlist->title);
        playlist->title = strdup (_("Default"));
        UNLOCK;
        plt_gen_conf ();
        conf_save ();
        plt_save_n (0);
        messagepump_push (DB_EV_PLAYLISTSWITCHED, 0, 0, 0);
        messagepump_push (DB_EV_PLAYLISTCHANGED, 0, DDB_PLAYLIST_CHANGE_DELETED, 0);
        return;
    }
    if (i != plt) {
        trace ("plt_remove %d failed\n", i);
    }
    if (p) {
        if (!prev) {
            playlists_head = p->next;
        }
        else {
            prev->next = p->next;
        }
    }
    playlist_t *next = p->next;
    playlist_t *old = playlist;
    playlist = p;
    playlist = old;
    if (p == playlist) {
        if (next) {
            playlist = next;
        }
        else {
            playlist = prev ? prev : playlists_head;
        }
    }

    plt_unref (p);
    playlists_count--;
    UNLOCK;

    plt_gen_conf ();
    conf_save ();
    if (!plt_loading) {
        messagepump_push (DB_EV_PLAYLISTSWITCHED, 0, 0, 0);
        messagepump_push (DB_EV_PLAYLISTCHANGED, 0, DDB_PLAYLIST_CHANGE_DELETED, 0);
   }
}

int
plt_find (const char *name) {
    playlist_t *p = playlists_head;
    int i = -1;
    for (i = 0; p; i++, p = p->next) {
        if (!strcmp (p->title, name)) {
            return i;
        }
    }
    return -1;
}


void
plt_set_curr (playlist_t *plt) {
    LOCK;
    if (plt != playlist) {
        playlist = plt ? plt : &dummy_playlist;
        if (!plt_loading) {
            messagepump_push (DB_EV_PLAYLISTSWITCHED, 0, 0, 0);
            conf_set_int ("playlist.current", plt_get_curr_idx ());
            conf_save ();
        }
    }
    UNLOCK;
}

void
plt_set_curr_idx (int plt) {
    int i;
    LOCK;
    playlist_t *p = playlists_head;
    for (i = 0; p && p->next && i < plt; i++) {
        p = p->next;
    }
    if (p != playlist) {
        playlist = p;
        if (!plt_loading) {
            messagepump_push (DB_EV_PLAYLISTSWITCHED, 0, 0, 0);
            conf_set_int ("playlist.current", plt);
            conf_save ();
        }
    }
    UNLOCK;
}

int
plt_get_curr_idx(void) {
    int i;
    LOCK;
    playlist_t *p = playlists_head;
    for (i = 0; p && i < playlists_count; i++) {
        if (p == playlist) {
            UNLOCK;
            return i;
        }
        p = p->next;
    }
    UNLOCK;
    return -1;
}

int
plt_get_idx_of (playlist_t *plt) {
    int i;
    LOCK;
    playlist_t *p = playlists_head;
    for (i = 0; p && i < playlists_count; i++) {
        if (p == plt) {
            UNLOCK;
            return i;
        }
        p = p->next;
    }
    UNLOCK;
    return -1;
}

int
plt_get_title (playlist_t *p, char *buffer, int bufsize) {
    LOCK;
    if (!buffer) {
        int l = (int)strlen (p->title);
        UNLOCK;
        return l;
    }
    strncpy (buffer, p->title, bufsize);
    buffer[bufsize-1] = 0;
    UNLOCK;
    return 0;
}

int
plt_set_title (playlist_t *p, const char *title) {
    LOCK;
    free (p->title);
    p->title = strdup (title);
    plt_gen_conf ();
    UNLOCK;
    conf_save ();
    if (!plt_loading) {
        messagepump_push (DB_EV_PLAYLISTCHANGED, 0, DDB_PLAYLIST_CHANGE_TITLE, 0);
    }
    return 0;
}

void
plt_modified (playlist_t *plt) {
    pl_lock ();
    plt->modification_idx++;
    pl_unlock ();
}

int
plt_get_modification_idx (playlist_t *plt) {
    pl_lock ();
    int idx = plt->modification_idx;
    pl_unlock ();
    return idx;
}

void
plt_free (playlist_t *plt) {
    LOCK;

    plt_clear (plt);

    if (plt->title) {
        free (plt->title);
    }

    while (plt->meta) {
        DB_metaInfo_t *m = plt->meta;
        plt->meta = m->next;
        metacache_remove_string (m->key);
        metacache_remove_string (m->value);
        free (m);
    }

    free (plt);
    UNLOCK;
}

void
plt_move (int from, int to) {
    if (from == to) {
        return;
    }
    int i;
    LOCK;
    playlist_t *p = playlists_head;

    playlist_t *pfrom = NULL;
    playlist_t *prev = NULL;

    // move 'from' to temp file
    char path1[PATH_MAX];
    char temp[PATH_MAX];
    if (snprintf (path1, sizeof (path1), "%s/playlists/%d.dbpl", dbconfdir, from) > sizeof (path1)) {
        fprintf (stderr, "error: failed to make path string for playlist file\n");
        UNLOCK;
        return;
    }
    if (snprintf (temp, sizeof (temp), "%s/playlists/temp.dbpl", dbconfdir) > sizeof (temp)) {
        fprintf (stderr, "error: failed to make path string for playlist file\n");
        UNLOCK;
        return;
    }

    struct stat st;
    int err = stat (path1, &st);
    if (!err) {
        int err = rename (path1, temp);
        if (err != 0) {
            fprintf (stderr, "playlist rename %s->%s failed: %s\n", path1, temp, strerror (errno));
            UNLOCK;
            return;
        }
    }

    // remove 'from' from list
    for (i = 0; p && i <= from; i++) {
        if (i == from) {
            pfrom = p;
            if (prev) {
                prev->next = p->next;
            }
            else {
                playlists_head = p->next;
            }
            break;
        }
        prev = p;
        p = p->next;
    }

    if (!pfrom) {
        UNLOCK;
        return;
    }

    // shift files to fill the gap
    for (int i = from; i < playlists_count-1; i++) {
        char path2[PATH_MAX];
        if (snprintf (path1, sizeof (path1), "%s/playlists/%d.dbpl", dbconfdir, i) > sizeof (path1)) {
            fprintf (stderr, "error: failed to make path string for playlist file\n");
            continue;
        }
        if (snprintf (path2, sizeof (path2), "%s/playlists/%d.dbpl", dbconfdir, i+1) > sizeof (path2)) {
            fprintf (stderr, "error: failed to make path string for playlist file\n");
            continue;
        }
        int err = stat (path2, &st);
        if (!err) {
            int err = rename (path2, path1);
            if (err != 0) {
                fprintf (stderr, "playlist rename %s->%s failed: %s\n", path2, path1, strerror (errno));
            }
        }
    }
    // open new gap
    for (int i = playlists_count-2; i >= to; i--) {
        char path2[PATH_MAX];
        if (snprintf (path1, sizeof (path1), "%s/playlists/%d.dbpl", dbconfdir, i) > sizeof (path1)) {
            fprintf (stderr, "error: failed to make path string for playlist file\n");
            continue;
        }
        if (snprintf (path2, sizeof (path2), "%s/playlists/%d.dbpl", dbconfdir, i+1) > sizeof (path2)) {
            fprintf (stderr, "error: failed to make path string for playlist file\n");
            continue;
        }
        int err = stat (path1, &st);
        if (!err) {
            int err = rename (path1, path2);
            if (err != 0) {
                fprintf (stderr, "playlist rename %s->%s failed: %s\n", path1, path2, strerror (errno));
            }
        }
    }
    // move temp file
    if (snprintf (path1, sizeof (path1), "%s/playlists/%d.dbpl", dbconfdir, to) > sizeof (path1)) {
        fprintf (stderr, "error: failed to make path string for playlist file\n");
    }
    else {
        int err = stat (temp, &st);
        if (!err) {
            int err = rename (temp, path1);
            if (err != 0) {
                fprintf (stderr, "playlist rename %s->%s failed: %s\n", temp, path1, strerror (errno));
            }
        }
    }

    // move pointers
    if (to == 0) {
        // insert 'from' as head
        pfrom->next = playlists_head;
        playlists_head = pfrom;
    }
    else {
        // insert 'from' in the middle
        p = playlists_head;
        for (i = 0; p && i < to; i++) {
            if (i == to-1) {
                playlist_t *next = p->next;
                p->next = pfrom;
                pfrom->next = next;
                break;
            }
            p = p->next;
        }
    }

    UNLOCK;
    plt_gen_conf ();
    conf_save ();
}

void
plt_clear (playlist_t *plt) {
    pl_lock ();
    while (plt->head[PL_MAIN]) {
        plt_remove_item (plt, plt->head[PL_MAIN]);
    }
    plt->current_row[PL_MAIN] = -1;
    plt->current_row[PL_SEARCH] = -1;
    plt_modified (plt);
    pl_unlock ();
}

void
pl_clear (void) {
    LOCK;
    plt_clear (playlist);
    UNLOCK;
}


playItem_t * /* insert internal cuesheet - called by plugins */
plt_insert_cue_from_buffer (playlist_t *playlist, playItem_t *after, playItem_t *origin, const uint8_t *buffer, int buffersize, int numsamples, int samplerate) {
    if (playlist->loading_cue) {
        // means it was called from _load_cue
        playlist->cue_numsamples = numsamples;
        playlist->cue_samplerate = samplerate;
    }
    return NULL;
}

playItem_t * /* insert external cuesheet - called by plugins */
plt_insert_cue (playlist_t *plt, playItem_t *after, playItem_t *origin, int numsamples, int samplerate) {
    if (plt->loading_cue) {
        // means it was called from _load_cue
        plt->cue_numsamples = numsamples;
        plt->cue_samplerate = samplerate;
    }
    return NULL;
}

static playItem_t *
plt_insert_dir_int (int visibility, playlist_t *playlist, DB_vfs_t *vfs, playItem_t *after, const char *dirname, int *pabort, int (*cb)(playItem_t *it, void *data), void *user_data);

static playItem_t *
plt_load_int (int visibility, playlist_t *plt, playItem_t *after, const char *fname, int *pabort, int (*cb)(playItem_t *it, void *data), void *user_data);

static int
fileadd_filter_test (ddb_file_found_data_t *data) {
    for (ddb_fileadd_filter_t *f = file_add_filters; f; f = f->next) {
        int res = f->callback (data, f->user_data);
        if (res < 0) {
            return res;
        }
    }
    return 0;
}

static playItem_t *
plt_insert_file_int (int visibility, playlist_t *playlist, playItem_t *after, const char *fname, int *pabort, int (*cb)(playItem_t *it, void *data), void *user_data) {
    if (!fname || !(*fname)) {
        return NULL;
    }

    // check if that is supported container format
    if (!playlist->ignore_archives) {
        DB_vfs_t **vfsplugs = plug_get_vfs_list ();
        for (int i = 0; vfsplugs[i]; i++) {
            if (vfsplugs[i]->is_container) {
                if (vfsplugs[i]->is_container (fname)) {
                    playItem_t *it = plt_insert_dir_int (visibility, playlist, vfsplugs[i], after, fname, pabort, cb, user_data);
                    if (it) {
                        return it;
                    }
                }
            }
        }
    }

    const char *fn = strrchr (fname, '/');
    if (!fn) {
        fn = fname;
    }
    else {
        fn++;
    }

    // add all possible streams as special-case:
    // set decoder to NULL, and filetype to "content"
    // streamer is responsible to determine content type on 1st access and
    // update decoder and filetype fields
    if (strncasecmp (fname, "file://", 7)) {
        const char *p = fname;
        int detect_on_access = 1;

        // check if it's URI
        for (; *p; p++) {
            if (!strncmp (p, "://", 3)) {
                break;
            }
            if (!isalpha (*p)) {
                detect_on_access = 0;
                break;
            }
        }

        if (detect_on_access) {
            // find vfs plugin
            DB_vfs_t **vfsplugs = plug_get_vfs_list ();
            for (int i = 0; vfsplugs[i]; i++) {
                if (vfsplugs[i]->get_schemes) {
                    const char **sch = vfsplugs[i]->get_schemes ();
                    int s = 0;
                    for (s = 0; sch[s]; s++) {
                        if (!strncasecmp (sch[s], fname, strlen (sch[s]))) {
                            break;
                        }
                    }
                    if (sch[s]) {
                        if (!vfsplugs[i]->is_streaming || !vfsplugs[i]->is_streaming()) {
                            detect_on_access = 0;
                        }
                        break;
                    }
                }
            }
        }

        if (detect_on_access && *p == ':') {
            // check for wrong chars like CR/LF, TAB, etc
            // they are not allowed and might lead to corrupt display in GUI
            int32_t i = 0;
            while (fname[i]) {
                uint32_t c = u8_nextchar (fname, &i);
                if (c < 0x20) {
                    return NULL;
                }
            }

            playItem_t *it = pl_item_alloc_init (fname, NULL);
            pl_replace_meta (it, ":FILETYPE", "content");
            after = plt_insert_item (addfiles_playlist ? addfiles_playlist : playlist, after, it);
            pl_item_unref (it);
            return after;
        }
    }
    else {
        char *escaped = uri_unescape (fname, (int)strlen (fname));
        if (escaped) {
            fname = strdupa (escaped);
            free (escaped);
        }
        fname += 7;
    }

    // detect decoder
    const char *eol = strrchr (fname, '.');
    if (!eol) {
        return NULL;
    }
    eol++;

    #ifdef __MINGW32__
    // replace backslashes with normal slashes
    char fname_conv[strlen(fname)+1];
    if (strchr(fname, '\\')) {
        trace ("plt_insert_file_int: backslash(es) detected: %s\n", fname);
        strcpy (fname_conv, fname);
        char *slash_p = fname_conv;
        while (slash_p = strchr(slash_p, '\\')) {
            *slash_p = '/';
            slash_p++;
        }
        fname = fname_conv;
    }
    // path should start with "X:/", not "/X:/", fixing to avoid file opening problems
    if (fname[0] == '/' && isalpha(fname[1]) && fname[2] == ':') {
        fname++;
    }
    #endif

    // handle cue files
    if (!strcasecmp (eol, "cue")) {
        playItem_t *inserted = plt_load_cue_file(playlist, after, fname, NULL, NULL, 0);
        return inserted;
    }

    int filter_done = 0;
    int file_recognized = 0;

    DB_decoder_t **decoders = plug_get_decoder_list ();
    // match by decoder
    for (int i = 0; decoders[i]; i++) {
        if (decoders[i]->exts && decoders[i]->insert) {
            const char **exts = decoders[i]->exts;
            for (int e = 0; exts[e]; e++) {
                if (!strcasecmp (exts[e], eol) || !strcmp (exts[e], "*")) {
                    if (!filter_done) {
                        ddb_file_found_data_t dt;
                        dt.filename = fname;
                        dt.plt = (ddb_playlist_t *)playlist;
                        dt.is_dir = 0;
                        if (fileadd_filter_test (&dt) < 0) {
                            return NULL;
                        }
                        filter_done = 1;
                    }

                    file_recognized = 1;

                    playItem_t *inserted = (playItem_t *)decoders[i]->insert ((ddb_playlist_t *)playlist, DB_PLAYITEM (after), fname);
                    if (inserted != NULL) {
                        if (cb && cb (inserted, user_data) < 0) {
                            *pabort = 1;
                        }
                        if (file_add_listeners) {
                            ddb_fileadd_data_t d;
                            memset (&d, 0, sizeof (d));
                            d.visibility = visibility;
                            d.plt = (ddb_playlist_t *)playlist;
                            d.track = (ddb_playItem_t *)inserted;
                            for (ddb_fileadd_listener_t *l = file_add_listeners; l; l = l->next) {
                                if (pabort && l->callback (&d, l->user_data) < 0) {
                                    *pabort = 1;
                                    break;
                                }
                            }
                        }
                        return inserted;
                    }
                }
            }
        }
        if (decoders[i]->prefixes && decoders[i]->insert) {
            const char **prefixes = decoders[i]->prefixes;
            for (int e = 0; prefixes[e]; e++) {
                if (!strncasecmp (prefixes[e], fn, strlen(prefixes[e])) && *(fn + strlen (prefixes[e])) == '.') {
                    if (!filter_done) {
                        ddb_file_found_data_t dt;
                        dt.filename = fname;
                        dt.plt = (ddb_playlist_t *)playlist;
                        dt.is_dir = 0;
                        if (fileadd_filter_test (&dt) < 0) {
                            return NULL;
                        }
                        filter_done = 1;
                    }

                    file_recognized = 1;
                    playItem_t *inserted = (playItem_t *)decoders[i]->insert ((ddb_playlist_t *)playlist, DB_PLAYITEM (after), fname);
                    if (inserted != NULL) {
                        if (cb && cb (inserted, user_data) < 0) {
                            *pabort = 1;
                        }
                        if (file_add_listeners) {
                            ddb_fileadd_data_t d;
                            memset (&d, 0, sizeof (d));
                            d.visibility = visibility;
                            d.plt = (ddb_playlist_t *)playlist;
                            d.track = (ddb_playItem_t *)inserted;
                            for (ddb_fileadd_listener_t *l = file_add_listeners; l; l = l->next) {
                                if (l->callback (&d, l->user_data) < 0) {
                                    *pabort = 1;
                                    break;
                                }
                            }
                        }
                        return inserted;
                    }
                }
            }
        }
    }
    if (file_recognized) {
        trace_err ("ERROR: could not load: %s\n", fname);
    }
    return NULL;
}

playItem_t *
plt_insert_file (playlist_t *playlist, playItem_t *after, const char *fname, int *pabort, int (*cb)(playItem_t *it, void *data), void *user_data) {
    return plt_insert_file_int (0, playlist, after, fname, pabort, cb, user_data);
}

static int dirent_alphasort (const struct dirent **a, const struct dirent **b) {
    return strcmp ((*a)->d_name, (*b)->d_name);
}

static void
_get_fullname_and_dir (char *fullname, int sz, char *dir, int dirsz, DB_vfs_t *vfs, const char *dirname, const char *d_name) {
    if (!vfs) {
        // prevent double-slashes
        char *stripped_dirname = strdupa (dirname);
        char *p = stripped_dirname + strlen(stripped_dirname) - 1;
        while (p > stripped_dirname && (*p == '/' || *p == '\\')) {
            p--;
        }
        p++;
        *p = 0;

        snprintf (fullname, sz, "%s/%s", stripped_dirname, d_name);
        if (dir) {
            *dir = 0;
            strncat (dir, stripped_dirname, dirsz - 1);
        }
    }
    else {
        const char *sch = NULL;
        if (vfs->plugin.api_vminor >= 6 && vfs->get_scheme_for_name) {
            sch = vfs->get_scheme_for_name (dirname);
        }
        if (sch && strncmp (sch, d_name, strlen (sch))) {
            snprintf (fullname, sz, "%s%s:%s", sch, dirname, d_name);
            if (dir) {
                snprintf (dir, dirsz, "%s%s:", sch, dirname);
            }
        }
        else {
            *fullname = 0;
            strncat (fullname, d_name, sz - 1);
            if (dir) {
                *dir = 0;
                strncat (dir, dirname, dirsz - 1);
            }
        }
    }

    #ifdef __MINGW32__
    if (fullname && strchr(fullname, '\\')) {
        char *slash_p = fullname;
        while (slash_p = strchr(slash_p, '\\')) {
            *slash_p = '/';
            slash_p++;
        }
        trace ("_get_fullname_and_dir backslash(es) found, converted: %s\n", fullname);
    }
    if (dir && strchr(dir, '\\')) {
        char *slash_p = dir;
        while (slash_p = strchr(slash_p, '\\')) {
            *slash_p = '/';
            slash_p++;
        }
        trace ("_get_fullname_and_dir backslash(es) found, converted: %s\n", dir);
    }
    #endif
}

static playItem_t *
plt_insert_dir_int (int visibility, playlist_t *playlist, DB_vfs_t *vfs, playItem_t *after, const char *dirname, int *pabort, int (*cb)(playItem_t *it, void *data), void *user_data) {
    if (!strncmp (dirname, "file://", 7)) {
        dirname += 7;
    }

    #ifdef __MINGW32__
    // replace backslashes with normal slashes
    char dirname_conv[strlen(dirname)+1];
    if (strchr(dirname, '\\')) {
        trace ("plt_insert_dir_int: backslash(es) detected: %s\n", dirname);
        strcpy (dirname_conv, dirname);
        char *slash_p = dirname_conv;
        while (slash_p = strchr(slash_p, '\\')) {
            *slash_p = '/';
            slash_p++;
        }
        dirname = dirname_conv;
    }
    // path should start with "X:/", not "/X:/", fixing to avoid file opening problems
    if (dirname[0] == '/' && isalpha(dirname[1]) && dirname[2] == ':') {
        dirname++;
    }
    #endif

    if (!playlist->follow_symlinks && !vfs) {
        struct stat buf;
        lstat (dirname, &buf);
        if (S_ISLNK(buf.st_mode)) {
            return NULL;
        }
    }

    ddb_file_found_data_t dt;
    dt.filename = dirname;
    dt.plt = (ddb_playlist_t *)playlist;
    dt.is_dir = 1;
    if (fileadd_filter_test (&dt) < 0) {
        return NULL;
    }

    struct dirent **namelist = NULL;
    int n;

    if (vfs && vfs->scandir) {
        n = vfs->scandir (dirname, &namelist, NULL, dirent_alphasort);
        // we can't rely on vfs plugins to set d_type
        // windows: missing dirent[]->d_type
        #ifndef __MINGW32__
        for (int i = 0; i < n; i++) {
            namelist[i]->d_type = DT_REG;
        }
        #endif
    }
    else {
        n = scandir (dirname, &namelist, NULL, dirent_alphasort);
    }
    if (n < 0) {
        if (namelist) {
            free (namelist);
        }
        return NULL;	// not a dir or no read access
    }

    // find all cue files in the folder
    int cuefiles[n];
    int ncuefiles = 0;

    for (int i = 0; i < n; i++) {
        // no hidden files
        // windows: missing dirent[]->d_type
        #ifdef __MINGW32__
        if (namelist[i]->d_name[0] == '.') {
        #else
        if (namelist[i]->d_name[0] == '.' || (namelist[i]->d_type != DT_REG && namelist[i]->d_type != DT_UNKNOWN)) {
        #endif
            continue;
        }

        size_t l = strlen (namelist[i]->d_name);
        if (l > 4 && !strcasecmp (namelist[i]->d_name + l - 4, ".cue")) {
            cuefiles[ncuefiles++] = i;
        }
    }

    char fullname[PATH_MAX];
    char fulldir[PATH_MAX];

    // try loading cuesheets first
    for (int c = 0; c < ncuefiles; c++) {
        int i = cuefiles[c];
        _get_fullname_and_dir (fullname, sizeof (fullname), fulldir, sizeof(fulldir), vfs, dirname, namelist[i]->d_name);

        playItem_t *inserted = plt_load_cue_file (playlist, after, fullname, fulldir, namelist, n);
        namelist[i]->d_name[0] = 0;

        if (inserted) {
            after = inserted;
        }
        if (pabort && *pabort) {
            break;
        }
    }

    // load the rest of the files
    if (!pabort || !*pabort) {
        for (int i = 0; i < n; i++)
        {
            // no hidden files
            if (!namelist[i]->d_name[0] || namelist[i]->d_name[0] == '.') {
                continue;
            }
            _get_fullname_and_dir (fullname, sizeof (fullname), NULL, 0, vfs, dirname, namelist[i]->d_name);
            playItem_t *inserted = NULL;
            if (!vfs) {
                inserted = plt_insert_dir_int (visibility, playlist, vfs, after, fullname, pabort, cb, user_data);
            }
            if (!inserted) {
                inserted = plt_insert_file_int (visibility, playlist, after, fullname, pabort, cb, user_data);
            }

            if (inserted) {
                after = inserted;
            }

            if (pabort && *pabort) {
                break;
            }
        }
    }

    // free data
    for (int i = 0; i < n; i++) {
        free (namelist[i]);
    }
    free (namelist);

    return after;
}

playItem_t *
plt_insert_dir (playlist_t *playlist, playItem_t *after, const char *dirname, int *pabort, int (*cb)(playItem_t *it, void *data), void *user_data) {
    int prev_sl = playlist->follow_symlinks;
    playlist->follow_symlinks = conf_get_int ("add_folders_follow_symlinks", 0);

    int prev = playlist->ignore_archives;
    playlist->ignore_archives = conf_get_int ("ignore_archives", 1);

    playItem_t *ret = plt_insert_dir_int (0, playlist, NULL, after, dirname, pabort, cb, user_data);

    playlist->follow_symlinks = prev_sl;
    playlist->ignore_archives = prev;

    return ret;
}

static int
plt_add_file_int (int visibility, playlist_t *plt, const char *fname, int (*cb)(playItem_t *it, void *data), void *user_data) {
    int abort = 0;
    playItem_t *it = plt_insert_file_int (visibility, plt, plt->tail[PL_MAIN], fname, &abort, cb, user_data);
    if (it) {
        // pl_insert_file doesn't hold reference, don't unref here
        return 0;
    }
    return -1;
}

int
plt_add_file (playlist_t *plt, const char *fname, int (*cb)(playItem_t *it, void *data), void *user_data) {
    return plt_add_file_int (0, plt, fname, cb, user_data);
}

int
plt_add_dir (playlist_t *plt, const char *dirname, int (*cb)(playItem_t *it, void *data), void *user_data) {
    int abort = 0;
    playItem_t *it = plt_insert_dir (plt, plt->tail[PL_MAIN], dirname, &abort, cb, user_data);
    if (it) {
        // pl_insert_file doesn't hold reference, don't unref here
        return 0;
    }
    return -1;
}

int
pl_add_files_begin (playlist_t *plt) {
    return plt_add_files_begin (plt, 0);
}

void
pl_add_files_end (void) {
    return plt_add_files_end (addfiles_playlist, 0);
}

int
plt_remove_item (playlist_t *playlist, playItem_t *it) {
    if (!it)
        return -1;

    if (!no_remove_notify) {
        streamer_song_removed_notify (it);
        playqueue_remove (it);
    }

    // remove from both lists
    LOCK;
    for (int iter = PL_MAIN; iter <= PL_SEARCH; iter++) {
        if (it->prev[iter] || it->next[iter] || playlist->head[iter] == it || playlist->tail[iter] == it) {
            playlist->count[iter]--;
        }

        playItem_t *next = it->next[iter];
        playItem_t *prev = it->prev[iter];

        if (prev) {
            prev->next[iter] = next;
        }
        else {
            playlist->head[iter] = next;
        }
        if (next) {
            next->prev[iter] = prev;
        }
        else {
            playlist->tail[iter] = prev;
        }

        it->next[iter] = NULL;
        it->prev[iter] = NULL;
    }

    float dur = pl_get_item_duration (it);
    if (dur > 0) {
        // totaltime
        playlist->totaltime -= dur;
        if (playlist->totaltime < 0) {
            playlist->totaltime = 0;
        }

        // selected time
        if (it->selected) {
            playlist->seltime -= dur;
            if (playlist->seltime < 0) {
                playlist->seltime = 0;
            }
        }
    }

    plt_modified (playlist);
    pl_item_unref (it);
    UNLOCK;
    return 0;
}

int
plt_get_item_count (playlist_t *plt, int iter) {
    return plt->count[iter];
}

int
pl_getcount (int iter) {
    LOCK;
    if (!playlist) {
        UNLOCK;
        return 0;
    }

    int cnt = playlist->count[iter];
    UNLOCK;
    return cnt;
}

int
plt_getselcount (playlist_t *playlist) {
    // FIXME: slow!
    int cnt = 0;
    for (playItem_t *it = playlist->head[PL_MAIN]; it; it = it->next[PL_MAIN]) {
        if (it->selected) {
            cnt++;
        }
    }
    return cnt;
}

int
pl_getselcount (void) {
    LOCK;
    int cnt = plt_getselcount (playlist);
    UNLOCK;
    return cnt;
}

playItem_t *
plt_get_item_for_idx (playlist_t *playlist, int idx, int iter) {
    LOCK;
    playItem_t *it = playlist->head[iter];
    while (idx--) {
        if (!it) {
            UNLOCK;
            return NULL;
        }
        it = it->next[iter];
    }
    if (it) {
        pl_item_ref (it);
    }
    UNLOCK;
    return it;
}

playItem_t *
pl_get_for_idx_and_iter (int idx, int iter) {
    LOCK;
    playItem_t *it = plt_get_item_for_idx (playlist, idx, iter);
    UNLOCK;
    return it;
}

playItem_t *
pl_get_for_idx (int idx) {
    return pl_get_for_idx_and_iter (idx, PL_MAIN);
}

int
plt_get_item_idx (playlist_t *playlist, playItem_t *it, int iter) {
    LOCK;
    playItem_t *c = playlist->head[iter];
    int idx = 0;
    while (c && c != it) {
        c = c->next[iter];
        idx++;
    }
    if (!c) {
        UNLOCK;
        return -1;
    }
    UNLOCK;
    return idx;
}

int
pl_get_idx_of (playItem_t *it) {
    return pl_get_idx_of_iter (it, PL_MAIN);
}

int
pl_get_idx_of_iter (playItem_t *it, int iter) {
    LOCK;
    int idx = plt_get_item_idx (playlist, it, iter);
    UNLOCK;
    return idx;
}

playItem_t *
plt_insert_item (playlist_t *playlist, playItem_t *after, playItem_t *it) {
    LOCK;
    pl_item_ref (it);
    if (!after) {
        it->next[PL_MAIN] = playlist->head[PL_MAIN];
        it->prev[PL_MAIN] = NULL;
        if (playlist->head[PL_MAIN]) {
            playlist->head[PL_MAIN]->prev[PL_MAIN] = it;
        }
        else {
            playlist->tail[PL_MAIN] = it;
        }
        playlist->head[PL_MAIN] = it;
    }
    else {
        it->prev[PL_MAIN] = after;
        it->next[PL_MAIN] = after->next[PL_MAIN];
        if (after->next[PL_MAIN]) {
            after->next[PL_MAIN]->prev[PL_MAIN] = it;
        }
        after->next[PL_MAIN] = it;
        if (after == playlist->tail[PL_MAIN]) {
            playlist->tail[PL_MAIN] = it;
        }
    }
    it->in_playlist = 1;

    playlist->count[PL_MAIN]++;

    // shuffle
    playItem_t *prev = it->prev[PL_MAIN];
    const char *aa = NULL, *prev_aa = NULL;
    if (prev) {
        aa = pl_find_meta_raw (it, "band");
        if (!aa) {
            aa = pl_find_meta_raw (it, "album artist");
        }
        if (!aa) {
            aa = pl_find_meta_raw (it, "albumartist");
        }
        prev_aa = pl_find_meta_raw (prev, "band");
        if (!prev_aa) {
            prev_aa = pl_find_meta_raw (prev, "album artist");
        }
        if (!prev_aa) {
            prev_aa = pl_find_meta_raw (prev, "albumartist");
        }
    }
    if (pl_order == PLAYBACK_ORDER_SHUFFLE_ALBUMS && prev && pl_find_meta_raw (prev, "album") == pl_find_meta_raw (it, "album") && ((aa && prev_aa && aa == prev_aa) || pl_find_meta_raw (prev, "artist") == pl_find_meta_raw (it, "artist"))) {
        it->shufflerating = prev->shufflerating;
    }
    else {
        it->shufflerating = rand ();
    }
    it->played = 0;

    // totaltime
    float dur = pl_get_item_duration (it);
    if (dur > 0) {
        playlist->totaltime += dur;
    }

    plt_modified (playlist);

    UNLOCK;
    return it;
}

playItem_t *
pl_insert_item (playItem_t *after, playItem_t *it) {
    return plt_insert_item (addfiles_playlist ? addfiles_playlist : playlist, after, it);
}

void
pl_item_copy (playItem_t *out, playItem_t *it) {
    LOCK;
    out->startsample = it->startsample;
    out->endsample = it->endsample;
    out->startsample64 = it->startsample64;
    out->endsample64 = it->endsample64;
    out->shufflerating = it->shufflerating;
    out->_duration = it->_duration;
    out->next[PL_MAIN] = it->next[PL_MAIN];
    out->prev[PL_MAIN] = it->prev[PL_MAIN];
    out->next[PL_SEARCH] = it->next[PL_SEARCH];
    out->prev[PL_SEARCH] = it->prev[PL_SEARCH];
    out->_refc = 1;

    for (DB_metaInfo_t *meta = it->meta; meta; meta = meta->next) {
        pl_add_meta_copy (out, meta);
    }
    UNLOCK;
}

playItem_t *
pl_item_alloc (void) {
    playItem_t *it = malloc (sizeof (playItem_t));
    memset (it, 0, sizeof (playItem_t));
    it->_duration = -1;
    it->_refc = 1;
    return it;
}

playItem_t *
pl_item_alloc_init (const char *fname, const char *decoder_id) {
    playItem_t *it = pl_item_alloc ();
    pl_add_meta (it, ":URI", fname);
    pl_add_meta (it, ":DECODER", decoder_id);
    return it;
}

void
pl_item_ref (playItem_t *it) {
    LOCK;
    it->_refc++;
    //fprintf (stderr, "\033[0;34m+it %p: refc=%d: %s\033[37;0m\n", it, it->_refc, pl_find_meta_raw (it, ":URI"));
    UNLOCK;
}

static void
pl_item_free (playItem_t *it) {
    LOCK;
    if (it) {
        while (it->meta) {
            pl_meta_free_values (it->meta);
            DB_metaInfo_t *m = it->meta;
            it->meta = m->next;
            free (m);
        }

        free (it);
    }
    UNLOCK;
}

void
pl_item_unref (playItem_t *it) {
    LOCK;
    it->_refc--;
    //trace ("\033[0;31m-it %p: refc=%d: %s\033[37;0m\n", it, it->_refc, pl_find_meta_raw (it, ":URI"));
    if (it->_refc < 0) {
        trace ("\033[0;31mplaylist: bad refcount on item %p\033[37;0m\n", it);
    }
    if (it->_refc <= 0) {
        //printf ("\033[0;31mdeleted %s\033[37;0m\n", pl_find_meta_raw (it, ":URI"));
        pl_item_free (it);
    }
    UNLOCK;
}

int
plt_delete_selected (playlist_t *playlist) {
    LOCK;
    int i = 0;
    int ret = -1;
    playItem_t *next = NULL;
    for (playItem_t *it = playlist->head[PL_MAIN]; it; it = next, i++) {
        next = it->next[PL_MAIN];
        if (it->selected) {
            if (ret == -1) {
                ret = i;
            }
            plt_remove_item (playlist, it);
        }
    }
    if (playlist->current_row[PL_MAIN] >= playlist->count[PL_MAIN]) {
        playlist->current_row[PL_MAIN] = playlist->count[PL_MAIN] - 1;
    }
    if (playlist->current_row[PL_SEARCH] >= playlist->count[PL_SEARCH]) {
        playlist->current_row[PL_SEARCH] = playlist->count[PL_SEARCH] - 1;
    }
    UNLOCK;
    return ret;
}

int
pl_delete_selected (void) {
    LOCK;
    int ret = plt_delete_selected (playlist);
    UNLOCK;
    return ret;
}

void
plt_crop_selected (playlist_t *playlist) {
    LOCK;
    playItem_t *next = NULL;
    for (playItem_t *it = playlist->head[PL_MAIN]; it; it = next) {
        next = it->next[PL_MAIN];
        if (!it->selected) {
            plt_remove_item (playlist, it);
        }
    }
    UNLOCK;
}

void
pl_crop_selected (void) {
    LOCK;
    plt_crop_selected (playlist);
    UNLOCK;
}

int
plt_save (playlist_t *plt, playItem_t *first, playItem_t *last, const char *fname, int *pabort, int (*cb)(playItem_t *it, void *data), void *user_data) {
    LOCK;
    plt->last_save_modification_idx = plt->last_save_modification_idx;
    const char *ext = strrchr (fname, '.');
    if (ext) {
        DB_playlist_t **plug = deadbeef->plug_get_playlist_list ();
        for (int i = 0; plug[i]; i++) {
            if (plug[i]->extensions && plug[i]->load) {
                const char **exts = plug[i]->extensions;
                if (exts && plug[i]->save) {
                    for (int e = 0; exts[e]; e++) {
                        if (!strcasecmp (exts[e], ext+1)) {
                            int res = plug[i]->save ((ddb_playlist_t *)plt, fname, (DB_playItem_t *)playlist->head[PL_MAIN], NULL);
                            UNLOCK;
                            return res;
                        }
                    }
                }
            }
        }
    }

    char tempfile[PATH_MAX];
    snprintf (tempfile, sizeof (tempfile), "%s.tmp", fname);
    const char magic[] = "DBPL";
    uint8_t majorver = PLAYLIST_MAJOR_VER;
    uint8_t minorver = PLAYLIST_MINOR_VER;
    FILE *fp = fopen (tempfile, "w+b");
    if (!fp) {
        UNLOCK;
        return -1;
    }
    if (fwrite (magic, 1, 4, fp) != 4) {
        goto save_fail;
    }
    if (fwrite (&majorver, 1, 1, fp) != 1) {
        goto save_fail;
    }
    if (fwrite (&minorver, 1, 1, fp) != 1) {
        goto save_fail;
    }
    uint32_t cnt = plt->count[PL_MAIN];
    if (fwrite (&cnt, 1, 4, fp) != 4) {
        goto save_fail;
    }
    for (playItem_t *it = plt->head[PL_MAIN]; it; it = it->next[PL_MAIN]) {
        uint16_t l;
        uint8_t ll;
        if (cb) {
            cb(it, user_data);
        }
#if (PLAYLIST_MINOR_VER==2)
        const char *fname = pl_find_meta_raw (it, ":URI");
        l = strlen (fname);
        if (fwrite (&l, 1, 2, fp) != 2) {
            goto save_fail;
        }
        if (fwrite (fname, 1, l, fp) != l) {
            goto save_fail;
        }
        const char *decoder_id = pl_find_meta_raw (it, ":DECODER");
        if (decoder_id) {
            ll = strlen (decoder_id);
            if (fwrite (&ll, 1, 1, fp) != 1) {
                goto save_fail;
            }
            if (fwrite (decoder_id, 1, ll, fp) != ll) {
                goto save_fail;
            }
        }
        else
        {
            ll = 0;
            if (fwrite (&ll, 1, 1, fp) != 1) {
                goto save_fail;
            }
        }
        l = pl_find_meta_int (it, ":TRACKNUM", 0);
        if (fwrite (&l, 1, 2, fp) != 2) {
            goto save_fail;
        }
#endif
        if (fwrite (&it->startsample, 1, 4, fp) != 4) {
            goto save_fail;
        }
        if (fwrite (&it->endsample, 1, 4, fp) != 4) {
            goto save_fail;
        }
        if (fwrite (&it->_duration, 1, 4, fp) != 4) {
            goto save_fail;
        }
#if (PLAYLIST_MINOR_VER==2)
        const char *filetype = pl_find_meta_raw (it, ":FILETYPE");
        if (!filetype) {
            filetype = "";
        }
        uint8_t ft = strlen (filetype);
        if (fwrite (&ft, 1, 1, fp) != 1) {
            goto save_fail;
        }
        if (ft) {
            if (fwrite (filetype, 1, ft, fp) != ft) {
                goto save_fail;
            }
        }
        float rg_albumgain = pl_get_item_replaygain (it, DDB_REPLAYGAIN_ALBUMGAIN);
        float rg_albumpeak = pl_get_item_replaygain (it, DDB_REPLAYGAIN_ALBUMPEAK);
        float rg_trackgain = pl_get_item_replaygain (it, DDB_REPLAYGAIN_TRACKGAIN);
        float rg_trackpeak = pl_get_item_replaygain (it, DDB_REPLAYGAIN_TRACKPEAK);
        if (fwrite (&rg_albumgain, 1, 4, fp) != 4) {
            goto save_fail;
        }
        if (fwrite (&rg_albumpeak, 1, 4, fp) != 4) {
            goto save_fail;
        }
        if (fwrite (&rg_trackgain, 1, 4, fp) != 4) {
            goto save_fail;
        }
        if (fwrite (&rg_trackpeak, 1, 4, fp) != 4) {
            goto save_fail;
        }
#endif
        if (fwrite (&it->_flags, 1, 4, fp) != 4) {
            goto save_fail;
        }

        int16_t nm = 0;
        DB_metaInfo_t *m;
        for (m = it->meta; m; m = m->next) {
            if (m->key[0] == '_' || m->key[0] == '!') {
                continue; // skip reserved names
            }
            nm++;
        }
        if (fwrite (&nm, 1, 2, fp) != 2) {
            goto save_fail;
        }
        for (m = it->meta; m; m = m->next) {
            if (m->key[0] == '_' || m->key[0] == '!') {
                continue; // skip reserved names
            }

            l = strlen (m->key);
            if (fwrite (&l, 1, 2, fp) != 2) {
                goto save_fail;
            }
            if (l) {
                if (fwrite (m->key, 1, l, fp) != l) {
                    goto save_fail;
                }
            }
            l = m->valuesize-1;
            if (fwrite (&l, 1, 2, fp) != 2) {
                goto save_fail;
            }
            if (l) {
                if (fwrite (m->value, 1, l, fp) != l) {
                    goto save_fail;
                }
            }
        }
    }

    // write playlist metadata
    int16_t nm = 0;
    DB_metaInfo_t *m;
    for (m = plt->meta; m; m = m->next) {
        nm++;
    }
    if (fwrite (&nm, 1, 2, fp) != 2) {
        goto save_fail;
    }

    for (m = plt->meta; m; m = m->next) {
        uint16_t l;
        l = strlen (m->key);
        if (fwrite (&l, 1, 2, fp) != 2) {
            goto save_fail;
        }
        if (l) {
            if (fwrite (m->key, 1, l, fp) != l) {
                goto save_fail;
            }
        }
        l = strlen (m->value);
        if (fwrite (&l, 1, 2, fp) != 2) {
            goto save_fail;
        }
        if (l) {
            if (fwrite (m->value, 1, l, fp) != l) {
                goto save_fail;
            }
        }
    }

    UNLOCK;
    fclose (fp);
    if (rename (tempfile, fname) != 0) {
        fprintf (stderr, "playlist rename %s -> %s failed: %s\n", tempfile, fname, strerror (errno));
        return -1;
    }
    return 0;
save_fail:
    UNLOCK;
    fclose (fp);
    unlink (tempfile);
    return -1;
}

int
plt_save_n (int n) {
    char path[PATH_MAX];
    if (snprintf (path, sizeof (path), "%s/playlists", dbconfdir) > sizeof (path)) {
        fprintf (stderr, "error: failed to make path string for playlists folder\n");
        return -1;
    }
    // make folder
    mkdir (path, 0755);

    LOCK;
    int err = 0;

    plt_loading = 1;
    if (snprintf (path, sizeof (path), "%s/playlists/%d.dbpl", dbconfdir, n) > sizeof (path)) {
        fprintf (stderr, "error: failed to make path string for playlist file\n");
        UNLOCK;
        return -1;
    }

    int i;
    playlist_t *plt;
    for (i = 0, plt = playlists_head; plt && i < n; i++, plt = plt->next);
    err = plt_save (plt, NULL, NULL, path, NULL, NULL, NULL);
    plt_loading = 0;
    UNLOCK;
    return err;
}

int
pl_save_current (void) {
    return plt_save_n (plt_get_curr_idx ());
}

int
pl_save_all (void) {
    char path[PATH_MAX];
    if (snprintf (path, sizeof (path), "%s/playlists", dbconfdir) > sizeof (path)) {
        fprintf (stderr, "error: failed to make path string for playlists folder\n");
        return -1;
    }
    // make folder
    mkdir (path, 0755);

    LOCK;
    playlist_t *p = playlists_head;
    int i;
    int cnt = plt_get_count ();
    int err = 0;

    plt_gen_conf ();
    plt_loading = 1;
    for (i = 0; i < cnt; i++, p = p->next) {
        if (snprintf (path, sizeof (path), "%s/playlists/%d.dbpl", dbconfdir, i) > sizeof (path)) {
            fprintf (stderr, "error: failed to make path string for playlist file\n");
            err = -1;
            break;
        }
        if (p->last_save_modification_idx == p->modification_idx) {
            continue;
        }
        err = plt_save (p, NULL, NULL, path, NULL, NULL, NULL);
        if (err < 0) {
            break;
        }
    }
    plt_loading = 0;
    UNLOCK;
    return err;
}

static playItem_t *
plt_load_int (int visibility, playlist_t *plt, playItem_t *after, const char *fname, int *pabort, int (*cb)(playItem_t *it, void *data), void *user_data) {
    // try plugins 1st
    char *escaped = uri_unescape (fname, (int)strlen (fname));
    if (escaped) {
        fname = strdupa (escaped);
        free (escaped);
    }

    const char *ext = strrchr (fname, '.');
    if (ext) {
        ext++;
        DB_playlist_t **plug = plug_get_playlist_list ();
        int p, e;
        for (p = 0; plug[p]; p++) {
            for (e = 0; plug[p]->extensions[e]; e++) {
                if (plug[p]->load && !strcasecmp (ext, plug[p]->extensions[e])) {
                    DB_playItem_t *it = NULL;
                    if (cb || (plug[p]->load && !plug[p]->load2)) {
                        it = plug[p]->load ((ddb_playlist_t *)plt, (DB_playItem_t *)after, fname, pabort, (int (*)(DB_playItem_t *, void *))cb, user_data);
                    }
                    else if (plug[p]->load2) {
                        plug[p]->load2 (visibility, (ddb_playlist_t *)plt, (DB_playItem_t *)after, fname, pabort);
                    }
                    return (playItem_t *)it;
                }
            }
        }
    }
    FILE *fp = fopen (fname, "rb");
    if (!fp) {
        trace ("plt_load: failed to open %s\n", fname);
        return NULL;
    }

    playItem_t *last_added = NULL;

    uint8_t majorver;
    uint8_t minorver;
    playItem_t *it = NULL;
    char magic[4];
    if (fread (magic, 1, 4, fp) != 4) {
        trace ("failed to read magic\n");
        goto load_fail;
    }
    if (strncmp (magic, "DBPL", 4)) {
        trace ("bad signature\n");
        goto load_fail;
    }
    if (fread (&majorver, 1, 1, fp) != 1) {
        goto load_fail;
    }
    if (majorver != PLAYLIST_MAJOR_VER) {
        trace ("bad majorver=%d\n", majorver);
        goto load_fail;
    }
    if (fread (&minorver, 1, 1, fp) != 1) {
        goto load_fail;
    }
    if (minorver < 1) {
        trace ("bad minorver=%d\n", minorver);
        goto load_fail;
    }
    uint32_t cnt;
    if (fread (&cnt, 1, 4, fp) != 4) {
        goto load_fail;
    }

    for (uint32_t i = 0; i < cnt; i++) {
        it = pl_item_alloc ();
        if (!it) {
            goto load_fail;
        }
        uint16_t l;
        int16_t tracknum = 0;
        if (minorver <= 2) {
            // fname
            if (fread (&l, 1, 2, fp) != 2) {
                goto load_fail;
            }
            char fname[l+1];
            if (fread (fname, 1, l, fp) != l) {
                goto load_fail;
            }
            fname[l] = 0;
            pl_add_meta (it, ":URI", fname);
            // decoder
            uint8_t ll;
            if (fread (&ll, 1, 1, fp) != 1) {
                goto load_fail;
            }
            if (ll >= 20) {
                goto load_fail;
            }
            char decoder_id[20] = "";
            if (ll) {
                if (fread (decoder_id, 1, ll, fp) != ll) {
                    goto load_fail;
                }
                decoder_id[ll] = 0;
                pl_add_meta (it, ":DECODER", decoder_id);
            }
            // tracknum
            if (fread (&tracknum, 1, 2, fp) != 2) {
                goto load_fail;
            }
            pl_set_meta_int (it, ":TRACKNUM", tracknum);
        }
        // startsample
        if (fread (&it->startsample, 1, 4, fp) != 4) {
            goto load_fail;
        }
        // endsample
        if (fread (&it->endsample, 1, 4, fp) != 4) {
            goto load_fail;
        }
        // duration
        if (fread (&it->_duration, 1, 4, fp) != 4) {
            goto load_fail;
        }
        char s[100];
        pl_format_time (it->_duration, s, sizeof(s));
        pl_replace_meta (it, ":DURATION", s);

        if (minorver <= 2) {
            // legacy filetype support
            uint8_t ft;
            if (fread (&ft, 1, 1, fp) != 1) {
                goto load_fail;
            }
            if (ft) {
                char ftype[ft+1];
                if (fread (ftype, 1, ft, fp) != ft) {
                    goto load_fail;
                }
                ftype[ft] = 0;
                pl_replace_meta (it, ":FILETYPE", ftype);
            }

            float f;

            if (fread (&f, 1, 4, fp) != 4) {
                goto load_fail;
            }
            if (f != 0) {
                pl_set_item_replaygain (it, DDB_REPLAYGAIN_ALBUMGAIN, f);
            }

            if (fread (&f, 1, 4, fp) != 4) {
                goto load_fail;
            }
            if (f == 0) {
                f = 1;
            }
            if (f != 1) {
                pl_set_item_replaygain (it, DDB_REPLAYGAIN_ALBUMPEAK, f);
            }

            if (fread (&f, 1, 4, fp) != 4) {
                goto load_fail;
            }
            if (f != 0) {
                pl_set_item_replaygain (it, DDB_REPLAYGAIN_TRACKGAIN, f);
            }

            if (fread (&f, 1, 4, fp) != 4) {
                goto load_fail;
            }
            if (f == 0) {
                f = 1;
            }
            if (f != 1) {
                pl_set_item_replaygain (it, DDB_REPLAYGAIN_TRACKPEAK, f);
            }
        }

        uint32_t flg = 0;
        if (minorver >= 2) {
            if (fread (&flg, 1, 4, fp) != 4) {
                goto load_fail;
            }
        }
        else {
            if (pl_item_get_startsample (it) > 0 || pl_item_get_endsample (it) > 0 || tracknum > 0) {
                flg |= DDB_IS_SUBTRACK;
            }
        }
        pl_set_item_flags (it, flg);

        int16_t nm = 0;
        if (fread (&nm, 1, 2, fp) != 2) {
            goto load_fail;
        }
        for (int i = 0; i < nm; i++) {
            if (fread (&l, 1, 2, fp) != 2) {
                goto load_fail;
            }
            if (l >= 20000) {
                goto load_fail;
            }
            char key[l+1];
            if (fread (key, 1, l, fp) != l) {
                goto load_fail;
            }
            key[l] = 0;
            if (fread (&l, 1, 2, fp) != 2) {
                goto load_fail;
            }
            if (l >= 20000) {
                // skip
                fseek (fp, l, SEEK_CUR);
            }
            else {
                char value[l+1];
                int res = (int)fread (value, 1, l, fp);
                if (res != l) {
                    trace ("playlist read error: requested %d, got %d\n", l, res);
                    goto load_fail;
                }
                value[l] = 0;
                if (key[0] == ':') {
                    if (!strcmp (key, ":STARTSAMPLE")) {
                        pl_item_set_startsample (it, atoll (value));
                    }
                    else if (!strcmp (key, ":ENDSAMPLE")) {
                        pl_item_set_endsample (it, atoll (value));
                    }
                    else {
                        // some values are stored twice:
                        // once in legacy format, and once in metadata format
                        // here, we delete what was set from legacy, and overwrite with metadata
                        pl_replace_meta (it, key, value);
                    }
                }
                else {
                    pl_add_meta_full (it, key, value, l+1);
                }
            }
        }
        plt_insert_item (plt, plt->tail[PL_MAIN], it);
        if (last_added) {
            pl_item_unref (last_added);
        }
        last_added = it;
    }

    // load playlist metadata
    int16_t nm = 0;
    // for backwards format compatibility, don't fail if metadata is not found
    if (fread (&nm, 1, 2, fp) == 2) {
        for (int i = 0; i < nm; i++) {
            int16_t l;
            if (fread (&l, 1, 2, fp) != 2) {
                goto load_fail;
            }
            if (l < 0 || l >= 20000) {
                goto load_fail;
            }
            char key[l+1];
            if (fread (key, 1, l, fp) != l) {
                goto load_fail;
            }
            key[l] = 0;
            if (fread (&l, 1, 2, fp) != 2) {
                goto load_fail;
            }
            if (l<0 || l >= 20000) {
                // skip
                fseek (fp, l, SEEK_CUR);
            }
            else {
                char value[l+1];
                int res = (int)fread (value, 1, l, fp);
                if (res != l) {
                    trace ("playlist read error: requested %d, got %d\n", l, res);
                    goto load_fail;
                }
                value[l] = 0;
                // FIXME: multivalue support
                plt_add_meta (plt, key, value);
            }
        }
    }

    if (fp) {
        fclose (fp);
    }
    if (last_added) {
        pl_item_unref (last_added);
    }
    return last_added;
load_fail:
    trace ("playlist load fail (%s)!\n", fname);
    if (fp) {
        fclose (fp);
    }
    if (last_added) {
        pl_item_unref (last_added);
    }
    return last_added;
}


playItem_t *
plt_load (playlist_t *plt, playItem_t *after, const char *fname, int *pabort, int (*cb)(playItem_t *it, void *data), void *user_data) {
    return plt_load_int (0, plt, after, fname, pabort, cb, user_data);
}

int
pl_load_all (void) {
    int i = 0;
    int err = 0;
    char path[1024];
    DB_conf_item_t *it = conf_find ("playlist.tab.", NULL);
    if (!it) {
        // legacy (0.3.3 and earlier)
        char defpl[1024]; // $HOME/.config/deadbeef/default.dbpl
        if (snprintf (defpl, sizeof (defpl), "%s/default.dbpl", dbconfdir) > sizeof (defpl)) {
            fprintf (stderr, "error: cannot make string with default playlist path\n");
            return -1;
        }
        if (plt_add (plt_get_count (), _("Default")) < 0) {
            return -1;
        }

        playlist_t *plt = plt_get_for_idx (0);
        /*playItem_t *it = */ plt_load (plt, NULL, defpl, NULL, NULL, NULL);
        plt_unref (plt);
        return 0;
    }
    LOCK;
    plt_loading = 1;
    while (it) {
        if (!err) {
            if (plt_add (plt_get_count (), it->value) < 0) {
                return -1;
            }
            plt_set_curr_idx (plt_get_count () - 1);
        }
        err = 0;
        if (snprintf (path, sizeof (path), "%s/playlists/%d.dbpl", dbconfdir, i) > sizeof (path)) {
            fprintf (stderr, "error: failed to make path string for playlist filename\n");
            err = -1;
        }
        else {
            fprintf (stderr, "INFO: from file %s\n", path);

            playlist_t *plt = plt_get_curr ();
            /* playItem_t *trk = */ plt_load (plt, NULL, path, NULL, NULL, NULL);
            char conf[100];
            snprintf (conf, sizeof (conf), "playlist.cursor.%d", i);
            plt->current_row[PL_MAIN] = deadbeef->conf_get_int (conf, -1);
            snprintf (conf, sizeof (conf), "playlist.scroll.%d", i);
            plt->scroll = deadbeef->conf_get_int (conf, 0);
            plt->last_save_modification_idx = plt->modification_idx = 0;
            plt_unref (plt);

            if (!it) {
                fprintf (stderr, "WARNING: there were errors while loading playlist '%s' (%s)\n", it->value, path);
            }
        }
        it = conf_find ("playlist.tab.", it);
        i++;
    }
    plt_set_curr_idx (0);
    plt_loading = 0;
    plt_gen_conf ();
    messagepump_push (DB_EV_PLAYLISTSWITCHED, 0, 0, 0);
    UNLOCK;
    return err;
}

static inline void
pl_set_selected_in_playlist (playlist_t *playlist, playItem_t *it, int sel)
{
    it->selected = sel;
    playlist->recalc_seltime = 1;
}

void
plt_select_all (playlist_t *playlist) {
    LOCK;
    for (playItem_t *it = playlist->head[PL_MAIN]; it; it = it->next[PL_MAIN]) {
        pl_set_selected_in_playlist (playlist, it, 1);
    }
    UNLOCK;
}

void
pl_select_all (void) {
    LOCK;
    plt_select_all (playlist);
    UNLOCK;
}

void
plt_reshuffle (playlist_t *playlist, playItem_t **ppmin, playItem_t **ppmax) {
    LOCK;
    playItem_t *pmin = NULL;
    playItem_t *pmax = NULL;
    playItem_t *prev = NULL;
    const char *alb = NULL;
    const char *art = NULL;
    const char *aa = NULL;
    for (playItem_t *it = playlist->head[PL_MAIN]; it; it = it->next[PL_MAIN]) {
        const char *new_aa = NULL;
        new_aa = pl_find_meta_raw (it, "band");
        if (!new_aa) {
            new_aa = pl_find_meta_raw (it, "album artist");
        }
        if (!new_aa) {
            new_aa = pl_find_meta_raw (it, "albumartist");
        }
        if (pl_order == PLAYBACK_ORDER_SHUFFLE_ALBUMS && prev && alb == pl_find_meta_raw (it, "album") && ((aa && new_aa && aa == new_aa) || art == pl_find_meta_raw (it, "artist"))) {
            it->shufflerating = prev->shufflerating;
        }
        else {
            prev = it;
            it->shufflerating = rand ();
            alb = pl_find_meta_raw (it, "album");
            art = pl_find_meta_raw (it, "artist");
            aa = new_aa;
        }
        if (!pmin || it->shufflerating < pmin->shufflerating) {
            pmin = it;
        }
        if (!pmax || it->shufflerating > pmax->shufflerating) {
            pmax = it;
        }
        it->played = 0;
    }
    if (ppmin) {
        *ppmin = pmin;
    }
    if (ppmax) {
        *ppmax = pmax;
    }
    UNLOCK;
}

void
plt_set_item_duration (playlist_t *playlist, playItem_t *it, float duration) {
    LOCK;
    if (it->in_playlist && playlist) {
        if (it->_duration > 0) {
            playlist->totaltime -= it->_duration;
        }
        if (duration > 0) {
            playlist->totaltime += duration;
        }
        if (playlist->totaltime < 0) {
            playlist->totaltime = 0;
        }
    }
    it->_duration = duration;
    char s[100];
    pl_format_time (it->_duration, s, sizeof(s));
    pl_replace_meta (it, ":DURATION", s);
    UNLOCK;
}

float
pl_get_item_duration (playItem_t *it) {
    return it->_duration;
}

void
pl_set_item_replaygain (playItem_t *it, int idx, float value) {
    char s[100];
    switch (idx) {
    case DDB_REPLAYGAIN_ALBUMGAIN:
    case DDB_REPLAYGAIN_TRACKGAIN:
        snprintf (s, sizeof (s), "%0.2f dB", value);
        break;
    case DDB_REPLAYGAIN_ALBUMPEAK:
    case DDB_REPLAYGAIN_TRACKPEAK:
        snprintf (s, sizeof (s), "%0.6f", value);
        break;
    default:
        return;
    }
    pl_replace_meta (it, ddb_internal_rg_keys[idx], s);
}

float
pl_get_item_replaygain (playItem_t *it, int idx) {
    if (idx < 0 || idx > DDB_REPLAYGAIN_TRACKPEAK) {
        return 0;
    }

    switch (idx) {
    case DDB_REPLAYGAIN_ALBUMGAIN:
    case DDB_REPLAYGAIN_TRACKGAIN:
        return pl_find_meta_float (it, ddb_internal_rg_keys[idx], 0);
    case DDB_REPLAYGAIN_ALBUMPEAK:
    case DDB_REPLAYGAIN_TRACKPEAK:
        return pl_find_meta_float (it, ddb_internal_rg_keys[idx], 1);
    }
    return 0;
}

int
pl_format_item_queue (playItem_t *it, char *s, int size) {
    LOCK;
    *s = 0;
    int initsize = size;
    const char *val = pl_find_meta_raw (it, "_playing");
    while (val && *val) {
        while (*val && *val != '=') {
            val++;
        }
        if (*val == '=') {
            // found value
            val++;
            if (!(*val)) {
                break;
            }
            const char *e = NULL;
            if (*val == '"') {
                val++;
                e = val;
                while (*e && *e != '"') {
                    e++;
                }
            }
            else {
                e = val;
                while (*e && *e != ' ') {
                    e++;
                }
            }
            int n = (int)(e - val);
            if (n > size-1) {
                n = size-1;
            }
            strncpy (s, val, n);
            s += n;
            *s++ = ' ';
            *s = 0;
            size -= n+1;
            val = e;
            if (*val) {
                val++;
            }
            while (*val && *val == ' ') {
                val++;
            }
        }
    }

    int pq_cnt = playqueue_getcount ();

    if (!pq_cnt) {
        UNLOCK;
        return 0;
    }

    int qinitsize = size;
    int init = 1;
    int len;
    for (int i = 0; i < pq_cnt; i++) {
        if (size <= 0) {
            break;
        }

        playItem_t *pqitem = playqueue_get_item (i);
        if (pqitem == it) {
            if (init) {
                init = 0;
                s[0] = '(';
                s++;
                size--;
                len = snprintf (s, size, "%d", i+1);
            }
            else {
                len = snprintf (s, size, ",%d", i+1);
            }
            s += len;
            size -= len;
        }
        pl_item_unref (pqitem);
    }
    if (size != qinitsize && size > 0) {
        len = snprintf (s, size, ")");
        s += len;
        size -= len;
    }
    UNLOCK;
    return initsize-size;
}

void
pl_format_time (float t, char *dur, int size) {
    if (t >= 0) {
        t = roundf (t);
        int hourdur = t / (60 * 60);
        int mindur = (t - hourdur * 60 * 60) / 60;
        int secdur = t - hourdur*60*60 - mindur * 60;

        if (hourdur) {
            snprintf (dur, size, "%d:%02d:%02d", hourdur, mindur, secdur);
        }
        else {
            snprintf (dur, size, "%d:%02d", mindur, secdur);
        }
    }
    else {
        strcpy (dur, "∞");
    }
}

const char *
pl_format_duration (playItem_t *it, const char *ret, char *dur, int size) {
    if (ret) {
        return ret;
    }
    pl_format_time (pl_get_item_duration (it), dur, size);
    return dur;
}

static const char *
pl_format_elapsed (const char *ret, char *elapsed, int size) {
    if (ret) {
        return ret;
    }
    float playpos = streamer_get_playpos ();
    pl_format_time (playpos, elapsed, size);
    return elapsed;
}

// this function allows to escape special chars substituted for conversions
// @escape_chars: list of escapable characters terminated with 0, or NULL if none
static int
pl_format_title_int (const char *escape_chars, playItem_t *it, int idx, char *s, int size, int id, const char *fmt) {
    char tmp[50];
    char tags[200];
    char dirname[PATH_MAX];
    const char *duration = NULL;
    const char *elapsed = NULL;
    int escape_slash = 0;

    char *ss = s;

    LOCK;
    if (id != -1 && it) {
        const char *text = NULL;
        switch (id) {
        case DB_COLUMN_FILENUMBER:
            if (idx == -1) {
                idx = pl_get_idx_of (it);
            }
            snprintf (tmp, sizeof (tmp), "%d", idx+1);
            text = tmp;
            break;
        case DB_COLUMN_PLAYING:
            UNLOCK;
            return pl_format_item_queue (it, s, size);
        }
        if (text) {
            strncpy (s, text, size);
            UNLOCK;
            for (ss = s; *ss; ss++) {
                if (*ss == '\n') {
                    *ss = ';';
                }
            }
            return (int)strlen (s);
        }
        else {
            s[0] = 0;
        }
        UNLOCK;
        return 0;
    }
    int n = size-1;
    while (*fmt && n > 0) {
        if (*fmt != '%') {
            *s++ = *fmt;
            n--;
        }
        else {
            fmt++;
            const char *meta = NULL;
            if (*fmt == 0) {
                break;
            }
            else if (!it && *fmt != 'V') {
                // only %V (version) works without track pointer
            }
            else if (*fmt == '@') {
                const char *e = fmt;
                e++;
                while (*e && *e != '@') {
                    e++;
                }
                if (*e == '@') {
                    char nm[100];
                    int l = (int)(e-fmt-1);
                    l = min (l, sizeof (nm)-1);
                    strncpy (nm, fmt+1, l);
                    nm[l] = 0;
                    meta = pl_find_meta_raw (it, nm);
                    if (!meta) {
                        meta = "";
                    }
                    fmt = e;
                }
            }
            else if (*fmt == '/') {
                // this means all '/' in the ongoing fields must be replaced with '\'
                escape_slash = 1;
            }
            else if (*fmt == 'a') {
                meta = pl_find_meta_raw (it, "artist");
#ifndef DISABLE_CUSTOM_TITLE
                const char *custom = pl_find_meta_raw (it, ":CUSTOM_TITLE");
#endif
                if (!meta
#ifndef DISABLE_CUSTOM_TITLE
                && !custom
#endif
                ) {
                    meta = "Unknown artist";
                }

#ifndef DISABLE_CUSTOM_TITLE
                if (custom) {
                    if (!meta) {
                        meta = custom;
                    }
                    else {
                        int l = strlen (custom) + strlen (meta) + 4;
                        char *out = alloca (l);
                        snprintf (out, l, "[%s] %s", custom, meta);
                        meta = out;
                    }
                }
#endif
            }
            else if (*fmt == 't') {
                meta = pl_find_meta_raw (it, "title");
                if (!meta) {
                    const char *f = pl_find_meta_raw (it, ":URI");
                    if (f) {
                        const char *start = strrchr (f, '/');
                        if (start) {
                            start++;
                        }
                        else {
                            start = f;
                        }
                        const char *end = strrchr (start, '.');
                        if (end) {
                            int n = (int)(end-start);
                            n = min (n, sizeof (dirname)-1);
                            strncpy (dirname, start, n);
                            dirname[n] = 0;
                            meta = dirname;
                        }
                        else {
                            meta = "";
                        }
                    }
                    else {
                        meta = "";
                    }
                }
            }
            else if (*fmt == 'b') {
                meta = pl_find_meta_raw (it, "album");
                if (!meta) {
                    meta = "Unknown album";
                }
            }
            else if (*fmt == 'B') {
                meta = pl_find_meta_raw (it, "band");
                if (!meta) {
                    meta = pl_find_meta_raw (it, "album artist");
                    if (!meta) {
                        meta = pl_find_meta_raw (it, "albumartist");
                        if (!meta) {
                            meta = pl_find_meta_raw (it, "artist");
                        }
                    }
                }

#ifndef DISABLE_CUSTOM_TITLE
                const char *custom = pl_find_meta_raw (it, ":CUSTOM_TITLE");
                if (custom) {
                    if (!meta) {
                        meta = custom;
                    }
                    else {
                        int l = strlen (custom) + strlen (meta) + 4;
                        char *out = alloca (l);
                        snprintf (out, l, "[%s] %s", custom, meta);
                        meta = out;
                    }
                }
#endif
            }
            else if (*fmt == 'C') {
                meta = pl_find_meta_raw (it, "composer");
            }
            else if (*fmt == 'n') {
                meta = pl_find_meta_raw (it, "track");
                if (meta) {
                    // check if it's numbers only
                    const char *p = meta;
                    while (*p) {
                        if (!isdigit (*p)) {
                            break;
                        }
                        p++;
                    }
                    if (!(*p)) {
                        snprintf (tmp, sizeof (tmp), "%02d", atoi (meta));
                        meta = tmp;
                    }
                }
            }
            else if (*fmt == 'N') {
                meta = pl_find_meta_raw (it, "numtracks");
            }
            else if (*fmt == 'y') {
                meta = pl_find_meta_raw (it, "year");
            }
            else if (*fmt == 'Y') {
                meta = pl_find_meta_raw (it, "original_release_time");
                if (!meta) {
                    meta = pl_find_meta_raw (it, "original_release_year");
                    if (!meta) {
                        meta = pl_find_meta_raw (it, "year");
                    }
                }
            }
            else if (*fmt == 'g') {
                meta = pl_find_meta_raw (it, "genre");
            }
            else if (*fmt == 'c') {
                meta = pl_find_meta_raw (it, "comment");
            }
            else if (*fmt == 'r') {
                meta = pl_find_meta_raw (it, "copyright");
            }
            else if (*fmt == 'l') {
                const char *value = (duration = pl_format_duration (it, duration, tmp, sizeof (tmp)));
                while (n > 0 && *value) {
                    *s++ = *value++;
                    n--;
                }
            }
            else if (*fmt == 'e') {
                // what a hack..
                const char *value = (elapsed = pl_format_elapsed (elapsed, tmp, sizeof (tmp)));
                while (n > 0 && *value) {
                    *s++ = *value++;
                    n--;
                }
            }
            else if (*fmt == 'f') {
                const char *f = pl_find_meta_raw (it, ":URI");
                meta = strrchr (f, '/');
                if (meta) {
                    meta++;
                }
                else {
                    meta = f;
                }
            }
            else if (*fmt == 'F') {
                meta = pl_find_meta_raw (it, ":URI");
            }
            else if (*fmt == 'T') {
                char *t = tags;
                char *e = tags + sizeof (tags);
                int c;
                *t = 0;

                if (it->_flags & DDB_TAG_ID3V1) {
                    c = snprintf (t, e-t, "ID3v1 | ");
                    t += c;
                }
                if (it->_flags & DDB_TAG_ID3V22) {
                    c = snprintf (t, e-t, "ID3v2.2 | ");
                    t += c;
                }
                if (it->_flags & DDB_TAG_ID3V23) {
                    c = snprintf (t, e-t, "ID3v2.3 | ");
                    t += c;
                }
                if (it->_flags & DDB_TAG_ID3V24) {
                    c = snprintf (t, e-t, "ID3v2.4 | ");
                    t += c;
                }
                if (it->_flags & DDB_TAG_APEV2) {
                    c = snprintf (t, e-t, "APEv2 | ");
                    t += c;
                }
                if (it->_flags & DDB_TAG_VORBISCOMMENTS) {
                    c = snprintf (t, e-t, "VorbisComments | ");
                    t += c;
                }
                if (it->_flags & DDB_TAG_CUESHEET) {
                    c = snprintf (t, e-t, "CueSheet | ");
                    t += c;
                }
                if (it->_flags & DDB_TAG_ICY) {
                    c = snprintf (t, e-t, "Icy | ");
                    t += c;
                }
                if (it->_flags & DDB_TAG_ITUNES) {
                    c = snprintf (t, e-t, "iTunes | ");
                    t += c;
                }
                if (t != tags) {
                    *(t - 3) = 0;
                }
                meta = tags;
            }
            else if (*fmt == 'd') {
                // directory
                const char *f = pl_find_meta_raw (it, ":URI");
                const char *end = strrchr (f, '/');
                if (!end) {
                    meta = ""; // got relative path without folder (should not happen)
                }
                else {
                    const char *start = end;
                    start--;
                    while (start > f && (*start != '/')) {
                        start--;
                    }

                    if (*start == '/') {
                        start++;
                    }

                    // copy
                    int len = (int)(end-start);
                    len = min (len, sizeof (dirname)-1);
                    strncpy (dirname, start, len);
                    dirname[len] = 0;
                    meta = dirname;
                }
            }
            else if (*fmt == 'D') {
                const char *f = pl_find_meta_raw (it, ":URI");
                // directory with path
                const char *end = strrchr (f, '/');
                if (!end) {
                    meta = ""; // got relative path without folder (should not happen)
                }
                else {
                    // copy
                    int len = (int)(end - f);
                    len = min (len, sizeof (dirname)-1);
                    strncpy (dirname, f, len);
                    dirname[len] = 0;
                    meta = dirname;
                }
            }
            else if (*fmt == 'L') {
                float l = 0;
                for (playItem_t *it = playlist->head[PL_MAIN]; it; it = it->next[PL_MAIN]) {
                    if (it->selected) {
                        l += it->_duration;
                    }
                }
                pl_format_time (l, tmp, sizeof(tmp));
                meta = tmp;
            }
            else if (*fmt == 'X') {
                int n = 0;
                for (playItem_t *it = playlist->head[PL_MAIN]; it; it = it->next[PL_MAIN]) {
                    if (it->selected) {
                        n++;
                    }
                }
                snprintf (tmp, sizeof (tmp), "%d", n);
                meta = tmp;
            }
            else if (*fmt == 'Z') {
                DB_fileinfo_t *c = deadbeef->streamer_get_current_fileinfo (); // FIXME: might crash streamer
                if (c) {
                    if (c->fmt.channels <= 2) {
                        meta = c->fmt.channels == 1 ? _("Mono") : _("Stereo");
                    }
                    else {
                        snprintf (tmp, sizeof (tmp), "%dch Multichannel", c->fmt.channels);
                        meta = tmp;
                    }
                }
            }
#ifdef VERSION
            else if (*fmt == 'V') {
                meta = VERSION;
            }
#endif
            else {
                *s++ = *fmt;
                n--;
            }

            if (meta) {
                const char *value = meta;
                if (escape_chars) {
                    // need space for at least 2 single-quotes
                    if (n < 2) {
                        goto error;
                    }
                    *s++ = '\'';
                    n--;
                    while (n > 2 && *value) {
                        if (strchr (escape_chars, *value)) {
                            *s++ = '\\';
                            n--;
                            *s++ = *value++;
                            n--;
                        }
                        else if (escape_slash && *value == '/') {
                            *s++ = '\\';
                            n--;
                            *s++ = '\\';
                            n--;
                            break;
                        }
                        else {
                            *s++ = *value++;
                        }
                    }
                    if (n < 1) {
                        fprintf (stderr, "pl_format_title_int: got unpredicted state while formatting escaped string. please report a bug.\n");
                        *ss = 0; // should never happen
                        return -1;
                    }
                    *s++ = '\'';
                    n--;
                }
                else {
                    while (n > 0 && *value) {
                        if (escape_slash && *value == '/') {
                            *s++ = '\\';
                            n--;
                            value++;
                        }
                        else {
                            *s++ = *value++;
                            n--;
                        }
                    }
                }
            }
        }
        fmt++;
    }
error:
    *s = 0;
    UNLOCK;

    // replace all \n with ;
    while (*ss) {
        if (*ss == '\n') {
            *ss = ';';
        }
        ss++;
    }

    return size - n - 1;
}

int
pl_format_title (playItem_t *it, int idx, char *s, int size, int id, const char *fmt) {
    return pl_format_title_int (NULL, it, idx, s, size, id, fmt);
}

int
pl_format_title_escaped (playItem_t *it, int idx, char *s, int size, int id, const char *fmt) {
    return pl_format_title_int ("'", it, idx, s, size, id, fmt);
}

void
plt_reset_cursor (playlist_t *playlist) {
    int i;
    LOCK;
    for (i = 0; i < PL_MAX_ITERATORS; i++) {
        playlist->current_row[i] = -1;
    }
    UNLOCK;
}

float
plt_get_totaltime (playlist_t *playlist) {
    if (!playlist) {
        return 0;
    }
    return playlist->totaltime;
}

float
pl_get_totaltime (void) {
    LOCK;
    float t = plt_get_totaltime (playlist);
    UNLOCK;
    return t;
}

float
plt_get_selection_playback_time (playlist_t *playlist) {
    LOCK;

    if (!playlist->recalc_seltime) {
        float t = playlist->seltime;
        UNLOCK;
        return t;
    }

    float t = 0;

    for (playItem_t *it = playlist->head[PL_MAIN]; it; it = it->next[PL_MAIN]) {
        if (it->selected){
            t += it->_duration;
        }
    }

    playlist->seltime = t;
    playlist->recalc_seltime = 0;

    UNLOCK;

    return t;
}

void
pl_set_selected (playItem_t *it, int sel) {
    LOCK;
    // NOTE: it's not really known here, which playlist the item belongs to, but we're assuming it's in the current playlist.
    // If the item is in another playlist -- this call will make selection playback time
    // to be recalculated for the current playlist, next time it's requested,
    // while the same value will be off in the playlist which the item belongs to.
    pl_set_selected_in_playlist(playlist, it, sel);
    UNLOCK;
}

int
pl_is_selected (playItem_t *it) {
    return it->selected;
}

playItem_t *
plt_get_first (playlist_t *playlist, int iter) {
    if (!playlist) {
        return NULL;
    }
    playItem_t *p = playlist->head[iter];
    if (p) {
        pl_item_ref (p);
    }
    return p;
}

playItem_t *
pl_get_first (int iter) {
    LOCK;
    playItem_t *it = plt_get_first (playlist, iter);
    UNLOCK;
    return it;
}

playItem_t *
plt_get_last (playlist_t *playlist, int iter) {
    playItem_t *p = playlist->tail[iter];
    if (p) {
        pl_item_ref (p);
    }
    return p;
}

playItem_t *
pl_get_last (int iter) {
    LOCK;
    playItem_t *it = plt_get_last (playlist, iter);
    UNLOCK;
    return it;
}

playItem_t *
pl_get_next (playItem_t *it, int iter) {
    playItem_t *next = it ? it->next[iter] : NULL;
    if (next) {
        pl_item_ref (next);
    }
    return next;
}

playItem_t *
pl_get_prev (playItem_t *it, int iter) {
    playItem_t *prev = it ? it->prev[iter] : NULL;
    if (prev) {
        pl_item_ref (prev);
    }
    return prev;
}

int
plt_get_cursor (playlist_t *playlist, int iter) {
    if (!playlist) {
        return -1;
    }
    return playlist->current_row[iter];
}

int
pl_get_cursor (int iter) {
    LOCK;
    int c = plt_get_cursor (playlist, iter);
    UNLOCK;
    return c;
}

void
plt_set_cursor (playlist_t *playlist, int iter, int cursor) {
    playlist->current_row[iter] = cursor;
}

void
pl_set_cursor (int iter, int cursor) {
    LOCK;
    plt_set_cursor (playlist, iter, cursor);
    UNLOCK;
}

// this function must move items in playlist
// list of items is indexes[count]
// drop_before is insertion point
void
plt_move_items (playlist_t *to, int iter, playlist_t *from, playItem_t *drop_before, uint32_t *indexes, int count) {
    LOCK;

    if (!from || !to) {
        UNLOCK;
        return;
    }

    // don't let streamer think that current song was removed
    no_remove_notify = 1;

    // unlink items from from, and link together
    int processed = 0;
    int idx = 0;
    playItem_t *next = NULL;

    // find insertion point
    playItem_t *drop_after = NULL;
    if (drop_before) {
        drop_after = drop_before->prev[iter];
    }
    else {
        drop_after = to->tail[iter];
    }

    playItem_t *playing = streamer_get_playing_track ();

    for (playItem_t *it = from->head[iter]; it && processed < count; it = next, idx++) {
        next = it->next[iter];
        if (idx == indexes[processed]) {
            if (it == playing && to != from) {
                streamer_set_streamer_playlist (to);
            }
            pl_item_ref (it);
            if (drop_after == it) {
                drop_after = it->prev[PL_MAIN];
            }
            plt_remove_item (from, it);
            plt_insert_item (to, drop_after, it);
            pl_item_unref (it);
            drop_after = it;
            processed++;
        }
    }

    if (playing) {
        pl_item_unref (playing);
    }


    no_remove_notify = 0;
    UNLOCK;
}

void
plt_copy_items (playlist_t *to, int iter, playlist_t *from, playItem_t *before, uint32_t *indices, int cnt) {
    pl_lock ();

    if (!from || !to || cnt == 0) {
        pl_unlock ();
        return;
    }

    playItem_t **items = malloc (cnt * sizeof(playItem_t *));
    for (int i = 0; i < cnt; i++) {
        playItem_t *it = from->head[iter];
        for (int idx = 0; it && idx < indices[i]; idx++) {
            it = it->next[iter];
        }
        items[i] = it;
        if (!it) {
            trace ("plt_copy_items: warning: item %d not found in source plt_to\n", indices[i]);
        }
    }
    playItem_t *after = before ? before->prev[iter] : to->tail[iter];
    for (int i = 0; i < cnt; i++) {
        if (items[i]) {
            playItem_t *new_it = pl_item_alloc();
            pl_item_copy (new_it, items[i]);
            pl_insert_item (after, new_it);
            pl_item_unref (new_it);
            after = new_it;
        }
    }
    free(items);

    pl_unlock ();
}

static void
plt_search_reset_int (playlist_t *playlist, int clear_selection) {
    LOCK;
    while (playlist->head[PL_SEARCH]) {
        playItem_t *next = playlist->head[PL_SEARCH]->next[PL_SEARCH];
        if (clear_selection) {
            pl_set_selected_in_playlist(playlist, playlist->head[PL_SEARCH], 0);
        }
        playlist->head[PL_SEARCH]->next[PL_SEARCH] = NULL;
        playlist->head[PL_SEARCH]->prev[PL_SEARCH] = NULL;
        playlist->head[PL_SEARCH] = next;
    }
    playlist->tail[PL_SEARCH] = NULL;
    playlist->count[PL_SEARCH] = 0;
    UNLOCK;
}

void
plt_search_reset (playlist_t *playlist) {
    plt_search_reset_int (playlist, 1);
}

static void
_plsearch_append (playlist_t *plt, playItem_t *it, int select_results) {
    it->next[PL_SEARCH] = NULL;
    it->prev[PL_SEARCH] = plt->tail[PL_SEARCH];
    if (plt->tail[PL_SEARCH]) {
        plt->tail[PL_SEARCH]->next[PL_SEARCH] = it;
        plt->tail[PL_SEARCH] = it;
    }
    else {
        plt->head[PL_SEARCH] = plt->tail[PL_SEARCH] = it;
    }
    if (select_results) {
        pl_set_selected_in_playlist(plt, it, 1);
    }
    plt->count[PL_SEARCH]++;
}

void
plt_search_process2 (playlist_t *playlist, const char *text, int select_results) {
    LOCK;
    plt_search_reset_int (playlist, select_results);

    // convert text to lowercase, to save some cycles
    char lc[1000];
    int n = sizeof (lc)-1;
    const char *p = text;
    char *out = lc;
    while (*p) {
        int32_t i = 0;
        char s[10];
        u8_nextchar (p, &i);
        int l = u8_tolower ((const int8_t *)p, i, s);
        n -= l;
        if (n < 0) {
            break;
        }
        memcpy (out, s, l);
        p += i;
        out += l;
    }
    *out = 0;

    int lc_is_valid_u8 = u8_valid (lc, (int)strlen (lc), NULL);

    playlist->search_cmpidx++;
    if (playlist->search_cmpidx > 127) {
        playlist->search_cmpidx = 1;
    }

    for (playItem_t *it = playlist->head[PL_MAIN]; it; it = it->next[PL_MAIN]) {
        if (select_results) {
            pl_set_selected_in_playlist(playlist, it, 0);
        }
        if (*text) {
            DB_metaInfo_t *m = NULL;
            for (m = it->meta; m; m = m->next) {
                int is_uri = !strcmp (m->key, ":URI");
                if ((m->key[0] == ':' && !is_uri) || m->key[0] == '_' || m->key[0] == '!') {
                    break;
                }
                if (!strcasecmp(m->key, "cuesheet") || !strcasecmp (m->key, "log")) {
                    continue;
                }

                const char *value = m->value;
                const char *end = value + m->valuesize;

                if (is_uri) {
                    value = strrchr (value, '/');
                    if (value) {
                        value++;
                    }
                    else {
                        value = m->value;
                    }
                }

                char cmp = *(m->value-1);

                if (abs (cmp) == playlist->search_cmpidx) { // string was already compared in this search
                    if (cmp > 0) { // it's a match -- append to search results
                        _plsearch_append (playlist, it, select_results);
                        break;
                    }
                }
                else {
                    int match = -playlist->search_cmpidx; // assume no match
                    do {
                        int len = (int)strlen(value);
                        if (lc_is_valid_u8 && u8_valid(value, len, NULL) && utfcasestr_fast (value, lc)) {
                            _plsearch_append (playlist, it, select_results);
                            match = playlist->search_cmpidx; // it's a match
                            break;
                        }
                        value += len+1;
                    } while (value < end);
                    *((char *)m->value-1) = match;
                    if (match > 0) {
                        break;
                    }
                }
            }
        }
    }
    UNLOCK;
}

void
plt_search_process (playlist_t *playlist, const char *text) {
    plt_search_process2 (playlist, text, 1);
}

void
send_trackinfochanged (playItem_t *track) {
    ddb_event_track_t *ev = (ddb_event_track_t *)messagepump_event_alloc (DB_EV_TRACKINFOCHANGED);
    ev->track = DB_PLAYITEM (track);
    if (track) {
        pl_item_ref (track);
    }

    messagepump_push_event ((ddb_event_t*)ev, 0, 0);
}

void
pl_items_copy_junk (playItem_t *from, playItem_t *first, playItem_t *last) {
    LOCK;
    DB_metaInfo_t *meta = from->meta;
    while (meta) {
        playItem_t *i;
        for (i = first; i; i = i->next[PL_MAIN]) {
            i->_flags = from->_flags;
            pl_add_meta_copy (i, meta);
            if (i == last) {
                break;
            }
        }
        meta = meta->next;
    }
    UNLOCK;
}

uint32_t
pl_get_item_flags (playItem_t *it) {
    LOCK;
    uint32_t flags = it->_flags;
    UNLOCK;
    return flags;
}

void
pl_set_item_flags (playItem_t *it, uint32_t flags) {
    LOCK;
    it->_flags = flags;

    char s[200];
    pl_format_title (it, -1, s, sizeof (s), -1, "%T");
    pl_replace_meta (it, ":TAGS", s);
    pl_replace_meta (it, ":HAS_EMBEDDED_CUESHEET", (flags & DDB_HAS_EMBEDDED_CUESHEET) ? _("Yes") : _("No"));
    UNLOCK;
}

playlist_t *
pl_get_playlist (playItem_t *it) {
    LOCK;
    playlist_t *p = playlists_head;
    while (p) {
        int idx = plt_get_item_idx (p, it, PL_MAIN);
        if (idx != -1) {
            plt_ref (p);
            UNLOCK;
            return p;
        }
        p = p->next;
    }
    UNLOCK;
    return NULL;
}

// this function must be called when the user starts track manually in shuffle albums mode
// r is an index of current track
// mark previous songs in the album as played
void
plt_init_shuffle_albums (playlist_t *plt, int r) {
    pl_lock ();
    playItem_t *first = plt_get_item_for_idx (plt, r, PL_MAIN);
    if (!first) {
        pl_unlock ();
        return;
    }
    if (first->played) {
        plt_reshuffle (plt, NULL, NULL);
    }
    if (first) {
        int rating = first->shufflerating;
        playItem_t *it = first->prev[PL_MAIN];
        pl_item_unref (first);
        while (it && rating == it->shufflerating) {
            it->played = 1;
            it = it->prev[PL_MAIN];
        }
    }
    pl_unlock ();
}

void
plt_set_fast_mode (playlist_t *plt, int fast) {
    plt->fast_mode = (unsigned)fast;
}

int
plt_is_fast_mode (playlist_t *plt) {
    return plt->fast_mode;
}

void
pl_ensure_lock (void) {
#if DETECT_PL_LOCK_RC
    pthread_t tid = pthread_self ();
    for (int i = 0; i < ntids; i++) {
        if (tids[i] == tid) {
            return;
        }
    }
    fprintf (stderr, "\033[0;31mnon-thread-safe playlist access function was called outside of pl_lock. please make a backtrace and post a bug. thank you.\033[37;0m\n");
    assert(0);
#endif
}

int
plt_get_idx (playlist_t *plt) {
    int i;
    playlist_t *p;
    for (i = 0, p = playlists_head; p && p != plt; i++, p = p->next);
    if (p == 0) {
        return -1;
    }
    return i;
}

int
plt_save_config (playlist_t *plt) {
    int i = plt_get_idx (plt);
    if (i == -1) {
        return -1;
    }
    return plt_save_n (i);
}

int
listen_file_added (int (*callback)(ddb_fileadd_data_t *data, void *user_data), void *user_data) {
    ddb_fileadd_listener_t *l;

    int id = 1;
    for (l = file_add_listeners; l; l = l->next) {
        if (l->id > id) {
            id = l->id+1;
        }
    }

    l = malloc (sizeof (ddb_fileadd_listener_t));
    memset (l, 0, sizeof (ddb_fileadd_listener_t));
    l->id = id;
    l->callback = callback;
    l->user_data = user_data;
    l->next = file_add_listeners;
    file_add_listeners = l;
    return id;
}

void
unlisten_file_added (int id) {
    ddb_fileadd_listener_t *prev = NULL;
    for (ddb_fileadd_listener_t *l = file_add_listeners; l; prev = l, l = l->next) {
        if (l->id == id) {
            if (prev) {
                prev->next = l->next;
            }
            else {
                file_add_listeners = l->next;
            }
            free (l);
            break;
        }
    }
}

int
listen_file_add_beginend (void (*callback_begin) (ddb_fileadd_data_t *data, void *user_data), void (*callback_end)(ddb_fileadd_data_t *data, void *user_data), void *user_data) {
    ddb_fileadd_beginend_listener_t *l;

    int id = 1;
    for (l = file_add_beginend_listeners; l; l = l->next) {
        if (l->id > id) {
            id = l->id+1;
        }
    }

    l = malloc (sizeof (ddb_fileadd_beginend_listener_t));
    memset (l, 0, sizeof (ddb_fileadd_beginend_listener_t));
    l->id = id;
    l->callback_begin = callback_begin;
    l->callback_end = callback_end;
    l->user_data = user_data;
    l->next = file_add_beginend_listeners;
    file_add_beginend_listeners = l;
    return id;
}

void
unlisten_file_add_beginend (int id) {
    ddb_fileadd_beginend_listener_t *prev = NULL;
    for (ddb_fileadd_beginend_listener_t *l = file_add_beginend_listeners; l; prev = l, l = l->next) {
        if (l->id == id) {
            if (prev) {
                prev->next = l->next;
            }
            else {
                file_add_beginend_listeners = l->next;
            }
            free (l);
            break;
        }
    }
}

int
register_fileadd_filter (int (*callback)(ddb_file_found_data_t *data, void *user_data), void *user_data) {
    ddb_fileadd_filter_t *l;

    int id = 1;
    for (l = file_add_filters; l; l = l->next) {
        if (l->id > id) {
            id = l->id+1;
        }
    }

    l = malloc (sizeof (ddb_fileadd_filter_t));
    memset (l, 0, sizeof (ddb_fileadd_filter_t));
    l->id = id;
    l->callback = callback;
    l->user_data = user_data;
    l->next = file_add_filters;
    file_add_filters = l;
    return id;
}

void
unregister_fileadd_filter (int id) {
    ddb_fileadd_filter_t *prev = NULL;
    for (ddb_fileadd_filter_t *l = file_add_filters; l; prev = l, l = l->next) {
        if (l->id == id) {
            if (prev) {
                prev->next = l->next;
            }
            else {
                file_add_filters = l->next;
            }
            free (l);
            break;
        }
    }
}

playItem_t *
plt_load2 (int visibility, playlist_t *plt, playItem_t *after, const char *fname, int *pabort, int (*callback)(playItem_t *it, void *user_data), void *user_data) {
    return plt_load_int (visibility, plt, after, fname, pabort, callback, user_data);
}

int
plt_add_file2 (int visibility, playlist_t *plt, const char *fname, int (*callback)(playItem_t *it, void *user_data), void *user_data) {
    int res = plt_add_file_int (visibility, plt, fname, callback, user_data);
    return res;
}

playItem_t *
pl_item_init (const char *fname) {
    playlist_t plt;
    memset (&plt, 0, sizeof (plt));
    return plt_insert_file_int (-1, &plt, NULL, fname, NULL, NULL, NULL);
}

int
plt_add_dir2 (int visibility, playlist_t *plt, const char *dirname, int (*callback)(playItem_t *it, void *user_data), void *user_data) {
    int prev_sl = plt->follow_symlinks;
    plt->follow_symlinks = conf_get_int ("add_folders_follow_symlinks", 0);

    int prev = plt->ignore_archives;
    plt->ignore_archives = conf_get_int ("ignore_archives", 1);

    int abort = 0;
    playItem_t *it = plt_insert_dir_int (visibility, plt, NULL, plt->tail[PL_MAIN], dirname, &abort, callback, user_data);

    plt->ignore_archives = prev;
    plt->follow_symlinks = prev_sl;
    if (it) {
        // pl_insert_file doesn't hold reference, don't unref here
        return 0;
    }
    return -1;
}

playItem_t *
plt_insert_file2 (int visibility, playlist_t *playlist, playItem_t *after, const char *fname, int *pabort, int (*callback)(playItem_t *it, void *user_data), void *user_data) {
    return plt_insert_file_int (visibility, playlist, after, fname, pabort, callback, user_data);
}

playItem_t *
plt_insert_dir2 (int visibility, playlist_t *plt, playItem_t *after, const char *dirname, int *pabort, int (*callback)(playItem_t *it, void *user_data), void *user_data) {
    int prev_sl = plt->follow_symlinks;
    plt->follow_symlinks = conf_get_int ("add_folders_follow_symlinks", 0);
    plt->ignore_archives = conf_get_int ("ignore_archives", 1);

    playItem_t *ret = plt_insert_dir_int (visibility, plt, NULL, after, dirname, pabort, callback, user_data);

    plt->follow_symlinks = prev_sl;
    plt->ignore_archives = 0;
    return ret;
}

int
plt_add_files_begin (playlist_t *plt, int visibility) {
    pl_lock ();
    if (addfiles_playlist) {
        pl_unlock ();
        return -1;
    }
    if (plt->files_adding) {
        pl_unlock ();
        return -1;
    }
    addfiles_playlist = plt;
    plt_ref (addfiles_playlist);
    plt->files_adding = 1;
    plt->files_add_visibility = visibility;
    pl_unlock ();
    ddb_fileadd_data_t d;
    memset (&d, 0, sizeof (d));
    d.visibility = visibility;
    d.plt = (ddb_playlist_t *)plt;
    for (ddb_fileadd_beginend_listener_t *l = file_add_beginend_listeners; l; l = l->next) {
        l->callback_begin (&d, l->user_data);
    }
    background_job_increment ();
    return 0;
}

void
plt_add_files_end (playlist_t *plt, int visibility) {
    pl_lock ();
    if (addfiles_playlist) {
        plt_unref (addfiles_playlist);
    }
    addfiles_playlist = NULL;
    messagepump_push (DB_EV_PLAYLISTCHANGED, 0, DDB_PLAYLIST_CHANGE_CONTENT, 0);
    plt->files_adding = 0;
    pl_unlock ();
    ddb_fileadd_data_t d;
    memset (&d, 0, sizeof (d));
    d.visibility = visibility;
    d.plt = (ddb_playlist_t *)plt;
    for (ddb_fileadd_beginend_listener_t *l = file_add_beginend_listeners; l; l = l->next) {
        l->callback_end (&d, l->user_data);
    }
    background_job_decrement ();
}

void
plt_deselect_all (playlist_t *playlist) {
    LOCK;
    for (playItem_t *it = playlist->head[PL_MAIN]; it; it = it->next[PL_MAIN]) {
        it->selected = 0;
    }
    playlist->seltime = 0;
    playlist->recalc_seltime = 0;
    UNLOCK;
}

void
plt_set_scroll (playlist_t *plt, int scroll) {
    plt->scroll = scroll;
}

int
plt_get_scroll (playlist_t *plt) {
    return plt->scroll;
}

static playItem_t *
plt_process_embedded_cue (playlist_t *plt, playItem_t *after, playItem_t *it, int64_t totalsamples, int samplerate) {
    if (plt->loading_cue) {
        // means it was called from _load_cue
        plt->cue_numsamples = totalsamples;
        plt->cue_samplerate = samplerate;
        return NULL;
    }
    playItem_t *cue = NULL;
    pl_lock();
    const char *cuesheet = pl_find_meta (it, "cuesheet");
    if (cuesheet) {
        const char *fname = pl_find_meta (it, ":URI");
        cue = plt_load_cuesheet_from_buffer (plt, after, fname, it, totalsamples, samplerate, (const uint8_t *)cuesheet, (int)strlen (cuesheet), NULL, 0, 0);
    }
    pl_unlock();
    return cue;
}

playItem_t * /* called by plugins */
plt_process_cue (playlist_t *plt, playItem_t *after, playItem_t *it, uint64_t totalsamples, int samplerate) {
    if (plt->loading_cue) {
        // means it was called from _load_cue
        plt->cue_numsamples = totalsamples;
        plt->cue_samplerate = samplerate;
        return NULL;
    }
    return plt_process_embedded_cue (plt, after, it, (int64_t)totalsamples, samplerate);
}

void
pl_configchanged (void) {
    conf_cue_prefer_embedded = conf_get_int ("cue.prefer_embedded", 0);
}

int64_t
pl_item_get_startsample (playItem_t *it) {
    if (!it->has_startsample64) {
        return it->startsample;
    }
    return it->startsample64;
}

int64_t
pl_item_get_endsample (playItem_t *it) {
    if (!it->has_endsample64) {
        return it->endsample;
    }
    return it->endsample64;
}

void
pl_item_set_startsample (playItem_t *it, int64_t sample) {
    it->startsample64 = sample;
    it->startsample = sample >= 0x7fffffff ? 0x7fffffff : (int32_t)sample;
    it->has_startsample64 = 1;
    pl_set_meta_int64 (it, ":STARTSAMPLE", sample);
}

void
pl_item_set_endsample (playItem_t *it, int64_t sample) {
    it->endsample64 = sample;
    it->endsample = sample >= 0x7fffffff ? 0x7fffffff : (int32_t)sample;
    it->has_endsample64 = 1;
    pl_set_meta_int64 (it, ":ENDSAMPLE", sample);
}

int
plt_is_loading_cue (playlist_t *plt) {
    return plt->loading_cue;
}
