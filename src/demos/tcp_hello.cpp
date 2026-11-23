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
// is embedded in the operation and comes back through the completion result, so no
// buffer memory is allocated on the I/O path and the layout is stable for the whole
// run. Only the counters are shared, kept in a dojio::SharedValue.
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
#include <memory_resource>
#include <string_view>
#include <thread>
#include <utility>
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

using RecvResult = dojio::RecvResult<dojio::RegisteredBuffer>;
using SendResult = dojio::SendResult<dojio::RegisteredBuffer>;

/// Server-side counters shared by every connection callback.
struct ServerCounters {
  std::int32_t num_clients = 0;
  std::int32_t accepted = 0;
  std::int32_t active = 0;
};

/// Client-side counter shared by the send/recv chain of one connection.
struct ClientCounters {
  std::int32_t count = 0;
};

/// Terminates a server connection: the buffers are dropped (returned to the pool) and
/// the server stops once every accepted client is gone.
void server_connection_done(dojio::RingEngine& ring, dojio::SharedValue<ServerCounters>& counters,
                            std::int32_t num_clients) {
  counters->active -= 1;
  if (counters->accepted == num_clients && counters->active == 0) {
    ring.request_stop();
  }
}

void conn_send(dojio::RingEngine& ring, dojio::FileDescriptor fd, std::int32_t num_clients,
               dojio::SharedValue<ServerCounters> counters, dojio::RegisteredBuffer send_buf,
               dojio::RegisteredBuffer recv_buf);

/// Receives one request on an accepted connection, then replies.
void conn_recv(dojio::RingEngine& ring, dojio::FileDescriptor fd, std::int32_t num_clients,
               dojio::SharedValue<ServerCounters> counters, dojio::RegisteredBuffer recv_buf,
               dojio::RegisteredBuffer send_buf) {
  ring.submit(dojio::recv(fd, std::move(recv_buf)), [fd, num_clients, counters, send_buf = std::move(send_buf)](
                                                        dojio::RingEngine& r, dojio::Result<RecvResult> res) mutable {
    if (!res.has_value() || res->size == 0) {
      server_connection_done(r, counters, num_clients); // client went away
      return;
    }
    conn_send(r, fd, num_clients, std::move(counters), std::move(send_buf), std::move(res->buffer));
  });
}

/// Replies on an accepted connection, then arms the next receive.
void conn_send(dojio::RingEngine& ring, dojio::FileDescriptor fd, std::int32_t num_clients,
               dojio::SharedValue<ServerCounters> counters, dojio::RegisteredBuffer send_buf,
               dojio::RegisteredBuffer recv_buf) {
  ring.submit(dojio::send(fd, std::move(send_buf)), [fd, num_clients, counters, recv_buf = std::move(recv_buf)](
                                                        dojio::RingEngine& r, dojio::Result<SendResult> res) mutable {
    if (!res.has_value() || res->size == 0) {
      server_connection_done(r, counters, num_clients);
      return;
    }
    conn_recv(r, fd, num_clients, std::move(counters), std::move(recv_buf), std::move(res->buffer));
  });
}

/// Arms the next accept; called until every client is connected.
void accept_next(dojio::RingEngine& ring, dojio::FileDescriptor lfd, std::int32_t num_clients,
                 dojio::SharedValue<ServerCounters> counters, dojio::RegisteredMemoryPool& pool) {
  ring.submit(dojio::accept(lfd), [lfd, num_clients, counters,
                                   pool = &pool](dojio::RingEngine& r, dojio::Result<dojio::AcceptResult> res) mutable {
    if (!res.has_value()) {
      if (counters->accepted < num_clients) {
        accept_next(r, lfd, num_clients, std::move(counters), *pool); // keep waiting for clients
      }
      return;
    }
    counters->accepted += 1;
    counters->active += 1;
    // two buffers per connection: one in flight (recv or send), one ready
    auto recv_buf = dojio::result_or_fail(pool->borrow(), "server: borrow");
    auto send_buf = dojio::result_or_fail(pool->borrow(), "server: borrow");
    send_buf.append(k_reply);
    conn_recv(r, res->fd, num_clients, counters, std::move(recv_buf), std::move(send_buf));
    if (counters->accepted < num_clients) {
      accept_next(r, lfd, num_clients, std::move(counters), *pool); // keep accepting
    }
  });
}

void client_recv(dojio::RingEngine& ring, dojio::FileDescriptor fd, std::int32_t messages,
                 dojio::SharedValue<ClientCounters> counters, dojio::RegisteredBuffer recv_buf,
                 dojio::RegisteredBuffer send_buf);

