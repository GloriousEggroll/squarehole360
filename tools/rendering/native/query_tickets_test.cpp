#include <rex/graphics/native_query_tickets.h>

#include <array>
#include <iostream>
#include <stdexcept>

using rex::graphics::NativeQueryIdentity;
using rex::graphics::NativeQueryTickets;
using Status = NativeQueryTickets::EndStatus;
namespace {
void Require(bool condition) {
  if (!condition) throw std::runtime_error("Native query ticket assertion failed");
}
template<class F> void Reject(F&& action) {
  bool rejected = false;
  try { action(); } catch (const std::runtime_error&) { rejected = true; }
  Require(rejected);
}
NativeQueryIdentity Id(uint64_t generation, uint32_t address = 0x4000) {
  return {address, generation, 27, true};
}
}

int main() {
  try {
    NativeQueryTickets tickets;
    uint32_t writes = 0;
    const auto publish = [&] { ++writes; };
    const auto a = Id(1);
    Reject([&] { tickets.Defer(a); });
    Reject([&] { tickets.Publish(a, publish); });
    Reject([&] { auto scope = tickets.BeginEndCallback(Id(0)); });
    Require(!tickets.current_end() && tickets.empty());
    {
      const auto scope = tickets.BeginEndCallback(a);
      Require(tickets.current_end() == a && tickets.end_status() == Status::Outstanding);
      Reject([&] { auto nested = tickets.BeginEndCallback(Id(2)); });
      for (const auto wrong : {Id(2), Id(1, 0x4040), NativeQueryIdentity{0x4000, 1, 28, true},
                               NativeQueryIdentity{0x4000, 1, 27, false}}) {
        Reject([&] { tickets.Defer(wrong); });
        Reject([&] { tickets.Publish(wrong, publish); });
      }
      Reject([&] { tickets.Publish(a, [] { throw std::runtime_error("write failed"); }); });
      Require(tickets.end_status() == Status::Outstanding);
      Require(tickets.Publish(a, publish) && writes == 1);
      Require(tickets.current_end() == a && tickets.end_status() == Status::Published);
      Reject([&] { tickets.Publish(a, publish); });
      Reject([&] { tickets.Defer(a); });
    }
    Require(!tickets.current_end() && tickets.empty());
    Reject([&] { tickets.Publish(a, publish); });

    // Deferred ownership survives both a normal callback return and unwinding.
    const auto b = Id(2), c = Id(3, 0x4080);
    try {
      const auto scope = tickets.BeginEndCallback(b);
      tickets.Defer(b);
      Require(tickets.current_end() == b && tickets.end_status() == Status::Deferred);
      Reject([&] { tickets.Defer(b); });
      throw std::runtime_error("provider failed after accepting query");
    } catch (const std::runtime_error&) {}
    Require(!tickets.current_end() && tickets.size() == 1);
    Reject([&] { tickets.Publish(b, [] { throw std::runtime_error("GPU completion failed"); }); });
    Require(tickets.size() == 1);
    {
      const auto scope = tickets.BeginEndCallback(c);
      // Completing an older ticket must not consume a different current end.
      Require(tickets.Publish(b, publish));
      Require(tickets.current_end() == c && tickets.end_status() == Status::Outstanding);
      tickets.Defer(c);
      Require(tickets.Publish(c, publish));
      Require(tickets.current_end() == c && tickets.end_status() == Status::Published);
      Reject([&] { tickets.Publish(c, publish); });
    }
    Require(writes == 3 && tickets.empty());

    // Reuse invalidates only writes, not ownership. Old completions still need
    // their exact generation; they cannot touch a newer generation's report.
    const auto old = Id(4), newer = Id(5), other = Id(6, 0x4100);
    for (const auto id : {old, other}) {
      const auto scope = tickets.BeginEndCallback(id);
      tickets.Defer(id);
    }
    tickets.SupersedeAddress(old.end_address);
    {
      const auto scope = tickets.BeginEndCallback(newer);
      tickets.Defer(newer);
      Require(!tickets.Publish(old, [] { throw std::runtime_error("stale write"); }));
      Require(tickets.current_end() == newer && tickets.end_status() == Status::Deferred);
      Require(tickets.Publish(other, publish));
      Require(tickets.Publish(newer, publish));
      Require(tickets.end_status() == Status::Published);
    }
    Require(writes == 5 && tickets.empty());
    Reject([&] { tickets.Publish(old, publish); });
    Reject([&] { tickets.Publish(newer, publish); });

    // Exhaustion fails before accepting another end; pressure may then retire
    // old exact tickets. No old ticket is overwritten or silently dropped.
    std::array<NativeQueryIdentity, NativeQueryTickets::kCapacity> full;
    for (size_t i = 0; i < full.size(); ++i) {
      full[i] = Id(100 + i, uint32_t(0x8000 + i * 64));
      const auto scope = tickets.BeginEndCallback(full[i]);
      tickets.Defer(full[i]);
    }
    Require(tickets.full() && tickets.size() == 128);
    const auto extra = Id(300, 0x10000);
    {
      const auto scope = tickets.BeginEndCallback(extra);
      Reject([&] { tickets.Defer(extra); });
      Require(tickets.end_status() == Status::Outstanding && tickets.size() == 128);
      Require(tickets.Publish(full[0], publish));
      tickets.Defer(extra);
      Require(tickets.full());
    }
    for (size_t i = 1; i < full.size(); ++i) tickets.Publish(full[i], publish);
    tickets.Publish(extra, publish);
    Require(tickets.empty() && writes == 134);

    // Shadow callbacks retain synchronous publication and cannot acquire a
    // deferred native ticket, even with otherwise matching identity fields.
    auto shadow = Id(400); shadow.native_authority = false;
    {
      const auto scope = tickets.BeginEndCallback(shadow);
      Reject([&] { tickets.Defer(shadow); });
      tickets.Publish(shadow, publish);
    }
    Require(writes == 135 && tickets.empty() && !tickets.current_end());
    const auto stats = tickets.statistics();
    Require(stats.deferred == 134 && stats.retired == 134 && stats.pending == 0);
    Require(stats.guest_written == 133 && stats.superseded == 1 && stats.peak == 128);
    std::cout << "Native query ticket identity, lifecycle, reuse, capacity and failure tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
