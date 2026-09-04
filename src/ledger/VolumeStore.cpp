#include "VolumeStore.h"
#include "common/Logger.h"
#include "common/Serialize.hpp"
#include "lib/common/BinaryPack.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pp {

VolumeStore::VolumeStore() {}

VolumeStore::~VolumeStore() { close(); }

void VolumeStore::close() {
  if (!config_.volumesDir.empty() &&
      std::filesystem::exists(config_.volumesDir)) {
    flush();
  }
  volumeInfoMap_.clear();
  volumeIdOrder_.clear();
  totalBlockCount_ = 0;
  currentVolumeId_ = 0;
  indexFilePath_.clear();
  config_ = {};
}

std::string
VolumeStore::getVolumesIndexFilePath(const std::string &volumesDir) {
  return volumesDir + "/" + VOLUMES_INDEX_FILENAME;
}

std::string VolumeStore::getVolumePath(uint32_t volumeId) const {
  return config_.volumesDir + "/v" + formatId(volumeId);
}

VolumeStore::Roe<void> VolumeStore::init(const InitConfig &config) {
  auto sizeResult = validateMinFileSize(config.maxFileSize);
  if (!sizeResult.isOk()) {
    return sizeResult;
  }
  if (config.maxFileCount == 0) {
    return Error("Max file count must be greater than 0");
  }
  if (config.maxVolumes == 0) {
    return Error("Max volumes must be greater than 0");
  }

  config_.volumesDir = config.volumesDir;
  config_.maxFileCount = config.maxFileCount;
  config_.maxFileSize = config.maxFileSize;
  config_.maxVolumes = config.maxVolumes;
  currentVolumeId_ = 0;
  indexFilePath_ = getVolumesIndexFilePath(config.volumesDir);
  volumeInfoMap_.clear();
  volumeIdOrder_.clear();
  totalBlockCount_ = 0;

  if (std::filesystem::exists(indexFilePath_)) {
    return Error("Volumes index already exists: " + indexFilePath_ +
                 ". Use mount() to load existing volumes.");
  }

  auto dirResult = ensureDirectory(config.volumesDir);
  if (!dirResult.isOk()) {
    return dirResult;
  }

  if (!saveIndex()) {
    return Error("Failed to create volumes index file");
  }

  // Always start with v000001 — no root special case / relocate.
  if (!createVolume(1, 0)) {
    return Error("Failed to create initial volume v000001");
  }
  currentVolumeId_ = 1;
  if (!saveIndex()) {
    return Error("Failed to save volumes index after creating v000001");
  }

  log().info << "VolumeStore initialized at " << config_.volumesDir
             << " (maxFileCount: " << config_.maxFileCount
             << ", maxFileSize: " << config_.maxFileSize
             << ", maxVolumes: " << config_.maxVolumes << ")";
  return {};
}

VolumeStore::Roe<void> VolumeStore::mount(const MountConfig &config) {
  if (!std::filesystem::exists(config.volumesDir)) {
    return Error("Volumes directory does not exist: " + config.volumesDir +
                 ". Use init() to create a new store.");
  }

  config_.volumesDir = config.volumesDir;
  currentVolumeId_ = 0;
  indexFilePath_ = getVolumesIndexFilePath(config.volumesDir);
  volumeInfoMap_.clear();
  volumeIdOrder_.clear();
  totalBlockCount_ = 0;

  if (!std::filesystem::exists(indexFilePath_)) {
    return Error("Volumes index does not exist: " + indexFilePath_);
  }

  if (!loadIndex()) {
    return Error("Failed to load volumes index");
  }

  auto sizeResult = validateMinFileSize(config_.maxFileSize);
  if (!sizeResult.isOk()) {
    return sizeResult;
  }
  if (config_.maxFileCount == 0) {
    return Error("Invalid max file count in volumes index");
  }
  if (config_.maxVolumes == 0) {
    return Error("Invalid max volumes in volumes index");
  }

  updateCurrentVolumeId();

  auto openResult = openExistingVolumes();
  if (!openResult.isOk()) {
    return openResult;
  }

  recalculateTotalBlockCount();

  log().info << "VolumeStore mounted from " << config_.volumesDir << " with "
             << volumeInfoMap_.size() << " volumes and " << totalBlockCount_
             << " blocks";
  return {};
}

