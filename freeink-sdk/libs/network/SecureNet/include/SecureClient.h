#pragma once

// FreeInk SDK — TLS 1.3 secure client.
//
// WHY: the precompiled mbedTLS shipped in the ESP-IDF/pioarduino package has
// TLS 1.3 compiled out as empty stubs (PSA crypto prerequisites disabled), so
// WiFiClientSecure / esp_http_client cannot reach TLS-1.3-only servers
// (e.g. KOSync at kosync.ak-team.com:3042 — handshake fails with
// -0x7780 MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE). A -D Kconfig flag can't change a
// precompiled .a, and a custom_sdkconfig rebuild fails on managed-component
// dependencies. The only fix that doesn't rebuild ESP-IDF is to bring our own
// TLS stack compiled from source: wolfSSL, which supports TLS 1.3 + PSA.
//
// SecureClient is an Arduino Client wrapping a wolfSSL session over a plain
// WiFiClient transport, independent of system mbedTLS.
//
// OPT-IN: enable with -DFREEINK_NET_WOLFSSL=1 and add wolfSSL to lib_deps. With
// the flag off, this compiles to an inert no-op (connectSecure() returns false)
// so the rest of the SDK builds without the wolfSSL dependency present.

#include <Arduino.h>
#include <Client.h>
#include <WiFiClient.h>

namespace freeink {

class SecureClient : public Client {
 public:
  SecureClient() = default;
  ~SecureClient() override;

  // Certificate / verification configuration (applied before connect()).
  void setCACert(const char* rootCA);
  void setInsecure();  // skip peer verification (testing only)

  // Connect and perform a TLS 1.3 handshake to host:port (uses the SNI host).
  int connect(IPAddress ip, uint16_t port) override;
  int connect(const char* host, uint16_t port) override;

  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buf, size_t size) override;
  int available() override;
  int read() override;
  int read(uint8_t* buf, size_t size) override;
  int peek() override;
  void flush() override;
  void stop() override;
  uint8_t connected() override;
  operator bool() override { return connected(); }

  // True if the library was built with wolfSSL TLS 1.3 support enabled.
  static bool tls13Available();

  // Diagnostic detail for the last failed connect(): which stage failed
  // ("dns", "tcp", "ctx", "ca_load", "ssl_alloc", "sni", "tls", "tls_timeout",
  // "no_tls") plus the wolfSSL error code when one exists. Empty stage means
  // the last connect() succeeded. Serial logging alone is not enough on
  // devices with no accessible UART, so callers surface this in the UI.
  const char* lastErrorStage() const { return _lastStage; }
  int lastErrorCode() const { return _lastCode; }

  // Pin of the TCP endpoint. While set, connect(host, port) does NOT resolve
  // `host`: it dials this address directly. SNI (wolfSSL_UseSNI) and the
  // certificate name check (wolfSSL_check_domain_name) keep using `host`, so
  // verification is never weakened -- only the A-record lookup is bypassed.
  // Exists because a network whose DNS-over-UDP is dead (captive APs, phone
  // hotspots) can still reach the server once the caller has an address from
  // some other channel.
  // This is transport configuration: it survives stop() and successive
  // connects. setResolvedAddress(IPAddress()) (0.0.0.0) clears the pin.
  void setResolvedAddress(IPAddress ip);
  void clearResolvedAddress();
  bool hasResolvedAddress() const;
  IPAddress resolvedAddress() const;

  // IPv4 resolution over the BSD socket API (lwip_getaddrinfo). Replaces
  // Network.hostByName across the whole SDK: NetworkManager::hostByName calls
  // raw-lwIP dns_clear_cache() without the TCPIP core lock, and with the DNS
  // query of a just-timed-out SNTP attempt still pending, the clear fires
  // SNTP's dns callback on the CALLING task -- its re-request allocates a UDP
  // pcb and lwIP's LWIP_ASSERT_CORE_LOCKED panics the device
  // (udp_new_ip_type, udp.c:1278). Available with and without
  // FREEINK_NET_WOLFSSL.
  static bool resolveHostIPv4(const char* host, IPAddress& out);

 private:
  int connectWithMethod(const char* host, uint16_t port, void* method, const char* label);

  WiFiClient _transport;
  const char* _rootCA = nullptr;
  bool _insecure = false;
  void* _ssl = nullptr;  // WOLFSSL* (opaque to keep wolfSSL headers out of here)
  void* _ctx = nullptr;  // WOLFSSL_CTX*
  bool _connected = false;
  const char* _lastStage = "";
  int _lastCode = 0;
  IPAddress _pinnedIp;
  bool _pinned = false;
};

}  // namespace freeink
