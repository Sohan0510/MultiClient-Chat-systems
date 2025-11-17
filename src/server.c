/* server.c
   Multi-client chat server (fork-per-connection) with:
   - rooms, history (logs/<room>.log)
   - /nick, /join, /rooms, /history, /pm, /admin, /quit
   - profanity filter via fork()+exec() -> ./filter
   - uses pipes, fork, exec, wait, select, open, read, write, signals
*/
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------ CONSTANTS ------------ */
#define PORT 12345
#define BACKLOG 10
#define BUF 8192
#define MAX_CLIENTS 128
#define MAX_ROOMS 128
#define LOGDIR "logs"
#define ADMIN_PASSWORD "admin123"

/* ------------ DATA STRUCTURES ------------ */
typedef struct {
    pid_t pid;
    int to_child_fd;
    int from_child_fd;
    char username[64];
    char room[64];
    bool connected;
    bool muted;
    bool is_admin; /* true when this client authenticated as admin */
} client_t;

static client_t clients[MAX_CLIENTS];
static char rooms[MAX_ROOMS][64];
static int room_count = 0;
/* per-client last appeal message to avoid duplicate forwards */
static char last_appeal_msg[MAX_CLIENTS][512];

static volatile sig_atomic_t shutdown_requested = 0;
static int listen_fd = -1;

/* ------------ HELPERS ------------ */
static inline void trim_newline(char *s) {
    if (!s) return;
    s[strcspn(s, "\r\n")] = '\0';
}

ssize_t writef(int fd, const char *fmt, ...) {
    char buf[BUF];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    return write(fd, buf, n);
}

void ensure_logdir() {
    struct stat st;
    if (stat(LOGDIR, &st) == -1)
        mkdir(LOGDIR, 0755);
}

void add_room_if_missing(const char *r) {
    if (!r || !r[0]) return;
    for (int i = 0; i < room_count; ++i)
        if (strcmp(rooms[i], r) == 0) return;

    if (room_count < MAX_ROOMS) {
        strncpy(rooms[room_count], r, sizeof(rooms[0]) - 1);
        rooms[room_count][sizeof(rooms[0]) - 1] = '\0';
        room_count++;
    }
}

/* ------------ LOGGING ------------ */
void append_room_log(const char *room, const char *msg) {
    ensure_logdir();
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.log", LOGDIR, room);

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        write(fd, msg, strlen(msg));
        write(fd, "\n", 1);
        close(fd);
    }
}

/* ------------ FILTER ------------ */
char *run_filter_and_get_output(const char *input) {
    int p2f[2], f2p[2];
    if (pipe(p2f) < 0 || pipe(f2p) < 0)
        return strdup(input);

    pid_t pid = fork();
    if (pid < 0)
        return strdup(input);

    if (pid == 0) {
        dup2(p2f[0], STDIN_FILENO);
        dup2(f2p[1], STDOUT_FILENO);
        close(p2f[0]); close(p2f[1]); close(f2p[0]); close(f2p[1]);
        execl("./filter", "./filter", NULL);
        _exit(127);
    }

    close(p2f[0]);
    close(f2p[1]);
    write(p2f[1], input, strlen(input));
    write(p2f[1], "\n", 1);
    close(p2f[1]);

    char buf[BUF];
    ssize_t n = read(f2p[0], buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(f2p[0]);
        waitpid(pid, NULL, 0);
        return strdup(input);
    }
    buf[n] = '\0';
    trim_newline(buf);
    close(f2p[0]);
    waitpid(pid, NULL, 0);

    return strdup(buf);
}

/* ------------ BROADCAST ------------ */




void broadcast_to_room(const char *room, const char *from, const char *msg) {
    if (!room) return;
    add_room_if_missing(room);
    char *filtered = run_filter_and_get_output(msg ? msg : "");
    char line[BUF];
    const char *sender = from ? from : "server";
    snprintf(line, sizeof(line), "[%s] %s: %s", room, sender, filtered);
    append_room_log(room, line);

    /* If room == "global" send to all connected clients (broadcast) */
    if (strcmp(room, "global") == 0) {
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i].connected) {
                writef(clients[i].to_child_fd, "%s\n", line);
            }
        }
    } else {
        /* send only to clients in that room (no monitor copies to admins) */
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i].connected && strcmp(clients[i].room, room) == 0) {
                writef(clients[i].to_child_fd, "%s\n", line);
            }
        }
    }

    free(filtered);
}





