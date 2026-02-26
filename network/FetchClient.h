#pragma once

#include "Module.h"
#include "ResultOrError.hpp"
#include "TcpClient.h"
#include "Types.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace pp {
namespace network {

/**
 * FetchClient - Simple client for sending data and receiving responses
 *
 * Uses TCP sockets for peer-to-peer communication.
 * Simple pattern: connect, send, receive, close.
 */
class FetchClient : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  using ResponseCallback = std::function<void(const Roe<std::string> &)>;

  /** Default timeout for synchronous fetch operations. */
  static constexpr std::chrono::milliseconds DEFAULT_TIMEOUT{30000};

  /**
   * Constructor
   */
  FetchClient();

  ~FetchClient() override = default;

  /**
   * Fetch data from a remote peer (async)
   * @param endpoint Endpoint to connect to
   * @param data Data to send to the peer
   * @param callback Callback function to receive the response
   * @param timeout Maximum time to wait for a response (0 = no timeout)
   */
  void fetch(const IpEndpoint &endpoint, const std::string &data,
             ResponseCallback callback,
             std::chrono::milliseconds timeout = DEFAULT_TIMEOUT);

  /**
   * Synchronous fetch - blocks until response is received or timeout expires
   * @param endpoint Endpoint to connect to
   * @param data Data to send to the peer
   * @param timeout Maximum time to wait for a response (0 = no timeout)
   * @return Response data or error
   */
  Roe<std::string> fetchSync(const IpEndpoint &endpoint,
                             const std::string &data,
                             std::chrono::milliseconds timeout = DEFAULT_TIMEOUT);
};

} // namespace network
} // namespace pp
