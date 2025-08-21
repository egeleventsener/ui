#include "delete_directory.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#ifdef _WIN32
  #define CLOSESOCK closesocket
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #ifndef strncasecmp
  #define strncasecmp _strnicmp
  #endif
#else
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <arpa/inet.h>
  #define CLOSESOCK close
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define BUF_SIZE 1024

static const char* path_basename(const char* p){ const char *b=p,*s; for(s=p;*s;++s) if(*s=='/'||*s=='\\') b=s+1; return b; }
static int is_dir_path(const char *p){ struct stat st; return (stat(p,&st)==0 && S_ISDIR(st.st_mode)); }
static void join_path(char *out, size_t sz, const char *dir, const char *base){ size_t n=snprintf(out,sz,"%s",dir); if(n>=sz){out[sz-1]='\0';return;} char sep=(strchr(dir,'\\')&&!strchr(dir,'/'))?'\\':'/'; if(n>0&&out[n-1]!=sep){out[n++]=sep; out[n]='\0';} strncat(out,base,sz-strlen(out)-1); }
static void local_pwd(void){ char cwd[PATH_MAX]; if(getcwd(cwd,sizeof(cwd))) printf("%s\n",cwd); else perror("pwd"); }
static void local_cd(const char *path){ if(!path||!*path){fprintf(stderr,"cd: missing path\n");return;} if(chdir(path)!=0) perror("cd"); }
static void local_ls(void){ DIR *d=opendir("."); if(!d){perror("ls");return;} struct dirent *e; int count=0; while((e=readdir(d))){ if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue; puts(e->d_name); count++; } if(!count) puts("(empty)"); closedir(d); }
static void local_mkdir(const char *name){
    if(!name||!*name){ fprintf(stderr,"mkdir: missing name\n"); return; }
#ifdef _WIN32
    if(mkdir(name)!=0) perror("mkdir");
#else
    if(mkdir(name,0777)!=0) perror("mkdir");
#endif
}
static void local_rm(const char *path){ if(!path||!*path){fprintf(stderr,"rm: missing path\n");return;} if(!strcmp(path,"/")){fprintf(stderr,"rm: refusing to delete '/'\n");return;} if(delete_directory(path)!=0) fprintf(stderr,"rm: failed\n"); }

static int send_all(int sock, const void* buf, size_t len){ const char* p=(const char*)buf; size_t sent=0; while(sent<len){ int r=send(sock, p+sent, (int)(len-sent), 0); if(r<=0) return -1; sent += (size_t)r; } return 0; }

static int send_file_with_size(FILE *fp, int sock){
    long long fsz=0;
#ifdef _WIN32
    _fseeki64(fp,0,SEEK_END); fsz=_ftelli64(fp); _fseeki64(fp,0,SEEK_SET);
#else
    fseeko(fp,0,SEEK_END); fsz=ftello(fp); fseeko(fp,0,SEEK_SET);
#endif
    if(fsz<0) return -1;
    char hdr[64]; int m = snprintf(hdr,sizeof(hdr),"SIZE %lld\n", fsz);
    if(m<=0 || send_all(sock, hdr, (size_t)m)<0) return -1;
    char data[BUF_SIZE]; size_t n;
    while((n=fread(data,1,sizeof(data),fp))>0){ if(send_all(sock,data,n)<0) return -1; }
    return 0;
}

