#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

struct Process {
    pid_t pid = -1;
    int input = -1;
};

std::atomic<int> nextPort{39000};

Process startProcess(const fs::path &executable, const std::vector<std::string> &arguments,
                     const fs::path &workingDirectory, bool withInput) {
    fs::remove_all(workingDirectory);
    fs::create_directories(workingDirectory);
    int pipefd[2] = {-1, -1};
    if (withInput && pipe(pipefd) != 0) return {};
    const pid_t pid = fork();
    if (pid < 0) return {};
    if (pid == 0) {
        if (chdir(workingDirectory.c_str()) != 0) _exit(120);
        const int nullfd = open("/dev/null", O_RDWR);
        if (withInput) {
            dup2(pipefd[0], STDIN_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
        } else {
            dup2(nullfd, STDIN_FILENO);
        }
        dup2(nullfd, STDOUT_FILENO);
        dup2(nullfd, STDERR_FILENO);
        close(nullfd);
        std::vector<char *> argv;
        std::vector<std::string> args;
        args.push_back(executable.string());
        args.insert(args.end(), arguments.begin(), arguments.end());
        for (auto &arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        execv(executable.c_str(), argv.data());
        _exit(121);
    }
    if (withInput) {
        close(pipefd[0]);
        return {pid, pipefd[1]};
    }
    return {pid, -1};
}

void stopProcess(Process &process) {
    if (process.input >= 0) {
        close(process.input);
        process.input = -1;
    }
    if (process.pid > 0) {
        kill(process.pid, SIGKILL);
        waitpid(process.pid, nullptr, 0);
        process.pid = -1;
    }
}

void sendCommand(const Process &client, const std::string &command) {
    ASSERT_GE(client.input, 0);
    ASSERT_EQ(static_cast<ssize_t>(command.size()), write(client.input, command.data(), command.size()));
}

fs::path executablePath(const char *name) {
    return fs::current_path() / name;
}

struct Fixture {
    fs::path root = fs::current_path();
    fs::path serverDir = root / "additional_server";
    fs::path clientDir = root / "additional_client";
    int port = nextPort.fetch_add(1);
    Process server;
    Process client;

    void start(bool standardServer, bool standardClient, int testId) {
        std::vector<std::string> serverArguments = {"127.0.0.1", std::to_string(port)};
        if (standardServer) serverArguments.push_back(std::to_string(testId));
        server = startProcess(executablePath(standardServer ? "ftp_server_std" : "ftp_server"),
                              serverArguments, serverDir, false);
        client = startProcess(executablePath(standardClient ? "ftp_client_std" : "ftp_client"),
                              {std::to_string(testId)}, clientDir, true);
        ASSERT_GT(server.pid, 0);
        ASSERT_GT(client.pid, 0);
        usleep(500000);
    }

    void open() { sendCommand(client, "open 127.0.0.1 " + std::to_string(port) + "\n"); usleep(500000); }

    ~Fixture() { stopProcess(client); stopProcess(server); }
};

std::vector<char> makeData(size_t size, uint32_t seed) {
    std::vector<char> data(size);
    std::mt19937 generator(seed);
    for (auto &value : data) value = static_cast<char>(generator() & 0xff);
    return data;
}

void writeBytes(const fs::path &path, const std::vector<char> &data) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output);
    output.write(data.data(), data.size());
    ASSERT_TRUE(output);
}

std::vector<char> readBytes(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input);
    return std::vector<char>(std::istreambuf_iterator<char>(input), {});
}

std::string randomFileName(uint32_t seed) {
    std::mt19937 generator(seed);
    const unsigned number = 100 + generator() % 900;
    return std::to_string(number) + ".txt";
}

constexpr size_t kBigFileSize = 1024 * 1024;

}  // namespace

TEST(FTPServerAdditional, GetBig) {
    Fixture fixture;
    fixture.start(false, true, 3);
    const auto data = makeData(kBigFileSize, 1);
    const auto fileName = randomFileName(11);
    writeBytes(fixture.serverDir / fileName, data);
    fixture.open();
    sendCommand(fixture.client, "get " + fileName + "\n");
    usleep(1500000);
    EXPECT_EQ(readBytes(fixture.clientDir / fileName), data);
}

TEST(FTPServerAdditional, PutBig) {
    Fixture fixture;
    fixture.start(false, true, 4);
    const auto data = makeData(kBigFileSize, 2);
    const auto fileName = randomFileName(12);
    writeBytes(fixture.clientDir / fileName, data);
    fixture.open();
    sendCommand(fixture.client, "put " + fileName + "\n");
    usleep(1500000);
    EXPECT_EQ(readBytes(fixture.serverDir / fileName), data);
}

TEST(FTPServerAdditional, MultiCdList) {
    constexpr int kClientCount = 16;
    const fs::path root = fs::current_path();
    const fs::path serverDir = root / "additional_multi_server";
    fs::remove_all(serverDir);
    fs::create_directories(serverDir);
    const int port = nextPort.fetch_add(1);
    Process server = startProcess(executablePath("ftp_server"), {"127.0.0.1", std::to_string(port)}, serverDir, false);
    ASSERT_GT(server.pid, 0);
    std::vector<Process> clients;
    std::vector<fs::path> clientDirs;
    for (int i = 0; i < kClientCount; ++i) {
        const fs::path directory = root / ("additional_multi_client" + std::to_string(i));
        clientDirs.push_back(directory);
        clients.push_back(startProcess(executablePath("ftp_client_std"), {"6"}, directory, true));
        ASSERT_GT(clients.back().pid, 0);
        fs::create_directory(serverDir / ("dir" + std::to_string(i)));
        writeBytes(serverDir / ("dir" + std::to_string(i)) / "only.txt", {'o', 'k', '\n'});
    }
    usleep(500000);
    for (int i = 0; i < kClientCount; ++i) {
        sendCommand(clients[i], "open 127.0.0.1 " + std::to_string(port) + "\n");
        sendCommand(clients[i], "cd dir" + std::to_string(i) + "\n");
        sendCommand(clients[i], "ls\n");
    }
    usleep(1500000);
    for (const auto &directory : clientDirs) {
        std::ifstream input(directory / "tmp.out");
        ASSERT_TRUE(input);
        std::string listing((std::istreambuf_iterator<char>(input)), {});
        EXPECT_EQ(listing, "only.txt\n");
    }
    for (auto &client : clients) stopProcess(client);
    stopProcess(server);
}

TEST(FTPClientAdditional, GetBig) {
    Fixture fixture;
    fixture.start(true, false, 3);
    const auto data = makeData(kBigFileSize, 3);
    const auto fileName = randomFileName(13);
    writeBytes(fixture.serverDir / fileName, data);
    fixture.open();
    sendCommand(fixture.client, "get " + fileName + "\n");
    usleep(1500000);
    EXPECT_EQ(readBytes(fixture.clientDir / fileName), data);
}

TEST(FTPClientAdditional, PutBig) {
    Fixture fixture;
    fixture.start(true, false, 4);
    const auto data = makeData(kBigFileSize, 4);
    const auto fileName = randomFileName(14);
    writeBytes(fixture.clientDir / fileName, data);
    fixture.open();
    sendCommand(fixture.client, "put " + fileName + "\n");
    usleep(1500000);
    EXPECT_EQ(readBytes(fixture.serverDir / fileName), data);
}
