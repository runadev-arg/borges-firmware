#pragma once
template <typename T>
class PersistableStore {
 public:
  static T& getInstance() {
    static T value;
    return value;
  }
};
