/*
    client.c
*/

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <errno.h>
#include <termios.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>

#define PORT 8080
#define SERVER_IP "127.0.0.1"
#define BUF_SIZE 4096
#define MAX_LOG_LINES 20
#define MAX_QUEUE 10
#define MAX_INPUT 256
#define MAX_HISTORY 5

// ─────────────────────────────────────────────
// Global State
// ─────────────────────────────────────────────
char log_lines[MAX_LOG_LINES][256];
int log_head = 0, log_count = 0;

char queue[MAX_QUEUE][128];
int queue_len = 0;

char history_cmds[MAX_HISTORY][MAX_INPUT];
char history_resps[MAX_HISTORY][MAX_INPUT];
int history_len = 0;
int history_pos = -1; // -1 = not browsing

char session_log_path[256];
char current_song[MAX_INPUT] = ""; // holds name of current song

// ─────────────────────────────────────────────
// Log and Queue Helpers
// ─────────────────────────────────────────────
void add_log(const char *msg) {
    snprintf(log_lines[log_head], sizeof(log_lines[0]), "%s", msg);
    log_head = (log_head + 1) % MAX_LOG_LINES;
    if (log_count < MAX_LOG_LINES) log_count++;
}

void update_queue(const char *line) {
    if (strncmp(line, "QUEUE ", 6) == 0) {
        queue_len = 0;
        const char *p = line + 6;
        char *copy = strdup(p);
        if (!copy) return;
        char *token = strtok(copy, ",");
        while (token && queue_len < MAX_QUEUE) {
            snprintf(queue[queue_len++], sizeof(queue[0]), "%s", token);
            token = strtok(NULL, ",");
        }
        free(copy);
    }
}

// ─────────────────────────────────────────────
// Command History
// ─────────────────────────────────────────────
void add_to_history(const char *cmd) {
    if (!cmd || strlen(cmd) == 0) return;

    if (history_len < MAX_HISTORY) {
        snprintf(history_cmds[history_len], MAX_INPUT, "%s", cmd);
        history_resps[history_len][0] = '\0';
        history_len++;
    } else {
        // Safe array shift
        memmove(history_cmds, history_cmds + 1, (MAX_HISTORY - 1) * sizeof(history_cmds[0]));
        memmove(history_resps, history_resps + 1, (MAX_HISTORY - 1) * sizeof(history_resps[0]));

        snprintf(history_cmds[MAX_HISTORY - 1], MAX_INPUT, "%s", cmd);
        history_resps[MAX_HISTORY - 1][0] = '\0';
    }

    history_pos = -1;

    FILE *f = fopen(session_log_path, "a");
    if (f) {
        fprintf(f, "[COMMAND] %s\n", cmd);
        fclose(f);
    }
}

void attach_response_to_last_command(const char *resp) {
    if (!resp || history_len == 0) return;

    strncpy(history_resps[history_len - 1], resp, MAX_INPUT - 1);
    history_resps[history_len - 1][MAX_INPUT - 1] = '\0';

    FILE *f = fopen(session_log_path, "a");
    if (f) {
        fprintf(f, "  [RESPONSE] %s\n", resp);
        fclose(f);
    }
}

// ─────────────────────────────────────────────
// Utility: Time Formatter
// ─────────────────────────────────────────────
void print_time_mmss(double secs, char *out, size_t cap) {
    int s = (int)secs;
    if (s < 0) s = 0;
    snprintf(out, cap, "%02d:%02d", s / 60, s % 60);
}

// ─────────────────────────────────────────────
// UI Rendering
// ─────────────────────────────────────────────
void draw_ui(const char *state, double elapsed, double duration, const char *input_buffer) {
    printf("\033[H\033[J");
    printf("🎵  Mini Music Client (UTF-8 UI)\n");
    printf("──────────────────────────────────────────\n");

    if (strlen(current_song) > 0)
        printf("🎶  Now Playing: %s\n", current_song);
    else
        printf("🎶  Now Playing: (none)\n");

    const char *symbol = strcmp(state, "PLAYING") == 0 ? "▶" :
                         strcmp(state, "PAUSED") == 0  ? "⏸" :
                         strcmp(state, "STOPPED") == 0 ? "⏹" : "•";

    int width = 30;
    int filled = duration > 0 ? (int)((elapsed / duration) * width) : 0;
    if (filled > width) filled = width;

    printf("%s  [", symbol);
    for (int i = 0; i < filled; ++i) fputs("█", stdout);
    for (int i = filled; i < width; ++i) fputs("░", stdout);
    printf("] ");

    char em[16], dm[16];
    print_time_mmss(elapsed, em, sizeof(em));
    print_time_mmss(duration, dm, sizeof(dm));
    printf("%s / %s\n", em, dm);

    printf("\nQueue:\n");
    if (queue_len == 0)
        printf("  (empty)\n");
    else
        for (int i = 0; i < queue_len; ++i)
            printf("  %d. %s\n", i + 1, queue[i]);

    printf("\n──────────────────────────────────────────\n");
    printf("Command> %s", input_buffer);
    printf("\n──────────────────────────────────────────\n");

    printf("Last %d Commands:\n", MAX_HISTORY);
    if (history_len == 0)
        printf("  (no commands yet)\n");
    else
        for (int i = 0; i < history_len; ++i)
            printf("  %d. %s | %s\n", i + 1, history_cmds[i],
                   history_resps[i][0] ? history_resps[i] : "(pending)");

    printf("\n──────────────────────────────────────────\n");
    printf("Commands: play | pause | next | add <path> | list | stop | exit\n");
    fflush(stdout);
}

