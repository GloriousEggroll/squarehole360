// squarehole-lobby: a self-hostable lobby and relay for internet System Link.
//
// Players join a room and appear to each other as one LAN: the server assigns
// virtual addresses, distributes the member list, fans System Link broadcasts
// out to the room and relays game traffic between players whose direct
// connection does not work. See rex/system/xam/lobby_protocol.h.
//
// Usage: squarehole-lobby [--port 36360] [--bind 0.0.0.0] [--max-rooms 256]
// Needs one UDP port reachable from the internet (forward it on the router).

#include <rex/system/xam/lobby_protocol.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
#endif

namespace {
using namespace rex::lobby;
using Clock = std::chrono::steady_clock;
constexpr auto kMemberTimeout = std::chrono::seconds(15);

struct Endpoint {
  uint32_t ip = 0;  // host order
  uint16_t port = 0;
  bool operator<(const Endpoint& o) const { return ip != o.ip ? ip < o.ip : port < o.port; }
  bool operator==(const Endpoint&) const = default;
};

std::string Text(const Endpoint& e) {
  return std::to_string(e.ip >> 24) + "." + std::to_string((e.ip >> 16) & 255) + "." +
         std::to_string((e.ip >> 8) & 255) + "." + std::to_string(e.ip & 255) + ":" +
         std::to_string(e.port);
}

struct ServerMember {
  Member info;
  Endpoint endpoint;
  std::string room;
  Clock::time_point last_seen;
};

struct Room {
  std::string password;
  std::vector<uint32_t> members;  // vips
};

class Server {
 public:
  Server(SocketHandle socket, size_t max_rooms) : socket_(socket), max_rooms_(max_rooms) {}

  void Run() {
    std::vector<uint8_t> buffer(2048);
    auto last_sweep = Clock::now();
    for (;;) {
      fd_set read;
      FD_ZERO(&read);
      FD_SET(socket_, &read);
      timeval timeout{1, 0};
      if (select(int(socket_) + 1, &read, nullptr, nullptr, &timeout) > 0) {
        sockaddr_in from{};
        socklen_t length = sizeof(from);
        const int size = recvfrom(socket_, reinterpret_cast<char*>(buffer.data()), int(buffer.size()),
                                  0, reinterpret_cast<sockaddr*>(&from), &length);
        if (size > 0)
          Handle({ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)}, buffer.data(), size_t(size));
      }
      if (Clock::now() - last_sweep > std::chrono::seconds(1)) {
        Sweep();
        last_sweep = Clock::now();
      }
    }
  }

