#ifndef PP_LEDGER_HTTP_GATEWAY_H
#define PP_LEDGER_HTTP_GATEWAY_H

// pp-http's /v1/: the verifying gateway's answers (projects/finality FN004), as JSON.

#include "gateway/Gateway.h"

#include <httplib.h>

#include <string>

/** Register /v1/ on `svr`. With a `token`, every /v1/ request must carry `Authorization: Bearer <token>`. */
void registerGatewayRoutes(httplib::Server &svr, pp::gateway::Gateway &gateway, const std::string &token);

#endif // PP_LEDGER_HTTP_GATEWAY_H