int main(){
    int sock;
    struct sockaddr_in server;
    char buffer[1000];

#ifdef _WIN32
    WSADATA wsa; if(WSAStartup(MAKEWORD(2,2), &wsa)!=0){ fprintf(stderr,"WSAStartup failed (%ld)\n",(long)WSAGetLastError()); return 1; }
#endif

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock==-1){
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    server.sin_family = AF_INET;
    server.sin_port   = htons(5000);
    server.sin_addr.s_addr = inet_addr("192.168.0.172");

    if(connect(sock,(struct sockaddr*)&server,sizeof(server))<0){
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    printf("Connected to server.\n");

    for(;;){
        printf("\nEnter command: ");
        if(!fgets(buffer,sizeof(buffer),stdin)) break;
        buffer[strcspn(buffer,"\n")] = 0;

        if(!strcmp(buffer,"exit")){ printf("Closing connection...\n"); break; }

        if(!strcmp(buffer,"pwd")){ local_pwd(); continue; }
        if(!strncmp(buffer,"cd ",3)){ local_cd(buffer+3); continue; }
        if(!strcmp(buffer,"ls")){ local_ls(); continue; }
        if(!strncmp(buffer,"mkdir ",6)){ local_mkdir(buffer+6); continue; }
        if(!strncmp(buffer,"rm ",3)){ local_rm(buffer+3); continue; }

        if(!strncmp(buffer,"send_file",9)){
            char src[PATH_MAX], mode[16], dest[PATH_MAX];

            if(buffer[9]==' ' && buffer[10]!='\0'){ strncpy(src, buffer+10, sizeof(src)-1); src[sizeof(src)-1]='\0'; }
            else {
                printf("Enter full path of file to send: ");
                if(!fgets(src,sizeof(src),stdin)) continue;
                src[strcspn(src,"\n")] = 0;
            }

            printf("Target (server/local): ");
            if(!fgets(mode,sizeof(mode),stdin)) continue;
            mode[strcspn(mode,"\n")] = 0;

            if(!strncasecmp(mode,"local",5)){
                printf("Enter destination path on client: ");
                if(!fgets(dest,sizeof(dest),stdin)) continue;
                dest[strcspn(dest,"\n")] = 0;
                int treat_dir=0; size_t dl=strlen(dest);
                if(dl && (dest[dl-1]=='/'||dest[dl-1]=='\\')) treat_dir=1;
                if(!treat_dir && is_dir_path(dest)) treat_dir=1;
                char finaldst[PATH_MAX];
                if(treat_dir) join_path(finaldst,sizeof(finaldst),dest,path_basename(src));
                else { strncpy(finaldst,dest,sizeof(finaldst)-1); finaldst[sizeof(finaldst)-1]='\0'; }
                FILE *in=fopen(src,"rb"); if(!in){ perror("open src"); continue; }
                FILE *out=fopen(finaldst,"wb"); if(!out){ perror("open dst"); fclose(in); continue; }
                char bufc[BUF_SIZE]; size_t r;
                while((r=fread(bufc,1,sizeof(bufc),in))>0){ if(fwrite(bufc,1,r,out)!=r){ perror("write dst"); break; } }
                fclose(in); fclose(out);
                printf("Local copy done: %s\n", finaldst);
                continue;
            }

            printf("Enter destination directory on server (e.g., . or uploads): ");
            if(!fgets(dest,sizeof(dest),stdin)) continue;
            dest[strcspn(dest,"\n")] = 0;

            FILE *fp = fopen(src,"rb");
            if(!fp){ perror("open file"); continue; }

            if(dest[0]!='\0' && !(dest[0]=='.' && dest[1]=='\0')){
                char scd[PATH_MAX+8]; int m=snprintf(scd,sizeof(scd),"scd %s",dest);
                if(m>0){ if(send_all(sock, scd, (size_t)m)<0 || send_all(sock,"\n",1)<0){ perror("send scd"); fclose(fp); continue; } }
            }

            if(send_all(sock,"write_file\n",11)<0){ perror("send write_file"); fclose(fp); continue; }

            const char *fname = path_basename(src);
            if(send_all(sock, fname, strlen(fname))<0 || send_all(sock,"\n",1)<0){ perror("send filename"); fclose(fp); continue; }

            if(send_file_with_size(fp, sock)<0){ perror("send file"); fclose(fp); continue; }
            fclose(fp);

            char resp[256]={0}; int rcv = recv(sock, resp, sizeof(resp)-1, 0);
            if(rcv>0){ resp[rcv]='\0'; printf("Server: %s", resp); }
            continue;
        }

        if(send_all(sock, buffer, strlen(buffer))<0 || send_all(sock,"\n",1)<0){ printf("Send failed\n"); break; }

        char reply[BUF_SIZE]={0}; int n = recv(sock, reply, sizeof(reply)-1, 0);
        if(n>0){ reply[n]='\0'; printf("Server: %s", reply); if(reply[n-1]!='\n') printf("\n"); }
        else if(n==0){ printf("Server disconnected\n"); break; }
        else { perror("recv"); break; }
    }

    CLOSESOCK(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
