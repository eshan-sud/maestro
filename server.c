/*
    server.c
*/

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>

#define PORT 8080
#define MAX_SONGS 100
#define MAX_LEN 512
#define BACKLOG 5

/* Playlist */
char *playlist[MAX_SONGS];
int song_count = 0;

/* Playback state */
pid_t player_pid = -1;
int current_song = -1;
enum { STATE_STOPPED=0, STATE_PLAYING=1, STATE_PAUSED=2 } state = STATE_STOPPED;

/* Time accounting */
time_t play_start = 0;        // wall time when playback started (or resumed)
time_t paused_since = 0;      // when pause started
double paused_accum = 0.0;    // total paused seconds accumulated during current song
double current_duration = 0.0; // seconds (from ffprobe)

/* playlist persistence */
void load_playlist() {
    FILE *fp = fopen("playlist.txt", "r");
    if (!fp) return;
    char line[MAX_LEN];
    while (fgets(line, sizeof(line), fp) && song_count < MAX_SONGS) {
        line[strcspn(line, "\n")] = 0;
        playlist[song_count++] = strdup(line);
    }
    fclose(fp);
}
void save_playlist() {
    FILE *fp = fopen("playlist.txt", "w");
    if (!fp) return;
    for (int i = 0; i < song_count; ++i) {
        fprintf(fp, "%s\n", playlist[i]);
    }
    fclose(fp);
}

/* Utility: get duration (seconds) using ffprobe */
double get_duration_seconds(const char *path) {
    char cmd[MAX_LEN*2];
    snprintf(cmd, sizeof(cmd),
        "ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 \"%s\" 2>/dev/null",
        path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0.0;
    double dur = 0.0;
    if (fscanf(fp, "%lf", &dur) != 1) {
        dur = 0.0;
    }
    pclose(fp);
    return dur;
}

/* Start playback: kills existing player, reset time accounting, launches mpg123 */
void play_song(int index) {
    if (index < 0 || index >= song_count) return;

    // Kill existing player if any
    if (player_pid > 0) {
        kill(player_pid, SIGKILL);
        waitpid(player_pid, NULL, 0);
        player_pid = -1;
    }

    current_song = index;
    paused_accum = 0.0;
    paused_since = 0;
    current_duration = get_duration_seconds(playlist[index]);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }
    if (pid == 0) {
        // Child: execlp mpg123; use -q to reduce console noise
        execlp("mpg123", "mpg123", "-q", playlist[index], NULL);
        perror("execlp mpg123 failed");
        _exit(1);
    } else {
        player_pid = pid;
        play_start = time(NULL);
        state = STATE_PLAYING;
        fprintf(stderr, "[server] Started mpg123 pid=%d playing '%s' duration=%.2f\n", (int)player_pid, playlist[index], current_duration);
    }
}

/* Pause/resume/next */
void pause_song() {
    if (player_pid > 0 && state == STATE_PLAYING) {
        if (kill(player_pid, SIGSTOP) == 0) {
            paused_since = time(NULL);
            state = STATE_PAUSED;
            fprintf(stderr, "[server] Paused pid=%d\n", (int)player_pid);
        }
    }
}
void resume_song() {
    if (player_pid > 0 && state == STATE_PAUSED) {
        // accumulate paused time
        if (paused_since) {
            paused_accum += difftime(time(NULL), paused_since);
            paused_since = 0;
        }
        if (kill(player_pid, SIGCONT) == 0) {
            play_start = play_start; // unchanged; elapsed computed using paused_accum
            state = STATE_PLAYING;
            fprintf(stderr, "[server] Resumed pid=%d\n", (int)player_pid);
        }
    }
}
void stop_song() {
    if (player_pid > 0) {
        kill(player_pid, SIGKILL);
        waitpid(player_pid, NULL, 0);
        player_pid = -1;
    }
    state = STATE_STOPPED;
    current_song = -1;
    paused_since = 0;
    paused_accum = 0.0;
    current_duration = 0.0;
}

/* compute elapsed seconds */
double current_elapsed_seconds() {
    if (state == STATE_STOPPED) return 0.0;
    if (state == STATE_PLAYING) {
        time_t now = time(NULL);
        double elapsed = difftime(now, play_start) - paused_accum;
        if (elapsed < 0) elapsed = 0;
        return elapsed;
    }
    // paused
    if (state == STATE_PAUSED) {
        double elapsed = difftime(paused_since, play_start) - paused_accum;
        if (elapsed < 0) elapsed = 0;
        return elapsed;
    }
    return 0.0;
}

/* next song (wrap) */
void next_song() {
    if (song_count == 0) return;
    int next = (current_song + 1) % song_count;
    play_song(next);
}

/* Format MM:SS helper (not used in STATUS; used for logs if needed) */
void sec_to_mmss(double s, char *out, size_t cap) {
    int secs = (int) s;
    int mm = secs / 60;
    int ss = secs % 60;
    snprintf(out, cap, "%02d:%02d", mm, ss);
}

