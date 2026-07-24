#pragma once

#include <cstddef>
#include <cstdint>

#include "StreamingJsonParser.h"

class ReleaseJsonParser {
 public:
  ReleaseJsonParser();

  ReleaseJsonParser(const ReleaseJsonParser&) = delete;
  ReleaseJsonParser& operator=(const ReleaseJsonParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);

  bool foundTag() const;
  bool foundFirmware() const;
  const char* getTagName() const;
  const char* getFirmwareUrl() const;
  size_t getFirmwareSize() const;
  const char* getProduct() const;
  const char* getTarget() const;
  const char* getChannel() const;
  const char* getFirmwareSha256() const;
  uint32_t getManifestSchema() const;
  uint32_t getSyncProtocolMin() const;

 private:
  enum class Position : uint8_t {
    TOP_LEVEL,
    IN_ASSETS_ARRAY,
    IN_ASSET_OBJECT,
  };

  enum class LastKey : uint8_t {
    NONE,
    TAG_NAME,
    ASSETS,
    FIRMWARE,
    ASSET_NAME,
    ASSET_URL,
    ASSET_SIZE,
    PRODUCT,
    TARGET,
    CHANNEL,
    MANIFEST_SCHEMA,
    SYNC_PROTOCOL_MIN,
    ASSET_SHA256,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void commitAsset();

  StreamingJsonParser parser;

  Position position;
  LastKey lastKey;
  uint8_t depth;
  uint8_t assetDepth;
  bool directFirmwareObject;

  char tagName[32];
  char firmwareUrl[512];
  char product[32];
  char target[48];
  char channel[16];
  char firmwareSha256[65];
  size_t firmwareSize;
  uint32_t manifestSchema;
  uint32_t syncProtocolMin;
  bool tagFound;
  bool firmwareFound;

  char currentAssetName[32];
  char currentAssetUrl[512];
  char currentAssetSha256[65];
  size_t currentAssetSize;
  bool currentAssetSha256Valid;
};
