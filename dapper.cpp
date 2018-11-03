#include <cstdio>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char *SOCKET_PATH = "/tmp/dapper.socket";

void err(std::string msg) {
  std::cerr << msg << std::endl;
  std::exit(1);
}

int main() {
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

  bool running = true;
  int max_fd = sock_fd;
  char msg[1024];

  while (running) {
    fd_set descriptors;
    FD_ZERO(&descriptors);
    FD_SET(sock_fd, &descriptors);

    if (select(max_fd + 1, &descriptors, NULL, NULL, NULL) > 0) {
      if (FD_ISSET(sock_fd, &descriptors)) {
        int cli_fd = accept(sock_fd, NULL, 0);
        if (cli_fd > 0) {
          int n = recv(cli_fd, msg, sizeof(msg), 0);
          if (n > 0) {
            msg[n] = '\0';

            std::cout << std::string(msg) << std::endl;
          }
        }
      }
    }
  }
}