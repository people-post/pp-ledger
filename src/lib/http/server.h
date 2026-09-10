//
//  server.h
//

#ifndef CPPHTTPLIB_SERVER_H
#define CPPHTTPLIB_SERVER_H

#include "types.h"

namespace httplib {

class Server {
public:
  using Handler = std::function<void(const Request &, Response &)>;

  using ExceptionHandler =
      std::function<void(const Request &, Response &, std::exception_ptr ep)>;

  enum class HandlerResponse {
    Handled,
    Unhandled,
  };
  using HandlerWithResponse =
      std::function<HandlerResponse(const Request &, Response &)>;

  using HandlerWithContentReader = std::function<void(
      const Request &, Response &, const ContentReader &content_reader)>;

  using Expect100ContinueHandler =
      std::function<int(const Request &, Response &)>;

  using WebSocketHandler =
      std::function<void(const Request &, ws::WebSocket &)>;
  using SubProtocolSelector =
      std::function<std::string(const std::vector<std::string> &protocols)>;

  explicit Server(const ServerConfig &config = {});

  virtual ~Server();

  virtual bool is_valid() const;

  Server &Get(const std::string &pattern, Handler handler);
  Server &Post(const std::string &pattern, Handler handler);
  Server &Post(const std::string &pattern, HandlerWithContentReader handler);
  Server &Put(const std::string &pattern, Handler handler);
  Server &Put(const std::string &pattern, HandlerWithContentReader handler);
  Server &Patch(const std::string &pattern, Handler handler);
  Server &Patch(const std::string &pattern, HandlerWithContentReader handler);
  Server &Delete(const std::string &pattern, Handler handler);
  Server &Delete(const std::string &pattern, HandlerWithContentReader handler);
  Server &Options(const std::string &pattern, Handler handler);

  Server &WebSocket(const std::string &pattern, WebSocketHandler handler);
  Server &WebSocket(const std::string &pattern, WebSocketHandler handler,
                    SubProtocolSelector sub_protocol_selector);

  bool set_base_dir(const std::string &dir,
                    const std::string &mount_point = std::string());
  bool set_mount_point(const std::string &mount_point, const std::string &dir,
                       Headers headers = Headers());
  bool remove_mount_point(const std::string &mount_point);
  Server &set_file_extension_and_mimetype_mapping(const std::string &ext,
                                                  const std::string &mime);
  Server &set_default_file_mimetype(const std::string &mime);
  Server &set_file_request_handler(Handler handler);

  template <class ErrorHandlerFunc>
  Server &set_error_handler(ErrorHandlerFunc &&handler) {
    return set_error_handler_core(
        std::forward<ErrorHandlerFunc>(handler),
        std::is_convertible<ErrorHandlerFunc, HandlerWithResponse>{});
  }

  Server &set_exception_handler(ExceptionHandler handler);

  Server &set_pre_routing_handler(HandlerWithResponse handler);
  Server &set_post_routing_handler(Handler handler);

  Server &set_pre_request_handler(HandlerWithResponse handler);

  Server &set_expect_100_continue_handler(Expect100ContinueHandler handler);
  Server &set_logger(Logger logger);
  Server &set_pre_compression_logger(Logger logger);
  Server &set_error_logger(ErrorLogger error_logger);

  Server &set_address_family(int family);
  Server &set_tcp_nodelay(bool on);
  Server &set_ipv6_v6only(bool on);
  Server &set_socket_options(SocketOptions socket_options);

  Server &set_default_headers(Headers headers);
  Server &
  set_header_writer(std::function<ssize_t(Stream &, Headers &)> const &writer);

  Server &set_trusted_proxies(const std::vector<std::string> &proxies);

  Server &set_keep_alive_max_count(size_t count);
  Server &set_keep_alive_timeout(time_t sec);

  Server &set_read_timeout(time_t sec, time_t usec = 0);
  template <class Rep, class Period>
  Server &set_read_timeout(const std::chrono::duration<Rep, Period> &duration);

  Server &set_write_timeout(time_t sec, time_t usec = 0);
  template <class Rep, class Period>
  Server &set_write_timeout(const std::chrono::duration<Rep, Period> &duration);

  Server &set_idle_interval(time_t sec, time_t usec = 0);
  template <class Rep, class Period>
  Server &set_idle_interval(const std::chrono::duration<Rep, Period> &duration);

  Server &set_payload_max_length(size_t length);

  bool bind_to_port(const std::string &host, int port, int socket_flags = 0);
  int bind_to_any_port(const std::string &host, int socket_flags = 0);
  bool listen_after_bind();

  bool listen(const std::string &host, int port, int socket_flags = 0);

  bool is_running() const;
  void wait_until_ready() const;
  void stop();
  void decommission();

  std::function<TaskQueue *(void)> new_task_queue;

protected:
  bool process_request(Stream &strm, const std::string &remote_addr,
                       int remote_port, const std::string &local_addr,
                       int local_port, bool close_connection,
                       bool &connection_closed,
                       const std::function<void(Request &)> &setup_request,
                       bool *websocket_upgraded = nullptr);

  std::atomic<socket_t> svr_sock_{INVALID_SOCKET};

