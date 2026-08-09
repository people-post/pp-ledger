#include "httplib.h"
namespace httplib {

#ifdef CPPHTTPLIB_SSL_ENABLED
/*
 * TLS abstraction layer - internal function declarations
 * These are implementation details and not part of the public API.
 */
namespace tls {

// Client context
ctx_t create_client_context();
void free_context(ctx_t ctx);
bool set_min_version(ctx_t ctx, Version version);
bool load_ca_pem(ctx_t ctx, const char *pem, size_t len);
bool load_ca_file(ctx_t ctx, const char *file_path);
bool load_ca_dir(ctx_t ctx, const char *dir_path);
bool load_system_certs(ctx_t ctx);
bool set_client_cert_pem(ctx_t ctx, const char *cert, const char *key,
                         const char *password);
bool set_client_cert_file(ctx_t ctx, const char *cert_path,
                          const char *key_path, const char *password);

// Server context
ctx_t create_server_context();
bool set_server_cert_pem(ctx_t ctx, const char *cert, const char *key,
                         const char *password);
bool set_server_cert_file(ctx_t ctx, const char *cert_path,
                          const char *key_path, const char *password);
bool set_client_ca_file(ctx_t ctx, const char *ca_file, const char *ca_dir);
void set_verify_client(ctx_t ctx, bool require);

// Session management
session_t create_session(ctx_t ctx, socket_t sock);
void free_session(session_t session);
bool set_sni(session_t session, const char *hostname);
bool set_hostname(session_t session, const char *hostname);

// Handshake (non-blocking capable)
TlsError connect(session_t session);
TlsError accept(session_t session);

// Handshake with timeout (blocking until timeout)
bool connect_nonblocking(session_t session, socket_t sock, time_t timeout_sec,
                         time_t timeout_usec, TlsError *err);
bool accept_nonblocking(session_t session, socket_t sock, time_t timeout_sec,
                        time_t timeout_usec, TlsError *err);

// I/O (non-blocking capable)
ssize_t read(session_t session, void *buf, size_t len, TlsError &err);
ssize_t write(session_t session, const void *buf, size_t len, TlsError &err);
int pending(const_session_t session);
void shutdown(session_t session, bool graceful);

// Connection state
bool is_peer_closed(session_t session, socket_t sock);

// Certificate verification
cert_t get_peer_cert(const_session_t session);
void free_cert(cert_t cert);
bool verify_hostname(cert_t cert, const char *hostname);
uint64_t hostname_mismatch_code();
long get_verify_result(const_session_t session);

// Certificate introspection
std::string get_cert_subject_cn(cert_t cert);
std::string get_cert_issuer_name(cert_t cert);
bool get_cert_sans(cert_t cert, std::vector<SanEntry> &sans);
bool get_cert_validity(cert_t cert, time_t &not_before, time_t &not_after);
std::string get_cert_serial(cert_t cert);
bool get_cert_der(cert_t cert, std::vector<unsigned char> &der);
const char *get_sni(const_session_t session);

// CA store management
ca_store_t create_ca_store(const char *pem, size_t len);
void free_ca_store(ca_store_t store);
bool set_ca_store(ctx_t ctx, ca_store_t store);
size_t get_ca_certs(ctx_t ctx, std::vector<cert_t> &certs);
std::vector<std::string> get_ca_names(ctx_t ctx);

// Dynamic certificate update (for servers)
bool update_server_cert(ctx_t ctx, const char *cert_pem, const char *key_pem,
                        const char *password);
bool update_server_client_ca(ctx_t ctx, const char *ca_pem);

// Certificate verification callback
bool set_verify_callback(ctx_t ctx, VerifyCallback callback);
long get_verify_error(const_session_t session);
std::string verify_error_string(long error_code);

// TlsError information
uint64_t peek_error();
uint64_t get_error();
std::string error_string(uint64_t code);

} // namespace tls
#endif // CPPHTTPLIB_SSL_ENABLED
#ifdef CPPHTTPLIB_SSL_ENABLED
namespace detail {

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
std::string message_digest(const std::string &s, const EVP_MD *algo) {
  auto context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(
      EVP_MD_CTX_new(), EVP_MD_CTX_free);

  unsigned int hash_length = 0;
  unsigned char hash[EVP_MAX_MD_SIZE];

  EVP_DigestInit_ex(context.get(), algo, nullptr);
  EVP_DigestUpdate(context.get(), s.c_str(), s.size());
  EVP_DigestFinal_ex(context.get(), hash, &hash_length);

  std::stringstream ss;
  for (auto i = 0u; i < hash_length; ++i) {
    ss << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<unsigned int>(hash[i]);
  }

  return ss.str();
}

std::string MD5(const std::string &s) {
  return message_digest(s, EVP_md5());
}

std::string SHA_256(const std::string &s) {
  return message_digest(s, EVP_sha256());
}

std::string SHA_512(const std::string &s) {
  return message_digest(s, EVP_sha512());
}
#elif defined(CPPHTTPLIB_MBEDTLS_SUPPORT)
namespace {
template <size_t N>
std::string hash_to_hex(const unsigned char (&hash)[N]) {
  std::stringstream ss;
  for (size_t i = 0; i < N; ++i) {
    ss << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<unsigned int>(hash[i]);
  }
  return ss.str();
}
} // namespace

std::string MD5(const std::string &s) {
  unsigned char hash[16];
#ifdef CPPHTTPLIB_MBEDTLS_V3
  mbedtls_md5(reinterpret_cast<const unsigned char *>(s.c_str()), s.size(),
              hash);
#else
  mbedtls_md5_ret(reinterpret_cast<const unsigned char *>(s.c_str()), s.size(),
                  hash);
#endif
  return hash_to_hex(hash);
}

std::string SHA_256(const std::string &s) {
  unsigned char hash[32];
#ifdef CPPHTTPLIB_MBEDTLS_V3
  mbedtls_sha256(reinterpret_cast<const unsigned char *>(s.c_str()), s.size(),
                 hash, 0);
#else
  mbedtls_sha256_ret(reinterpret_cast<const unsigned char *>(s.c_str()),
                     s.size(), hash, 0);
#endif
  return hash_to_hex(hash);
}

std::string SHA_512(const std::string &s) {
  unsigned char hash[64];
#ifdef CPPHTTPLIB_MBEDTLS_V3
  mbedtls_sha512(reinterpret_cast<const unsigned char *>(s.c_str()), s.size(),
                 hash, 0);
#else
  mbedtls_sha512_ret(reinterpret_cast<const unsigned char *>(s.c_str()),
                     s.size(), hash, 0);
#endif
  return hash_to_hex(hash);
}
#elif defined(CPPHTTPLIB_WOLFSSL_SUPPORT)
namespace {
template <size_t N>
std::string hash_to_hex(const unsigned char (&hash)[N]) {
  std::stringstream ss;
  for (size_t i = 0; i < N; ++i) {
    ss << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<unsigned int>(hash[i]);
  }
  return ss.str();
}
} // namespace

std::string MD5(const std::string &s) {
  unsigned char hash[WC_MD5_DIGEST_SIZE];
  wc_Md5Hash(reinterpret_cast<const unsigned char *>(s.c_str()),
             static_cast<word32>(s.size()), hash);
  return hash_to_hex(hash);
}

std::string SHA_256(const std::string &s) {
  unsigned char hash[WC_SHA256_DIGEST_SIZE];
  wc_Sha256Hash(reinterpret_cast<const unsigned char *>(s.c_str()),
                static_cast<word32>(s.size()), hash);
  return hash_to_hex(hash);
}

std::string SHA_512(const std::string &s) {
  unsigned char hash[WC_SHA512_DIGEST_SIZE];
  wc_Sha512Hash(reinterpret_cast<const unsigned char *>(s.c_str()),
                static_cast<word32>(s.size()), hash);
  return hash_to_hex(hash);
}
#endif

bool is_ip_address(const std::string &host) {
  struct in_addr addr4;
  struct in6_addr addr6;
  return inet_pton(AF_INET, host.c_str(), &addr4) == 1 ||
         inet_pton(AF_INET6, host.c_str(), &addr6) == 1;
}

template <typename T>
bool process_server_socket_ssl(
    const std::atomic<socket_t> &svr_sock, tls::session_t session,
    socket_t sock, size_t keep_alive_max_count, time_t keep_alive_timeout_sec,
    time_t read_timeout_sec, time_t read_timeout_usec, time_t write_timeout_sec,
    time_t write_timeout_usec, T callback) {
  return process_server_socket_core(
      svr_sock, sock, keep_alive_max_count, keep_alive_timeout_sec,
      [&](bool close_connection, bool &connection_closed) {
        SSLSocketStream strm(sock, session, read_timeout_sec, read_timeout_usec,
                             write_timeout_sec, write_timeout_usec);
        return callback(strm, close_connection, connection_closed);
      });
}

template <typename T>
bool process_client_socket_ssl(
    tls::session_t session, socket_t sock, time_t read_timeout_sec,
    time_t read_timeout_usec, time_t write_timeout_sec,
    time_t write_timeout_usec, time_t max_timeout_msec,
    std::chrono::time_point<std::chrono::steady_clock> start_time, T callback) {
  SSLSocketStream strm(sock, session, read_timeout_sec, read_timeout_usec,
                       write_timeout_sec, write_timeout_usec, max_timeout_msec,
                       start_time);
  return callback(strm);
}

std::pair<std::string, std::string> make_digest_authentication_header(
    const Request &req, const std::map<std::string, std::string> &auth,
    size_t cnonce_count, const std::string &cnonce, const std::string &username,
    const std::string &password, bool is_proxy = false) {
  std::string nc;
  {
    std::stringstream ss;
    ss << std::setfill('0') << std::setw(8) << std::hex << cnonce_count;
    nc = ss.str();
  }

  std::string qop;
  if (auth.find("qop") != auth.end()) {
    qop = auth.at("qop");
    if (qop.find("auth-int") != std::string::npos) {
      qop = "auth-int";
    } else if (qop.find("auth") != std::string::npos) {
      qop = "auth";
    } else {
      qop.clear();
    }
  }

  std::string algo = "MD5";
  if (auth.find("algorithm") != auth.end()) { algo = auth.at("algorithm"); }

  std::string response;
  {
    auto H = algo == "SHA-256"   ? detail::SHA_256
             : algo == "SHA-512" ? detail::SHA_512
                                 : detail::MD5;

    auto A1 = username + ":" + auth.at("realm") + ":" + password;

    auto A2 = req.method + ":" + req.path;
    if (qop == "auth-int") { A2 += ":" + H(req.body); }

    if (qop.empty()) {
      response = H(H(A1) + ":" + auth.at("nonce") + ":" + H(A2));
    } else {
      response = H(H(A1) + ":" + auth.at("nonce") + ":" + nc + ":" + cnonce +
                   ":" + qop + ":" + H(A2));
    }
  }

  auto opaque = (auth.find("opaque") != auth.end()) ? auth.at("opaque") : "";

  auto field = "Digest username=\"" + username + "\", realm=\"" +
               auth.at("realm") + "\", nonce=\"" + auth.at("nonce") +
               "\", uri=\"" + req.path + "\", algorithm=" + algo +
               (qop.empty() ? ", response=\""
                            : ", qop=" + qop + ", nc=" + nc + ", cnonce=\"" +
                                  cnonce + "\", response=\"") +
               response + "\"" +
               (opaque.empty() ? "" : ", opaque=\"" + opaque + "\"");

  auto key = is_proxy ? "Proxy-Authorization" : "Authorization";
  return std::make_pair(key, field);
}

bool match_hostname(const std::string &pattern,
                           const std::string &hostname) {
  // Exact match (case-insensitive)
  if (detail::case_ignore::equal(hostname, pattern)) { return true; }

  // Split both pattern and hostname into components by '.'
  std::vector<std::string> pattern_components;
  if (!pattern.empty()) {
    split(pattern.data(), pattern.data() + pattern.size(), '.',
          [&](const char *b, const char *e) {
            pattern_components.emplace_back(b, e);
          });
  }

  std::vector<std::string> host_components;
  if (!hostname.empty()) {
    split(hostname.data(), hostname.data() + hostname.size(), '.',
          [&](const char *b, const char *e) {
            host_components.emplace_back(b, e);
          });
  }

  // Component count must match
  if (host_components.size() != pattern_components.size()) { return false; }

  // Compare each component with wildcard support
  // Supports: "*" (full wildcard), "prefix*" (partial wildcard)
  // https://bugs.launchpad.net/ubuntu/+source/firefox-3.0/+bug/376484
  auto itr = pattern_components.begin();
  for (const auto &h : host_components) {
    auto &p = *itr;
    if (!detail::case_ignore::equal(p, h) && p != "*") {
      bool partial_match = false;
      if (!p.empty() && p[p.size() - 1] == '*') {
        const auto prefix_length = p.size() - 1;
        if (prefix_length == 0) {
          partial_match = true;
        } else if (h.size() >= prefix_length) {
          partial_match =
              std::equal(p.begin(),
                         p.begin() + static_cast<std::string::difference_type>(
                                         prefix_length),
                         h.begin(), [](const char ca, const char cb) {
                           return detail::case_ignore::to_lower(ca) ==
                                  detail::case_ignore::to_lower(cb);
                         });
        }
      }
      if (!partial_match) { return false; }
    }
    ++itr;
  }

  return true;
}

#ifdef _WIN32
// Verify certificate using Windows CertGetCertificateChain API.
// This provides real-time certificate validation with Windows Update
// integration, independent of the TLS backend (OpenSSL or MbedTLS).
bool verify_cert_with_windows_schannel(
    const std::vector<unsigned char> &der_cert, const std::string &hostname,
    bool verify_hostname, unsigned long &out_error) {
  if (der_cert.empty()) { return false; }

  out_error = 0;

  // Create Windows certificate context from DER data
  auto cert_context = CertCreateCertificateContext(
      X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der_cert.data(),
      static_cast<DWORD>(der_cert.size()));

  if (!cert_context) {
    out_error = GetLastError();
    return false;
  }

  auto cert_guard =
      scope_exit([&] { CertFreeCertificateContext(cert_context); });

  // Setup chain parameters
  CERT_CHAIN_PARA chain_para = {};
  chain_para.cbSize = sizeof(chain_para);

  // Build certificate chain with revocation checking
  PCCERT_CHAIN_CONTEXT chain_context = nullptr;
  auto chain_result = CertGetCertificateChain(
      nullptr, cert_context, nullptr, cert_context->hCertStore, &chain_para,
      CERT_CHAIN_CACHE_END_CERT | CERT_CHAIN_REVOCATION_CHECK_END_CERT |
          CERT_CHAIN_REVOCATION_ACCUMULATIVE_TIMEOUT,
      nullptr, &chain_context);

  if (!chain_result || !chain_context) {
    out_error = GetLastError();
    return false;
  }

  auto chain_guard =
      scope_exit([&] { CertFreeCertificateChain(chain_context); });

  // Check if chain has errors
  if (chain_context->TrustStatus.dwErrorStatus != CERT_TRUST_NO_ERROR) {
    out_error = chain_context->TrustStatus.dwErrorStatus;
    return false;
  }

  // Verify SSL policy
  SSL_EXTRA_CERT_CHAIN_POLICY_PARA extra_policy_para = {};
  extra_policy_para.cbSize = sizeof(extra_policy_para);
#ifdef AUTHTYPE_SERVER
  extra_policy_para.dwAuthType = AUTHTYPE_SERVER;
#endif

  std::wstring whost;
  if (verify_hostname) {
    whost = u8string_to_wstring(hostname.c_str());
    extra_policy_para.pwszServerName = const_cast<wchar_t *>(whost.c_str());
  }

  CERT_CHAIN_POLICY_PARA policy_para = {};
  policy_para.cbSize = sizeof(policy_para);
#ifdef CERT_CHAIN_POLICY_IGNORE_ALL_REV_UNKNOWN_FLAGS
  policy_para.dwFlags = CERT_CHAIN_POLICY_IGNORE_ALL_REV_UNKNOWN_FLAGS;
#else
  policy_para.dwFlags = 0;
#endif
  policy_para.pvExtraPolicyPara = &extra_policy_para;

  CERT_CHAIN_POLICY_STATUS policy_status = {};
  policy_status.cbSize = sizeof(policy_status);

  if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain_context,
                                        &policy_para, &policy_status)) {
    out_error = GetLastError();
    return false;
  }

  if (policy_status.dwError != 0) {
    out_error = policy_status.dwError;
    return false;
  }

  return true;
}
#endif // _WIN32

bool setup_client_tls_session(const std::string &host, tls::ctx_t &ctx,
                                     tls::session_t &session, socket_t sock,
                                     bool server_certificate_verification,
                                     const std::string &ca_cert_file_path,
                                     tls::ca_store_t ca_cert_store,
                                     time_t timeout_sec, time_t timeout_usec) {
  using namespace tls;

  ctx = create_client_context();
  if (!ctx) { return false; }

  if (server_certificate_verification) {
    if (!ca_cert_file_path.empty()) {
      load_ca_file(ctx, ca_cert_file_path.c_str());
    }
    if (ca_cert_store) { set_ca_store(ctx, ca_cert_store); }
    load_system_certs(ctx);
  }

  bool is_ip = is_ip_address(host);

#ifdef CPPHTTPLIB_MBEDTLS_SUPPORT
  if (is_ip && server_certificate_verification) {
    set_verify_client(ctx, false);
  } else {
    set_verify_client(ctx, server_certificate_verification);
  }
#endif

  session = create_session(ctx, sock);
  if (!session) { return false; }

  // RFC 6066: SNI must not be set for IP addresses
  if (!is_ip) { set_sni(session, host.c_str()); }
  if (server_certificate_verification) { set_hostname(session, host.c_str()); }

  if (!connect_nonblocking(session, sock, timeout_sec, timeout_usec, nullptr)) {
    return false;
  }

  if (server_certificate_verification) {
    if (get_verify_result(session) != 0) { return false; }
  }

  return true;
}

} // namespace detail
#endif // CPPHTTPLIB_SSL_ENABLED
#ifdef CPPHTTPLIB_SSL_ENABLED
namespace detail {

// SSL socket stream implementation
SSLSocketStream::SSLSocketStream(
    socket_t sock, tls::session_t session, time_t read_timeout_sec,
    time_t read_timeout_usec, time_t write_timeout_sec,
    time_t write_timeout_usec, time_t max_timeout_msec,
    std::chrono::time_point<std::chrono::steady_clock> start_time)
    : sock_(sock), session_(session), read_timeout_sec_(read_timeout_sec),
      read_timeout_usec_(read_timeout_usec),
      write_timeout_sec_(write_timeout_sec),
      write_timeout_usec_(write_timeout_usec),
      max_timeout_msec_(max_timeout_msec), start_time_(start_time) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
  // Clear AUTO_RETRY for proper non-blocking I/O timeout handling
  // Note: create_session() also clears this, but SSLClient currently
  // uses ssl_new() which does not. Until full TLS API migration is complete,
  // we need to ensure AUTO_RETRY is cleared here regardless of how the
  // SSL session was created.
  SSL_clear_mode(static_cast<SSL *>(session), SSL_MODE_AUTO_RETRY);
#endif
}

SSLSocketStream::~SSLSocketStream() = default;

bool SSLSocketStream::is_readable() const {
  return tls::pending(session_) > 0;
}

bool SSLSocketStream::wait_readable() const {
  if (max_timeout_msec_ <= 0) {
    return select_read(sock_, read_timeout_sec_, read_timeout_usec_) > 0;
  }

  time_t read_timeout_sec;
  time_t read_timeout_usec;
  calc_actual_timeout(max_timeout_msec_, duration(), read_timeout_sec_,
                      read_timeout_usec_, read_timeout_sec, read_timeout_usec);

  return select_read(sock_, read_timeout_sec, read_timeout_usec) > 0;
}

bool SSLSocketStream::wait_writable() const {
  return select_write(sock_, write_timeout_sec_, write_timeout_usec_) > 0 &&
         is_socket_alive(sock_) && !tls::is_peer_closed(session_, sock_);
}

ssize_t SSLSocketStream::read(char *ptr, size_t size) {
  if (tls::pending(session_) > 0) {
    tls::TlsError err;
    auto ret = tls::read(session_, ptr, size, err);
    if (ret == 0 || err.code == tls::ErrorCode::PeerClosed) {
      error_ = Error::ConnectionClosed;
    }
    return ret;
  } else if (wait_readable()) {
    tls::TlsError err;
    auto ret = tls::read(session_, ptr, size, err);
    if (ret < 0) {
      auto n = 1000;
#ifdef _WIN32
      while (--n >= 0 && (err.code == tls::ErrorCode::WantRead ||
                          (err.code == tls::ErrorCode::SyscallError &&
                           WSAGetLastError() == WSAETIMEDOUT))) {
#else
      while (--n >= 0 && err.code == tls::ErrorCode::WantRead) {
#endif
        if (tls::pending(session_) > 0) {
          return tls::read(session_, ptr, size, err);
        } else if (wait_readable()) {
          std::this_thread::sleep_for(std::chrono::microseconds{10});
          ret = tls::read(session_, ptr, size, err);
          if (ret >= 0) { return ret; }
        } else {
          break;
        }
      }
      assert(ret < 0);
    } else if (ret == 0 || err.code == tls::ErrorCode::PeerClosed) {
      error_ = Error::ConnectionClosed;
    }
    return ret;
  } else {
    error_ = Error::Timeout;
    return -1;
  }
}

ssize_t SSLSocketStream::write(const char *ptr, size_t size) {
  if (wait_writable()) {
    auto handle_size =
        std::min<size_t>(size, (std::numeric_limits<int>::max)());

    tls::TlsError err;
    auto ret = tls::write(session_, ptr, handle_size, err);
    if (ret < 0) {
      auto n = 1000;
#ifdef _WIN32
      while (--n >= 0 && (err.code == tls::ErrorCode::WantWrite ||
                          (err.code == tls::ErrorCode::SyscallError &&
                           WSAGetLastError() == WSAETIMEDOUT))) {
#else
      while (--n >= 0 && err.code == tls::ErrorCode::WantWrite) {
#endif
        if (wait_writable()) {
          std::this_thread::sleep_for(std::chrono::microseconds{10});
          ret = tls::write(session_, ptr, handle_size, err);
          if (ret >= 0) { return ret; }
        } else {
          break;
        }
      }
      assert(ret < 0);
    }
    return ret;
  }
  return -1;
}

void SSLSocketStream::get_remote_ip_and_port(std::string &ip,
                                                    int &port) const {
  detail::get_remote_ip_and_port(sock_, ip, port);
}

void SSLSocketStream::get_local_ip_and_port(std::string &ip,
                                                   int &port) const {
  detail::get_local_ip_and_port(sock_, ip, port);
}

socket_t SSLSocketStream::socket() const { return sock_; }

time_t SSLSocketStream::duration() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start_time_)
      .count();
}

