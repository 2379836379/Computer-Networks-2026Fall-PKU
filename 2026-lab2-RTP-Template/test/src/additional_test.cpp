#include <gtest/gtest.h>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef SOURCE_DIR
#define SOURCE_DIR "."
#endif
#ifndef REFERENCE_BIN_DIR
#define REFERENCE_BIN_DIR SOURCE_DIR "/test/bin"
#endif

namespace {

constexpr size_t kInputSize = 2 * 1024 * 1024;
constexpr int kProcessTimeoutSeconds = 150;
int next_port = 52000 + (getpid() % 10000);

class RtpAdditionalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char input_template[] = "/tmp/rtp-additional-input-XXXXXX";
    int fd = mkstemp(input_template);
    ASSERT_GE(fd, 0);
    input_path_ = input_template;

    FILE* random = fopen("/dev/urandom", "rb");
    ASSERT_NE(random, nullptr);
    std::vector<char> bytes(kInputSize);
    ASSERT_EQ(fread(bytes.data(), 1, bytes.size(), random), bytes.size());
    fclose(random);
    ASSERT_EQ(write_all(fd, bytes.data(), bytes.size()), 0);
    close(fd);

    char output_template[] = "/tmp/rtp-additional-output-XXXXXX";
    fd = mkstemp(output_template);
    ASSERT_GE(fd, 0);
    output_path_ = output_template;
    close(fd);
    ASSERT_EQ(unlink(output_path_.c_str()), 0);
  }

  void TearDown() override {
    if (!input_path_.empty()) unlink(input_path_.c_str());
    if (!output_path_.empty()) unlink(output_path_.c_str());
  }

  int RunCase(int window, int mode, int error_mask, int probability,
              bool local_sender) {
    const int port = next_port++;
    const std::string port_text = std::to_string(port);
    const std::string window_text = std::to_string(window);
    const std::string mode_text = std::to_string(mode);
    const std::string error_text = std::to_string(error_mask);
    const std::string probability_text = std::to_string(probability);
    const std::string root = SOURCE_DIR;
    const std::string local_sender_path = root + "/build/sender";
    const std::string local_receiver_path = root + "/build/receiver";
    const std::string reference_sender_path =
        std::string(REFERENCE_BIN_DIR) + "/test_sender";
    const std::string reference_receiver_path =
        std::string(REFERENCE_BIN_DIR) + "/test_receiver";

    pid_t receiver_pid = fork();
    if (receiver_pid == 0) {
      if (local_sender) {
        execl(reference_receiver_path.c_str(), reference_receiver_path.c_str(),
              port_text.c_str(), output_path_.c_str(), window_text.c_str(),
              mode_text.c_str(), error_text.c_str(), probability_text.c_str(),
              nullptr);
      } else {
        execl(local_receiver_path.c_str(), local_receiver_path.c_str(),
              port_text.c_str(), output_path_.c_str(), window_text.c_str(),
              mode_text.c_str(), nullptr);
      }
      _exit(127);
    }
    if (receiver_pid < 0) return -1;

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pid_t sender_pid = fork();
    if (sender_pid == 0) {
      if (local_sender) {
        execl(local_sender_path.c_str(), local_sender_path.c_str(), "127.0.0.1",
              port_text.c_str(), input_path_.c_str(), window_text.c_str(),
              mode_text.c_str(), nullptr);
      } else {
        execl(reference_sender_path.c_str(), reference_sender_path.c_str(),
              "127.0.0.1", port_text.c_str(), input_path_.c_str(),
              window_text.c_str(), mode_text.c_str(), error_text.c_str(),
              probability_text.c_str(), nullptr);
      }
      _exit(127);
    }
    if (sender_pid < 0) {
      kill(receiver_pid, SIGTERM);
      waitpid(receiver_pid, nullptr, 0);
      return -1;
    }

    int sender_status = 0;
    int receiver_status = 0;
    bool sender_done = false;
    bool receiver_done = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(kProcessTimeoutSeconds);
    while ((!sender_done || !receiver_done) &&
           std::chrono::steady_clock::now() < deadline) {
      if (!sender_done) {
        pid_t result = waitpid(sender_pid, &sender_status, WNOHANG);
        if (result == sender_pid) sender_done = true;
        if (result < 0) break;
      }
      if (!receiver_done) {
        pid_t result = waitpid(receiver_pid, &receiver_status, WNOHANG);
        if (result == receiver_pid) receiver_done = true;
        if (result < 0) break;
      }
      if ((sender_done && (!WIFEXITED(sender_status) || WEXITSTATUS(sender_status) != 0)) ||
          (receiver_done && (!WIFEXITED(receiver_status) || WEXITSTATUS(receiver_status) != 0)))
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!sender_done) kill(sender_pid, SIGTERM);
    if (!receiver_done) kill(receiver_pid, SIGTERM);
    if (!sender_done) waitpid(sender_pid, &sender_status, 0);
    if (!receiver_done) waitpid(receiver_pid, &receiver_status, 0);

    if (!WIFEXITED(sender_status) || WEXITSTATUS(sender_status) != 0 ||
        !WIFEXITED(receiver_status) || WEXITSTATUS(receiver_status) != 0)
      return -1;
    return files_equal(input_path_, output_path_) ? 1 : -1;
  }

 private:
  static int write_all(int fd, const char* data, size_t length) {
    size_t written = 0;
    while (written < length) {
      ssize_t count = write(fd, data + written, length - written);
      if (count <= 0) return -1;
      written += static_cast<size_t>(count);
    }
    return 0;
  }

  static bool files_equal(const std::string& left, const std::string& right) {
    FILE* a = fopen(left.c_str(), "rb");
    FILE* b = fopen(right.c_str(), "rb");
    if (!a || !b) {
      if (a) fclose(a);
      if (b) fclose(b);
      return false;
    }
    char a_buffer[65536];
    char b_buffer[65536];
    bool equal = true;
    for (;;) {
      size_t a_size = fread(a_buffer, 1, sizeof(a_buffer), a);
      size_t b_size = fread(b_buffer, 1, sizeof(b_buffer), b);
      if (a_size != b_size || memcmp(a_buffer, b_buffer, a_size) != 0) {
        equal = false;
        break;
      }
      if (a_size < sizeof(a_buffer)) {
        equal = feof(a) && feof(b);
        break;
      }
    }
    fclose(a);
    fclose(b);
    return equal;
  }

  std::string input_path_;
  std::string output_path_;
};

