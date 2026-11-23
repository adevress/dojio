// UDP ping-pong demo: two UDP sockets on 127.0.0.1 exchange datagrams through a
// single dojio::RingEngine. Each datagram goes client -> server -> client; the demo
// reports the round-trip latency distribution.
//
// usage: demo_udp_pingpong [iterations=10000] [payload_size=64]

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
#include <memory_resource>
#include <numeric>
#include <string>
#include <system_error>
#include <vector>

#include <dojio/dojio.hpp>

namespace {

using Bytes = std::vector<std::byte>;
using RecvResult = dojio::RecvResult<Bytes>;
using SendResult = dojio::SendResult<Bytes>;

/// Data shared by every completion callback: the counters (and the vectors they fill).
struct Counters {
  std::int32_t round_trips = 0;
  std::int32_t failed = 0;
  std::vector<std::int64_t> latencies_ns;
  std::chrono::steady_clock::time_point send_a_time{};
};

/// Creates and binds a UDP socket on an ephemeral loopback port through the engine.
/// Fills bound_addr with the port the kernel assigned.
std::int32_t make_udp_socket(dojio::RingEngine& engine, sockaddr_in& bound_addr) {
  const std::int32_t fd =
      dojio::result_or_fail(
          dojio::create_socket(dojio::AddressFamily::Inet, dojio::SocketType::Dgram, dojio::SocketProtocol::Udp),
          "socket")
          .value();
  // bind through the RingEngine (IORING_OP_BIND on a recent kernel, inline bind(2) otherwise) to an
  // ephemeral loopback port
  engine.submit(
      dojio::bind(dojio::FileDescriptor{fd}, dojio::Ipv4Endpoint::loopback(0)),
      [](dojio::RingEngine&, dojio::Result<dojio::BindResult> r) { dojio::result_or_fail(std::move(r), "bind"); });
  engine.process(dojio::RingEngine::ProcessMode::Batch);
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    fmt::print(stderr, "getsockname: {}\n", std::error_code(errno, std::system_category()).message());
    std::exit(1);
  }
  bound_addr = addr;
  return fd;
}

// The ping-pong is a cycle of operations, each handing its buffer to the next step
// through the completion result. fd is passed by value (a descriptor is four bytes)
// and the engine is the first callback parameter, so nothing else needs to be carried.

void echo_b_recv(dojio::RingEngine& ring, dojio::FileDescriptor fd, std::uint64_t payload_size,
                 dojio::SharedValue<Counters> counters, Bytes recv_buf, Bytes send_buf);

/// b -> a: sends the reply, then arms the next receive on b.
void echo_b_send(dojio::RingEngine& ring, dojio::FileDescriptor fd, std::uint64_t payload_size,
                 dojio::SharedValue<Counters> counters, Bytes send_buf, Bytes recv_buf) {
  ring.submit(dojio::send(fd, std::move(send_buf)), [fd, payload_size, counters, recv_buf = std::move(recv_buf)](
                                                        dojio::RingEngine& r, dojio::Result<SendResult> res) mutable {
    if (!res.has_value() || res->size != payload_size) {
      counters->failed += 1;
      r.request_stop();
      return;
    }
    echo_b_recv(r, fd, payload_size, std::move(counters), std::move(recv_buf), std::move(res->buffer));
  });
}

/// a -> b: receives the request, then echoes it back on b.
void echo_b_recv(dojio::RingEngine& ring, dojio::FileDescriptor fd, std::uint64_t payload_size,
                 dojio::SharedValue<Counters> counters, Bytes recv_buf, Bytes send_buf) {
  ring.submit(dojio::recv(fd, std::move(recv_buf)), [fd, payload_size, counters, send_buf = std::move(send_buf)](
                                                        dojio::RingEngine& r, dojio::Result<RecvResult> res) mutable {
    if (!res.has_value() || res->size != payload_size) {
      counters->failed += 1;
      r.request_stop();
      return;
    }
    echo_b_send(r, fd, payload_size, std::move(counters), std::move(send_buf), std::move(res->buffer));
  });
}

void ping_a_send(dojio::RingEngine& ring, dojio::FileDescriptor fd, std::uint64_t payload_size, std::int32_t iterations,
                 dojio::SharedValue<Counters> counters, Bytes send_buf, Bytes recv_buf);

