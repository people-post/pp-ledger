#include "DirStore.h"
#include "Logger.h"
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace pp {

std::string DirStore::formatId(uint32_t id) {
    std::ostringstream oss;
    oss << std::setw(6) << std::setfill('0') << id;
    return oss.str();
}

std::string DirStore::getIndexFilePath(const std::string &dirPath) {
    return dirPath + "/idx.dat";
}

DirStore::Roe<void> DirStore::ensureDirectory(const std::string &dirPath) {
    std::error_code ec;
    if (!std::filesystem::exists(dirPath, ec)) {
        if (ec) {
            log().error << "Failed to check directory existence " << dirPath
                        << ": " << ec.message();
            return Error("Failed to check directory: " + ec.message());
        }
        if (!std::filesystem::create_directories(dirPath, ec)) {
            log().error << "Failed to create directory " << dirPath << ": "
                        << ec.message();
            return Error("Failed to create directory: " + ec.message());
        }
        log().info << "Created directory: " << dirPath;
    } else if (ec) {
        log().error << "Failed to check directory existence " << dirPath
                    << ": " << ec.message();
        return Error("Failed to check directory: " + ec.message());
    }
    return {};
}

} // namespace pp
