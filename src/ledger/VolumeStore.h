#ifndef PP_LEDGER_VOLUME_STORE_H
#define PP_LEDGER_VOLUME_STORE_H

#include "DirStore.h"
#include "FileDirStore.h"
#include "lib/common/BinaryPack.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pp {

/**
 * VolumeStore — ordered volumes of FileDirStore.
 *
 * Growth opens a new empty volume when the active (last) volume cannot fit a
 * block. Existing volumes are never relocated or rewritten for capacity.
 *
 * Layout:
 *   volumesDir/
 *     volumes_idx.dat
 *     v000001/   # FileDirStore
 *     v000002/
 */
class VolumeStore : public DirStore {
public:
  struct InitConfig {
    std::string volumesDir;
    size_t maxFileCount{0};
    size_t maxFileSize{0};
    size_t maxVolumes{0};
  };

  struct MountConfig {
    std::string volumesDir;
  };

  VolumeStore();
  ~VolumeStore() override;

  bool canFit(uint64_t size) const override;
  uint64_t getBlockCount() const override;
  uint64_t countSizeFromBlockId(uint64_t blockId) const override;

  Roe<void> init(const InitConfig &config);
  Roe<void> mount(const MountConfig &config);

  Roe<std::string> readBlock(uint64_t index) const override;
  Roe<uint64_t> appendBlock(const std::string &block) override;
  Roe<void> rewindTo(uint64_t index) override;

  void close();

  size_t getVolumeCount() const { return volumeIdOrder_.size(); }

private:
  static constexpr const char *VOLUMES_INDEX_FILENAME = "volumes_idx.dat";

  struct Config {
    std::string volumesDir;
    size_t maxFileCount{0};
    size_t maxFileSize{0};
    size_t maxVolumes{0};
  };

  struct IndexFileHeader {
    static constexpr uint32_t MAGIC = MAGIC_VOLUMES; // "PLVO"
    static constexpr uint16_t CURRENT_VERSION = 1;

    uint32_t magic{MAGIC};
    uint16_t version{CURRENT_VERSION};
    uint16_t reserved{0};
    uint64_t headerSize{sizeof(IndexFileHeader)};
    uint64_t maxFileCount{0};
    uint64_t maxFileSize{0};
    uint64_t maxVolumes{0};

    IndexFileHeader() = default;

    template <typename Archive> void serialize(Archive &ar) {
      ar & magic & version & reserved & headerSize & maxFileCount &
          maxFileSize & maxVolumes;
    }
  };

  struct VolumeIndexEntry {
    uint32_t volumeId{0};
    uint64_t startBlockId{0};

    template <typename Archive> void serialize(Archive &ar) {
      ar & volumeId & startBlockId;
    }
  };

  struct VolumeInfo {
    std::unique_ptr<FileDirStore> store;
    uint64_t startBlockId{0};
  };

  Config config_;
  std::string indexFilePath_;
  uint32_t currentVolumeId_{0};
  std::unordered_map<uint32_t, VolumeInfo> volumeInfoMap_;
  std::vector<uint32_t> volumeIdOrder_;
  uint64_t totalBlockCount_{0};

  static std::string getVolumesIndexFilePath(const std::string &volumesDir);
  std::string getVolumePath(uint32_t volumeId) const;
  std::pair<uint32_t, uint64_t> findBlockVolume(uint64_t blockId) const;

  FileDirStore *getActiveVolume(uint64_t dataSize);
  FileDirStore *createVolume(uint32_t volumeId, uint64_t startBlockId);

  bool loadIndex();
  bool saveIndex();
  bool writeIndexHeader(std::ostream &os);
  bool readIndexHeader(std::istream &is);
  void flush();

  Roe<void> openExistingVolumes();
  void recalculateTotalBlockCount();
  void updateCurrentVolumeId();
};

} // namespace pp

#endif // PP_LEDGER_VOLUME_STORE_H
