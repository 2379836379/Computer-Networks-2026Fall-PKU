#include "rtp.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SEQ_MASK ((uint32_t)((1u << 30) - 1u))
#define ACK_WAIT_MS 100

typedef struct {
    rtp_packet_t packet;
    int received;
} receive_slot_t;

static uint32_t seq_add(uint32_t seq, uint32_t n) { return (seq + n) & SEQ_MASK; }
static uint32_t seq_distance(uint32_t from, uint32_t to) { return (to - from) & SEQ_MASK; }

static void make_packet(rtp_packet_t *packet, uint32_t seq, uint8_t flags) {
    memset(packet, 0, sizeof(*packet));
    packet->rtp.seq_num = seq & SEQ_MASK;
    packet->rtp.flags = flags;
    packet->rtp.length = 0;
    packet->rtp.checksum = compute_checksum(packet, sizeof(rtp_header_t));
}

static int packet_valid(const rtp_packet_t *packet, ssize_t received,
                        uint8_t expected_flags, int require_data) {
    if (received < (ssize_t)sizeof(rtp_header_t) ||
        received > (ssize_t)(sizeof(rtp_header_t) + PAYLOAD_MAX)) return 0;
    uint16_t length = packet->rtp.length;
    if (length > PAYLOAD_MAX || received != (ssize_t)(sizeof(rtp_header_t) + length)) return 0;
    if (packet->rtp.flags != expected_flags || (require_data && length == 0)) return 0;
    rtp_packet_t copy;
    memcpy(&copy, packet, (size_t)received);
    uint32_t checksum = copy.rtp.checksum;
    copy.rtp.checksum = 0;
    return checksum == compute_checksum(&copy, (size_t)received);
}

static int wait_readable(int fd, int milliseconds) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = {milliseconds / 1000, (milliseconds % 1000) * 1000};
    int ret;
    do {
        ret = select(fd + 1, &set, NULL, NULL, &tv);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

static int same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_family == b->sin_family && a->sin_port == b->sin_port &&
           a->sin_addr.s_addr == b->sin_addr.s_addr;
}

static int send_packet(int fd, const struct sockaddr_in *peer,
                       const rtp_packet_t *packet) {
    size_t size = sizeof(rtp_header_t) + packet->rtp.length;
    ssize_t sent = sendto(fd, packet, size, 0, (const struct sockaddr *)peer, sizeof(*peer));
    return sent == (ssize_t)size ? 0 : -1;
}

