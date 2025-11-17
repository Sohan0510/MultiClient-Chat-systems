/* admin_client.c
   Admin client with persistent 'admin> ' prompt and immediate incoming message handling.
   - Uses select() to monitor socket + stdin
   - Redraws prompt after incoming messages
*/
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* for strcasecmp */
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#include <ctype.h>

#define PORT 12345
#define BUF 8192

/* read password without echo */
static char *read_password(const char *prompt) {
    static char pwdbuf[512];
    struct termios oldt, newt;
    if (tcgetattr(STDIN_FILENO, &oldt) != 0) return NULL;
    newt = oldt;
    newt.c_lflag &= ~ECHO;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) != 0) return NULL;
    if (prompt) { fputs(prompt, stdout); fflush(stdout); }
    if (!fgets(pwdbuf, sizeof(pwdbuf), stdin)) { tcsetattr(STDIN_FILENO, TCSANOW, &oldt); return NULL; }
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    pwdbuf[strcspn(pwdbuf, "\r\n")] = '\0';
    fputc('\n', stdout);
    return pwdbuf;
}

static void trim_newline(char *s) { if (!s) return; s[strcspn(s, "\r\n")] = '\0'; }

/* print the prompt; keeps it consistent */
static void print_prompt(void) {
    fputs("admin> ", stdout);
    fflush(stdout);
}

/* Clear current input line and print a received message then redraw prompt.
   Simpler approach: print newline, the message, then reprint prompt.
   (Preserving partially typed input is possible but more code; not required here.) */
static void handle_incoming_and_redraw(const char *msg) {
    /* ensure message ends with newline */
    size_t len = strlen(msg);
    if (len == 0) return;
    /* print newline first so prompt doesn't mix */
    fputc('\n', stdout);
    fputs(msg, stdout);
    if (msg[len-1] != '\n') fputc('\n', stdout);
    print_prompt();
}

int main(int argc, char *argv[]) {
    char *host = "127.0.0.1";
    if (argc >= 2) host = argv[1];

    char admin_name[128] = {0};
    char *pwd = NULL;

    /* read admin name */
    printf("Admin name: ");
    if (!fgets(admin_name, sizeof(admin_name), stdin)) {
        fprintf(stderr, "Failed to read admin name\n");
        return 1;
    }
    trim_newline(admin_name);
    if (admin_name[0] == '\0') strncpy(admin_name, "admin", sizeof(admin_name)-1);

    /* read password */
    pwd = read_password("Admin password: ");
    if (!pwd) { fprintf(stderr, "Failed to read password\n"); return 1; }

    /* create socket and connect */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    struct sockaddr_in serv = {0};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(PORT);
    if (inet_pton(AF_INET, host, &serv.sin_addr) <= 0) { perror("inet_pton"); close(sock); return 1; }
    if (connect(sock, (struct sockaddr *)&serv, sizeof(serv)) < 0) { perror("connect"); close(sock); return 1; }

    /* set line buffering for stdout so prompt and messages flush promptly */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("Connected to %s:%d as admin '%s'\n", host, PORT, admin_name);
    printf("Enter admin commands (KICK <user>, MUTE <user>, UNMUTE <user>, BROADCAST <text>, USERS, ROOMS, QUIT)\n");

    fd_set rfds;
    int maxfd = sock > STDIN_FILENO ? sock : STDIN_FILENO;
    char inbuf[BUF];
    char sendbuf[BUF];

    print_prompt();

    while (1) {
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(sock, &rfds);

        int rv = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rv < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* incoming server data */
        if (FD_ISSET(sock, &rfds)) {
            ssize_t n = read(sock, inbuf, sizeof(inbuf) - 1);
            if (n > 0) {
                inbuf[n] = '\0';
                handle_incoming_and_redraw(inbuf);
            } else if (n == 0) {
                fprintf(stderr, "\nServer closed connection\n");
                break;
            } else {
                perror("read from server");
                break;
            }
        }

        /* user typed something */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            if (!fgets(inbuf, sizeof(inbuf), stdin)) {
                /* EOF / ctrl-D */
                break;
            }
            trim_newline(inbuf);
            /* ignore empty lines */
            char *line = inbuf;
            while (*line && isspace((unsigned char)*line)) ++line;
            if (*line == '\0') {
                print_prompt();
                continue;
            }

            if (strcasecmp(line, "quit") == 0 || strcasecmp(line, "exit") == 0) {
                break;
            }

            /* split action and args */
            char action[256] = {0};
            char args[BUF] = {0};
            char *sp = strchr(line, ' ');
            if (sp) {
                size_t alen = sp - line;
                if (alen >= sizeof(action)) alen = sizeof(action)-1;
                memcpy(action, line, alen);
                action[alen] = '\0';
                while (*sp == ' ') ++sp;
                strncpy(args, sp, sizeof(args)-1);
            } else {
                strncpy(action, line, sizeof(action)-1);
            }

            /* Compose the /admin line that the child will transform into ADMIN|... */
            int r;
            if (args[0]) {
                r = snprintf(sendbuf, sizeof(sendbuf), "/admin %s|%s|%s\n", pwd, action, args);
            } else {
                r = snprintf(sendbuf, sizeof(sendbuf), "/admin %s|%s\n", pwd, action);
            }
            if (r < 0 || (size_t)r >= sizeof(sendbuf)) {
                fprintf(stderr, "Command too long\n");
                print_prompt();
                continue;
            }

            ssize_t w = write(sock, sendbuf, strlen(sendbuf));
            if (w <= 0) {
                perror("write to server");
                break;
            }

            /* after sending, reprint prompt for next command */
            print_prompt();
        }
    }

    close(sock);
    printf("\nAdmin client exiting.\n");
    return 0;
}
