/*
    Tabulated Hashing
    Copyright (C) 2020 Keith Cancel <admin@keith.pro>

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
#ifndef _TAB_HASH_H
#define _TAB_HASH_H

#include <stdint.h>
#include <wchar.h>

uint64_t
hash_bytes (const uint8_t* bytes, size_t len);

// input is a null terminated string
uint64_t
hash_string (const char* str);

// Easy calculation for concatenated strings without have to concat in memory.
uint64_t
hash_string_continue (uint64_t hash, const char* str);

#endif // _TAB_HASH_H
