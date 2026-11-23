// Single-threaded TCP hello-world server with N multi-threaded clients.
//
// One server thread owns a dojio::RingEngine, a single listening socket on
// 127.0.0.1:<base_port> and a dojio::RegisteredMemoryPool of fixed buffers; it
// accepts all N client connections on that socket. N client threads each own a
// dojio::RingEngine, a small RegisteredMemoryPool and one connection to the server.
// Each round trip: the client sends "hello", the server replies "world". The demo
// reports the aggregate throughput.
//
// Every recv/send is issued with a buffer borrowed from the pool: a RegisteredBuffer
// aliasing a chunk of the pre-registered, cache-line aligned mmap region. The buffer
// travels with the operation and comes back through the completion Result, so no
// buffer memory is allocated on the I/O path and the memory layout is stable for the
// whole run. The accept carries no address buffer (dojio::accept without a buffer).
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
#include <string_view>
#include <thread>
#include <vector>

#include <dojio/dojio.hpp>

namespace {

/// Message sent by a client for every round trip.
constexpr std::string_view k_request = "hello";
/// Message sent back by the server for every round trip.
constexpr std::string_view k_reply = "world";
static_assert(k_request.size() == k_reply.size());
/// Pool buffer size: one message per buffer (recv and send use whole buffers).
constexpr std::uint64_t k_message_size = k_request.size();

/// Creates the registered buffer pool of one engine, exiting the demo on failure.
dojio::RegisteredMemoryPool make_pool(dojio::detail::IoUring& ring, std::uint64_t count, std::string_view who) {
  return dojio::result_or_fail(dojio::RegisteredMemoryPool::create(ring, k_message_size, count),
                               fmt::format("{}: memory pool", who));
}

} // namespace

