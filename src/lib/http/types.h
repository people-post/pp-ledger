//
//  types.h
//

#ifndef CPPHTTPLIB_TYPES_H
#define CPPHTTPLIB_TYPES_H

#include "platform.h"

namespace httplib {

namespace ws {
class WebSocket;
} // namespace ws

namespace detail {

/*
 * Backport std::make_unique from C++14.
 *
 * NOTE: This code came up with the following stackoverflow post:
 * https://stackoverflow.com/questions/10149840/c-arrays-and-make-unique
 *
 */

template <class T, class... Args>
typename std::enable_if<!std::is_array<T>::value, std::unique_ptr<T>>::type
make_unique(Args &&...args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

template <class T>
typename std::enable_if<std::is_array<T>::value, std::unique_ptr<T>>::type
make_unique(std::size_t n) {
  typedef typename std::remove_extent<T>::type RT;
  return std::unique_ptr<T>(new RT[n]);
}

namespace case_ignore {

inline unsigned char to_lower(int c) {
  const static unsigned char table[256] = {
      0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
      15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
      30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
      45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
      60,  61,  62,  63,  64,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106,
      107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121,
      122, 91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104,
      105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
      120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134,
      135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
      150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164,
      165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179,
      180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 224, 225, 226,
      227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241,
      242, 243, 244, 245, 246, 215, 248, 249, 250, 251, 252, 253, 254, 223, 224,
      225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
      240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254,
      255,
  };
  return table[(unsigned char)(char)c];
}

inline bool equal(const std::string &a, const std::string &b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char ca, char cb) {
           return to_lower(ca) == to_lower(cb);
         });
}

struct equal_to {
  bool operator()(const std::string &a, const std::string &b) const {
    return equal(a, b);
  }
};

struct hash {
  size_t operator()(const std::string &key) const {
    return hash_core(key.data(), key.size(), 0);
  }

  size_t hash_core(const char *s, size_t l, size_t h) const {
    return (l == 0) ? h
                    : hash_core(s + 1, l - 1,
                                // Unsets the 6 high bits of h, therefore no
                                // overflow happens
                                (((std::numeric_limits<size_t>::max)() >> 6) &
                                 h * 33) ^
                                    static_cast<unsigned char>(to_lower(*s)));
  }
};

template <typename T>
using unordered_set = std::unordered_set<T, detail::case_ignore::hash,
                                         detail::case_ignore::equal_to>;

} // namespace case_ignore

// This is based on
// "http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2014/n4189".

struct scope_exit {
  explicit scope_exit(std::function<void(void)> &&f)
      : exit_function(std::move(f)), execute_on_destruction{true} {}

  scope_exit(scope_exit &&rhs) noexcept
      : exit_function(std::move(rhs.exit_function)),
        execute_on_destruction{rhs.execute_on_destruction} {
    rhs.release();
  }

  ~scope_exit() {
    if (execute_on_destruction) { this->exit_function(); }
  }

  void release() { this->execute_on_destruction = false; }

private:
  scope_exit(const scope_exit &) = delete;
  void operator=(const scope_exit &) = delete;
  scope_exit &operator=(scope_exit &&) = delete;

  std::function<void(void)> exit_function;
  bool execute_on_destruction;
};

// Simple from_chars implementation for integer and double types (C++17
// substitute)
template <typename T> struct from_chars_result {
  const char *ptr;
  std::errc ec;
};

template <typename T>
inline from_chars_result<T> from_chars(const char *first, const char *last,
                                       T &value, int base = 10) {
  value = 0;
  const char *p = first;
  bool negative = false;

  if (p != last && *p == '-') {
    negative = true;
    ++p;
  }
  if (p == last) { return {first, std::errc::invalid_argument}; }

  T result = 0;
  for (; p != last; ++p) {
    char c = *p;
    int digit = -1;
    if ('0' <= c && c <= '9') {
      digit = c - '0';
    } else if ('a' <= c && c <= 'z') {
      digit = c - 'a' + 10;
    } else if ('A' <= c && c <= 'Z') {
      digit = c - 'A' + 10;
    } else {
      break;
    }

    if (digit < 0 || digit >= base) { break; }
    if (result > ((std::numeric_limits<T>::max)() - digit) / base) {
      return {p, std::errc::result_out_of_range};
    }
    result = result * base + digit;
  }

  if (p == first || (negative && p == first + 1)) {
    return {first, std::errc::invalid_argument};
  }

  value = negative ? -result : result;
  return {p, std::errc{}};
}

// from_chars for double (simple wrapper for strtod)
inline from_chars_result<double> from_chars(const char *first, const char *last,
                                            double &value) {
  std::string s(first, last);
  char *endptr = nullptr;
  errno = 0;
  value = std::strtod(s.c_str(), &endptr);
  if (endptr == s.c_str()) { return {first, std::errc::invalid_argument}; }
  if (errno == ERANGE) {
    return {first + (endptr - s.c_str()), std::errc::result_out_of_range};
  }
  return {first + (endptr - s.c_str()), std::errc{}};
}

} // namespace detail