/* ------------ PM ------------ */
bool send_private(const char *from, const char *to, const char *msg) {
    if (!from || !to || !msg) return false;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].connected && strcmp(clients[i].username, to) == 0) {
            char *filtered = run_filter_and_get_output(msg);
            writef(clients[i].to_child_fd, "[PM] %s -> you: %s\n", from, filtered);
            free(filtered);
            return true;
        }
    }
    return false;
}

/* ------------ SIGNAL HANDLERS ------------ */
void sigint_handler(int s) { (void)s; shutdown_requested = 1; }
void sigusr1_handler(int s) {
    int active = 0;
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i].connected) active++;
    printf("Stats: %d clients, %d rooms\n", active, room_count);
}

/* ------------ CLEANUP ------------ */
void cleanup_and_exit() {
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i].connected)
            writef(clients[i].to_child_fd, "/server_shutdown\n");

    while (wait(NULL) > 0) {}
    if (listen_fd != -1) close(listen_fd);
    exit(0);
}

/* ------------ HELPERS ------------ */
int find_free_slot() {
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (!clients[i].connected) return i;
    return -1;
}

int find_client_by_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i].connected && strcmp(clients[i].username, name) == 0)
            return i;
    return -1;
}

/* ------------ PARENT MESSAGE HANDLER ------------ */
void handle_parent_messages() {
    fd_set rfds;
    FD_ZERO(&rfds);

    int maxfd = -1;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].connected) {
            FD_SET(clients[i].from_child_fd, &rfds);
            if (clients[i].from_child_fd > maxfd) maxfd = clients[i].from_child_fd;
        }
    }
    if (maxfd < 0) return;

    struct timeval tv = {0, 300000};
    int rv = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (rv <= 0) return;

    char buf[BUF];
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].connected) continue;
        if (!FD_ISSET(clients[i].from_child_fd, &rfds)) continue;

        ssize_t n = read(clients[i].from_child_fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            close(clients[i].from_child_fd);
            close(clients[i].to_child_fd);
            clients[i].connected = false;
            continue;
        }
        buf[n] = '\0';
        trim_newline(buf);

        char *save = NULL;
        char *cmd = strtok_r(buf, "|", &save);
        if (!cmd) continue;

        if (strcmp(cmd, "JOIN") == 0) {
            char *username = strtok_r(NULL, "|", &save);
            char *room = strtok_r(NULL, "|", &save);
            if (!username || !room) continue;
            strncpy(clients[i].username, username, sizeof(clients[i].username)-1);
            strncpy(clients[i].room, room, sizeof(clients[i].room)-1);
            add_room_if_missing(room);
            writef(clients[i].to_child_fd, "Welcome %s to %s\n", username, room);
            broadcast_to_room(room, "server", "a new user has joined");
        }

        else if (strcmp(cmd, "MSG") == 0) {
            char *username = strtok_r(NULL, "|", &save);
            char *room = strtok_r(NULL, "|", &save);
            char *message = strtok_r(NULL, "|", &save);
            if (!username || !room || !message) continue;
            if (clients[i].muted) writef(clients[i].to_child_fd, "You are muted.\n");
            else broadcast_to_room(room, username, message);
        }

        else if (strcmp(cmd, "PM") == 0) {
            char *from = strtok_r(NULL, "|", &save);
            char *to = strtok_r(NULL, "|", &save);
            char *message = strtok_r(NULL, "|", &save);
            if (!from || !to || !message) continue;
            if (!send_private(from, to, message))
                writef(clients[i].to_child_fd, "User %s not found\n", to);
            else
                writef(clients[i].to_child_fd, "PM sent to %s\n", to);
        }
        
        else if (strcmp(cmd, "APPEAL") == 0) {
            /* APPEAL|from|message  -> forward to all admins only, with deduping per-sender */
            char *from = strtok_r(NULL, "|", &save);
            char *message = strtok_r(NULL, "\n", &save);
            if (!from || !message) continue;
            int sender_idx = find_client_by_name(from);
            /* dedupe: if the same message already forwarded for this sender, skip */
            if (sender_idx >= 0 && last_appeal_msg[sender_idx][0] != '\0' && strcmp(last_appeal_msg[sender_idx], message) == 0) {
                /* already forwarded recently */
                writef(clients[i].to_child_fd, "Your appeal was already sent to admins recently.\n");
                continue;
            }
            /* store last appeal for this sender */
            if (sender_idx >= 0) {
                strncpy(last_appeal_msg[sender_idx], message, sizeof(last_appeal_msg[sender_idx]) - 1);
                last_appeal_msg[sender_idx][sizeof(last_appeal_msg[sender_idx]) - 1] = '\0';
            }
            int sent = 0;
            for (int k = 0; k < MAX_CLIENTS; ++k) {
                if (clients[k].connected && clients[k].is_admin) {
                    writef(clients[k].to_child_fd, "[APPEAL] %s: %s\n", from, message);
                    sent++;
                    /* server-side log for demo visibility */
                    printf("Forwarded APPEAL from '%s' to admin slot %d (user='%s', room='%s')\n",
                           from, k, clients[k].username[0] ? clients[k].username : "(unnamed)",
                           clients[k].room[0] ? clients[k].room : "(none)");
                }
            }
            if (sent == 0) {
                writef(clients[i].to_child_fd, "No admins currently online. Try again later.\n");
            } else {
                writef(clients[i].to_child_fd, "Your appeal was sent to %d admin(s).\n", sent);
            }
        }



        else if (strcmp(cmd, "HISTORY") == 0) {
            char *room = strtok_r(NULL, "|", &save);
            if (!room) continue;
            char path[256];
            snprintf(path, sizeof(path), "%s/%s.log", LOGDIR, room);
            int fd = open(path, O_RDONLY);
            if (fd < 0) writef(clients[i].to_child_fd, "No history for %s\n", room);
            else {
                char rbuf[1024];
                ssize_t rn;
                while ((rn = read(fd, rbuf, sizeof(rbuf))) > 0)
                    write(clients[i].to_child_fd, rbuf, rn);
                close(fd);
            }
        }

        else if (strcmp(cmd, "ROOMS") == 0) {
            if (room_count == 0) writef(clients[i].to_child_fd, "No rooms\n");
            else for (int r = 0; r < room_count; ++r) writef(clients[i].to_child_fd, "%s\n", rooms[r]);
        }

        else if (strcmp(cmd, "QUIT") == 0) {
            writef(clients[i].to_child_fd, "Goodbye\n");
            close(clients[i].from_child_fd);
            close(clients[i].to_child_fd);
            clients[i].connected = false;
        }

        else if (strcmp(cmd, "ADMIN") == 0) {
            /* Robust ADMIN parsing:
               Accept either:
                 ADMIN|username|password|ACTION|args...
               or:
                 ADMIN|username|password ACTION args...
            */
            char *username = strtok_r(NULL, "|", &save);
            char *third = strtok_r(NULL, "|", &save); /* may contain password OR "password ACTION..." */
            char *action = strtok_r(NULL, "|", &save); /* null if the client used space-separated form */

            if (!username || !third) { writef(clients[i].to_child_fd, "Admin malformed\n"); continue; }

            /* If action is NULL, try to split third by first space into password and action+args */
            char *password = NULL;
            char *action_with_args = NULL;

            if (action == NULL) {
                /* attempt space-split on 'third' */
                char *sp = strchr(third, ' ');
                if (sp) {
                    *sp = '\0';
                    password = third;
                    action_with_args = sp + 1;
                } else {
                    /* only password provided (no action) */
                    password = third;
                    action_with_args = NULL;
                }
            } else {
                password = third;
                action_with_args = action;
            }

            if (!password) { writef(clients[i].to_child_fd, "Admin malformed\n"); continue; }

            /* extract action word and optional args */
            char *action_word = NULL;
            char *action_args = NULL;
            if (action_with_args) {
                char *sp2 = strchr(action_with_args, ' ');
                if (sp2) {
                    *sp2 = '\0';
                    action_word = action_with_args;
                    action_args = sp2 + 1;
                } else {
                    action_word = action_with_args;
                    action_args = NULL;
                }
            }

            /* authenticate */
            if (strcmp(password, ADMIN_PASSWORD) != 0) {
                writef(clients[i].to_child_fd, "Admin auth failed\n");
                continue;
            }
            /* mark this client as an admin so they can receive appeals */
            clients[i].is_admin = true;
            /* mark this client as an admin so they can receive appeals */
            clients[i].is_admin = true;

            if (!action_word) { writef(clients[i].to_child_fd, "Admin: no action\n"); continue; }

            if (strcmp(action_word, "KICK") == 0) {
                char *target = action_args ? action_args : strtok_r(NULL, "|", &save);
                if (!target) { writef(clients[i].to_child_fd, "KICK requires username\n"); continue; }
                int idx = find_client_by_name(target);
                if (idx >= 0) {
                    writef(clients[idx].to_child_fd, "You have been kicked by admin\n");
                    close(clients[idx].from_child_fd);
                    close(clients[idx].to_child_fd);
                    clients[idx].connected = false;
                } else writef(clients[i].to_child_fd, "User not found\n");
            }

            else if (strcmp(action_word, "MUTE") == 0) {
                char *target = action_args ? action_args : strtok_r(NULL, "|", &save);
                if (!target) { writef(clients[i].to_child_fd, "MUTE requires username\n"); continue; }
                int idx = find_client_by_name(target);
                if (idx >= 0) { clients[idx].muted = true; writef(clients[idx].to_child_fd, "You are muted by admin\n"); }
                else writef(clients[i].to_child_fd, "User not found\n");
            }

            else if (strcmp(action_word, "UNMUTE") == 0) {
                char *target = action_args ? action_args : strtok_r(NULL, "|", &save);
                if (!target) { writef(clients[i].to_child_fd, "UNMUTE requires username\n"); continue; }
                int idx = find_client_by_name(target);
                if (idx >= 0) { clients[idx].muted = false; writef(clients[idx].to_child_fd, "You are unmuted by admin\n"); }
                else writef(clients[i].to_child_fd, "User not found\n");
            }

            else if (strcmp(action_word, "BROADCAST") == 0) {
                char *msg = action_args ? action_args : strtok_r(NULL, "|", &save);
                if (!msg) msg = "";
                broadcast_to_room("global", "admin", msg);
            }
            else if (strcmp(action_word, "ROOMS") == 0) {
                if (room_count == 0) {
                    writef(clients[i].to_child_fd, "No rooms\n");
                } else {
                    writef(clients[i].to_child_fd, "Rooms (%d):\n", room_count);
                    for (int r = 0; r < room_count; ++r) {
                        writef(clients[i].to_child_fd, " - %s\n", rooms[r]);
                    }
                }
            }


            else if (strcmp(action_word, "USERS") == 0) {
                int active = 0;
                for (int k = 0; k < MAX_CLIENTS; ++k)
                    if (clients[k].connected) active++;

                writef(clients[i].to_child_fd, "Active users: %d\n", active);

                for (int k = 0; k < MAX_CLIENTS; ++k) {
                    if (clients[k].connected && clients[k].username[0]) {
                        writef(clients[i].to_child_fd,
                               " - %s (room: %s)\n",
                               clients[k].username,
                               clients[k].room[0] ? clients[k].room : "none");
                    }
                }
            }
  
            


            else {
                writef(clients[i].to_child_fd, "Unknown admin action: %s\n", action_word);
            }
        }

        else {
            writef(clients[i].to_child_fd, "Unknown command: %s\n", cmd);
        }
    }
}

