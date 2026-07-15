#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdint.h>

#define PAYLOAD_SIZE 160
#define PACKET_BUF_SIZE (4 + PAYLOAD_SIZE)

int main(void) {
    // 1. Bind to source input port
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr)) < 0) {
        perror("bind 47010");
        return 1;
    }

    // 2. Setup destination to relay
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = {0};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char input_buf[2048];
    
    // Redundancy storage
    uint32_t prev_seq = 0xffffffff;
    unsigned char prev_payload[PAYLOAD_SIZE];
    int has_prev = 0;

    // Buffer for outgoing custom frame:
    // Format: [4-byte current_seq] [160-byte current_payload] [4-byte prev_seq] [160-byte prev_payload]
    unsigned char tx_buf[4 + PAYLOAD_SIZE + 4 + PAYLOAD_SIZE];

    for (;;) {
        ssize_t n = recvfrom(in_fd, input_buf, sizeof(input_buf), 0, NULL, NULL);
        if (n < PACKET_BUF_SIZE) continue;

        // Parse incoming frame from harness
        uint32_t curr_seq;
        memcpy(&curr_seq, input_buf, 4);

        // Construct current packet
        memcpy(tx_buf, input_buf, PACKET_BUF_SIZE);

        // Pack previous redundant frame
        uint32_t prev_seq_net = htonl(prev_seq);
        memcpy(tx_buf + PACKET_BUF_SIZE, &prev_seq_net, 4);
        if (has_prev) {
            memcpy(tx_buf + PACKET_BUF_SIZE + 4, prev_payload, PAYLOAD_SIZE);
        } else {
            memset(tx_buf + PACKET_BUF_SIZE + 4, 0, PAYLOAD_SIZE);
        }

        // Send to relay
        size_t tx_bytes = PACKET_BUF_SIZE + 4 + PAYLOAD_SIZE;
        sendto(out_fd, tx_buf, tx_bytes, 0, (struct sockaddr *)&relay, sizeof(relay));

        // Save current as previous
        prev_seq = ntohl(curr_seq);
        memcpy(prev_payload, input_buf + 4, PAYLOAD_SIZE);
        has_prev = 1;
    }
    return 0;
}
