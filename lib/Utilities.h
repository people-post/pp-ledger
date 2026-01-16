#ifndef PP_LEDGER_UTILITIES_H
#define PP_LEDGER_UTILITIES_H

#include <string>
#include <cstdint>

namespace pp {
namespace utl {

/**
 * Parse an integer from a string
 * @param str String to parse
 * @param value Output parameter for the parsed value
 * @return true if parsing succeeded, false otherwise
 */
bool parseInt(const std::string &str, int &value);

/**
 * Parse a 64-bit signed integer from a string
 * @param str String to parse
 * @param value Output parameter for the parsed value
 * @return true if parsing succeeded, false otherwise
 */
bool parseInt64(const std::string &str, int64_t &value);

/**
 * Parse a 64-bit unsigned integer from a string
 * @param str String to parse
 * @param value Output parameter for the parsed value
 * @return true if parsing succeeded, false otherwise
 */
bool parseUInt64(const std::string &str, uint64_t &value);

/**
 * Parse a port number from a string (validates range 0-65535)
 * @param str String to parse
 * @param port Output parameter for the parsed port
 * @return true if parsing succeeded and port is in valid range, false otherwise
 */
bool parsePort(const std::string &str, uint16_t &port);

/**
 * Parse a host:port string into separate host and port components
 * @param hostPort String in format "host:port"
 * @param host Output parameter for the host part
 * @param port Output parameter for the port part
 * @return true if parsing succeeded, false otherwise
 */
bool parseHostPort(const std::string &hostPort, std::string &host, uint16_t &port);

} // namespace utl
} // namespace pp

#endif // PP_LEDGER_UTILITIES_H