  std::vector<std::string> trusted_proxies_;

  size_t keep_alive_max_count_;
  time_t keep_alive_timeout_sec_;
  time_t read_timeout_sec_;
  time_t read_timeout_usec_;
  time_t write_timeout_sec_;
  time_t write_timeout_usec_;
  time_t idle_interval_sec_;
  time_t idle_interval_usec_;
  size_t payload_max_length_;

private:
  using Handlers =
      std::vector<std::pair<std::unique_ptr<detail::MatcherBase>, Handler>>;
  using HandlersForContentReader =
      std::vector<std::pair<std::unique_ptr<detail::MatcherBase>,
                            HandlerWithContentReader>>;

  static std::unique_ptr<detail::MatcherBase>
  make_matcher(const std::string &pattern);

  Server &set_error_handler_core(HandlerWithResponse handler, std::true_type);
  Server &set_error_handler_core(Handler handler, std::false_type);

  socket_t create_server_socket(const std::string &host, int port,
                                int socket_flags,
                                SocketOptions socket_options) const;
  int bind_internal(const std::string &host, int port, int socket_flags);
  bool listen_internal();

  bool routing(Request &req, Response &res, Stream &strm);
  bool handle_file_request(Request &req, Response &res);
  bool check_if_not_modified(const Request &req, Response &res,
                             const std::string &etag, time_t mtime) const;
  bool check_if_range(Request &req, const std::string &etag,
                      time_t mtime) const;
  bool dispatch_request(Request &req, Response &res,
                        const Handlers &handlers) const;
  bool dispatch_request_for_content_reader(
      Request &req, Response &res, ContentReader content_reader,
      const HandlersForContentReader &handlers) const;

  bool parse_request_line(const char *s, Request &req) const;
  void apply_ranges(const Request &req, Response &res,
                    std::string &content_type, std::string &boundary) const;
  bool write_response(Stream &strm, bool close_connection, Request &req,
                      Response &res);
  bool write_response_with_content(Stream &strm, bool close_connection,
                                   const Request &req, Response &res);
  bool write_response_core(Stream &strm, bool close_connection,
                           const Request &req, Response &res,
                           bool need_apply_ranges);
  bool write_content_with_provider(Stream &strm, const Request &req,
                                   Response &res, const std::string &boundary,
                                   const std::string &content_type);
  bool read_content(Stream &strm, Request &req, Response &res);
  bool read_content_with_content_receiver(Stream &strm, Request &req,
                                          Response &res,
                                          ContentReceiver receiver,
                                          FormDataHeader multipart_header,
                                          ContentReceiver multipart_receiver);
  bool read_content_core(Stream &strm, Request &req, Response &res,
                         ContentReceiver receiver,
                         FormDataHeader multipart_header,
                         ContentReceiver multipart_receiver) const;

  virtual bool process_and_close_socket(socket_t sock);

  void output_log(const Request &req, const Response &res) const;
  void output_pre_compression_log(const Request &req,
                                  const Response &res) const;
  void output_error_log(const Error &err, const Request *req) const;

  std::atomic<bool> is_running_{false};
  std::atomic<bool> is_decommissioned{false};

  struct MountPointEntry {
    std::string mount_point;
    std::string base_dir;
    Headers headers;
  };
  std::vector<MountPointEntry> base_dirs_;
  std::map<std::string, std::string> file_extension_and_mimetype_map_;
  std::string default_file_mimetype_ = "application/octet-stream";
  Handler file_request_handler_;

  Handlers get_handlers_;
  Handlers post_handlers_;
  HandlersForContentReader post_handlers_for_content_reader_;
  Handlers put_handlers_;
  HandlersForContentReader put_handlers_for_content_reader_;
  Handlers patch_handlers_;
  HandlersForContentReader patch_handlers_for_content_reader_;
  Handlers delete_handlers_;
  HandlersForContentReader delete_handlers_for_content_reader_;
  Handlers options_handlers_;

  struct WebSocketHandlerEntry {
    std::unique_ptr<detail::MatcherBase> matcher;
    WebSocketHandler handler;
    SubProtocolSelector sub_protocol_selector;
  };
  using WebSocketHandlers = std::vector<WebSocketHandlerEntry>;
  WebSocketHandlers websocket_handlers_;

  HandlerWithResponse error_handler_;
  ExceptionHandler exception_handler_;
  HandlerWithResponse pre_routing_handler_;
  Handler post_routing_handler_;
  HandlerWithResponse pre_request_handler_;
  Expect100ContinueHandler expect_100_continue_handler_;

  mutable std::mutex logger_mutex_;
  Logger logger_;
  Logger pre_compression_logger_;
  ErrorLogger error_logger_;

  int address_family_ = AF_UNSPEC;
  bool tcp_nodelay_;
  bool ipv6_v6only_;
  int listen_backlog_;
  SocketOptions socket_options_ = default_socket_options;

  Headers default_headers_;
  std::function<ssize_t(Stream &, Headers &)> header_writer_ =
      detail::write_headers;
};


} // namespace httplib

#endif // CPPHTTPLIB_SERVER_H
