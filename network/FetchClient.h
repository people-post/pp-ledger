#pragma once

#include "Module.h"
#include "ResultOrError.hpp"
#include "TcpClient.h"
#include "Types.hpp"
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
   */
  void fetch(const TcpEndpoint &endpoint, const std::string &data,
             ResponseCallback callback);

  /**
   * Synchronous fetch - blocks until response is received
   * @param endpoint Endpoint to connect to
   * @param data Data to send to the peer
   * @return Response data or error
   */
  Roe<std::string> fetchSync(const TcpEndpoint &endpoint,
                             const std::string &data);
};

} // namespace network
} // namespace pp