enum SSLVerifierResponse {
  // no decision has been made, use the built-in certificate verifier
  NoDecisionMade,
  // connection certificate is verified and accepted
  CertificateAccepted,
  // connection certificate was processed but is rejected
  CertificateRejected
};

enum StatusCode {
  // Information responses
  Continue_100 = 100,
  SwitchingProtocol_101 = 101,
  Processing_102 = 102,
  EarlyHints_103 = 103,

  // Successful responses
  OK_200 = 200,
  Created_201 = 201,
  Accepted_202 = 202,
  NonAuthoritativeInformation_203 = 203,
  NoContent_204 = 204,
  ResetContent_205 = 205,
  PartialContent_206 = 206,
  MultiStatus_207 = 207,
  AlreadyReported_208 = 208,
  IMUsed_226 = 226,

  // Redirection messages
  MultipleChoices_300 = 300,
  MovedPermanently_301 = 301,
  Found_302 = 302,
  SeeOther_303 = 303,
  NotModified_304 = 304,
  UseProxy_305 = 305,
  unused_306 = 306,
  TemporaryRedirect_307 = 307,
  PermanentRedirect_308 = 308,

  // Client error responses
  BadRequest_400 = 400,
  Unauthorized_401 = 401,
  PaymentRequired_402 = 402,
  Forbidden_403 = 403,
  NotFound_404 = 404,
  MethodNotAllowed_405 = 405,
  NotAcceptable_406 = 406,
  ProxyAuthenticationRequired_407 = 407,
  RequestTimeout_408 = 408,
  Conflict_409 = 409,
  Gone_410 = 410,
  LengthRequired_411 = 411,
  PreconditionFailed_412 = 412,
  PayloadTooLarge_413 = 413,
  UriTooLong_414 = 414,
  UnsupportedMediaType_415 = 415,
  RangeNotSatisfiable_416 = 416,
  ExpectationFailed_417 = 417,
  ImATeapot_418 = 418,
  MisdirectedRequest_421 = 421,
  UnprocessableContent_422 = 422,
  Locked_423 = 423,
  FailedDependency_424 = 424,
  TooEarly_425 = 425,
  UpgradeRequired_426 = 426,
  PreconditionRequired_428 = 428,
  TooManyRequests_429 = 429,
  RequestHeaderFieldsTooLarge_431 = 431,
  UnavailableForLegalReasons_451 = 451,

  // Server error responses
  InternalServerError_500 = 500,
  NotImplemented_501 = 501,
  BadGateway_502 = 502,
  ServiceUnavailable_503 = 503,
  GatewayTimeout_504 = 504,
  HttpVersionNotSupported_505 = 505,
  VariantAlsoNegotiates_506 = 506,
  InsufficientStorage_507 = 507,
  LoopDetected_508 = 508,
  NotExtended_510 = 510,
  NetworkAuthenticationRequired_511 = 511,
};

using Headers =
    std::unordered_multimap<std::string, std::string, detail::case_ignore::hash,
                            detail::case_ignore::equal_to>;

using Params = std::multimap<std::string, std::string>;
using Match = std::smatch;

using DownloadProgress = std::function<bool(size_t current, size_t total)>;
using UploadProgress = std::function<bool(size_t current, size_t total)>;


#if __cplusplus >= 201703L

using any = std::any;
using bad_any_cast = std::bad_any_cast;

