/*
    Album Art plugin for DeaDBeeF
    Copyright (C) 2009-2011 Viktor Semykin <thesame.ml@gmail.com>
    Copyright (C) 2009-2013 Alexey Yakovenko <waker@users.sourceforge.net>

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
*/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>

#include "cache_paths.h"
#include "../../common.h" // For CACHEDIR & HOMEDIR

// Get rid of redefinition warnings.
#ifdef trace
#undef trace
#endif
extern DB_functions_t *deadbeef;
#define trace(...) { deadbeef->log (__VA_ARGS__); }

// Maybe put this in utils?
#ifdef __MINGW32__
#define canonicalize_nix(str) replace_char (str, '\\', '/')
#define canonicalize_sys(str) replace_char (str, '/', '\\')
#else
#define canonicalize_nix(str)
#define canonicalize_sys(str)
#endif

static void
replace_char (char* str, char c, char r) {
    while (*str != '\0') {
        if (*str == c) {
            *str = r;
        }
        ++str;
    }
}

static int
is_illegal_windows_char(char c) {
    // Forbidden characters in a windows file name: <>:"/\|?* and codepoints with value < 32
    // https://docs.microsoft.com/en-us/windows/win32/fileio/naming-a-file#naming-conventions
    const char bad_chars[] = { '<', '>', ':', '"', '/', '\\', '|', '?', '*' };
    for (int i = 0; i < sizeof(bad_chars); i++) {
        if(c == bad_chars[i]) {
            return 1;
        }
    }
    return 0;
}

static int
is_illegal_windows_name (const char* str) {
    // A file can't be named these regardless of case.
    // "CON", "PRN", "AUX", "NUL",
    // "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
    // "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"

    // Also if a dot follows right after any of these it's forbidden as well
    // since windows treats that as a file extension.
    static const uint32_t com = ('C' << 16) | ('O' << 8) | 'M';
    static const uint32_t lpt = ('L' << 16) | ('P' << 8) | 'T';
    static const uint32_t names[] = {
        ('A' << 16) | ('U' << 8) | 'X',
        ('C' << 16) | ('O' << 8) | 'N',
        ('N' << 16) | ('U' << 8) | 'L',
        ('P' << 16) | ('R' << 8) | 'N'
    };
    uint32_t val = 0;
    for (int i = 0; i < 3; i++) {
        char tmp = str[i];
        if(str[i] == '\0') {
            return 0;
        }
        val <<= 8;
        val  |= tmp;
    }
    val &= 0xdfdfdf; // Make case insensitive.
    if (str[3] >= '0' && str[3] <= '9' && (val == com || val == lpt) && (str[4] == '\0' || str[4] == '.')) {
        return 1;
    }
    if (str[3] == '\0' || str[3] == '.') {
        for (int i = 0; i < 4; i++) {
            if(val == names[i]) {
                return 1;
            }
        }
    }
    return 0;
}

// Maybe put this in utils as well?
static size_t
sanitize_name_for_file_system (const char* name, char* clean, size_t clean_capacity) {
    if(clean_capacity < 1) {
        return 0;
    }
    // Trim leading white spaces
    while (isspace (*name)) {
        name++;
    }

    // Most Unix OSes only care about `/`, and `NULL`, but Mac OS does not like `:`
    // https://en.wikipedia.org/wiki/Filename#Comparison_of_filename_limitations
    size_t length = 0;
    const char* name_end = name;
    // An other possibility is escape to hex, but the old esc_char function just used `_`
    while (*name_end != '\0' && (length + 1) < clean_capacity) {
        char c = *name_end;
        if (isspace (c)) {
            c = ' ';
        }
        if (c == '/') {
            c = '\\';
        }
        if (c == ':') {
            c = '_';
        }
        // While only a restriction on windows these seem like a bad idea in general
        if (c < 32) {
            c = '_';
        }
        #ifdef __MINGW32__
        if (is_illegal_windows_char (c)) {
            c = '_';
        }
        #endif

        clean[length] = c;
        length++;
        name_end++;
    }
    clean[length] = '\0';
    //  Just an empty string nothing more process.
    if(length < 1) {
        return 0;
    }

    // A leading dash can make interacting with a file from the commandline a pain
    if(clean[0] == '-') {
        clean[0] = '_';
    }

    // Adjust length to trim trailing spaces
    while (length > 0 && isspace (*(clean + length - 1))) {
        length--;
    }

    #ifdef __MINGW32__
    // prepend an underscore to the name
    if (is_illegal_windows_name (clean)) {
        if(length + 1 >= clean_capacity) {
            length--; // truncate by one then
        }
        memmove (clean + 1, clean, length);
        length++;
        clean[0] = '_';
        clean[length] = '\0';
    }
    // File names can't end with a period `.` on windows
    if(clean[length - 1] == '.') {
        clean[length - 1] = '_';
    }
    #endif
    return length;
}