TEST_F(RtpAdditionalTest, NORMAL_HUGE_WINDOW) {
  ASSERT_EQ(RunCase(20000, 0, 0, 0, true), 1) << "local sender";
  ASSERT_EQ(RunCase(20000, 0, 0, 0, false), 1) << "local receiver";
}

TEST_F(RtpAdditionalTest, NORMAL_OPT_HUGE_WINDOW) {
  ASSERT_EQ(RunCase(20000, 1, 0, 0, true), 1) << "local sender";
  ASSERT_EQ(RunCase(20000, 1, 0, 0, false), 1) << "local receiver";
}

TEST_F(RtpAdditionalTest, OPT_RECEIVER_MIXED_3) {
  ASSERT_EQ(RunCase(32, 1, 15, 60, false), 1);
}

TEST_F(RtpAdditionalTest, OPT_RECEIVER_MIXED_4) {
  ASSERT_EQ(RunCase(32, 1, 11, 50, false), 1);
}

TEST_F(RtpAdditionalTest, OPT_SENDER_MIXED_3) {
  ASSERT_EQ(RunCase(32, 1, 15, 60, true), 1);
}

TEST_F(RtpAdditionalTest, OPT_SENDER_MIXED_4) {
  ASSERT_EQ(RunCase(32, 1, 11, 50, true), 1);
}

}  // namespace

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