template <typename T> T any_cast(const any &a) { return std::any_cast<T>(a); }
template <typename T> T any_cast(any &a) { return std::any_cast<T>(a); }
template <typename T> T any_cast(any &&a) {
  return std::any_cast<T>(std::move(a));
}
template <typename T> const T *any_cast(const any *a) noexcept {
  return std::any_cast<T>(a);
}
template <typename T> T *any_cast(any *a) noexcept {
  return std::any_cast<T>(a);
}

#else // C++11/14 implementation

class bad_any_cast : public std::bad_cast {
public:
  const char *what() const noexcept override { return "bad any_cast"; }
};

namespace detail {

using any_type_id = const void *;

// Returns a unique per-type ID without RTTI.
// The static address is stable across TUs because function templates are
// implicitly inline and the ODR merges their statics into one.
template <typename T> any_type_id any_typeid() noexcept {
  static const char id = 0;
  return &id;
}

struct any_storage {
  virtual ~any_storage() = default;
  virtual std::unique_ptr<any_storage> clone() const = 0;
  virtual any_type_id type_id() const noexcept = 0;
};

template <typename T> struct any_value final : any_storage {
  T value;
  template <typename U> explicit any_value(U &&v) : value(std::forward<U>(v)) {}
  std::unique_ptr<any_storage> clone() const override {
    return std::unique_ptr<any_storage>(new any_value<T>(value));
  }
  any_type_id type_id() const noexcept override { return any_typeid<T>(); }
};

} // namespace detail

class any {
  std::unique_ptr<detail::any_storage> storage_;

public:
  any() noexcept = default;
  any(const any &o) : storage_(o.storage_ ? o.storage_->clone() : nullptr) {}
  any(any &&) noexcept = default;
  any &operator=(const any &o) {
    storage_ = o.storage_ ? o.storage_->clone() : nullptr;
    return *this;
  }
  any &operator=(any &&) noexcept = default;

  template <
      typename T, typename D = typename std::decay<T>::type,
      typename std::enable_if<!std::is_same<D, any>::value, int>::type = 0>
  any(T &&v) : storage_(new detail::any_value<D>(std::forward<T>(v))) {}

  template <
      typename T, typename D = typename std::decay<T>::type,
      typename std::enable_if<!std::is_same<D, any>::value, int>::type = 0>
  any &operator=(T &&v) {
    storage_.reset(new detail::any_value<D>(std::forward<T>(v)));
    return *this;
  }

  bool has_value() const noexcept { return storage_ != nullptr; }
  void reset() noexcept { storage_.reset(); }

  template <typename T> friend T *any_cast(any *a) noexcept;
  template <typename T> friend const T *any_cast(const any *a) noexcept;
};

template <typename T> T *any_cast(any *a) noexcept {
  if (!a || !a->storage_) { return nullptr; }
  if (a->storage_->type_id() != detail::any_typeid<T>()) { return nullptr; }
  return &static_cast<detail::any_value<T> *>(a->storage_.get())->value;
}

template <typename T> const T *any_cast(const any *a) noexcept {
  if (!a || !a->storage_) { return nullptr; }
  if (a->storage_->type_id() != detail::any_typeid<T>()) { return nullptr; }
  return &static_cast<const detail::any_value<T> *>(a->storage_.get())->value;
}

template <typename T> T any_cast(const any &a) {
  using U =
      typename std::remove_cv<typename std::remove_reference<T>::type>::type;
  const U *p = any_cast<U>(&a);
  if (!p) { throw bad_any_cast{}; }
  return static_cast<T>(*p);
}

template <typename T> T any_cast(any &a) {
  using U =
      typename std::remove_cv<typename std::remove_reference<T>::type>::type;
  U *p = any_cast<U>(&a);
  if (!p) { throw bad_any_cast{}; }
  return static_cast<T>(*p);
}

template <typename T> T any_cast(any &&a) {
  using U =
      typename std::remove_cv<typename std::remove_reference<T>::type>::type;
  U *p = any_cast<U>(&a);
  if (!p) { throw bad_any_cast{}; }
  return static_cast<T>(std::move(*p));
}

#endif // __cplusplus >= 201703L

struct Response;
using ResponseHandler = std::function<bool(const Response &response)>;

struct FormData {
  std::string name;
  std::string content;
  std::string filename;
  std::string content_type;
  Headers headers;
};

