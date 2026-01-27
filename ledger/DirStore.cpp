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

DirStore::Roe<void> DirStore::ensureDirectory(const std::string &dirPath) const {
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

DirStore::Roe<void> DirStore::validateMinFileSize(size_t maxFileSize) const {
    if (maxFileSize < 1024 * 1024) {
        return Error("Max file size shall be at least 1MB");
    }
    return {};
}

DirStore::Roe<std::string> DirStore::performDirectoryRelocation(
    const std::string &originalPath, const std::string &subdirName,
    const std::vector<std::string> &excludeFiles) {
    
    std::filesystem::path originalDir(originalPath);
    std::filesystem::path parentDir = originalDir.parent_path();
    std::string dirName = originalDir.filename().string();
    std::string tempPath = (parentDir / (dirName + "_tmp_relocate")).string();
    std::string targetSubdir = originalPath + "/" + subdirName;

    std::error_code ec;

    // Step 1: If we have files to exclude, move them to temp location first
    std::vector<std::string> movedExcludedFiles;
    if (!excludeFiles.empty()) {
        std::string excludeTempDir = (parentDir / (dirName + "_exclude_tmp")).string();
        if (!std::filesystem::create_directories(excludeTempDir, ec)) {
            return Error("Failed to create temp directory for excluded files: " + ec.message());
        }

        for (const auto &fileName : excludeFiles) {
            std::string srcFile = originalPath + "/" + fileName;
            if (std::filesystem::exists(srcFile)) {
                std::string destFile = excludeTempDir + "/" + fileName;
                std::filesystem::rename(srcFile, destFile, ec);
                if (ec) {
                    // Clean up and return error
                    std::filesystem::remove_all(excludeTempDir, ec);
                    return Error("Failed to move excluded file " + fileName + ": " + ec.message());
                }
                movedExcludedFiles.push_back(fileName);
            }
        }
    }

    // Step 2: Rename current dir to temp name (dir -> dir_tmp_relocate)
    std::filesystem::rename(originalPath, tempPath, ec);
    if (ec) {
        // Restore excluded files if any
        if (!movedExcludedFiles.empty()) {
            std::string excludeTempDir = (parentDir / (dirName + "_exclude_tmp")).string();
            for (const auto &fileName : movedExcludedFiles) {
                std::string srcFile = excludeTempDir + "/" + fileName;
                std::string destFile = originalPath + "/" + fileName;
                std::filesystem::rename(srcFile, destFile, ec);
            }
            std::filesystem::remove_all(excludeTempDir, ec);
        }
        return Error("Failed to rename directory to temp: " + ec.message());
    }

    // Step 3: Create the original directory again
    if (!std::filesystem::create_directories(originalPath, ec)) {
        // Try to restore
        std::filesystem::rename(tempPath, originalPath, ec);
        if (!movedExcludedFiles.empty()) {
            std::string excludeTempDir = (parentDir / (dirName + "_exclude_tmp")).string();
            std::filesystem::remove_all(excludeTempDir, ec);
        }
        return Error("Failed to recreate original directory: " + ec.message());
    }

    // Step 4: Restore excluded files to original directory
    if (!movedExcludedFiles.empty()) {
        std::string excludeTempDir = (parentDir / (dirName + "_exclude_tmp")).string();
        for (const auto &fileName : movedExcludedFiles) {
            std::string srcFile = excludeTempDir + "/" + fileName;
            std::string destFile = originalPath + "/" + fileName;
            std::filesystem::rename(srcFile, destFile, ec);
            if (ec) {
                // This is bad - try to restore everything
                std::filesystem::remove_all(originalPath, ec);
                std::filesystem::rename(tempPath, originalPath, ec);
                std::filesystem::remove_all(excludeTempDir, ec);
                return Error("Failed to restore excluded file " + fileName + ": " + ec.message());
            }
        }
        std::filesystem::remove_all(excludeTempDir, ec);
    }

    // Step 5: Rename temp to be a subdirectory of original (dir_tmp -> dir/subdirName)
    std::filesystem::rename(tempPath, targetSubdir, ec);
    if (ec) {
        // Try to restore
        std::filesystem::remove_all(originalPath, ec);
        std::filesystem::rename(tempPath, originalPath, ec);
        return Error("Failed to rename temp to subdirectory: " + ec.message());
    }

    return targetSubdir;
}

} // namespace pp
