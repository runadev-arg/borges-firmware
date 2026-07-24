#pragma once

#include <string>
#include <vector>

struct BookmarkEntry;

namespace toto {

class BookmarkSync {
 public:
  using PersistBookmarks = bool (*)(const std::string&, const std::vector<BookmarkEntry>&);

  static BookmarkSync& instance();

  bool reconcile(const std::string& epubPath, const std::string& title, const std::string& author,
                 std::vector<BookmarkEntry>& bookmarks, PersistBookmarks persist);
  bool enqueueLocalUpsert(const std::string& epubPath, const std::string& title, const std::string& author,
                          BookmarkEntry& bookmark, const std::string& visiblePassage);
  bool enqueueLocalDelete(const std::string& epubPath, const std::string& title, const std::string& author,
                          const BookmarkEntry& bookmark);

 private:
  BookmarkSync() = default;
};

}  // namespace toto

#define TOTO_BOOKMARK_SYNC toto::BookmarkSync::instance()
