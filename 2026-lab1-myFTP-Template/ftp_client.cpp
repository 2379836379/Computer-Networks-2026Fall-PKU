#include <arpa/inet.h>
#include <algorithm>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr char kMagic[] = "\xc1\xa1\x10" "ftp";
constexpr uint32_t kHeaderSize = 12; constexpr size_t kBufferSize = 64 * 1024;
enum MessageType : uint8_t { OPEN_CONN_REQUEST = 0xA1, OPEN_CONN_REPLY = 0xA2, LIST_REQUEST = 0xA3, LIST_REPLY = 0xA4, CHANGE_DIR_REQUEST = 0xA5, CHANGE_DIR_REPLY = 0xA6, GET_REQUEST = 0xA7, GET_REPLY = 0xA8, PUT_REQUEST = 0xA9, PUT_REPLY = 0xAA, SHA_REQUEST = 0xAB, SHA_REPLY = 0xAC, QUIT_REQUEST = 0xAD, QUIT_REPLY = 0xAE, FILE_DATA = 0xFF };
struct __attribute__((packed)) Header { char protocol[6]; uint8_t type; uint8_t status; uint32_t length; };
bool sendAll(int fd, const void *data, size_t length) { const char *p = static_cast<const char *>(data); while (length) { ssize_t n = send(fd, p, length, MSG_NOSIGNAL); if (n <= 0) return false; p += n; length -= n; } return true; }
bool recvAll(int fd, void *data, size_t length) { char *p = static_cast<char *>(data); while (length) { ssize_t n = recv(fd, p, length, 0); if (n <= 0) return false; p += n; length -= n; } return true; }
bool sendHeader(int fd, uint8_t type, uint8_t status, uint32_t length) { Header h{}; std::memcpy(h.protocol, kMagic, 6); h.type = type; h.status = status; h.length = htonl(length); return sendAll(fd, &h, sizeof(h)); }
bool recvHeader(int fd, Header &h, uint32_t &payloadLength) { if (!recvAll(fd, &h, sizeof(h))) return false; uint32_t length = ntohl(h.length); if (std::memcmp(h.protocol, kMagic, 6) || length < kHeaderSize) return false; payloadLength = length - kHeaderSize; return true; }
bool sendFrame(int fd, uint8_t type, uint8_t status, const std::string &payload) { return payload.size() <= INT_MAX - kHeaderSize && sendHeader(fd, type, status, kHeaderSize + payload.size()) && (payload.empty() || sendAll(fd, payload.data(), payload.size())); }
bool recvTextFrame(int fd, uint8_t expected, std::string &value) { Header h{}; uint32_t length = 0; if (!recvHeader(fd, h, length) || h.type != expected || length > 4096) return false; std::vector<char> data(length); if (length && !recvAll(fd, data.data(), length)) return false; value.assign(data.data(), data.size()); return true; }
bool sendFile(int fd, const std::filesystem::path &path) { std::error_code ec; uintmax_t size = std::filesystem::file_size(path, ec); std::ifstream input(path, std::ios::binary); if (ec || !input || size > INT_MAX - kHeaderSize || !sendHeader(fd, FILE_DATA, 0, kHeaderSize + size)) return false; std::vector<char> data(kBufferSize); while (size) { size_t wanted = std::min<uintmax_t>(data.size(), size); input.read(data.data(), wanted); std::streamsize n = input.gcount(); if (n <= 0 || !sendAll(fd, data.data(), n)) return false; size -= n; } return true; }
bool receiveFile(int fd, const std::filesystem::path &path) { Header h{}; uint32_t length = 0; if (!recvHeader(fd, h, length) || h.type != FILE_DATA) return false; std::ofstream output(path, std::ios::binary | std::ios::trunc); if (!output) return false; std::vector<char> data(kBufferSize); while (length) { size_t wanted = std::min<size_t>(data.size(), length); if (!recvAll(fd, data.data(), wanted)) return false; output.write(data.data(), wanted); if (!output) return false; length -= wanted; } return true; }
bool nameRequest(int fd, uint8_t type, const std::string &name) { return sendFrame(fd, type, 0, name + '\0'); }
bool reply(int fd, uint8_t type, uint8_t &status) { Header h{}; uint32_t length = 0; if (!recvHeader(fd, h, length) || h.type != type || length) return false; status = h.status; return true; }
int connectTo(const std::string &ip, const std::string &port) { sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(static_cast<uint16_t>(std::strtoul(port.c_str(), nullptr, 10))); if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) return -1; int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0 || connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address))) { if (fd >= 0) close(fd); return -1; } return fd; }
void disconnect(int &fd) { if (fd >= 0) close(fd); fd = -1; }
}

int main() {
    std::signal(SIGPIPE, SIG_IGN); int fd = -1; std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream parser(line); std::string operation, argument, extra; parser >> operation >> argument >> extra;
        if (operation.empty()) continue;
        if (fd < 0) {
            if (operation == "quit" && argument.empty()) break;
            if (operation != "open") { std::cerr << "Not connected\n"; continue; }
            std::string port; extra.clear(); parser.clear(); parser.str(line); parser >> operation >> argument >> port >> extra;
            if (port.empty() || !extra.empty()) { std::cerr << "Usage: open <IP> <port>\n"; continue; }
            fd = connectTo(argument, port); uint8_t status = 0;
            if (fd < 0 || !sendHeader(fd, OPEN_CONN_REQUEST, 0, kHeaderSize) || !reply(fd, OPEN_CONN_REPLY, status) || status != 1) { std::cerr << "Connection failed\n"; disconnect(fd); }
            continue;
        }
        bool ok = true;
        if (operation == "ls" && argument.empty()) { ok = sendHeader(fd, LIST_REQUEST, 0, kHeaderSize); std::string files; ok = ok && recvTextFrame(fd, LIST_REPLY, files); if (ok) std::cout << files.c_str(); }
        else if (operation == "cd" && !argument.empty() && extra.empty()) { uint8_t status = 0; ok = nameRequest(fd, CHANGE_DIR_REQUEST, argument) && reply(fd, CHANGE_DIR_REPLY, status); if (ok && !status) std::cerr << "Directory not found\n"; }
        else if (operation == "get" && !argument.empty() && extra.empty()) { uint8_t status = 0; ok = nameRequest(fd, GET_REQUEST, argument) && reply(fd, GET_REPLY, status); if (ok && status) ok = receiveFile(fd, argument); else if (ok) std::cerr << "File not found\n"; }
        else if (operation == "put" && !argument.empty() && extra.empty()) { if (!std::filesystem::is_regular_file(argument)) { std::cerr << "File not found\n"; continue; } uint8_t status = 0; ok = nameRequest(fd, PUT_REQUEST, argument) && reply(fd, PUT_REPLY, status) && sendFile(fd, argument); }
        else if (operation == "sha256" && !argument.empty() && extra.empty()) { uint8_t status = 0; ok = nameRequest(fd, SHA_REQUEST, argument) && reply(fd, SHA_REPLY, status); if (ok && status) { std::string value; ok = recvTextFrame(fd, FILE_DATA, value); if (ok) std::cout << value.c_str(); } else if (ok) std::cerr << "File not found\n"; }
        else if (operation == "quit" && argument.empty()) { uint8_t status = 0; ok = sendHeader(fd, QUIT_REQUEST, 0, kHeaderSize) && reply(fd, QUIT_REPLY, status); disconnect(fd); continue; }
        else { std::cerr << "Invalid command\n"; continue; }
        if (!ok) { std::cerr << "Connection error\n"; disconnect(fd); }
    }
    disconnect(fd); return 0;
}
