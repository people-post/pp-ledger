//
//  stream.h  (detail helpers, stream::, sse::)
//

#ifndef CPPHTTPLIB_STREAM_H
#define CPPHTTPLIB_STREAM_H

#include "ssl.h"

namespace httplib {

namespace detail {

template <typename T, typename U>
inline void duration_to_sec_and_usec(const T &duration, U callback) {
  auto sec = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
  auto usec = std::chrono::duration_cast<std::chrono::microseconds>(
                  duration - std::chrono::seconds(sec))
                  .count();
  callback(static_cast<time_t>(sec), static_cast<time_t>(usec));
}

template <size_t N> inline constexpr size_t str_len(const char (&)[N]) {
  return N - 1;
}

inline bool is_numeric(const std::string &str) {
  return !str.empty() &&
         std::all_of(str.cbegin(), str.cend(),
                     [](unsigned char c) { return std::isdigit(c); });
}

inline size_t get_header_value_u64(const Headers &headers,
                                   const std::string &key, size_t def,
                                   size_t id, bool &is_invalid_value) {
  is_invalid_value = false;
  auto rng = headers.equal_range(key);
  auto it = rng.first;
  std::advance(it, static_cast<ssize_t>(id));
  if (it != rng.second) {
    if (is_numeric(it->second)) {
      return std::strtoull(it->second.data(), nullptr, 10);
    } else {
      is_invalid_value = true;
    }
  }
  return def;
}

inline size_t get_header_value_u64(const Headers &headers,
                                   const std::string &key, size_t def,
                                   size_t id) {
  auto dummy = false;
  return get_header_value_u64(headers, key, def, id, dummy);
}

} // namespace detail

template <class Rep, class Period>
inline Server &
Server::set_read_timeout(const std::chrono::duration<Rep, Period> &duration) {
  detail::duration_to_sec_and_usec(
      duration, [&](time_t sec, time_t usec) { set_read_timeout(sec, usec); });
  return *this;
}

template <class Rep, class Period>
inline Server &
Server::set_write_timeout(const std::chrono::duration<Rep, Period> &duration) {
  detail::duration_to_sec_and_usec(
      duration, [&](time_t sec, time_t usec) { set_write_timeout(sec, usec); });
  return *this;
}

template <class Rep, class Period>
inline Server &
Server::set_idle_interval(const std::chrono::duration<Rep, Period> &duration) {
  detail::duration_to_sec_and_usec(
      duration, [&](time_t sec, time_t usec) { set_idle_interval(sec, usec); });
  return *this;
}

template <class Rep, class Period>
inline void ClientImpl::set_connection_timeout(
    const std::chrono::duration<Rep, Period> &duration) {
  detail::duration_to_sec_and_usec(duration, [&](time_t sec, time_t usec) {
    set_connection_timeout(sec, usec);
  });
}

template <class Rep, class Period>
inline void ClientImpl::set_read_timeout(
    const std::chrono::duration<Rep, Period> &duration) {
  detail::duration_to_sec_and_usec(
      duration, [&](time_t sec, time_t usec) { set_read_timeout(sec, usec); });
}

template <class Rep, class Period>
inline void ClientImpl::set_write_timeout(
    const std::chrono::duration<Rep, Period> &duration) {
  detail::duration_to_sec_and_usec(
      duration, [&](time_t sec, time_t usec) { set_write_timeout(sec, usec); });
}

template <class Rep, class Period>
inline void ClientImpl::set_max_timeout(
    const std::chrono::duration<Rep, Period> &duration) {
  auto msec =
      std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
  set_max_timeout(msec);
}

template <class Rep, class Period>
inline void Client::set_connection_timeout(
    const std::chrono::duration<Rep, Period> &duration) {
  cli_->set_connection_timeout(duration);
}

template <class Rep, class Period>
inline void
Client::set_read_timeout(const std::chrono::duration<Rep, Period> &duration) {
  cli_->set_read_timeout(duration);
}

template <class Rep, class Period>
inline void
Client::set_write_timeout(const std::chrono::duration<Rep, Period> &duration) {
  cli_->set_write_timeout(duration);
}

inline void Client::set_max_timeout(time_t msec) {
  cli_->set_max_timeout(msec);
}

template <class Rep, class Period>
inline void
Client::set_max_timeout(const std::chrono::duration<Rep, Period> &duration) {
  cli_->set_max_timeout(duration);
}

/*
 * Forward declarations and types that will be part of the .h file if split into
 * .h + .cc.
 */

std::string hosted_at(const std::string &hostname);

void hosted_at(const std::string &hostname, std::vector<std::string> &addrs);

// JavaScript-style URL encoding/decoding functions
std::string encode_uri_component(const std::string &value);
std::string encode_uri(const std::string &value);
std::string decode_uri_component(const std::string &value);
std::string decode_uri(const std::string &value);

// RFC 3986 compliant URL component encoding/decoding functions
std::string encode_path_component(const std::string &component);
std::string decode_path_component(const std::string &component);
std::string encode_query_component(const std::string &component,
                                   bool space_as_plus = true);
std::string decode_query_component(const std::string &component,
                                   bool plus_as_space = true);

std::string append_query_params(const std::string &path, const Params &params);

std::pair<std::string, std::string> make_range_header(const Ranges &ranges);

std::pair<std::string, std::string>
make_basic_authentication_header(const std::string &username,
                                 const std::string &password,
                                 bool is_proxy = false);

namespace detail {

#if defined(_WIN32)
inline std::wstring u8string_to_wstring(const char *s) {
  if (!s) { return std::wstring(); }

  auto len = static_cast<int>(strlen(s));
  if (!len) { return std::wstring(); }

  auto wlen = ::MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
  if (!wlen) { return std::wstring(); }

  std::wstring ws;
  ws.resize(wlen);
  wlen = ::MultiByteToWideChar(
      CP_UTF8, 0, s, len,
      const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(ws.data())), wlen);
  if (wlen != static_cast<int>(ws.size())) { ws.clear(); }
  return ws;
}
#endif

struct FileStat {
  FileStat(const std::string &path);
  bool is_file() const;
  bool is_dir() const;
  time_t mtime() const;
  size_t size() const;

private:
#if defined(_WIN32)
  struct _stat st_;
#else
  struct stat st_;
#endif
  int ret_ = -1;
};

std::string make_host_and_port_string(const std::string &host, int port,
                                      bool is_ssl);

std::string trim_copy(const std::string &s);

void divide(
    const char *data, std::size_t size, char d,
    std::function<void(const char *, std::size_t, const char *, std::size_t)>
        fn);

void divide(
    const std::string &str, char d,
    std::function<void(const char *, std::size_t, const char *, std::size_t)>
        fn);

void split(const char *b, const char *e, char d,
           std::function<void(const char *, const char *)> fn);

void split(const char *b, const char *e, char d, size_t m,
           std::function<void(const char *, const char *)> fn);

bool process_client_socket(
    socket_t sock, time_t read_timeout_sec, time_t read_timeout_usec,
    time_t write_timeout_sec, time_t write_timeout_usec,
    time_t max_timeout_msec,
    std::chrono::time_point<std::chrono::steady_clock> start_time,
    std::function<bool(Stream &)> callback);

socket_t create_client_socket(const std::string &host, const std::string &ip,
                              int port, int address_family, bool tcp_nodelay,
                              bool ipv6_v6only, SocketOptions socket_options,
                              time_t connection_timeout_sec,
                              time_t connection_timeout_usec,
                              time_t read_timeout_sec, time_t read_timeout_usec,
                              time_t write_timeout_sec,
                              time_t write_timeout_usec,
                              const std::string &intf, Error &error);

const char *get_header_value(const Headers &headers, const std::string &key,
                             const char *def, size_t id);

std::string params_to_query_str(const Params &params);

void parse_query_text(const char *data, std::size_t size, Params &params);

void parse_query_text(const std::string &s, Params &params);

bool parse_multipart_boundary(const std::string &content_type,
                              std::string &boundary);

bool parse_range_header(const std::string &s, Ranges &ranges);

bool parse_accept_header(const std::string &s,
                         std::vector<std::string> &content_types);

int close_socket(socket_t sock);

ssize_t send_socket(socket_t sock, const void *ptr, size_t size, int flags);

ssize_t read_socket(socket_t sock, void *ptr, size_t size, int flags);

enum class EncodingType { None = 0, Gzip, Brotli, Zstd };

EncodingType encoding_type(const Request &req, const Response &res);

class BufferStream final : public Stream {
public:
  BufferStream() = default;
  ~BufferStream() override = default;

