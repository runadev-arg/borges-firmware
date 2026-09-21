#pragma once
#include <cstdint>
#include <string>

// A single bookmark entry — a position in a book.
struct BookmarkEntry {
  std::string xpath;               // XPath-like progress string
  std::string summary;             // First few words of a page to help identify it
  std::string borgesSyncId;          // Stable cross-device annotation identity
  std::string borgesNote;            // Bounded remote note shown on the reader
  std::string borgesFingerprint;     // Last locally queued/applied snapshot
  std::string borgesPendingEventId;  // Durable event that still needs its own ACK
  float percentage;                // Progress percentage (0.0 to 1.0)
  uint64_t borgesRevision = 0;       // Server-authoritative optimistic revision
  uint64_t borgesPendingSequence = 0;
  bool borgesConflict = false;

  uint16_t computedSpineIndex = 0;        // Spine index at the time of bookmarking
  uint16_t computedChapterPageCount = 0;  // Total page count of the chapter at the time of bookmarking
  uint16_t computedChapterProgress = 0;   // Number of pages into the chapter at the time of bookmarking

  // Exact visible-codepoint offset of the bookmarked page within its spine. Unlike the page
  // number above it is immune to re-pagination, so it lands on the right page under any
  // font/margin/orientation. Absent (hasVisibleTextOffset == false) for pre-offset bookmarks.
  bool hasVisibleTextOffset = false;
  uint32_t visibleTextOffset = 0;
};
