//
//  ssl.h
//

#ifndef CPPHTTPLIB_SSL_H
#define CPPHTTPLIB_SSL_H

#include "client.h"

namespace httplib {

#ifdef CPPHTTPLIB_SSL_ENABLED
class SSLServer : public Server {
public:
  SSLServer(const char *cert_path, const char *private_key_path,
            const char *client_ca_cert_file_path = nullptr,
            const char *client_ca_cert_dir_path = nullptr,
            const char *private_key_password = nullptr);

  struct PemMemory {
    const char *cert_pem;
    size_t cert_pem_len;
    const char *key_pem;
    size_t key_pem_len;
    const char *client_ca_pem;
    size_t client_ca_pem_len;
    const char *private_key_password;
  };
  explicit SSLServer(const PemMemory &pem);

  // The callback receives the ctx_t handle which can be cast to the
  // appropriate backend type (SSL_CTX* for OpenSSL,
  // tls::impl::MbedTlsContext* for Mbed TLS)
  explicit SSLServer(const tls::ContextSetupCallback &setup_callback);

  ~SSLServer() override;

  bool is_valid() const override;

  bool update_certs_pem(const char *cert_pem, const char *key_pem,
                        const char *client_ca_pem = nullptr,
                        const char *password = nullptr);

  tls::ctx_t tls_context() const { return ctx_; }

  int ssl_last_error() const { return last_ssl_error_; }

private:
  bool process_and_close_socket(socket_t sock) override;

  tls::ctx_t ctx_ = nullptr;
  std::mutex ctx_mutex_;

  int last_ssl_error_ = 0;

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
public:
  [[deprecated("Use SSLServer(PemMemory) or "
               "SSLServer(ContextSetupCallback) instead")]]
  SSLServer(X509 *cert, EVP_PKEY *private_key,
            X509_STORE *client_ca_cert_store = nullptr);

  [[deprecated("Use SSLServer(ContextSetupCallback) instead")]]
  SSLServer(
      const std::function<bool(SSL_CTX &ssl_ctx)> &setup_ssl_ctx_callback);

  [[deprecated("Use tls_context() instead")]]
  SSL_CTX *ssl_context() const;

  [[deprecated("Use update_certs_pem() instead")]]
  void update_certs(X509 *cert, EVP_PKEY *private_key,
                    X509_STORE *client_ca_cert_store = nullptr);
#endif
};

class SSLClient final : public ClientImpl {
public:
  explicit SSLClient(const std::string &host);

  explicit SSLClient(const std::string &host, int port);

  explicit SSLClient(const std::string &host, int port,
                     const std::string &client_cert_path,
                     const std::string &client_key_path,
                     const std::string &private_key_password = std::string());

  struct PemMemory {
    const char *cert_pem;
    size_t cert_pem_len;
    const char *key_pem;
    size_t key_pem_len;
    const char *private_key_password;
  };
  explicit SSLClient(const std::string &host, int port, const PemMemory &pem);

  ~SSLClient() override;

  bool is_valid() const override;

  void set_ca_cert_store(tls::ca_store_t ca_cert_store);
  void load_ca_cert_store(const char *ca_cert, std::size_t size);

  void set_server_certificate_verifier(tls::VerifyCallback verifier);

  // Post-handshake session verifier (backend-independent)
  void set_session_verifier(
      std::function<SSLVerifierResponse(tls::session_t)> verifier);

  tls::ctx_t tls_context() const { return ctx_; }

#if defined(_WIN32) &&                                                         \
    !defined(CPPHTTPLIB_DISABLE_WINDOWS_AUTOMATIC_ROOT_CERTIFICATES_UPDATE)
  void enable_windows_certificate_verification(bool enabled);
#endif

private:
  bool create_and_connect_socket(Socket &socket, Error &error) override;
  bool ensure_socket_connection(Socket &socket, Error &error) override;
  void shutdown_ssl(Socket &socket, bool shutdown_gracefully) override;
  void shutdown_ssl_impl(Socket &socket, bool shutdown_gracefully);

  bool
  process_socket(const Socket &socket,
                 std::chrono::time_point<std::chrono::steady_clock> start_time,
                 std::function<bool(Stream &strm)> callback) override;
  bool is_ssl() const override;

  bool connect_with_proxy(
      Socket &sock,
      std::chrono::time_point<std::chrono::steady_clock> start_time,
      Response &res, bool &success, Error &error);
  bool initialize_ssl(Socket &socket, Error &error);

  bool load_certs();

  tls::ctx_t ctx_ = nullptr;
  std::mutex ctx_mutex_;
  std::once_flag initialize_cert_;

  long verify_result_ = 0;

  std::function<SSLVerifierResponse(tls::session_t)> session_verifier_;

#if defined(_WIN32) &&                                                         \
    !defined(CPPHTTPLIB_DISABLE_WINDOWS_AUTOMATIC_ROOT_CERTIFICATES_UPDATE)
  bool enable_windows_cert_verification_ = true;
#endif

  friend class ClientImpl;

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
public:
  [[deprecated("Use SSLClient(host, port, PemMemory) instead")]]
  explicit SSLClient(const std::string &host, int port, X509 *client_cert,
                     EVP_PKEY *client_key,
                     const std::string &private_key_password = std::string());

  [[deprecated("Use Result::ssl_backend_error() instead")]]
  long get_verify_result() const;

  [[deprecated("Use tls_context() instead")]]
  SSL_CTX *ssl_context() const;

  [[deprecated("Use set_session_verifier(session_t) instead")]]
  void set_server_certificate_verifier(
      std::function<SSLVerifierResponse(SSL *ssl)> verifier) override;

private:
  bool verify_host(X509 *server_cert) const;
  bool verify_host_with_subject_alt_name(X509 *server_cert) const;
  bool verify_host_with_common_name(X509 *server_cert) const;
#endif
};
#endif // CPPHTTPLIB_SSL_ENABLED


} // namespace httplib

#endif // CPPHTTPLIB_SSL_H