  bool is_readable() const override;
  bool wait_readable() const override;
  bool wait_writable() const override;
  ssize_t read(char *ptr, size_t size) override;
  ssize_t write(const char *ptr, size_t size) override;
  void get_remote_ip_and_port(std::string &ip, int &port) const override;
  void get_local_ip_and_port(std::string &ip, int &port) const override;
  socket_t socket() const override;
  time_t duration() const override;

  const std::string &get_buffer() const;

private:
  std::string buffer;
  size_t position = 0;
};

class compressor {
public:
  virtual ~compressor() = default;

  typedef std::function<bool(const char *data, size_t data_len)> Callback;
  virtual bool compress(const char *data, size_t data_length, bool last,
                        Callback callback) = 0;
};

class decompressor {
public:
  virtual ~decompressor() = default;

  virtual bool is_valid() const = 0;

  typedef std::function<bool(const char *data, size_t data_len)> Callback;
  virtual bool decompress(const char *data, size_t data_length,
                          Callback callback) = 0;
};

class nocompressor final : public compressor {
public:
  ~nocompressor() override = default;

  bool compress(const char *data, size_t data_length, bool /*last*/,
                Callback callback) override;
};

#ifdef CPPHTTPLIB_ZLIB_SUPPORT
class gzip_compressor final : public compressor {
public:
  gzip_compressor();
  ~gzip_compressor() override;

