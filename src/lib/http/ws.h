//
//  ws.h
//

#ifndef CPPHTTPLIB_WS_H
#define CPPHTTPLIB_WS_H

#include "stream.h"

namespace httplib {

namespace ws {

enum class Opcode : uint8_t {
  Continuation = 0x0,
  Text = 0x1,
  Binary = 0x2,
  Close = 0x8,
  Ping = 0x9,
  Pong = 0xA,
};

enum class CloseStatus : uint16_t {
  Normal = 1000,
  GoingAway = 1001,
  ProtocolError = 1002,
  UnsupportedData = 1003,
  NoStatus = 1005,
  Abnormal = 1006,
  InvalidPayload = 1007,
  PolicyViolation = 1008,
  MessageTooBig = 1009,
  MandatoryExtension = 1010,
  InternalError = 1011,
};

enum ReadResult : int { Fail = 0, Text = 1, Binary = 2 };

class WebSocket {
public:
  WebSocket(const WebSocket &) = delete;
  WebSocket &operator=(const WebSocket &) = delete;
  ~WebSocket();

  ReadResult read(std::string &msg);
  bool send(const std::string &data);
  bool send(const char *data, size_t len);
  void close(CloseStatus status = CloseStatus::Normal,
             const std::string &reason = "");
  const Request &request() const;
  bool is_open() const;

private:
  friend class httplib::Server;
  friend class WebSocketClient;

  WebSocket(Stream &strm, const Request &req, bool is_server)
      : strm_(strm), req_(req), is_server_(is_server) {
    start_heartbeat();
  }

  WebSocket(std::unique_ptr<Stream> &&owned_strm, const Request &req,
            bool is_server)
      : strm_(*owned_strm), owned_strm_(std::move(owned_strm)), req_(req),
        is_server_(is_server) {
    start_heartbeat();
  }

  void start_heartbeat();
  bool send_frame(Opcode op, const char *data, size_t len, bool fin = true);

  Stream &strm_;
  std::unique_ptr<Stream> owned_strm_;
  Request req_;
  bool is_server_;
  std::atomic<bool> closed_{false};
  std::mutex write_mutex_;
  std::thread ping_thread_;
  std::mutex ping_mutex_;
  std::condition_variable ping_cv_;
};

class WebSocketClient {
public:
  explicit WebSocketClient(const std::string &scheme_host_port_path,
                           const Headers &headers = {});

  ~WebSocketClient();
  WebSocketClient(const WebSocketClient &) = delete;
  WebSocketClient &operator=(const WebSocketClient &) = delete;

  bool is_valid() const;

  bool connect();
  ReadResult read(std::string &msg);
  bool send(const std::string &data);
  bool send(const char *data, size_t len);
  void close(CloseStatus status = CloseStatus::Normal,
             const std::string &reason = "");
  bool is_open() const;
  const std::string &subprotocol() const;
  void set_read_timeout(time_t sec, time_t usec = 0);
  void set_write_timeout(time_t sec, time_t usec = 0);

#ifdef CPPHTTPLIB_SSL_ENABLED
  void set_ca_cert_path(const std::string &path);
  void set_ca_cert_store(tls::ca_store_t store);
  void enable_server_certificate_verification(bool enabled);
#endif

private:
  void shutdown_and_close();
  bool create_stream(std::unique_ptr<Stream> &strm);

  std::string host_;
  int port_;
  std::string path_;
  Headers headers_;
  std::string subprotocol_;
  bool is_valid_ = false;
  socket_t sock_ = INVALID_SOCKET;
  std::unique_ptr<WebSocket> ws_;
  time_t read_timeout_sec_ = Defaults::get().websocket.timeouts.websocket_read_sec;
  time_t read_timeout_usec_ = 0;
  time_t write_timeout_sec_ = Defaults::get().websocket.timeouts.client_write_sec;
  time_t write_timeout_usec_ = Defaults::get().websocket.timeouts.client_write_usec;

#ifdef CPPHTTPLIB_SSL_ENABLED
  bool is_ssl_ = false;
  tls::ctx_t tls_ctx_ = nullptr;
  tls::session_t tls_session_ = nullptr;
  std::string ca_cert_file_path_;
  tls::ca_store_t ca_cert_store_ = nullptr;
  bool server_certificate_verification_ = true;
#endif
};

namespace impl {

bool is_valid_utf8(const std::string &s);

bool read_websocket_frame(Stream &strm, Opcode &opcode, std::string &payload,
                          bool &fin, bool expect_masked, size_t max_len);

} // namespace impl

} // namespace ws

namespace detail {

bool write_websocket_frame(Stream &strm, ws::Opcode opcode, const char *data,
                           size_t len, bool fin, bool mask);
bool read_websocket_upgrade_response(Stream &strm,
                                     const std::string &expected_accept,
                                     std::string &selected_subprotocol);
bool perform_websocket_handshake(Stream &strm, const std::string &host,
                                 int port, const std::string &path,
                                 const Headers &headers,
                                 std::string &selected_subprotocol);

} // namespace detail


} // namespace httplib

#endif // CPPHTTPLIB_WS_H
