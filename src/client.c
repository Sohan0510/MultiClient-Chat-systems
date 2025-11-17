/* client.c
   Simple interactive client that sends raw input to server.
   Supports /nick, /join, /rooms, /history, /pm, /admin, /quit
*/
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 12345
#define BUF 8192

static inline void trim_newline(char *s) { if (!s) return; s[strcspn(s, "\r\n")] = '\0'; }

int main(int argc, char *argv[]) {
    char *host = "127.0.0.1";
    if (argc >= 2) host = argv[1];
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    struct sockaddr_in serv = {0};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(PORT);
    if (inet_pton(AF_INET, host, &serv.sin_addr) <= 0) { perror("inet_pton"); return 1; }
    if (connect(sock, (struct sockaddr *)&serv, sizeof(serv)) < 0) { perror("connect"); return 1; }

    printf("Connected to %s:%d\n", host, PORT);
    printf("Commands: /nick <name>, /join <room>, /rooms, /history, /pm <user> <msg>, /admin <pwd> <CMD>, /quit\n");

    fd_set rfds;
    char inbuf[BUF];
    while (1) {
        FD_ZERO(&rfds);
        FD_SET(0, &rfds);
        FD_SET(sock, &rfds);
        int maxfd = (sock > 0) ? sock : 0;
        int rv = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rv < 0) { if (errno == EINTR) continue; perror("select"); break; }
        if (FD_ISSET(sock, &rfds)) {
            ssize_t n = read(sock, inbuf, sizeof(inbuf) - 1);
            if (n <= 0) { printf("Disconnected from server\n"); break; }
            inbuf[n] = '\0';
            printf("%s", inbuf);
        }
        if (FD_ISSET(0, &rfds)) {
            if (!fgets(inbuf, sizeof(inbuf), stdin)) break;
            trim_newline(inbuf);
            strcat(inbuf, "\n");
            write(sock, inbuf, strlen(inbuf));
            if (strncmp(inbuf, "/quit", 5) == 0) break;
        }
    }
    close(sock);
    return 0;
}
