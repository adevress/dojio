// UDP ping-pong demo: two UDP sockets on 127.0.0.1 exchange datagrams through a
// single ringu::RingEngine. Each datagram goes client -> server -> client; the demo
// reports the round-trip latency distribution.
//
// usage: demo_udp_pingpong [iterations=10000] [payload_size=64] [sqpoll=1]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio> // for stderr
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include <ringu/ringu.hpp>

namespace {

std::int32_t make_udp_socket(ringu::RingEngine& engine, sockaddr_in& bound_addr) {
  auto sock = ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Dgram, ringu::SocketProtocol::Udp);
  if (!sock.has_value()) {
    fmt::print(stderr, "socket: {}\n", std::get<ringu::IoError>(sock.error()).message());
    std::exit(1);
  }
  const std::int32_t fd = sock->value();
  if (fd < 0) {
    std::perror("socket");
    std::exit(1);
  }
  // bind through the RingEngine (inline Bind operation) to an ephemeral loopback port
  bool bound = false;
  engine.submit(ringu::bind(ringu::FileDescriptor{fd}, ringu::Ipv4Endpoint::loopback(0)),
                [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t) {
                  if (r.has_value()) {
                    bound = true;
                  } else {
                    fmt::print(stderr, "bind: {}\n", std::get<ringu::IoError>(r.error()).message());
                  }
                });
  engine.process(ringu::RingEngine::ProcessMode::Batch);
  if (!bound) {
    std::exit(1);
  }
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    std::perror("getsockname");
    std::exit(1);
  }
  bound_addr = addr;
  return fd;
}

} // namespace

