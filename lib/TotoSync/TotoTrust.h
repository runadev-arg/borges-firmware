#pragma once

#include <string>

namespace toto {

const char* rootCertificate();

// `detail`, when not null, receives the tag of the path that did (or did not)
// set the clock: "" when it was already in time, "anchor:ok" (durable-queue
// wall anchor), "sntp:ok", "httpdate:ok" or "httpdate:fail(...)" preceded by
// the resolution tag.
bool ensureTrustedClock(std::string* detail = nullptr);

}  // namespace toto