static int establish_connection(int fd, struct sockaddr_in *peer,
                                uint32_t *initial_sequence) {
    rtp_packet_t incoming, synack;
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    ssize_t received;
    int ready;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (long)(now.tv_sec - started.tv_sec) * 1000L +
                          (now.tv_nsec - started.tv_nsec) / 1000000L;
        int remaining = 5000 - (int)elapsed_ms;
        if (remaining <= 0) return -1;
        ready = wait_readable(fd, remaining);
        if (ready <= 0) return -1;
        socklen_t peer_length = sizeof(*peer);
        received = recvfrom(fd, &incoming, sizeof(incoming), 0,
                            (struct sockaddr *)peer, &peer_length);
        if (received < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (packet_valid(&incoming, received, RTP_SYN, 0)) break;
    }
    uint32_t sequence = incoming.rtp.seq_num;
    make_packet(&synack, seq_add(sequence, 1), RTP_SYN | RTP_ACK);

    for (;;) {
        if (send_packet(fd, peer, &synack) < 0) return -1;
        ready = wait_readable(fd, ACK_WAIT_MS);
        if (ready < 0) return -1;
        if (ready == 0) continue;
        struct sockaddr_in source;
        socklen_t source_length = sizeof(source);
        received = recvfrom(fd, &incoming, sizeof(incoming), 0,
                            (struct sockaddr *)&source, &source_length);
        if (received < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (!same_peer(&source, peer)) continue;
        if (packet_valid(&incoming, received, RTP_SYN, 0) &&
            incoming.rtp.seq_num == sequence) continue;
        if (packet_valid(&incoming, received, RTP_ACK, 0) &&
            incoming.rtp.seq_num == seq_add(sequence, 1)) {
            *initial_sequence = sequence;
            return 0;
        }
    }
}

static int send_ack(int fd, const struct sockaddr_in *peer, uint32_t sequence) {
    rtp_packet_t ack;
    make_packet(&ack, sequence, RTP_ACK);
    return send_packet(fd, peer, &ack);
}

static int finish_connection(int fd, const struct sockaddr_in *peer, uint32_t sequence) {
    rtp_packet_t finack, incoming;
    make_packet(&finack, sequence, RTP_FIN | RTP_ACK);
    /* Keep the receiver alive briefly to answer a FIN retransmission whose
     * first FINACK was lost. */
    for (int elapsed = 0; elapsed < 2000; elapsed += ACK_WAIT_MS) {
        if (wait_readable(fd, ACK_WAIT_MS) <= 0) continue;
        struct sockaddr_in source;
        socklen_t length = sizeof(source);
        ssize_t received = recvfrom(fd, &incoming, sizeof(incoming), 0,
                                    (struct sockaddr *)&source, &length);
        if (received >= 0 && same_peer(&source, peer) &&
            packet_valid(&incoming, received, RTP_FIN, 0) &&
            incoming.rtp.seq_num == sequence)
            send_packet(fd, peer, &finack);
    }
    return 0;
}

static int receive_data(int fd, const struct sockaddr_in *peer, FILE *output,
                        uint32_t initial_sequence, size_t window, int selective) {
    uint32_t expected = seq_add(initial_sequence, 1);
    receive_slot_t *slots = selective ? calloc(window, sizeof(*slots)) : NULL;
    if (selective && !slots) return -1;
    size_t head = 0;
    rtp_packet_t incoming;
    for (;;) {
        int ready = wait_readable(fd, 5000);
        if (ready <= 0) {
            free(slots);
            return -1;
        }
        struct sockaddr_in source;
        socklen_t source_length = sizeof(source);
        ssize_t received = recvfrom(fd, &incoming, sizeof(incoming), 0,
                                    (struct sockaddr *)&source, &source_length);
        if (received < 0) {
            if (errno == EINTR) continue;
            free(slots);
            return -1;
        }
        if (!same_peer(&source, peer)) continue;

        if (packet_valid(&incoming, received, RTP_FIN, 0) &&
            incoming.rtp.seq_num == expected) {
            rtp_packet_t finack;
            make_packet(&finack, expected, RTP_FIN | RTP_ACK);
            if (send_packet(fd, peer, &finack) < 0) {
                free(slots);
                return -1;
            }
            free(slots);
            return finish_connection(fd, peer, expected);
        }
        if (!packet_valid(&incoming, received, 0, 1)) continue;

        uint32_t offset = seq_distance(expected, incoming.rtp.seq_num);
        if (!selective) {
            if (offset == 0) {
                if (fwrite(incoming.payload, 1, incoming.rtp.length, output) != incoming.rtp.length) {
                    free(slots);
                    return -1;
                }
                expected = seq_add(expected, 1);
            }
            /* GBN ACKs are cumulative, including for out-of-order and duplicate packets. */
            if (send_ack(fd, peer, expected) < 0) {
                free(slots);
                return -1;
            }
        } else {
            if (offset < window) {
                size_t index = (head + offset) % window;
                if (!slots[index].received) {
                    memcpy(&slots[index].packet, &incoming, (size_t)received);
                    slots[index].received = 1;
                }
                if (send_ack(fd, peer, incoming.rtp.seq_num) < 0) {
                    free(slots);
                    return -1;
                }
                while (slots[head].received) {
                    receive_slot_t *slot = &slots[head];
                    if (fwrite(slot->packet.payload, 1, slot->packet.rtp.length, output) !=
                        slot->packet.rtp.length) {
                        free(slots);
                        return -1;
                    }
                    memset(slot, 0, sizeof(*slot));
                    head = (head + 1) % window;
                    expected = seq_add(expected, 1);
                }
            } else {
                uint32_t behind = seq_distance(incoming.rtp.seq_num, expected);
                if (behind > 0 && behind <= window && send_ack(fd, peer, incoming.rtp.seq_num) < 0) {
                    free(slots);
                    return -1;
                }
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 5) {
        LOG_FATAL("Usage: ./receiver [listen port] [file path] [window size] "
                  "[mode]\n");
    }
    char *end = NULL;
    long port_value = strtol(argv[1], &end, 10);
    long window_value = strtol(argv[3], &end, 10);
    long mode_value = strtol(argv[4], &end, 10);
    if (port_value < 1 || port_value > 65535 || window_value < 1 ||
        window_value > 100000 || mode_value < 0 || mode_value > 1)
        LOG_FATAL("Receiver: invalid parameters\n");
    FILE *output = fopen(argv[2], "wb");
    if (!output) LOG_FATAL("Receiver: failed to open file\n");
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) LOG_FATAL("Receiver: failed to create socket\n");
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons((uint16_t)port_value);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0)
        LOG_FATAL("Receiver: failed to bind socket\n");

    struct sockaddr_in peer;
    uint32_t initial_sequence;
    if (establish_connection(fd, &peer, &initial_sequence) < 0)
        LOG_FATAL("Receiver: failed to establish connection\n");
    if (receive_data(fd, &peer, output, initial_sequence, (size_t)window_value,
                     mode_value == 1) < 0)
        LOG_FATAL("Receiver: failed while receiving data\n");
    fclose(output);
    close(fd);
    LOG_DEBUG("Receiver: exiting...\n");
    return 0;
}