bool VolumeStore::canFit(uint64_t size) const {
  if (size > config_.maxFileSize) {
    return false;
  }
  if (volumeInfoMap_.empty()) {
    return config_.maxVolumes > 0;
  }
  auto it = volumeInfoMap_.find(currentVolumeId_);
  if (it != volumeInfoMap_.end() && it->second.store &&
      it->second.store->canFit(size)) {
    return true;
  }
  return volumeInfoMap_.size() < config_.maxVolumes;
}

uint64_t VolumeStore::getBlockCount() const { return totalBlockCount_; }

uint64_t VolumeStore::countSizeFromBlockId(uint64_t blockId) const {
  if (blockId >= totalBlockCount_) {
    return 0;
  }

  auto [volumeId, indexWithin] = findBlockVolume(blockId);
  if (volumeId == 0 && indexWithin == 0 && blockId != 0) {
    return 0;
  }

  auto it = volumeInfoMap_.find(volumeId);
  if (it == volumeInfoMap_.end() || !it->second.store) {
    return 0;
  }

  uint64_t total = it->second.store->countSizeFromBlockId(indexWithin);

  auto orderIt =
      std::find(volumeIdOrder_.begin(), volumeIdOrder_.end(), volumeId);
  if (orderIt != volumeIdOrder_.end()) {
    ++orderIt;
    for (; orderIt != volumeIdOrder_.end(); ++orderIt) {
      auto vit = volumeInfoMap_.find(*orderIt);
      if (vit != volumeInfoMap_.end() && vit->second.store) {
        total += vit->second.store->countSizeFromBlockId(0);
      }
    }
  }
  return total;
}

VolumeStore::Roe<std::string> VolumeStore::readBlock(uint64_t index) const {
  auto [volumeId, indexWithin] = findBlockVolume(index);
  if (volumeId == 0 && indexWithin == 0 && index != 0) {
    return Error("Block " + std::to_string(index) + " not found");
  }

  auto it = volumeInfoMap_.find(volumeId);
  if (it == volumeInfoMap_.end() || !it->second.store) {
    return Error("Volume " + std::to_string(volumeId) + " not found");
  }

  return it->second.store->readBlock(indexWithin);
}

VolumeStore::Roe<uint64_t> VolumeStore::appendBlock(const std::string &block) {
  FileDirStore *active = getActiveVolume(block.size());
  if (!active) {
    return Error("Failed to get active volume (store full?)");
  }

  auto result = active->appendBlock(block);
  if (!result.isOk()) {
    return Error("Failed to write block to volume: " + result.error().message);
  }

  totalBlockCount_++;
  saveIndex();
  return totalBlockCount_ - 1;
}

VolumeStore::Roe<void> VolumeStore::rewindTo(uint64_t index) {
  if (index > totalBlockCount_) {
    return Error("Cannot rewind to index " + std::to_string(index) +
                 " (max: " + std::to_string(totalBlockCount_) + ")");
  }

  if (index == totalBlockCount_) {
    return {};
  }

  // Find the volume that should remain as tip after rewind.
  // Prefer the last volume with startBlockId <= index (covers empty tip volumes).
  uint32_t volumeId = 0;
  uint64_t indexWithin = 0;
  for (uint32_t vid : volumeIdOrder_) {
    auto it = volumeInfoMap_.find(vid);
    if (it == volumeInfoMap_.end()) {
      continue;
    }
    uint64_t start = it->second.startBlockId;
    if (index < start) {
      break;
    }
    volumeId = vid;
    indexWithin = index - start;
  }

  std::vector<uint32_t> toRemove;
  for (uint32_t vid : volumeIdOrder_) {
    if (vid > volumeId) {
      toRemove.push_back(vid);
    }
  }

  for (uint32_t vid : toRemove) {
    auto it = volumeInfoMap_.find(vid);
    if (it != volumeInfoMap_.end()) {
      if (it->second.store) {
        it->second.store->close();
      }
      std::string path = getVolumePath(vid);
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
      volumeInfoMap_.erase(vid);
    }
    volumeIdOrder_.erase(
        std::remove(volumeIdOrder_.begin(), volumeIdOrder_.end(), vid),
        volumeIdOrder_.end());
  }

  if (volumeId > 0) {
    auto it = volumeInfoMap_.find(volumeId);
    if (it != volumeInfoMap_.end() && it->second.store) {
      auto rewindResult = it->second.store->rewindTo(indexWithin);
      if (!rewindResult.isOk()) {
        return Error("Failed to rewind volume: " +
                     rewindResult.error().message);
      }
    }
  }

  recalculateTotalBlockCount();
  updateCurrentVolumeId();
  saveIndex();
  return {};
}