  bool compress(const char *data, size_t data_length, bool last,
                Callback callback) override;

private:
  bool is_valid_ = false;
  z_stream strm_;
};

class gzip_decompressor final : public decompressor {
public:
  gzip_decompressor();
  ~gzip_decompressor() override;

  bool is_valid() const override;

  bool decompress(const char *data, size_t data_length,
                  Callback callback) override;

private:
  bool is_valid_ = false;
  z_stream strm_;
};
#endif

#ifdef CPPHTTPLIB_BROTLI_SUPPORT
class brotli_compressor final : public compressor {
public:
  brotli_compressor();
  ~brotli_compressor();

  bool compress(const char *data, size_t data_length, bool last,
                Callback callback) override;

private:
  BrotliEncoderState *state_ = nullptr;
};

class brotli_decompressor final : public decompressor {
public:
  brotli_decompressor();
  ~brotli_decompressor();

  bool is_valid() const override;

  bool decompress(const char *data, size_t data_length,
                  Callback callback) override;

private:
  BrotliDecoderResult decoder_r;
  BrotliDecoderState *decoder_s = nullptr;
};
#endif

#ifdef CPPHTTPLIB_ZSTD_SUPPORT
class zstd_compressor : public compressor {
public:
  zstd_compressor();
  ~zstd_compressor();

  bool compress(const char *data, size_t data_length, bool last,
                Callback callback) override;

private:
  ZSTD_CCtx *ctx_ = nullptr;
};

class zstd_decompressor : public decompressor {
public:
  zstd_decompressor();
  ~zstd_decompressor();

  bool is_valid() const override;

  bool decompress(const char *data, size_t data_length,
                  Callback callback) override;

private:
  ZSTD_DCtx *ctx_ = nullptr;
};
#endif

// NOTE: until the read size reaches `fixed_buffer_size`, use `fixed_buffer`
// to store data. The call can set memory on stack for performance.
class stream_line_reader {
public:
  stream_line_reader(Stream &strm, char *fixed_buffer,
                     size_t fixed_buffer_size);
  const char *ptr() const;
  size_t size() const;
  bool end_with_crlf() const;
  bool getline();

private:
  void append(char c);

  Stream &strm_;
  char *fixed_buffer_;
  const size_t fixed_buffer_size_;
  size_t fixed_buffer_used_size_ = 0;
  std::string growable_buffer_;
};

bool parse_trailers(stream_line_reader &line_reader, Headers &dest,
                    const Headers &src_headers, const Limits &limits = Limits{});

struct ChunkedDecoder {
  Stream &strm;
  size_t chunk_remaining = 0;
  bool finished = false;
  char line_buf[64];
  size_t last_chunk_total = 0;
  size_t last_chunk_offset = 0;

  explicit ChunkedDecoder(Stream &s);

  ssize_t read_payload(char *buf, size_t len, size_t &out_chunk_offset,
                       size_t &out_chunk_total);

  bool parse_trailers_into(Headers &dest, const Headers &src_headers,
                           const Limits &limits = Limits{});
};

class mmap {
public:
  mmap(const char *path);
  ~mmap();

