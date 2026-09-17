#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "TotoResumeFlow.h"

namespace toto {

// Where the answers to "continue from the other device?" are kept.
//
// The answer has to outlive the screen that asked. Before this, backing out of
// the dialog only set a flag in RAM, so the same position was proposed again
// on the next visit and after every reboot -- exactly the insistence the
// contract rules out. Here it is a file, written once per answer.
//
// It is account data: it names suggestions issued to one account, so it is
// wiped together with the queues when the reader changes account.
class ResumeStore {
 public:
  static ResumeStore& instance();

  // Reads the file once per boot. A missing or damaged file is an empty log,
  // never a failure: at worst the reader is asked one more time.
  ResumeDecisionLog& decisions();

  // Records the answer and writes it through. Returns false only when the
  // position had nothing to identify it or the card could not be written --
  // the caller then keeps the suggestion instead of pretending it was answered.
  bool remember(std::string_view bookHash, const ResumePosition& position, bool accepted, std::string_view suggestionId,
                uint64_t at, bool synced);

  bool markSynced(std::string_view bookHash, std::string_view suggestionId);
  bool forget(std::string_view bookHash);

  // Drops every remembered answer. Called on the account boundary.
  bool purge();

  bool save();

 private:
  ResumeStore() = default;

  ResumeDecisionLog log;
  bool loaded = false;

  void load();
};

}  // namespace toto

#define TOTO_RESUME toto::ResumeStore::instance()
