#ifndef PP_LEDGER_FILE_SYNC_H
#define PP_LEDGER_FILE_SYNC_H

#include "common/Error.h"

#include <string>

namespace pp::utl {

/**
 * Durable file writes: what survives a power loss, not only a process crash. Writing and flushing a
 * stream hands the bytes to the OS; only a sync puts them on the device.
 */

/** Put `path`'s written bytes on the device (fsync; F_FULLFSYNC on macOS; FlushFileBuffers on Windows). */
pp::Roe<void> syncFile(const std::string &path);

/**
 * Put directory `path`'s entries (files created, renamed or removed in it) on the device. POSIX
 * only; on Windows a no-op — replaceFile renames with write-through there.
 */
pp::Roe<void> syncDirectory(const std::string &path);

/**
 * Replace `path` with `bytes` durably: write `path`.tmp, sync it, rename it over `path`, sync the
 * directory. After a crash or a power loss the file holds the old bytes or the new, never part of
 * either.
 */
pp::Roe<void> replaceFile(const std::string &path, const std::string &bytes);

} // namespace pp::utl

#endif // PP_LEDGER_FILE_SYNC_H
