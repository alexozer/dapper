// Sockets code largely stolen from bspwm

#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include <cstdio>
#include <iostream>
#include <memory>
#include <queue>
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

  std::vector<TagPtr> m_view_tags;
  std::unordered_map<std::string, TagPtr> m_class_tags;

  int m_next_vapp_id;
  std::unordered_map<int, TagPtr> m_vapp_tags;
  std::priority_queue<int, std::vector<int>, std::greater<int>> m_free_vapp_ids;

  std::string m_shell;

  void spawn_shell(const std::string &shell_cmd) {
    const char *cmd[] = {m_shell.c_str(), "-c", shell_cmd.c_str(), nullptr};
    spawn(cmd, false);
  }

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

  void evacuate_split_area() {
    // Restore all windows from the split desktop
    if (m_view_tags.size() > 1) {
      for (auto &tag : m_view_tags) {
        tag->in_split = false;
        for (auto &win : tag->windows) {
          move_window(win, tag->origin_desk);
        }
      }
    }
    m_view_tags.clear();
  }

  std::string new_desk_name() {
    if (m_free_desks.size() > 0) {
      std::string name = m_free_desks.back();
      m_free_desks.pop_back();
      return name;
    }
    return std::to_string(m_next_desk++);
  }

  int new_vapp_id() {
    if (m_free_vapp_ids.size() > 0) {
      int id = m_free_vapp_ids.top;
      m_free_vapp_ids.pop();
      return id;
    }
    return m_next_vapp_id++;
  }

  void show_tag(TagPtr tag) {
    evacuate_split_area();
    focus_desk(tag->origin_desk);
    m_view_tags.push_back(tag);
  }

  void show_app(const std::vector<std::string> &classes,
                const std::string &launch_cmd) {
    // Check if a tag is already associated with any of theses classes
    // (if so, assume the tag is already associated with all of them)
    bool tag_exists = false;
    for (auto &cls : classes) {

      auto it = m_class_tags.find(cls);
      if (it != m_class_tags.cend()) {
        tag_exists = true;
        auto &tag = it->second;

        // If there's no windows there, launch one
        if (tag->windows.size() == 0) {
          spawn_shell(launch_cmd);
          // Only handle the window once its creation event occurs
        } else {
          // Focus the tag
          show_tag(tag);
        }

        break; // Assume the tag is associated with all provided classes
      }
    }

    if (!tag_exists) {
      // Create a tag for it
      auto tag = std::make_shared<Tag>();
      tag->direction = SplitDirection::none;
      tag->in_split = false;
      tag->origin_desk = new_desk_name();
      make_desk(tag->origin_desk);

      // Associate the tag with each window class
      for (auto &cls : classes) {
        m_class_tags[cls] = tag;
      }

      // Move all matching windows to new desktop
      bool existing_windows = false;
      for (auto &vapp_tag : m_vapp_tags) {
        // Record windows that don't need to be removed from tag
        std::vector<Window> keep_windows;

        auto &windows = vapp_tag.second->windows;

        for (auto win_it = windows.begin(); win_it != windows.end();) {
          for (auto &cls : classes) {
            if (win_it->second == cls) {
              existing_windows = true;
              move_window(*win_it, tag->origin_desk);
              windows.erase(win_it);
            } else {
              ++win_it;
            }
          }
        }

        vapp_tag.second->windows = keep_windows;
      }

      if (existing_windows) {
        // Clean up any now-empty vapps
        for (auto it = m_vapp_tags.begin(); it != m_vapp_tags.end();) {
          if (it->second->windows.size() == 0) {
            m_vapp_tags.erase(it);
            m_free_vapp_ids.push(it->first);
          } else {
            ++it;
          }
        }
      } else {
        // Spawn program for this tag, but don't focus desktop yet
        spawn_shell(launch_cmd);
      }
    }
  }

  void show_vapp(int vapp_idx) {
    auto it = m_vapp_tags.find(vapp_idx);
    if (it == m_vapp_tags.end())
      return;

    show_tag(it->second);
  }

public:
  Dapper() : m_next_desk{0} {
    // Determine a suitable shellfor spawning applications
    m_shell = getenv("SHELL");
    if (m_shell.size() == 0) {
      m_shell = "sh";
    }

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

      m_vapp_tags.[new_vapp_id()] = tag;
    }

    // Focus a vapp tag if it exists, otherwose just focus the split desk
    if (m_vapp_tags.size() > 0) {
      focus_desk(m_vapp_tags[0]->origin_desk);
    } else {
      focus_desk(m_split_desk);
    }
  }

  void handle_command(const std::string &command) {}

  void handle_events(const std::string &events) {}
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

  Dapper dapper;
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
          std::string events_str(buffer, n);
          dapper.handle_events(events_str);
        }
      }

      if (FD_ISSET(sock_fd, &descriptors)) {
        int cli_fd = accept(sock_fd, NULL, 0);
        if (cli_fd > 0) {
          int n = recv(cli_fd, buffer, BUF_SIZE, 0);
          if (n > 0) {
            std::string commands_str(buffer, n);
            dapper.handle_command(commands_str);
          }
        }
      }
    }
  }

  close(sock_fd);
  unlink(SOCKET_PATH);
  close(pipe_read);
}