/* Client handler: persistent connection that reads commands and sends STATUS lines */
void handle_client(int client_fd) {
    char buf[MAX_LEN];
    ssize_t n;
    // Make socket non-blocking for write operations to avoid blocking the status loop
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK); // keep blocking reads for simplicity

    // We'll use a simple loop: use select with 1s timeout to both check incoming commands and send STATUS every sec
    while (1) {
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(client_fd, &readfds);
        // Also monitor for commands via client socket; client will send newline-terminated commands
        tv.tv_sec = 1; tv.tv_usec = 0;
        int rv = select(client_fd + 1, &readfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (rv > 0 && FD_ISSET(client_fd, &readfds)) {
            // read a command (up to newline)
            memset(buf, 0, sizeof(buf));
            n = recv(client_fd, buf, sizeof(buf)-1, 0);
            if (n <= 0) {
                // client closed
                break;
            }
            // trim newline
            buf[strcspn(buf, "\r\n")] = 0;
            fprintf(stderr, "[server] Received command: '%s'\n", buf);

            if (strncmp(buf, "play", 4) == 0) {
                if (state == STATE_STOPPED) {
                    if (song_count > 0) {
                        play_song(0);
                    } else {
                        const char *msg = "ERR No songs in playlist\n";
                        send(client_fd, msg, strlen(msg), 0);
                    }
                } else if (state == STATE_PAUSED) {
                    resume_song();
                } else {
                    // already playing
                }
                const char *ok = "OK Playing\n";
                send(client_fd, ok, strlen(ok), 0);
            } else if (strncmp(buf, "pause", 5) == 0) {
                pause_song();
                const char *ok = "OK Paused\n";
                send(client_fd, ok, strlen(ok), 0);
            } else if (strncmp(buf, "next", 4) == 0) {
                next_song();
                const char *ok = "OK Next\n";
                send(client_fd, ok, strlen(ok), 0);
            } else if (strncmp(buf, "add ", 4) == 0) {
                char *song = buf + 4;
                if (song_count < MAX_SONGS) {
                    playlist[song_count++] = strdup(song);
                    save_playlist();
                    const char *ok = "OK Song added\n";
                    send(client_fd, ok, strlen(ok), 0);
                } else {
                    const char *err = "ERR Playlist full\n";
                    send(client_fd, err, strlen(err), 0);
                }
            } else if (strncmp(buf, "list", 4) == 0) {
                char listbuf[4096] = "";
                for (int i = 0; i < song_count; ++i) {
                    char line[MAX_LEN];
                    snprintf(line, sizeof(line), "%d. %s\n", i+1, playlist[i]);
                    strncat(listbuf, line, sizeof(listbuf) - strlen(listbuf) - 1);
                }
                if (song_count==0) strncpy(listbuf, "No songs.\n", sizeof(listbuf));
                send(client_fd, listbuf, strlen(listbuf), 0);
            } else if (strncmp(buf, "stop", 4) == 0 || strncmp(buf, "exit", 4) == 0) {
                const char *ok = "OK Bye\n";
                send(client_fd, ok, strlen(ok), 0);
                break;
            } else {
                const char *unk = "ERR Unknown command\n";
                send(client_fd, unk, strlen(unk), 0);
            }
        }

        // Periodic status update (every select timeout)
        double elapsed = current_elapsed_seconds();
        char status_line[256];
        const char *stname = (state==STATE_PLAYING) ? "PLAYING" : (state==STATE_PAUSED) ? "PAUSED" : "STOPPED";

        // Send STATUS
        snprintf(status_line, sizeof(status_line), "STATUS %s %.0f %.0f\n", stname, elapsed, current_duration);
        ssize_t sres = send(client_fd, status_line, strlen(status_line), 0);

        // Send CURRENT song info
        if (current_song >= 0 && current_song < song_count) {
            char now_line[256];
            snprintf(now_line, sizeof(now_line), "PLAYING %s\n", playlist[current_song]);
            send(client_fd, now_line, strlen(now_line), 0);

            // Send NEXT song info
            if (song_count > 1) {
                int next = (current_song + 1) % song_count;
                char next_line[256];
                snprintf(next_line, sizeof(next_line), "NEXT %s\n", playlist[next]);
                send(client_fd, next_line, strlen(next_line), 0);
            }
        }
        if (sres <= 0) {
            // If send fails (client disconnected), break
            if (errno == EPIPE || errno == ECONNRESET) break;
        }

        // If playback finished, auto advance to next if appropriate
        if (state == STATE_PLAYING && current_duration > 1.0) {
            if (elapsed >= current_duration - 0.5) { // a little tolerance
                // song finished
                fprintf(stderr, "[server] Song finished (elapsed %.1f >= duration %.1f)\n", elapsed, current_duration);
                // kill the child if still there
                if (player_pid > 0) {
                    kill(player_pid, SIGKILL);
                    waitpid(player_pid, NULL, 0);
                    player_pid = -1;
                }
                // automatically advance
                if (song_count > 0) {
                    int next = (current_song + 1) % song_count;
                    play_song(next);
                } else {
                    stop_song();
                }
            }
        }
    } // end while

    close(client_fd);
    fprintf(stderr, "[server] Client disconnected\n");
}

int main() {
    int sockfd, newsock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    load_playlist();

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(1);
    }

    if (listen(sockfd, BACKLOG) < 0) {
        perror("listen");
        exit(1);
    }

    fprintf(stderr, "🎵 Music Player Daemon running on port %d...\n", PORT);

    while (1) {
        newsock = accept(sockfd, (struct sockaddr *)&client_addr, &client_len);
        if (newsock < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(newsock);
            continue;
        }
        if (pid == 0) {
            // child handles client
            close(sockfd);
            handle_client(newsock);
            exit(0);
        } else {
            // parent closes client socket and continues
            close(newsock);
            // optionally reap children
            while (waitpid(-1, NULL, WNOHANG) > 0) {}
        }
    }

    close(sockfd);
    return 0;
}