int main(int argc, char** argv) { // main() signature is the C runtime ABI
  const std::int32_t iterations = argc > 1 ? static_cast<std::int32_t>(std::atoi(argv[1])) : 10000;
  const std::uint64_t payload_size = argc > 2 ? static_cast<std::uint64_t>(std::atoi(argv[2])) : 64;
  const bool sqpoll = argc > 3 ? std::atoi(argv[3]) != 0 : true; // SQPOLL on by default

  auto ring_engine_result = ringu::RingEngine::create(ringu::RingEngineOptions{.entries = 256, .sqpoll = sqpoll});
  if (!ring_engine_result.has_value()) {
    fmt::print(stderr, "ring engine: {}\n", ring_engine_result.error().message);
    return 1;
  }
  auto engine = std::move(ring_engine_result).value();

  sockaddr_in addr_a{};
  sockaddr_in addr_b{};
  const std::int32_t a = make_udp_socket(*engine, addr_a);
  const std::int32_t b = make_udp_socket(*engine, addr_b);
  // connect both sockets so the Send/Recv operations can be used
  if (::connect(a, reinterpret_cast<const sockaddr*>(&addr_b), sizeof(addr_b)) != 0 ||
      ::connect(b, reinterpret_cast<const sockaddr*>(&addr_a), sizeof(addr_a)) != 0) {
    std::perror("connect");
    return 1;
  }

  std::vector<std::byte> payload(static_cast<std::size_t>(payload_size));
  std::optional<std::vector<std::byte>> send_a_buf = payload; // a -> b
  std::optional<std::vector<std::byte>> recv_a_buf;           // b -> a
  std::optional<std::vector<std::byte>> send_b_buf = payload; // b -> a
  std::optional<std::vector<std::byte>> recv_b_buf;           // a -> b
  recv_a_buf.emplace(static_cast<std::size_t>(payload_size));
  recv_b_buf.emplace(static_cast<std::size_t>(payload_size));

  std::vector<std::int64_t> latencies_ns;
  latencies_ns.reserve(static_cast<std::size_t>(iterations));
  std::int32_t round_trips = 0;
  ringu::OperationId recv_b_id{};
  std::chrono::steady_clock::time_point t_send_a;

  std::function<void()> submit_recv_a;
  std::function<void()> submit_send_a;
  std::function<void()> submit_recv_b;
  std::function<void()> submit_send_b;

  submit_recv_b = [&] {
    recv_b_id = engine->submit(ringu::recv(ringu::FileDescriptor{b}), std::move(*recv_b_buf),
                               [&](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t res) {
                                 if (!r.has_value() || res <= 0) {
                                   engine->request_stop();
                                   return;
                                 }
                                 recv_b_buf = std::move(*r);
                                 recv_b_buf->resize(static_cast<std::size_t>(payload_size));
                                 submit_send_b();
                               });
  };

  submit_send_b = [&] {
    engine->submit(ringu::send(ringu::FileDescriptor{b}), std::move(*send_b_buf),
                   [&](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t res) {
                     if (!r.has_value() || res <= 0) {
                       engine->request_stop();
                       return;
                     }
                     send_b_buf = std::move(*r);
                     submit_recv_b(); // arm the next receive on b
                   });
  };

  submit_send_a = [&] {
    t_send_a = std::chrono::steady_clock::now();
    const std::uint64_t seq = static_cast<std::uint64_t>(round_trips);
    std::memcpy(send_a_buf->data(), &seq, sizeof(seq));
    engine->submit(ringu::send(ringu::FileDescriptor{a}), std::move(*send_a_buf),
                   [&](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t res) {
                     if (!r.has_value() || res <= 0) {
                       engine->request_stop();
                       return;
                     }
                     send_a_buf = std::move(*r);
                   });
  };

  submit_recv_a = [&] {
    engine->submit(ringu::recv(ringu::FileDescriptor{a}), std::move(*recv_a_buf),
                   [&](ringu::Result<std::vector<std::byte>> r, ringu::OperationId, std::int32_t res) {
                     if (!r.has_value() || res <= 0) {
                       engine->request_stop();
                       return;
                     }
                     recv_a_buf = std::move(*r);
                     const auto now = std::chrono::steady_clock::now();
                     latencies_ns.push_back(
                         std::chrono::duration_cast<std::chrono::nanoseconds>(now - t_send_a).count());
                     round_trips += 1;
                     if (round_trips < iterations) {
                       recv_a_buf->resize(static_cast<std::size_t>(payload_size));
                       submit_recv_a(); // arm the next receive on a
                       submit_send_a();
                     } else {
                       engine->cancel(recv_b_id); // nothing will arrive on b anymore
                       engine->request_stop();
                     }
                   });
  };

  submit_recv_b();
  submit_recv_a();
  submit_send_a();
  const auto t_start = std::chrono::steady_clock::now();
  engine->process(ringu::RingEngine::ProcessMode::Forever);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_start).count();

  ::close(a);
  ::close(b);

  if (latencies_ns.empty()) {
    fmt::print(stderr, "no round trip completed\n");
    return 1;
  }
  std::sort(latencies_ns.begin(), latencies_ns.end());
  const std::int64_t min = latencies_ns.front();
  const std::int64_t max = latencies_ns.back();
  const std::uint64_t lat_count = static_cast<std::uint64_t>(latencies_ns.size());
  const std::int64_t p50 = latencies_ns[static_cast<std::size_t>(lat_count / 2)];
  const std::int64_t p99 = latencies_ns[static_cast<std::size_t>((lat_count * 99) / 100)];
  const double avg = static_cast<double>(std::accumulate(latencies_ns.begin(), latencies_ns.end(), std::int64_t{0})) /
                     static_cast<double>(lat_count);

  fmt::print("ringu UDP ping-pong: {} round trips, payload {} bytes (sqpoll {})\n", round_trips, payload_size,
             sqpoll ? "on" : "off");
  fmt::print("  round-trip latency (us): min {:.3f}  avg {:.3f}  p50 {:.3f}  p99 {:.3f}  max {:.3f}\n",
             static_cast<double>(min) / 1e3, avg / 1e3, static_cast<double>(p50) / 1e3, static_cast<double>(p99) / 1e3,
             static_cast<double>(max) / 1e3);
  const double wall_s = static_cast<double>(elapsed) / 1e9;
  fmt::print("  wall time: {:.3f} s, throughput: {:.0f} round trips/s ({:.2f} MiB/s payload)\n", wall_s,
             static_cast<double>(round_trips) / wall_s,
             static_cast<double>(round_trips) / wall_s * (2.0 * static_cast<double>(payload_size)) / (1024.0 * 1024.0));
  return 0;
}