struct FormField {
  std::string name;
  std::string content;
  Headers headers;
};
using FormFields = std::multimap<std::string, FormField>;

using FormFiles = std::multimap<std::string, FormData>;

struct MultipartFormData {
  FormFields fields; // Text fields from multipart
  FormFiles files;   // Files from multipart

  // Text field access
  std::string get_field(const std::string &key, size_t id = 0) const;
  std::vector<std::string> get_fields(const std::string &key) const;
  bool has_field(const std::string &key) const;
  size_t get_field_count(const std::string &key) const;

  // File access
  FormData get_file(const std::string &key, size_t id = 0) const;
  std::vector<FormData> get_files(const std::string &key) const;
  bool has_file(const std::string &key) const;
  size_t get_file_count(const std::string &key) const;
};

struct UploadFormData {
  std::string name;
  std::string content;
  std::string filename;
  std::string content_type;
};
using UploadFormDataItems = std::vector<UploadFormData>;

class DataSink {
public:
  DataSink() : os(&sb_), sb_(*this) {}

  DataSink(const DataSink &) = delete;
  DataSink &operator=(const DataSink &) = delete;
  DataSink(DataSink &&) = delete;
  DataSink &operator=(DataSink &&) = delete;

  std::function<bool(const char *data, size_t data_len)> write;
  std::function<bool()> is_writable;
  std::function<void()> done;
  std::function<void(const Headers &trailer)> done_with_trailer;
  std::ostream os;

private:
  class data_sink_streambuf final : public std::streambuf {
  public:
    explicit data_sink_streambuf(DataSink &sink) : sink_(sink) {}

  protected:
    std::streamsize xsputn(const char *s, std::streamsize n) override {
      sink_.write(s, static_cast<size_t>(n));
      return n;
    }

  private:
    DataSink &sink_;
  };

  data_sink_streambuf sb_;
};

using ContentProvider =
    std::function<bool(size_t offset, size_t length, DataSink &sink)>;

using ContentProviderWithoutLength =
    std::function<bool(size_t offset, DataSink &sink)>;

using ContentProviderResourceReleaser = std::function<void(bool success)>;

struct FormDataProvider {
  std::string name;
  ContentProviderWithoutLength provider;
  std::string filename;
  std::string content_type;
};
using FormDataProviderItems = std::vector<FormDataProvider>;

using ContentReceiverWithProgress = std::function<bool(
    const char *data, size_t data_length, size_t offset, size_t total_length)>;

using ContentReceiver =
    std::function<bool(const char *data, size_t data_length)>;

using FormDataHeader = std::function<bool(const FormData &file)>;

class ContentReader {
public:
  using Reader = std::function<bool(ContentReceiver receiver)>;
  using FormDataReader =
      std::function<bool(FormDataHeader header, ContentReceiver receiver)>;

  ContentReader(Reader reader, FormDataReader multipart_reader)
      : reader_(std::move(reader)),
        formdata_reader_(std::move(multipart_reader)) {}

  bool operator()(FormDataHeader header, ContentReceiver receiver) const {
    return formdata_reader_(std::move(header), std::move(receiver));
  }

  bool operator()(ContentReceiver receiver) const {
    return reader_(std::move(receiver));
  }

  Reader reader_;
  FormDataReader formdata_reader_;
};

using Range = std::pair<ssize_t, ssize_t>;
using Ranges = std::vector<Range>;

#ifdef CPPHTTPLIB_SSL_ENABLED
// TLS abstraction layer - public type definitions and API
namespace tls {

// Opaque handles (defined as void* for abstraction)
using ctx_t = void *;
using session_t = void *;
using const_session_t = const void *; // For read-only session access
using cert_t = void *;
using ca_store_t = void *;

// TLS versions
enum class Version {
  TLS1_2 = 0x0303,
  TLS1_3 = 0x0304,
};

// Subject Alternative Names (SAN) entry types
enum class SanType { DNS, IP, EMAIL, URI, OTHER };

// SAN entry structure
struct SanEntry {
  SanType type;
  std::string value;
};

// Verification context for certificate verification callback
struct VerifyContext {
  session_t session;        // TLS session handle
  cert_t cert;              // Current certificate being verified
  int depth;                // Certificate chain depth (0 = leaf)
  bool preverify_ok;        // OpenSSL/Mbed TLS pre-verification result
  long error_code;          // Backend-specific error code (0 = no error)
  const char *error_string; // Human-readable error description

