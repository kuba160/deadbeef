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


/*
* ======= Description of Algorthim =======
* The hash algorithim I have implemented is called tabulated hashing. It's a
* very straight forward idea for hashing strings. Simply generate a table of
* random numbers for each possible value a character or unit in a string. Then
* take your current hash value and rotate it by 1 bit. Then take the current
* unit in the string and look it up in your table and xor with the rotated
* current value. Repeat these for each character/unit in your string.
*
* It also has a couple nice properties. First it really easy to generate a new
* function just generate a new randome table. Useful for any algorthims that
* require to generate new hash functions on the fly. Secondly, it's pair-wise
* independent except till the strings get longer. The number of pair-wise
* independent bits in the hash output for a 64 bit hash is: 64 - L - 1
* Where L is the length of the string.
*/

#include "hash.h"

// openssl rand -hex 128
static const uint64_t v_tbl[] = {
    0x50b39fd87724c98c, 0x8ddbf0214aa3cbd5, 0x4064b5fe42f09d60, 0x8f03d6c854174876,
    0x1bf66d8086e397ad, 0x2612f2fc45376459, 0x0fe4a54bc60101fd, 0x779f417e3bf149c7,
    0x40556ac5dce90ba9, 0xdf15b4e8cc47fa46, 0x297191c88722e4ee, 0x1f9bcf9b21cf8079,
    0xb47980b136b3dea1, 0xe13403a9095790f9, 0x9af58de272a90ef9, 0xf0f0e29f65df5162
};
// bitwise rotate left, compilers optimize this to a rotate instruction.
#define ROL64_R(x, y) ((x >> y) | ( x << (64 - y)))

// Frac(sqrt(2)) mainly for zero length strings could be any random value.
#define HASH_INIT 0x6a09e667f3bcc908

// Instead of a table 256 entries which would take 256 * 8 Bytes or 2 KB it is
// more efficeint to split a byte in half reducing the table to only 128 bytes
// small enough to fit in just a couple cache lines. Also small enough to not
// cause issues on most micro-controllers.

static inline uint64_t
hash_one_byte (uint64_t cur_hash, uint8_t byte) {
    cur_hash  = ROL64_R (cur_hash, 1);
    cur_hash ^= v_tbl[byte >> 4];  // hash upper half
    cur_hash  = ROL64_R (cur_hash, 1);
    cur_hash ^= v_tbl[byte & 0xf]; // hash lower half
    return cur_hash;
}

uint64_t
hash_bytes (const uint8_t* bytes, size_t len) {
    uint64_t cur_hash = HASH_INIT;
    for (size_t i = 0; i < len; i++) {
        cur_hash = hash_one_byte (cur_hash, bytes[i]);
    }
    return cur_hash;
}

uint64_t
hash_string_continue (uint64_t hash, const char* str) {
    uint64_t cur_hash = hash;
    const char* c = str;
    while (*c != '\0') {
        cur_hash = hash_one_byte (cur_hash, (uint8_t)(*c));
        ++c;
    }
    return cur_hash;
}

uint64_t
hash_string (const char* str) {
    return hash_string_continue (HASH_INIT, str);
}
