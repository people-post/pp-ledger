#include "httplib.h"
namespace httplib {

namespace ws {
namespace impl {

bool is_valid_utf8(const std::string &s) {
  size_t i = 0;
  auto n = s.size();
  while (i < n) {
    auto c = static_cast<unsigned char>(s[i]);
    size_t len;
    uint32_t cp;
    if (c < 0x80) {
      i++;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + len > n) { return false; }
    for (size_t j = 1; j < len; j++) {
      auto b = static_cast<unsigned char>(s[i + j]);
      if ((b & 0xC0) != 0x80) { return false; }
      cp = (cp << 6) | (b & 0x3F);
    }
    // Overlong encoding check
    if (len == 2 && cp < 0x80) { return false; }
    if (len == 3 && cp < 0x800) { return false; }
    if (len == 4 && cp < 0x10000) { return false; }
    // Surrogate halves (U+D800..U+DFFF) and beyond U+10FFFF are invalid
    if (cp >= 0xD800 && cp <= 0xDFFF) { return false; }
    if (cp > 0x10FFFF) { return false; }
    i += len;
  }
  return true;
}

} // namespace impl
} // namespace ws
namespace ws {
namespace impl {

bool read_websocket_frame(Stream &strm, Opcode &opcode,
                                 std::string &payload, bool &fin,
                                 bool expect_masked, size_t max_len) {
  // Read first 2 bytes
  uint8_t header[2];
  if (strm.read(reinterpret_cast<char *>(header), 2) != 2) { return false; }

  fin = (header[0] & 0x80) != 0;

  // RSV1, RSV2, RSV3 must be 0 when no extension is negotiated
  if (header[0] & 0x70) { return false; }

  opcode = static_cast<Opcode>(header[0] & 0x0F);
  bool masked = (header[1] & 0x80) != 0;
  uint64_t payload_len = header[1] & 0x7F;

  // RFC 6455 Section 5.5: control frames MUST NOT be fragmented and
  // MUST have a payload length of 125 bytes or less
  bool is_control = (static_cast<uint8_t>(opcode) & 0x08) != 0;
  if (is_control) {
    if (!fin) { return false; }
    if (payload_len > 125) { return false; }
  }

  if (masked != expect_masked) { return false; }

  // Extended payload length
  if (payload_len == 126) {
    uint8_t ext[2];
    if (strm.read(reinterpret_cast<char *>(ext), 2) != 2) { return false; }
    payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
  } else if (payload_len == 127) {
    uint8_t ext[8];
    if (strm.read(reinterpret_cast<char *>(ext), 8) != 8) { return false; }
    // RFC 6455 Section 5.2: the most significant bit MUST be 0
    if (ext[0] & 0x80) { return false; }
    payload_len = 0;
    for (int i = 0; i < 8; i++) {
      payload_len = (payload_len << 8) | ext[i];
    }
  }

  if (payload_len > max_len) { return false; }

  // Read mask key if present
  uint8_t mask_key[4] = {0};
  if (masked) {
    if (strm.read(reinterpret_cast<char *>(mask_key), 4) != 4) { return false; }
  }

  // Read payload
  payload.resize(static_cast<size_t>(payload_len));
  if (payload_len > 0) {
    size_t total_read = 0;
    while (total_read < payload_len) {
      auto n = strm.read(&payload[total_read],
                         static_cast<size_t>(payload_len - total_read));
      if (n <= 0) { return false; }
      total_read += static_cast<size_t>(n);
    }
  }

  // Unmask if needed
  if (masked) {
    for (size_t i = 0; i < payload.size(); i++) {
      payload[i] ^= static_cast<char>(mask_key[i % 4]);
    }
  }

  return true;
}

} // namespace impl
} // namespace ws
namespace ws {

bool WebSocket::send_frame(Opcode op, const char *data, size_t len,
                                  bool fin) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  if (closed_) { return false; }
  return detail::write_websocket_frame(strm_, op, data, len, fin, !is_server_);
}

ReadResult WebSocket::read(std::string &msg) {
  while (!closed_) {
    Opcode opcode;
    std::string payload;
    bool fin;

    if (!impl::read_websocket_frame(strm_, opcode, payload, fin, is_server_,
                                    CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH)) {
      closed_ = true;
      return Fail;
    }

    switch (opcode) {
    case Opcode::Ping: {
      std::lock_guard<std::mutex> lock(write_mutex_);
      detail::write_websocket_frame(strm_, Opcode::Pong, payload.data(),
                                    payload.size(), true, !is_server_);
      continue;
    }
    case Opcode::Pong: continue;
    case Opcode::Close: {
      if (!closed_.exchange(true)) {
        // Echo close frame back
        std::lock_guard<std::mutex> lock(write_mutex_);
        detail::write_websocket_frame(strm_, Opcode::Close, payload.data(),
                                      payload.size(), true, !is_server_);
      }
      return Fail;
    }
    case Opcode::Text:
    case Opcode::Binary: {
      auto result = opcode == Opcode::Text ? Text : Binary;
      msg = std::move(payload);

      // Handle fragmentation
      if (!fin) {
        while (true) {
          Opcode cont_opcode;
          std::string cont_payload;
          bool cont_fin;
          if (!impl::read_websocket_frame(
                  strm_, cont_opcode, cont_payload, cont_fin, is_server_,
                  CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH)) {
            closed_ = true;
            return Fail;
          }
          if (cont_opcode == Opcode::Ping) {
            std::lock_guard<std::mutex> lock(write_mutex_);
            detail::write_websocket_frame(
                strm_, Opcode::Pong, cont_payload.data(), cont_payload.size(),
                true, !is_server_);
            continue;
          }
          if (cont_opcode == Opcode::Pong) { continue; }
          if (cont_opcode == Opcode::Close) {
            if (!closed_.exchange(true)) {
              std::lock_guard<std::mutex> lock(write_mutex_);
              detail::write_websocket_frame(
                  strm_, Opcode::Close, cont_payload.data(),
                  cont_payload.size(), true, !is_server_);
            }
            return Fail;
          }
          // RFC 6455: continuation frames must use opcode 0x0
          if (cont_opcode != Opcode::Continuation) {
            closed_ = true;
            return Fail;
          }
          msg += cont_payload;
          if (msg.size() > CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH) {
            closed_ = true;
            return Fail;
          }
          if (cont_fin) { break; }
        }
      }
      // RFC 6455 Section 5.6: text frames must contain valid UTF-8
      if (result == Text && !impl::is_valid_utf8(msg)) {
        close(CloseStatus::InvalidPayload, "invalid UTF-8");
        return Fail;
      }
      return result;
    }
    default: closed_ = true; return Fail;
    }
  }
  return Fail;
}

bool WebSocket::send(const std::string &data) {
  return send_frame(Opcode::Text, data.data(), data.size());
}

bool WebSocket::send(const char *data, size_t len) {
  return send_frame(Opcode::Binary, data, len);
}

void WebSocket::close(CloseStatus status, const std::string &reason) {
  if (closed_.exchange(true)) { return; }
  ping_cv_.notify_all();
  std::string payload;
  auto code = static_cast<uint16_t>(status);
  payload.push_back(static_cast<char>((code >> 8) & 0xFF));
  payload.push_back(static_cast<char>(code & 0xFF));
  // RFC 6455 Section 5.5: control frame payload must not exceed 125 bytes
  // Close frame has 2-byte status code, so reason is limited to 123 bytes
  payload += reason.substr(0, 123);
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    detail::write_websocket_frame(strm_, Opcode::Close, payload.data(),
                                  payload.size(), true, !is_server_);
  }

