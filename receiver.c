#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>

#define PAYLOAD_SIZE 160
#define RX_PACKET_SIZE (4 + PAYLOAD_SIZE + 4 + PAYLOAD_SIZE)
#define JITTER_BUF_CAP 20000

// Thread-safe circular jitter buffer
typedef struct {
    unsigned char payload[PAYLOAD_SIZE];
    int occupied;
} FrameSlot;

FrameSlot jitter_buffer[JITTER_BUF_CAP];
pthread_mutex_t buf_mutex = PTHREAD_MUTEX_INITIALIZER;

void store_frame(uint32_t seq, const unsigned char *payload) {
    if (seq >= JITTER_BUF_CAP) return; // Guard bounds
    pthread_mutex_lock(&buf_mutex);
    if (!jitter_buffer[seq].occupied) {
        memcpy(jitter_buffer[seq].payload, payload, PAYLOAD_SIZE);
        jitter_buffer[seq].occupied = 1;
    }
    pthread_mutex_unlock(&buf_mutex);
}

// Playback background worker thread
typedef struct {
    int out_fd;
    struct sockaddr_in player_addr;
    double t0;
    double delay_sec;
} PlaybackArgs;

void *playback_thread_fn(void *arg) {
    PlaybackArgs *args = (PlaybackArgs *)arg;
    uint32_t next_play_seq = 0;

    for (;;) {
        // Calculate exact target dispatch epoch for frame i
        double target_time = args->t0 + args->delay_sec + (next_play_seq * 20.0 / 1000.0);
        struct timeval tv;
        gettimeofday(&tv, NULL);
        double now = tv.tv_sec + (tv.tv_usec / 1000000.0);

        double wait_time = target_time - now;
        if (wait_time > 0.001) {
            usleep((useconds_t)(wait_time * 1000000.0));
        }

        // Retrieve frame from buffer
        unsigned char tx_frame[4 + PAYLOAD_SIZE];
        uint32_t net_seq = htonl(next_play_seq);
        memcpy(tx_frame, &net_seq, 4);

        pthread_mutex_lock(&buf_mutex);
        if (jitter_buffer[next_play_seq].occupied) {
            memcpy(tx_frame + 4, jitter_buffer[next_play_seq].payload, PAYLOAD_SIZE);
            pthread_mutex_unlock(&buf_mutex);
            
            sendto(args->out_fd, tx_frame, 4 + PAYLOAD_SIZE, 0,
                   (struct sockaddr *)&args->player_addr, sizeof(args->player_addr));
        } else {
            pthread_mutex_unlock(&buf_mutex);
            // Even if empty, we tick forward to preserve the clock
        }
        next_play_seq++;
    }
    return NULL;
}

int main(void) {
    // 1. Fetch environment parameters assigned by run.py
    char *t0_env = getenv("T0");
    char *delay_env = getenv("DELAY_MS");
    if (!t0_env || !delay_env) {
        fprintf(stderr, "Missing environment config variables.\n");
        return 1;
    }
    double t0 = atof(t0_env);
    double delay_ms = atof(delay_env);

    // Initialize buffer state
    memset(jitter_buffer, 0, sizeof(jitter_buffer));

    // 2. Bind to inbound socket
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr)) < 0) {
        perror("bind 47002");
        return 1;
    }

    // 3. Setup outbound client
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player_addr = {0};
    player_addr.sin_family = AF_INET;
    player_addr.sin_port = htons(47020);
    player_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Start background playback scheduler
    pthread_t play_thread;
    PlaybackArgs args = {
        .out_fd = out_fd,
        .player_addr = player_addr,
        .t0 = t0,
        .delay_sec = delay_ms / 1000.0
    };
    if (pthread_create(&play_thread, NULL, playback_thread_fn, &args) != 0) {
        perror("pthread_create");
        return 1;
    }

    unsigned char rx_buf[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, rx_buf, sizeof(rx_buf), 0, NULL, NULL);
        if (n < RX_PACKET_SIZE) continue;

        // Process current frame
        uint32_t curr_seq;
        memcpy(&curr_seq, rx_buf, 4);
        curr_seq = ntohl(curr_seq);
        store_frame(curr_seq, rx_buf + 4);

        // Process piggybacked historical redundant frame
        uint32_t prev_seq;
        memcpy(&prev_seq, rx_buf + 4 + PAYLOAD_SIZE, 4);
        prev_seq = ntohl(prev_seq);
        if (prev_seq != 0xffffffff) {
            store_frame(prev_seq, rx_buf + 4 + PAYLOAD_SIZE + 4);
        }
    }
    return 0;
}
