#include "TotoHttp.h"

#include <Arduino.h>
#include <SecureHttpClient.h>

#include "TotoCredentialStore.h"
#include "TotoTrust.h"

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "development"
#endif

namespace toto {
namespace {

constexpr uint32_t MIN_FREE_FOR_TLS = 50000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

// ISRG Root X1 from https://letsencrypt.org/certs/isrgrootx1.pem.
// The owned endpoint serves Let's Encrypt's default compatibility chain to
// this trust anchor. No caller may fall back to setInsecure().
constexpr char ISRG_ROOT_X1[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)PEM";

bool insufficientHeap() { return ESP.getFreeHeap() < MIN_FREE_FOR_TLS || ESP.getMaxAllocHeap() < MIN_BLOCK_FOR_TLS; }

}  // namespace

int postJson(const HttpRequest& request, std::string& response) {
  if (request.path == nullptr || request.body == nullptr) return HTTP_BEGIN_FAILED;
  if (request.requireHeadroom && insufficientHeap()) return HTTP_LOW_MEMORY;
  if (request.requireTrustedClock && !ensureTrustedClock()) return HTTP_CLOCK_ERROR;
  const std::string& baseUrl = TOTO_CREDENTIALS.getBaseUrl();
  if (baseUrl.rfind("https://", 0) != 0) return HTTP_INSECURE_URL;

  freeink::SecureHttpClient http;
  http.setCACert(ISRG_ROOT_X1);
  http.setTimeout(request.timeoutMs);
  http.setReuse(false);
  http.setUserAgent(std::string("CrossPoint-Toto/") + CROSSPOINT_VERSION);
  if (!http.begin(baseUrl + request.path)) return HTTP_BEGIN_FAILED;
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");
  if (request.bearerToken != nullptr) http.addHeader("Authorization", std::string("Bearer ") + request.bearerToken);

  response.clear();
  response.reserve(request.reserveBytes);
  const size_t limit = request.maxResponseBytes;
  const int status = http.sendRequest("POST", reinterpret_cast<const uint8_t*>(request.body->data()),
                                      request.body->size(), [&response, limit](const uint8_t* data, size_t size) {
                                        if (response.size() + size > limit) return false;
                                        response.append(reinterpret_cast<const char*>(data), size);
                                        return true;
                                      });
  const bool complete = http.responseComplete() && !http.callbackAborted();
  http.end();
  return complete ? status : HTTP_INCOMPLETE;
}

}  // namespace toto
