// Multi-threaded, share-nothing TCP hello-world demo.
//
// N server threads each own a ringu::Reactor and a listening socket on
// 127.0.0.1:<base_port+i>. N client threads each own a ringu::Reactor and one
// connection to their server thread. Each round trip: the client sends "hello",
// the server replies "world". The demo reports the aggregate throughput.
//
// usage: demo_tcp_hello [threads=4] [messages_per_connection=20000] [base_port=24100] [sqpoll=1]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio> // for stderr
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <ringu/ringu.hpp>

namespace {

std::vector<std::byte> to_bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), reinterpret_cast<const std::byte*>(s.data()) + s.size()};
}

} // namespace

int main(int argc, char** argv) {
  const std::int32_t num_threads = argc > 1 ? static_cast<std::int32_t>(std::atoi(argv[1])) : 4;
  const std::int32_t messages = argc > 2 ? static_cast<std::int32_t>(std::atoi(argv[2])) : 20000;
  const std::uint16_t base_port = argc > 3 ? static_cast<std::uint16_t>(std::atoi(argv[3])) : 24100;
  const bool sqpoll = argc > 4 ? std::atoi(argv[4]) != 0 : true; // SQPOLL on by default
  const ringu::ReactorOptions reactor_opts{.entries = 1024, .sqpoll = sqpoll};

  std::atomic<std::int32_t> ready{0};
  std::vector<std::thread> servers;
  std::vector<std::thread> clients;
  std::vector<std::int64_t> client_msgs(static_cast<std::size_t>(num_threads), 0);
  std::vector<double> client_seconds(static_cast<std::size_t>(num_threads), 0.0);

  // server threads: one Reactor + one listening socket each, nothing shared
  for (std::int32_t i = 0; i < num_threads; ++i) {
    servers.emplace_back([&, i] {
      auto reactor_result = ringu::Reactor::Create(reactor_opts);
      if (!reactor_result.has_value()) {
        fmt::print(stderr, "server {}: reactor: {}\n", i, reactor_result.error().message);
        std::exit(1);
      }
      auto reactor = std::move(reactor_result).value();
      auto sock = ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Stream,
                                       ringu::SocketProtocol::Tcp, ringu::SocketFlag::CloExec);
      if (!sock.has_value()) {
        fmt::print(stderr, "server {}: socket: {}\n", i, std::get<ringu::IoError>(sock.error()).message());
        std::exit(1);
      }
      const std::int32_t lfd = sock->value();
      const std::int32_t one = 1;
      ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
      // bind through the reactor (inline Bind operation)
      bool bound = false;
      reactor->submit(ringu::bind(ringu::FileDescriptor{lfd},
                                  ringu::Ipv4Endpoint::loopback(static_cast<std::uint16_t>(base_port + i))),
                      [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t) {
                        if (r.has_value()) {
                          bound = true;
                        } else {
                          fmt::print(stderr, "server {}: bind: {}\n", i, std::get<ringu::IoError>(r.error()).message());
                        }
                      });
      reactor->process(ringu::Reactor::ProcessMode::Batch);
      if (!bound) {
        fmt::print(stderr, "server {}: bind failed\n", i);
        std::exit(1);
      }
      // listen through the reactor (inline Listen operation)
      bool listened = false;
      reactor->submit(ringu::listen(ringu::FileDescriptor{lfd}, 128),
                      [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t) {
                        if (r.has_value())
                          listened = true;
                      });
      reactor->process(ringu::Reactor::ProcessMode::Batch);
      if (!listened) {
        fmt::print(stderr, "server {}: listen failed\n", i);
        std::exit(1);
      }
      ready.fetch_add(1);

      reactor->submit(ringu::accept(ringu::FileDescriptor{lfd}), std::vector<std::byte>(sizeof(sockaddr_storage)),
                      [&](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t) {
                        if (!r.has_value()) {
                          reactor->request_stop();
                          return;
                        }
                        // per-connection state, self-owned so the recursive callbacks keep it alive
                        struct Conn {
                          ringu::Reactor& reactor;
                          std::int32_t cfd;
                          std::shared_ptr<std::vector<std::byte>> rbuf;
                          std::shared_ptr<std::vector<std::byte>> sbuf;
                          std::function<void()> do_recv;
                        };
                        auto conn =
                            std::make_shared<Conn>(Conn{*reactor,
                                                        r->value(),
                                                        std::make_shared<std::vector<std::byte>>(16),
                                                        std::make_shared<std::vector<std::byte>>(to_bytes("world")),
                                                        {}});
                        conn->do_recv = [conn] {
                          conn->reactor.submit(
                              ringu::recv(ringu::FileDescriptor{conn->cfd}), std::move(*conn->rbuf),
                              [conn](ringu::Result<std::vector<std::byte>> rr, ringu::OperationId, std::int32_t res) {
                                if (!rr.has_value() || res <= 0) {
                                  conn->reactor.request_stop(); // client went away
                                  return;
                                }
                                *conn->rbuf = std::move(*rr);
                                conn->rbuf->resize(16);
                                conn->reactor.submit(
                                    ringu::send(ringu::FileDescriptor{conn->cfd}), std::move(*conn->sbuf),
                                    [conn](ringu::Result<std::vector<std::byte>> sr, ringu::OperationId, std::int32_t) {
                                      if (!sr.has_value()) {
                                        conn->reactor.request_stop();
                                        return;
                                      }
                                      *conn->sbuf = std::move(*sr);
                                      conn->do_recv();
                                    });
                              });
                        };
                        conn->do_recv();
                      });
      reactor->process(ringu::Reactor::ProcessMode::Forever);
      ::close(lfd);
    });
  }

  // client threads: one Reactor + one connection each, nothing shared
  for (std::int32_t i = 0; i < num_threads; ++i) {
    clients.emplace_back([&, i] {
      while (ready.load() < num_threads) {
        std::this_thread::yield();
      }
      auto reactor_result = ringu::Reactor::Create(reactor_opts);
      if (!reactor_result.has_value()) {
        fmt::print(stderr, "client {}: reactor: {}\n", i, reactor_result.error().message);
        std::exit(1);
      }
      auto reactor = std::move(reactor_result).value();
      auto sock =
          ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Stream, ringu::SocketProtocol::Tcp);
      if (!sock.has_value()) {
        fmt::print(stderr, "client {}: socket: {}\n", i, std::get<ringu::IoError>(sock.error()).message());
        std::exit(1);
      }
      const std::int32_t fd = sock->value();
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = htons(static_cast<std::uint16_t>(base_port + i));
      // connect through the reactor with the Connect operation
      const auto endpoint = ringu::Ipv4Endpoint::loopback(static_cast<std::uint16_t>(::ntohs(addr.sin_port)));
      bool connected = false;
      for (std::int32_t tries = 0; tries < 10000 && !connected; ++tries) {
        reactor->submit(ringu::connect(ringu::FileDescriptor{fd}, endpoint),
                        [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t) {
                          if (r.has_value())
                            connected = true;
                        });
        reactor->process(ringu::Reactor::ProcessMode::Batch);
        if (!connected) {
          std::this_thread::yield();
        }
      }
      if (!connected) {
        fmt::print(stderr, "client {}: connect failed\n", i);
        std::exit(1);
      }
      auto sbuf = std::make_shared<std::vector<std::byte>>(to_bytes("hello"));
      auto rbuf = std::make_shared<std::vector<std::byte>>(16);
      std::int64_t count = 0;
      const auto start = std::chrono::steady_clock::now();
      std::function<void()> do_send;
      std::function<void()> do_recv;
      do_recv = [&] {
        reactor->submit(ringu::recv(ringu::FileDescriptor{fd}), std::move(*rbuf),
                        [&](ringu::Result<std::vector<std::byte>> rr, ringu::OperationId, std::int32_t res) {
                          if (!rr.has_value() || res <= 0) {
                            reactor->request_stop();
                            return;
                          }
                          *rbuf = std::move(*rr);
                          rbuf->resize(16);
                          count += 1;
                          if (count < messages) {
                            do_send();
                          } else {
                            reactor->request_stop();
                          }
                        });
      };
      do_send = [&] {
        reactor->submit(ringu::send(ringu::FileDescriptor{fd}), std::move(*sbuf),
                        [&](ringu::Result<std::vector<std::byte>> sr, ringu::OperationId, std::int32_t res) {
                          if (!sr.has_value() || res <= 0) {
                            reactor->request_stop();
                            return;
                          }
                          *sbuf = std::move(*sr);
                          do_recv();
                        });
      };
      do_send();
      reactor->process(ringu::Reactor::ProcessMode::Forever);
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
      client_msgs[static_cast<std::size_t>(i)] = count;
      client_seconds[static_cast<std::size_t>(i)] = static_cast<double>(elapsed.count()) / 1e9;
      ::close(fd);
    });
  }

  for (auto& t : clients) {
    t.join();
  }
  for (auto& t : servers) {
    t.join();
  }

  std::int64_t total_msgs = 0;
  double wall = 0.0;
  for (std::int32_t i = 0; i < num_threads; ++i) {
    total_msgs += client_msgs[static_cast<std::size_t>(i)];
    if (client_seconds[static_cast<std::size_t>(i)] > wall) {
      wall = client_seconds[static_cast<std::size_t>(i)];
    }
    fmt::print("  thread {}: {} round trips in {:.3f} s ({:.0f} rt/s, avg latency {:.2f} us)\n", i,
               client_msgs[static_cast<std::size_t>(i)], client_seconds[static_cast<std::size_t>(i)],
               static_cast<double>(client_msgs[static_cast<std::size_t>(i)]) /
                   client_seconds[static_cast<std::size_t>(i)],
               client_seconds[static_cast<std::size_t>(i)] * 1e6 /
                   static_cast<double>(client_msgs[static_cast<std::size_t>(i)]));
  }
  const double rt_per_s = wall > 0.0 ? static_cast<double>(total_msgs) / wall : 0.0;
  const double bytes_per_rt = 10.0; // "hello" + "world"
  fmt::print("ringu TCP hello-world: {} threads x {} messages (sqpoll {})\n", num_threads, messages,
             sqpoll ? "on" : "off");
  fmt::print("  total round trips: {} in {:.3f} s\n", total_msgs, wall);
  fmt::print("  throughput: {:.0f} round trips/s ({:.2f} MiB/s payload)\n", rt_per_s,
             rt_per_s * bytes_per_rt / (1024.0 * 1024.0));
  fmt::print("  avg round-trip latency: {:.2f} us\n", wall * 1e6 / static_cast<double>(total_msgs));
  return 0;
}