void SSLSocketStream::set_read_timeout(time_t sec, time_t usec) {
  read_timeout_sec_ = sec;
  read_timeout_usec_ = usec;
}

} // namespace detail
#endif // CPPHTTPLIB_SSL_ENABLED
#ifdef CPPHTTPLIB_SSL_ENABLED

// SSL HTTP server implementation
SSLServer::SSLServer(const char *cert_path, const char *private_key_path,
                            const char *client_ca_cert_file_path,
                            const char *client_ca_cert_dir_path,
                            const char *private_key_password) {
  using namespace tls;

  ctx_ = create_server_context();
  if (!ctx_) { return; }

  // Load server certificate and private key
  if (!set_server_cert_file(ctx_, cert_path, private_key_path,
                            private_key_password)) {
    last_ssl_error_ = static_cast<int>(get_error());
    free_context(ctx_);
    ctx_ = nullptr;
    return;
  }

  // Load client CA certificates for client authentication
  if (client_ca_cert_file_path || client_ca_cert_dir_path) {
    if (!set_client_ca_file(ctx_, client_ca_cert_file_path,
                            client_ca_cert_dir_path)) {
      last_ssl_error_ = static_cast<int>(get_error());
      free_context(ctx_);
      ctx_ = nullptr;
      return;
    }
    // Enable client certificate verification
    set_verify_client(ctx_, true);
  }
}

SSLServer::SSLServer(const PemMemory &pem) {
  using namespace tls;
  ctx_ = create_server_context();
  if (ctx_) {
    if (!set_server_cert_pem(ctx_, pem.cert_pem, pem.key_pem,
                             pem.private_key_password)) {
      last_ssl_error_ = static_cast<int>(get_error());
      free_context(ctx_);
      ctx_ = nullptr;
    } else if (pem.client_ca_pem && pem.client_ca_pem_len > 0) {
      if (!load_ca_pem(ctx_, pem.client_ca_pem, pem.client_ca_pem_len)) {
        last_ssl_error_ = static_cast<int>(get_error());
        free_context(ctx_);
        ctx_ = nullptr;
      } else {
        set_verify_client(ctx_, true);
      }
    }
  }
}

SSLServer::SSLServer(const tls::ContextSetupCallback &setup_callback) {
  using namespace tls;
  ctx_ = create_server_context();
  if (ctx_) {
    if (!setup_callback(ctx_)) {
      free_context(ctx_);
      ctx_ = nullptr;
    }
  }
}

SSLServer::~SSLServer() {
  if (ctx_) { tls::free_context(ctx_); }
}

bool SSLServer::is_valid() const { return ctx_ != nullptr; }

bool SSLServer::process_and_close_socket(socket_t sock) {
  using namespace tls;

  // Create TLS session with mutex protection
  session_t session = nullptr;
  {
    std::lock_guard<std::mutex> guard(ctx_mutex_);
    session = create_session(static_cast<ctx_t>(ctx_), sock);
  }

  if (!session) {
    last_ssl_error_ = static_cast<int>(get_error());
    detail::shutdown_socket(sock);
    detail::close_socket(sock);
    return false;
  }

  // Use scope_exit to ensure cleanup on all paths (including exceptions)
  bool handshake_done = false;
  bool ret = false;
  bool websocket_upgraded = false;
  auto cleanup = detail::scope_exit([&] {
    if (handshake_done) { shutdown(session, !websocket_upgraded && ret); }
    free_session(session);
    detail::shutdown_socket(sock);
    detail::close_socket(sock);
  });

  // Perform TLS accept handshake with timeout
  TlsError tls_err;
  if (!accept_nonblocking(session, sock, read_timeout_sec_, read_timeout_usec_,
                          &tls_err)) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    // Map TlsError to legacy ssl_error for backward compatibility
    if (tls_err.code == ErrorCode::WantRead) {
      last_ssl_error_ = SSL_ERROR_WANT_READ;
    } else if (tls_err.code == ErrorCode::WantWrite) {
      last_ssl_error_ = SSL_ERROR_WANT_WRITE;
    } else {
      last_ssl_error_ = SSL_ERROR_SSL;
    }
#else
    last_ssl_error_ = static_cast<int>(get_error());
#endif
    return false;
  }

  handshake_done = true;

  std::string remote_addr;
  int remote_port = 0;
  detail::get_remote_ip_and_port(sock, remote_addr, remote_port);

  std::string local_addr;
  int local_port = 0;
  detail::get_local_ip_and_port(sock, local_addr, local_port);

  ret = detail::process_server_socket_ssl(
      svr_sock_, session, sock, keep_alive_max_count_, keep_alive_timeout_sec_,
      read_timeout_sec_, read_timeout_usec_, write_timeout_sec_,
      write_timeout_usec_,
      [&](Stream &strm, bool close_connection, bool &connection_closed) {
        return process_request(
            strm, remote_addr, remote_port, local_addr, local_port,
            close_connection, connection_closed,
            [&](Request &req) { req.ssl = session; }, &websocket_upgraded);
      });

  return ret;
}

bool SSLServer::update_certs_pem(const char *cert_pem,
                                        const char *key_pem,
                                        const char *client_ca_pem,
                                        const char *password) {
  if (!ctx_) { return false; }
  std::lock_guard<std::mutex> guard(ctx_mutex_);
  if (!tls::update_server_cert(ctx_, cert_pem, key_pem, password)) {
    return false;
  }
  if (client_ca_pem) {
    return tls::update_server_client_ca(ctx_, client_ca_pem);
  }
  return true;
}

// SSL HTTP client implementation
SSLClient::~SSLClient() {
  if (ctx_) { tls::free_context(ctx_); }
  // Make sure to shut down SSL since shutdown_ssl will resolve to the
  // base function rather than the derived function once we get to the
  // base class destructor, and won't free the SSL (causing a leak).
  shutdown_ssl_impl(socket_, true);
}

bool SSLClient::is_valid() const { return ctx_ != nullptr; }

void SSLClient::shutdown_ssl(Socket &socket, bool shutdown_gracefully) {
  shutdown_ssl_impl(socket, shutdown_gracefully);
}

void SSLClient::shutdown_ssl_impl(Socket &socket,
                                         bool shutdown_gracefully) {
  if (socket.sock == INVALID_SOCKET) {
    assert(socket.ssl == nullptr);
    return;
  }
  if (socket.ssl) {
    tls::shutdown(socket.ssl, shutdown_gracefully);
    {
      std::lock_guard<std::mutex> guard(ctx_mutex_);
      tls::free_session(socket.ssl);
    }
    socket.ssl = nullptr;
  }
  assert(socket.ssl == nullptr);
}

bool SSLClient::process_socket(
    const Socket &socket,
    std::chrono::time_point<std::chrono::steady_clock> start_time,
    std::function<bool(Stream &strm)> callback) {
  assert(socket.ssl);
  return detail::process_client_socket_ssl(
      socket.ssl, socket.sock, read_timeout_sec_, read_timeout_usec_,
      write_timeout_sec_, write_timeout_usec_, max_timeout_msec_, start_time,
      std::move(callback));
}

bool SSLClient::is_ssl() const { return true; }

bool SSLClient::create_and_connect_socket(Socket &socket, Error &error) {
  if (!is_valid()) {
    error = Error::SSLConnection;
    return false;
  }
  return ClientImpl::create_and_connect_socket(socket, error);
}

// Assumes that socket_mutex_ is locked and that there are no requests in
// flight
bool SSLClient::connect_with_proxy(
    Socket &socket,
    std::chrono::time_point<std::chrono::steady_clock> start_time,
    Response &res, bool &success, Error &error) {
  success = true;
  Response proxy_res;
  if (!detail::process_client_socket(
          socket.sock, read_timeout_sec_, read_timeout_usec_,
          write_timeout_sec_, write_timeout_usec_, max_timeout_msec_,
          start_time, [&](Stream &strm) {
            Request req2;
            req2.method = "CONNECT";
            req2.path =
                detail::make_host_and_port_string_always_port(host_, port_);
            if (max_timeout_msec_ > 0) {
              req2.start_time_ = std::chrono::steady_clock::now();
            }
            return process_request(strm, req2, proxy_res, false, error);
          })) {
    // Thread-safe to close everything because we are assuming there are no
    // requests in flight
    shutdown_ssl(socket, true);
    shutdown_socket(socket);
    close_socket(socket);
    success = false;
    return false;
  }

  if (proxy_res.status == StatusCode::ProxyAuthenticationRequired_407) {
    if (!proxy_digest_auth_username_.empty() &&
        !proxy_digest_auth_password_.empty()) {
      std::map<std::string, std::string> auth;
      if (detail::parse_www_authenticate(proxy_res, auth, true)) {
        // Close the current socket and create a new one for the authenticated
        // request
        shutdown_ssl(socket, true);
        shutdown_socket(socket);
        close_socket(socket);

        // Create a new socket for the authenticated CONNECT request
        if (!ensure_socket_connection(socket, error)) {
          success = false;
          output_error_log(error, nullptr);
          return false;
        }

        proxy_res = Response();
        if (!detail::process_client_socket(
                socket.sock, read_timeout_sec_, read_timeout_usec_,
                write_timeout_sec_, write_timeout_usec_, max_timeout_msec_,
                start_time, [&](Stream &strm) {
                  Request req3;
                  req3.method = "CONNECT";
                  req3.path = detail::make_host_and_port_string_always_port(
                      host_, port_);
                  req3.headers.insert(detail::make_digest_authentication_header(
                      req3, auth, 1, detail::random_string(10),
                      proxy_digest_auth_username_, proxy_digest_auth_password_,
                      true));
                  if (max_timeout_msec_ > 0) {
                    req3.start_time_ = std::chrono::steady_clock::now();
                  }
                  return process_request(strm, req3, proxy_res, false, error);
                })) {
          // Thread-safe to close everything because we are assuming there are
          // no requests in flight
          shutdown_ssl(socket, true);
          shutdown_socket(socket);
          close_socket(socket);
          success = false;
          return false;
        }
      }
    }
  }

  // If status code is not 200, proxy request is failed.
  // Set error to ProxyConnection and return proxy response
  // as the response of the request
  if (proxy_res.status != StatusCode::OK_200) {
    error = Error::ProxyConnection;
    output_error_log(error, nullptr);
    res = std::move(proxy_res);
    // Thread-safe to close everything because we are assuming there are
    // no requests in flight
    shutdown_ssl(socket, true);
    shutdown_socket(socket);
    close_socket(socket);
    return false;
  }

  return true;
}

bool SSLClient::ensure_socket_connection(Socket &socket, Error &error) {
  if (!ClientImpl::ensure_socket_connection(socket, error)) { return false; }

  if (!proxy_host_.empty() && proxy_port_ != -1) { return true; }

  if (!initialize_ssl(socket, error)) {
    shutdown_socket(socket);
    close_socket(socket);
    return false;
  }

  return true;
}

// SSL HTTP client implementation
SSLClient::SSLClient(const std::string &host)
    : SSLClient(host, 443, std::string(), std::string()) {}

SSLClient::SSLClient(const std::string &host, int port)
    : SSLClient(host, port, std::string(), std::string()) {}

SSLClient::SSLClient(const std::string &host, int port,
                            const std::string &client_cert_path,
                            const std::string &client_key_path,
                            const std::string &private_key_password)
    : ClientImpl(host, port, client_cert_path, client_key_path) {
  ctx_ = tls::create_client_context();
  if (!ctx_) { return; }

  tls::set_min_version(ctx_, tls::Version::TLS1_2);

  if (!client_cert_path.empty() && !client_key_path.empty()) {
    const char *password =
        private_key_password.empty() ? nullptr : private_key_password.c_str();
    if (!tls::set_client_cert_file(ctx_, client_cert_path.c_str(),
                                   client_key_path.c_str(), password)) {
      last_backend_error_ = tls::get_error();
      tls::free_context(ctx_);
      ctx_ = nullptr;
    }
  }
}

SSLClient::SSLClient(const std::string &host, int port,
                            const PemMemory &pem)
    : ClientImpl(host, port) {
  ctx_ = tls::create_client_context();
  if (!ctx_) { return; }

  tls::set_min_version(ctx_, tls::Version::TLS1_2);

  if (pem.cert_pem && pem.key_pem) {
    if (!tls::set_client_cert_pem(ctx_, pem.cert_pem, pem.key_pem,
                                  pem.private_key_password)) {
      last_backend_error_ = tls::get_error();
      tls::free_context(ctx_);
      ctx_ = nullptr;
    }
  }
}

void SSLClient::set_ca_cert_store(tls::ca_store_t ca_cert_store) {
  if (ca_cert_store && ctx_) {
    // set_ca_store takes ownership of ca_cert_store
    tls::set_ca_store(ctx_, ca_cert_store);
  } else if (ca_cert_store) {
    tls::free_ca_store(ca_cert_store);
  }
}

void
SSLClient::set_server_certificate_verifier(tls::VerifyCallback verifier) {
  if (!ctx_) { return; }
  tls::set_verify_callback(ctx_, verifier);
}

void SSLClient::set_session_verifier(
    std::function<SSLVerifierResponse(tls::session_t)> verifier) {
  session_verifier_ = std::move(verifier);
}

#if defined(_WIN32) &&                                                         \
    !defined(CPPHTTPLIB_DISABLE_WINDOWS_AUTOMATIC_ROOT_CERTIFICATES_UPDATE)
void SSLClient::enable_windows_certificate_verification(bool enabled) {
  enable_windows_cert_verification_ = enabled;
}
#endif

void SSLClient::load_ca_cert_store(const char *ca_cert,
                                          std::size_t size) {
  if (ctx_ && ca_cert && size > 0) {
    ca_cert_pem_.assign(ca_cert, size); // Store for redirect transfer
    tls::load_ca_pem(ctx_, ca_cert, size);
  }
}

bool SSLClient::load_certs() {
  auto ret = true;

  std::call_once(initialize_cert_, [&]() {
    std::lock_guard<std::mutex> guard(ctx_mutex_);

    if (!ca_cert_file_path_.empty()) {
      if (!tls::load_ca_file(ctx_, ca_cert_file_path_.c_str())) {
        last_backend_error_ = tls::get_error();
        ret = false;
      }
    } else if (!ca_cert_dir_path_.empty()) {
      if (!tls::load_ca_dir(ctx_, ca_cert_dir_path_.c_str())) {
        last_backend_error_ = tls::get_error();
        ret = false;
      }
    } else if (ca_cert_pem_.empty()) {
      if (!tls::load_system_certs(ctx_)) {
        last_backend_error_ = tls::get_error();
      }
    }
  });

  return ret;
}

bool SSLClient::initialize_ssl(Socket &socket, Error &error) {
  using namespace tls;

  // Load CA certificates if server verification is enabled
  if (server_certificate_verification_) {
    if (!load_certs()) {
      error = Error::SSLLoadingCerts;
      output_error_log(error, nullptr);
      return false;
    }
  }

  bool is_ip = detail::is_ip_address(host_);

#if defined(CPPHTTPLIB_MBEDTLS_SUPPORT) || defined(CPPHTTPLIB_WOLFSSL_SUPPORT)
  // MbedTLS/wolfSSL need explicit verification mode (OpenSSL uses
  // SSL_VERIFY_NONE by default and performs all verification post-handshake).
  // For IP addresses with verification enabled, use OPTIONAL mode since
  // these backends require hostname for strict verification.
  if (is_ip && server_certificate_verification_) {
    set_verify_client(ctx_, false);
  } else {
    set_verify_client(ctx_, server_certificate_verification_);
  }
#endif

  // Create TLS session
  session_t session = nullptr;
  {
    std::lock_guard<std::mutex> guard(ctx_mutex_);
    session = create_session(ctx_, socket.sock);
  }

  if (!session) {
    error = Error::SSLConnection;
    last_backend_error_ = get_error();
    return false;
  }

  // Use scope_exit to ensure session is freed on error paths
  bool success = false;
  auto session_guard = detail::scope_exit([&] {
    if (!success) { free_session(session); }
  });

  // Set SNI extension (skip for IP addresses per RFC 6066).
  // On MbedTLS, set_sni also enables hostname verification internally.
  // On OpenSSL, set_sni only sets SNI; verification is done post-handshake.
  if (!is_ip) {
    if (!set_sni(session, host_.c_str())) {
      error = Error::SSLConnection;
      last_backend_error_ = get_error();
      return false;
    }
  }

  // Perform non-blocking TLS handshake with timeout
  TlsError tls_err;
  if (!connect_nonblocking(session, socket.sock, connection_timeout_sec_,
                           connection_timeout_usec_, &tls_err)) {
    last_ssl_error_ = static_cast<int>(tls_err.code);
    last_backend_error_ = tls_err.backend_code;
    if (tls_err.code == ErrorCode::CertVerifyFailed) {
      error = Error::SSLServerVerification;
    } else if (tls_err.code == ErrorCode::HostnameMismatch) {
      error = Error::SSLServerHostnameVerification;
    } else {
      error = Error::SSLConnection;
    }
    output_error_log(error, nullptr);
    return false;
  }

  // Post-handshake session verifier callback
  auto verification_status = SSLVerifierResponse::NoDecisionMade;
  if (session_verifier_) { verification_status = session_verifier_(session); }

  if (verification_status == SSLVerifierResponse::CertificateRejected) {
    last_backend_error_ = get_error();
    error = Error::SSLServerVerification;
    output_error_log(error, nullptr);
    return false;
  }

  // Default server certificate verification
  if (verification_status == SSLVerifierResponse::NoDecisionMade &&
      server_certificate_verification_) {
    verify_result_ = tls::get_verify_result(session);
    if (verify_result_ != 0) {
      last_backend_error_ = static_cast<unsigned long>(verify_result_);
      error = Error::SSLServerVerification;
      output_error_log(error, nullptr);
      return false;
    }

    auto server_cert = get_peer_cert(session);
    if (!server_cert) {
      last_backend_error_ = get_error();
      error = Error::SSLServerVerification;
      output_error_log(error, nullptr);
      return false;
    }
    auto cert_guard = detail::scope_exit([&] { free_cert(server_cert); });

    // Hostname verification (post-handshake for all cases).
    // On OpenSSL, verification is always post-handshake (SSL_VERIFY_NONE).
    // On MbedTLS, set_sni already enabled hostname verification during
    // handshake for non-IP hosts, but this check is still needed for IP
    // addresses where SNI is not set.
    if (server_hostname_verification_) {
      if (!verify_hostname(server_cert, host_.c_str())) {
        last_backend_error_ = hostname_mismatch_code();
        error = Error::SSLServerHostnameVerification;
        output_error_log(error, nullptr);
        return false;
      }
    }

#if defined(_WIN32) &&                                                         \
    !defined(CPPHTTPLIB_DISABLE_WINDOWS_AUTOMATIC_ROOT_CERTIFICATES_UPDATE)
    // Additional Windows Schannel verification.
    // This provides real-time certificate validation with Windows Update
    // integration, working with both OpenSSL and MbedTLS backends.
    // Skip when a custom CA cert is specified, as the Windows certificate
    // store would not know about user-provided CA certificates.
    if (enable_windows_cert_verification_ && ca_cert_file_path_.empty() &&
        ca_cert_dir_path_.empty() && ca_cert_pem_.empty()) {
      std::vector<unsigned char> der;
      if (get_cert_der(server_cert, der)) {
        unsigned long wincrypt_error = 0;
        if (!detail::verify_cert_with_windows_schannel(
                der, host_, server_hostname_verification_, wincrypt_error)) {
          last_backend_error_ = wincrypt_error;
          error = Error::SSLServerVerification;
          output_error_log(error, nullptr);
          return false;
        }
      }
    }
#endif
  }

  success = true;
  socket.ssl = session;
  return true;
}

void Client::set_digest_auth(const std::string &username,
                                    const std::string &password) {
  cli_->set_digest_auth(username, password);
}

void Client::set_proxy_digest_auth(const std::string &username,
                                          const std::string &password) {
  cli_->set_proxy_digest_auth(username, password);
}

void Client::enable_server_certificate_verification(bool enabled) {
  cli_->enable_server_certificate_verification(enabled);
}

void Client::enable_server_hostname_verification(bool enabled) {
  cli_->enable_server_hostname_verification(enabled);
}

#if defined(_WIN32) &&                                                         \
    !defined(CPPHTTPLIB_DISABLE_WINDOWS_AUTOMATIC_ROOT_CERTIFICATES_UPDATE)
void Client::enable_windows_certificate_verification(bool enabled) {
  if (is_ssl_) {
    static_cast<SSLClient &>(*cli_).enable_windows_certificate_verification(
        enabled);
  }
}
#endif

void Client::set_ca_cert_path(const std::string &ca_cert_file_path,
                                     const std::string &ca_cert_dir_path) {
  cli_->set_ca_cert_path(ca_cert_file_path, ca_cert_dir_path);
}

void Client::set_ca_cert_store(tls::ca_store_t ca_cert_store) {
  if (is_ssl_) {
    static_cast<SSLClient &>(*cli_).set_ca_cert_store(ca_cert_store);
  } else if (ca_cert_store) {
    tls::free_ca_store(ca_cert_store);
  }
}

void Client::load_ca_cert_store(const char *ca_cert, std::size_t size) {
  set_ca_cert_store(tls::create_ca_store(ca_cert, size));
}

void
Client::set_server_certificate_verifier(tls::VerifyCallback verifier) {
  if (is_ssl_) {
    static_cast<SSLClient &>(*cli_).set_server_certificate_verifier(
        std::move(verifier));
  }
}

void Client::set_session_verifier(
    std::function<SSLVerifierResponse(tls::session_t)> verifier) {
  if (is_ssl_) {
    static_cast<SSLClient &>(*cli_).set_session_verifier(std::move(verifier));
  }
}