  // Certificate introspection methods
  std::string subject_cn() const;
  std::string issuer_name() const;
  bool check_hostname(const char *hostname) const;
  std::vector<SanEntry> sans() const;
  bool validity(time_t &not_before, time_t &not_after) const;
  std::string serial() const;
};

using VerifyCallback = std::function<bool(const VerifyContext &ctx)>;

// TlsError codes for TLS operations (backend-independent)
enum class ErrorCode : int {
  Success = 0,
  WantRead,         // Non-blocking: need to wait for read
  WantWrite,        // Non-blocking: need to wait for write
  PeerClosed,       // Peer closed the connection
  Fatal,            // Unrecoverable error
  SyscallError,     // System call error (check sys_errno)
  CertVerifyFailed, // Certificate verification failed
  HostnameMismatch, // Hostname verification failed
};

// TLS error information
struct TlsError {
  ErrorCode code = ErrorCode::Fatal;
  uint64_t backend_code = 0; // OpenSSL: ERR_get_error(), mbedTLS: return value
  int sys_errno = 0;         // errno when SyscallError

  // Convert verification error code to human-readable string
  static std::string verify_error_to_string(long error_code);
};

// RAII wrapper for peer certificate
class PeerCert {
public:
  PeerCert();
  PeerCert(PeerCert &&other) noexcept;
  PeerCert &operator=(PeerCert &&other) noexcept;
  ~PeerCert();

  PeerCert(const PeerCert &) = delete;
  PeerCert &operator=(const PeerCert &) = delete;

  explicit operator bool() const;
  std::string subject_cn() const;
  std::string issuer_name() const;
  bool check_hostname(const char *hostname) const;
  std::vector<SanEntry> sans() const;
  bool validity(time_t &not_before, time_t &not_after) const;
  std::string serial() const;

private:
  explicit PeerCert(cert_t cert);
  cert_t cert_ = nullptr;
  friend PeerCert get_peer_cert_from_session(const_session_t session);
};

// Callback for TLS context setup (used by SSLServer constructor)
using ContextSetupCallback = std::function<bool(ctx_t ctx)>;

} // namespace tls
#endif

struct Request {
  std::string method;
  std::string path;
  std::string matched_route;
  Params params;
  Headers headers;
  Headers trailers;
  std::string body;

  std::string remote_addr;
  int remote_port = -1;
  std::string local_addr;
  int local_port = -1;

  // for server
  std::string version;
  std::string target;
  MultipartFormData form;
  Ranges ranges;
  Match matches;
  std::unordered_map<std::string, std::string> path_params;
  std::function<bool()> is_connection_closed = []() { return true; };

  // for client
  std::vector<std::string> accept_content_types;
  ResponseHandler response_handler;
  ContentReceiverWithProgress content_receiver;
  DownloadProgress download_progress;
  UploadProgress upload_progress;

  bool has_header(const std::string &key) const;
  std::string get_header_value(const std::string &key, const char *def = "",
                               size_t id = 0) const;
  size_t get_header_value_u64(const std::string &key, size_t def = 0,
                              size_t id = 0) const;
  size_t get_header_value_count(const std::string &key) const;
  void set_header(const std::string &key, const std::string &val);

  bool has_trailer(const std::string &key) const;
  std::string get_trailer_value(const std::string &key, size_t id = 0) const;
  size_t get_trailer_value_count(const std::string &key) const;

  bool has_param(const std::string &key) const;
  std::string get_param_value(const std::string &key, size_t id = 0) const;
  size_t get_param_value_count(const std::string &key) const;

  bool is_multipart_form_data() const;

  // private members...
  size_t redirect_count_ = Limits{}.redirect_max_count;
  size_t content_length_ = 0;
  ContentProvider content_provider_;
  bool is_chunked_content_provider_ = false;
  size_t authorization_count_ = 0;
  std::chrono::time_point<std::chrono::steady_clock> start_time_ =
      (std::chrono::steady_clock::time_point::min)();

#ifdef CPPHTTPLIB_SSL_ENABLED
  tls::const_session_t ssl = nullptr;
  tls::PeerCert peer_cert() const;
  std::string sni() const;
#endif
};

