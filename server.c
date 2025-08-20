// server.c
#include "delete_directory.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <arpa/inet.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define BASE_DIR "/home/ege/project_son"
#include <errno.h>
#define SIZE 1024
#define DEST_DIR "/home/ege/project_son/destination"
#include <pthread.h>
static __thread char client_cwd[PATH_MAX];

void handle_command(int client_socket, char *buffer);
void* client_handler(void* arg) {
    int client_socket = *(int*)arg;
    free(arg); // because we'll malloc for each new client socket

    strncpy(client_cwd, BASE_DIR, sizeof(client_cwd)-1);
    client_cwd[sizeof(client_cwd)-1] = '\0';

    char buffer[1024];
    

    while (1) {
        memset(buffer, 0, sizeof(buffer));
        int received = recv(client_socket, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            printf("Client disconnected.\n");
            break;
        }

        printf("Client sent command: %s\n", buffer);
        
        if (strncmp(buffer, "exit", 4) == 0) {
            handle_command(client_socket, buffer);
            break;
        }

        handle_command(client_socket, buffer);
    }

    close(client_socket);
    return NULL;
}

int secure_cd(const char *path){
    char target[PATH_MAX];
    if (chdir(path) != 0){
        perror("cd error");
        return -1;
    }
    if(!realpath(".", target)){
        perror("realpath error");
        return -1;
    }
    if (strncmp(target, BASE_DIR, strlen(BASE_DIR)) != 0) {
        fprintf(stderr, "Access denied: %s\n", target);
        chdir(BASE_DIR);
        return -1;
}
    return 0;
}