// esc_char is needed to prevent using file path separators,
// e.g. to avoid writing arbitrary files using "../../../filename"
static char
esc_char (char c) {
#ifndef WIN32
    if (c == '/') {
        return '\\';
    }
#else
    if (c == '/' || c == ':') {
        return '_';
    }
#endif
    return c;
}

int
make_cache_root_path (char *path, const size_t size) {
    const char usr_dir[] = "/.cache/deadbeef/";
    const char sys_dir[] = "/deadbeef/";
    const char *base_dir = getenv (CACHEDIR);
    const char *sub_dir  = base_dir ? sys_dir : usr_dir;
    size_t      sub_len  = (base_dir ? sizeof(sys_dir): sizeof(usr_dir)) - 1;

    memset (path, '\0', size);
    base_dir = base_dir ? base_dir : getenv (HOMEDIR);
    if (!base_dir) {
        trace ("Artwork File Cache: Can't find a suitable cache root directory in the environment variables!\n");
        return -1;
    }

    size_t base_len = strlen (base_dir);
    if ((base_len + sub_len) >= size) {
        trace ("Artwork File Cache: Cache root directory longer than %d bytes\n", (int)size - 1);
        return -1;
    }
    memcpy (path, base_dir, base_len);
    memcpy (path + base_len, sub_dir, sub_len);
    canonicalize_nix (path);
    return 0;
}

int
make_cache_dir_path (char *path, int size, const char *artist, int img_size) {
    char esc_artist[NAME_MAX] = "Unknown artist";
    if (artist) {
        sanitize_name_for_file_system(artist, esc_artist, NAME_MAX);
    }

    if (make_cache_root_path (path, size) < 0) {
        return -1;
    }

    const size_t size_left = size - strlen (path);
    int path_length;
    if (img_size == -1) {
        path_length = snprintf (path+strlen (path), size_left, "covers/%s/", esc_artist);
    }
    else {
        path_length = snprintf (path+strlen (path), size_left, "covers-%d/%s/", img_size, esc_artist);
    }
    if (path_length >= size_left) {
        trace ("Cache path truncated at %d bytes\n", size);
        return -1;
    }

    return 0;
}

int
make_cache_path2 (char *path, int size, const char *fname, const char *album, const char *artist, int img_size) {
    path[0] = '\0';

    if (!album || !*album) {
        if (fname) {
            album = fname;
        }
        else if (artist && *artist) {
            album = artist;
        }
        else {
            trace ("not possible to get any unique album name\n");
            return -1;
        }
    }
    if (!artist || !*artist) {
        artist = "Unknown artist";
    }

    #ifdef __MINGW32__
    if (make_cache_dir_path (path, size, artist, img_size)) {
        return -1;
    }
    #else
    if (make_cache_dir_path (path, size-NAME_MAX, artist, img_size)) {
        return -1;
    }
    #endif

    int max_album_chars = min (NAME_MAX, size - strlen (path)) - sizeof ("1.jpg.part");
    #ifdef __MINGW32__
    // override char limit todo
    max_album_chars = 250;
    #endif
    if (max_album_chars <= 0) {
        trace ("Path buffer not long enough for %s and filename\n", path);
        return -1;
    }

    char esc_album[max_album_chars+1];
    const char *palbum = strlen (album) > max_album_chars ? album+strlen (album)-max_album_chars : album;
    size_t i = 0;
    do {
        esc_album[i] = esc_char (palbum[i]);
    } while (palbum[i++]);

    sprintf (path+strlen (path), "%s%s", esc_album, ".jpg");
    return 0;
}

void
make_cache_path (char *path, int size, const char *album, const char *artist, int img_size) {
    make_cache_path2 (path, size, NULL, album, artist, img_size);
}