tls::ctx_t Client::tls_context() const {
  if (is_ssl_) { return static_cast<SSLClient &>(*cli_).tls_context(); }
  return nullptr;
}

#endif // CPPHTTPLIB_SSL_ENABLED
#ifdef CPPHTTPLIB_SSL_ENABLED

namespace tls {

// Helper for PeerCert construction
PeerCert get_peer_cert_from_session(const_session_t session) {
  return PeerCert(get_peer_cert(session));
}

namespace impl {

VerifyCallback &get_verify_callback() {
  static thread_local VerifyCallback callback;
  return callback;
}

VerifyCallback &get_mbedtls_verify_callback() {
  static thread_local VerifyCallback callback;
  return callback;
}

// Check if a string is an IPv4 address
bool is_ipv4_address(const std::string &str) {
  int dots = 0;
  for (char c : str) {
    if (c == '.') {
      dots++;
    } else if (!isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return dots == 3;
}

// Parse IPv4 address string to bytes
bool parse_ipv4(const std::string &str, unsigned char *out) {
  int parts[4];
  if (sscanf(str.c_str(), "%d.%d.%d.%d", &parts[0], &parts[1], &parts[2],
             &parts[3]) != 4) {
    return false;
  }
  for (int i = 0; i < 4; i++) {
    if (parts[i] < 0 || parts[i] > 255) return false;
    out[i] = static_cast<unsigned char>(parts[i]);
  }
  return true;
}

#ifdef _WIN32
// Enumerate Windows system certificates and call callback with DER data
template <typename Callback>
bool enumerate_windows_system_certs(Callback cb) {
  bool loaded = false;
  static const wchar_t *store_names[] = {L"ROOT", L"CA"};
  for (auto store_name : store_names) {
    HCERTSTORE hStore = CertOpenSystemStoreW(0, store_name);
    if (hStore) {
      PCCERT_CONTEXT pContext = nullptr;
      while ((pContext = CertEnumCertificatesInStore(hStore, pContext)) !=
             nullptr) {
        if (cb(pContext->pbCertEncoded, pContext->cbCertEncoded)) {
          loaded = true;
        }
      }
      CertCloseStore(hStore, 0);
    }
  }
  return loaded;
}
#endif

#if defined(__APPLE__) && defined(CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN)
// Enumerate macOS Keychain certificates and call callback with DER data
template <typename Callback>
bool enumerate_macos_keychain_certs(Callback cb) {
  bool loaded = false;
  CFArrayRef certs = nullptr;
  OSStatus status = SecTrustCopyAnchorCertificates(&certs);
  if (status == errSecSuccess && certs) {
    CFIndex count = CFArrayGetCount(certs);
    for (CFIndex i = 0; i < count; i++) {
      SecCertificateRef cert =
          (SecCertificateRef)CFArrayGetValueAtIndex(certs, i);
      CFDataRef data = SecCertificateCopyData(cert);
      if (data) {
        if (cb(CFDataGetBytePtr(data),
               static_cast<size_t>(CFDataGetLength(data)))) {
          loaded = true;
        }
        CFRelease(data);
      }
    }
    CFRelease(certs);
  }
  return loaded;
}
#endif

#if !defined(_WIN32) && !(defined(__APPLE__) &&                                \
                          defined(CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN))
// Common CA certificate file paths on Linux/Unix
const char **system_ca_paths() {
  static const char *paths[] = {
      "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu
      "/etc/pki/tls/certs/ca-bundle.crt",   // RHEL/CentOS
      "/etc/ssl/ca-bundle.pem",             // OpenSUSE
      "/etc/pki/tls/cacert.pem",            // OpenELEC
      "/etc/ssl/cert.pem",                  // Alpine, FreeBSD
      nullptr};
  return paths;
}

// Common CA certificate directory paths on Linux/Unix
const char **system_ca_dirs() {
  static const char *dirs[] = {"/etc/ssl/certs",             // Debian/Ubuntu
                               "/etc/pki/tls/certs",         // RHEL/CentOS
                               "/usr/share/ca-certificates", // Other
                               nullptr};
  return dirs;
}
#endif

} // namespace impl

bool set_client_ca_file(ctx_t ctx, const char *ca_file,
                               const char *ca_dir) {
  if (!ctx) { return false; }

  bool success = true;
  if (ca_file && *ca_file) {
    if (!load_ca_file(ctx, ca_file)) { success = false; }
  }
  if (ca_dir && *ca_dir) {
    if (!load_ca_dir(ctx, ca_dir)) { success = false; }
  }

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
  // Set CA list for client certificate request (CertificateRequest message)
  if (ca_file && *ca_file) {
    auto list = SSL_load_client_CA_file(ca_file);
    if (list) { SSL_CTX_set_client_CA_list(static_cast<SSL_CTX *>(ctx), list); }
  }
#endif

  return success;
}

bool set_server_cert_pem(ctx_t ctx, const char *cert, const char *key,
                                const char *password) {
  return set_client_cert_pem(ctx, cert, key, password);
}

bool set_server_cert_file(ctx_t ctx, const char *cert_path,
                                 const char *key_path, const char *password) {
  return set_client_cert_file(ctx, cert_path, key_path, password);
}

// PeerCert implementation
PeerCert::PeerCert() = default;

PeerCert::PeerCert(cert_t cert) : cert_(cert) {}

PeerCert::PeerCert(PeerCert &&other) noexcept : cert_(other.cert_) {
  other.cert_ = nullptr;
}

PeerCert &PeerCert::operator=(PeerCert &&other) noexcept {
  if (this != &other) {
    if (cert_) { free_cert(cert_); }
    cert_ = other.cert_;
    other.cert_ = nullptr;
  }
  return *this;
}

PeerCert::~PeerCert() {
  if (cert_) { free_cert(cert_); }
}

PeerCert::operator bool() const { return cert_ != nullptr; }

std::string PeerCert::subject_cn() const {
  return cert_ ? get_cert_subject_cn(cert_) : std::string();
}

std::string PeerCert::issuer_name() const {
  return cert_ ? get_cert_issuer_name(cert_) : std::string();
}

bool PeerCert::check_hostname(const char *hostname) const {
  return cert_ ? verify_hostname(cert_, hostname) : false;
}

std::vector<SanEntry> PeerCert::sans() const {
  std::vector<SanEntry> result;
  if (cert_) { get_cert_sans(cert_, result); }
  return result;
}

bool PeerCert::validity(time_t &not_before, time_t &not_after) const {
  return cert_ ? get_cert_validity(cert_, not_before, not_after) : false;
}

std::string PeerCert::serial() const {
  return cert_ ? get_cert_serial(cert_) : std::string();
}

// VerifyContext method implementations
std::string VerifyContext::subject_cn() const {
  return cert ? get_cert_subject_cn(cert) : std::string();
}

std::string VerifyContext::issuer_name() const {
  return cert ? get_cert_issuer_name(cert) : std::string();
}

bool VerifyContext::check_hostname(const char *hostname) const {
  return cert ? verify_hostname(cert, hostname) : false;
}

std::vector<SanEntry> VerifyContext::sans() const {
  std::vector<SanEntry> result;
  if (cert) { get_cert_sans(cert, result); }
  return result;
}

bool VerifyContext::validity(time_t &not_before,
                                    time_t &not_after) const {
  return cert ? get_cert_validity(cert, not_before, not_after) : false;
}

std::string VerifyContext::serial() const {
  return cert ? get_cert_serial(cert) : std::string();
}

// TlsError static method implementation
std::string TlsError::verify_error_to_string(long error_code) {
  return verify_error_string(error_code);
}

} // namespace tls

// Request::peer_cert() implementation
tls::PeerCert Request::peer_cert() const {
  return tls::get_peer_cert_from_session(ssl);
}

// Request::sni() implementation
std::string Request::sni() const {
  if (!ssl) { return std::string(); }
  const char *s = tls::get_sni(ssl);
  return s ? std::string(s) : std::string();
}

#endif // CPPHTTPLIB_SSL_ENABLED

/*
 * Group 8: TLS abstraction layer - OpenSSL backend
 */

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
SSL_CTX *Client::ssl_context() const {
  if (is_ssl_) { return static_cast<SSLClient &>(*cli_).ssl_context(); }
  return nullptr;
}

void Client::set_server_certificate_verifier(
    std::function<SSLVerifierResponse(SSL *ssl)> verifier) {
  cli_->set_server_certificate_verifier(verifier);
}

long Client::get_verify_result() const {
  if (is_ssl_) { return static_cast<SSLClient &>(*cli_).get_verify_result(); }
  return -1; // NOTE: -1 doesn't match any of X509_V_ERR_???
}
#endif // CPPHTTPLIB_OPENSSL_SUPPORT

/*
 * OpenSSL Backend Implementation
 */

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
namespace tls {

namespace impl {

// OpenSSL-specific helpers for converting native types to PEM
std::string x509_to_pem(X509 *cert) {
  if (!cert) return {};
  BIO *bio = BIO_new(BIO_s_mem());
  if (!bio) return {};
  if (PEM_write_bio_X509(bio, cert) != 1) {
    BIO_free(bio);
    return {};
  }
  char *data = nullptr;
  long len = BIO_get_mem_data(bio, &data);
  std::string pem(data, static_cast<size_t>(len));
  BIO_free(bio);
  return pem;
}

std::string evp_pkey_to_pem(EVP_PKEY *key) {
  if (!key) return {};
  BIO *bio = BIO_new(BIO_s_mem());
  if (!bio) return {};
  if (PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr,
                               nullptr) != 1) {
    BIO_free(bio);
    return {};
  }
  char *data = nullptr;
  long len = BIO_get_mem_data(bio, &data);
  std::string pem(data, static_cast<size_t>(len));
  BIO_free(bio);
  return pem;
}

std::string x509_store_to_pem(X509_STORE *store) {
  if (!store) return {};
  std::string pem;
  auto objs = X509_STORE_get0_objects(store);
  if (!objs) return {};
  auto count = sk_X509_OBJECT_num(objs);
  for (decltype(count) i = 0; i < count; i++) {
    auto obj = sk_X509_OBJECT_value(objs, i);
    if (X509_OBJECT_get_type(obj) == X509_LU_X509) {
      auto cert = X509_OBJECT_get0_X509(obj);
      if (cert) { pem += x509_to_pem(cert); }
    }
  }
  return pem;
}

// Helper to map OpenSSL SSL_get_error to ErrorCode
ErrorCode map_ssl_error(int ssl_error, int &out_errno) {
  switch (ssl_error) {
  case SSL_ERROR_NONE: return ErrorCode::Success;
  case SSL_ERROR_WANT_READ: return ErrorCode::WantRead;
  case SSL_ERROR_WANT_WRITE: return ErrorCode::WantWrite;
  case SSL_ERROR_ZERO_RETURN: return ErrorCode::PeerClosed;
  case SSL_ERROR_SYSCALL: out_errno = errno; return ErrorCode::SyscallError;
  case SSL_ERROR_SSL:
  default: return ErrorCode::Fatal;
  }
}

// Helper: Create client CA list from PEM string
// Returns a new STACK_OF(X509_NAME)* or nullptr on failure
// Caller takes ownership of returned list
STACK_OF(X509_NAME) *
    create_client_ca_list_from_pem(const char *ca_pem) {
  if (!ca_pem) { return nullptr; }

  auto ca_list = sk_X509_NAME_new_null();
  if (!ca_list) { return nullptr; }

  BIO *bio = BIO_new_mem_buf(ca_pem, -1);
  if (!bio) {
    sk_X509_NAME_pop_free(ca_list, X509_NAME_free);
    return nullptr;
  }

  X509 *cert = nullptr;
  while ((cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) !=
         nullptr) {
    X509_NAME *name = X509_get_subject_name(cert);
    if (name) { sk_X509_NAME_push(ca_list, X509_NAME_dup(name)); }
    X509_free(cert);
  }
  BIO_free(bio);

  return ca_list;
}

// Helper: Extract CA names from X509_STORE
// Returns a new STACK_OF(X509_NAME)* or nullptr on failure
// Caller takes ownership of returned list
STACK_OF(X509_NAME) *
    extract_client_ca_list_from_store(X509_STORE *store) {
  if (!store) { return nullptr; }

  auto ca_list = sk_X509_NAME_new_null();
  if (!ca_list) { return nullptr; }

  auto objs = X509_STORE_get0_objects(store);
  if (!objs) {
    sk_X509_NAME_free(ca_list);
    return nullptr;
  }

  auto count = sk_X509_OBJECT_num(objs);
  for (decltype(count) i = 0; i < count; i++) {
    auto obj = sk_X509_OBJECT_value(objs, i);
    if (X509_OBJECT_get_type(obj) == X509_LU_X509) {
      auto cert = X509_OBJECT_get0_X509(obj);
      if (cert) {
        auto subject = X509_get_subject_name(cert);
        if (subject) {
          auto name_dup = X509_NAME_dup(subject);
          if (name_dup) { sk_X509_NAME_push(ca_list, name_dup); }
        }
      }
    }
  }

  if (sk_X509_NAME_num(ca_list) == 0) {
    sk_X509_NAME_free(ca_list);
    return nullptr;
  }

  return ca_list;
}

// OpenSSL verify callback wrapper
int openssl_verify_callback(int preverify_ok, X509_STORE_CTX *ctx) {
  auto &callback = get_verify_callback();
  if (!callback) { return preverify_ok; }

  // Get SSL object from X509_STORE_CTX
  auto ssl = static_cast<SSL *>(
      X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
  if (!ssl) { return preverify_ok; }

  // Get current certificate and depth
  auto cert = X509_STORE_CTX_get_current_cert(ctx);
  int depth = X509_STORE_CTX_get_error_depth(ctx);
  int error = X509_STORE_CTX_get_error(ctx);

  // Build context
  VerifyContext verify_ctx;
  verify_ctx.session = static_cast<session_t>(ssl);
  verify_ctx.cert = static_cast<cert_t>(cert);
  verify_ctx.depth = depth;
  verify_ctx.preverify_ok = (preverify_ok != 0);
  verify_ctx.error_code = error;
  verify_ctx.error_string =
      (error != X509_V_OK) ? X509_verify_cert_error_string(error) : nullptr;

  return callback(verify_ctx) ? 1 : 0;
}

} // namespace impl

ctx_t create_client_context() {
  SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
  if (ctx) {
    // Disable auto-retry to properly handle non-blocking I/O
    SSL_CTX_clear_mode(ctx, SSL_MODE_AUTO_RETRY);
    // Set minimum TLS version
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  }
  return static_cast<ctx_t>(ctx);
}

void free_context(ctx_t ctx) {
  if (ctx) { SSL_CTX_free(static_cast<SSL_CTX *>(ctx)); }
}

bool set_min_version(ctx_t ctx, Version version) {
  if (!ctx) return false;
  return SSL_CTX_set_min_proto_version(static_cast<SSL_CTX *>(ctx),
                                       static_cast<int>(version)) == 1;
}

bool load_ca_pem(ctx_t ctx, const char *pem, size_t len) {
  if (!ctx || !pem || len == 0) return false;

  auto ssl_ctx = static_cast<SSL_CTX *>(ctx);
  auto store = SSL_CTX_get_cert_store(ssl_ctx);
  if (!store) return false;

  auto bio = BIO_new_mem_buf(pem, static_cast<int>(len));
  if (!bio) return false;

  bool ok = true;
  X509 *cert = nullptr;
  while ((cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) !=
         nullptr) {
    if (X509_STORE_add_cert(store, cert) != 1) {
      // Ignore duplicate errors
      auto err = ERR_peek_last_error();
      if (ERR_GET_REASON(err) != X509_R_CERT_ALREADY_IN_HASH_TABLE) {
        ok = false;
      }
    }
    X509_free(cert);
    if (!ok) break;
  }
  BIO_free(bio);

  // Clear any "no more certificates" errors
  ERR_clear_error();
  return ok;
}

bool load_ca_file(ctx_t ctx, const char *file_path) {
  if (!ctx || !file_path) return false;
  return SSL_CTX_load_verify_locations(static_cast<SSL_CTX *>(ctx), file_path,
                                       nullptr) == 1;
}

bool load_ca_dir(ctx_t ctx, const char *dir_path) {
  if (!ctx || !dir_path) return false;
  return SSL_CTX_load_verify_locations(static_cast<SSL_CTX *>(ctx), nullptr,
                                       dir_path) == 1;
}

bool load_system_certs(ctx_t ctx) {
  if (!ctx) return false;
  auto ssl_ctx = static_cast<SSL_CTX *>(ctx);

#ifdef _WIN32
  // Windows: Load from system certificate store (ROOT and CA)
  auto store = SSL_CTX_get_cert_store(ssl_ctx);
  if (!store) return false;

  bool loaded_any = false;
  static const wchar_t *store_names[] = {L"ROOT", L"CA"};
  for (auto store_name : store_names) {
    auto hStore = CertOpenSystemStoreW(NULL, store_name);
    if (!hStore) continue;

    PCCERT_CONTEXT pContext = nullptr;
    while ((pContext = CertEnumCertificatesInStore(hStore, pContext)) !=
           nullptr) {
      const unsigned char *data = pContext->pbCertEncoded;
      auto x509 = d2i_X509(nullptr, &data, pContext->cbCertEncoded);
      if (x509) {
        if (X509_STORE_add_cert(store, x509) == 1) { loaded_any = true; }
        X509_free(x509);
      }
    }
    CertCloseStore(hStore, 0);
  }
  return loaded_any;

#elif defined(__APPLE__)
#ifdef CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN
  // macOS: Load from Keychain
  auto store = SSL_CTX_get_cert_store(ssl_ctx);
  if (!store) return false;

  CFArrayRef certs = nullptr;
  if (SecTrustCopyAnchorCertificates(&certs) != errSecSuccess || !certs) {
    return SSL_CTX_set_default_verify_paths(ssl_ctx) == 1;
  }

  bool loaded_any = false;
  auto count = CFArrayGetCount(certs);
  for (CFIndex i = 0; i < count; i++) {
    auto cert = reinterpret_cast<SecCertificateRef>(
        const_cast<void *>(CFArrayGetValueAtIndex(certs, i)));
    CFDataRef der = SecCertificateCopyData(cert);
    if (der) {
      const unsigned char *data = CFDataGetBytePtr(der);
      auto x509 = d2i_X509(nullptr, &data, CFDataGetLength(der));
      if (x509) {
        if (X509_STORE_add_cert(store, x509) == 1) { loaded_any = true; }
        X509_free(x509);
      }
      CFRelease(der);
    }
  }
  CFRelease(certs);
  return loaded_any || SSL_CTX_set_default_verify_paths(ssl_ctx) == 1;
#else
  return SSL_CTX_set_default_verify_paths(ssl_ctx) == 1;
#endif

#else
  // Other Unix: use default verify paths
  return SSL_CTX_set_default_verify_paths(ssl_ctx) == 1;
#endif
}

bool set_client_cert_pem(ctx_t ctx, const char *cert, const char *key,
                                const char *password) {
  if (!ctx || !cert || !key) return false;

  auto ssl_ctx = static_cast<SSL_CTX *>(ctx);

  // Load certificate
  auto cert_bio = BIO_new_mem_buf(cert, -1);
  if (!cert_bio) return false;

  auto x509 = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
  BIO_free(cert_bio);
  if (!x509) return false;

  auto cert_ok = SSL_CTX_use_certificate(ssl_ctx, x509) == 1;
  X509_free(x509);
  if (!cert_ok) return false;

  // Load private key
  auto key_bio = BIO_new_mem_buf(key, -1);
  if (!key_bio) return false;

  auto pkey = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr,
                                      password ? const_cast<char *>(password)
                                               : nullptr);
  BIO_free(key_bio);
  if (!pkey) return false;

  auto key_ok = SSL_CTX_use_PrivateKey(ssl_ctx, pkey) == 1;
  EVP_PKEY_free(pkey);

  return key_ok && SSL_CTX_check_private_key(ssl_ctx) == 1;
}

bool set_client_cert_file(ctx_t ctx, const char *cert_path,
                                 const char *key_path, const char *password) {
  if (!ctx || !cert_path || !key_path) return false;

  auto ssl_ctx = static_cast<SSL_CTX *>(ctx);

  if (password && password[0] != '\0') {
    SSL_CTX_set_default_passwd_cb_userdata(
        ssl_ctx, reinterpret_cast<void *>(const_cast<char *>(password)));
  }

  return SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_path) == 1 &&
         SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) == 1;
}

ctx_t create_server_context() {
  SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
  if (ctx) {
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION |
                                 SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  }
  return static_cast<ctx_t>(ctx);
}

void set_verify_client(ctx_t ctx, bool require) {
  if (!ctx) return;
  SSL_CTX_set_verify(static_cast<SSL_CTX *>(ctx),
                     require
                         ? (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT)
                         : SSL_VERIFY_NONE,
                     nullptr);
}

session_t create_session(ctx_t ctx, socket_t sock) {
  if (!ctx || sock == INVALID_SOCKET) return nullptr;

  auto ssl_ctx = static_cast<SSL_CTX *>(ctx);
  SSL *ssl = SSL_new(ssl_ctx);
  if (!ssl) return nullptr;

  // Disable auto-retry for proper non-blocking I/O handling
  SSL_clear_mode(ssl, SSL_MODE_AUTO_RETRY);

  auto bio = BIO_new_socket(static_cast<int>(sock), BIO_NOCLOSE);
  if (!bio) {
    SSL_free(ssl);
    return nullptr;
  }

  SSL_set_bio(ssl, bio, bio);
  return static_cast<session_t>(ssl);
}

void free_session(session_t session) {
  if (session) { SSL_free(static_cast<SSL *>(session)); }
}

bool set_sni(session_t session, const char *hostname) {
  if (!session || !hostname) return false;

  auto ssl = static_cast<SSL *>(session);

  // Set SNI (Server Name Indication) only - does not enable verification
#if defined(OPENSSL_IS_BORINGSSL)
  return SSL_set_tlsext_host_name(ssl, hostname) == 1;
#else
  // Direct call instead of macro to suppress -Wold-style-cast warning
  return SSL_ctrl(ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name,
                  static_cast<void *>(const_cast<char *>(hostname))) == 1;
#endif
}