FileDirStore *VolumeStore::getActiveVolume(uint64_t dataSize) {
  auto it = volumeInfoMap_.find(currentVolumeId_);
  if (it != volumeInfoMap_.end() && it->second.store &&
      it->second.store->canFit(dataSize)) {
    return it->second.store.get();
  }

  if (volumeInfoMap_.size() >= config_.maxVolumes) {
    log().error << "Reached max volumes: " << config_.maxVolumes;
    return nullptr;
  }

  uint32_t nextId = currentVolumeId_ + 1;
  if (nextId == 0) {
    nextId = 1;
  }
  FileDirStore *created = createVolume(nextId, totalBlockCount_);
  if (!created) {
    return nullptr;
  }
  currentVolumeId_ = nextId;
  saveIndex();
  return created;
}

FileDirStore *VolumeStore::createVolume(uint32_t volumeId,
                                        uint64_t startBlockId) {
  std::string path = getVolumePath(volumeId);
  auto store = std::make_unique<FileDirStore>();
  store->redirectLogger(log().getFullName() + ".Vol" +
                        std::to_string(volumeId));

  FileDirStore::InitConfig fdConfig;
  fdConfig.dirPath = path;
  fdConfig.maxFileCount = config_.maxFileCount;
  fdConfig.maxFileSize = config_.maxFileSize;
  auto result = store->init(fdConfig);
  if (!result.isOk()) {
    log().error << "Failed to create volume " << path << ": "
                << result.error().message;
    return nullptr;
  }

  FileDirStore *raw = store.get();
  VolumeInfo info;
  info.store = std::move(store);
  info.startBlockId = startBlockId;
  volumeInfoMap_[volumeId] = std::move(info);
  volumeIdOrder_.push_back(volumeId);

  log().info << "Created volume " << path
             << " (startBlockId: " << startBlockId << ")";
  return raw;
}

std::pair<uint32_t, uint64_t>
VolumeStore::findBlockVolume(uint64_t blockId) const {
  for (uint32_t volumeId : volumeIdOrder_) {
    auto it = volumeInfoMap_.find(volumeId);
    if (it == volumeInfoMap_.end() || !it->second.store) {
      continue;
    }
    uint64_t start = it->second.startBlockId;
    uint64_t count = it->second.store->getBlockCount();
    if (count > 0 && blockId >= start && blockId < start + count) {
      return {volumeId, blockId - start};
    }
  }
  return {0, 0};
}

bool VolumeStore::loadIndex() {
  std::ifstream indexFile(indexFilePath_, std::ios::binary);
  if (!indexFile.is_open()) {
    log().error << "Failed to open volumes index: " << indexFilePath_;
    return false;
  }

  volumeInfoMap_.clear();
  volumeIdOrder_.clear();

  if (!readIndexHeader(indexFile)) {
    indexFile.close();
    return false;
  }

  VolumeIndexEntry entry;
  while (indexFile.good() && !indexFile.eof()) {
    if (indexFile.peek() == EOF) {
      break;
    }
    InputArchive ar(indexFile);
    ar & entry;
    if (ar.failed()) {
      break;
    }
    VolumeInfo info;
    info.store = nullptr;
    info.startBlockId = entry.startBlockId;
    volumeInfoMap_[entry.volumeId] = std::move(info);
    volumeIdOrder_.push_back(entry.volumeId);
  }

  indexFile.close();
  return true;
}