  // RFC 6455 Section 7.1.1: after sending a Close frame, wait for the peer's
  // Close response before closing the TCP connection. Use a short timeout to
  // avoid hanging if the peer doesn't respond.
  strm_.set_read_timeout(CPPHTTPLIB_WEBSOCKET_CLOSE_TIMEOUT_SECOND, 0);
  Opcode op;
  std::string resp;
  bool fin;
  while (impl::read_websocket_frame(strm_, op, resp, fin, is_server_, 125)) {
    if (op == Opcode::Close) { break; }
  }
}

WebSocket::~WebSocket() {
  {
    std::lock_guard<std::mutex> lock(ping_mutex_);
    closed_ = true;
  }
  ping_cv_.notify_all();
  if (ping_thread_.joinable()) { ping_thread_.join(); }
}

void WebSocket::start_heartbeat() {
  ping_thread_ = std::thread([this]() {
    std::unique_lock<std::mutex> lock(ping_mutex_);
    while (!closed_) {
      ping_cv_.wait_for(lock, std::chrono::seconds(
                                  CPPHTTPLIB_WEBSOCKET_PING_INTERVAL_SECOND));
      if (closed_) { break; }
      lock.unlock();
      if (!send_frame(Opcode::Ping, nullptr, 0)) {
        closed_ = true;
        break;
      }
      lock.lock();
    }
  });
}

const Request &WebSocket::request() const { return req_; }

bool WebSocket::is_open() const { return !closed_; }

