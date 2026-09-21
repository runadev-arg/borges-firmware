#include "ReleaseJsonParser.h"

#include <cstdlib>
#include <cstring>

namespace {

void safeCopy(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  size_t n = srcLen < dstSize - 1 ? srcLen : dstSize - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

}  // namespace

ReleaseJsonParser::ReleaseJsonParser()
    : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd}) {
  safeCopy(firmwareAssetName, sizeof(firmwareAssetName), "firmware.bin", sizeof("firmware.bin") - 1);
  reset();
}

void ReleaseJsonParser::setFirmwareAssetName(const char* name) {
  safeCopy(firmwareAssetName, sizeof(firmwareAssetName), name, strlen(name));
}

void ReleaseJsonParser::reset() {
  parser.reset();
  position = Position::TOP_LEVEL;
  lastKey = LastKey::NONE;
  depth = 0;
  assetDepth = 0;
  directFirmwareObject = false;
  tagName[0] = '\0';
  firmwareUrl[0] = '\0';
  product[0] = '\0';
  target[0] = '\0';
  channel[0] = '\0';
  firmwareSha256[0] = '\0';
  firmwareSize = 0;
  manifestSchema = 0;
  syncProtocolMin = 0;
  tagFound = false;
  firmwareFound = false;
  currentAssetName[0] = '\0';
  currentAssetUrl[0] = '\0';
  currentAssetSha256[0] = '\0';
  currentAssetSize = 0;
  currentAssetSha256Valid = false;
}

void ReleaseJsonParser::feed(const char* data, size_t len) { parser.feed(data, len); }

bool ReleaseJsonParser::foundTag() const { return tagFound; }
bool ReleaseJsonParser::foundFirmware() const { return firmwareFound; }
const char* ReleaseJsonParser::getTagName() const { return tagName; }
const char* ReleaseJsonParser::getFirmwareUrl() const { return firmwareUrl; }
size_t ReleaseJsonParser::getFirmwareSize() const { return firmwareSize; }
const char* ReleaseJsonParser::getProduct() const { return product; }
const char* ReleaseJsonParser::getTarget() const { return target; }
const char* ReleaseJsonParser::getChannel() const { return channel; }
const char* ReleaseJsonParser::getFirmwareSha256() const { return firmwareSha256; }
uint32_t ReleaseJsonParser::getManifestSchema() const { return manifestSchema; }
uint32_t ReleaseJsonParser::getSyncProtocolMin() const { return syncProtocolMin; }

void ReleaseJsonParser::commitAsset() {
  if (strcmp(currentAssetName, firmwareAssetName) == 0) {
    memcpy(firmwareUrl, currentAssetUrl, sizeof(firmwareUrl));
    if (currentAssetSha256Valid)
      memcpy(firmwareSha256, currentAssetSha256, sizeof(firmwareSha256));
    else
      firmwareSha256[0] = '\0';
    firmwareSize = currentAssetSize;
    firmwareFound = true;
  }
  currentAssetName[0] = '\0';
  currentAssetUrl[0] = '\0';
  currentAssetSha256[0] = '\0';
  currentAssetSize = 0;
  currentAssetSha256Valid = false;
}

// -- SAX callbacks (static trampolines) -------------------------------------