bool set_hostname(session_t session, const char *hostname) {
  if (!session || !hostname) return false;

  auto ssl = static_cast<SSL *>(session);

  // Set SNI (Server Name Indication)
  if (!set_sni(session, hostname)) { return false; }

  // Enable hostname verification
  auto param = SSL_get0_param(ssl);
  if (!param) return false;

  X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
  if (X509_VERIFY_PARAM_set1_host(param, hostname, 0) != 1) { return false; }

  SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
  return true;
}

TlsError connect(session_t session) {
  if (!session) { return TlsError(); }

  auto ssl = static_cast<SSL *>(session);
  auto ret = SSL_connect(ssl);

  TlsError err;
  if (ret == 1) {
    err.code = ErrorCode::Success;
  } else {
    auto ssl_err = SSL_get_error(ssl, ret);
    err.code = impl::map_ssl_error(ssl_err, err.sys_errno);
    err.backend_code = ERR_get_error();
  }
  return err;
}

TlsError accept(session_t session) {
  if (!session) { return TlsError(); }

  auto ssl = static_cast<SSL *>(session);
  auto ret = SSL_accept(ssl);

  TlsError err;
  if (ret == 1) {
    err.code = ErrorCode::Success;
  } else {
    auto ssl_err = SSL_get_error(ssl, ret);
    err.code = impl::map_ssl_error(ssl_err, err.sys_errno);
    err.backend_code = ERR_get_error();
  }
  return err;
}

bool connect_nonblocking(session_t session, socket_t sock,
                                time_t timeout_sec, time_t timeout_usec,
                                TlsError *err) {
  if (!session) {
    if (err) { err->code = ErrorCode::Fatal; }
    return false;
  }

  auto ssl = static_cast<SSL *>(session);
  auto bio = SSL_get_rbio(ssl);

  // Set non-blocking mode for handshake
  detail::set_nonblocking(sock, true);
  if (bio) { BIO_set_nbio(bio, 1); }

  auto cleanup = detail::scope_exit([&]() {
    // Restore blocking mode after handshake
    if (bio) { BIO_set_nbio(bio, 0); }
    detail::set_nonblocking(sock, false);
  });

  auto res = 0;
  while ((res = SSL_connect(ssl)) != 1) {
    auto ssl_err = SSL_get_error(ssl, res);
    switch (ssl_err) {
    case SSL_ERROR_WANT_READ:
      if (detail::select_read(sock, timeout_sec, timeout_usec) > 0) {
        continue;
      }
      break;
    case SSL_ERROR_WANT_WRITE:
      if (detail::select_write(sock, timeout_sec, timeout_usec) > 0) {
        continue;
      }
      break;
    default: break;
    }
    if (err) {
      err->code = impl::map_ssl_error(ssl_err, err->sys_errno);
      err->backend_code = ERR_get_error();
    }
    return false;
  }
  if (err) { err->code = ErrorCode::Success; }
  return true;
}

bool accept_nonblocking(session_t session, socket_t sock,
                               time_t timeout_sec, time_t timeout_usec,
                               TlsError *err) {
  if (!session) {
    if (err) { err->code = ErrorCode::Fatal; }
    return false;
  }

  auto ssl = static_cast<SSL *>(session);
  auto bio = SSL_get_rbio(ssl);

  // Set non-blocking mode for handshake
  detail::set_nonblocking(sock, true);
  if (bio) { BIO_set_nbio(bio, 1); }

  auto cleanup = detail::scope_exit([&]() {
    // Restore blocking mode after handshake
    if (bio) { BIO_set_nbio(bio, 0); }
    detail::set_nonblocking(sock, false);
  });

  auto res = 0;
  while ((res = SSL_accept(ssl)) != 1) {
    auto ssl_err = SSL_get_error(ssl, res);
    switch (ssl_err) {
    case SSL_ERROR_WANT_READ:
      if (detail::select_read(sock, timeout_sec, timeout_usec) > 0) {
        continue;
      }
      break;
    case SSL_ERROR_WANT_WRITE:
      if (detail::select_write(sock, timeout_sec, timeout_usec) > 0) {
        continue;
      }
      break;
    default: break;
    }
    if (err) {
      err->code = impl::map_ssl_error(ssl_err, err->sys_errno);
      err->backend_code = ERR_get_error();
    }
    return false;
  }
  if (err) { err->code = ErrorCode::Success; }
  return true;
}

ssize_t read(session_t session, void *buf, size_t len, TlsError &err) {
  if (!session || !buf) {
    err.code = ErrorCode::Fatal;
    return -1;
  }

  auto ssl = static_cast<SSL *>(session);
  constexpr auto max_len =
      static_cast<size_t>((std::numeric_limits<int>::max)());
  if (len > max_len) { len = max_len; }
  auto ret = SSL_read(ssl, buf, static_cast<int>(len));

  if (ret > 0) {
    err.code = ErrorCode::Success;
    return ret;
  }

  auto ssl_err = SSL_get_error(ssl, ret);
  err.code = impl::map_ssl_error(ssl_err, err.sys_errno);
  if (err.code == ErrorCode::Fatal) { err.backend_code = ERR_get_error(); }
  return -1;
}

ssize_t write(session_t session, const void *buf, size_t len,
                     TlsError &err) {
  if (!session || !buf) {
    err.code = ErrorCode::Fatal;
    return -1;
  }

  auto ssl = static_cast<SSL *>(session);
  auto ret = SSL_write(ssl, buf, static_cast<int>(len));

  if (ret > 0) {
    err.code = ErrorCode::Success;
    return ret;
  }

  auto ssl_err = SSL_get_error(ssl, ret);
  err.code = impl::map_ssl_error(ssl_err, err.sys_errno);
  if (err.code == ErrorCode::Fatal) { err.backend_code = ERR_get_error(); }
  return -1;
}

int pending(const_session_t session) {
  if (!session) return 0;
  return SSL_pending(static_cast<SSL *>(const_cast<void *>(session)));
}

void shutdown(session_t session, bool graceful) {
  if (!session) return;

  auto ssl = static_cast<SSL *>(session);
  if (graceful) {
    // First call sends close_notify
    if (SSL_shutdown(ssl) == 0) {
      // Second call waits for peer's close_notify
      SSL_shutdown(ssl);
    }
  }
}

bool is_peer_closed(session_t session, socket_t sock) {
  if (!session) return true;

  // Temporarily set socket to non-blocking to avoid blocking on SSL_peek
  detail::set_nonblocking(sock, true);
  auto se = detail::scope_exit([&]() { detail::set_nonblocking(sock, false); });

  auto ssl = static_cast<SSL *>(session);
  char buf;
  auto ret = SSL_peek(ssl, &buf, 1);
  if (ret > 0) return false;

  auto err = SSL_get_error(ssl, ret);
  return err == SSL_ERROR_ZERO_RETURN;
}

cert_t get_peer_cert(const_session_t session) {
  if (!session) return nullptr;
  return static_cast<cert_t>(SSL_get1_peer_certificate(
      static_cast<SSL *>(const_cast<void *>(session))));
}

void free_cert(cert_t cert) {
  if (cert) { X509_free(static_cast<X509 *>(cert)); }
}

bool verify_hostname(cert_t cert, const char *hostname) {
  if (!cert || !hostname) return false;

  auto x509 = static_cast<X509 *>(cert);

  // Use X509_check_ip_asc for IP addresses, X509_check_host for DNS names
  if (detail::is_ip_address(hostname)) {
    return X509_check_ip_asc(x509, hostname, 0) == 1;
  }
  return X509_check_host(x509, hostname, strlen(hostname), 0, nullptr) == 1;
}

uint64_t hostname_mismatch_code() {
  return static_cast<uint64_t>(X509_V_ERR_HOSTNAME_MISMATCH);
}

long get_verify_result(const_session_t session) {
  if (!session) return X509_V_ERR_UNSPECIFIED;
  return SSL_get_verify_result(static_cast<SSL *>(const_cast<void *>(session)));
}

std::string get_cert_subject_cn(cert_t cert) {
  if (!cert) return "";
  auto x509 = static_cast<X509 *>(cert);
  auto subject_name = X509_get_subject_name(x509);
  if (!subject_name) return "";

  char buf[256];
  auto len =
      X509_NAME_get_text_by_NID(subject_name, NID_commonName, buf, sizeof(buf));
  if (len < 0) return "";
  return std::string(buf, static_cast<size_t>(len));
}

std::string get_cert_issuer_name(cert_t cert) {
  if (!cert) return "";
  auto x509 = static_cast<X509 *>(cert);
  auto issuer_name = X509_get_issuer_name(x509);
  if (!issuer_name) return "";

  char buf[256];
  X509_NAME_oneline(issuer_name, buf, sizeof(buf));
  return std::string(buf);
}

bool get_cert_sans(cert_t cert, std::vector<SanEntry> &sans) {
  sans.clear();
  if (!cert) return false;
  auto x509 = static_cast<X509 *>(cert);

  auto names = static_cast<GENERAL_NAMES *>(
      X509_get_ext_d2i(x509, NID_subject_alt_name, nullptr, nullptr));
  if (!names) return true; // No SANs is valid

  auto count = sk_GENERAL_NAME_num(names);
  for (decltype(count) i = 0; i < count; i++) {
    auto gen = sk_GENERAL_NAME_value(names, i);
    if (!gen) continue;

    SanEntry entry;
    switch (gen->type) {
    case GEN_DNS:
      entry.type = SanType::DNS;
      if (gen->d.dNSName) {
        entry.value = std::string(
            reinterpret_cast<const char *>(
                ASN1_STRING_get0_data(gen->d.dNSName)),
            static_cast<size_t>(ASN1_STRING_length(gen->d.dNSName)));
      }
      break;
    case GEN_IPADD:
      entry.type = SanType::IP;
      if (gen->d.iPAddress) {
        auto data = ASN1_STRING_get0_data(gen->d.iPAddress);
        auto len = ASN1_STRING_length(gen->d.iPAddress);
        if (len == 4) {
          // IPv4
          char buf[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, data, buf, sizeof(buf));
          entry.value = buf;
        } else if (len == 16) {
          // IPv6
          char buf[INET6_ADDRSTRLEN];
          inet_ntop(AF_INET6, data, buf, sizeof(buf));
          entry.value = buf;
        }
      }
      break;
    case GEN_EMAIL:
      entry.type = SanType::EMAIL;
      if (gen->d.rfc822Name) {
        entry.value = std::string(
            reinterpret_cast<const char *>(
                ASN1_STRING_get0_data(gen->d.rfc822Name)),
            static_cast<size_t>(ASN1_STRING_length(gen->d.rfc822Name)));
      }
      break;
    case GEN_URI:
      entry.type = SanType::URI;
      if (gen->d.uniformResourceIdentifier) {
        entry.value = std::string(
            reinterpret_cast<const char *>(
                ASN1_STRING_get0_data(gen->d.uniformResourceIdentifier)),
            static_cast<size_t>(
                ASN1_STRING_length(gen->d.uniformResourceIdentifier)));
      }
      break;
    default: entry.type = SanType::OTHER; break;
    }

    if (!entry.value.empty()) { sans.push_back(std::move(entry)); }
  }

  GENERAL_NAMES_free(names);
  return true;
}

bool get_cert_validity(cert_t cert, time_t &not_before,
                              time_t &not_after) {
  if (!cert) return false;
  auto x509 = static_cast<X509 *>(cert);

  auto nb = X509_get0_notBefore(x509);
  auto na = X509_get0_notAfter(x509);
  if (!nb || !na) return false;

  ASN1_TIME *epoch = ASN1_TIME_new();
  if (!epoch) return false;
  auto se = detail::scope_exit([&] { ASN1_TIME_free(epoch); });

  if (!ASN1_TIME_set(epoch, 0)) return false;

  int pday, psec;

  if (!ASN1_TIME_diff(&pday, &psec, epoch, nb)) return false;
  not_before = 86400 * (time_t)pday + psec;

  if (!ASN1_TIME_diff(&pday, &psec, epoch, na)) return false;
  not_after = 86400 * (time_t)pday + psec;

  return true;
}

std::string get_cert_serial(cert_t cert) {
  if (!cert) return "";
  auto x509 = static_cast<X509 *>(cert);

  auto serial = X509_get_serialNumber(x509);
  if (!serial) return "";

  auto bn = ASN1_INTEGER_to_BN(serial, nullptr);
  if (!bn) return "";

  auto hex = BN_bn2hex(bn);
  BN_free(bn);
  if (!hex) return "";

  std::string result(hex);
  OPENSSL_free(hex);
  return result;
}

bool get_cert_der(cert_t cert, std::vector<unsigned char> &der) {
  if (!cert) return false;
  auto x509 = static_cast<X509 *>(cert);
  auto len = i2d_X509(x509, nullptr);
  if (len < 0) return false;
  der.resize(static_cast<size_t>(len));
  auto p = der.data();
  i2d_X509(x509, &p);
  return true;
}

const char *get_sni(const_session_t session) {
  if (!session) return nullptr;
  auto ssl = static_cast<SSL *>(const_cast<void *>(session));
  return SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
}

uint64_t peek_error() { return ERR_peek_last_error(); }

uint64_t get_error() { return ERR_get_error(); }

std::string error_string(uint64_t code) {
  char buf[256];
  ERR_error_string_n(static_cast<unsigned long>(code), buf, sizeof(buf));
  return std::string(buf);
}

ca_store_t create_ca_store(const char *pem, size_t len) {
  auto mem = BIO_new_mem_buf(pem, static_cast<int>(len));
  if (!mem) { return nullptr; }
  auto mem_guard = detail::scope_exit([&] { BIO_free_all(mem); });

  auto inf = PEM_X509_INFO_read_bio(mem, nullptr, nullptr, nullptr);
  if (!inf) { return nullptr; }

  auto store = X509_STORE_new();
  if (store) {
    for (auto i = 0; i < static_cast<int>(sk_X509_INFO_num(inf)); i++) {
      auto itmp = sk_X509_INFO_value(inf, i);
      if (!itmp) { continue; }
      if (itmp->x509) { X509_STORE_add_cert(store, itmp->x509); }
      if (itmp->crl) { X509_STORE_add_crl(store, itmp->crl); }
    }
  }

  sk_X509_INFO_pop_free(inf, X509_INFO_free);
  return static_cast<ca_store_t>(store);
}

void free_ca_store(ca_store_t store) {
  if (store) { X509_STORE_free(static_cast<X509_STORE *>(store)); }
}

bool set_ca_store(ctx_t ctx, ca_store_t store) {
  if (!ctx || !store) { return false; }
  auto ssl_ctx = static_cast<SSL_CTX *>(ctx);
  auto x509_store = static_cast<X509_STORE *>(store);

  // Check if same store is already set
  if (SSL_CTX_get_cert_store(ssl_ctx) == x509_store) { return true; }

  // SSL_CTX_set_cert_store takes ownership and frees the old store
  SSL_CTX_set_cert_store(ssl_ctx, x509_store);
  return true;
}

size_t get_ca_certs(ctx_t ctx, std::vector<cert_t> &certs) {
  certs.clear();
  if (!ctx) { return 0; }
  auto ssl_ctx = static_cast<SSL_CTX *>(ctx);

  auto store = SSL_CTX_get_cert_store(ssl_ctx);
  if (!store) { return 0; }

  auto objs = X509_STORE_get0_objects(store);
  if (!objs) { return 0; }

  auto count = sk_X509_OBJECT_num(objs);
  for (decltype(count) i = 0; i < count; i++) {
    auto obj = sk_X509_OBJECT_value(objs, i);
    if (!obj) { continue; }
    if (X509_OBJECT_get_type(obj) == X509_LU_X509) {
      auto x509 = X509_OBJECT_get0_X509(obj);
      if (x509) {
        // Increment reference count so caller can free it
        X509_up_ref(x509);
        certs.push_back(static_cast<cert_t>(x509));
      }
    }
  }
  return certs.size();
}

std::vector<std::string> get_ca_names(ctx_t ctx) {
  std::vector<std::string> names;
  if (!ctx) { return names; }
  auto ssl_ctx = static_cast<SSL_CTX *>(ctx);

  auto store = SSL_CTX_get_cert_store(ssl_ctx);
  if (!store) { return names; }

  auto objs = X509_STORE_get0_objects(store);
  if (!objs) { return names; }

  auto count = sk_X509_OBJECT_num(objs);
  for (decltype(count) i = 0; i < count; i++) {
    auto obj = sk_X509_OBJECT_value(objs, i);
    if (!obj) { continue; }
    if (X509_OBJECT_get_type(obj) == X509_LU_X509) {
      auto x509 = X509_OBJECT_get0_X509(obj);
      if (x509) {
        auto subject = X509_get_subject_name(x509);
        if (subject) {
          char buf[512];
          X509_NAME_oneline(subject, buf, sizeof(buf));
          names.push_back(buf);
        }
      }
    }
  }
  return names;
}

bool update_server_cert(ctx_t ctx, const char *cert_pem,
                               const char *key_pem, const char *password) {
  if (!ctx || !cert_pem || !key_pem) { return false; }
  auto ssl_ctx = static_cast<SSL_CTX *>(ctx);

  // Load certificate from PEM
  auto cert_bio = BIO_new_mem_buf(cert_pem, -1);
  if (!cert_bio) { return false; }
  auto cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
  BIO_free(cert_bio);
  if (!cert) { return false; }

  // Load private key from PEM
  auto key_bio = BIO_new_mem_buf(key_pem, -1);
  if (!key_bio) {
    X509_free(cert);
    return false;
  }
  auto key = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr,
                                     password ? const_cast<char *>(password)
                                              : nullptr);
  BIO_free(key_bio);
  if (!key) {
    X509_free(cert);
    return false;
  }

  // Update certificate and key
  auto ret = SSL_CTX_use_certificate(ssl_ctx, cert) == 1 &&
             SSL_CTX_use_PrivateKey(ssl_ctx, key) == 1;

  X509_free(cert);
  EVP_PKEY_free(key);
  return ret;
}

bool update_server_client_ca(ctx_t ctx, const char *ca_pem) {
  if (!ctx || !ca_pem) { return false; }
  auto ssl_ctx = static_cast<SSL_CTX *>(ctx);

  // Create new X509_STORE from PEM
  auto store = create_ca_store(ca_pem, strlen(ca_pem));
  if (!store) { return false; }

  // SSL_CTX_set_cert_store takes ownership
  SSL_CTX_set_cert_store(ssl_ctx, static_cast<X509_STORE *>(store));

  // Set client CA list for client certificate request
  auto ca_list = impl::create_client_ca_list_from_pem(ca_pem);
  if (ca_list) {
    // SSL_CTX_set_client_CA_list takes ownership of ca_list
    SSL_CTX_set_client_CA_list(ssl_ctx, ca_list);
  }

  return true;
}

bool set_verify_callback(ctx_t ctx, VerifyCallback callback) {
  if (!ctx) { return false; }
  auto ssl_ctx = static_cast<SSL_CTX *>(ctx);

  impl::get_verify_callback() = std::move(callback);

  if (impl::get_verify_callback()) {
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, impl::openssl_verify_callback);
  } else {
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, nullptr);
  }
  return true;
}

long get_verify_error(const_session_t session) {
  if (!session) { return -1; }
  auto ssl = static_cast<SSL *>(const_cast<void *>(session));
  return SSL_get_verify_result(ssl);
}

std::string verify_error_string(long error_code) {
  if (error_code == X509_V_OK) { return ""; }
  const char *str = X509_verify_cert_error_string(static_cast<int>(error_code));
  return str ? str : "unknown error";
}

namespace impl {

// OpenSSL-specific helpers for public API wrappers
ctx_t create_server_context_from_x509(X509 *cert, EVP_PKEY *key,
                                             X509_STORE *client_ca_store,
                                             int &out_error) {
  out_error = 0;
  auto cert_pem = x509_to_pem(cert);
  auto key_pem = evp_pkey_to_pem(key);
  if (cert_pem.empty() || key_pem.empty()) {
    out_error = static_cast<int>(ERR_get_error());
    return nullptr;
  }

  auto ctx = create_server_context();
  if (!ctx) {
    out_error = static_cast<int>(get_error());
    return nullptr;
  }

  if (!set_server_cert_pem(ctx, cert_pem.c_str(), key_pem.c_str(), nullptr)) {
    out_error = static_cast<int>(get_error());
    free_context(ctx);
    return nullptr;
  }

  if (client_ca_store) {
    // Set cert store for verification (SSL_CTX_set_cert_store takes ownership)
    SSL_CTX_set_cert_store(static_cast<SSL_CTX *>(ctx), client_ca_store);

    // Extract and set client CA list directly from store (more efficient than
    // PEM conversion)
    auto ca_list = extract_client_ca_list_from_store(client_ca_store);
    if (ca_list) {
      SSL_CTX_set_client_CA_list(static_cast<SSL_CTX *>(ctx), ca_list);
    }

    set_verify_client(ctx, true);
  }

  return ctx;
}

void update_server_certs_from_x509(ctx_t ctx, X509 *cert, EVP_PKEY *key,
                                          X509_STORE *client_ca_store) {
  auto cert_pem = x509_to_pem(cert);
  auto key_pem = evp_pkey_to_pem(key);

  if (!cert_pem.empty() && !key_pem.empty()) {
    update_server_cert(ctx, cert_pem.c_str(), key_pem.c_str(), nullptr);
  }

  if (client_ca_store) {
    auto ca_pem = x509_store_to_pem(client_ca_store);
    if (!ca_pem.empty()) { update_server_client_ca(ctx, ca_pem.c_str()); }
    X509_STORE_free(client_ca_store);
  }
}

ctx_t create_client_context_from_x509(X509 *cert, EVP_PKEY *key,
                                             const char *password,
                                             unsigned long &out_error) {
  out_error = 0;
  auto ctx = create_client_context();
  if (!ctx) {
    out_error = static_cast<unsigned long>(get_error());
    return nullptr;
  }

  if (cert && key) {
    auto cert_pem = x509_to_pem(cert);
    auto key_pem = evp_pkey_to_pem(key);
    if (cert_pem.empty() || key_pem.empty()) {
      out_error = ERR_get_error();
      free_context(ctx);
      return nullptr;
    }
    if (!set_client_cert_pem(ctx, cert_pem.c_str(), key_pem.c_str(),
                             password)) {
      out_error = static_cast<unsigned long>(get_error());
      free_context(ctx);
      return nullptr;
    }
  }

  return ctx;
}

} // namespace impl

} // namespace tls