/// Sends one request on a client connection, then arms the matching receive.
void client_send(dojio::RingEngine& ring, dojio::FileDescriptor fd, std::int32_t messages,
                 dojio::SharedValue<ClientCounters> counters, dojio::RegisteredBuffer send_buf,
                 dojio::RegisteredBuffer recv_buf) {
  ring.submit(dojio::send(fd, std::move(send_buf)), [fd, messages, counters, recv_buf = std::move(recv_buf)](
                                                        dojio::RingEngine& r, dojio::Result<SendResult> res) mutable {
    if (!res.has_value() || res->size == 0) {
      r.request_stop();
      return;
    }
    client_recv(r, fd, messages, std::move(counters), std::move(recv_buf), std::move(res->buffer));
  });
}

/// Receives one reply, counts the round trip and sends the next request.
void client_recv(dojio::RingEngine& ring, dojio::FileDescriptor fd, std::int32_t messages,
                 dojio::SharedValue<ClientCounters> counters, dojio::RegisteredBuffer recv_buf,
                 dojio::RegisteredBuffer send_buf) {
  ring.submit(dojio::recv(fd, std::move(recv_buf)), [fd, messages, counters, send_buf = std::move(send_buf)](
                                                        dojio::RingEngine& r, dojio::Result<RecvResult> res) mutable {
    if (!res.has_value() || res->size == 0) {
      r.request_stop();
      return;
    }
    counters->count += 1;
    if (counters->count < messages) {
      client_send(r, fd, messages, std::move(counters), std::move(send_buf), std::move(res->buffer));
    } else {
      r.request_stop();
    }
  });
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
    dojio::RegisteredMemoryPool pool = dojio::result_or_fail(
        dojio::RegisteredMemoryPool::create(engine->ring(), k_message_size,
                                            static_cast<std::uint64_t>(2) * static_cast<std::uint64_t>(num_clients)),
        "server: memory pool");
    const std::int32_t lfd =
        dojio::result_or_fail(dojio::create_socket(dojio::AddressFamily::Inet, dojio::SocketType::Stream,
                                                   dojio::SocketProtocol::Tcp, dojio::SocketFlag::CloExec),
                              "server: socket")
            .value();
    const std::int32_t one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // bind through the RingEngine (IORING_OP_BIND on a recent kernel, inline bind(2) otherwise)
    engine->submit(dojio::bind(dojio::FileDescriptor{lfd}, dojio::Ipv4Endpoint::loopback(base_port)),
                   [](dojio::RingEngine&, dojio::Result<dojio::BindResult> r) {
                     dojio::result_or_fail(std::move(r), "server: bind");
                   });
    engine->process(dojio::RingEngine::ProcessMode::Batch);
    // listen through the RingEngine (IORING_OP_LISTEN on a recent kernel, inline listen(2) otherwise)
    engine->submit(dojio::listen(dojio::FileDescriptor{lfd}, 128),
                   [](dojio::RingEngine&, dojio::Result<dojio::ListenResult> r) {
                     dojio::result_or_fail(std::move(r), "server: listen");
                   });
    engine->process(dojio::RingEngine::ProcessMode::Batch);
    ready.fetch_add(1);

    dojio::SharedValue<ServerCounters> counters =
        dojio::SharedValue<ServerCounters>::make(std::pmr::new_delete_resource());
    counters->num_clients = num_clients;
    accept_next(*engine, dojio::FileDescriptor{lfd}, num_clients, counters, pool);
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
      dojio::RegisteredMemoryPool pool = dojio::result_or_fail(
          dojio::RegisteredMemoryPool::create(engine->ring(), k_message_size, 2), fmt::format("client {}", i));
      const std::int32_t fd =
          dojio::result_or_fail(
              dojio::create_socket(dojio::AddressFamily::Inet, dojio::SocketType::Stream, dojio::SocketProtocol::Tcp),
              fmt::format("client {}: socket", i))
              .value();
      // connect through the engine with the Connect operation (the server is listening:
      // the ready counter was incremented after its listen completed)
      engine->submit(dojio::connect(dojio::FileDescriptor{fd}, dojio::Ipv4Endpoint::loopback(base_port)),
                     [](dojio::RingEngine&, dojio::Result<dojio::ConnectResult> r) {
                       dojio::result_or_fail(std::move(r), "client: connect");
                     });
      engine->process(dojio::RingEngine::ProcessMode::Batch);

      dojio::SharedValue<ClientCounters> counters =
          dojio::SharedValue<ClientCounters>::make(std::pmr::new_delete_resource());
      dojio::RegisteredBuffer send_buf = dojio::result_or_fail(pool.borrow(), fmt::format("client {}: borrow", i));
      dojio::RegisteredBuffer recv_buf = dojio::result_or_fail(pool.borrow(), fmt::format("client {}: borrow", i));
      send_buf.append(k_request);

      const auto start = std::chrono::steady_clock::now();
      client_send(*engine, dojio::FileDescriptor{fd}, messages, counters, std::move(send_buf), std::move(recv_buf));
      engine->process(dojio::RingEngine::ProcessMode::Forever);
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
      client_msgs[static_cast<std::size_t>(i)] = counters->count;
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