// WebSocketClient implementation
WebSocketClient::WebSocketClient(
    const std::string &scheme_host_port_path, const Headers &headers)
    : headers_(headers) {
  const static std::regex re(
      R"(([a-z]+):\/\/(?:\[([a-fA-F\d:]+)\]|([^:/?#]+))(?::(\d+))?(\/.*))");

  std::smatch m;
  if (std::regex_match(scheme_host_port_path, m, re)) {
    auto scheme = m[1].str();

#ifdef CPPHTTPLIB_SSL_ENABLED
    if (scheme != "ws" && scheme != "wss") {
#else
    if (scheme != "ws") {
#endif
#ifndef CPPHTTPLIB_NO_EXCEPTIONS
      std::string msg = "'" + scheme + "' scheme is not supported.";
      throw std::invalid_argument(msg);
#endif
      return;
    }

    auto is_ssl = scheme == "wss";

    host_ = m[2].str();
    if (host_.empty()) { host_ = m[3].str(); }

    auto port_str = m[4].str();
    port_ = !port_str.empty() ? std::stoi(port_str) : (is_ssl ? 443 : 80);

    path_ = m[5].str();

#ifdef CPPHTTPLIB_SSL_ENABLED
    is_ssl_ = is_ssl;
#else
    if (is_ssl) { return; }
#endif

    is_valid_ = true;
  }
}

WebSocketClient::~WebSocketClient() { shutdown_and_close(); }

bool WebSocketClient::is_valid() const { return is_valid_; }

void WebSocketClient::shutdown_and_close() {
#ifdef CPPHTTPLIB_SSL_ENABLED
  if (is_ssl_) {
    if (tls_session_) {
      tls::shutdown(tls_session_, true);
      tls::free_session(tls_session_);
      tls_session_ = nullptr;
    }
    if (tls_ctx_) {
      tls::free_context(tls_ctx_);
      tls_ctx_ = nullptr;
    }
  }
#endif
  if (ws_ && ws_->is_open()) { ws_->close(); }
  ws_.reset();
  if (sock_ != INVALID_SOCKET) {
    detail::shutdown_socket(sock_);
    detail::close_socket(sock_);
    sock_ = INVALID_SOCKET;
  }
}

bool WebSocketClient::create_stream(std::unique_ptr<Stream> &strm) {
#ifdef CPPHTTPLIB_SSL_ENABLED
  if (is_ssl_) {
    if (!detail::setup_client_tls_session(
            host_, tls_ctx_, tls_session_, sock_,
            server_certificate_verification_, ca_cert_file_path_,
            ca_cert_store_, read_timeout_sec_, read_timeout_usec_)) {
      return false;
    }

    strm = std::unique_ptr<Stream>(new detail::SSLSocketStream(
        sock_, tls_session_, read_timeout_sec_, read_timeout_usec_,
        write_timeout_sec_, write_timeout_usec_));
    return true;
  }
#endif
  strm = std::unique_ptr<Stream>(
      new detail::SocketStream(sock_, read_timeout_sec_, read_timeout_usec_,
                               write_timeout_sec_, write_timeout_usec_));
  return true;
}

bool WebSocketClient::connect() {
  if (!is_valid_) { return false; }
  shutdown_and_close();

  Error error;
  sock_ = detail::create_client_socket(
      host_, std::string(), port_, AF_UNSPEC, false, false, nullptr, 5, 0,
      read_timeout_sec_, read_timeout_usec_, write_timeout_sec_,
      write_timeout_usec_, std::string(), error);

  if (sock_ == INVALID_SOCKET) { return false; }

  std::unique_ptr<Stream> strm;
  if (!create_stream(strm)) {
    shutdown_and_close();
    return false;
  }

  std::string selected_subprotocol;
  if (!detail::perform_websocket_handshake(*strm, host_, port_, path_, headers_,
                                           selected_subprotocol)) {
    shutdown_and_close();
    return false;
  }
  subprotocol_ = std::move(selected_subprotocol);

  Request req;
  req.method = "GET";
  req.path = path_;
  ws_ = std::unique_ptr<WebSocket>(new WebSocket(std::move(strm), req, false));
  return true;
}

ReadResult WebSocketClient::read(std::string &msg) {
  if (!ws_) { return Fail; }
  return ws_->read(msg);
}

bool WebSocketClient::send(const std::string &data) {
  if (!ws_) { return false; }
  return ws_->send(data);
}

bool WebSocketClient::send(const char *data, size_t len) {
  if (!ws_) { return false; }
  return ws_->send(data, len);
}

void WebSocketClient::close(CloseStatus status,
                                   const std::string &reason) {
  if (ws_) { ws_->close(status, reason); }
}

bool WebSocketClient::is_open() const { return ws_ && ws_->is_open(); }

const std::string &WebSocketClient::subprotocol() const {
  return subprotocol_;
}

void WebSocketClient::set_read_timeout(time_t sec, time_t usec) {
  read_timeout_sec_ = sec;
  read_timeout_usec_ = usec;
}

void WebSocketClient::set_write_timeout(time_t sec, time_t usec) {
  write_timeout_sec_ = sec;
  write_timeout_usec_ = usec;
}

#ifdef CPPHTTPLIB_SSL_ENABLED

void WebSocketClient::set_ca_cert_path(const std::string &path) {
  ca_cert_file_path_ = path;
}

void WebSocketClient::set_ca_cert_store(tls::ca_store_t store) {
  ca_cert_store_ = store;
}

void
WebSocketClient::enable_server_certificate_verification(bool enabled) {
  server_certificate_verification_ = enabled;
}

#endif // CPPHTTPLIB_SSL_ENABLED

} // namespace ws

} // namespace httplib