// ClientImpl::set_ca_cert_store - defined here to use
// tls::impl::x509_store_to_pem Deprecated: converts X509_STORE to PEM and
// stores for redirect transfer
void ClientImpl::set_ca_cert_store(X509_STORE *ca_cert_store) {
  if (ca_cert_store) {
    ca_cert_pem_ = tls::impl::x509_store_to_pem(ca_cert_store);
  }
}

SSLServer::SSLServer(X509 *cert, EVP_PKEY *private_key,
                            X509_STORE *client_ca_cert_store) {
  ctx_ = tls::impl::create_server_context_from_x509(
      cert, private_key, client_ca_cert_store, last_ssl_error_);
}

SSLServer::SSLServer(
    const std::function<bool(SSL_CTX &ssl_ctx)> &setup_ssl_ctx_callback) {
  // Use abstract API to create context
  ctx_ = tls::create_server_context();
  if (ctx_) {
    // Pass to OpenSSL-specific callback (ctx_ is SSL_CTX* internally)
    auto ssl_ctx = static_cast<SSL_CTX *>(ctx_);
    if (!setup_ssl_ctx_callback(*ssl_ctx)) {
      tls::free_context(ctx_);
      ctx_ = nullptr;
    }
  }
}

SSL_CTX *SSLServer::ssl_context() const {
  return static_cast<SSL_CTX *>(ctx_);
}

void SSLServer::update_certs(X509 *cert, EVP_PKEY *private_key,
                                    X509_STORE *client_ca_cert_store) {
  std::lock_guard<std::mutex> guard(ctx_mutex_);
  tls::impl::update_server_certs_from_x509(ctx_, cert, private_key,
                                           client_ca_cert_store);
}

SSLClient::SSLClient(const std::string &host, int port,
                            X509 *client_cert, EVP_PKEY *client_key,
                            const std::string &private_key_password)
    : ClientImpl(host, port) {
  const char *password =
      private_key_password.empty() ? nullptr : private_key_password.c_str();
  ctx_ = tls::impl::create_client_context_from_x509(
      client_cert, client_key, password, last_backend_error_);
}

long SSLClient::get_verify_result() const { return verify_result_; }

void SSLClient::set_server_certificate_verifier(
    std::function<SSLVerifierResponse(SSL *ssl)> verifier) {
  // Wrap SSL* callback into backend-independent session_verifier_
  auto v = std::make_shared<std::function<SSLVerifierResponse(SSL *)>>(
      std::move(verifier));
  session_verifier_ = [v](tls::session_t session) {
    return (*v)(static_cast<SSL *>(session));
  };
}

SSL_CTX *SSLClient::ssl_context() const {
  return static_cast<SSL_CTX *>(ctx_);
}

bool SSLClient::verify_host(X509 *server_cert) const {
  /* Quote from RFC2818 section 3.1 "Server Identity"

     If a subjectAltName extension of type dNSName is present, that MUST
     be used as the identity. Otherwise, the (most specific) Common Name
     field in the Subject field of the certificate MUST be used. Although
     the use of the Common Name is existing practice, it is deprecated and
     Certification Authorities are encouraged to use the dNSName instead.

     Matching is performed using the matching rules specified by
     [RFC2459].  If more than one identity of a given type is present in
     the certificate (e.g., more than one dNSName name, a match in any one
     of the set is considered acceptable.) Names may contain the wildcard
     character * which is considered to match any single domain name
     component or component fragment. E.g., *.a.com matches foo.a.com but
     not bar.foo.a.com. f*.com matches foo.com but not bar.com.

     In some cases, the URI is specified as an IP address rather than a
     hostname. In this case, the iPAddress subjectAltName must be present
     in the certificate and must exactly match the IP in the URI.

  */
  return verify_host_with_subject_alt_name(server_cert) ||
         verify_host_with_common_name(server_cert);
}

bool
SSLClient::verify_host_with_subject_alt_name(X509 *server_cert) const {
  auto ret = false;

  auto type = GEN_DNS;

  struct in6_addr addr6 = {};
  struct in_addr addr = {};
  size_t addr_len = 0;

#ifndef __MINGW32__
  if (inet_pton(AF_INET6, host_.c_str(), &addr6)) {
    type = GEN_IPADD;
    addr_len = sizeof(struct in6_addr);
  } else if (inet_pton(AF_INET, host_.c_str(), &addr)) {
    type = GEN_IPADD;
    addr_len = sizeof(struct in_addr);
  }
#endif

  auto alt_names = static_cast<const struct stack_st_GENERAL_NAME *>(
      X509_get_ext_d2i(server_cert, NID_subject_alt_name, nullptr, nullptr));

  if (alt_names) {
    auto dsn_matched = false;
    auto ip_matched = false;

    auto count = sk_GENERAL_NAME_num(alt_names);

    for (decltype(count) i = 0; i < count && !dsn_matched; i++) {
      auto val = sk_GENERAL_NAME_value(alt_names, i);
      if (!val || val->type != type) { continue; }

      auto name =
          reinterpret_cast<const char *>(ASN1_STRING_get0_data(val->d.ia5));
      if (name == nullptr) { continue; }

      auto name_len = static_cast<size_t>(ASN1_STRING_length(val->d.ia5));

      switch (type) {
      case GEN_DNS:
        dsn_matched =
            detail::match_hostname(std::string(name, name_len), host_);
        break;

      case GEN_IPADD:
        if (!memcmp(&addr6, name, addr_len) || !memcmp(&addr, name, addr_len)) {
          ip_matched = true;
        }
        break;
      }
    }

    if (dsn_matched || ip_matched) { ret = true; }
  }

  GENERAL_NAMES_free(const_cast<STACK_OF(GENERAL_NAME) *>(
      reinterpret_cast<const STACK_OF(GENERAL_NAME) *>(alt_names)));
  return ret;
}

bool SSLClient::verify_host_with_common_name(X509 *server_cert) const {
  const auto subject_name = X509_get_subject_name(server_cert);

  if (subject_name != nullptr) {
    char name[BUFSIZ];
    auto name_len = X509_NAME_get_text_by_NID(subject_name, NID_commonName,
                                              name, sizeof(name));

    if (name_len != -1) {
      return detail::match_hostname(
          std::string(name, static_cast<size_t>(name_len)), host_);
    }
  }

  return false;
}

#endif // CPPHTTPLIB_OPENSSL_SUPPORT

/*
 * Group 9: TLS abstraction layer - Mbed TLS backend
 */

/*
 * Mbed TLS Backend Implementation
 */

#ifdef CPPHTTPLIB_MBEDTLS_SUPPORT
namespace tls {

namespace impl {

// Mbed TLS session wrapper
struct MbedTlsSession {
  mbedtls_ssl_context ssl;
  socket_t sock = INVALID_SOCKET;
  std::string hostname;     // For client: set via set_sni
  std::string sni_hostname; // For server: received from client via SNI callback

  MbedTlsSession() { mbedtls_ssl_init(&ssl); }

  ~MbedTlsSession() { mbedtls_ssl_free(&ssl); }

  MbedTlsSession(const MbedTlsSession &) = delete;
  MbedTlsSession &operator=(const MbedTlsSession &) = delete;
};

// Thread-local error code accessor for Mbed TLS (since it doesn't have an error
// queue)
int &mbedtls_last_error() {
  static thread_local int err = 0;
  return err;
}

// Helper to map Mbed TLS error to ErrorCode
ErrorCode map_mbedtls_error(int ret, int &out_errno) {
  if (ret == 0) { return ErrorCode::Success; }
  if (ret == MBEDTLS_ERR_SSL_WANT_READ) { return ErrorCode::WantRead; }
  if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) { return ErrorCode::WantWrite; }
  if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
    return ErrorCode::PeerClosed;
  }
  if (ret == MBEDTLS_ERR_NET_CONN_RESET || ret == MBEDTLS_ERR_NET_SEND_FAILED ||
      ret == MBEDTLS_ERR_NET_RECV_FAILED) {
    out_errno = errno;
    return ErrorCode::SyscallError;
  }
  if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
    return ErrorCode::CertVerifyFailed;
  }
  return ErrorCode::Fatal;
}

// BIO-like send callback for Mbed TLS
int mbedtls_net_send_cb(void *ctx, const unsigned char *buf,
                               size_t len) {
  auto sock = *static_cast<socket_t *>(ctx);
#ifdef _WIN32
  auto ret =
      send(sock, reinterpret_cast<const char *>(buf), static_cast<int>(len), 0);
  if (ret == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) { return MBEDTLS_ERR_SSL_WANT_WRITE; }
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
#else
  auto ret = send(sock, buf, len, 0);
  if (ret < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
#endif
  return static_cast<int>(ret);
}

// BIO-like recv callback for Mbed TLS
int mbedtls_net_recv_cb(void *ctx, unsigned char *buf, size_t len) {
  auto sock = *static_cast<socket_t *>(ctx);
#ifdef _WIN32
  auto ret =
      recv(sock, reinterpret_cast<char *>(buf), static_cast<int>(len), 0);
  if (ret == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) { return MBEDTLS_ERR_SSL_WANT_READ; }
    return MBEDTLS_ERR_NET_RECV_FAILED;
  }
#else
  auto ret = recv(sock, buf, len, 0);
  if (ret < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
  }
#endif
  if (ret == 0) { return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY; }
  return static_cast<int>(ret);
}

// MbedTlsContext constructor/destructor implementations
MbedTlsContext::MbedTlsContext() {
  mbedtls_ssl_config_init(&conf);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_x509_crt_init(&ca_chain);
  mbedtls_x509_crt_init(&own_cert);
  mbedtls_pk_init(&own_key);
}

MbedTlsContext::~MbedTlsContext() {
  mbedtls_pk_free(&own_key);
  mbedtls_x509_crt_free(&own_cert);
  mbedtls_x509_crt_free(&ca_chain);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  mbedtls_ssl_config_free(&conf);
}

// Thread-local storage for SNI captured during handshake
// This is needed because the SNI callback doesn't have a way to pass
// session-specific data before the session is fully set up
std::string &mbedpending_sni() {
  static thread_local std::string sni;
  return sni;
}

// SNI callback for Mbed TLS server to capture client's SNI hostname
int mbedtls_sni_callback(void *p_ctx, mbedtls_ssl_context *ssl,
                                const unsigned char *name, size_t name_len) {
  (void)p_ctx;
  (void)ssl;

  // Store SNI name in thread-local storage
  // It will be retrieved and stored in the session after handshake
  if (name && name_len > 0) {
    mbedpending_sni().assign(reinterpret_cast<const char *>(name), name_len);
  } else {
    mbedpending_sni().clear();
  }
  return 0; // Accept any SNI
}

int mbedtls_verify_callback(void *data, mbedtls_x509_crt *crt,
                                   int cert_depth, uint32_t *flags);

// MbedTLS verify callback wrapper
int mbedtls_verify_callback(void *data, mbedtls_x509_crt *crt,
                                   int cert_depth, uint32_t *flags) {
  auto &callback = get_verify_callback();
  if (!callback) { return 0; } // Continue with default verification

  // data points to the MbedTlsSession
  auto *session = static_cast<MbedTlsSession *>(data);

  // Build context
  VerifyContext verify_ctx;
  verify_ctx.session = static_cast<session_t>(session);
  verify_ctx.cert = static_cast<cert_t>(crt);
  verify_ctx.depth = cert_depth;
  verify_ctx.preverify_ok = (*flags == 0);
  verify_ctx.error_code = static_cast<long>(*flags);

  // Convert Mbed TLS flags to error string
  static thread_local char error_buf[256];
  if (*flags != 0) {
    mbedtls_x509_crt_verify_info(error_buf, sizeof(error_buf), "", *flags);
    verify_ctx.error_string = error_buf;
  } else {
    verify_ctx.error_string = nullptr;
  }

  bool accepted = callback(verify_ctx);

  if (accepted) {
    *flags = 0; // Clear all error flags
    return 0;
  }
  return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
}

} // namespace impl

ctx_t create_client_context() {
  auto ctx = new (std::nothrow) impl::MbedTlsContext();
  if (!ctx) { return nullptr; }

  ctx->is_server = false;

  // Seed the random number generator
  const char *pers = "httplib_client";
  int ret = mbedtls_ctr_drbg_seed(
      &ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy,
      reinterpret_cast<const unsigned char *>(pers), strlen(pers));
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    delete ctx;
    return nullptr;
  }

  // Set up SSL config for client
  ret = mbedtls_ssl_config_defaults(&ctx->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    delete ctx;
    return nullptr;
  }

  // Set random number generator
  mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

  // Default: verify peer certificate
  mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);

  // Set minimum TLS version to 1.2
#ifdef CPPHTTPLIB_MBEDTLS_V3
  mbedtls_ssl_conf_min_tls_version(&ctx->conf, MBEDTLS_SSL_VERSION_TLS1_2);
#else
  mbedtls_ssl_conf_min_version(&ctx->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                               MBEDTLS_SSL_MINOR_VERSION_3);
#endif

  return static_cast<ctx_t>(ctx);
}

ctx_t create_server_context() {
  auto ctx = new (std::nothrow) impl::MbedTlsContext();
  if (!ctx) { return nullptr; }

  ctx->is_server = true;

  // Seed the random number generator
  const char *pers = "httplib_server";
  int ret = mbedtls_ctr_drbg_seed(
      &ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy,
      reinterpret_cast<const unsigned char *>(pers), strlen(pers));
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    delete ctx;
    return nullptr;
  }

  // Set up SSL config for server
  ret = mbedtls_ssl_config_defaults(&ctx->conf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    delete ctx;
    return nullptr;
  }

  // Set random number generator
  mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

  // Default: don't verify client
  mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);

  // Set minimum TLS version to 1.2
#ifdef CPPHTTPLIB_MBEDTLS_V3
  mbedtls_ssl_conf_min_tls_version(&ctx->conf, MBEDTLS_SSL_VERSION_TLS1_2);
#else
  mbedtls_ssl_conf_min_version(&ctx->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                               MBEDTLS_SSL_MINOR_VERSION_3);
#endif

  // Set SNI callback to capture client's SNI hostname
  mbedtls_ssl_conf_sni(&ctx->conf, impl::mbedtls_sni_callback, nullptr);

  return static_cast<ctx_t>(ctx);
}

void free_context(ctx_t ctx) {
  if (ctx) { delete static_cast<impl::MbedTlsContext *>(ctx); }
}

bool set_min_version(ctx_t ctx, Version version) {
  if (!ctx) { return false; }
  auto mctx = static_cast<impl::MbedTlsContext *>(ctx);

#ifdef CPPHTTPLIB_MBEDTLS_V3
  // Mbed TLS 3.x uses mbedtls_ssl_protocol_version enum
  mbedtls_ssl_protocol_version min_ver = MBEDTLS_SSL_VERSION_TLS1_2;
  if (version >= Version::TLS1_3) {
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    min_ver = MBEDTLS_SSL_VERSION_TLS1_3;
#endif
  }
  mbedtls_ssl_conf_min_tls_version(&mctx->conf, min_ver);
#else
  // Mbed TLS 2.x uses major/minor version numbers
  int major = MBEDTLS_SSL_MAJOR_VERSION_3;
  int minor = MBEDTLS_SSL_MINOR_VERSION_3; // TLS 1.2
  if (version >= Version::TLS1_3) {
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    minor = MBEDTLS_SSL_MINOR_VERSION_4; // TLS 1.3
#else
    minor = MBEDTLS_SSL_MINOR_VERSION_3; // Fall back to TLS 1.2
#endif
  }
  mbedtls_ssl_conf_min_version(&mctx->conf, major, minor);
#endif
  return true;
}

bool load_ca_pem(ctx_t ctx, const char *pem, size_t len) {
  if (!ctx || !pem) { return false; }
  auto mctx = static_cast<impl::MbedTlsContext *>(ctx);

  // mbedtls_x509_crt_parse expects null-terminated string for PEM
  // Add null terminator if not present
  std::string pem_str(pem, len);
  int ret = mbedtls_x509_crt_parse(
      &mctx->ca_chain, reinterpret_cast<const unsigned char *>(pem_str.c_str()),
      pem_str.size() + 1);
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  mbedtls_ssl_conf_ca_chain(&mctx->conf, &mctx->ca_chain, nullptr);
  return true;
}

bool load_ca_file(ctx_t ctx, const char *file_path) {
  if (!ctx || !file_path) { return false; }
  auto mctx = static_cast<impl::MbedTlsContext *>(ctx);

  int ret = mbedtls_x509_crt_parse_file(&mctx->ca_chain, file_path);
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  mbedtls_ssl_conf_ca_chain(&mctx->conf, &mctx->ca_chain, nullptr);
  return true;
}

bool load_ca_dir(ctx_t ctx, const char *dir_path) {
  if (!ctx || !dir_path) { return false; }
  auto mctx = static_cast<impl::MbedTlsContext *>(ctx);

  int ret = mbedtls_x509_crt_parse_path(&mctx->ca_chain, dir_path);
  if (ret < 0) { // Returns number of certs on success, negative on error
    impl::mbedtls_last_error() = ret;
    return false;
  }

  mbedtls_ssl_conf_ca_chain(&mctx->conf, &mctx->ca_chain, nullptr);
  return true;
}

bool load_system_certs(ctx_t ctx) {
  if (!ctx) { return false; }
  auto mctx = static_cast<impl::MbedTlsContext *>(ctx);
  bool loaded = false;

#ifdef _WIN32
  loaded = impl::enumerate_windows_system_certs(
      [&](const unsigned char *data, size_t len) {
        return mbedtls_x509_crt_parse_der(&mctx->ca_chain, data, len) == 0;
      });
#elif defined(__APPLE__) && defined(CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN)
  loaded = impl::enumerate_macos_keychain_certs(
      [&](const unsigned char *data, size_t len) {
        return mbedtls_x509_crt_parse_der(&mctx->ca_chain, data, len) == 0;
      });
#else
  for (auto path = impl::system_ca_paths(); *path; ++path) {
    if (mbedtls_x509_crt_parse_file(&mctx->ca_chain, *path) >= 0) {
      loaded = true;
      break;
    }
  }

  if (!loaded) {
    for (auto dir = impl::system_ca_dirs(); *dir; ++dir) {
      if (mbedtls_x509_crt_parse_path(&mctx->ca_chain, *dir) >= 0) {
        loaded = true;
        break;
      }
    }
  }
#endif

  if (loaded) {
    mbedtls_ssl_conf_ca_chain(&mctx->conf, &mctx->ca_chain, nullptr);
  }
  return loaded;
}

bool set_client_cert_pem(ctx_t ctx, const char *cert, const char *key,
                                const char *password) {
  if (!ctx || !cert || !key) { return false; }
  auto mctx = static_cast<impl::MbedTlsContext *>(ctx);

  // Parse certificate
  std::string cert_str(cert);
  int ret = mbedtls_x509_crt_parse(
      &mctx->own_cert,
      reinterpret_cast<const unsigned char *>(cert_str.c_str()),
      cert_str.size() + 1);
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  // Parse private key
  std::string key_str(key);
  const unsigned char *pwd =
      password ? reinterpret_cast<const unsigned char *>(password) : nullptr;
  size_t pwd_len = password ? strlen(password) : 0;

#ifdef CPPHTTPLIB_MBEDTLS_V3
  ret = mbedtls_pk_parse_key(
      &mctx->own_key, reinterpret_cast<const unsigned char *>(key_str.c_str()),
      key_str.size() + 1, pwd, pwd_len, mbedtls_ctr_drbg_random,
      &mctx->ctr_drbg);
#else
  ret = mbedtls_pk_parse_key(
      &mctx->own_key, reinterpret_cast<const unsigned char *>(key_str.c_str()),
      key_str.size() + 1, pwd, pwd_len);
#endif
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  // Verify that the certificate and private key match
#ifdef CPPHTTPLIB_MBEDTLS_V3
  ret = mbedtls_pk_check_pair(&mctx->own_cert.pk, &mctx->own_key,
                              mbedtls_ctr_drbg_random, &mctx->ctr_drbg);
#else
  ret = mbedtls_pk_check_pair(&mctx->own_cert.pk, &mctx->own_key);
#endif
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  ret = mbedtls_ssl_conf_own_cert(&mctx->conf, &mctx->own_cert, &mctx->own_key);
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  return true;
}

bool set_client_cert_file(ctx_t ctx, const char *cert_path,
                                 const char *key_path, const char *password) {
  if (!ctx || !cert_path || !key_path) { return false; }
  auto mctx = static_cast<impl::MbedTlsContext *>(ctx);

  // Parse certificate file
  int ret = mbedtls_x509_crt_parse_file(&mctx->own_cert, cert_path);
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  // Parse private key file
#ifdef CPPHTTPLIB_MBEDTLS_V3
  ret = mbedtls_pk_parse_keyfile(&mctx->own_key, key_path, password,
                                 mbedtls_ctr_drbg_random, &mctx->ctr_drbg);
#else
  ret = mbedtls_pk_parse_keyfile(&mctx->own_key, key_path, password);
#endif
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  // Verify that the certificate and private key match
#ifdef CPPHTTPLIB_MBEDTLS_V3
  ret = mbedtls_pk_check_pair(&mctx->own_cert.pk, &mctx->own_key,
                              mbedtls_ctr_drbg_random, &mctx->ctr_drbg);
#else
  ret = mbedtls_pk_check_pair(&mctx->own_cert.pk, &mctx->own_key);
#endif
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  ret = mbedtls_ssl_conf_own_cert(&mctx->conf, &mctx->own_cert, &mctx->own_key);
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  return true;
}

