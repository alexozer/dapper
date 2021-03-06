// Sockets code largely stolen from bspwm

#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include <cstdio>
#include <iostream>
#include <memory>
#include <queue>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>

using namespace rapidjson;

static const char *SOCKET_PATH = "/tmp/dapper.socket";

#define MAX(A, B) ((A) > (B) ? (A) : (B))

static volatile bool running = false;

constexpr size_t BUF_SIZE = 10240;
char buffer[BUF_SIZE];

void err(const std::string& msg) {
  std::cerr << msg << std::endl;
  std::exit(1);
}

void execute(const char *cmd[]) {
  setsid();
  execvp(cmd[0], (char **) cmd);
}

int spawn(const char *cmd[], bool sync) {
  if (fork() == 0) {
    if (sync) {
      execute(cmd);
    } else {
      if (fork() == 0) {
        execute(cmd);
      }
    }
  }

  int status;
  wait(&status);
  return WEXITSTATUS(status);
}

FILE *capture(const char *cmd[], const std::string& out = "") {
  int in_pipe[2];
  int out_pipe[2];
  pipe(in_pipe);
  pipe(out_pipe);

  int pid = fork();
  if (pid == 0) {
    dup2(in_pipe[1], STDOUT_FILENO);
    dup2(out_pipe[0], STDIN_FILENO);
    close(in_pipe[0]);
    close(out_pipe[1]);

    execvp(cmd[0], (char **) cmd);
  }

  close(in_pipe[1]);
  close(out_pipe[0]);

  if (!out.empty()) {
    write(out_pipe[1], out.c_str(), out.size());
  }
  close(out_pipe[1]);

  return fdopen(in_pipe[0], "r");
}

Document json_from_file(FILE *file) {
  FileReadStream is(file, buffer, BUF_SIZE);

  Document d;
  d.ParseStream(is);

  fclose(file);
  return d;
}

Document capture_cmd_json(const char *cmd[]) {
  return json_from_file(capture(cmd));
}

