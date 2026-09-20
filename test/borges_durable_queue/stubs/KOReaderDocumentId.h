#pragma once
#include <string>
struct KOReaderDocumentId {
  static std::string calculate(const std::string& path) {
    return std::string(32, path.find("other") == std::string::npos ? '1' : '2');
  }
};