void ReleaseJsonParser::sOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth == 1) {
        if ((len == 8 && memcmp(key, "tag_name", 8) == 0) || (len == 7 && memcmp(key, "version", 7) == 0))
          self->lastKey = LastKey::TAG_NAME;
        else if (len == 6 && memcmp(key, "assets", 6) == 0)
          self->lastKey = LastKey::ASSETS;
        else if (len == 8 && memcmp(key, "firmware", 8) == 0)
          self->lastKey = LastKey::FIRMWARE;
        else if (len == 7 && memcmp(key, "product", 7) == 0)
          self->lastKey = LastKey::PRODUCT;
        else if (len == 6 && memcmp(key, "target", 6) == 0)
          self->lastKey = LastKey::TARGET;
        else if (len == 7 && memcmp(key, "channel", 7) == 0)
          self->lastKey = LastKey::CHANNEL;
        else if (len == 15 && memcmp(key, "manifest_schema", 15) == 0)
          self->lastKey = LastKey::MANIFEST_SCHEMA;
        else if (len == 17 && memcmp(key, "sync_protocol_min", 17) == 0)
          self->lastKey = LastKey::SYNC_PROTOCOL_MIN;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    case Position::IN_ASSET_OBJECT:
      if (self->assetDepth == 1) {
        if (len == 4 && memcmp(key, "name", 4) == 0)
          self->lastKey = LastKey::ASSET_NAME;
        else if ((len == 20 && memcmp(key, "browser_download_url", 20) == 0) ||
                 (len == 3 && memcmp(key, "url", 3) == 0))
          self->lastKey = LastKey::ASSET_URL;
        else if (len == 4 && memcmp(key, "size", 4) == 0)
          self->lastKey = LastKey::ASSET_SIZE;
        else if (len == 6 && memcmp(key, "sha256", 6) == 0)
          self->lastKey = LastKey::ASSET_SHA256;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->lastKey) {
    case LastKey::TAG_NAME:
      if (self->position == Position::TOP_LEVEL && self->depth == 1) {
        safeCopy(self->tagName, sizeof(self->tagName), value, len);
        self->tagFound = true;
      }
      break;
    case LastKey::ASSET_NAME:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1)
        safeCopy(self->currentAssetName, sizeof(self->currentAssetName), value, len);
      break;
    case LastKey::ASSET_URL:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1)
        safeCopy(self->currentAssetUrl, sizeof(self->currentAssetUrl), value, len);
      break;
    case LastKey::PRODUCT:
      if (self->position == Position::TOP_LEVEL && self->depth == 1)
        safeCopy(self->product, sizeof(self->product), value, len);
      break;
    case LastKey::TARGET:
      if (self->position == Position::TOP_LEVEL && self->depth == 1)
        safeCopy(self->target, sizeof(self->target), value, len);
      break;
    case LastKey::CHANNEL:
      if (self->position == Position::TOP_LEVEL && self->depth == 1)
        safeCopy(self->channel, sizeof(self->channel), value, len);
      break;
    case LastKey::ASSET_SHA256:
      if (self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1) {
        self->currentAssetSha256Valid = len == 64;
        for (size_t i = 0; self->currentAssetSha256Valid && i < len; ++i) {
          const char c = value[i];
          self->currentAssetSha256Valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }
        if (self->currentAssetSha256Valid)
          safeCopy(self->currentAssetSha256, sizeof(self->currentAssetSha256), value, len);
        else
          self->currentAssetSha256[0] = '\0';
      }
      break;
    default:
      break;
  }
  self->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnNumber(void* ctx, const char* value, size_t /*len*/) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  if (self->lastKey == LastKey::ASSET_SIZE && self->position == Position::IN_ASSET_OBJECT && self->assetDepth == 1) {
    self->currentAssetSize = static_cast<size_t>(strtoul(value, nullptr, 10));
  } else if (self->lastKey == LastKey::MANIFEST_SCHEMA && self->position == Position::TOP_LEVEL && self->depth == 1) {
    self->manifestSchema = static_cast<uint32_t>(strtoul(value, nullptr, 10));
  } else if (self->lastKey == LastKey::SYNC_PROTOCOL_MIN && self->position == Position::TOP_LEVEL && self->depth == 1) {
    self->syncProtocolMin = static_cast<uint32_t>(strtoul(value, nullptr, 10));
  }
  self->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnBool(void* ctx, bool /*value*/) {
  static_cast<ReleaseJsonParser*>(ctx)->lastKey = LastKey::NONE;
}

void ReleaseJsonParser::sOnNull(void* ctx) { static_cast<ReleaseJsonParser*>(ctx)->lastKey = LastKey::NONE; }

void ReleaseJsonParser::sOnObjectStart(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->lastKey == LastKey::FIRMWARE && self->depth == 1) {
        self->position = Position::IN_ASSET_OBJECT;
        self->assetDepth = 1;
        self->directFirmwareObject = true;
        safeCopy(self->currentAssetName, sizeof(self->currentAssetName), "firmware.bin", 12);
        self->currentAssetUrl[0] = '\0';
        self->currentAssetSha256[0] = '\0';
        self->currentAssetSize = 0;
        self->currentAssetSha256Valid = false;
      } else {
        self->depth++;
      }
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSETS_ARRAY:
      self->position = Position::IN_ASSET_OBJECT;
      self->assetDepth = 1;
      self->currentAssetName[0] = '\0';
      self->currentAssetUrl[0] = '\0';
      self->currentAssetSha256[0] = '\0';
      self->currentAssetSize = 0;
      self->currentAssetSha256Valid = false;
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth++;
      self->lastKey = LastKey::NONE;
      break;
  }
}

void ReleaseJsonParser::sOnObjectEnd(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth--;
      if (self->assetDepth == 0) {
        self->commitAsset();
        self->position = self->directFirmwareObject ? Position::TOP_LEVEL : Position::IN_ASSETS_ARRAY;
        self->directFirmwareObject = false;
      }
      self->lastKey = LastKey::NONE;
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnArrayStart(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->lastKey == LastKey::ASSETS && self->depth == 1) {
        self->position = Position::IN_ASSETS_ARRAY;
      } else {
        self->depth++;
      }
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth++;
      self->lastKey = LastKey::NONE;
      break;
    default:
      break;
  }
}

void ReleaseJsonParser::sOnArrayEnd(void* ctx) {
  auto* self = static_cast<ReleaseJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_ASSETS_ARRAY:
      self->position = Position::TOP_LEVEL;
      break;
    case Position::IN_ASSET_OBJECT:
      self->assetDepth--;
      self->lastKey = LastKey::NONE;
      break;
  }
}
