#include "delete_directory.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>


#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define BUF_SIZE 1024
#ifdef _WIN32
  #define CLOSESOCK closesocket
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  static void sleep_seconds(unsigned sec) { Sleep(sec * 1000); }
  static void log_sock_err(const char* msg) { fprintf(stderr, "%s (WSAGetLastError=%ld)\n", msg, (long)WSAGetLastError()); }
  #ifndef strncasecmp
  #define strncasecmp _strnicmp
  #endif
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
  #define CLOSESOCK close
  static void sleep_seconds(unsigned sec) { sleep(sec); }
  static void log_sock_err(const char* msg) { perror(msg); }
#endif

static const char* path_basename(const char* p){
    const char *b = p, *s;
    for (s = p; *s; ++s) if (*s=='/' || *s=='\\') b = s+1;
    return b;
}
static int is_dir_path(const char *p){
    struct stat st;
    return (stat(p, &st) == 0 && S_ISDIR(st.st_mode));
}
static void join_path(char *out, size_t sz, const char *dir, const char *base){
    size_t n = snprintf(out, sz, "%s", dir);
    if (n >= sz) { out[sz-1] = '\0'; return; }
    char sep = (strchr(dir,'\\') && !strchr(dir,'/')) ? '\\' : '/';
    if (n > 0 && out[n-1] != sep) { out[n++] = sep; out[n] = '\0'; }
    strncat(out, base, sz - strlen(out) - 1);
}
static void local_pwd(void){
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) printf("%s\n", cwd);
    else perror("pwd");
}
static void local_cd(const char *path){
    if (!path || !*path) { fprintf(stderr, "cd: missing path\n"); return; }
    if (chdir(path) != 0) perror("cd");
}
static void local_ls(void){
    DIR *d = opendir(".");
    if (!d) { perror("ls"); return; }
    struct dirent *e; int count=0;
    while ((e=readdir(d))){
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        puts(e->d_name); count++;
    }
    if (!count) puts("(empty)");
    closedir(d);
}
static void local_mkdir(const char *name){
    if (!name||!*name){ fprintf(stderr,"mkdir: missing name\n"); return; }
#ifdef _WIN32
    if (mkdir(name)!=0) perror("mkdir");
#else
    if (mkdir(name, 0777)!=0) perror("mkdir");
#endif
}

static void local_rm(const char *path){
    if (!path||!*path){ fprintf(stderr,"rm: missing path\n"); return; }
    if (!strcmp(path,"/")){ fprintf(stderr,"rm: refusing to delete '/'\n"); return; }
    if (delete_directory(path)!=0) fprintf(stderr,"rm: failed\n");
}
static int local_copy_file(const char *src, const char *dst){
    FILE *in=fopen(src,"rb"); if(!in){perror("open src"); return -1;}
    FILE *out=fopen(dst,"wb"); if(!out){perror("open dst"); fclose(in); return -1;}
    char buf[BUF_SIZE]; size_t r;
    while((r=fread(buf,1,sizeof(buf),in))>0){
        if(fwrite(buf,1,r,out)!=r){perror("write dst"); fclose(in); fclose(out); return -1;}
    }
    fclose(in); fclose(out); return 0;
}
static void send_file_chunks(FILE *fp, int sockfd) {
    char data[BUF_SIZE];
    size_t n;
    while ((n = fread(data, 1, sizeof(data), fp)) > 0) {
        ssize_t sent = send(sockfd, data, n, 0);
        if (sent < 0) { perror("send file chunk"); return; }
    }
    if (send(sockfd, "EOF", 3, 0) < 0) perror("send EOF");
}