void write_file(int sockfd) {
    int n;
    FILE *fp;
    char filename[256];
    char final_path[PATH_MAX];
    char buffer[SIZE];

    //receive filename
    n = recv(sockfd, filename, sizeof(filename)-1, 0);
    if (n <= 0) {
        perror("Error receiving filename");
        return;
    }
    filename[n] = '\0';

    //base name only
    char *base_name = strrchr(filename, '/');
    base_name = base_name ? base_name + 1 : filename;

    {
        size_t cwd_len = strlen(client_cwd);
        size_t bn_len  = strlen(base_name);
        if (cwd_len + 1 + bn_len + 1 > sizeof(final_path)) {
            const char *m = "Path too long\n";
            send(sockfd, m, strlen(m), 0);
            return;
        }
        memcpy(final_path, client_cwd, cwd_len);
        final_path[cwd_len] = '/';
        memcpy(final_path + cwd_len + 1, base_name, bn_len);
        final_path[cwd_len + 1 + bn_len] = '\0';
    }

    {
        char *last_slash = strrchr(final_path, '/');
        char *dot = strrchr(final_path, '.');
        if (dot && (!last_slash || dot > last_slash)) {
            size_t tail = strlen(dot);
            size_t cur  = strlen(final_path);
            if (cur + 5 + 1 > sizeof(final_path)) {
                const char *m = "Path too long\n";
                send(sockfd, m, strlen(m), 0);
                return;
            }
            memmove(dot + 5, dot, tail + 1);
            memcpy(dot, "_rcvd", 5);
        } else {
            size_t cur = strlen(final_path);
            if (cur + 5 + 1 > sizeof(final_path)) {
                const char *m = "Path too long\n";
                send(sockfd, m, strlen(m), 0);
                return;
            }
            memcpy(final_path + cur, "_rcvd", 5);
            final_path[cur + 5] = '\0';
        }
    }

    printf("Saving file as: %s\n", final_path);
    fp = fopen(final_path, "wb");
    if (fp == NULL) {
        perror("File creation failed");
        return;
    }
    send(sockfd, "Ready to receive", strlen("Ready to receive"), 0);

    for (;;) {
        n = recv(sockfd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;

        if (n == 3 && memcmp(buffer, "EOF", 3) == 0) {
            printf("File transfer complete\n");
            const char *done = "File received successfully";
            send(sockfd, done, strlen(done), 0);
            break;
        }

        if ((size_t)fwrite(buffer, 1, n, fp) != (size_t)n) {
            perror("fwrite");
            break;
        }
    }

    fclose(fp);
}

//cpwd
static void cmd_cpwd(int sock) {
    char *rem = strstr(client_cwd, "project_son");
    char out[PATH_MAX];

    if (!rem) {
        const char *msg = "Error: path not found\n";
        send(sock, msg, strlen(msg), 0);
        return;
    }

    size_t len = strnlen(rem, sizeof(out) - 2); 
    memcpy(out, rem, len);
    out[len++] = '\n';   // add newline
    out[len] = '\0';     // null-terminate

    send(sock, out, len, 0);
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

//ccd
static void cmd_ccd(int sock, const char *path) {
    char candidate[PATH_MAX];
    char resolved[PATH_MAX];

    if (path == NULL || path[0] == '\0') {
        const char *m = "ccd: missing path\n";
        send(sock, m, strlen(m), 0);
        return;
    }

    if (strcmp(path, "..") == 0) {
        size_t base_len = strlen(BASE_DIR);
        size_t len = strlen(client_cwd);
        if (len <= base_len) {
            const char *m = "Already at base\n";
            send(sock, m, strlen(m), 0);
            return;
        }
        while (len > base_len + 1 && client_cwd[len - 1] == '/') len--;

        size_t i = len;
        while (i > base_len && client_cwd[i - 1] != '/') i--;
        if (i <= base_len) {
            const char *m = "Already at base\n";
            send(sock, m, strlen(m), 0);
            return;
        }

        size_t newlen = (i >= 1) ? (i - 1) : 0;
        if (newlen + 1 > sizeof(candidate)) {
            const char *m = "Path too long\n";
            send(sock, m, strlen(m), 0);
            return;
        }
        memcpy(candidate, client_cwd, newlen);
        candidate[newlen] = '\0';

    } else {
        if (path[0] == '/') {
            size_t b = strlen(BASE_DIR);
            size_t p = strlen(path);
            if (b + p + 1 > sizeof(candidate)) {
                const char *m = "Path too long\n";
                send(sock, m, strlen(m), 0);
                return;
            }
            memcpy(candidate, BASE_DIR, b);
            memcpy(candidate + b, path, p);
            candidate[b + p] = '\0';

        } else {
            size_t base = strlen(client_cwd);
            size_t plen = strlen(path);
            if (base + 1 + plen + 1 > sizeof(candidate)) {
                const char *m = "Path too long\n";
                send(sock, m, strlen(m), 0);
                return;
            }
            memcpy(candidate, client_cwd, base);
            candidate[base] = '/';
            memcpy(candidate + base + 1, path, plen);
            candidate[base + 1 + plen] = '\0';
        }
    }

    if (!realpath(candidate, resolved)) {
        const char *m = "Directory change failed\n";
        send(sock, m, strlen(m), 0);
        return;
    }
    if (strncmp(resolved, BASE_DIR, strlen(BASE_DIR)) != 0) {
        const char *m = "Access denied\n";
        send(sock, m, strlen(m), 0);
        return;
    }
    if (!is_dir(resolved)) {
        const char *m = "Not a directory\n";
        send(sock, m, strlen(m), 0);
        return;
    }

    strncpy(client_cwd, resolved, sizeof(client_cwd) - 1);
    client_cwd[sizeof(client_cwd) - 1] = '\0';
    {
        const char *ok = "Directory changed (client only)\n";
        send(sock, ok, strlen(ok), 0);
    }
}

static void cmd_cls(int sock) {
    DIR *d = opendir(client_cwd);
    if (!d) {
        const char *msg = "cls: cannot open directory\n";
        send(sock, msg, strlen(msg), 0);
        return;
    }

    struct dirent *entry;
    char out[1024];
    int count = 0;

    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        int m = snprintf(out, sizeof(out), "%s\n", entry->d_name);
        if (m > 0) send(sock, out, (size_t)m, 0);
        count++;
    }

    closedir(d);

    if (count == 0) {
        const char *empty = "(empty)\n";
        send(sock, empty, strlen(empty), 0);
    }
}

void handle_command(int client_socket, char *buffer) {
    buffer[strcspn(buffer, "\r\n")] = 0;

    
    if (strcmp(buffer, "cpwd") == 0) {
        cmd_cpwd(client_socket);
        return;
    }
    if (strncmp(buffer, "ccd ", 4) == 0) {
        cmd_ccd(client_socket, buffer + 4);
        return;
    }
    if (strcmp(buffer, "cls") == 0) {
        cmd_cls(client_socket);
        return;
    }

    if (strncmp(buffer, "write_file", 10) == 0) {
        write_file(client_socket);
    }
    else if (strncmp(buffer, "exit", 4) == 0) {
        send(client_socket, "Goodbye\n", strlen("Goodbye\n"), 0);
        return;  
    }

    // ls  
    else if (strcmp(buffer, "ls") == 0) {
        DIR *d = opendir(".");
        if (!d) {
            const char *msg = "ls: cannot open directory\n";
            send(client_socket, msg, strlen(msg), 0);
        } else {
            struct dirent *entry;
            char out[1024];
            int count = 0;

            while ((entry = readdir(d)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                    continue;

                int m = snprintf(out, sizeof(out), "%s\n", entry->d_name);
                if (m > 0) send(client_socket, out, (size_t)m, 0);
                count++;
            }

            closedir(d);

            if (count == 0) {
                const char *empty = "(empty)\n";
                send(client_socket, empty, strlen(empty), 0);
            }
        }
    }

    // mkdir 
    else if (strncmp(buffer, "mkdir ", 6) == 0) {
        char *dir_name = buffer + 6;
        dir_name[strcspn(dir_name, "\n")] = '\0';

        if (mkdir(dir_name, 0777) == 0) {
            send(client_socket, "Directory created\n", strlen("Directory created\n"), 0);
        } else {
            send(client_socket, "Failed to create directory\n", strlen("Failed to create directory\n"), 0);
        }
    }

    // rm 
    else if (strncmp(buffer, "rm ", 3) == 0) {
        char *tbd = buffer + 3;
        tbd[strcspn(tbd, "\n")] = '\0';

        if (delete_directory(tbd) == 0) {
            send(client_socket, "Deleted\n", strlen("Deleted\n"), 0);
        } else {
            send(client_socket, "Failed to delete\n", strlen("Failed to delete\n"), 0);
        }
    }

    // rename 
    else if (strncmp(buffer, "rename ", 7) == 0) {
        char *arg = buffer + 7;
        char *o_name = strtok(arg, " \n");
        char *n_name = strtok(NULL, " \n");

        if (o_name != NULL && n_name != NULL) {
            if (rename(o_name, n_name) == 0) {
                send(client_socket, "Renamed\n", strlen("Renamed\n"), 0);
            } else {
                send(client_socket, "Rename failed\n", strlen("Rename failed\n"), 0);
            }
        } else {
            send(client_socket, "Invalid rename command\n", strlen("Invalid rename command\n"), 0);
        }
    }

    // cd 
    else if (strncmp(buffer, "cd ", 3) == 0) {
        char *dirpath = buffer + 3;
        dirpath[strcspn(dirpath, "\n")] = '\0';

        if (strcmp(dirpath, "..") == 0) {
            if (secure_cd("..") == 0) {
                send(client_socket, "Moved up\n", strlen("Moved up\n"), 0);
            } else {
                if (errno == EPERM) {
                    send(client_socket, "Already at base\n", strlen("Already at base\n"), 0);
                } else {
                    send(client_socket, "Failed to move up\n", strlen("Failed to move up\n"), 0);
                }
            }
        } else {
            if (secure_cd(dirpath) == 0) {
                send(client_socket, "Directory changed\n", strlen("Directory changed\n"), 0);
            } else {
                    send(client_socket, "Directory change failed\n", strlen("Directory change failed\n"), 0);
            }
        }
    }

    // pwd
    else if (strcmp(buffer, "pwd") == 0) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            const char *rem = strstr(cwd, "project_son");

            char out[PATH_MAX + 2];
            size_t len = strnlen(rem, sizeof(out) - 2);
            memcpy(out, rem, len);
            send(client_socket, out, len, 0);
        }
        else {
            const char *err = "pwd fail";
            send(client_socket, err, strlen(err), 0);
        }    
        }
        
    }

int main() {
    int server_socket;
    struct sockaddr_in server_address, client_address;
    socklen_t client_len = sizeof(client_address);

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Socket creation failed");
        exit(1);
    }

    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(5000);

    if (bind(server_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        perror("Bind failed");
        exit(1);
    }

    if (listen(server_socket, 5) < 0) {
        perror("Listen failed");
        exit(1);
    }

    printf("Server is listening...\n");

    while (1) {
        int* new_sock = malloc(sizeof(int));
        *new_sock = accept(server_socket, (struct sockaddr*)&client_address, &client_len);
        if (*new_sock < 0) {
            perror("Accept failed");
            free(new_sock);
            continue;
        }

        printf("Client connected\n");

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler, new_sock) != 0) {
            perror("Could not create thread");
            close(*new_sock);
            free(new_sock);
        } else {
            pthread_detach(tid);
        }
    }

    close(server_socket);
    return 0;
}