  bool open(const char *path);
  void close();

  bool is_open() const;
  size_t size() const;
  const char *data() const;

private:
#if defined(_WIN32)
  HANDLE hFile_ = NULL;
  HANDLE hMapping_ = NULL;
#else
  int fd_ = -1;
#endif
  size_t size_ = 0;
  void *addr_ = nullptr;
  bool is_open_empty_file = false;
};

// NOTE: https://www.rfc-editor.org/rfc/rfc9110#section-5
namespace fields {

bool is_token_char(char c);
bool is_token(const std::string &s);
bool is_field_name(const std::string &s);
bool is_vchar(char c);
bool is_obs_text(char c);
bool is_field_vchar(char c);
bool is_field_content(const std::string &s);
bool is_field_value(const std::string &s);

} // namespace fields

int shutdown_socket(socket_t sock);

class SocketStream final : public Stream {
public:
  SocketStream(socket_t sock, time_t read_timeout_sec, time_t read_timeout_usec,
               time_t write_timeout_sec, time_t write_timeout_usec,
               time_t max_timeout_msec = 0,
               std::chrono::time_point<std::chrono::steady_clock> start_time =
                   (std::chrono::steady_clock::time_point::min)());
  ~SocketStream() override;

  bool is_readable() const override;
  bool wait_readable() const override;
  bool wait_writable() const override;
  ssize_t read(char *ptr, size_t size) override;
  ssize_t write(const char *ptr, size_t size) override;
  void get_remote_ip_and_port(std::string &ip, int &port) const override;
  void get_local_ip_and_port(std::string &ip, int &port) const override;
  socket_t socket() const override;
  time_t duration() const override;
  void set_read_timeout(time_t sec, time_t usec = 0) override;

private:
  socket_t sock_;
  time_t read_timeout_sec_;
  time_t read_timeout_usec_;
  time_t write_timeout_sec_;
  time_t write_timeout_usec_;
  time_t max_timeout_msec_;
  const std::chrono::time_point<std::chrono::steady_clock> start_time_;

  std::vector<char> read_buff_;
  size_t read_buff_off_ = 0;
  size_t read_buff_content_size_ = 0;

  static const size_t read_buff_size_ = 1024l * 4;
};

#ifdef CPPHTTPLIB_SSL_ENABLED
class SSLSocketStream final : public Stream {
public:
  SSLSocketStream(
      socket_t sock, tls::session_t session, time_t read_timeout_sec,
      time_t read_timeout_usec, time_t write_timeout_sec,
      time_t write_timeout_usec, time_t max_timeout_msec = 0,
      std::chrono::time_point<std::chrono::steady_clock> start_time =
          (std::chrono::steady_clock::time_point::min)());
  ~SSLSocketStream() override;

  bool is_readable() const override;
  bool wait_readable() const override;
  bool wait_writable() const override;
  ssize_t read(char *ptr, size_t size) override;
  ssize_t write(const char *ptr, size_t size) override;
  void get_remote_ip_and_port(std::string &ip, int &port) const override;
  void get_local_ip_and_port(std::string &ip, int &port) const override;
  socket_t socket() const override;
  time_t duration() const override;
  void set_read_timeout(time_t sec, time_t usec = 0) override;

private:
  socket_t sock_;
  tls::session_t session_;
  time_t read_timeout_sec_;
  time_t read_timeout_usec_;
  time_t write_timeout_sec_;
  time_t write_timeout_usec_;
  time_t max_timeout_msec_;
  const std::chrono::time_point<std::chrono::steady_clock> start_time_;
};
#endif

} // namespace detail

/*
 * TLS Abstraction Layer Declarations
 */

#ifdef CPPHTTPLIB_SSL_ENABLED
// TLS abstraction layer - backend-specific type declarations
#ifdef CPPHTTPLIB_MBEDTLS_SUPPORT
namespace tls {
namespace impl {

// Mbed TLS context wrapper (holds config, entropy, DRBG, CA chain, own
// cert/key). This struct is accessible via tls::impl for use in SSL context
// setup callbacks (cast ctx_t to tls::impl::MbedTlsContext*).
struct MbedTlsContext {
  mbedtls_ssl_config conf;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_x509_crt ca_chain;
  mbedtls_x509_crt own_cert;
  mbedtls_pk_context own_key;
  bool is_server = false;
  bool verify_client = false;
  bool has_verify_callback = false;

