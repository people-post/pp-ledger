//
//  client.h
//

#ifndef CPPHTTPLIB_CLIENT_H
#define CPPHTTPLIB_CLIENT_H

#include "server.h"

namespace httplib {

class Result {
public:
  Result() = default;
  Result(std::unique_ptr<Response> &&res, Error err,
         Headers &&request_headers = Headers{})
      : res_(std::move(res)), err_(err),
        request_headers_(std::move(request_headers)) {}
  // Response
  operator bool() const { return res_ != nullptr; }
  bool operator==(std::nullptr_t) const { return res_ == nullptr; }
  bool operator!=(std::nullptr_t) const { return res_ != nullptr; }
  const Response &value() const { return *res_; }
  Response &value() { return *res_; }
  const Response &operator*() const { return *res_; }
  Response &operator*() { return *res_; }
  const Response *operator->() const { return res_.get(); }
  Response *operator->() { return res_.get(); }

  // Error
  Error error() const { return err_; }

  // Request Headers
  bool has_request_header(const std::string &key) const;
  std::string get_request_header_value(const std::string &key,
                                       const char *def = "",
                                       size_t id = 0) const;
  size_t get_request_header_value_u64(const std::string &key, size_t def = 0,
                                      size_t id = 0) const;
  size_t get_request_header_value_count(const std::string &key) const;

private:
  std::unique_ptr<Response> res_;
  Error err_ = Error::Unknown;
  Headers request_headers_;

#ifdef CPPHTTPLIB_SSL_ENABLED
public:
  Result(std::unique_ptr<Response> &&res, Error err, Headers &&request_headers,
         int ssl_error)
      : res_(std::move(res)), err_(err),
        request_headers_(std::move(request_headers)), ssl_error_(ssl_error) {}
  Result(std::unique_ptr<Response> &&res, Error err, Headers &&request_headers,
         int ssl_error, unsigned long ssl_backend_error)
      : res_(std::move(res)), err_(err),
        request_headers_(std::move(request_headers)), ssl_error_(ssl_error),
        ssl_backend_error_(ssl_backend_error) {}

  int ssl_error() const { return ssl_error_; }
  unsigned long ssl_backend_error() const { return ssl_backend_error_; }

private:
  int ssl_error_ = 0;
  unsigned long ssl_backend_error_ = 0;
#endif

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
public:
  [[deprecated("Use ssl_backend_error() instead")]]
  unsigned long ssl_openssl_error() const {
    return ssl_backend_error_;
  }
#endif
};

struct ClientConnection {
  socket_t sock = INVALID_SOCKET;

  bool is_open() const { return sock != INVALID_SOCKET; }

  ClientConnection() = default;

  ~ClientConnection();

  ClientConnection(const ClientConnection &) = delete;
  ClientConnection &operator=(const ClientConnection &) = delete;

  ClientConnection(ClientConnection &&other) noexcept
      : sock(other.sock)
#ifdef CPPHTTPLIB_SSL_ENABLED
        ,
        session(other.session)
#endif
  {
    other.sock = INVALID_SOCKET;
#ifdef CPPHTTPLIB_SSL_ENABLED
    other.session = nullptr;
#endif
  }

  ClientConnection &operator=(ClientConnection &&other) noexcept {
    if (this != &other) {
      sock = other.sock;
      other.sock = INVALID_SOCKET;
#ifdef CPPHTTPLIB_SSL_ENABLED
      session = other.session;
      other.session = nullptr;
#endif
    }
    return *this;
  }

#ifdef CPPHTTPLIB_SSL_ENABLED
  tls::session_t session = nullptr;
#endif
};

namespace detail {

struct ChunkedDecoder;

struct BodyReader {
  Stream *stream = nullptr;
  bool has_content_length = false;
  size_t content_length = 0;
  size_t payload_max_length = Defaults::get().limits().payload_max;
  size_t bytes_read = 0;
  bool chunked = false;
  bool eof = false;
  std::unique_ptr<ChunkedDecoder> chunked_decoder;
  Error last_error = Error::Success;

  ssize_t read(char *buf, size_t len);
  bool has_error() const { return last_error != Error::Success; }
};

inline ssize_t read_body_content(Stream *stream, BodyReader &br, char *buf,
                                 size_t len) {
  (void)stream;
  return br.read(buf, len);
}

class decompressor;

} // namespace detail

class ClientImpl {
public:
  explicit ClientImpl(const std::string &host,
                      const ClientConfig &config = {});

