#include "OpdsFilename.h"

#include "StringUtils.h"

std::string opdsBookFilename(const std::string& author, const std::string& title, OpdsFilenameFormat format) {
  std::string base;
  switch (format) {
    case OpdsFilenameFormat::TitleAuthor:
      base = author.empty() ? title : title + " - " + author;
      break;
    case OpdsFilenameFormat::TitleOnly:
      base = title;
      break;
    case OpdsFilenameFormat::AuthorTitle:
    default:
      base = author.empty() ? title : author + " - " + title;
      break;
  }
  // sanitizeFilename caps at 100 bytes and never returns empty (falls back to
  // "book"); ".epub" is appended after so the extension is never truncated —
  // identical treatment to the previous inline construction.
  return StringUtils::sanitizeFilename(base) + ".epub";
}

std::string cinabrioBookFilename(const std::string& author, const std::string& title, OpdsFilenameFormat format,
                                 const std::string& acquisitionHref) {
  const std::string marker = "/api/opds/books/";
  const size_t start = acquisitionHref.find(marker);
  if (start == std::string::npos) return opdsBookFilename(author, title, format);
  const std::string id = acquisitionHref.substr(start + marker.size(), 36);
  if (id.size() != 36 || acquisitionHref.substr(start + marker.size() + 36, 10) != "/file.epub")
    return opdsBookFilename(author, title, format);
  for (size_t i = 0; i < id.size(); ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? id[i] != '-'
             : !((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f') || (id[i] >= 'A' && id[i] <= 'F')))
      return opdsBookFilename(author, title, format);
  }
  const std::string displayName = opdsBookFilename(author, title, format);
  return displayName.substr(0, displayName.size() - 5) + " [" + id + "].epub";
}