/* ------------ ACCEPT & SPAWN CHILD ------------ */
void accept_and_spawn() {
    struct sockaddr_in cli;
    socklen_t sz = sizeof(cli);
    int ns = accept(listen_fd, (struct sockaddr *)&cli, &sz);
    if (ns < 0) return;

    int slot = find_free_slot();
    if (slot < 0) {
        write(ns, "Server full\n", 12);
        close(ns);
        return;
    }

    int p2c[2], c2p[2];
    if (pipe(p2c) < 0 || pipe(c2p) < 0) { close(ns); return; }

    pid_t pid = fork();
    if (pid < 0) { close(ns); return; }

    if (pid == 0) {
        close(p2c[1]); close(c2p[0]);
        int readfd = p2c[0];
        int writefd = c2p[1];
        int sock = ns;

        char username[64] = "unnamed";
        char room[64] = "lobby";
        add_room_if_missing("lobby");

        char buf[BUF];

        while (1) {
            fd_set st;
            FD_ZERO(&st);
            FD_SET(sock, &st);
            FD_SET(readfd, &st);
            int maxfd = sock > readfd ? sock : readfd;

            int rv = select(maxfd + 1, &st, NULL, NULL, NULL);
            if (rv < 0) {
                if (errno == EINTR) continue;
                break;
            }

            if (FD_ISSET(readfd, &st)) {
                ssize_t n = read(readfd, buf, sizeof(buf) - 1);
                if (n <= 0) break;
                write(sock, buf, n);
            }

            if (FD_ISSET(sock, &st)) {
                ssize_t n = read(sock, buf, sizeof(buf) - 1);
                if (n <= 0) {
                    write(writefd, "QUIT|\n", 6);
                    break;
                }
                buf[n] = '\0';
                trim_newline(buf);

                if (buf[0] == '/') {
                    if (!strncmp(buf, "/nick ", 6)) {
                        strncpy(username, buf + 6, sizeof(username)-1);
                        char out[BUF];
                        snprintf(out, sizeof(out), "JOIN|%s|%s\n", username, room);
                        write(writefd, out, strlen(out));
                    } else if (!strncmp(buf, "/join ", 6)) {
                        strncpy(room, buf + 6, sizeof(room)-1);
                        char out[BUF];
                        snprintf(out, sizeof(out), "JOIN|%s|%s\n", username, room);
                        write(writefd, out, strlen(out));
                    } else if (!strcmp(buf, "/rooms")) {
                        write(writefd, "ROOMS|\n", 7);
                    } else if (!strcmp(buf, "/history")) {
                        char out[BUF];
                        snprintf(out, sizeof(out), "HISTORY|%s\n", room);
                        write(writefd, out, strlen(out));
                    } else if (!strncmp(buf, "/pm ", 4)) {
                        char *rest = buf + 4;
                        char *sp = strchr(rest, ' ');
                        if (!sp) write(sock, "Usage: /pm <user> <msg>\n", 25);
                        else {
                            *sp = '\0';
                            char *to = rest;
                            char *msg = sp + 1;
                            char out[BUF];
                            snprintf(out, sizeof(out), "PM|%s|%s|%s\n", username, to, msg);
                            write(writefd, out, strlen(out));
                        }
                    }
                    else if (!strncmp(buf, "/appeal ", 8)) {
                        /* allow muted users to send an appeal to admins */
                        char out[BUF];
                        /* send APPEAL|<username>|<message> to parent */
                        snprintf(out, sizeof(out), "APPEAL|%s|%s\n", username, buf + 8);
                        write(writefd, out, strlen(out));
                    }
 else if (!strncmp(buf, "/admin ", 7)) {
                        /* send raw remainder as is (server will robustly parse) */
                        char out[BUF];
                        snprintf(out, sizeof(out), "ADMIN|%s|%s\n", username, buf + 7);
                        write(writefd, out, strlen(out));
                    } else if (!strcmp(buf, "/quit")) {
                        write(writefd, "QUIT|\n", 6);
                        break;
                    } else {
                        write(sock, "Unknown command\n", 16);
                    }
                } else {
                    /* normal message: safe truncation */
                    char out[BUF];
                    size_t msg_max = BUF - 128;
                    char msg_trunc[BUF];
                    if (strlen(buf) >= msg_max) {
                        memcpy(msg_trunc, buf, msg_max - 1);
                        msg_trunc[msg_max - 1] = '\0';
                    } else {
                        strcpy(msg_trunc, buf);
                    }
                    snprintf(out, sizeof(out), "MSG|%s|%s|%s\n", username, room, msg_trunc);
                    write(writefd, out, strlen(out));
                }
            }
        }

        close(readfd); close(writefd); close(sock);
        _exit(0);
    }

    /* parent */
    close(p2c[0]); close(c2p[1]);
    clients[slot].pid = pid;
    clients[slot].to_child_fd = p2c[1];
    clients[slot].from_child_fd = c2p[0];
    clients[slot].username[0] = '\0';
    clients[slot].room[0] = '\0';
    clients[slot].connected = true;
    clients[slot].muted = false;
    writef(clients[slot].to_child_fd, "Welcome to MultiChat! Use /nick, /join, /pm, /rooms\n");
    close(ns);
}

/* ------------ MAIN ------------ */
int main() {
    signal(SIGINT, sigint_handler);
    signal(SIGUSR1, sigusr1_handler);

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        clients[i].connected = false;
        clients[i].muted = false;
        clients[i].is_admin = false;
    }
    for (int i = 0; i < MAX_CLIENTS; ++i) last_appeal_msg[i][0] = '\0';
    add_room_if_missing("lobby");

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(PORT);
    srv.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr *)&srv, sizeof(srv)) < 0) { perror("bind"); exit(1); }
    if (listen(listen_fd, BACKLOG) < 0) { perror("listen"); exit(1); }

    printf("Server listening on %d...\n", PORT);

    while (!shutdown_requested) {
        fd_set s;
        FD_ZERO(&s);
        FD_SET(listen_fd, &s);
        struct timeval tv = {1, 0};
        int rv = select(listen_fd + 1, &s, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (FD_ISSET(listen_fd, &s)) accept_and_spawn();
        handle_parent_messages();
    }

    cleanup_and_exit();
    return 0;
}
