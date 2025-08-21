#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <limits.h>

#define BUF_SIZE 1024

static int send_str(int c, const char *s){
    return send(c, s, (int)strlen(s), 0);
}

static int recv_line(int c, char *buf, size_t bufsz){
    size_t used = 0;
    while (used + 1 < bufsz) {
        char ch; ssize_t r = recv(c, &ch, 1, 0);
        if (r <= 0) return -1;
        if (ch == '\n') { buf[used] = '\0'; return (int)used; }
        buf[used++] = ch;
    }
    buf[bufsz-1] = '\0';
    return (int)used;
}

static int recv_n_to_file(int c, const char* fname, long long nbytes){
    FILE *fp = fopen(fname, "wb");
    if (!fp) return -1;
    char buf[BUF_SIZE];
    long long left = nbytes;
    while (left > 0){
        ssize_t r = recv(c, buf, (left > (long long)sizeof(buf)) ? sizeof(buf) : (size_t)left, 0);
        if (r <= 0){ fclose(fp); return -1; }
        if (fwrite(buf,1,(size_t)r,fp)!=(size_t)r){ fclose(fp); return -1; }
        left -= r;
    }
    fclose(fp);
    return 0;
}

static void handle_client(int client){
    char cmd[BUF_SIZE];
    while (1){
        if(recv_line(client, cmd, sizeof(cmd))<0) break;
        if(strcmp(cmd,"exit")==0){ send_str(client,"bye\n"); break; }
        if(strcmp(cmd,"write_file")==0){
            char fname[PATH_MAX];
            if (recv_line(client, fname, sizeof(fname)) < 0) { send_str(client, "filename error\n"); continue; }

            char sizeln[128];
            if (recv_line(client, sizeln, sizeof(sizeln)) < 0) { send_str(client, "size error\n"); continue; }

            long long fsz = -1;
            if (sscanf(sizeln, "SIZE %lld", &fsz) != 1 || fsz < 0) { send_str(client, "bad size\n"); continue; }

            if (recv_n_to_file(client, fname, fsz) == 0) send_str(client, "OK\n");
            else send_str(client, "FAIL\n");
            continue;
        }
        send_str(client,"Unknown cmd\n");
    }
    close(client);
}

int main(){
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr; addr.sin_family=AF_INET;
    addr.sin_port=htons(5000); addr.sin_addr.s_addr=INADDR_ANY;
    bind(s,(struct sockaddr*)&addr,sizeof(addr));
    listen(s,5);
    printf("Server listening...\n");
    while(1){
        int c = accept(s,NULL,NULL);
        if(c>=0){ printf("Client connected\n"); handle_client(c); }
    }
    close(s);
    return 0;
}
