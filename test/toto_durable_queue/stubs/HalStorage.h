#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  bool isEmpty() const { return empty(); }
  bool endsWith(const char* suffix) const { return ends_with(suffix); }
};

struct HalFile {
  std::vector<uint8_t>* bytes = nullptr;
  size_t offset = 0;
  size_t fileSize() const { return bytes ? bytes->size() : 0; }
  int read(uint8_t* dest, size_t length) {
    if (!bytes) return -1;
    length = std::min(length, bytes->size() - offset);
    std::copy_n(bytes->begin() + offset, length, dest);
    offset += length;
    return static_cast<int>(length);
  }
  size_t write(const uint8_t* src, size_t length) {
    if (!bytes) return 0;
    bytes->insert(bytes->end(), src, src + length);
    return length;
  }
  void flush() {}
};

struct FakeStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  bool failRename = false;
  bool failOpen = false;
  int renameCalls = 0;
  int failRenameFromCall = 0;
  bool ensureDirectoryExists(const char*) { return true; }
  bool exists(const char* path) const { return files.contains(path); }
  bool remove(const char* path) { return files.erase(path) != 0; }
  bool rename(const char* from, const char* to) {
    ++renameCalls;
    if (failRename || (failRenameFromCall > 0 && renameCalls >= failRenameFromCall) || !files.contains(from) ||
        files.contains(to))
      return false;
    files[to] = files[from];
    files.erase(from);
    return true;
  }
  std::vector<String> listFiles(const char* dir, size_t limit) const {
    std::vector<String> result;
    const std::string prefix = std::string(dir) + "/";
    for (const auto& [path, data] : files) {
      if (path.starts_with(prefix) && path.find('/', prefix.size()) == std::string::npos) {
        result.emplace_back(path.substr(prefix.size()));
        if (result.size() == limit) break;
      }
    }
    return result;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    if (failOpen || !files.contains(path)) return false;
    file.bytes = &files[path];
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    if (failOpen) return false;
    files[path].clear();
    file.bytes = &files[path];
    return true;
  }
  String readFile(const char* path) const {
    if (!files.contains(path)) return {};
    const auto& bytes = files.at(path);
    return String(bytes.begin(), bytes.end());
  }
};
inline FakeStorage Storage;
