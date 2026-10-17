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
#define RETRY_LIMIT 100
#define ACK_WAIT_MS 100

typedef struct {
    rtp_packet_t packet;
    int acknowledged;
    int sent;
} data_packet_t;

static uint32_t seq_add(uint32_t seq, uint32_t n) { return (seq + n) & SEQ_MASK; }
static uint32_t seq_distance(uint32_t from, uint32_t to) { return (to - from) & SEQ_MASK; }

static void make_packet(rtp_packet_t *packet, uint32_t seq, uint8_t flags,
                        const void *payload, uint16_t length) {
    memset(packet, 0, sizeof(*packet));
    packet->rtp.seq_num = seq & SEQ_MASK;
    packet->rtp.length = length;
    packet->rtp.flags = flags;
    if (payload && length) memcpy(packet->payload, payload, length);
    packet->rtp.checksum = 0;
    packet->rtp.checksum = compute_checksum(packet, sizeof(rtp_header_t) + length);
}

static int packet_valid(const rtp_packet_t *packet, ssize_t received,
                        uint8_t expected_flags, int allow_data) {
    if (received < (ssize_t)sizeof(rtp_header_t) ||
        received > (ssize_t)(sizeof(rtp_header_t) + PAYLOAD_MAX)) return 0;
    uint16_t length = packet->rtp.length;
    if (length > PAYLOAD_MAX || received != (ssize_t)(sizeof(rtp_header_t) + length)) return 0;
    if (packet->rtp.flags != expected_flags || (!allow_data && length != 0)) return 0;
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

static int send_packet(int fd, const struct sockaddr_in *destination,
                       const rtp_packet_t *packet) {
    size_t size = sizeof(rtp_header_t) + packet->rtp.length;
    ssize_t sent = sendto(fd, packet, size, 0,
                          (const struct sockaddr *)destination, sizeof(*destination));
    return sent == (ssize_t)size ? 0 : -1;
}

static int establish_connection(int fd, const struct sockaddr_in *destination,
                                uint32_t *initial_sequence) {
    rtp_packet_t syn, incoming;
    uint32_t sequence = ((uint32_t)rand() ^ ((uint32_t)getpid() << 16)) & SEQ_MASK;
    make_packet(&syn, sequence, RTP_SYN, NULL, 0);
    for (int attempt = 0; attempt < RETRY_LIMIT; ++attempt) {
        if (send_packet(fd, destination, &syn) < 0) return -1;
        int ready = wait_readable(fd, ACK_WAIT_MS);
        if (ready <= 0) continue;
        ssize_t received = recv(fd, &incoming, sizeof(incoming), 0);
        if (received < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (!packet_valid(&incoming, received, RTP_SYN | RTP_ACK, 0) ||
            incoming.rtp.seq_num != seq_add(sequence, 1)) continue;

        rtp_packet_t ack;
        make_packet(&ack, seq_add(sequence, 1), RTP_ACK, NULL, 0);
        if (send_packet(fd, destination, &ack) < 0) return -1;
        for (int elapsed = 0; elapsed < 2000; elapsed += ACK_WAIT_MS) {
            ready = wait_readable(fd, ACK_WAIT_MS);
            if (ready <= 0) continue;
            received = recv(fd, &incoming, sizeof(incoming), 0);
            if (received >= 0 && packet_valid(&incoming, received, RTP_SYN | RTP_ACK, 0) &&
                incoming.rtp.seq_num == seq_add(sequence, 1)) send_packet(fd, destination, &ack);
        }
        *initial_sequence = sequence;
        return 0;
    }
    return -1;
}

static int transmit_data(int fd, const struct sockaddr_in *destination,
                         data_packet_t *packets, size_t count, size_t window,
                         int selective) {
    size_t base = 0, next = 0;
    int timeout_retries = 0;
    while (base < count) {
        while (next < count && next < base + window) {
            if (send_packet(fd, destination, &packets[next].packet) < 0) return -1;
            packets[next].sent = 1;
            ++next;
        }
        int ready = wait_readable(fd, ACK_WAIT_MS);
        if (ready > 0) {
            rtp_packet_t incoming;
            ssize_t received = recv(fd, &incoming, sizeof(incoming), 0);
            if (received < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (!packet_valid(&incoming, received, RTP_ACK, 0)) continue;
            if (selective) {
                if (next > base) {
                    uint32_t offset = seq_distance(packets[base].packet.rtp.seq_num,
                                                   incoming.rtp.seq_num);
                    if (offset < next - base) packets[base + offset].acknowledged = 1;
                }
            } else if (next > base) {
                uint32_t advance = seq_distance(packets[base].packet.rtp.seq_num,
                                                incoming.rtp.seq_num);
                if (advance <= next - base)
                    for (size_t i = 0; i < advance; ++i) packets[base + i].acknowledged = 1;
            }
            while (base < next && packets[base].acknowledged) ++base;
            timeout_retries = 0;
        } else if (ready == 0) {
            if (++timeout_retries > RETRY_LIMIT) return -1;
            for (size_t i = base; i < next; ++i) {
                if (!selective || !packets[i].acknowledged) {
                    if (send_packet(fd, destination, &packets[i].packet) < 0) return -1;
                }
            }
        } else {
            return -1;
        }
    }
    return 0;
}

static int terminate_connection(int fd, const struct sockaddr_in *destination,
                                uint32_t sequence) {
    rtp_packet_t fin, incoming;
    make_packet(&fin, sequence, RTP_FIN, NULL, 0);
    for (int attempt = 0; attempt < RETRY_LIMIT; ++attempt) {
        if (send_packet(fd, destination, &fin) < 0) return -1;
        int ready = wait_readable(fd, ACK_WAIT_MS);
        if (ready <= 0) continue;
        ssize_t received = recv(fd, &incoming, sizeof(incoming), 0);
        if (received < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (packet_valid(&incoming, received, RTP_FIN | RTP_ACK, 0) &&
            incoming.rtp.seq_num == sequence) return 0;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        LOG_FATAL("Usage: ./sender [receiver ip] [receiver port] [file path] "
                  "[window size] [mode]\n");
    }
    char *end = NULL;
    long port_value = strtol(argv[2], &end, 10);
    long window_value = strtol(argv[4], &end, 10);
    long mode_value = strtol(argv[5], &end, 10);
    if (port_value < 1 || port_value > 65535 || window_value < 1 ||
        window_value > 100000 || mode_value < 0 || mode_value > 1)
        LOG_FATAL("Sender: invalid parameters\n");

    FILE *file = fopen(argv[3], "rb");
    if (!file) LOG_FATAL("Sender: failed to open file\n");
    if (fseek(file, 0, SEEK_END) != 0) LOG_FATAL("Sender: failed to seek file\n");
    long file_size = ftell(file);
    if (file_size < 0 || fseek(file, 0, SEEK_SET) != 0)
        LOG_FATAL("Sender: failed to inspect file\n");
    size_t packet_count = (size_t)((file_size + PAYLOAD_MAX - 1) / PAYLOAD_MAX);
    data_packet_t *packets = packet_count ? calloc(packet_count, sizeof(*packets)) : NULL;
    if (packet_count && !packets) LOG_FATAL("Sender: out of memory\n");
    for (size_t i = 0; i < packet_count; ++i) {
        long remaining = file_size - (long)i * PAYLOAD_MAX;
        uint16_t length = (uint16_t)(remaining > PAYLOAD_MAX ? PAYLOAD_MAX : remaining);
        char buffer[PAYLOAD_MAX];
        if (fread(buffer, 1, length, file) != length) {
            free(packets);
            fclose(file);
            LOG_FATAL("Sender: failed to read file\n");
        }
        make_packet(&packets[i].packet, 0, 0, buffer, length);
    }
    fclose(file);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) LOG_FATAL("Sender: failed to create socket\n");
    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons((uint16_t)port_value);
    if (inet_pton(AF_INET, argv[1], &destination.sin_addr) != 1)
        LOG_FATAL("Sender: invalid receiver address\n");
    /* Connecting a UDP socket restricts receives to the selected peer while
     * preserving datagram semantics and lets recv() be used below. */
    if (connect(fd, (const struct sockaddr *)&destination, sizeof(destination)) < 0)
        LOG_FATAL("Sender: failed to connect socket\n");
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    uint32_t initial_sequence;
    if (establish_connection(fd, &destination, &initial_sequence) < 0)
        LOG_FATAL("Sender: failed to establish connection\n");
    for (size_t i = 0; i < packet_count; ++i) {
        packets[i].packet.rtp.seq_num = seq_add(initial_sequence, (uint32_t)i + 1);
        packets[i].packet.rtp.checksum = 0;
        packets[i].packet.rtp.checksum = compute_checksum(
            &packets[i].packet, sizeof(rtp_header_t) + packets[i].packet.rtp.length);
    }
    if (transmit_data(fd, &destination, packets, packet_count, (size_t)window_value,
                      mode_value == 1) < 0)
        LOG_FATAL("Sender: failed to transmit data\n");
    if (terminate_connection(fd, &destination,
                             seq_add(initial_sequence, (uint32_t)packet_count + 1)) < 0)
        LOG_FATAL("Sender: failed to terminate connection\n");
    free(packets);
    close(fd);
    LOG_DEBUG("Sender: exiting...\n");
    return 0;
}