void set_verify_client(ctx_t ctx, bool require) {
  if (!ctx) { return; }
  auto mctx = static_cast<impl::MbedTlsContext *>(ctx);
  mctx->verify_client = require;
  if (require) {
    mbedtls_ssl_conf_authmode(&mctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  } else {
    // If a verify callback is set, use OPTIONAL mode to ensure the callback
    // is called (matching OpenSSL behavior). Otherwise use NONE.
    mbedtls_ssl_conf_authmode(&mctx->conf, mctx->has_verify_callback
                                               ? MBEDTLS_SSL_VERIFY_OPTIONAL
                                               : MBEDTLS_SSL_VERIFY_NONE);
  }
}

session_t create_session(ctx_t ctx, socket_t sock) {
  if (!ctx || sock == INVALID_SOCKET) { return nullptr; }
  auto mctx = static_cast<impl::MbedTlsContext *>(ctx);

  auto session = new (std::nothrow) impl::MbedTlsSession();
  if (!session) { return nullptr; }

  session->sock = sock;

  int ret = mbedtls_ssl_setup(&session->ssl, &mctx->conf);
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    delete session;
    return nullptr;
  }

  // Set BIO callbacks
  mbedtls_ssl_set_bio(&session->ssl, &session->sock, impl::mbedtls_net_send_cb,
                      impl::mbedtls_net_recv_cb, nullptr);

  // Set per-session verify callback with session pointer if callback is
  // registered
  if (mctx->has_verify_callback) {
    mbedtls_ssl_set_verify(&session->ssl, impl::mbedtls_verify_callback,
                           session);
  }

  return static_cast<session_t>(session);
}

void free_session(session_t session) {
  if (session) { delete static_cast<impl::MbedTlsSession *>(session); }
}

bool set_sni(session_t session, const char *hostname) {
  if (!session || !hostname) { return false; }
  auto msession = static_cast<impl::MbedTlsSession *>(session);

  int ret = mbedtls_ssl_set_hostname(&msession->ssl, hostname);
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  msession->hostname = hostname;
  return true;
}

bool set_hostname(session_t session, const char *hostname) {
  // In Mbed TLS, set_hostname also sets up hostname verification
  return set_sni(session, hostname);
}

TlsError connect(session_t session) {
  TlsError err;
  if (!session) {
    err.code = ErrorCode::Fatal;
    return err;
  }

  auto msession = static_cast<impl::MbedTlsSession *>(session);
  int ret = mbedtls_ssl_handshake(&msession->ssl);

  if (ret == 0) {
    err.code = ErrorCode::Success;
  } else {
    err.code = impl::map_mbedtls_error(ret, err.sys_errno);
    err.backend_code = static_cast<uint64_t>(-ret);
    impl::mbedtls_last_error() = ret;
  }

  return err;
}

TlsError accept(session_t session) {
  // Same as connect for Mbed TLS - handshake works for both client and server
  auto result = connect(session);

  // After successful handshake, capture SNI from thread-local storage
  if (result.code == ErrorCode::Success && session) {
    auto msession = static_cast<impl::MbedTlsSession *>(session);
    msession->sni_hostname = std::move(impl::mbedpending_sni());
    impl::mbedpending_sni().clear();
  }

  return result;
}

bool connect_nonblocking(session_t session, socket_t sock,
                                time_t timeout_sec, time_t timeout_usec,
                                TlsError *err) {
  if (!session) {
    if (err) { err->code = ErrorCode::Fatal; }
    return false;
  }

  auto msession = static_cast<impl::MbedTlsSession *>(session);

  // Set socket to non-blocking mode
  detail::set_nonblocking(sock, true);
  auto cleanup =
      detail::scope_exit([&]() { detail::set_nonblocking(sock, false); });

  int ret;
  while ((ret = mbedtls_ssl_handshake(&msession->ssl)) != 0) {
    if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
      if (detail::select_read(sock, timeout_sec, timeout_usec) > 0) {
        continue;
      }
    } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      if (detail::select_write(sock, timeout_sec, timeout_usec) > 0) {
        continue;
      }
    }

    // TlsError or timeout
    if (err) {
      err->code = impl::map_mbedtls_error(ret, err->sys_errno);
      err->backend_code = static_cast<uint64_t>(-ret);
    }
    impl::mbedtls_last_error() = ret;
    return false;
  }

  if (err) { err->code = ErrorCode::Success; }
  return true;
}

bool accept_nonblocking(session_t session, socket_t sock,
                               time_t timeout_sec, time_t timeout_usec,
                               TlsError *err) {
  // Same implementation as connect for Mbed TLS
  bool result =
      connect_nonblocking(session, sock, timeout_sec, timeout_usec, err);

  // After successful handshake, capture SNI from thread-local storage
  if (result && session) {
    auto msession = static_cast<impl::MbedTlsSession *>(session);
    msession->sni_hostname = std::move(impl::mbedpending_sni());
    impl::mbedpending_sni().clear();
  }

  return result;
}

ssize_t read(session_t session, void *buf, size_t len, TlsError &err) {
  if (!session || !buf) {
    err.code = ErrorCode::Fatal;
    return -1;
  }

  auto msession = static_cast<impl::MbedTlsSession *>(session);
  int ret =
      mbedtls_ssl_read(&msession->ssl, static_cast<unsigned char *>(buf), len);

  if (ret > 0) {
    err.code = ErrorCode::Success;
    return static_cast<ssize_t>(ret);
  }

  if (ret == 0) {
    err.code = ErrorCode::PeerClosed;
    return 0;
  }

  err.code = impl::map_mbedtls_error(ret, err.sys_errno);
  err.backend_code = static_cast<uint64_t>(-ret);
  impl::mbedtls_last_error() = ret;
  return -1;
}

ssize_t write(session_t session, const void *buf, size_t len,
                     TlsError &err) {
  if (!session || !buf) {
    err.code = ErrorCode::Fatal;
    return -1;
  }

  auto msession = static_cast<impl::MbedTlsSession *>(session);
  int ret = mbedtls_ssl_write(&msession->ssl,
                              static_cast<const unsigned char *>(buf), len);

  if (ret > 0) {
    err.code = ErrorCode::Success;
    return static_cast<ssize_t>(ret);
  }

  if (ret == 0) {
    err.code = ErrorCode::PeerClosed;
    return 0;
  }

  err.code = impl::map_mbedtls_error(ret, err.sys_errno);
  err.backend_code = static_cast<uint64_t>(-ret);
  impl::mbedtls_last_error() = ret;
  return -1;
}

int pending(const_session_t session) {
  if (!session) { return 0; }
  auto msession =
      static_cast<impl::MbedTlsSession *>(const_cast<void *>(session));
  return static_cast<int>(mbedtls_ssl_get_bytes_avail(&msession->ssl));
}

void shutdown(session_t session, bool graceful) {
  if (!session) { return; }
  auto msession = static_cast<impl::MbedTlsSession *>(session);

  if (graceful) {
    // Try to send close_notify, but don't block forever
    int ret;
    int attempts = 0;
    while ((ret = mbedtls_ssl_close_notify(&msession->ssl)) != 0 &&
           attempts < 3) {
      if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
          ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
        break;
      }
      attempts++;
    }
  }
}

bool is_peer_closed(session_t session, socket_t sock) {
  if (!session || sock == INVALID_SOCKET) { return true; }
  auto msession = static_cast<impl::MbedTlsSession *>(session);

  // Check if there's already decrypted data available in the TLS buffer
  // If so, the connection is definitely alive
  if (mbedtls_ssl_get_bytes_avail(&msession->ssl) > 0) { return false; }

  // Set socket to non-blocking to avoid blocking on read
  detail::set_nonblocking(sock, true);
  auto cleanup =
      detail::scope_exit([&]() { detail::set_nonblocking(sock, false); });

  // Try a 1-byte read to check connection status
  // Note: This will consume the byte if data is available, but for the
  // purpose of checking if peer is closed, this should be acceptable
  // since we're only called when we expect the connection might be closing
  unsigned char buf;
  int ret = mbedtls_ssl_read(&msession->ssl, &buf, 1);

  // If we got data or WANT_READ (would block), connection is alive
  if (ret > 0 || ret == MBEDTLS_ERR_SSL_WANT_READ) { return false; }

  // If we get a peer close notify or a connection reset, the peer is closed
  return ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
         ret == MBEDTLS_ERR_NET_CONN_RESET || ret == 0;
}

cert_t get_peer_cert(const_session_t session) {
  if (!session) { return nullptr; }
  auto msession =
      static_cast<impl::MbedTlsSession *>(const_cast<void *>(session));

  // Mbed TLS returns a pointer to the internal peer cert chain.
  // WARNING: This pointer is only valid while the session is active.
  // Do not use the certificate after calling free_session().
  const mbedtls_x509_crt *cert = mbedtls_ssl_get_peer_cert(&msession->ssl);
  return const_cast<mbedtls_x509_crt *>(cert);
}

void free_cert(cert_t cert) {
  // Mbed TLS: peer certificate is owned by the SSL context.
  // No-op here, but callers should still call this for cross-backend
  // portability.
  (void)cert;
}

bool verify_hostname(cert_t cert, const char *hostname) {
  if (!cert || !hostname) { return false; }
  auto mcert = static_cast<const mbedtls_x509_crt *>(cert);
  std::string host_str(hostname);

  // Check if hostname is an IP address
  bool is_ip = impl::is_ipv4_address(host_str);
  unsigned char ip_bytes[4];
  if (is_ip) { impl::parse_ipv4(host_str, ip_bytes); }

  // Check Subject Alternative Names (SAN)
  // In Mbed TLS 3.x, subject_alt_names contains raw values without ASN.1 tags
  // - DNS names: raw string bytes
  // - IP addresses: raw IP bytes (4 for IPv4, 16 for IPv6)
  const mbedtls_x509_sequence *san = &mcert->subject_alt_names;
  while (san != nullptr && san->buf.p != nullptr && san->buf.len > 0) {
    const unsigned char *p = san->buf.p;
    size_t len = san->buf.len;

    if (is_ip) {
      // Check if this SAN is an IPv4 address (4 bytes)
      if (len == 4 && memcmp(p, ip_bytes, 4) == 0) { return true; }
      // Check if this SAN is an IPv6 address (16 bytes) - skip for now
    } else {
      // Check if this SAN is a DNS name (printable ASCII string)
      bool is_dns = len > 0;
      for (size_t i = 0; i < len && is_dns; i++) {
        if (p[i] < 32 || p[i] > 126) { is_dns = false; }
      }
      if (is_dns) {
        std::string san_name(reinterpret_cast<const char *>(p), len);
        if (detail::match_hostname(san_name, host_str)) { return true; }
      }
    }
    san = san->next;
  }

  // Fallback: Check Common Name (CN) in subject
  char cn[256];
  int ret = mbedtls_x509_dn_gets(cn, sizeof(cn), &mcert->subject);
  if (ret > 0) {
    std::string cn_str(cn);

    // Look for "CN=" in the DN string
    size_t cn_pos = cn_str.find("CN=");
    if (cn_pos != std::string::npos) {
      size_t start = cn_pos + 3;
      size_t end = cn_str.find(',', start);
      std::string cn_value =
          cn_str.substr(start, end == std::string::npos ? end : end - start);

      if (detail::match_hostname(cn_value, host_str)) { return true; }
    }
  }

  return false;
}

uint64_t hostname_mismatch_code() {
  return static_cast<uint64_t>(MBEDTLS_X509_BADCERT_CN_MISMATCH);
}

long get_verify_result(const_session_t session) {
  if (!session) { return -1; }
  auto msession =
      static_cast<impl::MbedTlsSession *>(const_cast<void *>(session));
  uint32_t flags = mbedtls_ssl_get_verify_result(&msession->ssl);
  // Return 0 (X509_V_OK equivalent) if verification passed
  return flags == 0 ? 0 : static_cast<long>(flags);
}

std::string get_cert_subject_cn(cert_t cert) {
  if (!cert) return "";
  auto x509 = static_cast<mbedtls_x509_crt *>(cert);

  // Find the CN in the subject
  const mbedtls_x509_name *name = &x509->subject;
  while (name != nullptr) {
    if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &name->oid) == 0) {
      return std::string(reinterpret_cast<const char *>(name->val.p),
                         name->val.len);
    }
    name = name->next;
  }
  return "";
}

std::string get_cert_issuer_name(cert_t cert) {
  if (!cert) return "";
  auto x509 = static_cast<mbedtls_x509_crt *>(cert);

  // Build a human-readable issuer name string
  char buf[512];
  int ret = mbedtls_x509_dn_gets(buf, sizeof(buf), &x509->issuer);
  if (ret < 0) return "";
  return std::string(buf);
}

