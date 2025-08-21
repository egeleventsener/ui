#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define rmdir _rmdir
#define unlink _unlink
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#define _GNU_SOURCE
#include "delete_directory.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32
// Windows’ta basit recursive silme (symlink yok)
static int remove_entry(const char *path) {
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return -1;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        WIN32_FIND_DATAA ffd;
        char search[MAX_PATH];
        snprintf(search, sizeof(search), "%s\\*", path);
        HANDLE h = FindFirstFileA(search, &ffd);
        if (h == INVALID_HANDLE_VALUE) return -1;
        do {
            if (!strcmp(ffd.cFileName, ".") || !strcmp(ffd.cFileName, "..")) continue;
            char child[MAX_PATH];
            snprintf(child, sizeof(child), "%s\\%s", path, ffd.cFileName);
            remove_entry(child);
        } while (FindNextFileA(h, &ffd));
        FindClose(h);
        return _rmdir(path);
    } else {
        return _unlink(path);
    }
}
#else
// Linux sürümü (senin yazdığın lstat+S_ISLNK’li olan)
static int remove_entry(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *e;
        char child[PATH_MAX];
        int ret = 0;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            if (remove_entry(child) != 0) { ret=-1; break; }
        }
        closedir(d);
        if (ret == 0) ret = rmdir(path);
        return ret;
    } else {
        return unlink(path);
    }
}
#endif