bool VolumeStore::saveIndex() {
  std::string tempPath = indexFilePath_ + ".tmp";
  std::ofstream indexFile(tempPath, std::ios::binary | std::ios::trunc);
  if (!indexFile.is_open()) {
    log().error << "Failed to open volumes index for writing: " << tempPath;
    return false;
  }

  if (!writeIndexHeader(indexFile)) {
    indexFile.close();
    return false;
  }

  for (uint32_t volumeId : volumeIdOrder_) {
    auto it = volumeInfoMap_.find(volumeId);
    if (it == volumeInfoMap_.end()) {
      continue;
    }
    VolumeIndexEntry entry;
    entry.volumeId = volumeId;
    entry.startBlockId = it->second.startBlockId;
    std::string packed = utl::binaryPack(entry);
    indexFile.write(packed.data(),
                    static_cast<std::streamsize>(packed.size()));
  }

  indexFile.close();

  std::error_code ec;
  std::filesystem::rename(tempPath, indexFilePath_, ec);
  if (ec) {
    log().error << "Failed to rename volumes index: " << ec.message();
    return false;
  }
  return true;
}

bool VolumeStore::writeIndexHeader(std::ostream &os) {
  IndexFileHeader header;
  header.maxFileCount = config_.maxFileCount;
  header.maxFileSize = config_.maxFileSize;
  header.maxVolumes = config_.maxVolumes;
  OutputArchive ar(os);
  ar & header;
  return os.good();
}

bool VolumeStore::readIndexHeader(std::istream &is) {
  IndexFileHeader header;
  InputArchive ar(is);
  ar & header;
  if (ar.failed()) {
    log().error << "Failed to read volumes index header";
    return false;
  }
  if (header.magic != IndexFileHeader::MAGIC) {
    log().error << "Invalid volumes index magic: 0x" << std::hex << header.magic;
    return false;
  }
  if (header.version != IndexFileHeader::CURRENT_VERSION) {
    log().error << "Unsupported volumes index version: " << header.version;
    return false;
  }
  config_.maxFileCount = static_cast<size_t>(header.maxFileCount);
  config_.maxFileSize = static_cast<size_t>(header.maxFileSize);
  config_.maxVolumes = static_cast<size_t>(header.maxVolumes);
  return true;
}

void VolumeStore::flush() {
  if (!saveIndex()) {
    log().error << "Failed to save volumes index during flush";
  }
}

VolumeStore::Roe<void> VolumeStore::openExistingVolumes() {
  for (uint32_t volumeId : volumeIdOrder_) {
    auto it = volumeInfoMap_.find(volumeId);
    if (it == volumeInfoMap_.end()) {
      continue;
    }
    std::string path = getVolumePath(volumeId);
    auto store = std::make_unique<FileDirStore>();
    store->redirectLogger(log().getFullName() + ".Vol" +
                          std::to_string(volumeId));
    auto result = store->mount(path);
    if (!result.isOk()) {
      return Error("Failed to mount volume " + path + ": " +
                   result.error().message);
    }
    it->second.store = std::move(store);
  }
  return {};
}

void VolumeStore::recalculateTotalBlockCount() {
  totalBlockCount_ = 0;
  for (uint32_t volumeId : volumeIdOrder_) {
    auto it = volumeInfoMap_.find(volumeId);
    if (it != volumeInfoMap_.end() && it->second.store) {
      totalBlockCount_ += it->second.store->getBlockCount();
    }
  }
}

void VolumeStore::updateCurrentVolumeId() {
  currentVolumeId_ = 0;
  for (uint32_t volumeId : volumeIdOrder_) {
    if (volumeId > currentVolumeId_) {
      currentVolumeId_ = volumeId;
    }
  }
}

} // namespace pp
