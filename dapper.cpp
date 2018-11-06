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

Document json_from_file(FILE *file) {
  FileReadStream is(file, buffer, BUF_SIZE);

  Document d;
  d.ParseStream(is);

  fclose(file);
  return d;
}

Document parse_bspc_json(const char *cmd[]) {
  return json_from_file(capture(cmd));
}

Document parse_config() {
  std::string target_path =
      std::string(getenv("HOME")) + "/.config/dapper/config.json";
  FILE *file = fopen(target_path.c_str(), "r");
  if (!file) {
    err("Could not read config file at: " + target_path);
  }

  return json_from_file(file);
}

struct App {
  std::vector<std::string> commands;
  std::vector<std::string> classes;
};
typedef std::shared_ptr<App> AppPtr;

struct Config {
  std::unordered_map<std::string, AppPtr> apps;
  std::string launcher;
};

class Dapper {
private:
  std::string m_shell;
  Config m_config;

  std::unordered_map<int, std::string> m_window_apps;

  void spawn_shell(const std::string &shell_cmd) {
    const char *cmd[] = {m_shell.c_str(), "-c", shell_cmd.c_str(), nullptr};
    spawn(cmd, false);
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

public:
  Dapper() {
    // Determine an appropriate shell
    m_shell = getenv("SHELL");
    if (m_shell.size() == 0) {
      m_shell = "sh";
    }

    // Read config
    auto config = parse_config();

    for (auto &entry : config.GetObject()) {
      std::vector<std::string> commands;
      for (auto &cmd : entry.value["commands"].GetArray()) {
        commands.push_back(cmd.GetString());
      }

      std::vector<std::string> classes;
      for (auto &cmd : entry.value["commands"].GetArray()) {
        classes.push_back(cmd.GetString());
      }

      auto app = std::make_shared<App>(commands, classes);
      m_config.apps[entry.name.GetString()] = app;
    }

    m_config.launcher = config["launcher"].GetString();

    // Create desktops for all the apps
    for (auto &app : m_config.apps) {
      make_desk(app.first);
    }
  }

  void handle_command(const std::string &command) {
    StringStream ss(command.c_str());
    Document d;
    d.ParseStream(ss);
  }

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