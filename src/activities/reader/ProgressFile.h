#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ProgressFile {

// FAT cannot rename over an existing file. Keep the last complete position in
// .bak while publishing the temporary file, and read it if power was lost
// between the two renames. Never remove the sole complete position.
inline bool openForRead(const char* tag, const std::string& cachePath, HalFile& file) {
  return Storage.openFileForRead(tag, cachePath + "/progress.bin", file) ||
         Storage.openFileForRead(tag, cachePath + "/progress.bin.bak", file);
}

// Returns true only after the complete replacement is in progress.bin.
inline bool writeAtomic(const std::string& cachePath, const uint8_t* data, size_t len) {
  const std::string finalPath = cachePath + "/progress.bin";
  const std::string tmpPath = cachePath + "/progress.bin.tmp";
  const std::string backupPath = cachePath + "/progress.bin.bak";

  {
    HalFile f;
    if (!Storage.openFileForWrite("PRG", tmpPath, f)) {
      LOG_ERR("PRG", "Could not open temp progress file for write: %s", tmpPath.c_str());
      return false;
    }
    const size_t written = f.write(data, len);
    if (written != len) {
      LOG_ERR("PRG", "Short write saving progress to %s: %u/%u bytes", tmpPath.c_str(), (unsigned)written,
              (unsigned)len);
      return false;
    }
    f.flush();
    // f (the temp file) is closed at scope exit (DESTRUCTOR_CLOSES_FILE=1) before
    // the rename below -- SdFat must not rename a path that still has an open FsFile.
  }

  if (Storage.exists(finalPath.c_str())) {
    if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) return false;
    if (!Storage.rename(finalPath.c_str(), backupPath.c_str())) return false;
  }
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    // If rollback also fails, openForRead still recovers the complete backup.
    if (Storage.exists(backupPath.c_str())) Storage.rename(backupPath.c_str(), finalPath.c_str());
    LOG_ERR("PRG", "Failed to rename temp progress into place: %s", finalPath.c_str());
    return false;
  }
  return true;
}

}  // namespace ProgressFile
