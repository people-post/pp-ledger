//
//  config.h
//
//  Fork of cpp-httplib 0.32 (pp-ledger). Tunables live in ServerConfig /
//  ClientConfig / Limits / Defaults — not CPPHTTPLIB_* macros.
//  Feature gates (OPENSSL / zlib / …) remain compile-time macros.
//  Follow-up: plumb Limits into every detail parser (today Defaults is used).
//

#ifndef CPPHTTPLIB_CONFIG_H
#define CPPHTTPLIB_CONFIG_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <thread>

#define CPPHTTPLIB_VERSION "0.32.0"
#define CPPHTTPLIB_VERSION_NUM "0x002000"

namespace httplib {

struct Timeouts {
  time_t keep_alive_sec = 5;
  time_t keep_alive_check_interval_usec = 10000;
  time_t connection_sec = 300;
  time_t connection_usec = 0;
  time_t server_read_sec = 5;
  time_t server_read_usec = 0;
  time_t server_write_sec = 5;
  time_t server_write_usec = 0;
  time_t client_read_sec = 300;
  time_t client_read_usec = 0;
  time_t client_write_sec = 5;
  time_t client_write_usec = 0;
  time_t client_max_msec = 0;
  time_t idle_interval_sec = 0;
#ifdef _WIN32
  time_t idle_interval_usec = 1000;
#else
  time_t idle_interval_usec = 0;
#endif
  size_t expect_100_threshold = 1024;
  time_t expect_100_timeout_msec = 1000;
  size_t wait_early_server_response_threshold = 1024 * 1024;
  time_t wait_early_server_response_timeout_msec = 50;
  time_t websocket_read_sec = 300;
  time_t websocket_close_sec = 5;
  time_t websocket_ping_interval_sec = 30;
};

struct Limits {
  size_t request_uri_max = 8192;
  size_t header_max_length = 8192;
  size_t header_max_count = 100;
  size_t redirect_max_count = 20;
  size_t multipart_file_max = 1024;
  size_t payload_max = 100 * 1024 * 1024;
  size_t form_urlencoded_payload_max = 8192;
  size_t range_max_count = 1024;
  size_t keep_alive_max_count = 100;
  size_t websocket_max_payload = 16777216;
};

struct IoBuffers {
  static constexpr size_t recv = 16384u;
  static constexpr size_t send = 16384u;
  static constexpr size_t compression = 16384u;
  static constexpr size_t max_line = 32768u;
  static constexpr int recv_flags = 0;
  static constexpr int send_flags = 0;
};

struct ThreadPoolConfig {
  size_t count =
      (std::max)(static_cast<size_t>(8u),
                 std::thread::hardware_concurrency() > 0
                     ? static_cast<size_t>(std::thread::hardware_concurrency() - 1)
                     : static_cast<size_t>(0));
  size_t max_count = 0; // 0 => count * 4 at use site
  int idle_timeout_sec = 3;

  size_t effective_max_count() const {
    return max_count != 0 ? max_count : count * 4;
  }
};

struct ServerConfig {
  Timeouts timeouts;
  Limits limits;
  ThreadPoolConfig pool;
  int listen_backlog = 5;
  bool tcp_nodelay = false;
  bool ipv6_v6only = false;
};

struct ClientConfig {
  Timeouts timeouts;
  Limits limits;
  bool tcp_nodelay = false;
  bool ipv6_v6only = false;
};

struct WebsocketConfig {
  Timeouts timeouts;
  Limits limits;
};

/** Process-wide defaults for detail/ paths not yet plumbed per-instance. */
class Defaults {
public:
  static Defaults &get() {
    static Defaults instance;
    return instance;
  }

  ServerConfig server;
  ClientConfig client;
  WebsocketConfig websocket;
  Limits &limits() { return server.limits; }
  const Limits &limits() const { return server.limits; }
  Timeouts &timeouts() { return server.timeouts; }
  const Timeouts &timeouts() const { return server.timeouts; }
  ThreadPoolConfig &pool() { return server.pool; }
  const ThreadPoolConfig &pool() const { return server.pool; }

  Defaults(const Defaults &) = delete;
  Defaults &operator=(const Defaults &) = delete;

private:
  Defaults() = default;
};

} // namespace httplib

#endif // CPPHTTPLIB_CONFIG_H