 private:
  void Send(const Endpoint& to, const std::vector<uint8_t>& bytes) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(to.ip);
    address.sin_port = htons(to.port);
    sendto(socket_, reinterpret_cast<const char*>(bytes.data()), int(bytes.size()), 0,
           reinterpret_cast<sockaddr*>(&address), sizeof(address));
  }

  std::vector<Member> RoomMembers(const std::string& room) {
    std::vector<Member> list;
    for (const uint32_t vip : rooms_[room].members) list.push_back(members_[vip].info);
    return list;
  }

  void BroadcastMembers(const std::string& room) {
    const auto list = RoomMembers(room);
    const auto bytes = Writer(Type::kMembers).Members(list).bytes();
    for (const uint32_t vip : rooms_[room].members) Send(members_[vip].endpoint, bytes);
  }

  // The authenticated member a packet comes from, if any.
  ServerMember* Sender(const Endpoint& from, uint32_t vip, uint64_t token) {
    const auto it = members_.find(vip);
    if (it == members_.end() || it->second.info.token != token || !(it->second.endpoint == from))
      return nullptr;
    it->second.last_seen = Clock::now();
    return &it->second;
  }

  void Handle(const Endpoint& from, const uint8_t* data, size_t size) {
    Reader reader(data, size);
    const auto type = reader.Header();
    if (!type) return;
    switch (*type) {
      case Type::kHello: Hello(from, reader); break;
      case Type::kKeepAlive: {
        uint32_t vip;
        uint64_t token;
        // Echoed so clients know the server is still there.
        if (reader.U32(vip) && reader.U64(token) && Sender(from, vip, token))
          Send(from, Writer(Type::kKeepAlive).U32(vip).U64(0).bytes());
        break;
      }
      case Type::kData: {
        uint32_t src_vip, dst_vip;
        uint64_t token;
        uint16_t src_port, dst_port;
        if (!reader.U32(src_vip) || !reader.U64(token) || !reader.U32(dst_vip) ||
            !reader.U16(src_port) || !reader.U16(dst_port) || reader.remaining() > kMaxPayload)
          return;
        auto* sender = Sender(from, src_vip, token);
        if (!sender) return;
        // Relayed: token zero marks it as vouched for by the server.
        const auto bytes = Writer(Type::kData)
                               .U32(src_vip).U64(0).U32(dst_vip).U16(src_port).U16(dst_port)
                               .Bytes(reader.rest(), reader.remaining()).bytes();
        for (const uint32_t vip : rooms_[sender->room].members) {
          if (vip == src_vip || (dst_vip != kBroadcast && vip != dst_vip)) continue;
          Send(members_[vip].endpoint, bytes);
        }
        break;
      }
      case Type::kBye: {
        uint32_t vip;
        uint64_t token;
        if (reader.U32(vip) && reader.U64(token) && Sender(from, vip, token)) Remove(vip, "left");
        break;
      }
      default: break;
    }
  }

  void Hello(const Endpoint& from, Reader& reader) {
    uint64_t xuid;
    uint32_t title, nonce;
    std::string gamertag, room, password;
    if (!reader.U64(xuid) || !reader.Text(gamertag, kNameBytes) || !reader.U32(title) ||
        !reader.Text(room, kRoomBytes) || !reader.Text(password, kPasswordBytes) || !reader.U32(nonce))
      return;
    auto reject = [&](const char* reason) {
      Send(from, Writer(Type::kReject).U32(nonce).Text(reason, kReasonBytes).bytes());
    };
    if (room.empty()) room = "public";
    room += "#" + std::to_string(title);  // Rooms never mix games.
    // A repeated HELLO (lost WELCOME) from the same endpoint gets the same seat.
    for (auto& [vip, member] : members_) {
      if (member.endpoint == from && member.info.xuid == xuid && member.room == room) {
        member.last_seen = Clock::now();
        Welcome(from, nonce, member);
        return;
      }
    }
    if (!rooms_.contains(room) && rooms_.size() >= max_rooms_) return reject("The server is full.");
    auto& target = rooms_[room];
    if (target.members.empty()) target.password = password;
    else if (target.password != password) return reject("Wrong room password.");
    if (target.members.size() >= kMaxMembers) return reject("The room is full.");
    for (const uint32_t vip : target.members)
      if (members_[vip].info.xuid == xuid)
        return reject("A player with the same profile ID is already in the room.");
    const uint32_t vip = AllocateVip();
    if (!vip) return reject("The server is full.");
    ServerMember member;
    member.info.vip = vip;
    member.info.xuid = xuid;
    member.info.gamertag = gamertag.substr(0, kNameBytes - 1);
    member.info.public_ip = from.ip;
    member.info.public_port = from.port;
    member.info.token = random_();
    member.endpoint = from;
    member.room = room;
    member.last_seen = Clock::now();
    members_[vip] = member;
    target.members.push_back(vip);
    std::printf("join  %-16s %s room %s as 10.69.%u.%u\n", member.info.gamertag.c_str(),
                Text(from).c_str(), room.c_str(), (vip >> 8) & 255, vip & 255);
    std::fflush(stdout);
    Welcome(from, nonce, members_[vip]);
    BroadcastMembers(room);
  }

  void Welcome(const Endpoint& to, uint32_t nonce, const ServerMember& member) {
    Send(to, Writer(Type::kWelcome).U32(nonce).U32(member.info.vip).U64(member.info.token)
                 .Members(RoomMembers(member.room)).bytes());
  }

  uint32_t AllocateVip() {
    for (uint32_t i = 0; i < 0xFFFE; ++i) {
      next_host_ = next_host_ % 0xFFFE + 1;  // 10.69.0.1 .. 10.69.255.254
      const uint32_t vip = kVirtualNetwork | next_host_;
      if ((vip & 0xFF) != 0 && (vip & 0xFF) != 0xFF && !members_.contains(vip)) return vip;
    }
    return 0;
  }

  void Remove(uint32_t vip, const char* why) {
    const auto it = members_.find(vip);
    if (it == members_.end()) return;
    const std::string room = it->second.room;
    std::printf("leave %-16s %s (%s)\n", it->second.info.gamertag.c_str(),
                Text(it->second.endpoint).c_str(), why);
    std::fflush(stdout);
    members_.erase(it);
    auto& list = rooms_[room].members;
    list.erase(std::remove(list.begin(), list.end(), vip), list.end());
    if (list.empty()) rooms_.erase(room);
    else BroadcastMembers(room);
  }

  void Sweep() {
    std::vector<uint32_t> expired;
    for (const auto& [vip, member] : members_)
      if (Clock::now() - member.last_seen > kMemberTimeout) expired.push_back(vip);
    for (const uint32_t vip : expired) Remove(vip, "timed out");
  }

  SocketHandle socket_;
  size_t max_rooms_;
  std::unordered_map<uint32_t, ServerMember> members_;
  std::map<std::string, Room> rooms_;
  uint32_t next_host_ = 0;
  std::mt19937_64 random_{std::random_device{}()};
};
}  // namespace

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
  const SocketHandle handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1 ||
      bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    std::fprintf(stderr, "squarehole-lobby: cannot listen on %s:%u\n", bind_address.c_str(),
                 unsigned(port));
    return 1;
  }
  std::printf("squarehole-lobby listening on UDP %s:%u\n", bind_address.c_str(), unsigned(port));
  std::fflush(stdout);
  Server(handle, max_rooms).Run();
}
