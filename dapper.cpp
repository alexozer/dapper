// Sockets code largely stolen from bspwm

#include <cstdio>
#include <iostream>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *SOCKET_PATH = "/tmp/dapper.socket";

static volatile bool running = false;

#define MAX(A, B) ((A) > (B) ? (A) : (B))

void err(std::string msg) {
  std::cerr << msg << std::endl;
  std::exit(1);
}

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
    execvp("bspc", (char *const *)args);
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
  const size_t BUF_SIZE = 1024;
  char msg[BUF_SIZE];

  while (running) {
    fd_set descriptors;
    FD_ZERO(&descriptors);
    FD_SET(sock_fd, &descriptors);
    FD_SET(pipe_read, &descriptors);

    if (select(max_fd + 1, &descriptors, NULL, NULL, NULL) > 0) {
      if (FD_ISSET(pipe_read, &descriptors)) {
        int n = read(pipe_read, msg, BUF_SIZE);
        if (n > 0) {
          std::string message(msg, n);

          std::cout << message << std::endl;
        }
      }

      if (FD_ISSET(sock_fd, &descriptors)) {
        int cli_fd = accept(sock_fd, NULL, 0);
        if (cli_fd > 0) {
          int n = recv(cli_fd, msg, BUF_SIZE, 0);
          if (n > 0) {
            std::string message(msg, n);

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