struct Response {
  std::string version;
  int status = -1;
  std::string reason;
  Headers headers;
  Headers trailers;
  std::string body;
  std::string location; // Redirect location

  // User-defined context — set by pre-routing/pre-request handlers and read
  // by route handlers to pass arbitrary data (e.g. decoded auth tokens).
  std::map<std::string, any> user_data;

  bool has_header(const std::string &key) const;
  std::string get_header_value(const std::string &key, const char *def = "",
                               size_t id = 0) const;
  size_t get_header_value_u64(const std::string &key, size_t def = 0,
                              size_t id = 0) const;
  size_t get_header_value_count(const std::string &key) const;
  void set_header(const std::string &key, const std::string &val);

  bool has_trailer(const std::string &key) const;
  std::string get_trailer_value(const std::string &key, size_t id = 0) const;
  size_t get_trailer_value_count(const std::string &key) const;

  void set_redirect(const std::string &url, int status = StatusCode::Found_302);
  void set_content(const char *s, size_t n, const std::string &content_type);
  void set_content(const std::string &s, const std::string &content_type);
  void set_content(std::string &&s, const std::string &content_type);

  void set_content_provider(
      size_t length, const std::string &content_type, ContentProvider provider,
      ContentProviderResourceReleaser resource_releaser = nullptr);

  void set_content_provider(
      const std::string &content_type, ContentProviderWithoutLength provider,
      ContentProviderResourceReleaser resource_releaser = nullptr);

  void set_chunked_content_provider(
      const std::string &content_type, ContentProviderWithoutLength provider,
      ContentProviderResourceReleaser resource_releaser = nullptr);

  void set_file_content(const std::string &path,
                        const std::string &content_type);
  void set_file_content(const std::string &path);

  Response() = default;
  Response(const Response &) = default;
  Response &operator=(const Response &) = default;
  Response(Response &&) = default;
  Response &operator=(Response &&) = default;
  ~Response() {
    if (content_provider_resource_releaser_) {
      content_provider_resource_releaser_(content_provider_success_);
    }
  }

  // private members...
  size_t content_length_ = 0;
  ContentProvider content_provider_;
  ContentProviderResourceReleaser content_provider_resource_releaser_;
  bool is_chunked_content_provider_ = false;
  bool content_provider_success_ = false;
  std::string file_content_path_;
  std::string file_content_content_type_;
};

enum class Error {
  Success = 0,
  Unknown,
  Connection,
  BindIPAddress,
  Read,
  Write,
  ExceedRedirectCount,
  Canceled,
  SSLConnection,
  SSLLoadingCerts,
  SSLServerVerification,
  SSLServerHostnameVerification,
  UnsupportedMultipartBoundaryChars,
  Compression,
  ConnectionTimeout,
  ProxyConnection,
  ConnectionClosed,
  Timeout,
  ResourceExhaustion,
  TooManyFormDataFiles,
  ExceedMaxPayloadSize,
  ExceedUriMaxLength,
  ExceedMaxSocketDescriptorCount,
  InvalidRequestLine,
  InvalidHTTPMethod,
  InvalidHTTPVersion,
  InvalidHeaders,
  MultipartParsing,
  OpenFile,
  Listen,
  GetSockName,
  UnsupportedAddressFamily,
  HTTPParsing,
  InvalidRangeHeader,

  // For internal use only
  SSLPeerCouldBeClosed_,
};

std::string to_string(Error error);

std::ostream &operator<<(std::ostream &os, const Error &obj);

class Stream {
public:
  virtual ~Stream() = default;

  virtual bool is_readable() const = 0;
  virtual bool wait_readable() const = 0;
  virtual bool wait_writable() const = 0;

  virtual ssize_t read(char *ptr, size_t size) = 0;
  virtual ssize_t write(const char *ptr, size_t size) = 0;
  virtual void get_remote_ip_and_port(std::string &ip, int &port) const = 0;
  virtual void get_local_ip_and_port(std::string &ip, int &port) const = 0;
  virtual socket_t socket() const = 0;

  virtual time_t duration() const = 0;

  virtual void set_read_timeout(time_t sec, time_t usec = 0) {
    (void)sec;
    (void)usec;
  }

