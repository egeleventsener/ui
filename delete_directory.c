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
            if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) < 0) { ret=-1; break; }
            if (remove_entry(child) != 0) { ret=-1; break; }
        }
        closedir(d);
        if (ret == 0) ret = rmdir(path);
        return ret;
    } else {
        return unlink(path);
    }
}

int delete_directory(const char *path) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    if (!strcmp(path, "/")) { errno = EPERM; return -1; }
    return remove_entry(path);
}