  explicit ClientImpl(const std::string &host, int port,
                      const ClientConfig &config = {});

  explicit ClientImpl(const std::string &host, int port,
                      const std::string &client_cert_path,
                      const std::string &client_key_path,
                      const ClientConfig &config = {});

  virtual ~ClientImpl();

  virtual bool is_valid() const;

  struct StreamHandle {
    std::unique_ptr<Response> response;
    Error error = Error::Success;

    StreamHandle() = default;
    StreamHandle(const StreamHandle &) = delete;
    StreamHandle &operator=(const StreamHandle &) = delete;
    StreamHandle(StreamHandle &&) = default;
    StreamHandle &operator=(StreamHandle &&) = default;
    ~StreamHandle() = default;

    bool is_valid() const {
      return response != nullptr && error == Error::Success;
    }

    ssize_t read(char *buf, size_t len);
    void parse_trailers_if_needed();
    Error get_read_error() const { return body_reader_.last_error; }
    bool has_read_error() const { return body_reader_.has_error(); }

    bool trailers_parsed_ = false;

  private:
    friend class ClientImpl;

    ssize_t read_with_decompression(char *buf, size_t len);

    std::unique_ptr<ClientConnection> connection_;
    std::unique_ptr<Stream> socket_stream_;
    Stream *stream_ = nullptr;
    detail::BodyReader body_reader_;

    std::unique_ptr<detail::decompressor> decompressor_;
    std::string decompress_buffer_;
    size_t decompress_offset_ = 0;
    size_t decompressed_bytes_read_ = 0;
  };

  // clang-format off
  Result Get(const std::string &path, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, ContentReceiver content_receiver, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, ResponseHandler response_handler, ContentReceiver content_receiver, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, const Headers &headers, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, const Headers &headers, ContentReceiver content_receiver, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, const Headers &headers, ResponseHandler response_handler, ContentReceiver content_receiver, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, const Params &params, const Headers &headers, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, const Params &params, const Headers &headers, ContentReceiver content_receiver, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, const Params &params, const Headers &headers, ResponseHandler response_handler, ContentReceiver content_receiver, DownloadProgress progress = nullptr);

  Result Head(const std::string &path);
  Result Head(const std::string &path, const Headers &headers);