bool get_cert_sans(cert_t cert, std::vector<SanEntry> &sans) {
  sans.clear();
  if (!cert) return false;
  auto x509 = static_cast<mbedtls_x509_crt *>(cert);

  // Parse the Subject Alternative Name extension
  const mbedtls_x509_sequence *cur = &x509->subject_alt_names;
  while (cur != nullptr) {
    if (cur->buf.len > 0) {
      // Mbed TLS stores SAN as ASN.1 sequences
      // The tag byte indicates the type
      const unsigned char *p = cur->buf.p;
      size_t len = cur->buf.len;

      // First byte is the tag
      unsigned char tag = *p;
      p++;
      len--;

      // Parse length (simple single-byte length assumed)
      if (len > 0 && *p < 0x80) {
        size_t value_len = *p;
        p++;
        len--;

        if (value_len <= len) {
          SanEntry entry;
          // ASN.1 context tags for GeneralName
          switch (tag & 0x1F) {
          case 2: // dNSName
            entry.type = SanType::DNS;
            entry.value =
                std::string(reinterpret_cast<const char *>(p), value_len);
            break;
          case 7: // iPAddress
            entry.type = SanType::IP;
            if (value_len == 4) {
              // IPv4
              char buf[16];
              snprintf(buf, sizeof(buf), "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
              entry.value = buf;
            } else if (value_len == 16) {
              // IPv6
              char buf[64];
              snprintf(buf, sizeof(buf),
                       "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                       "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                       p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8],
                       p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
              entry.value = buf;
            }
            break;
          case 1: // rfc822Name (email)
            entry.type = SanType::EMAIL;
            entry.value =
                std::string(reinterpret_cast<const char *>(p), value_len);
            break;
          case 6: // uniformResourceIdentifier
            entry.type = SanType::URI;
            entry.value =
                std::string(reinterpret_cast<const char *>(p), value_len);
            break;
          default: entry.type = SanType::OTHER; break;
          }

          if (!entry.value.empty()) { sans.push_back(std::move(entry)); }
        }
      }
    }
    cur = cur->next;
  }
  return true;
}

bool get_cert_validity(cert_t cert, time_t &not_before,
                              time_t &not_after) {
  if (!cert) return false;
  auto x509 = static_cast<mbedtls_x509_crt *>(cert);

  // Convert mbedtls_x509_time to time_t
  auto to_time_t = [](const mbedtls_x509_time &t) -> time_t {
    struct tm tm_time = {};
    tm_time.tm_year = t.year - 1900;
    tm_time.tm_mon = t.mon - 1;
    tm_time.tm_mday = t.day;
    tm_time.tm_hour = t.hour;
    tm_time.tm_min = t.min;
    tm_time.tm_sec = t.sec;
#ifdef _WIN32
    return _mkgmtime(&tm_time);
#else
    return timegm(&tm_time);
#endif
  };

  not_before = to_time_t(x509->valid_from);
  not_after = to_time_t(x509->valid_to);
  return true;
}

std::string get_cert_serial(cert_t cert) {
  if (!cert) return "";
  auto x509 = static_cast<mbedtls_x509_crt *>(cert);

  // Convert serial number to hex string
  std::string result;
  result.reserve(x509->serial.len * 2);
  for (size_t i = 0; i < x509->serial.len; i++) {
    char hex[3];
    snprintf(hex, sizeof(hex), "%02X", x509->serial.p[i]);
    result += hex;
  }
  return result;
}

bool get_cert_der(cert_t cert, std::vector<unsigned char> &der) {
  if (!cert) return false;
  auto crt = static_cast<mbedtls_x509_crt *>(cert);
  if (!crt->raw.p || crt->raw.len == 0) return false;
  der.assign(crt->raw.p, crt->raw.p + crt->raw.len);
  return true;
}

const char *get_sni(const_session_t session) {
  if (!session) return nullptr;
  auto msession = static_cast<const impl::MbedTlsSession *>(session);

  // For server: return SNI received from client during handshake
  if (!msession->sni_hostname.empty()) {
    return msession->sni_hostname.c_str();
  }

  // For client: return the hostname set via set_sni
  if (!msession->hostname.empty()) { return msession->hostname.c_str(); }

  return nullptr;
}

uint64_t peek_error() {
  // Mbed TLS doesn't have an error queue, return the last error
  return static_cast<uint64_t>(-impl::mbedtls_last_error());
}

uint64_t get_error() {
  // Mbed TLS doesn't have an error queue, return and clear the last error
  uint64_t err = static_cast<uint64_t>(-impl::mbedtls_last_error());
  impl::mbedtls_last_error() = 0;
  return err;
}

std::string error_string(uint64_t code) {
  char buf[256];
  mbedtls_strerror(-static_cast<int>(code), buf, sizeof(buf));
  return std::string(buf);
}

ca_store_t create_ca_store(const char *pem, size_t len) {
  auto *ca_chain = new (std::nothrow) mbedtls_x509_crt;
  if (!ca_chain) { return nullptr; }

  mbedtls_x509_crt_init(ca_chain);

  // mbedtls_x509_crt_parse expects null-terminated PEM
  int ret = mbedtls_x509_crt_parse(ca_chain,
                                   reinterpret_cast<const unsigned char *>(pem),
                                   len + 1); // +1 for null terminator
  if (ret != 0) {
    // Try without +1 in case PEM is already null-terminated
    ret = mbedtls_x509_crt_parse(
        ca_chain, reinterpret_cast<const unsigned char *>(pem), len);
    if (ret != 0) {
      mbedtls_x509_crt_free(ca_chain);
      delete ca_chain;
      return nullptr;
    }
  }

  return static_cast<ca_store_t>(ca_chain);
}

void free_ca_store(ca_store_t store) {
  if (store) {
    auto *ca_chain = static_cast<mbedtls_x509_crt *>(store);
    mbedtls_x509_crt_free(ca_chain);
    delete ca_chain;
  }
}

bool set_ca_store(ctx_t ctx, ca_store_t store) {
  if (!ctx || !store) { return false; }
  auto *mbed_ctx = static_cast<impl::MbedTlsContext *>(ctx);
  auto *ca_chain = static_cast<mbedtls_x509_crt *>(store);

  // Free existing CA chain
  mbedtls_x509_crt_free(&mbed_ctx->ca_chain);
  mbedtls_x509_crt_init(&mbed_ctx->ca_chain);

  // Copy the CA chain (deep copy)
  // Parse from the raw data of the source cert
  mbedtls_x509_crt *src = ca_chain;
  while (src != nullptr) {
    int ret = mbedtls_x509_crt_parse_der(&mbed_ctx->ca_chain, src->raw.p,
                                         src->raw.len);
    if (ret != 0) { return false; }
    src = src->next;
  }

  // Update the SSL config to use the new CA chain
  mbedtls_ssl_conf_ca_chain(&mbed_ctx->conf, &mbed_ctx->ca_chain, nullptr);
  return true;
}

size_t get_ca_certs(ctx_t ctx, std::vector<cert_t> &certs) {
  certs.clear();
  if (!ctx) { return 0; }
  auto *mbed_ctx = static_cast<impl::MbedTlsContext *>(ctx);

  // Iterate through the CA chain
  mbedtls_x509_crt *cert = &mbed_ctx->ca_chain;
  while (cert != nullptr && cert->raw.len > 0) {
    // Create a copy of the certificate for the caller
    auto *copy = new mbedtls_x509_crt;
    mbedtls_x509_crt_init(copy);
    int ret = mbedtls_x509_crt_parse_der(copy, cert->raw.p, cert->raw.len);
    if (ret == 0) {
      certs.push_back(static_cast<cert_t>(copy));
    } else {
      mbedtls_x509_crt_free(copy);
      delete copy;
    }
    cert = cert->next;
  }
  return certs.size();
}

std::vector<std::string> get_ca_names(ctx_t ctx) {
  std::vector<std::string> names;
  if (!ctx) { return names; }
  auto *mbed_ctx = static_cast<impl::MbedTlsContext *>(ctx);

  // Iterate through the CA chain
  mbedtls_x509_crt *cert = &mbed_ctx->ca_chain;
  while (cert != nullptr && cert->raw.len > 0) {
    char buf[512];
    int ret = mbedtls_x509_dn_gets(buf, sizeof(buf), &cert->subject);
    if (ret > 0) { names.push_back(buf); }
    cert = cert->next;
  }
  return names;
}

bool update_server_cert(ctx_t ctx, const char *cert_pem,
                               const char *key_pem, const char *password) {
  if (!ctx || !cert_pem || !key_pem) { return false; }
  auto *mbed_ctx = static_cast<impl::MbedTlsContext *>(ctx);

  // Free existing certificate and key
  mbedtls_x509_crt_free(&mbed_ctx->own_cert);
  mbedtls_pk_free(&mbed_ctx->own_key);
  mbedtls_x509_crt_init(&mbed_ctx->own_cert);
  mbedtls_pk_init(&mbed_ctx->own_key);

  // Parse certificate PEM
  int ret = mbedtls_x509_crt_parse(
      &mbed_ctx->own_cert, reinterpret_cast<const unsigned char *>(cert_pem),
      strlen(cert_pem) + 1);
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  // Parse private key PEM
#ifdef CPPHTTPLIB_MBEDTLS_V3
  ret = mbedtls_pk_parse_key(
      &mbed_ctx->own_key, reinterpret_cast<const unsigned char *>(key_pem),
      strlen(key_pem) + 1,
      password ? reinterpret_cast<const unsigned char *>(password) : nullptr,
      password ? strlen(password) : 0, mbedtls_ctr_drbg_random,
      &mbed_ctx->ctr_drbg);
#else
  ret = mbedtls_pk_parse_key(
      &mbed_ctx->own_key, reinterpret_cast<const unsigned char *>(key_pem),
      strlen(key_pem) + 1,
      password ? reinterpret_cast<const unsigned char *>(password) : nullptr,
      password ? strlen(password) : 0);
#endif
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  // Configure SSL to use the new certificate and key
  ret = mbedtls_ssl_conf_own_cert(&mbed_ctx->conf, &mbed_ctx->own_cert,
                                  &mbed_ctx->own_key);
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  return true;
}

bool update_server_client_ca(ctx_t ctx, const char *ca_pem) {
  if (!ctx || !ca_pem) { return false; }
  auto *mbed_ctx = static_cast<impl::MbedTlsContext *>(ctx);

  // Free existing CA chain
  mbedtls_x509_crt_free(&mbed_ctx->ca_chain);
  mbedtls_x509_crt_init(&mbed_ctx->ca_chain);

  // Parse CA PEM
  int ret = mbedtls_x509_crt_parse(
      &mbed_ctx->ca_chain, reinterpret_cast<const unsigned char *>(ca_pem),
      strlen(ca_pem) + 1);
  if (ret != 0) {
    impl::mbedtls_last_error() = ret;
    return false;
  }

  // Update SSL config to use new CA chain
  mbedtls_ssl_conf_ca_chain(&mbed_ctx->conf, &mbed_ctx->ca_chain, nullptr);
  return true;
}

bool set_verify_callback(ctx_t ctx, VerifyCallback callback) {
  if (!ctx) { return false; }
  auto *mbed_ctx = static_cast<impl::MbedTlsContext *>(ctx);

  impl::get_verify_callback() = std::move(callback);
  mbed_ctx->has_verify_callback =
      static_cast<bool>(impl::get_verify_callback());

  if (mbed_ctx->has_verify_callback) {
    // Set OPTIONAL mode to ensure callback is called even when verification
    // is disabled (matching OpenSSL behavior where SSL_VERIFY_PEER is set)
    mbedtls_ssl_conf_authmode(&mbed_ctx->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_verify(&mbed_ctx->conf, impl::mbedtls_verify_callback,
                            nullptr);
  } else {
    mbedtls_ssl_conf_verify(&mbed_ctx->conf, nullptr, nullptr);
  }
  return true;
}

long get_verify_error(const_session_t session) {
  if (!session) { return -1; }
  auto *msession =
      static_cast<impl::MbedTlsSession *>(const_cast<void *>(session));
  return static_cast<long>(mbedtls_ssl_get_verify_result(&msession->ssl));
}

std::string verify_error_string(long error_code) {
  if (error_code == 0) { return ""; }
  char buf[256];
  mbedtls_x509_crt_verify_info(buf, sizeof(buf), "",
                               static_cast<uint32_t>(error_code));
  // Remove trailing newline if present
  std::string result(buf);
  while (!result.empty() && (result.back() == '\n' || result.back() == ' ')) {
    result.pop_back();
  }
  return result;
}

} // namespace tls

#endif // CPPHTTPLIB_MBEDTLS_SUPPORT

/*
 * Group 10: TLS abstraction layer - wolfSSL backend
 */

/*
 * wolfSSL Backend Implementation
 */

#ifdef CPPHTTPLIB_WOLFSSL_SUPPORT
namespace tls {

namespace impl {

// wolfSSL session wrapper
struct WolfSSLSession {
  WOLFSSL *ssl = nullptr;
  socket_t sock = INVALID_SOCKET;
  std::string hostname;     // For client: set via set_sni
  std::string sni_hostname; // For server: received from client via SNI callback

  WolfSSLSession() = default;

  ~WolfSSLSession() {
    if (ssl) { wolfSSL_free(ssl); }
  }

  WolfSSLSession(const WolfSSLSession &) = delete;
  WolfSSLSession &operator=(const WolfSSLSession &) = delete;
};

// Thread-local error code accessor for wolfSSL
uint64_t &wolfssl_last_error() {
  static thread_local uint64_t err = 0;
  return err;
}

// Helper to map wolfSSL error to ErrorCode.
// ssl_error is the value from wolfSSL_get_error().
// raw_ret is the raw return value from the wolfSSL call (for low-level error).
ErrorCode map_wolfssl_error(WOLFSSL *ssl, int ssl_error,
                                   int &out_errno) {
  switch (ssl_error) {
  case SSL_ERROR_NONE: return ErrorCode::Success;
  case SSL_ERROR_WANT_READ: return ErrorCode::WantRead;
  case SSL_ERROR_WANT_WRITE: return ErrorCode::WantWrite;
  case SSL_ERROR_ZERO_RETURN: return ErrorCode::PeerClosed;
  case SSL_ERROR_SYSCALL: out_errno = errno; return ErrorCode::SyscallError;
  default:
    if (ssl) {
      // wolfSSL stores the low-level error code as a negative value.
      // DOMAIN_NAME_MISMATCH (-322) indicates hostname verification failure.
      int low_err = ssl_error; // wolfSSL_get_error returns the low-level code
      if (low_err == DOMAIN_NAME_MISMATCH) {
        return ErrorCode::HostnameMismatch;
      }
      // Check verify result to distinguish cert verification from generic SSL
      // errors.
      long vr = wolfSSL_get_verify_result(ssl);
      if (vr != 0) { return ErrorCode::CertVerifyFailed; }
    }
    return ErrorCode::Fatal;
  }
}

// WolfSSLContext constructor/destructor implementations
WolfSSLContext::WolfSSLContext() { wolfSSL_Init(); }

WolfSSLContext::~WolfSSLContext() {
  if (ctx) { wolfSSL_CTX_free(ctx); }
}

// Thread-local storage for SNI captured during handshake
std::string &wolfssl_pending_sni() {
  static thread_local std::string sni;
  return sni;
}

// SNI callback for wolfSSL server to capture client's SNI hostname
int wolfssl_sni_callback(WOLFSSL *ssl, int *ret, void *exArg) {
  (void)ret;
  (void)exArg;

  void *name_data = nullptr;
  unsigned short name_len =
      wolfSSL_SNI_GetRequest(ssl, WOLFSSL_SNI_HOST_NAME, &name_data);

  if (name_data && name_len > 0) {
    wolfssl_pending_sni().assign(static_cast<const char *>(name_data),
                                 name_len);
  } else {
    wolfssl_pending_sni().clear();
  }
  return 0; // Continue regardless
}

// wolfSSL verify callback wrapper
int wolfssl_verify_callback(int preverify_ok,
                                   WOLFSSL_X509_STORE_CTX *x509_ctx) {
  auto &callback = get_verify_callback();
  if (!callback) { return preverify_ok; }

  WOLFSSL_X509 *cert = wolfSSL_X509_STORE_CTX_get_current_cert(x509_ctx);
  int depth = wolfSSL_X509_STORE_CTX_get_error_depth(x509_ctx);
  int err = wolfSSL_X509_STORE_CTX_get_error(x509_ctx);

  // Get the WOLFSSL object from the X509_STORE_CTX
  WOLFSSL *ssl = static_cast<WOLFSSL *>(wolfSSL_X509_STORE_CTX_get_ex_data(
      x509_ctx, wolfSSL_get_ex_data_X509_STORE_CTX_idx()));

  VerifyContext verify_ctx;
  verify_ctx.session = static_cast<session_t>(ssl);
  verify_ctx.cert = static_cast<cert_t>(cert);
  verify_ctx.depth = depth;
  verify_ctx.preverify_ok = (preverify_ok != 0);
  verify_ctx.error_code = static_cast<long>(err);

  if (err != 0) {
    verify_ctx.error_string = wolfSSL_X509_verify_cert_error_string(err);
  } else {
    verify_ctx.error_string = nullptr;
  }

  bool accepted = callback(verify_ctx);
  return accepted ? 1 : 0;
}

void set_wolfssl_password_cb(WOLFSSL_CTX *ctx, const char *password) {
  wolfSSL_CTX_set_default_passwd_cb_userdata(ctx, const_cast<char *>(password));
  wolfSSL_CTX_set_default_passwd_cb(
      ctx, [](char *buf, int size, int /*rwflag*/, void *userdata) -> int {
        auto *pwd = static_cast<const char *>(userdata);
        if (!pwd) return 0;
        auto len = static_cast<int>(strlen(pwd));
        if (len > size) len = size;
        memcpy(buf, pwd, static_cast<size_t>(len));
        return len;
      });
}

} // namespace impl

ctx_t create_client_context() {
  auto ctx = new (std::nothrow) impl::WolfSSLContext();
  if (!ctx) { return nullptr; }

  ctx->is_server = false;

  WOLFSSL_METHOD *method = wolfTLSv1_2_client_method();
  if (!method) {
    delete ctx;
    return nullptr;
  }

  ctx->ctx = wolfSSL_CTX_new(method);
  if (!ctx->ctx) {
    delete ctx;
    return nullptr;
  }

  // Default: verify peer certificate
  wolfSSL_CTX_set_verify(ctx->ctx, SSL_VERIFY_PEER, nullptr);

  return static_cast<ctx_t>(ctx);
}

ctx_t create_server_context() {
  auto ctx = new (std::nothrow) impl::WolfSSLContext();
  if (!ctx) { return nullptr; }

  ctx->is_server = true;

  WOLFSSL_METHOD *method = wolfTLSv1_2_server_method();
  if (!method) {
    delete ctx;
    return nullptr;
  }

  ctx->ctx = wolfSSL_CTX_new(method);
  if (!ctx->ctx) {
    delete ctx;
    return nullptr;
  }

  // Default: don't verify client
  wolfSSL_CTX_set_verify(ctx->ctx, SSL_VERIFY_NONE, nullptr);

  // Enable SNI on server
  wolfSSL_CTX_SNI_SetOptions(ctx->ctx, WOLFSSL_SNI_HOST_NAME,
                             WOLFSSL_SNI_CONTINUE_ON_MISMATCH);
  wolfSSL_CTX_set_servername_callback(ctx->ctx, impl::wolfssl_sni_callback);

  return static_cast<ctx_t>(ctx);
}

void free_context(ctx_t ctx) {
  if (ctx) { delete static_cast<impl::WolfSSLContext *>(ctx); }
}

bool set_min_version(ctx_t ctx, Version version) {
  if (!ctx) { return false; }
  auto wctx = static_cast<impl::WolfSSLContext *>(ctx);

  int min_ver = WOLFSSL_TLSV1_2;
  if (version >= Version::TLS1_3) { min_ver = WOLFSSL_TLSV1_3; }

  return wolfSSL_CTX_SetMinVersion(wctx->ctx, min_ver) == WOLFSSL_SUCCESS;
}

bool load_ca_pem(ctx_t ctx, const char *pem, size_t len) {
  if (!ctx || !pem) { return false; }
  auto wctx = static_cast<impl::WolfSSLContext *>(ctx);

  int ret = wolfSSL_CTX_load_verify_buffer(
      wctx->ctx, reinterpret_cast<const unsigned char *>(pem),
      static_cast<long>(len), SSL_FILETYPE_PEM);
  if (ret != SSL_SUCCESS) {
    impl::wolfssl_last_error() =
        static_cast<uint64_t>(wolfSSL_ERR_peek_last_error());
    return false;
  }
  wctx->ca_pem_data_.append(pem, len);
  return true;
}

bool load_ca_file(ctx_t ctx, const char *file_path) {
  if (!ctx || !file_path) { return false; }
  auto wctx = static_cast<impl::WolfSSLContext *>(ctx);

  int ret = wolfSSL_CTX_load_verify_locations(wctx->ctx, file_path, nullptr);
  if (ret != SSL_SUCCESS) {
    impl::wolfssl_last_error() =
        static_cast<uint64_t>(wolfSSL_ERR_peek_last_error());
    return false;
  }
  return true;
}

bool load_ca_dir(ctx_t ctx, const char *dir_path) {
  if (!ctx || !dir_path) { return false; }
  auto wctx = static_cast<impl::WolfSSLContext *>(ctx);

  int ret = wolfSSL_CTX_load_verify_locations(wctx->ctx, nullptr, dir_path);
  // wolfSSL may fail if the directory doesn't contain properly hashed certs.
  // Unlike OpenSSL which lazily loads certs from directories, wolfSSL scans
  // immediately. Return true even on failure since the CA file may have
  // already been loaded, matching OpenSSL's lenient behavior.
  (void)ret;
  return true;
}

bool load_system_certs(ctx_t ctx) {
  if (!ctx) { return false; }
  auto wctx = static_cast<impl::WolfSSLContext *>(ctx);
  bool loaded = false;

#ifdef _WIN32
  loaded = impl::enumerate_windows_system_certs(
      [&](const unsigned char *data, size_t len) {
        return wolfSSL_CTX_load_verify_buffer(wctx->ctx, data,
                                              static_cast<long>(len),
                                              SSL_FILETYPE_ASN1) == SSL_SUCCESS;
      });
#elif defined(__APPLE__) && defined(CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN)
  loaded = impl::enumerate_macos_keychain_certs(
      [&](const unsigned char *data, size_t len) {
        return wolfSSL_CTX_load_verify_buffer(wctx->ctx, data,
                                              static_cast<long>(len),
                                              SSL_FILETYPE_ASN1) == SSL_SUCCESS;
      });
#else
  for (auto path = impl::system_ca_paths(); *path; ++path) {
    if (wolfSSL_CTX_load_verify_locations(wctx->ctx, *path, nullptr) ==
        SSL_SUCCESS) {
      loaded = true;
      break;
    }
  }

  if (!loaded) {
    for (auto dir = impl::system_ca_dirs(); *dir; ++dir) {
      if (wolfSSL_CTX_load_verify_locations(wctx->ctx, nullptr, *dir) ==
          SSL_SUCCESS) {
        loaded = true;
        break;
      }
    }
  }
#endif

  return loaded;
}

bool set_client_cert_pem(ctx_t ctx, const char *cert, const char *key,
                                const char *password) {
  if (!ctx || !cert || !key) { return false; }
  auto wctx = static_cast<impl::WolfSSLContext *>(ctx);

  // Load certificate
  int ret = wolfSSL_CTX_use_certificate_buffer(
      wctx->ctx, reinterpret_cast<const unsigned char *>(cert),
      static_cast<long>(strlen(cert)), SSL_FILETYPE_PEM);
  if (ret != SSL_SUCCESS) {
    impl::wolfssl_last_error() =
        static_cast<uint64_t>(wolfSSL_ERR_peek_last_error());
    return false;
  }

  // Set password callback if password is provided
  if (password) { impl::set_wolfssl_password_cb(wctx->ctx, password); }

  // Load private key
  ret = wolfSSL_CTX_use_PrivateKey_buffer(
      wctx->ctx, reinterpret_cast<const unsigned char *>(key),
      static_cast<long>(strlen(key)), SSL_FILETYPE_PEM);
  if (ret != SSL_SUCCESS) {
    impl::wolfssl_last_error() =
        static_cast<uint64_t>(wolfSSL_ERR_peek_last_error());
    return false;
  }

  // Verify that the certificate and private key match
  return wolfSSL_CTX_check_private_key(wctx->ctx) == SSL_SUCCESS;
}

bool set_client_cert_file(ctx_t ctx, const char *cert_path,
                                 const char *key_path, const char *password) {
  if (!ctx || !cert_path || !key_path) { return false; }
  auto wctx = static_cast<impl::WolfSSLContext *>(ctx);

  // Load certificate file
  int ret =
      wolfSSL_CTX_use_certificate_file(wctx->ctx, cert_path, SSL_FILETYPE_PEM);
  if (ret != SSL_SUCCESS) {
    impl::wolfssl_last_error() =
        static_cast<uint64_t>(wolfSSL_ERR_peek_last_error());
    return false;
  }

  // Set password callback if password is provided
  if (password) { impl::set_wolfssl_password_cb(wctx->ctx, password); }

  // Load private key file
  ret = wolfSSL_CTX_use_PrivateKey_file(wctx->ctx, key_path, SSL_FILETYPE_PEM);
  if (ret != SSL_SUCCESS) {
    impl::wolfssl_last_error() =
        static_cast<uint64_t>(wolfSSL_ERR_peek_last_error());
    return false;
  }

  // Verify that the certificate and private key match
  return wolfSSL_CTX_check_private_key(wctx->ctx) == SSL_SUCCESS;
}

void set_verify_client(ctx_t ctx, bool require) {
  if (!ctx) { return; }
  auto wctx = static_cast<impl::WolfSSLContext *>(ctx);
  wctx->verify_client = require;
  if (require) {
    wolfSSL_CTX_set_verify(
        wctx->ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
        wctx->has_verify_callback ? impl::wolfssl_verify_callback : nullptr);
  } else {
    if (wctx->has_verify_callback) {
      wolfSSL_CTX_set_verify(wctx->ctx, SSL_VERIFY_PEER,
                             impl::wolfssl_verify_callback);
    } else {
      wolfSSL_CTX_set_verify(wctx->ctx, SSL_VERIFY_NONE, nullptr);
    }
  }
}

session_t create_session(ctx_t ctx, socket_t sock) {
  if (!ctx || sock == INVALID_SOCKET) { return nullptr; }
  auto wctx = static_cast<impl::WolfSSLContext *>(ctx);

  auto session = new (std::nothrow) impl::WolfSSLSession();
  if (!session) { return nullptr; }

  session->sock = sock;
  session->ssl = wolfSSL_new(wctx->ctx);
  if (!session->ssl) {
    impl::wolfssl_last_error() =
        static_cast<uint64_t>(wolfSSL_ERR_peek_last_error());
    delete session;
    return nullptr;
  }

  wolfSSL_set_fd(session->ssl, static_cast<int>(sock));

  return static_cast<session_t>(session);
}

void free_session(session_t session) {
  if (session) { delete static_cast<impl::WolfSSLSession *>(session); }
}

bool set_sni(session_t session, const char *hostname) {
  if (!session || !hostname) { return false; }
  auto wsession = static_cast<impl::WolfSSLSession *>(session);

  int ret = wolfSSL_UseSNI(wsession->ssl, WOLFSSL_SNI_HOST_NAME, hostname,
                           static_cast<word16>(strlen(hostname)));
  if (ret != WOLFSSL_SUCCESS) {
    impl::wolfssl_last_error() =
        static_cast<uint64_t>(wolfSSL_ERR_peek_last_error());
    return false;
  }

  // Also set hostname for verification
  wolfSSL_check_domain_name(wsession->ssl, hostname);

  wsession->hostname = hostname;
  return true;
}

bool set_hostname(session_t session, const char *hostname) {
  // In wolfSSL, set_hostname also sets up hostname verification
  return set_sni(session, hostname);
}

TlsError connect(session_t session) {
  TlsError err;
  if (!session) {
    err.code = ErrorCode::Fatal;
    return err;
  }

  auto wsession = static_cast<impl::WolfSSLSession *>(session);
  int ret = wolfSSL_connect(wsession->ssl);

  if (ret == SSL_SUCCESS) {
    err.code = ErrorCode::Success;
  } else {
    int ssl_error = wolfSSL_get_error(wsession->ssl, ret);
    err.code = impl::map_wolfssl_error(wsession->ssl, ssl_error, err.sys_errno);
    err.backend_code = static_cast<uint64_t>(ssl_error);
    impl::wolfssl_last_error() = err.backend_code;
  }

  return err;
}

TlsError accept(session_t session) {
  TlsError err;
  if (!session) {
    err.code = ErrorCode::Fatal;
    return err;
  }

  auto wsession = static_cast<impl::WolfSSLSession *>(session);
  int ret = wolfSSL_accept(wsession->ssl);

  if (ret == SSL_SUCCESS) {
    err.code = ErrorCode::Success;
    // Capture SNI from thread-local storage after successful handshake
    wsession->sni_hostname = std::move(impl::wolfssl_pending_sni());
    impl::wolfssl_pending_sni().clear();
  } else {
    int ssl_error = wolfSSL_get_error(wsession->ssl, ret);
    err.code = impl::map_wolfssl_error(wsession->ssl, ssl_error, err.sys_errno);
    err.backend_code = static_cast<uint64_t>(ssl_error);
    impl::wolfssl_last_error() = err.backend_code;
  }

  return err;
}

bool connect_nonblocking(session_t session, socket_t sock,
                                time_t timeout_sec, time_t timeout_usec,
                                TlsError *err) {
  if (!session) {
    if (err) { err->code = ErrorCode::Fatal; }
    return false;
  }

  auto wsession = static_cast<impl::WolfSSLSession *>(session);

  // Set socket to non-blocking mode
  detail::set_nonblocking(sock, true);
  auto cleanup =
      detail::scope_exit([&]() { detail::set_nonblocking(sock, false); });

  int ret;
  while ((ret = wolfSSL_connect(wsession->ssl)) != SSL_SUCCESS) {
    int ssl_error = wolfSSL_get_error(wsession->ssl, ret);
    if (ssl_error == SSL_ERROR_WANT_READ) {
      if (detail::select_read(sock, timeout_sec, timeout_usec) > 0) {
        continue;
      }
    } else if (ssl_error == SSL_ERROR_WANT_WRITE) {
      if (detail::select_write(sock, timeout_sec, timeout_usec) > 0) {
        continue;
      }
    }

    // Error or timeout
    if (err) {
      err->code =
          impl::map_wolfssl_error(wsession->ssl, ssl_error, err->sys_errno);
      err->backend_code = static_cast<uint64_t>(ssl_error);
    }
    impl::wolfssl_last_error() = static_cast<uint64_t>(ssl_error);
    return false;
  }

  if (err) { err->code = ErrorCode::Success; }
  return true;
}

bool accept_nonblocking(session_t session, socket_t sock,
                               time_t timeout_sec, time_t timeout_usec,
                               TlsError *err) {
  if (!session) {
    if (err) { err->code = ErrorCode::Fatal; }
    return false;
  }

  auto wsession = static_cast<impl::WolfSSLSession *>(session);

  // Set socket to non-blocking mode
  detail::set_nonblocking(sock, true);
  auto cleanup =
      detail::scope_exit([&]() { detail::set_nonblocking(sock, false); });

  int ret;
  while ((ret = wolfSSL_accept(wsession->ssl)) != SSL_SUCCESS) {
    int ssl_error = wolfSSL_get_error(wsession->ssl, ret);
    if (ssl_error == SSL_ERROR_WANT_READ) {
      if (detail::select_read(sock, timeout_sec, timeout_usec) > 0) {
        continue;
      }
    } else if (ssl_error == SSL_ERROR_WANT_WRITE) {
      if (detail::select_write(sock, timeout_sec, timeout_usec) > 0) {
        continue;
      }
    }

    // Error or timeout
    if (err) {
      err->code =
          impl::map_wolfssl_error(wsession->ssl, ssl_error, err->sys_errno);
      err->backend_code = static_cast<uint64_t>(ssl_error);
    }
    impl::wolfssl_last_error() = static_cast<uint64_t>(ssl_error);
    return false;
  }

  if (err) { err->code = ErrorCode::Success; }

  // Capture SNI from thread-local storage after successful handshake
  wsession->sni_hostname = std::move(impl::wolfssl_pending_sni());
  impl::wolfssl_pending_sni().clear();

  return true;
}

ssize_t read(session_t session, void *buf, size_t len, TlsError &err) {
  if (!session || !buf) {
    err.code = ErrorCode::Fatal;
    return -1;
  }

  auto wsession = static_cast<impl::WolfSSLSession *>(session);
  int ret = wolfSSL_read(wsession->ssl, buf, static_cast<int>(len));

  if (ret > 0) {
    err.code = ErrorCode::Success;
    return static_cast<ssize_t>(ret);
  }

  if (ret == 0) {
    err.code = ErrorCode::PeerClosed;
    return 0;
  }

  int ssl_error = wolfSSL_get_error(wsession->ssl, ret);
  err.code = impl::map_wolfssl_error(wsession->ssl, ssl_error, err.sys_errno);
  err.backend_code = static_cast<uint64_t>(ssl_error);
  impl::wolfssl_last_error() = err.backend_code;
  return -1;
}

ssize_t write(session_t session, const void *buf, size_t len,
                     TlsError &err) {
  if (!session || !buf) {
    err.code = ErrorCode::Fatal;
    return -1;
  }

  auto wsession = static_cast<impl::WolfSSLSession *>(session);
  int ret = wolfSSL_write(wsession->ssl, buf, static_cast<int>(len));

  if (ret > 0) {
    err.code = ErrorCode::Success;
    return static_cast<ssize_t>(ret);
  }

  // wolfSSL_write returns 0 when the peer has sent a close_notify.
  // Treat this as an error (return -1) so callers don't spin in a
  // write loop adding zero to the offset.
  if (ret == 0) {
    err.code = ErrorCode::PeerClosed;
    return -1;
  }

  int ssl_error = wolfSSL_get_error(wsession->ssl, ret);
  err.code = impl::map_wolfssl_error(wsession->ssl, ssl_error, err.sys_errno);
  err.backend_code = static_cast<uint64_t>(ssl_error);
  impl::wolfssl_last_error() = err.backend_code;
  return -1;
}

int pending(const_session_t session) {
  if (!session) { return 0; }
  auto wsession =
      static_cast<impl::WolfSSLSession *>(const_cast<void *>(session));
  return wolfSSL_pending(wsession->ssl);
}

void shutdown(session_t session, bool graceful) {
  if (!session) { return; }
  auto wsession = static_cast<impl::WolfSSLSession *>(session);

  if (graceful) {
    int ret;
    int attempts = 0;
    while ((ret = wolfSSL_shutdown(wsession->ssl)) != SSL_SUCCESS &&
           attempts < 3) {
      int ssl_error = wolfSSL_get_error(wsession->ssl, ret);
      if (ssl_error != SSL_ERROR_WANT_READ &&
          ssl_error != SSL_ERROR_WANT_WRITE) {
        break;
      }
      attempts++;
    }
  } else {
    wolfSSL_shutdown(wsession->ssl);
  }
}

bool is_peer_closed(session_t session, socket_t sock) {
  if (!session || sock == INVALID_SOCKET) { return true; }
  auto wsession = static_cast<impl::WolfSSLSession *>(session);

  // Check if there's already decrypted data available
  if (wolfSSL_pending(wsession->ssl) > 0) { return false; }

  // Set socket to non-blocking to avoid blocking on read
  detail::set_nonblocking(sock, true);
  auto cleanup =
      detail::scope_exit([&]() { detail::set_nonblocking(sock, false); });

  // Peek 1 byte to check connection status without consuming data
  unsigned char buf;
  int ret = wolfSSL_peek(wsession->ssl, &buf, 1);

  // If we got data or WANT_READ (would block), connection is alive
  if (ret > 0) { return false; }

  int ssl_error = wolfSSL_get_error(wsession->ssl, ret);
  if (ssl_error == SSL_ERROR_WANT_READ) { return false; }

  return ssl_error == SSL_ERROR_ZERO_RETURN || ssl_error == SSL_ERROR_SYSCALL ||
         ret == 0;
}

cert_t get_peer_cert(const_session_t session) {
  if (!session) { return nullptr; }
  auto wsession =
      static_cast<impl::WolfSSLSession *>(const_cast<void *>(session));

  WOLFSSL_X509 *cert = wolfSSL_get_peer_certificate(wsession->ssl);
  return static_cast<cert_t>(cert);
}

void free_cert(cert_t cert) {
  if (cert) { wolfSSL_X509_free(static_cast<WOLFSSL_X509 *>(cert)); }
}

bool verify_hostname(cert_t cert, const char *hostname) {
  if (!cert || !hostname) { return false; }
  auto x509 = static_cast<WOLFSSL_X509 *>(cert);
  std::string host_str(hostname);

  // Check if hostname is an IP address
  bool is_ip = impl::is_ipv4_address(host_str);
  unsigned char ip_bytes[4];
  if (is_ip) { impl::parse_ipv4(host_str, ip_bytes); }

  // Check Subject Alternative Names
  auto *san_names = static_cast<WOLF_STACK_OF(WOLFSSL_GENERAL_NAME) *>(
      wolfSSL_X509_get_ext_d2i(x509, NID_subject_alt_name, nullptr, nullptr));

  if (san_names) {
    int san_count = wolfSSL_sk_num(san_names);
    for (int i = 0; i < san_count; i++) {
      auto *names =
          static_cast<WOLFSSL_GENERAL_NAME *>(wolfSSL_sk_value(san_names, i));
      if (!names) continue;

      if (!is_ip && names->type == WOLFSSL_GEN_DNS) {
        // DNS name
        unsigned char *dns_name = nullptr;
        int dns_len = wolfSSL_ASN1_STRING_to_UTF8(&dns_name, names->d.dNSName);
        if (dns_name && dns_len > 0) {
          std::string san_name(reinterpret_cast<char *>(dns_name),
                               static_cast<size_t>(dns_len));
          XFREE(dns_name, nullptr, DYNAMIC_TYPE_OPENSSL);
          if (detail::match_hostname(san_name, host_str)) {
            wolfSSL_sk_free(san_names);
            return true;
          }
        }
      } else if (is_ip && names->type == WOLFSSL_GEN_IPADD) {
        // IP address
        unsigned char *ip_data = wolfSSL_ASN1_STRING_data(names->d.iPAddress);
        int ip_len = wolfSSL_ASN1_STRING_length(names->d.iPAddress);
        if (ip_data && ip_len == 4 && memcmp(ip_data, ip_bytes, 4) == 0) {
          wolfSSL_sk_free(san_names);
          return true;
        }
      }
    }
    wolfSSL_sk_free(san_names);
  }

  // Fallback: Check Common Name (CN) in subject
  WOLFSSL_X509_NAME *subject = wolfSSL_X509_get_subject_name(x509);
  if (subject) {
    char cn[256] = {};
    int cn_len = wolfSSL_X509_NAME_get_text_by_NID(subject, NID_commonName, cn,
                                                   sizeof(cn));
    if (cn_len > 0) {
      std::string cn_str(cn, static_cast<size_t>(cn_len));
      if (detail::match_hostname(cn_str, host_str)) { return true; }
    }
  }

  return false;
}

uint64_t hostname_mismatch_code() {
  return static_cast<uint64_t>(DOMAIN_NAME_MISMATCH);
}

long get_verify_result(const_session_t session) {
  if (!session) { return -1; }
  auto wsession =
      static_cast<impl::WolfSSLSession *>(const_cast<void *>(session));
  long result = wolfSSL_get_verify_result(wsession->ssl);
  return result;
}

std::string get_cert_subject_cn(cert_t cert) {
  if (!cert) return "";
  auto x509 = static_cast<WOLFSSL_X509 *>(cert);

  WOLFSSL_X509_NAME *subject = wolfSSL_X509_get_subject_name(x509);
  if (!subject) return "";

  char cn[256] = {};
  int cn_len = wolfSSL_X509_NAME_get_text_by_NID(subject, NID_commonName, cn,
                                                 sizeof(cn));
  if (cn_len <= 0) return "";
  return std::string(cn, static_cast<size_t>(cn_len));
}

std::string get_cert_issuer_name(cert_t cert) {
  if (!cert) return "";
  auto x509 = static_cast<WOLFSSL_X509 *>(cert);

  WOLFSSL_X509_NAME *issuer = wolfSSL_X509_get_issuer_name(x509);
  if (!issuer) return "";

  char *name_str = wolfSSL_X509_NAME_oneline(issuer, nullptr, 0);
  if (!name_str) return "";

  std::string result(name_str);
  XFREE(name_str, nullptr, DYNAMIC_TYPE_OPENSSL);
  return result;
}

bool get_cert_sans(cert_t cert, std::vector<SanEntry> &sans) {
  sans.clear();
  if (!cert) return false;
  auto x509 = static_cast<WOLFSSL_X509 *>(cert);

  auto *san_names = static_cast<WOLF_STACK_OF(WOLFSSL_GENERAL_NAME) *>(
      wolfSSL_X509_get_ext_d2i(x509, NID_subject_alt_name, nullptr, nullptr));
  if (!san_names) return true; // No SANs is not an error

  int count = wolfSSL_sk_num(san_names);
  for (int i = 0; i < count; i++) {
    auto *name =
        static_cast<WOLFSSL_GENERAL_NAME *>(wolfSSL_sk_value(san_names, i));
    if (!name) continue;

    SanEntry entry;
    switch (name->type) {
    case WOLFSSL_GEN_DNS: {
      entry.type = SanType::DNS;
      unsigned char *dns_name = nullptr;
      int dns_len = wolfSSL_ASN1_STRING_to_UTF8(&dns_name, name->d.dNSName);
      if (dns_name && dns_len > 0) {
        entry.value = std::string(reinterpret_cast<char *>(dns_name),
                                  static_cast<size_t>(dns_len));
        XFREE(dns_name, nullptr, DYNAMIC_TYPE_OPENSSL);
      }
      break;
    }
    case WOLFSSL_GEN_IPADD: {
      entry.type = SanType::IP;
      unsigned char *ip_data = wolfSSL_ASN1_STRING_data(name->d.iPAddress);
      int ip_len = wolfSSL_ASN1_STRING_length(name->d.iPAddress);
      if (ip_data && ip_len == 4) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d.%d.%d.%d", ip_data[0], ip_data[1],
                 ip_data[2], ip_data[3]);
        entry.value = buf;
      } else if (ip_data && ip_len == 16) {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                 "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                 ip_data[0], ip_data[1], ip_data[2], ip_data[3], ip_data[4],
                 ip_data[5], ip_data[6], ip_data[7], ip_data[8], ip_data[9],
                 ip_data[10], ip_data[11], ip_data[12], ip_data[13],
                 ip_data[14], ip_data[15]);
        entry.value = buf;
      }
      break;
    }
    case WOLFSSL_GEN_EMAIL:
      entry.type = SanType::EMAIL;
      {
        unsigned char *email = nullptr;
        int email_len = wolfSSL_ASN1_STRING_to_UTF8(&email, name->d.rfc822Name);
        if (email && email_len > 0) {
          entry.value = std::string(reinterpret_cast<char *>(email),
                                    static_cast<size_t>(email_len));
          XFREE(email, nullptr, DYNAMIC_TYPE_OPENSSL);
        }
      }
      break;
    case WOLFSSL_GEN_URI:
      entry.type = SanType::URI;
      {
        unsigned char *uri = nullptr;
        int uri_len = wolfSSL_ASN1_STRING_to_UTF8(
            &uri, name->d.uniformResourceIdentifier);
        if (uri && uri_len > 0) {
          entry.value = std::string(reinterpret_cast<char *>(uri),
                                    static_cast<size_t>(uri_len));
          XFREE(uri, nullptr, DYNAMIC_TYPE_OPENSSL);
        }
      }
      break;
    default: entry.type = SanType::OTHER; break;
    }

    if (!entry.value.empty()) { sans.push_back(std::move(entry)); }
  }
  wolfSSL_sk_free(san_names);
  return true;
}