// ─────────────────────────────────────────────
// Terminal Raw Mode
// ─────────────────────────────────────────────
void enable_raw_mode(struct termios *orig) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, orig);
    raw = *orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void disable_raw_mode(const struct termios *orig) {
    tcsetattr(STDIN_FILENO, TCSANOW, orig);
}

// ─────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────
int main() {
    int sock;
    struct sockaddr_in server;
    char recvbuf[BUF_SIZE];

    mkdir("logs", 0755);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(session_log_path, sizeof(session_log_path),
             "logs/session_%04d%02d%02d_%02d%02d%02d.txt",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    FILE *session_log = fopen(session_log_path, "w");
    if (session_log) {
        fprintf(session_log, "Session started at %s\n", asctime(t));
        fclose(session_log);
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); exit(1); }

    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);
    server.sin_addr.s_addr = inet_addr(SERVER_IP);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("connect");
        exit(1);
    }

    add_log("Connected to server.");

    int maxfd = sock > STDIN_FILENO ? sock : STDIN_FILENO;
    fd_set readfds;

    char current_state[32] = "STOPPED";
    double elapsed = 0.0, duration = 0.0;
    char input_buffer[MAX_INPUT] = {0};
    int input_len = 0;

    struct termios orig_term;
    enable_raw_mode(&orig_term);

    draw_ui(current_state, elapsed, duration, input_buffer);

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(sock, &readfds);
        struct timeval tv = {1, 0};

        int rv = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (FD_ISSET(sock, &readfds)) {
            memset(recvbuf, 0, sizeof(recvbuf));
            ssize_t n = recv(sock, recvbuf, sizeof(recvbuf) - 1, 0);
            if (n <= 0) {
                add_log("[Disconnected]");
                break;
            }

            char *saveptr = NULL;
            char *line = strtok_r(recvbuf, "\n", &saveptr);
            while (line) {
                if (strncmp(line, "STATUS ", 7) == 0) {
                    sscanf(line + 7, "%31s %lf %lf", current_state, &elapsed, &duration);
                } else if (strncmp(line, "QUEUE ", 6) == 0) {
                    update_queue(line);
                } else if (strncmp(line, "PLAYING ", 8) == 0) {
                    const char *songpath = line + 8;
                    const char *basename = strrchr(songpath, '/');
                    if (basename) basename++; else basename = songpath;
                    snprintf(current_song, sizeof(current_song), "%s", basename);
                    size_t len = strlen(current_song);
                    if (len > 0 && current_song[len - 1] == '\n')
                        current_song[len - 1] = '\0';
                } else if (strncmp(line, "NEXT ", 5) == 0) {
                    const char *nextpath = line + 5;
                    const char *basename = strrchr(nextpath, '/');
                    if (basename) basename++; else basename = nextpath;
                    snprintf(queue[0], sizeof(queue[0]), "Next: %s", basename);
                    queue_len = 1;
                } else if (strncmp(line, "STOPPED", 7) == 0) {
                    current_song[0] = '\0';
                } else {
                    attach_response_to_last_command(line);
                    add_log(line);
                }
                line = strtok_r(NULL, "\n", &saveptr);
            }
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 27) { // arrow keys
                    char seq[2];
                    if (read(STDIN_FILENO, &seq[0], 1) == 0) continue;
                    if (read(STDIN_FILENO, &seq[1], 1) == 0) continue;
                    if (seq[0] == '[') {
                        if (seq[1] == 'A') { // Up
                            if (history_len > 0) {
                                if (history_pos < history_len - 1) history_pos++;
                                int idx = history_len - 1 - history_pos;
                                snprintf(input_buffer, MAX_INPUT, "%s", history_cmds[idx]);
                                input_len = strlen(input_buffer);
                            }
                        } else if (seq[1] == 'B') { // Down
                            if (history_pos > 0) {
                                history_pos--;
                                int idx = history_len - 1 - history_pos;
                                snprintf(input_buffer, MAX_INPUT, "%s", history_cmds[idx]);
                                input_len = strlen(input_buffer);
                            } else if (history_pos == 0) {
                                history_pos = -1;
                                input_buffer[0] = '\0';
                                input_len = 0;
                            }
                        }
                    }
                } else if (c == 127 || c == 8) { // Backspace
                    if (input_len > 0) input_buffer[--input_len] = '\0';
                } else if (c == '\n' || c == '\r') { // Enter
                    if (input_len > 0) {
                        input_buffer[input_len] = '\0';
                        char sendbuf[600];
                        snprintf(sendbuf, sizeof(sendbuf), "%s\n", input_buffer);
                        send(sock, sendbuf, strlen(sendbuf), 0);
                        add_log(input_buffer);
                        add_to_history(input_buffer);
                        if (strcmp(input_buffer, "exit") == 0)
                            goto done;
                        input_len = 0;
                        input_buffer[0] = '\0';
                    }
                } else if (isprint((unsigned char)c) && input_len < MAX_INPUT - 1) {
                    input_buffer[input_len++] = c;
                    input_buffer[input_len] = '\0';
                }
            }
        }

        draw_ui(current_state, elapsed, duration, input_buffer);
    }

done:
    disable_raw_mode(&orig_term);
    close(sock);
    printf("\nClient terminated.\nSession log saved at: %s\n", session_log_path);
    return 0;
}