  Result Post(const std::string &path);
  Result Post(const std::string &path, const char *body, size_t content_length, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const std::string &body, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, size_t content_length, ContentProvider content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, size_t content_length, ContentProvider content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Post(const std::string &path, ContentProviderWithoutLength content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, ContentProviderWithoutLength content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Params &params);
  Result Post(const std::string &path, const UploadFormDataItems &items, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers);
  Result Post(const std::string &path, const Headers &headers, const char *body, size_t content_length, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, const std::string &body, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, size_t content_length, ContentProvider content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, size_t content_length, ContentProvider content_provider, const std::string &content_type, ContentReceiver content_receiver, DownloadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, ContentProviderWithoutLength content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, ContentProviderWithoutLength content_provider, const std::string &content_type, ContentReceiver content_receiver, DownloadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, const Params &params);
  Result Post(const std::string &path, const Headers &headers, const UploadFormDataItems &items, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, const UploadFormDataItems &items, const std::string &boundary, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, const UploadFormDataItems &items, const FormDataProviderItems &provider_items, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, const std::string &body, const std::string &content_type, ContentReceiver content_receiver, DownloadProgress progress = nullptr);

  Result Put(const std::string &path);
  Result Put(const std::string &path, const char *body, size_t content_length, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const std::string &body, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, size_t content_length, ContentProvider content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, size_t content_length, ContentProvider content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Put(const std::string &path, ContentProviderWithoutLength content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, ContentProviderWithoutLength content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Params &params);
  Result Put(const std::string &path, const UploadFormDataItems &items, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers);
  Result Put(const std::string &path, const Headers &headers, const char *body, size_t content_length, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, const std::string &body, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, size_t content_length, ContentProvider content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, size_t content_length, ContentProvider content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, ContentProviderWithoutLength content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, ContentProviderWithoutLength content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, const Params &params);
  Result Put(const std::string &path, const Headers &headers, const UploadFormDataItems &items, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, const UploadFormDataItems &items, const std::string &boundary, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, const UploadFormDataItems &items, const FormDataProviderItems &provider_items, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, const std::string &body, const std::string &content_type, ContentReceiver content_receiver, DownloadProgress progress = nullptr);

  Result Patch(const std::string &path);
  Result Patch(const std::string &path, const char *body, size_t content_length, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const std::string &body, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, size_t content_length, ContentProvider content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, size_t content_length, ContentProvider content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, ContentProviderWithoutLength content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, ContentProviderWithoutLength content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Params &params);
  Result Patch(const std::string &path, const UploadFormDataItems &items, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, const char *body, size_t content_length, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, const std::string &body, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, size_t content_length, ContentProvider content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, size_t content_length, ContentProvider content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, ContentProviderWithoutLength content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, ContentProviderWithoutLength content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, const Params &params);
  Result Patch(const std::string &path, const Headers &headers, const UploadFormDataItems &items, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, const UploadFormDataItems &items, const std::string &boundary, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, const UploadFormDataItems &items, const FormDataProviderItems &provider_items, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, const std::string &body, const std::string &content_type, ContentReceiver content_receiver, DownloadProgress progress = nullptr);

  Result Delete(const std::string &path, DownloadProgress progress = nullptr);
  Result Delete(const std::string &path, const char *body, size_t content_length, const std::string &content_type, DownloadProgress progress = nullptr);
  Result Delete(const std::string &path, const std::string &body, const std::string &content_type, DownloadProgress progress = nullptr);
  Result Delete(const std::string &path, const Params &params, DownloadProgress progress = nullptr);
  Result Delete(const std::string &path, const Headers &headers, DownloadProgress progress = nullptr);
  Result Delete(const std::string &path, const Headers &headers, const char *body, size_t content_length, const std::string &content_type, DownloadProgress progress = nullptr);
  Result Delete(const std::string &path, const Headers &headers, const std::string &body, const std::string &content_type, DownloadProgress progress = nullptr);
  Result Delete(const std::string &path, const Headers &headers, const Params &params, DownloadProgress progress = nullptr);

  Result Options(const std::string &path);
  Result Options(const std::string &path, const Headers &headers);
  // clang-format on

  // Streaming API: Open a stream for reading response body incrementally
  // Socket ownership is transferred to StreamHandle for true streaming
  // Supports all HTTP methods (GET, POST, PUT, PATCH, DELETE, etc.)
  StreamHandle open_stream(const std::string &method, const std::string &path,
                           const Params &params = {},
                           const Headers &headers = {},
                           const std::string &body = {},
                           const std::string &content_type = {});

  bool send(Request &req, Response &res, Error &error);
  Result send(const Request &req);

  void stop();

  std::string host() const;
  int port() const;

  size_t is_socket_open() const;
  socket_t socket() const;

  void set_hostname_addr_map(std::map<std::string, std::string> addr_map);

  void set_default_headers(Headers headers);

  void
  set_header_writer(std::function<ssize_t(Stream &, Headers &)> const &writer);

  void set_address_family(int family);
  void set_tcp_nodelay(bool on);
  void set_ipv6_v6only(bool on);
  void set_socket_options(SocketOptions socket_options);

  void set_connection_timeout(time_t sec, time_t usec = 0);
  template <class Rep, class Period>
  void
  set_connection_timeout(const std::chrono::duration<Rep, Period> &duration);

  void set_read_timeout(time_t sec, time_t usec = 0);
  template <class Rep, class Period>
  void set_read_timeout(const std::chrono::duration<Rep, Period> &duration);

  void set_write_timeout(time_t sec, time_t usec = 0);
  template <class Rep, class Period>
  void set_write_timeout(const std::chrono::duration<Rep, Period> &duration);

  void set_max_timeout(time_t msec);
  template <class Rep, class Period>
  void set_max_timeout(const std::chrono::duration<Rep, Period> &duration);

  void set_basic_auth(const std::string &username, const std::string &password);
  void set_bearer_token_auth(const std::string &token);

  void set_keep_alive(bool on);
  void set_follow_location(bool on);

  void set_path_encode(bool on);

  void set_compress(bool on);

  void set_decompress(bool on);

  void set_payload_max_length(size_t length);

  void set_interface(const std::string &intf);

  void set_proxy(const std::string &host, int port);
  void set_proxy_basic_auth(const std::string &username,
                            const std::string &password);
  void set_proxy_bearer_token_auth(const std::string &token);

  void set_logger(Logger logger);
  void set_error_logger(ErrorLogger error_logger);

protected:
  struct Socket {
    socket_t sock = INVALID_SOCKET;

    // For Mbed TLS compatibility: start_time for request timeout tracking
    std::chrono::time_point<std::chrono::steady_clock> start_time_;

    bool is_open() const { return sock != INVALID_SOCKET; }

#ifdef CPPHTTPLIB_SSL_ENABLED
    tls::session_t ssl = nullptr;
#endif
  };

  virtual bool create_and_connect_socket(Socket &socket, Error &error);
  virtual bool ensure_socket_connection(Socket &socket, Error &error);

  // All of:
  //   shutdown_ssl
  //   shutdown_socket
  //   close_socket
  // should ONLY be called when socket_mutex_ is locked.
  // Also, shutdown_ssl and close_socket should also NOT be called concurrently
  // with a DIFFERENT thread sending requests using that socket.
  virtual void shutdown_ssl(Socket &socket, bool shutdown_gracefully);
  void shutdown_socket(Socket &socket) const;
  void close_socket(Socket &socket);

  bool process_request(Stream &strm, Request &req, Response &res,
                       bool close_connection, Error &error);

  bool write_content_with_provider(Stream &strm, const Request &req,
                                   Error &error) const;

  void copy_settings(const ClientImpl &rhs);

  void output_log(const Request &req, const Response &res) const;
  void output_error_log(const Error &err, const Request *req) const;

  // Socket endpoint information
  const std::string host_;
  const int port_;

  // Current open socket
  Socket socket_;
  mutable std::mutex socket_mutex_;
  std::recursive_mutex request_mutex_;

  // These are all protected under socket_mutex
  size_t socket_requests_in_flight_ = 0;
  std::thread::id socket_requests_are_from_thread_ = std::thread::id();
  bool socket_should_be_closed_when_request_is_done_ = false;

  // Hostname-IP map
  std::map<std::string, std::string> addr_map_;

  // Default headers
  Headers default_headers_;

  // Header writer
  std::function<ssize_t(Stream &, Headers &)> header_writer_ =
      detail::write_headers;

  // Settings
  std::string client_cert_path_;
  std::string client_key_path_;

  time_t connection_timeout_sec_ = Defaults::get().client.timeouts.connection_sec;
  time_t connection_timeout_usec_ = Defaults::get().client.timeouts.connection_usec;
  time_t read_timeout_sec_ = Defaults::get().client.timeouts.client_read_sec;
  time_t read_timeout_usec_ = Defaults::get().client.timeouts.client_read_usec;
  time_t write_timeout_sec_ = Defaults::get().client.timeouts.client_write_sec;
  time_t write_timeout_usec_ = Defaults::get().client.timeouts.client_write_usec;
  time_t max_timeout_msec_ = Defaults::get().client.timeouts.client_max_msec;

  std::string basic_auth_username_;
  std::string basic_auth_password_;
  std::string bearer_token_auth_token_;

  bool keep_alive_ = false;
  bool follow_location_ = false;

  bool path_encode_ = true;

  int address_family_ = AF_UNSPEC;
  bool tcp_nodelay_ = Defaults::get().client.tcp_nodelay;
  bool ipv6_v6only_ = Defaults::get().client.ipv6_v6only;
  SocketOptions socket_options_ = nullptr;

  bool compress_ = false;
  bool decompress_ = true;

  size_t payload_max_length_ = Defaults::get().client.limits.payload_max;
  bool has_payload_max_length_ = false;

  std::string interface_;

  std::string proxy_host_;
  int proxy_port_ = -1;

  std::string proxy_basic_auth_username_;
  std::string proxy_basic_auth_password_;
  std::string proxy_bearer_token_auth_token_;

  mutable std::mutex logger_mutex_;
  Logger logger_;
  ErrorLogger error_logger_;

private:
  bool send_(Request &req, Response &res, Error &error);
  Result send_(Request &&req);

  socket_t create_client_socket(Error &error) const;
  bool read_response_line(Stream &strm, const Request &req, Response &res,
                          bool skip_100_continue = true) const;
  bool write_request(Stream &strm, Request &req, bool close_connection,
                     Error &error, bool skip_body = false);
  bool write_request_body(Stream &strm, Request &req, Error &error);
  void prepare_default_headers(Request &r, bool for_stream,
                               const std::string &ct);
  bool redirect(Request &req, Response &res, Error &error);
  bool create_redirect_client(const std::string &scheme,
                              const std::string &host, int port, Request &req,
                              Response &res, const std::string &path,
                              const std::string &location, Error &error);
  template <typename ClientType> void setup_redirect_client(ClientType &client);
  bool handle_request(Stream &strm, Request &req, Response &res,
                      bool close_connection, Error &error);
  std::unique_ptr<Response> send_with_content_provider_and_receiver(
      Request &req, const char *body, size_t content_length,
      ContentProvider content_provider,
      ContentProviderWithoutLength content_provider_without_length,
      const std::string &content_type, ContentReceiver content_receiver,
      Error &error);
  Result send_with_content_provider_and_receiver(
      const std::string &method, const std::string &path,
      const Headers &headers, const char *body, size_t content_length,
      ContentProvider content_provider,
      ContentProviderWithoutLength content_provider_without_length,
      const std::string &content_type, ContentReceiver content_receiver,
      UploadProgress progress);
  ContentProviderWithoutLength get_multipart_content_provider(
      const std::string &boundary, const UploadFormDataItems &items,
      const FormDataProviderItems &provider_items) const;

  virtual bool
  process_socket(const Socket &socket,
                 std::chrono::time_point<std::chrono::steady_clock> start_time,
                 std::function<bool(Stream &strm)> callback);
  virtual bool is_ssl() const;

  void transfer_socket_ownership_to_handle(StreamHandle &handle);

#ifdef CPPHTTPLIB_SSL_ENABLED
public:
  void set_digest_auth(const std::string &username,
                       const std::string &password);
  void set_proxy_digest_auth(const std::string &username,
                             const std::string &password);
  void set_ca_cert_path(const std::string &ca_cert_file_path,
                        const std::string &ca_cert_dir_path = std::string());
  void enable_server_certificate_verification(bool enabled);
  void enable_server_hostname_verification(bool enabled);

protected:
  std::string digest_auth_username_;
  std::string digest_auth_password_;
  std::string proxy_digest_auth_username_;
  std::string proxy_digest_auth_password_;
  std::string ca_cert_file_path_;
  std::string ca_cert_dir_path_;
  bool server_certificate_verification_ = true;
  bool server_hostname_verification_ = true;
  std::string ca_cert_pem_; // Store CA cert PEM for redirect transfer
  int last_ssl_error_ = 0;
  unsigned long last_backend_error_ = 0;
#endif

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
public:
  [[deprecated("Use load_ca_cert_store() instead")]]
  void set_ca_cert_store(X509_STORE *ca_cert_store);

  [[deprecated("Use tls::create_ca_store() instead")]]
  X509_STORE *create_ca_cert_store(const char *ca_cert, std::size_t size) const;

  [[deprecated("Use set_server_certificate_verifier(VerifyCallback) instead")]]
  virtual void set_server_certificate_verifier(
      std::function<SSLVerifierResponse(SSL *ssl)> verifier);
#endif
};

class Client {
public:
  // Universal interface
  explicit Client(const std::string &scheme_host_port);

  explicit Client(const std::string &scheme_host_port,
                  const std::string &client_cert_path,
                  const std::string &client_key_path);

  // HTTP only interface
  explicit Client(const std::string &host, int port);

  explicit Client(const std::string &host, int port,
                  const std::string &client_cert_path,
                  const std::string &client_key_path);

  Client(Client &&) = default;
  Client &operator=(Client &&) = default;

  ~Client();

  bool is_valid() const;

  // clang-format off
  Result Get(const std::string &path, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, ContentReceiver content_receiver, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, ResponseHandler response_handler, ContentReceiver content_receiver, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, const Headers &headers, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, const Headers &headers, ContentReceiver content_receiver, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, const Headers &headers, ResponseHandler response_handler, ContentReceiver content_receiver, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, const Params &params, const Headers &headers, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, const Params &params, const Headers &headers, ContentReceiver content_receiver, DownloadProgress progress = nullptr);
  Result Get(const std::string &path, const Params &params, const Headers &headers, ResponseHandler response_handler, ContentReceiver content_receiver, DownloadProgress progress = nullptr);

  Result Head(const std::string &path);
  Result Head(const std::string &path, const Headers &headers);

  Result Post(const std::string &path);
  Result Post(const std::string &path, const char *body, size_t content_length, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const std::string &body, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, size_t content_length, ContentProvider content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, size_t content_length, ContentProvider content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Post(const std::string &path, ContentProviderWithoutLength content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, ContentProviderWithoutLength content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Params &params);
  Result Post(const std::string &path, const UploadFormDataItems &items, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers);
  Result Post(const std::string &path, const Headers &headers, const char *body, size_t content_length, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, const std::string &body, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, size_t content_length, ContentProvider content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, size_t content_length, ContentProvider content_provider, const std::string &content_type, ContentReceiver content_receiver, DownloadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, ContentProviderWithoutLength content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, ContentProviderWithoutLength content_provider, const std::string &content_type, ContentReceiver content_receiver, DownloadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, const Params &params);
  Result Post(const std::string &path, const Headers &headers, const UploadFormDataItems &items, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, const UploadFormDataItems &items, const std::string &boundary, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, const UploadFormDataItems &items, const FormDataProviderItems &provider_items, UploadProgress progress = nullptr);
  Result Post(const std::string &path, const Headers &headers, const std::string &body, const std::string &content_type, ContentReceiver content_receiver, DownloadProgress progress = nullptr);

  Result Put(const std::string &path);
  Result Put(const std::string &path, const char *body, size_t content_length, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const std::string &body, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, size_t content_length, ContentProvider content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, size_t content_length, ContentProvider content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Put(const std::string &path, ContentProviderWithoutLength content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, ContentProviderWithoutLength content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Params &params);
  Result Put(const std::string &path, const UploadFormDataItems &items, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers);
  Result Put(const std::string &path, const Headers &headers, const char *body, size_t content_length, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, const std::string &body, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, size_t content_length, ContentProvider content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, size_t content_length, ContentProvider content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, ContentProviderWithoutLength content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, ContentProviderWithoutLength content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, const Params &params);
  Result Put(const std::string &path, const Headers &headers, const UploadFormDataItems &items, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, const UploadFormDataItems &items, const std::string &boundary, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, const UploadFormDataItems &items, const FormDataProviderItems &provider_items, UploadProgress progress = nullptr);
  Result Put(const std::string &path, const Headers &headers, const std::string &body, const std::string &content_type, ContentReceiver content_receiver, DownloadProgress progress = nullptr);

  Result Patch(const std::string &path);
  Result Patch(const std::string &path, const char *body, size_t content_length, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const std::string &body, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, size_t content_length, ContentProvider content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, size_t content_length, ContentProvider content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, ContentProviderWithoutLength content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, ContentProviderWithoutLength content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Params &params);
  Result Patch(const std::string &path, const UploadFormDataItems &items, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers);
  Result Patch(const std::string &path, const Headers &headers, const char *body, size_t content_length, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, const std::string &body, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, size_t content_length, ContentProvider content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, size_t content_length, ContentProvider content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, ContentProviderWithoutLength content_provider, const std::string &content_type, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, ContentProviderWithoutLength content_provider, const std::string &content_type, ContentReceiver content_receiver, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, const Params &params);
  Result Patch(const std::string &path, const Headers &headers, const UploadFormDataItems &items, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, const UploadFormDataItems &items, const std::string &boundary, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, const UploadFormDataItems &items, const FormDataProviderItems &provider_items, UploadProgress progress = nullptr);
  Result Patch(const std::string &path, const Headers &headers, const std::string &body, const std::string &content_type, ContentReceiver content_receiver, DownloadProgress progress = nullptr);

  Result Delete(const std::string &path, DownloadProgress progress = nullptr);
  Result Delete(const std::string &path, const char *body, size_t content_length, const std::string &content_type, DownloadProgress progress = nullptr);
  Result Delete(const std::string &path, const std::string &body, const std::string &content_type, DownloadProgress progress = nullptr);
  Result Delete(const std::string &path, const Params &params, DownloadProgress progress = nullptr);
  Result Delete(const std::string &path, const Headers &headers, DownloadProgress progress = nullptr);
  Result Delete(const std::string &path, const Headers &headers, const char *body, size_t content_length, const std::string &content_type, DownloadProgress progress = nullptr);
  Result Delete(const std::string &path, const Headers &headers, const std::string &body, const std::string &content_type, DownloadProgress progress = nullptr);
  Result Delete(const std::string &path, const Headers &headers, const Params &params, DownloadProgress progress = nullptr);

  Result Options(const std::string &path);
  Result Options(const std::string &path, const Headers &headers);
  // clang-format on

  // Streaming API: Open a stream for reading response body incrementally
  // Socket ownership is transferred to StreamHandle for true streaming
  // Supports all HTTP methods (GET, POST, PUT, PATCH, DELETE, etc.)
  ClientImpl::StreamHandle open_stream(const std::string &method,
                                       const std::string &path,
                                       const Params &params = {},
                                       const Headers &headers = {},
                                       const std::string &body = {},
                                       const std::string &content_type = {});

  bool send(Request &req, Response &res, Error &error);
  Result send(const Request &req);

  void stop();

  std::string host() const;
  int port() const;

  size_t is_socket_open() const;
  socket_t socket() const;

  void set_hostname_addr_map(std::map<std::string, std::string> addr_map);

  void set_default_headers(Headers headers);

  void
  set_header_writer(std::function<ssize_t(Stream &, Headers &)> const &writer);

  void set_address_family(int family);
  void set_tcp_nodelay(bool on);
  void set_socket_options(SocketOptions socket_options);

  void set_connection_timeout(time_t sec, time_t usec = 0);
  template <class Rep, class Period>
  void
  set_connection_timeout(const std::chrono::duration<Rep, Period> &duration);

  void set_read_timeout(time_t sec, time_t usec = 0);
  template <class Rep, class Period>
  void set_read_timeout(const std::chrono::duration<Rep, Period> &duration);

  void set_write_timeout(time_t sec, time_t usec = 0);
  template <class Rep, class Period>
  void set_write_timeout(const std::chrono::duration<Rep, Period> &duration);

  void set_max_timeout(time_t msec);
  template <class Rep, class Period>
  void set_max_timeout(const std::chrono::duration<Rep, Period> &duration);

  void set_basic_auth(const std::string &username, const std::string &password);
  void set_bearer_token_auth(const std::string &token);

  void set_keep_alive(bool on);
  void set_follow_location(bool on);

  void set_path_encode(bool on);
  void set_url_encode(bool on);

  void set_compress(bool on);

  void set_decompress(bool on);

  void set_payload_max_length(size_t length);

  void set_interface(const std::string &intf);

  void set_proxy(const std::string &host, int port);
  void set_proxy_basic_auth(const std::string &username,
                            const std::string &password);
  void set_proxy_bearer_token_auth(const std::string &token);
  void set_logger(Logger logger);
  void set_error_logger(ErrorLogger error_logger);

private:
  std::unique_ptr<ClientImpl> cli_;

#ifdef CPPHTTPLIB_SSL_ENABLED
public:
  void set_digest_auth(const std::string &username,
                       const std::string &password);
  void set_proxy_digest_auth(const std::string &username,
                             const std::string &password);
  void enable_server_certificate_verification(bool enabled);
  void enable_server_hostname_verification(bool enabled);
  void set_ca_cert_path(const std::string &ca_cert_file_path,
                        const std::string &ca_cert_dir_path = std::string());

  void set_ca_cert_store(tls::ca_store_t ca_cert_store);
  void load_ca_cert_store(const char *ca_cert, std::size_t size);

  void set_server_certificate_verifier(tls::VerifyCallback verifier);

  void set_session_verifier(
      std::function<SSLVerifierResponse(tls::session_t)> verifier);

  tls::ctx_t tls_context() const;

#if defined(_WIN32) &&                                                         \
    !defined(CPPHTTPLIB_DISABLE_WINDOWS_AUTOMATIC_ROOT_CERTIFICATES_UPDATE)
  void enable_windows_certificate_verification(bool enabled);
#endif

private:
  bool is_ssl_ = false;
#endif

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
public:
  [[deprecated("Use tls_context() instead")]]
  SSL_CTX *ssl_context() const;

  [[deprecated("Use set_session_verifier(session_t) instead")]]
  void set_server_certificate_verifier(
      std::function<SSLVerifierResponse(SSL *ssl)> verifier);

  [[deprecated("Use Result::ssl_backend_error() instead")]]
  long get_verify_result() const;
#endif
};



} // namespace httplib

#endif // CPPHTTPLIB_CLIENT_H