  ssize_t write(const char *ptr);
  ssize_t write(const std::string &s);

  Error get_error() const { return error_; }

protected:
  Error error_ = Error::Success;
};

class TaskQueue {
public:
  TaskQueue() = default;
  virtual ~TaskQueue() = default;

  virtual bool enqueue(std::function<void()> fn) = 0;
  virtual void shutdown() = 0;

  virtual void on_idle() {}
};

class ThreadPool final : public TaskQueue {
public:
  explicit ThreadPool(size_t n, size_t max_n = 0, size_t mqr = 0,
                      int idle_timeout_sec = 3);
  ThreadPool(const ThreadPool &) = delete;
  ~ThreadPool() override = default;

  bool enqueue(std::function<void()> fn) override;
  void shutdown() override;

private:
  void worker(bool is_dynamic);
  void move_to_finished(std::thread::id id);
  void cleanup_finished_threads();

  size_t base_thread_count_;
  size_t max_thread_count_;
  size_t max_queued_requests_;
  int idle_timeout_sec_;
  size_t idle_thread_count_;

  bool shutdown_;

  std::list<std::function<void()>> jobs_;
  std::vector<std::thread> threads_;       // base threads
  std::list<std::thread> dynamic_threads_; // dynamic threads
  std::vector<std::thread>
      finished_threads_; // exited dynamic threads awaiting join

  std::condition_variable cond_;
  std::mutex mutex_;
};

using Logger = std::function<void(const Request &, const Response &)>;

// Forward declaration for Error type
enum class Error;
using ErrorLogger = std::function<void(const Error &, const Request *)>;

using SocketOptions = std::function<void(socket_t sock)>;

void default_socket_options(socket_t sock);

const char *status_message(int status);

std::string to_string(Error error);

std::ostream &operator<<(std::ostream &os, const Error &obj);

std::string get_bearer_token_auth(const Request &req);

namespace detail {

class MatcherBase {
public:
  MatcherBase(std::string pattern) : pattern_(std::move(pattern)) {}
  virtual ~MatcherBase() = default;

  const std::string &pattern() const { return pattern_; }

  // Match request path and populate its matches and
  virtual bool match(Request &request) const = 0;

private:
  std::string pattern_;
};

/**
 * Captures parameters in request path and stores them in Request::path_params
 *
 * Capture name is a substring of a pattern from : to /.
 * The rest of the pattern is matched against the request path directly
 * Parameters are captured starting from the next character after
 * the end of the last matched static pattern fragment until the next /.
 *
 * Example pattern:
 * "/path/fragments/:capture/more/fragments/:second_capture"
 * Static fragments:
 * "/path/fragments/", "more/fragments/"
 *
 * Given the following request path:
 * "/path/fragments/:1/more/fragments/:2"
 * the resulting capture will be
 * {{"capture", "1"}, {"second_capture", "2"}}
 */
class PathParamsMatcher final : public MatcherBase {
public:
  PathParamsMatcher(const std::string &pattern);

  bool match(Request &request) const override;

private:
  // Treat segment separators as the end of path parameter capture
  // Does not need to handle query parameters as they are parsed before path
  // matching
  static constexpr char separator = '/';

  // Contains static path fragments to match against, excluding the '/' after
  // path params
  // Fragments are separated by path params
  std::vector<std::string> static_fragments_;
  // Stores the names of the path parameters to be used as keys in the
  // Request::path_params map
  std::vector<std::string> param_names_;
};

/**
 * Performs std::regex_match on request path
 * and stores the result in Request::matches
 *
 * Note that regex match is performed directly on the whole request.
 * This means that wildcard patterns may match multiple path segments with /:
 * "/begin/(.*)/end" will match both "/begin/middle/end" and "/begin/1/2/end".
 */
class RegexMatcher final : public MatcherBase {
public:
  RegexMatcher(const std::string &pattern)
      : MatcherBase(pattern), regex_(pattern) {}

  bool match(Request &request) const override;

private:
  std::regex regex_;
};

int close_socket(socket_t sock);

ssize_t write_headers(Stream &strm, const Headers &headers);

bool set_socket_opt_time(socket_t sock, int level, int optname, time_t sec,
                         time_t usec);

} // namespace detail

} // namespace httplib

#endif // CPPHTTPLIB_TYPES_H
