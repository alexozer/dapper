// Sockets code largely stolen from bspc

#include <cstdio>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char *SOCKET_PATH = "/tmp/dapper.socket";

void err(std::string msg) {
  std::cerr << msg << std::endl;
  std::exit(1);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    err("No arguments given");
  }

  struct sockaddr_un sock_address;

  sock_address.sun_family = AF_UNIX;

  int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd == -1) {
    err("Failed to create the socket");
  }

  std::snprintf(sock_address.sun_path, sizeof(sock_address.sun_path), "%s",
                SOCKET_PATH);

  if (connect(sock_fd, (struct sockaddr *)&sock_address,
              sizeof(sock_address)) == -1) {
    err("Failed to connect to the socket");
  }

  // Concat args into single string
  std::stringstream arg_buf;
  for (int arg = 1; arg < argc - 1; arg++) {
    arg_buf << argv[arg] << ' ';
  }
  arg_buf << argv[argc - 1];
  std::string arg_str = arg_buf.str();
  size_t msg_size = arg_str.size() * sizeof(arg_str[0]);

  if (send(sock_fd, arg_str.c_str(), msg_size, 0) == -1) {
    err("Failed to send the data");
  }

  // struct pollfd poll_fd = {sock_fd, POLLIN, 0};

  // struct pollfd fds[] = {
  //     {sock_fd, POLLIN, 0},
  //     {STDOUT_FILENO, POLLHUP, 0},
  // };

  // while (poll(fds, 2, -1) > 0) {
  //   if (fds[1].revents & (POLLERR | POLLHUP)) {
  //     break;
  //   }
  // }

  close(sock_fd);
}
