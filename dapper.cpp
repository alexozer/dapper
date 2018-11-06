// Sockets code largely stolen from bspwm

#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include <cstdio>
#include <iostream>
#include <memory>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace rapidjson;

static const char *SOCKET_PATH = "/tmp/dapper.socket";

#define MAX(A, B) ((A) > (B) ? (A) : (B))

static volatile bool running = false;

constexpr size_t BUF_SIZE = 10240;
char buffer[BUF_SIZE];

void err(std::string msg) {
  std::cerr << msg << std::endl;
  std::exit(1);
}

void execute(const char *cmd[]) {
  setsid();
  execvp(cmd[0], (char **)cmd);
}

void spawn(const char *cmd[], bool sync) {
  if (fork() == 0) {
    if (sync) {
      execute(cmd);
    } else {
      if (fork() == 0) {
        execute(cmd);
      }
    }
  }
  wait(NULL);
}

FILE *capture(const char *cmd[]) {
  int pipe_fds[2];
  pipe(pipe_fds);

  int pid = fork();
  if (pid == 0) {
    dup2(pipe_fds[1], STDOUT_FILENO);
    execvp(cmd[0], (char **)cmd);
  }

  close(pipe_fds[1]);
  return fdopen(pipe_fds[0], "r");
}

Document parse_bspc_json(const char *cmd[]) {
  FILE *stream = capture(cmd);
  FileReadStream is(stream, buffer, BUF_SIZE);

  Document d;
  d.ParseStream(is);

  fclose(stream);
  return d;
}

// window id * window class
typedef std::pair<int, std::string> Window;

enum class SplitDirection { left, right, none };

struct Tag {
  std::string origin_desk;
  bool in_split;
  std::vector<Window> windows;
  SplitDirection direction;
};

typedef std::shared_ptr<Tag> TagPtr;

class Dapper {
private:
  int m_next_desk;
  std::vector<std::string> m_free_desks;

  std::string m_split_desk;
  std::unordered_map<std::string, TagPtr> m_class_tags;
  std::vector<TagPtr> m_vapp_tags;
  std::vector<TagPtr> m_view_tags;

  void get_desk_windows(const Value &desk, std::vector<Window> &windows) {
    const auto &root = desk["root"];
    if (root["firstChild"].IsNull()) {
      windows.emplace_back(root["id"].GetInt(),
                           root["client"]["className"].GetString());
    } else {
      get_desk_windows(root["firstChild"], windows);
      get_desk_windows(root["secondChild"], windows);
    }
  }

  void make_desk(const std::string &name) {
    const char *cmd[] = {"bspc", "monitor", "--add-desktops", name.c_str(),
                         nullptr};
    spawn(cmd, true);
  }

  void focus_desk(const std::string &name) {
    const char *cmd[] = {"bspc", "desktop", name.c_str(), "--focus", nullptr};
    spawn(cmd, true);
  }

  void move_window(const Window &window, const std::string &desk_name) {
    const char *cmd[] = {"bspc",
                         "node",
                         std::to_string(window.first).c_str(),
                         "--to-desktop",
                         desk_name.c_str(),
                         nullptr};
    spawn(cmd, true);
  }

public:
  Dapper() : m_next_desk{0} {
    const char *state_cmd[] = {"bspc", "query", "-m", "-T", nullptr};
    auto json = parse_bspc_json(state_cmd);

    // Find a number we can use to start allocating desktops at
    for (const auto &desk : json["desktops"].GetArray()) {
      int n;
      try {
        n = std::stoi(desk["name"].GetString());
      } catch (std::invalid_argument e) {
        continue;
      }

      if (n >= m_next_desk) {
        m_next_desk = n + 1;
      }
    }

    // Create a desktop for showing split views
    m_split_desk = m_next_desk++;
    make_desk(m_split_desk);

    // Get all window ids and tags
    std::vector<Window> windows;
    for (const auto &desk : json["desktops"].GetArray()) {
      get_desk_windows(desk, windows);
    }

    // Treat all windows as virtual apps for now, assign them tags
    for (auto &window : windows) {
      auto tag = std::make_shared<Tag>();
      tag->origin_desk = m_next_desk++;
      tag->windows.push_back(window);
      tag->direction = SplitDirection::none;
      tag->in_split = false;

      make_desk(tag->origin_desk);
      move_window(window, tag->origin_desk);

      m_vapp_tags.push_back(tag);
    }

    // Focus a vapp tags if it exists, otherwose just focus the split desk
    if (m_vapp_tags.size() > 0) {
      focus_desk(m_vapp_tags[0]->origin_desk);
    } else {
      focus_desk(m_split_desk);
    }
  }
};

void sig_handler(int sig) {
  if (sig == SIGINT || sig == SIGHUP || sig == SIGTERM) {
    running = false;
  }
}

int main() {
  // Create fd for `bspc subscribe` process's stdout

  int pipe_fds[2];
  if (pipe(pipe_fds) == -1) {
    err("Failed to create pipe");
  }

  int pipe_read = pipe_fds[0], pipe_write = pipe_fds[1];

  int pid = fork();
  if (pid == 0) {
    dup2(pipe_write, STDOUT_FILENO);
    const char *const args[] = {"bspc", "subscribe", "node", NULL};
    execvp(args[0], (char *const *)args);
  } else {
    close(pipe_write);
  }

  signal(SIGINT, sig_handler);
  signal(SIGHUP, sig_handler);
  signal(SIGTERM, sig_handler);
  signal(SIGPIPE, SIG_IGN);

  // Create fd for dapper's communication socket

  struct sockaddr_un sock_address;
  sock_address.sun_family = AF_UNIX;
  std::snprintf(sock_address.sun_path, sizeof(sock_address.sun_path), "%s",
                SOCKET_PATH);

  int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);

  if (sock_fd == -1) {
    err("Couldn't create the socket");
  }

  unlink(SOCKET_PATH);
  if (bind(sock_fd, (struct sockaddr *)&sock_address, sizeof(sock_address)) ==
      -1) {
    err("Couldn't bind a name to the socket");
  }

  if (listen(sock_fd, SOMAXCONN) == -1) {
    err("Couldn't listen to the socket");
  }

  // Loop over input fds

  running = true;
  int max_fd = MAX(sock_fd, pipe_read);

  while (running) {
    fd_set descriptors;
    FD_ZERO(&descriptors);
    FD_SET(sock_fd, &descriptors);
    FD_SET(pipe_read, &descriptors);

    if (select(max_fd + 1, &descriptors, NULL, NULL, NULL) > 0) {
      if (FD_ISSET(pipe_read, &descriptors)) {
        int n = read(pipe_read, buffer, BUF_SIZE);
        if (n > 0) {
          std::string message(buffer, n);

          std::cout << message << std::endl;
        }
      }

      if (FD_ISSET(sock_fd, &descriptors)) {
        int cli_fd = accept(sock_fd, NULL, 0);
        if (cli_fd > 0) {
          int n = recv(cli_fd, buffer, BUF_SIZE, 0);
          if (n > 0) {
            std::string message(buffer, n);

            std::cout << message << std::endl;
          }
        }
      }
    }
  }

  close(sock_fd);
  unlink(SOCKET_PATH);
  close(pipe_read);
}