  MbedTlsContext();
  ~MbedTlsContext();

  MbedTlsContext(const MbedTlsContext &) = delete;
  MbedTlsContext &operator=(const MbedTlsContext &) = delete;
};

} // namespace impl
} // namespace tls
#endif

#ifdef CPPHTTPLIB_WOLFSSL_SUPPORT
namespace tls {
namespace impl {

// wolfSSL context wrapper (holds WOLFSSL_CTX and related state).
// This struct is accessible via tls::impl for use in SSL context
// setup callbacks (cast ctx_t to tls::impl::WolfSSLContext*).
struct WolfSSLContext {
  WOLFSSL_CTX *ctx = nullptr;
  bool is_server = false;
  bool verify_client = false;
  bool has_verify_callback = false;
  std::string ca_pem_data_; // accumulated PEM for get_ca_names/get_ca_certs

  WolfSSLContext();
  ~WolfSSLContext();

  WolfSSLContext(const WolfSSLContext &) = delete;
  WolfSSLContext &operator=(const WolfSSLContext &) = delete;
};

// CA store for wolfSSL: holds raw PEM bytes to allow reloading into any ctx
struct WolfSSLCAStore {
  std::string pem_data;
};

} // namespace impl
} // namespace tls
#endif

#endif // CPPHTTPLIB_SSL_ENABLED

namespace stream {

class Result {
public:
  Result();
  explicit Result(ClientImpl::StreamHandle &&handle, size_t chunk_size = 8192);
  Result(Result &&other) noexcept;
  Result &operator=(Result &&other) noexcept;
  Result(const Result &) = delete;
  Result &operator=(const Result &) = delete;

  // Response info
  bool is_valid() const;
  explicit operator bool() const;
  int status() const;
  const Headers &headers() const;
  std::string get_header_value(const std::string &key,
                               const char *def = "") const;
  bool has_header(const std::string &key) const;
  Error error() const;
  Error read_error() const;
  bool has_read_error() const;

