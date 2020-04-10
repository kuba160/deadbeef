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
#ifndef __ARTWORK_CACHE_PATHS_H
#define __ARTWORK_CACHE_PATHS_H

int
make_cache_root_path (char *path, const size_t size);

int
make_cache_dir_path (char *path, int size, const char *artist, int img_size);

int
make_cache_path2 (char *path, int size, const char *fname, const char *album, const char *artist, int img_size);

void
make_cache_path (char *path, int size, const char *album, const char *artist, int img_size);

#endif /*__ARTWORK_CACHE_PATHS_H*/
