//client.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <limits.h>
#define SIZE 1024
#ifndef PATH_MAX
#define PATH_MAX 4096 
#endif

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>   // Sleep
  // For MSVC you can also keep this; harmless for MinGW:
  #pragma comment(lib, "ws2_32.lib")
  #define CLOSESOCK closesocket
  static void sleep_seconds(unsigned sec) { Sleep(sec * 1000); }
#else
  #include <unistd.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <arpa/inet.h>
  #define CLOSESOCK close
  static void sleep_seconds(unsigned sec) { sleep(sec); }
#endif


void send_file(FILE *fp, int sockfd, const char *filepath) {
    char data[SIZE] = {0};
    size_t n;

    // Send filename
    if (send(sockfd, filepath, strlen(filepath), 0) < 0) {
        perror("Error sending filepath");
        return;
    }

    // Wait for server ready signal
    char response[SIZE];
    if (recv(sockfd, response, SIZE, 0) <= 0) {
        perror("No response from server");
        return;
    }

    // Read and send file content
    while ((n = fread(data, 1, SIZE, fp)) > 0) {
        if (send(sockfd, data, n, 0) == -1) {
            perror("Error sending file content");
            return;
        }
        memset(data, 0, SIZE);
    }
    
    // Send EOF marker
    sleep(1); 
    if (send(sockfd, "EOF", 3, 0) < 0) {
        perror("Error sending EOF marker");
        return;
    }

    // Wait for completion confirmation
    if (recv(sockfd, response, SIZE, 0) > 0) {
        printf("Server: %s\n", response);
    }
}

int main() {
    int sock;
    struct sockaddr_in server;
    char buffer[1000], server_reply[2000];

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        printf("Socket creation failed\n");
        return 1;
    }

    server.sin_addr.s_addr = inet_addr("192.168.0.172");
    server.sin_family = AF_INET;
    server.sin_port = htons(5000);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("connect failed");
        return 1;
    }

    printf("Connected to server.\n");

    while (1) {
        printf("\nEnter command: ");
        fgets(buffer, sizeof(buffer), stdin);
        buffer[strcspn(buffer, "\n")] = 0;

        if (strcmp(buffer, "exit") == 0) {
            printf("Closing connection...\n");
            break;
        }

        // Send command to server
        if (send(sock, buffer, strlen(buffer), 0) < 0) {
            printf("Send failed\n");
            break;
        }

        if (strncmp(buffer, "write_file", 10) == 0) {
            char filepath[PATH_MAX];
            printf("Enter full path of file to send: ");
            fgets(filepath, sizeof(filepath), stdin);
            filepath[strcspn(filepath, "\n")] = 0;

            FILE *fp = fopen(filepath, "rb");
            if (fp == NULL) {
                printf("Error: Cannot open file '%s'\n", filepath);
                perror("Reason");
                continue;
            }

            send_file(fp, sock, filepath);
            fclose(fp);
            continue;  
        } else {
            
            char server_reply[SIZE];
            memset(server_reply, 0, SIZE);
            int recv_size = recv(sock, server_reply, SIZE-1, 0);
            if (recv_size > 0) {
                server_reply[recv_size] = '\0';
                printf("Server: %s\n", server_reply);
            } else {
                printf("Server disconnected\n");
                break;
            }
        }
    }

    close(sock);
    return 0;
}
