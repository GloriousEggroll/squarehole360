// gears3-lobby: a self-hostable lobby and relay for internet System Link.
//
// Players join a room and appear to each other as one LAN: the server assigns
// virtual addresses, distributes the member list, fans System Link broadcasts
// out to the room and relays game traffic between players whose direct
// connection does not work. See rex/system/xam/lobby_protocol.h.
//
// Usage: gears3-lobby [--port 36360] [--bind 0.0.0.0] [--max-rooms 256]
// Needs one UDP port reachable from the internet (forward it on the router).
// Games without a configured server also find it on the LAN (see
// lobby_protocol.h); the server itself is rex/system/xam/lobby_server.h.

#include <rex/system/xam/lobby_server.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace rex::lobby;

int main(int argc, char** argv) {
  uint16_t port = kDefaultPort;
  std::string bind_address = "0.0.0.0";
  size_t max_rooms = 256;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--port" && has_value) port = uint16_t(std::atoi(argv[++i]));
    else if (arg == "--bind" && has_value) bind_address = argv[++i];
    else if (arg == "--max-rooms" && has_value) max_rooms = size_t(std::atoi(argv[++i]));
    else {
      std::fprintf(stderr, "Usage: %s [--port %u] [--bind 0.0.0.0] [--max-rooms 256]\n", argv[0],
                   unsigned(kDefaultPort));
      return arg == "--help" || arg == "-h" ? 0 : 2;
    }
  }
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  const ServerSocket handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1 ||
      bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    std::fprintf(stderr, "gears3-lobby: cannot listen on %s:%u\n", bind_address.c_str(),
                 unsigned(port));
    return 1;
  }
  std::printf("gears3-lobby listening on UDP %s:%u\n", bind_address.c_str(), unsigned(port));
  std::fflush(stdout);
  Server(handle, max_rooms, [](const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
  }).Run();
}