  // Stream reading
  bool next();
  const char *data() const;
  size_t size() const;
  std::string read_all();

private:
  ClientImpl::StreamHandle handle_;
  std::string buffer_;
  size_t current_size_ = 0;
  size_t chunk_size_;
  bool finished_ = false;
};

// GET
template <typename ClientType>
inline Result Get(ClientType &cli, const std::string &path,
                  size_t chunk_size = 8192) {
  return Result{cli.open_stream("GET", path), chunk_size};
}

template <typename ClientType>
inline Result Get(ClientType &cli, const std::string &path,
                  const Headers &headers, size_t chunk_size = 8192) {
  return Result{cli.open_stream("GET", path, {}, headers), chunk_size};
}

template <typename ClientType>
inline Result Get(ClientType &cli, const std::string &path,
                  const Params &params, size_t chunk_size = 8192) {
  return Result{cli.open_stream("GET", path, params), chunk_size};
}

template <typename ClientType>
inline Result Get(ClientType &cli, const std::string &path,
                  const Params &params, const Headers &headers,
                  size_t chunk_size = 8192) {
  return Result{cli.open_stream("GET", path, params, headers), chunk_size};
}

// POST
template <typename ClientType>
inline Result Post(ClientType &cli, const std::string &path,
                   const std::string &body, const std::string &content_type,
                   size_t chunk_size = 8192) {
  return Result{cli.open_stream("POST", path, {}, {}, body, content_type),
                chunk_size};
}

template <typename ClientType>
inline Result Post(ClientType &cli, const std::string &path,
                   const Headers &headers, const std::string &body,
                   const std::string &content_type, size_t chunk_size = 8192) {
  return Result{cli.open_stream("POST", path, {}, headers, body, content_type),
                chunk_size};
}

template <typename ClientType>
inline Result Post(ClientType &cli, const std::string &path,
                   const Params &params, const std::string &body,
                   const std::string &content_type, size_t chunk_size = 8192) {
  return Result{cli.open_stream("POST", path, params, {}, body, content_type),
                chunk_size};
}

template <typename ClientType>
inline Result Post(ClientType &cli, const std::string &path,
                   const Params &params, const Headers &headers,
                   const std::string &body, const std::string &content_type,
                   size_t chunk_size = 8192) {
  return Result{
      cli.open_stream("POST", path, params, headers, body, content_type),
      chunk_size};
}

// PUT
template <typename ClientType>
inline Result Put(ClientType &cli, const std::string &path,
                  const std::string &body, const std::string &content_type,
                  size_t chunk_size = 8192) {
  return Result{cli.open_stream("PUT", path, {}, {}, body, content_type),
                chunk_size};
}

template <typename ClientType>
inline Result Put(ClientType &cli, const std::string &path,
                  const Headers &headers, const std::string &body,
                  const std::string &content_type, size_t chunk_size = 8192) {
  return Result{cli.open_stream("PUT", path, {}, headers, body, content_type),
                chunk_size};
}

template <typename ClientType>
inline Result Put(ClientType &cli, const std::string &path,
                  const Params &params, const std::string &body,
                  const std::string &content_type, size_t chunk_size = 8192) {
  return Result{cli.open_stream("PUT", path, params, {}, body, content_type),
                chunk_size};
}

template <typename ClientType>
inline Result Put(ClientType &cli, const std::string &path,
                  const Params &params, const Headers &headers,
                  const std::string &body, const std::string &content_type,
                  size_t chunk_size = 8192) {
  return Result{
      cli.open_stream("PUT", path, params, headers, body, content_type),
      chunk_size};
}

// PATCH
template <typename ClientType>
inline Result Patch(ClientType &cli, const std::string &path,
                    const std::string &body, const std::string &content_type,
                    size_t chunk_size = 8192) {
  return Result{cli.open_stream("PATCH", path, {}, {}, body, content_type),
                chunk_size};
}

template <typename ClientType>
inline Result Patch(ClientType &cli, const std::string &path,
                    const Headers &headers, const std::string &body,
                    const std::string &content_type, size_t chunk_size = 8192) {
  return Result{cli.open_stream("PATCH", path, {}, headers, body, content_type),
                chunk_size};
}

template <typename ClientType>
inline Result Patch(ClientType &cli, const std::string &path,
                    const Params &params, const std::string &body,
                    const std::string &content_type, size_t chunk_size = 8192) {
  return Result{cli.open_stream("PATCH", path, params, {}, body, content_type),
                chunk_size};
}

template <typename ClientType>
inline Result Patch(ClientType &cli, const std::string &path,
                    const Params &params, const Headers &headers,
                    const std::string &body, const std::string &content_type,
                    size_t chunk_size = 8192) {
  return Result{
      cli.open_stream("PATCH", path, params, headers, body, content_type),
      chunk_size};
}

// DELETE
template <typename ClientType>
inline Result Delete(ClientType &cli, const std::string &path,
                     size_t chunk_size = 8192) {
  return Result{cli.open_stream("DELETE", path), chunk_size};
}

template <typename ClientType>
inline Result Delete(ClientType &cli, const std::string &path,
                     const Headers &headers, size_t chunk_size = 8192) {
  return Result{cli.open_stream("DELETE", path, {}, headers), chunk_size};
}

template <typename ClientType>
inline Result Delete(ClientType &cli, const std::string &path,
                     const std::string &body, const std::string &content_type,
                     size_t chunk_size = 8192) {
  return Result{cli.open_stream("DELETE", path, {}, {}, body, content_type),
                chunk_size};
}

template <typename ClientType>
inline Result Delete(ClientType &cli, const std::string &path,
                     const Headers &headers, const std::string &body,
                     const std::string &content_type,
                     size_t chunk_size = 8192) {
  return Result{
      cli.open_stream("DELETE", path, {}, headers, body, content_type),
      chunk_size};
}

template <typename ClientType>
inline Result Delete(ClientType &cli, const std::string &path,
                     const Params &params, size_t chunk_size = 8192) {
  return Result{cli.open_stream("DELETE", path, params), chunk_size};
}

template <typename ClientType>
inline Result Delete(ClientType &cli, const std::string &path,
                     const Params &params, const Headers &headers,
                     size_t chunk_size = 8192) {
  return Result{cli.open_stream("DELETE", path, params, headers), chunk_size};
}

template <typename ClientType>
inline Result Delete(ClientType &cli, const std::string &path,
                     const Params &params, const std::string &body,
                     const std::string &content_type,
                     size_t chunk_size = 8192) {
  return Result{cli.open_stream("DELETE", path, params, {}, body, content_type),
                chunk_size};
}

template <typename ClientType>
inline Result Delete(ClientType &cli, const std::string &path,
                     const Params &params, const Headers &headers,
                     const std::string &body, const std::string &content_type,
                     size_t chunk_size = 8192) {
  return Result{
      cli.open_stream("DELETE", path, params, headers, body, content_type),
      chunk_size};
}

// HEAD
template <typename ClientType>
inline Result Head(ClientType &cli, const std::string &path,
                   size_t chunk_size = 8192) {
  return Result{cli.open_stream("HEAD", path), chunk_size};
}

template <typename ClientType>
inline Result Head(ClientType &cli, const std::string &path,
                   const Headers &headers, size_t chunk_size = 8192) {
  return Result{cli.open_stream("HEAD", path, {}, headers), chunk_size};
}

template <typename ClientType>
inline Result Head(ClientType &cli, const std::string &path,
                   const Params &params, size_t chunk_size = 8192) {
  return Result{cli.open_stream("HEAD", path, params), chunk_size};
}

template <typename ClientType>
inline Result Head(ClientType &cli, const std::string &path,
                   const Params &params, const Headers &headers,
                   size_t chunk_size = 8192) {
  return Result{cli.open_stream("HEAD", path, params, headers), chunk_size};
}

// OPTIONS
template <typename ClientType>
inline Result Options(ClientType &cli, const std::string &path,
                      size_t chunk_size = 8192) {
  return Result{cli.open_stream("OPTIONS", path), chunk_size};
}

template <typename ClientType>
inline Result Options(ClientType &cli, const std::string &path,
                      const Headers &headers, size_t chunk_size = 8192) {
  return Result{cli.open_stream("OPTIONS", path, {}, headers), chunk_size};
}

template <typename ClientType>
inline Result Options(ClientType &cli, const std::string &path,
                      const Params &params, size_t chunk_size = 8192) {
  return Result{cli.open_stream("OPTIONS", path, params), chunk_size};
}

template <typename ClientType>
inline Result Options(ClientType &cli, const std::string &path,
                      const Params &params, const Headers &headers,
                      size_t chunk_size = 8192) {
  return Result{cli.open_stream("OPTIONS", path, params, headers), chunk_size};
}

} // namespace stream

