#pragma once

// Move the previous installation as a unit before any settings are loaded.
// Each rename is atomic on the SD card and safe to resume after a restart.
template <typename StorageType>
bool migrateBorgesStorage(StorageType& storage) {
  if (!storage.exists("/.borges") && storage.exists("/.crosspoint") &&
      !storage.rename("/.crosspoint", "/.borges")) return false;
  if (!storage.exists("/.borges/borges.json") && storage.exists("/.borges/toto.json") &&
      !storage.rename("/.borges/toto.json", "/.borges/borges.json")) return false;
  if (!storage.exists("/.borges/borges") && storage.exists("/.borges/toto") &&
      !storage.rename("/.borges/toto", "/.borges/borges")) return false;
  return true;
}