bool get_cert_validity(cert_t cert, time_t &not_before,
                              time_t &not_after) {
  if (!cert) return false;
  auto x509 = static_cast<WOLFSSL_X509 *>(cert);

  const WOLFSSL_ASN1_TIME *nb = wolfSSL_X509_get_notBefore(x509);
  const WOLFSSL_ASN1_TIME *na = wolfSSL_X509_get_notAfter(x509);

  if (!nb || !na) return false;

  // wolfSSL_ASN1_TIME_to_tm is available
  struct tm tm_nb = {}, tm_na = {};
  if (wolfSSL_ASN1_TIME_to_tm(nb, &tm_nb) != WOLFSSL_SUCCESS) return false;
  if (wolfSSL_ASN1_TIME_to_tm(na, &tm_na) != WOLFSSL_SUCCESS) return false;

#ifdef _WIN32
  not_before = _mkgmtime(&tm_nb);
  not_after = _mkgmtime(&tm_na);
#else
  not_before = timegm(&tm_nb);
  not_after = timegm(&tm_na);
#endif
  return true;
}

std::string get_cert_serial(cert_t cert) {
  if (!cert) return "";
  auto x509 = static_cast<WOLFSSL_X509 *>(cert);

  WOLFSSL_ASN1_INTEGER *serial_asn1 = wolfSSL_X509_get_serialNumber(x509);
  if (!serial_asn1) return "";

  // Get the serial number data
  int len = serial_asn1->length;
  unsigned char *data = serial_asn1->data;
  if (!data || len <= 0) return "";

  std::string result;
  result.reserve(static_cast<size_t>(len) * 2);
  for (int i = 0; i < len; i++) {
    char hex[3];
    snprintf(hex, sizeof(hex), "%02X", data[i]);
    result += hex;
  }
  return result;
}

bool get_cert_der(cert_t cert, std::vector<unsigned char> &der) {
  if (!cert) return false;
  auto x509 = static_cast<WOLFSSL_X509 *>(cert);

  int der_len = 0;
  const unsigned char *der_data = wolfSSL_X509_get_der(x509, &der_len);
  if (!der_data || der_len <= 0) return false;

  der.assign(der_data, der_data + der_len);
  return true;
}

const char *get_sni(const_session_t session) {
  if (!session) return nullptr;
  auto wsession = static_cast<const impl::WolfSSLSession *>(session);

  // For server: return SNI received from client during handshake
  if (!wsession->sni_hostname.empty()) {
    return wsession->sni_hostname.c_str();
  }

  // For client: return the hostname set via set_sni
  if (!wsession->hostname.empty()) { return wsession->hostname.c_str(); }

  return nullptr;
}

uint64_t peek_error() {
  return static_cast<uint64_t>(wolfSSL_ERR_peek_last_error());
}

uint64_t get_error() {
  uint64_t err = impl::wolfssl_last_error();
  impl::wolfssl_last_error() = 0;
  return err;
}

std::string error_string(uint64_t code) {
  char buf[256];
  wolfSSL_ERR_error_string(static_cast<unsigned long>(code), buf);
  return std::string(buf);
}

ca_store_t create_ca_store(const char *pem, size_t len) {
  if (!pem || len == 0) { return nullptr; }
  // Validate by attempting to load into a temporary ctx
  WOLFSSL_CTX *tmp_ctx = wolfSSL_CTX_new(wolfTLSv1_2_client_method());
  if (!tmp_ctx) { return nullptr; }
  int ret = wolfSSL_CTX_load_verify_buffer(
      tmp_ctx, reinterpret_cast<const unsigned char *>(pem),
      static_cast<long>(len), SSL_FILETYPE_PEM);
  wolfSSL_CTX_free(tmp_ctx);
  if (ret != SSL_SUCCESS) { return nullptr; }
  return static_cast<ca_store_t>(
      new impl::WolfSSLCAStore{std::string(pem, len)});
}

void free_ca_store(ca_store_t store) {
  delete static_cast<impl::WolfSSLCAStore *>(store);
}

bool set_ca_store(ctx_t ctx, ca_store_t store) {
  if (!ctx || !store) { return false; }
  auto *wctx = static_cast<impl::WolfSSLContext *>(ctx);
  auto *ca = static_cast<impl::WolfSSLCAStore *>(store);
  int ret = wolfSSL_CTX_load_verify_buffer(
      wctx->ctx, reinterpret_cast<const unsigned char *>(ca->pem_data.data()),
      static_cast<long>(ca->pem_data.size()), SSL_FILETYPE_PEM);
  if (ret == SSL_SUCCESS) { wctx->ca_pem_data_ += ca->pem_data; }
  return ret == SSL_SUCCESS;
}

size_t get_ca_certs(ctx_t ctx, std::vector<cert_t> &certs) {
  certs.clear();
  if (!ctx) { return 0; }
  auto *wctx = static_cast<impl::WolfSSLContext *>(ctx);
  if (wctx->ca_pem_data_.empty()) { return 0; }

  const std::string &pem = wctx->ca_pem_data_;
  const std::string begin_marker = "-----BEGIN CERTIFICATE-----";
  const std::string end_marker = "-----END CERTIFICATE-----";
  size_t pos = 0;
  while ((pos = pem.find(begin_marker, pos)) != std::string::npos) {
    size_t end_pos = pem.find(end_marker, pos);
    if (end_pos == std::string::npos) { break; }
    end_pos += end_marker.size();
    std::string cert_pem = pem.substr(pos, end_pos - pos);
    WOLFSSL_X509 *x509 = wolfSSL_X509_load_certificate_buffer(
        reinterpret_cast<const unsigned char *>(cert_pem.data()),
        static_cast<int>(cert_pem.size()), WOLFSSL_FILETYPE_PEM);
    if (x509) { certs.push_back(static_cast<cert_t>(x509)); }
    pos = end_pos;
  }
  return certs.size();
}

std::vector<std::string> get_ca_names(ctx_t ctx) {
  std::vector<std::string> names;
  if (!ctx) { return names; }
  auto *wctx = static_cast<impl::WolfSSLContext *>(ctx);
  if (wctx->ca_pem_data_.empty()) { return names; }

  const std::string &pem = wctx->ca_pem_data_;
  const std::string begin_marker = "-----BEGIN CERTIFICATE-----";
  const std::string end_marker = "-----END CERTIFICATE-----";
  size_t pos = 0;
  while ((pos = pem.find(begin_marker, pos)) != std::string::npos) {
    size_t end_pos = pem.find(end_marker, pos);
    if (end_pos == std::string::npos) { break; }
    end_pos += end_marker.size();
    std::string cert_pem = pem.substr(pos, end_pos - pos);
    WOLFSSL_X509 *x509 = wolfSSL_X509_load_certificate_buffer(
        reinterpret_cast<const unsigned char *>(cert_pem.data()),
        static_cast<int>(cert_pem.size()), WOLFSSL_FILETYPE_PEM);
    if (x509) {
      WOLFSSL_X509_NAME *subject = wolfSSL_X509_get_subject_name(x509);
      if (subject) {
        char *name_str = wolfSSL_X509_NAME_oneline(subject, nullptr, 0);
        if (name_str) {
          names.push_back(name_str);
          XFREE(name_str, nullptr, DYNAMIC_TYPE_OPENSSL);
        }
      }
      wolfSSL_X509_free(x509);
    }
    pos = end_pos;
  }
  return names;
}

bool update_server_cert(ctx_t ctx, const char *cert_pem,
                               const char *key_pem, const char *password) {
  if (!ctx || !cert_pem || !key_pem) { return false; }
  auto *wctx = static_cast<impl::WolfSSLContext *>(ctx);

  // Load new certificate
  int ret = wolfSSL_CTX_use_certificate_buffer(
      wctx->ctx, reinterpret_cast<const unsigned char *>(cert_pem),
      static_cast<long>(strlen(cert_pem)), SSL_FILETYPE_PEM);
  if (ret != SSL_SUCCESS) {
    impl::wolfssl_last_error() =
        static_cast<uint64_t>(wolfSSL_ERR_peek_last_error());
    return false;
  }

  // Set password if provided
  if (password) { impl::set_wolfssl_password_cb(wctx->ctx, password); }

  // Load new private key
  ret = wolfSSL_CTX_use_PrivateKey_buffer(
      wctx->ctx, reinterpret_cast<const unsigned char *>(key_pem),
      static_cast<long>(strlen(key_pem)), SSL_FILETYPE_PEM);
  if (ret != SSL_SUCCESS) {
    impl::wolfssl_last_error() =
        static_cast<uint64_t>(wolfSSL_ERR_peek_last_error());
    return false;
  }

  return true;
}

bool update_server_client_ca(ctx_t ctx, const char *ca_pem) {
  if (!ctx || !ca_pem) { return false; }
  auto *wctx = static_cast<impl::WolfSSLContext *>(ctx);

  int ret = wolfSSL_CTX_load_verify_buffer(
      wctx->ctx, reinterpret_cast<const unsigned char *>(ca_pem),
      static_cast<long>(strlen(ca_pem)), SSL_FILETYPE_PEM);
  if (ret != SSL_SUCCESS) {
    impl::wolfssl_last_error() =
        static_cast<uint64_t>(wolfSSL_ERR_peek_last_error());
    return false;
  }
  return true;
}

bool set_verify_callback(ctx_t ctx, VerifyCallback callback) {
  if (!ctx) { return false; }
  auto *wctx = static_cast<impl::WolfSSLContext *>(ctx);

  impl::get_verify_callback() = std::move(callback);
  wctx->has_verify_callback = static_cast<bool>(impl::get_verify_callback());

  if (wctx->has_verify_callback) {
    wolfSSL_CTX_set_verify(wctx->ctx, SSL_VERIFY_PEER,
                           impl::wolfssl_verify_callback);
  } else {
    wolfSSL_CTX_set_verify(
        wctx->ctx,
        wctx->verify_client
            ? (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT)
            : SSL_VERIFY_NONE,
        nullptr);
  }
  return true;
}

long get_verify_error(const_session_t session) {
  if (!session) { return -1; }
  auto *wsession =
      static_cast<impl::WolfSSLSession *>(const_cast<void *>(session));
  return wolfSSL_get_verify_result(wsession->ssl);
}

std::string verify_error_string(long error_code) {
  if (error_code == 0) { return ""; }
  const char *str =
      wolfSSL_X509_verify_cert_error_string(static_cast<int>(error_code));
  return str ? std::string(str) : std::string();
}

} // namespace tls

#endif // CPPHTTPLIB_WOLFSSL_SUPPORT


} // namespace httplib