int main() {
    int sock;
    struct sockaddr_in server;
    char buffer[1000];

#ifdef _WIN32
    WSADATA wsa; if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { log_sock_err("WSAStartup failed"); return 1; }
#endif

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) { printf("Socket creation failed\n"); 
#ifdef _WIN32
        WSACleanup();
#endif
        return 1; }

    server.sin_family = AF_INET;
    server.sin_port   = htons(5000);
    server.sin_addr.s_addr = inet_addr("192.168.0.172");

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("connect failed");
        CLOSESOCK(sock);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    printf("Connected to server.\n");

    for (;;) {
        printf("\nEnter command: ");
        if (!fgets(buffer, sizeof(buffer), stdin)) break;
        buffer[strcspn(buffer, "\n")] = 0;

        if (!strcmp(buffer, "exit")) { printf("Closing connection...\n"); break; }

        if (!strcmp(buffer, "pwd")) { local_pwd(); continue; }
        if (!strncmp(buffer, "cd ", 3)) { local_cd(buffer+3); continue; }
        if (!strcmp(buffer, "ls")) { local_ls(); continue; }
        if (!strncmp(buffer, "mkdir ", 6)) { local_mkdir(buffer+6); continue; }
        if (!strncmp(buffer, "rm ", 3)) { local_rm(buffer+3); continue; }

        if (!strncmp(buffer, "send_file", 9)) {
            char src[PATH_MAX], mode[16], dest[PATH_MAX];

            if (buffer[9] == ' ' && buffer[10] != '\0') {
                strncpy(src, buffer + 10, sizeof(src)-1); src[sizeof(src)-1] = '\0';
            } else {
                printf("Enter full path of file to send: ");
                if (!fgets(src, sizeof(src), stdin)) { perror("fgets"); continue; }
                src[strcspn(src, "\n")] = 0;
            }

            printf("Target (server/local): ");
            if (!fgets(mode, sizeof(mode), stdin)) { perror("fgets"); continue; }
            mode[strcspn(mode, "\n")] = 0;

            if (!strncasecmp(mode, "local", 5)) {
                printf("Enter destination path on client: ");
                if (!fgets(dest, sizeof(dest), stdin)) { perror("fgets"); continue; }
                dest[strcspn(dest, "\n")] = 0;

                int treat_as_dir = 0;
                size_t dl = strlen(dest);
                if (dl && (dest[dl-1]=='/' || dest[dl-1]=='\\')) treat_as_dir = 1;
                if (!treat_as_dir && is_dir_path(dest)) treat_as_dir = 1;

                char finaldst[PATH_MAX];
                if (treat_as_dir) join_path(finaldst, sizeof(finaldst), dest, path_basename(src));
                else { strncpy(finaldst, dest, sizeof(finaldst)-1); finaldst[sizeof(finaldst)-1] = '\0'; }

                if (local_copy_file(src, finaldst) == 0) printf("Local copy OK: %s\n", finaldst);
                else printf("Local copy FAILED\n");
                continue;
            }

            printf("Enter destination directory on server (e.g., . or uploads): ");
            if (!fgets(dest, sizeof(dest), stdin)) { perror("fgets"); continue; }
            dest[strcspn(dest, "\n")] = 0;

            FILE *fp = fopen(src, "rb");
            if (!fp) { perror("open file"); continue; }

            if (dest[0] != '\0' && !(dest[0]=='.' && dest[1]=='\0')) {
                char scd[PATH_MAX+8];
                int m = snprintf(scd, sizeof(scd), "scd %s", dest);
                if (m > 0) {
                    if (send(sock, scd, (size_t)m, 0) < 0) { perror("send scd"); fclose(fp); continue; }
                }
            }

            if (send(sock, "write_file", 10, 0) < 0) { perror("send write_file"); fclose(fp); continue; }

            const char *fname = path_basename(src);
            if (send(sock, fname, strlen(fname), 0) < 0 || send(sock, "\n", 1, 0) < 0) {
                perror("send filename"); fclose(fp); continue;
            }

            send_file_chunks(fp, sock);
            fclose(fp);

            char resp[256] = {0};
            int r = recv(sock, resp, sizeof(resp)-1, 0);
            if (r > 0) { resp[r] = '\0'; printf("Server: %s", resp); }
            continue;
        }

        if (send(sock, buffer, strlen(buffer), 0) < 0) { printf("Send failed\n"); break; }

        char reply[BUF_SIZE] = {0};
        int n = recv(sock, reply, sizeof(reply)-1, 0);
        if (n > 0) {
            reply[n] = '\0';
            printf("Server: %s", reply);
            if (reply[n-1] != '\n') printf("\n");
        } else if (n == 0) {
            printf("Server disconnected\n");
            break;
        } else {
            perror("recv");
            break;
        }
    }

    CLOSESOCK(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
