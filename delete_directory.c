//delete_directory.c
#include "delete_directory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

int delete_directory(const char *path) {
    DIR *dir = opendir(path);
    if (dir == NULL) {
        perror("opendir");
        return -1;
    }

    struct dirent *entry;
    char full_path[1024];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        struct stat statbuf;
        if (stat(full_path, &statbuf) == 0) {
            if (S_ISDIR(statbuf.st_mode)) {
                if (delete_directory(full_path) != 0) {
                    closedir(dir);
                    return -1;
                }
            } else {
                if (unlink(full_path) != 0) {
                    perror("unlink");
                    closedir(dir);
                    return -1;
                }
            }
        }
    }

    closedir(dir);
    if (rmdir(path) != 0) {  
        perror("rmdir");
        return -1;
    }
    return 0;
}

