#include <windows.h>
#include <wchar.h>

typedef	unsigned short mode_t;

// TODO: Maybe also take into account mode.
int
win_mkdir (const char *path, mode_t mode) {
    // get length includeing NULL terminater
    int utf16_points = MultiByteToWideChar (CP_UTF8, /*Flags*/ 0, path, -1, NULL, 0);
    if (utf16_points < 1) {
        return -1;
    }
    wchar_t wPath[utf16_points];
    memset(wPath, 0, sizeof(wchar_t) * utf16_points);
    if(MultiByteToWideChar (CP_UTF8, /*Flags*/ 0, path, -1, wPath, utf16_points) < 1) {
        return -1;
    }
    return _wmkdir (wPath);
}