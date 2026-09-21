#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr char kMagic[] = "\xc1\xa1\x10" "ftp";
constexpr uint32_t kHeaderSize = 12;
constexpr size_t kBufferSize = 64 * 1024;
enum MessageType : uint8_t { OPEN_CONN_REQUEST = 0xA1, OPEN_CONN_REPLY = 0xA2, LIST_REQUEST = 0xA3, LIST_REPLY = 0xA4, CHANGE_DIR_REQUEST = 0xA5, CHANGE_DIR_REPLY = 0xA6, GET_REQUEST = 0xA7, GET_REPLY = 0xA8, PUT_REQUEST = 0xA9, PUT_REPLY = 0xAA, SHA_REQUEST = 0xAB, SHA_REPLY = 0xAC, QUIT_REQUEST = 0xAD, QUIT_REPLY = 0xAE, FILE_DATA = 0xFF };
struct __attribute__((packed)) Header { char protocol[6]; uint8_t type; uint8_t status; uint32_t length; };

bool sendAll(int fd, const void *data, size_t length) { const char *p = static_cast<const char *>(data); while (length) { ssize_t n = send(fd, p, length, MSG_NOSIGNAL); if (n <= 0) return false; p += n; length -= n; } return true; }
bool recvAll(int fd, void *data, size_t length) { char *p = static_cast<char *>(data); while (length) { ssize_t n = recv(fd, p, length, 0); if (n <= 0) return false; p += n; length -= n; } return true; }
bool sendHeader(int fd, uint8_t type, uint8_t status, uint32_t length) { Header h{}; std::memcpy(h.protocol, kMagic, 6); h.type = type; h.status = status; h.length = htonl(length); return sendAll(fd, &h, sizeof(h)); }
bool recvHeader(int fd, Header &h, uint32_t &payloadLength) { if (!recvAll(fd, &h, sizeof(h))) return false; uint32_t length = ntohl(h.length); if (std::memcmp(h.protocol, kMagic, 6) || length < kHeaderSize) return false; payloadLength = length - kHeaderSize; return true; }
bool sendFrame(int fd, uint8_t type, uint8_t status, const std::string &payload) { return payload.size() <= INT_MAX - kHeaderSize && sendHeader(fd, type, status, kHeaderSize + payload.size()) && (payload.empty() || sendAll(fd, payload.data(), payload.size())); }
bool recvName(int fd, uint32_t length, std::string &name) { if (!length || length > 4096) return false; std::vector<char> data(length); if (!recvAll(fd, data.data(), length) || data.back() != '\0') return false; name.assign(data.data(), length - 1); return name.find('\0') == std::string::npos; }
bool validName(const std::string &name) { if (name.empty() || name == "." || name == "..") return false; for (unsigned char c : name) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.')) return false; return true; }

bool sendFile(int fd, const std::filesystem::path &path) {
    std::error_code ec; uintmax_t size = std::filesystem::file_size(path, ec); std::ifstream input(path, std::ios::binary);
    if (ec || !input || size > INT_MAX - kHeaderSize || !sendHeader(fd, FILE_DATA, 0, kHeaderSize + size)) return false;
    std::vector<char> data(kBufferSize);
    while (size) { size_t wanted = std::min<uintmax_t>(data.size(), size); input.read(data.data(), wanted); std::streamsize n = input.gcount(); if (n <= 0 || !sendAll(fd, data.data(), n)) return false; size -= n; }
    return true;
}
bool receiveFile(int fd, const std::filesystem::path &path) {
    Header h{}; uint32_t length = 0; if (!recvHeader(fd, h, length) || h.type != FILE_DATA) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc); if (!output) return false; std::vector<char> data(kBufferSize);
    while (length) { size_t wanted = std::min<size_t>(data.size(), length); if (!recvAll(fd, data.data(), wanted)) return false; output.write(data.data(), wanted); if (!output) return false; length -= wanted; }
    return true;
}
std::string shellQuote(const std::string &text) { std::string result = "'"; for (char c : text) result += c == '\'' ? "'\\\"'\\\"'" : std::string(1, c); return result + "'"; }
std::string listDirectory(const std::filesystem::path &directory) { std::vector<std::string> names; std::error_code ec; for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) { if (ec) return {}; names.push_back(entry.path().filename().string()); } std::sort(names.begin(), names.end()); std::string result; for (const auto &name : names) result += name + "\n"; return result; }
std::string checksum(const std::filesystem::path &path) { FILE *pipe = popen(("sha256sum -- " + shellQuote(path.string())).c_str(), "r"); if (!pipe) return {}; std::string result; char data[256]; while (fgets(data, sizeof(data), pipe)) result += data; pclose(pipe); return result; }

void serveClient(int fd, const std::filesystem::path &root) {
    std::filesystem::path directory = root; Header h{}; uint32_t length = 0;
    if (!recvHeader(fd, h, length) || h.type != OPEN_CONN_REQUEST || length != 0 || !sendHeader(fd, OPEN_CONN_REPLY, 1, kHeaderSize)) { close(fd); return; }
    while (recvHeader(fd, h, length)) {
        if (h.type == LIST_REQUEST && length == 0) { std::string result = listDirectory(directory); result += '\0'; if (!sendFrame(fd, LIST_REPLY, 0, result)) break; }
        else if (h.type == CHANGE_DIR_REQUEST) { std::string name; bool ok = recvName(fd, length, name) && validName(name); std::error_code ec; std::filesystem::path candidate = directory / name; bool exists = ok && std::filesystem::is_directory(candidate, ec); if (exists) directory = candidate; if (!sendHeader(fd, CHANGE_DIR_REPLY, exists, kHeaderSize)) break; }
        else if (h.type == GET_REQUEST) { std::string name; bool ok = recvName(fd, length, name) && validName(name); std::error_code ec; std::filesystem::path path = directory / name; bool exists = ok && std::filesystem::is_regular_file(path, ec); if (!sendHeader(fd, GET_REPLY, exists, kHeaderSize) || (exists && !sendFile(fd, path))) break; }
        else if (h.type == PUT_REQUEST) { std::string name; if (!recvName(fd, length, name) || !validName(name) || !sendHeader(fd, PUT_REPLY, 0, kHeaderSize) || !receiveFile(fd, directory / name)) break; }
        else if (h.type == SHA_REQUEST) { std::string name; bool ok = recvName(fd, length, name) && validName(name); std::error_code ec; std::filesystem::path path = directory / name; bool exists = ok && std::filesystem::is_regular_file(path, ec); if (!sendHeader(fd, SHA_REPLY, exists, kHeaderSize)) break; if (exists) { std::string result = checksum(path); result += '\0'; if (!sendFrame(fd, FILE_DATA, 0, result)) break; } }
        else if (h.type == QUIT_REQUEST && length == 0) { sendHeader(fd, QUIT_REPLY, 0, kHeaderSize); break; }
        else break;
    }
    close(fd);
}
}

int main(int argc, char *argv[]) {
    if (argc != 3) return 1; std::signal(SIGPIPE, SIG_IGN);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 10)));
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) return 1;
    int listenFd = socket(AF_INET, SOCK_STREAM, 0); if (listenFd < 0) return 1; int reuse = 1; setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(listenFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(listenFd, 32)) { close(listenFd); return 1; }
    const auto root = std::filesystem::current_path();
    while (true) { int clientFd = accept(listenFd, nullptr, nullptr); if (clientFd < 0) { if (errno == EINTR) continue; break; } std::thread(serveClient, clientFd, root).detach(); }
    close(listenFd); return 0;
}