/// b -> a: receives the reply, records the round-trip latency, then arms the next send.
void ping_a_recv(dojio::RingEngine& ring, dojio::FileDescriptor fd, std::uint64_t payload_size, std::int32_t iterations,
                 dojio::SharedValue<Counters> counters, Bytes recv_buf, Bytes send_buf) {
  ring.submit(dojio::recv(fd, std::move(recv_buf)), [fd, payload_size, iterations, counters,
                                                     send_buf = std::move(send_buf)](
                                                        dojio::RingEngine& r, dojio::Result<RecvResult> res) mutable {
    if (!res.has_value() || res->size != payload_size) {
      counters->failed += 1;
      r.request_stop();
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    counters->latencies_ns.push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - counters->send_a_time).count());
    counters->round_trips += 1;
    if (counters->round_trips < iterations) {
      ping_a_send(r, fd, payload_size, iterations, std::move(counters), std::move(send_buf), std::move(res->buffer));
    } else {
      r.request_stop();
    }
  });
}

/// a -> b: stamps the send time, sends the sequence number, then arms the matching receive.
void ping_a_send(dojio::RingEngine& ring, dojio::FileDescriptor fd, std::uint64_t payload_size, std::int32_t iterations,
                 dojio::SharedValue<Counters> counters, Bytes send_buf, Bytes recv_buf) {
  counters->send_a_time = std::chrono::steady_clock::now();
  const std::uint64_t seq = static_cast<std::uint64_t>(counters->round_trips);
  std::memcpy(send_buf.data(), &seq, sizeof(seq));
  ring.submit(
      dojio::send(fd, std::move(send_buf)), [fd, payload_size, iterations, counters, recv_buf = std::move(recv_buf)](
                                                dojio::RingEngine& r, dojio::Result<SendResult> res) mutable {
        if (!res.has_value() || res->size != payload_size) {
          counters->failed += 1;
          r.request_stop();
          return;
        }
        ping_a_recv(r, fd, payload_size, iterations, std::move(counters), std::move(recv_buf), std::move(res->buffer));
      });
}

} // namespace

int main(int argc, char** argv) { // main() signature is the C runtime ABI
  const std::int32_t iterations = argc > 1 ? static_cast<std::int32_t>(std::atoi(argv[1])) : 10000;
  const std::uint64_t payload_size = argc > 2 ? static_cast<std::uint64_t>(std::atoi(argv[2])) : 64;
  const bool sqpoll = false;

  auto engine = dojio::result_or_fail(
      dojio::RingEngine::create(dojio::RingEngineOptions{.entries = 256, .sqpoll = sqpoll}), "ring engine");

  sockaddr_in addr_a{};
  sockaddr_in addr_b{};
  const std::int32_t a = make_udp_socket(*engine, addr_a);
  const std::int32_t b = make_udp_socket(*engine, addr_b);
  // connect both sockets so the Send/Recv operations can be used
  if (::connect(a, reinterpret_cast<const sockaddr*>(&addr_b), sizeof(addr_b)) != 0 ||
      ::connect(b, reinterpret_cast<const sockaddr*>(&addr_a), sizeof(addr_a)) != 0) {
    fmt::print(stderr, "connect: {}\n", std::error_code(errno, std::system_category()).message());
    return 1;
  }

  dojio::SharedValue<Counters> counters = dojio::SharedValue<Counters>::make(std::pmr::new_delete_resource());
  counters->latencies_ns.reserve(static_cast<std::size_t>(iterations));

  ping_a_send(*engine, dojio::FileDescriptor{a}, payload_size, iterations, counters,
              Bytes(static_cast<std::size_t>(payload_size)), Bytes(static_cast<std::size_t>(payload_size)));
  echo_b_recv(*engine, dojio::FileDescriptor{b}, payload_size, counters, Bytes(static_cast<std::size_t>(payload_size)),
              Bytes(static_cast<std::size_t>(payload_size)));

  const auto t_start = std::chrono::steady_clock::now();
  engine->process(dojio::RingEngine::ProcessMode::Forever);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_start).count();

  ::close(a);
  ::close(b);

  std::vector<std::int64_t>& latencies_ns = counters->latencies_ns;
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

  fmt::print("dojio UDP ping-pong: {} round trips, payload {} bytes (sqpoll {})\n", counters->round_trips, payload_size,
             sqpoll ? "on" : "off");
  fmt::print("  round-trip latency (us): min {:.3f}  avg {:.3f}  p50 {:.3f}  p99 {:.3f}  max {:.3f}\n",
             static_cast<double>(min) / 1e3, avg / 1e3, static_cast<double>(p50) / 1e3, static_cast<double>(p99) / 1e3,
             static_cast<double>(max) / 1e3);
  const double wall_s = static_cast<double>(elapsed) / 1e9;
  fmt::print("  wall time: {:.3f} s, throughput: {:.0f} round trips/s ({:.2f} MiB/s payload)\n", wall_s,
             static_cast<double>(counters->round_trips) / wall_s,
             static_cast<double>(counters->round_trips) / wall_s * (2.0 * static_cast<double>(payload_size)) /
                 (1024.0 * 1024.0));
  return 0;
}