int main(int argc, char** argv) {
  const std::int32_t num_clients = argc > 1 ? static_cast<std::int32_t>(std::atoi(argv[1])) : 4;
  const std::int32_t messages = argc > 2 ? static_cast<std::int32_t>(std::atoi(argv[2])) : 20000;
  const std::uint16_t base_port = argc > 3 ? static_cast<std::uint16_t>(std::atoi(argv[3])) : 24100;
  const bool sqpoll = false;
  const dojio::RingEngineOptions engine_opts{.entries = 1024, .sqpoll = sqpoll};

  std::atomic<std::int32_t> ready{0};
  std::vector<std::thread> clients;
  std::vector<std::int64_t> client_msgs(static_cast<std::size_t>(num_clients), 0);
  std::vector<double> client_seconds(static_cast<std::size_t>(num_clients), 0.0);

  // single server thread: one RingEngine + one listening socket, accepts all N clients
  std::thread server([&] {
    auto engine = dojio::result_or_fail(dojio::RingEngine::create(engine_opts), "server: ring engine");
    // one pool for the whole server: every connection holds at most one buffer for its
    // pending recv and one for its pending send
    dojio::RegisteredMemoryPool pool =
        make_pool(engine->ring(), static_cast<std::uint64_t>(2) * static_cast<std::uint64_t>(num_clients), "server");
    const std::int32_t lfd =
        dojio::result_or_fail(dojio::create_socket(dojio::AddressFamily::Inet, dojio::SocketType::Stream,
                                                   dojio::SocketProtocol::Tcp, dojio::SocketFlag::CloExec),
                              "server: socket")
            .value();
    const std::int32_t one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // bind through the RingEngine (IORING_OP_BIND on a recent kernel, inline bind(2) otherwise)
    engine->submit(dojio::bind(dojio::FileDescriptor{lfd}, dojio::Ipv4Endpoint::loopback(base_port)),
                   [](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t) {
                     dojio::result_or_fail(std::move(r), "server: bind");
                   });
    engine->process(dojio::RingEngine::ProcessMode::Batch);
    // listen through the RingEngine (IORING_OP_LISTEN on a recent kernel, inline listen(2) otherwise)
    engine->submit(dojio::listen(dojio::FileDescriptor{lfd}, 128),
                   [](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t) {
                     dojio::result_or_fail(std::move(r), "server: listen");
                   });
    engine->process(dojio::RingEngine::ProcessMode::Batch);
    ready.fetch_add(1);

    // accept/connection state shared by the callbacks below; every callback runs
    // on this single server thread, so no synchronization is needed
    struct State {
      dojio::RingEngine& engine;
      dojio::RegisteredMemoryPool& pool;
      std::int32_t lfd;
      std::int32_t num_clients;
      std::int32_t accepted = 0;
      std::int32_t active = 0;
    };
    auto state = std::make_shared<State>(State{*engine, pool, lfd, num_clients, 0, 0});

    // per-connection state, self-owned so the recursive callbacks keep it alive
    struct Conn {
      std::shared_ptr<State> state;
      std::int32_t cfd;
      dojio::RegisteredBuffer rbuf; // buffer of the pending recv
      dojio::RegisteredBuffer sbuf; // buffer of the pending send
      std::function<void()> do_recv;

      /// Drops the connection: returns its buffers to the pool and stops the server
      /// once every accepted client is gone. Breaks the self-reference of do_recv.
      void release() {
        do_recv = {};
        rbuf = {};
        sbuf = {};
        state->active -= 1;
        if (state->accepted == state->num_clients && state->active == 0) {
          state->engine.request_stop();
        }
      }
    };

    // one outstanding accept at a time; re-armed until every client is connected
    std::function<void()> do_accept;
    do_accept = [state, &do_accept] {
      // the accepted peer address is not needed: accept without an address buffer
      state->engine.submit(
          dojio::accept(dojio::FileDescriptor{state->lfd}),
          [state, &do_accept](dojio::Result<dojio::FileDescriptor> r, dojio::OperationId, std::int32_t) {
            if (!r.has_value()) {
              if (state->accepted < state->num_clients) {
                do_accept(); // transient accept error: keep waiting for clients
              }
              return;
            }
            state->accepted += 1;
            state->active += 1;
            // two buffers per connection: one in flight (recv or send), one ready
            auto conn = std::make_shared<Conn>(Conn{state,
                                                    r->value(),
                                                    dojio::result_or_fail(state->pool.borrow(), "server: borrow"),
                                                    dojio::result_or_fail(state->pool.borrow(), "server: borrow"),
                                                    {}});
            conn->sbuf.append(k_reply);
            conn->do_recv = [conn] {
              conn->state->engine.submit(
                  dojio::recv(dojio::FileDescriptor{conn->cfd}), std::move(conn->rbuf),
                  [conn](dojio::Result<dojio::RegisteredBuffer> rr, dojio::OperationId, std::int32_t res) {
                    if (!rr.has_value() || res <= 0) {
                      conn->release(); // client went away
                      return;
                    }
                    conn->rbuf = std::move(*rr); // the buffer came back through the Result
                    conn->state->engine.submit(
                        dojio::send(dojio::FileDescriptor{conn->cfd}), std::move(conn->sbuf),
                        [conn](dojio::Result<dojio::RegisteredBuffer> sr, dojio::OperationId, std::int32_t send_res) {
                          if (!sr.has_value() || send_res <= 0) {
                            conn->release();
                            return;
                          }
                          conn->sbuf = std::move(*sr);
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
    engine->process(dojio::RingEngine::ProcessMode::Forever);
    ::close(lfd);
  });

  // N client threads: one RingEngine + one connection each, all to the same server port
  for (std::int32_t i = 0; i < num_clients; ++i) {
    clients.emplace_back([&, i] {
      while (ready.load() < 1) {
        std::this_thread::yield();
      }
      auto engine =
          dojio::result_or_fail(dojio::RingEngine::create(engine_opts), fmt::format("client {}: ring engine", i));
      // one buffer for the pending send and one for the pending recv
      dojio::RegisteredMemoryPool pool = make_pool(engine->ring(), 2, fmt::format("client {}", i));
      const std::int32_t fd =
          dojio::result_or_fail(
              dojio::create_socket(dojio::AddressFamily::Inet, dojio::SocketType::Stream, dojio::SocketProtocol::Tcp),
              fmt::format("client {}: socket", i))
              .value();
      // connect through the engine with the Connect operation
      const auto endpoint = dojio::Ipv4Endpoint::loopback(base_port);
      bool connected = false;
      for (std::int32_t tries = 0; tries < 10000 && !connected; ++tries) {
        engine->submit(dojio::connect(dojio::FileDescriptor{fd}, endpoint),
                       [&](dojio::Result<std::int32_t> r, dojio::OperationId, std::int32_t) {
                         if (r.has_value()) {
                           connected = true;
                         }
                       });
        engine->process(dojio::RingEngine::ProcessMode::Batch);
        if (!connected) {
          std::this_thread::yield();
        }
      }
      if (!connected) {
        fmt::print(stderr, "client {}: connect failed\n", i);
        std::exit(1);
      }
      dojio::RegisteredBuffer sbuf = dojio::result_or_fail(pool.borrow(), fmt::format("client {}: borrow", i));
      dojio::RegisteredBuffer rbuf = dojio::result_or_fail(pool.borrow(), fmt::format("client {}: borrow", i));
      sbuf.append(k_request);
      std::int64_t count = 0;
      const auto start = std::chrono::steady_clock::now();
      std::function<void()> do_send;
      std::function<void()> do_recv;
      do_recv = [&] {
        engine->submit(dojio::recv(dojio::FileDescriptor{fd}), std::move(rbuf),
                       [&](dojio::Result<dojio::RegisteredBuffer> rr, dojio::OperationId, std::int32_t res) {
                         if (!rr.has_value() || res <= 0) {
                           engine->request_stop();
                           return;
                         }
                         rbuf = std::move(*rr);
                         count += 1;
                         if (count < messages) {
                           do_send();
                         } else {
                           engine->request_stop();
                         }
                       });
      };
      do_send = [&] {
        engine->submit(dojio::send(dojio::FileDescriptor{fd}), std::move(sbuf),
                       [&](dojio::Result<dojio::RegisteredBuffer> sr, dojio::OperationId, std::int32_t res) {
                         if (!sr.has_value() || res <= 0) {
                           engine->request_stop();
                           return;
                         }
                         sbuf = std::move(*sr);
                         do_recv();
                       });
      };
      do_send();
      engine->process(dojio::RingEngine::ProcessMode::Forever);
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
  const double bytes_per_rt = static_cast<double>(k_request.size() + k_reply.size());
  fmt::print("dojio TCP hello-world: 1 server thread + {} client threads x {} messages (sqpoll {})\n", num_clients,
             messages, sqpoll ? "on" : "off");
  fmt::print("  buffers: {} B chunks from a RegisteredMemoryPool ({} server + 2 per client)\n", k_message_size,
             2 * num_clients);
  fmt::print("  total round trips: {} in {:.3f} s\n", total_msgs, wall);
  fmt::print("  throughput: {:.0f} round trips/s ({:.2f} MiB/s payload)\n", rt_per_s,
             rt_per_s * bytes_per_rt / (1024.0 * 1024.0));
  fmt::print("  avg round-trip latency: {:.2f} us\n", wall * 1e6 / static_cast<double>(total_msgs));
  return 0;
}