std::vector<std::string> capture_cmd_lines(const char *cmd[],
                                           const std::string &out = "") {
  // TODO support writing to stdout
  FILE *file = capture(cmd, out);
  std::vector<std::string> lines;

  while (!feof(file)) {
    fgets(buffer, BUF_SIZE, file);
    lines.emplace_back(buffer);
  }

  fclose(file);
  return lines;
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

std::vector<std::string> split_string(const std::string &str, char delim) {
  std::stringstream stream(str);
  std::string word;
  std::vector<std::string> words;

  while (std::getline(stream, word, delim)) {
    words.push_back(word);
  }

  return words;
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

  std::string m_spare_desk; // where to move windows when we need to

  std::unordered_map<std::string, std::string> m_class_app_map; // class -> app
  std::unordered_map<std::string, std::unordered_set<int>>
      m_app_windows;                                  // app -> window ids
  std::unordered_map<int, std::string> m_window_apps; // window id -> app

  int spawn_shell(const std::string &shell_cmd) {
    const char *cmd[] = {m_shell.c_str(), "-c", shell_cmd.c_str(), nullptr};
    return spawn(cmd, false);
  }

  int make_desk(const std::string &name) {
    const char *cmd[] = {"bspc", "monitor", "--add-desktops", name.c_str(),
                         nullptr};
    return spawn(cmd, true);
  }

  int remove_desk(const std::string &name) {
    const char *cmd[] = {"bspc", "desktop", name.c_str(), "--remove", nullptr};
    return spawn(cmd, true);
  }

  int focus_desk(const std::string &name) {
    const char *cmd[] = {"bspc", "desktop", name.c_str(), "--focus", nullptr};
    return spawn(cmd, true);
  }

  int move_window(int wid, const std::string &desk) {
    auto wid_str = std::to_string(wid);
    const char *cmd[] = {
        "bspc", "node", wid_str.c_str(),
        "--to-desktop", desk.c_str(), nullptr};
    return spawn(cmd, true);
  }

  Document monitor_json() {
    const char *cmd[] = {"bspc", "query", "-m", "-T", nullptr};
    return capture_cmd_json(cmd);
  }

  std::string find_spare_desk() {
    for (auto &desk : monitor_json()["desktops"].GetArray()) {
      std::string name = desk["name"].GetString();
      if (m_config.apps.find(name) == m_config.apps.end()) {
        return name;
      }
    }

    std::string spare_name = "spare";
    make_desk(spare_name);
    return spare_name;
  }

  std::string desk_name_of_id(const std::string &desk_id) {
    const char *desk_name_cmd[] = {"bspc", "query", "-d", desk_id.c_str(),
                                   "-D", "--names", nullptr};
    auto lines = capture_cmd_lines(desk_name_cmd);
    return lines[0];
  }

  int wid_of_string(const std::string &str) {
    return std::stoi(str, nullptr, 16);
  }

  void handle_window(int wid, int desk_id) {
    // Determine if window needs moving. If it's an app window it does, if
    // it's a non-app window and it's on an app desktop, it also does.

    // Find window class
    auto wid_str = std::to_string(wid);
    const char *node_cmd[] = {"bspc", "query", "-n",
                              wid_str.c_str(), "-T", nullptr};
    auto node_json = capture_cmd_json(node_cmd);
    std::string cls = node_json["client"]["className"].GetString();

    auto cls_it = m_class_app_map.find(cls);
    if (cls_it != m_class_app_map.end()) {
      // This is a valid app window!
      std::string &app = cls_it->second;

      m_app_windows[app].emplace(wid);
      m_window_apps[wid] = app;

    } else {
      // Not an app window, move to other desktop if on app desktop
      std::string desk_name = desk_name_of_id(std::to_string(desk_id));
      if (m_config.apps.find(desk_name) != m_config.apps.end()) {
        move_window(wid, m_spare_desk);
      }
    }
  }

  typedef std::vector<std::pair<int, int>> win_desk_list;

  void desk_windows(int desk_id, const Value& node_val, win_desk_list& result) {
    if (node_val["firstChild"].IsNull()) {
      result.emplace_back(node_val["id"].GetInt(), desk_id);
      return;
    } else {
      desk_windows(desk_id, node_val["firstChild"], result);
      desk_windows(desk_id, node_val["secondChild"], result);
    }
  };

  win_desk_list current_windows() {
    win_desk_list result;
    auto json = monitor_json();

    // get all windows and desktops
    for (auto &desk_val : json["desktops"].GetArray()) {
      int desk_id = desk_val["id"].GetInt();
      auto &root_node = desk_val["root"];
      if (!root_node.IsNull()) {
        desk_windows(desk_id, root_node, result);
      }
    }

    return result;
  }

public:
  Dapper() {
    // Determine an appropriate shell
    m_shell = getenv("SHELL");
    if (m_shell.empty()) {
      m_shell = "sh";
    }

    // Read config
    auto config = parse_config();
    auto& app_config = config["apps"];

    for (auto &entry : app_config.GetObject()) {
      std::vector<std::string> commands;
      for (auto &cmd : entry.value["commands"].GetArray()) {
        commands.emplace_back(cmd.GetString());
      }

      std::vector<std::string> classes;
      for (auto &cls : entry.value["classes"].GetArray()) {
        classes.emplace_back(cls.GetString());
      }

      auto app = std::make_shared<App>();
      app->commands = commands;
      app->classes = classes;
      m_config.apps[entry.name.GetString()] = app;
    }

    m_config.launcher = config["launcher"].GetString();

    // Determine a spare desktop to move windows to if needed
    m_spare_desk = find_spare_desk();

    // Build class -> app map
    // Build app -> windows map
    // Build window -> app map
    for (auto &app : m_config.apps) {
      m_app_windows[app.first] = {};
      for (auto &cls : app.second->classes) {
        m_class_app_map[cls] = app.first;
      }
    }

    // Create desktops for all the apps
    for (auto &app : m_config.apps) {
      make_desk(app.first);
    }

    // Process all existing windows like they were newly opened
    for (auto& wid_desk_id : current_windows()) {
      handle_window(wid_desk_id.first, wid_desk_id.second);
    }
  }

  ~Dapper() {
    for (auto &app : m_config.apps) {
      remove_desk(app.first);
    }
  }

  void handle_command(const std::string &command) {
    auto words = split_string(command, ' ');
    if (words.empty()) {
      return;
    }

    auto &app = words[0];
    bool pull = words.size() > 1 && words[1] == "--pull";

    // TODO remove
    for (auto& word : words) {
      std::cout << word << ' ';
    }
    std::cout << std::endl;

    if (m_config.apps.find(app) == m_config.apps.end()) {
      return;
    }

    const std::string &target_desk = pull ? "focused" : app;

    if (!m_app_windows[app].empty()) {
      for (int wid : m_app_windows[app]) {
        move_window(wid, target_desk);
      }
      if (!pull) {
        focus_desk(app);
      }

    } else {
      // Try to open app

      if (!pull) {
        focus_desk(app);
      }

      auto &commands = m_config.apps[app]->commands;
      if (commands.size() == 1) {
        spawn_shell(commands[0]);

      } else {
        std::stringstream commands_combined;
        for (auto &cmd : commands) {
          commands_combined << cmd;
        }

        const char *launcher_cmd[] = {m_config.launcher.c_str(), nullptr};
        auto lines = capture_cmd_lines(launcher_cmd, commands_combined.str());

        if (!lines.empty()) {
          spawn_shell(lines[0]);
        }
      }
    }
  }

  void handle_events(const std::string &events) {
    for (auto &line : split_string(events, '\n')) {
      auto words = split_string(line, ' ');
      if (words.empty()) {
        continue;
      }

      if (words[0] == "node_add") {
        auto &desk_id_str = words[2];
        auto &node_id_str = words[4];
        int wid = wid_of_string(node_id_str);
        int desk_id = wid_of_string(desk_id_str);
        handle_window(wid, desk_id);

      } else if (words[0] == "node_remove") {
        auto &node_id = words[3];
        int wid = wid_of_string(node_id);

        auto wapp_it = m_window_apps.find(wid);
        if (wapp_it != m_window_apps.end()) {
          auto &app = wapp_it->second;
          m_app_windows[app].erase(wid);

          m_window_apps.erase(wapp_it);
        }

      } else if (words[0] == "desktop_remove") {
        auto &desk_id = words[2];
        std::string desk_name = desk_name_of_id(desk_id);

        if (desk_name == m_spare_desk) {
          m_spare_desk = find_spare_desk();
        }
      }
    }
  }
};

void sig_handler(int sig) {
  if (sig == SIGINT || sig == SIGHUP || sig == SIGTERM) {
    running = false;
  }
}

int main() {
  // Create file descriptor for `bspc subscribe` process's stdout

  int pipe_fds[2];
  if (pipe(pipe_fds) == -1) {
    err("Failed to create pipe");
  }

  int pipe_read = pipe_fds[0], pipe_write = pipe_fds[1];

  int pid = fork();
  if (pid == 0) {
    dup2(pipe_write, STDOUT_FILENO);
    const char *const args[] = {"bspc", "subscribe", "node", nullptr};
    execvp(args[0], (char *const *) args);
  } else {
    close(pipe_write);
  }

  signal(SIGINT, sig_handler);
  signal(SIGHUP, sig_handler);
  signal(SIGTERM, sig_handler);
  signal(SIGPIPE, SIG_IGN);

  // Create fd for dapper's communication socket

  struct sockaddr_un sock_address = {};
  sock_address.sun_family = AF_UNIX;
  std::snprintf(sock_address.sun_path, sizeof(sock_address.sun_path), "%s",
                SOCKET_PATH);

  int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);

  if (sock_fd == -1) {
    err("Couldn't create the socket");
  }

  unlink(SOCKET_PATH);
  if (bind(sock_fd, (struct sockaddr *) &sock_address, sizeof(sock_address)) ==
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

    if (select(max_fd + 1, &descriptors, nullptr, nullptr, nullptr) > 0) {
      if (FD_ISSET(pipe_read, &descriptors)) {
        ssize_t n = read(pipe_read, buffer, BUF_SIZE);
        if (n > 0) {
          std::string events_str(buffer, static_cast<unsigned long>(n));
          dapper.handle_events(events_str);
        }
      }

      if (FD_ISSET(sock_fd, &descriptors)) {
        int cli_fd = accept(sock_fd, nullptr, nullptr);
        if (cli_fd > 0) {
          ssize_t n = recv(cli_fd, buffer, BUF_SIZE, 0);
          if (n > 0) {
            std::string commands_str(buffer, static_cast<unsigned long>(n));
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
