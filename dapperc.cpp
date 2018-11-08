// Sockets code largely stolen from bspwm

#include <cstdio>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char *SOCKET_PATH = "/tmp/dapper.socket";

void err(const std::string& msg) {
  std::cerr << msg << std::endl;
  std::exit(1);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    err("No arguments given");
  }

  struct sockaddr_un sock_address = {};
  sock_address.sun_family = AF_UNIX;

  int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd == -1) {
    err("Failed to create the dapper socket");
  }

  std::snprintf(sock_address.sun_path, sizeof(sock_address.sun_path), "%s",
                SOCKET_PATH);

  if (connect(sock_fd, (struct sockaddr *)&sock_address,
              sizeof(sock_address)) == -1) {
    err("Failed to connect to the dapper socket");
  }

  if (send(sock_fd, argv[1], strlen(argv[1]), 0) == -1) {
    err("Failed to send the data");
  }

  close(sock_fd);
}
