// Single-threaded TCP hello-world server with N multi-threaded clients.
//
// One server thread owns a ringu::RingEngine and a single listening socket on
// 127.0.0.1:<base_port>; it accepts all N client connections on that socket.
// N client threads each own a ringu::RingEngine and one connection to the server.
// Each round trip: the client sends "hello", the server replies "world". The
// demo reports the aggregate throughput.
//
// usage: demo_tcp_hello [clients=4] [messages_per_connection=20000] [base_port=24100]

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio> // for stderr
#include <cstdlib>
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
  const std::int32_t num_clients = argc > 1 ? static_cast<std::int32_t>(std::atoi(argv[1])) : 4;
  const std::int32_t messages = argc > 2 ? static_cast<std::int32_t>(std::atoi(argv[2])) : 20000;
  const std::uint16_t base_port = argc > 3 ? static_cast<std::uint16_t>(std::atoi(argv[3])) : 24100;
  const bool sqpoll = false;
  const ringu::RingEngineOptions engine_opts{.entries = 1024, .sqpoll = sqpoll};

  std::atomic<std::int32_t> ready{0};
  std::vector<std::thread> clients;
  std::vector<std::int64_t> client_msgs(static_cast<std::size_t>(num_clients), 0);
  std::vector<double> client_seconds(static_cast<std::size_t>(num_clients), 0.0);

  // single server thread: one RingEngine + one listening socket, accepts all N clients
  std::thread server([&] {
    auto ring_engine_result = ringu::RingEngine::create(engine_opts);
    if (!ring_engine_result.has_value()) {
      fmt::print(stderr, "server: ring engine: {}\n", ring_engine_result.error().message);
      std::exit(1);
    }
    auto engine = std::move(ring_engine_result).value();
    auto sock = ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Stream, ringu::SocketProtocol::Tcp,
                                     ringu::SocketFlag::CloExec);
    if (!sock.has_value()) {
      fmt::print(stderr, "server: socket: {}\n", std::get<ringu::IoError>(sock.error()).message());
      std::exit(1);
    }
    const std::int32_t lfd = sock->value();
    const std::int32_t one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // bind through the engine (inline Bind operation)
    bool bound = false;
    engine->submit(ringu::bind(ringu::FileDescriptor{lfd}, ringu::Ipv4Endpoint::loopback(base_port)),
                   [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t) {
                     if (r.has_value()) {
                       bound = true;
                     } else {
                       fmt::print(stderr, "server: bind: {}\n", std::get<ringu::IoError>(r.error()).message());
                     }
                   });
    engine->process(ringu::RingEngine::ProcessMode::Batch);
    if (!bound) {
      fmt::print(stderr, "server: bind failed\n");
      std::exit(1);
    }
    // listen through the engine (inline Listen operation)
    bool listened = false;
    engine->submit(ringu::listen(ringu::FileDescriptor{lfd}, 128),
                   [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t) {
                     if (r.has_value())
                       listened = true;
                   });
    engine->process(ringu::RingEngine::ProcessMode::Batch);
    if (!listened) {
      fmt::print(stderr, "server: listen failed\n");
      std::exit(1);
    }
    ready.fetch_add(1);

    // accept/connection state shared by the callbacks below; every callback runs
    // on this single server thread, so no synchronization is needed
    struct State {
      ringu::RingEngine& engine;
      std::int32_t lfd;
      std::int32_t num_clients;
      std::int32_t accepted = 0;
      std::int32_t active = 0;
    };
    auto state = std::make_shared<State>(State{*engine, lfd, num_clients, 0, 0});

    // per-connection state, self-owned so the recursive callbacks keep it alive
    struct Conn {
      std::shared_ptr<State> state;
      std::int32_t cfd;
      std::shared_ptr<std::vector<std::byte>> rbuf;
      std::shared_ptr<std::vector<std::byte>> sbuf;
      std::function<void()> do_recv;
    };

    // one outstanding accept at a time; re-armed until every client is connected
    std::function<void()> do_accept;
    do_accept = [state, &do_accept] {
      state->engine.submit(
          ringu::accept(ringu::FileDescriptor{state->lfd}), std::vector<std::byte>(sizeof(sockaddr_storage)),
          [state, &do_accept](ringu::Result<ringu::FileDescriptor> r, ringu::OperationId, std::int32_t) {
            if (!r.has_value()) {
              if (state->accepted < state->num_clients) {
                do_accept(); // transient accept error: keep waiting for clients
              }
              return;
            }
            state->accepted += 1;
            state->active += 1;
            auto conn = std::make_shared<Conn>(Conn{state,
                                                    r->value(),
                                                    std::make_shared<std::vector<std::byte>>(16),
                                                    std::make_shared<std::vector<std::byte>>(to_bytes("world")),
                                                    {}});
            conn->do_recv = [conn] {
              conn->state->engine.submit(
                  ringu::recv(ringu::FileDescriptor{conn->cfd}), std::move(*conn->rbuf),
                  [conn](ringu::Result<std::vector<std::byte>> rr, ringu::OperationId, std::int32_t res) {
                    if (!rr.has_value() || res <= 0) {
                      // client went away: stop once every client is done
                      conn->state->active -= 1;
                      if (conn->state->accepted == conn->state->num_clients && conn->state->active == 0) {
                        conn->state->engine.request_stop();
                      }
                      return;
                    }
                    *conn->rbuf = std::move(*rr);
                    conn->rbuf->resize(16);
                    conn->state->engine.submit(
                        ringu::send(ringu::FileDescriptor{conn->cfd}), std::move(*conn->sbuf),
                        [conn](ringu::Result<std::vector<std::byte>> sr, ringu::OperationId, std::int32_t) {
                          if (!sr.has_value()) {
                            conn->state->active -= 1;
                            if (conn->state->accepted == conn->state->num_clients && conn->state->active == 0) {
                              conn->state->engine.request_stop();
                            }
                            return;
                          }
                          *conn->sbuf = std::move(*sr);
                          conn->do_recv();
                        });
                  });
            };
            conn->do_recv();
            if (state->accepted < state->num_clients) {
              do_accept(); // keep accepting the remaining clients
            }
          });
    };
    do_accept();
    engine->process(ringu::RingEngine::ProcessMode::Forever);
    ::close(lfd);
  });

  // N client threads: one RingEngine + one connection each, all to the same server port
  for (std::int32_t i = 0; i < num_clients; ++i) {
    clients.emplace_back([&, i] {
      while (ready.load() < 1) {
        std::this_thread::yield();
      }
      auto ring_engine_result = ringu::RingEngine::create(engine_opts);
      if (!ring_engine_result.has_value()) {
        fmt::print(stderr, "client {}: ring engine: {}\n", i, ring_engine_result.error().message);
        std::exit(1);
      }
      auto engine = std::move(ring_engine_result).value();
      auto sock =
          ringu::create_socket(ringu::AddressFamily::Inet, ringu::SocketType::Stream, ringu::SocketProtocol::Tcp);
      if (!sock.has_value()) {
        fmt::print(stderr, "client {}: socket: {}\n", i, std::get<ringu::IoError>(sock.error()).message());
        std::exit(1);
      }
      const std::int32_t fd = sock->value();
      // connect through the engine with the Connect operation
      const auto endpoint = ringu::Ipv4Endpoint::loopback(base_port);
      bool connected = false;
      for (std::int32_t tries = 0; tries < 10000 && !connected; ++tries) {
        engine->submit(ringu::connect(ringu::FileDescriptor{fd}, endpoint),
                       [&](ringu::Result<std::int32_t> r, ringu::OperationId, std::int32_t) {
                         if (r.has_value())
                           connected = true;
                       });
        engine->process(ringu::RingEngine::ProcessMode::Batch);
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
        engine->submit(ringu::recv(ringu::FileDescriptor{fd}), std::move(*rbuf),
                       [&](ringu::Result<std::vector<std::byte>> rr, ringu::OperationId, std::int32_t res) {
                         if (!rr.has_value() || res <= 0) {
                           engine->request_stop();
                           return;
                         }
                         *rbuf = std::move(*rr);
                         rbuf->resize(16);
                         count += 1;
                         if (count < messages) {
                           do_send();
                         } else {
                           engine->request_stop();
                         }
                       });
      };
      do_send = [&] {
        engine->submit(ringu::send(ringu::FileDescriptor{fd}), std::move(*sbuf),
                       [&](ringu::Result<std::vector<std::byte>> sr, ringu::OperationId, std::int32_t res) {
                         if (!sr.has_value() || res <= 0) {
                           engine->request_stop();
                           return;
                         }
                         *sbuf = std::move(*sr);
                         do_recv();
                       });
      };
      do_send();
      engine->process(ringu::RingEngine::ProcessMode::Forever);
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
  server.join();

  std::int64_t total_msgs = 0;
  double wall = 0.0;
  for (std::int32_t i = 0; i < num_clients; ++i) {
    total_msgs += client_msgs[static_cast<std::size_t>(i)];
    if (client_seconds[static_cast<std::size_t>(i)] > wall) {
      wall = client_seconds[static_cast<std::size_t>(i)];
    }
    fmt::print("  client {}: {} round trips in {:.3f} s ({:.0f} rt/s, avg latency {:.2f} us)\n", i,
               client_msgs[static_cast<std::size_t>(i)], client_seconds[static_cast<std::size_t>(i)],
               static_cast<double>(client_msgs[static_cast<std::size_t>(i)]) /
                   client_seconds[static_cast<std::size_t>(i)],
               client_seconds[static_cast<std::size_t>(i)] * 1e6 /
                   static_cast<double>(client_msgs[static_cast<std::size_t>(i)]));
  }
  const double rt_per_s = wall > 0.0 ? static_cast<double>(total_msgs) / wall : 0.0;
  const double bytes_per_rt = 10.0; // "hello" + "world"
  fmt::print("ringu TCP hello-world: 1 server thread + {} client threads x {} messages (sqpoll {})\n", num_clients,
             messages, sqpoll ? "on" : "off");
  fmt::print("  total round trips: {} in {:.3f} s\n", total_msgs, wall);
  fmt::print("  throughput: {:.0f} round trips/s ({:.2f} MiB/s payload)\n", rt_per_s,
             rt_per_s * bytes_per_rt / (1024.0 * 1024.0));
  fmt::print("  avg round-trip latency: {:.2f} us\n", wall * 1e6 / static_cast<double>(total_msgs));
  return 0;
}