namespace sse {

struct SSEMessage {
  std::string event; // Event type (default: "message")
  std::string data;  // Event payload
  std::string id;    // Event ID for Last-Event-ID header

  SSEMessage();
  void clear();
};

class SSEClient {
public:
  using MessageHandler = std::function<void(const SSEMessage &)>;
  using ErrorHandler = std::function<void(Error)>;
  using OpenHandler = std::function<void()>;

  SSEClient(Client &client, const std::string &path);
  SSEClient(Client &client, const std::string &path, const Headers &headers);
  ~SSEClient();

  SSEClient(const SSEClient &) = delete;
  SSEClient &operator=(const SSEClient &) = delete;

  // Event handlers
  SSEClient &on_message(MessageHandler handler);
  SSEClient &on_event(const std::string &type, MessageHandler handler);
  SSEClient &on_open(OpenHandler handler);
  SSEClient &on_error(ErrorHandler handler);
  SSEClient &set_reconnect_interval(int ms);
  SSEClient &set_max_reconnect_attempts(int n);

  // State accessors
  bool is_connected() const;
  const std::string &last_event_id() const;

  // Blocking start - runs event loop with auto-reconnect
  void start();

  // Non-blocking start - runs in background thread
  void start_async();

  // Stop the client (thread-safe)
  void stop();

private:
  bool parse_sse_line(const std::string &line, SSEMessage &msg, int &retry_ms);
  void run_event_loop();
  void dispatch_event(const SSEMessage &msg);
  bool should_reconnect(int count) const;
  void wait_for_reconnect();

  // Client and path
  Client &client_;
  std::string path_;
  Headers headers_;

  // Callbacks
  MessageHandler on_message_;
  std::map<std::string, MessageHandler> event_handlers_;
  OpenHandler on_open_;
  ErrorHandler on_error_;

  // Configuration
  int reconnect_interval_ms_ = 3000;
  int max_reconnect_attempts_ = 0; // 0 = unlimited

  // State
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::string last_event_id_;

  // Async support
  std::thread async_thread_;
};

} // namespace sse


} // namespace httplib

#endif // CPPHTTPLIB_STREAM_H
