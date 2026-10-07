#pragma once
#ifdef USE_LIBRETINY

#include "esphome/core/defines.h"

#include <cstddef>
#include <cstdint>

// RTC-like preference storage is compiled in only when a config option selects it
// (USE_LIBRETINY_RTC_PREFERENCES) and the family has it: on the BK7231T, spare words of the JPEG
// quantization table keep their content across soft resets but not power loss.
#if defined(USE_LIBRETINY_RTC_PREFERENCES) && defined(USE_LIBRETINY_VARIANT_BK7231T)
#define USE_LIBRETINY_RTC_PREFERENCES_STORAGE
#endif

// Forward declare FlashDB types to avoid pulling in flashdb.h
// NOLINTBEGIN(readability-identifier-naming)
struct fdb_kvdb;
struct fdb_blob;
// NOLINTEND(readability-identifier-naming)

namespace esphome::libretiny {

class LibreTinyPreferenceBackend final {
 public:
  bool save(const uint8_t *data, size_t len);
  bool load(uint8_t *data, size_t len);

  uint32_t key;
  struct fdb_kvdb *db;
  struct fdb_blob *blob;
#ifdef USE_LIBRETINY_RTC_PREFERENCES_STORAGE
  uint8_t rtc_offset{0};    // RTC path: word offset into the storage region
  uint8_t length_words{0};  // RTC path: data length in 32-bit words
  bool in_flash{true};      // false: store in the RTC-like region instead of FlashDB
#endif
};

class LibreTinyPreferences;
LibreTinyPreferences *get_preferences();

}  // namespace esphome::libretiny

namespace esphome {
using PreferenceBackend = libretiny::LibreTinyPreferenceBackend;
}  // namespace esphome

#endif  // USE_LIBRETINY
