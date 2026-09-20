#pragma once

#include <string_view>

namespace download_policy {
inline std::string_view origin(std::string_view url) {
  const auto scheme = url.find("://");
  if (scheme == std::string_view::npos) return {};
  const auto end = url.find_first_of("/?#", scheme + 3);
  return url.substr(0, end);
}

inline bool sameOrigin(std::string_view first, std::string_view second) {
  return !origin(first).empty() && origin(first) == origin(second);
}

inline bool safeRedirect(std::string_view first, std::string_view next, bool hasCredentials) {
  if (first.starts_with("https://") && !next.starts_with("https://")) return false;
  return !hasCredentials || sameOrigin(first, next);
}
}  // namespace download_policy
