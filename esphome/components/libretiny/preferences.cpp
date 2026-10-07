#ifdef USE_LIBRETINY

#include "preferences.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <cstring>
#include <vector>

#ifdef USE_LIBRETINY_RTC_PREFERENCES_STORAGE
#include "esphome/core/preferences_rtc.h"
#include <array>
#endif

namespace esphome::libretiny {

static const char *const TAG = "preferences";

struct NVSData {
  uint32_t key;
  SmallInlineBuffer<8> data;  // Most prefs fit in 8 bytes (covers fan, cover, select, etc.)
};

static std::vector<NVSData> s_pending_save;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

#ifdef USE_LIBRETINY_RTC_PREFERENCES_STORAGE
// BK7231T: words 4-31 of the JPEG encoder's quantization table (JPEG_REG20_QUANT_TAB, 32 words at
// 0x0080a080). The JPEG engine is unused, and the table keeps its content across soft resets and OTA
// updates but not power loss, like RTC memory. Word 0 is the SDK's start-type register; words 1-3 are
// left alone too, since newer BDKs read them as a deep-sleep wake GPIO backup (GPIO_WAKEUP_INT_BAK_ADDR).
// These are peripheral registers, so they are only accessed as whole 32-bit words.
static constexpr uintptr_t RTC_PREF_BASE = 0x0080a090;
static constexpr size_t RTC_PREF_SIZE_WORDS = 28;

static volatile uint32_t *rtc_pref_word(size_t index) {
  return reinterpret_cast<volatile uint32_t *>(RTC_PREF_BASE) + index;  // NOLINT(performance-no-int-to-ptr)
}

static bool rtc_pref_fits(uint8_t offset, uint8_t length_words, size_t len) {
  return rtc_pref_bytes_to_words(len) == length_words &&
         static_cast<size_t>(offset) + length_words + 1 <= RTC_PREF_SIZE_WORDS;
}

static bool save_to_rtc(uint8_t offset, uint32_t key, uint8_t length_words, const uint8_t *data, size_t len) {
  if (!rtc_pref_fits(offset, length_words, len))
    return false;
  std::array<uint32_t, RTC_PREF_SIZE_WORDS> record{};
  rtc_pref_encode(record.data(), key, length_words, data, len);
  for (size_t i = 0; i <= length_words; i++)
    *rtc_pref_word(offset + i) = record[i];
  return true;
}

static bool load_from_rtc(uint8_t offset, uint32_t key, uint8_t length_words, uint8_t *data, size_t len) {
  if (!rtc_pref_fits(offset, length_words, len))
    return false;
  std::array<uint32_t, RTC_PREF_SIZE_WORDS> record{};
  for (size_t i = 0; i <= length_words; i++)
    record[i] = *rtc_pref_word(offset + i);
  return rtc_pref_decode(record.data(), key, length_words, data, len);
}
#endif  // USE_LIBRETINY_RTC_PREFERENCES_STORAGE

bool LibreTinyPreferenceBackend::save(const uint8_t *data, size_t len) {
#ifdef USE_LIBRETINY_RTC_PREFERENCES_STORAGE
  if (!this->in_flash)
    return save_to_rtc(this->rtc_offset, this->key, this->length_words, data, len);
#endif
  // try find in pending saves and update that
  for (auto &obj : s_pending_save) {
    if (obj.key == this->key) {
      obj.data.set(data, len);
      return true;
    }
  }
  NVSData save{};
  save.key = this->key;
  save.data.set(data, len);
  s_pending_save.push_back(std::move(save));
  ESP_LOGVV(TAG, "s_pending_save: key: %" PRIu32 ", len: %zu", this->key, len);
  return true;
}

bool LibreTinyPreferenceBackend::load(uint8_t *data, size_t len) {
#ifdef USE_LIBRETINY_RTC_PREFERENCES_STORAGE
  if (!this->in_flash)
    return load_from_rtc(this->rtc_offset, this->key, this->length_words, data, len);
#endif
  // try find in pending saves and load from that
  for (auto &obj : s_pending_save) {
    if (obj.key == this->key) {
      if (obj.data.size() != len) {
        // size mismatch
        return false;
      }
      memcpy(data, obj.data.data(), len);
      return true;
    }
  }

  char key_str[UINT32_MAX_STR_SIZE];
  uint32_to_str(key_str, this->key);
  fdb_blob_make(this->blob, data, len);
  size_t actual_len = fdb_kv_get_blob(this->db, key_str, this->blob);
  if (actual_len != len) {
    ESP_LOGVV(TAG, "NVS length does not match (%zu!=%zu)", actual_len, len);
    return false;
  } else {
    ESP_LOGVV(TAG, "fdb_kv_get_blob: key: %s, len: %zu", key_str, len);
  }
  return true;
}

void LibreTinyPreferences::open() {
  //
  fdb_err_t err = fdb_kvdb_init(&this->db, "esphome", "kvs", NULL, NULL);
  if (err != FDB_NO_ERR) {
    LT_E("fdb_kvdb_init(...) failed: %d", err);
  } else {
    LT_I("Preferences initialized");
  }
}

ESPPreferenceObject LibreTinyPreferences::make_preference(size_t length, uint32_t type, bool in_flash) {
#ifdef USE_LIBRETINY_RTC_PREFERENCES_STORAGE
  if (!in_flash)
    return this->make_rtc_preference_(length, type);
#endif
  // in_flash, or no RTC-like storage on this family: use FlashDB.
  return this->make_preference(length, type);
}

#ifdef USE_LIBRETINY_RTC_PREFERENCES_STORAGE
ESPPreferenceObject LibreTinyPreferences::make_rtc_preference_(size_t length, uint32_t type) {
  const size_t length_words = rtc_pref_bytes_to_words(length);
  const size_t total_words = length_words + 1;  // +1 for checksum
  if (static_cast<size_t>(this->current_rtc_offset_) + total_words > RTC_PREF_SIZE_WORDS) {
    ESP_LOGE(TAG, "RTC preference storage full, cannot allocate %" PRIu32 " words", static_cast<uint32_t>(total_words));
    return {};
  }
  auto *pref = new LibreTinyPreferenceBackend();  // NOLINT(cppcoreguidelines-owning-memory)
  pref->key = type;
  pref->in_flash = false;
  pref->rtc_offset = this->current_rtc_offset_;
  pref->length_words = static_cast<uint8_t>(length_words);
  this->current_rtc_offset_ += static_cast<uint8_t>(total_words);
  return ESPPreferenceObject(pref);
}
#endif  // USE_LIBRETINY_RTC_PREFERENCES_STORAGE

ESPPreferenceObject LibreTinyPreferences::make_preference(size_t length, uint32_t type) {
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
  return ESPPreferenceObject(new LibreTinyPreferenceBackend(this->make_backend_(type)));
}

LibreTinyPreferenceBackend LibreTinyPreferences::make_backend_(uint32_t type) {
  LibreTinyPreferenceBackend backend;
  backend.key = type;
  backend.db = &this->db;
  backend.blob = &this->blob;
  return backend;
}

bool LibreTinyPreferences::load_from_key(uint32_t type, uint8_t *data, size_t len) {
  LibreTinyPreferenceBackend backend = this->make_backend_(type);
  return backend.load(data, len);
}

bool LibreTinyPreferences::sync() {
  if (s_pending_save.empty())
    return true;

  ESP_LOGV(TAG, "Saving %zu items...", s_pending_save.size());
  int cached = 0, written = 0, failed = 0;
  fdb_err_t last_err = FDB_NO_ERR;
  uint32_t last_key = 0;

  for (const auto &save : s_pending_save) {
    char key_str[UINT32_MAX_STR_SIZE];
    uint32_to_str(key_str, save.key);
    ESP_LOGVV(TAG, "Checking if FDB data %s has changed", key_str);
    if (this->is_changed_(&this->db, save, key_str)) {
      ESP_LOGV(TAG, "sync: key: %s, len: %zu", key_str, save.data.size());
      fdb_blob_make(&this->blob, save.data.data(), save.data.size());
      fdb_err_t err = fdb_kv_set_blob(&this->db, key_str, &this->blob);
      if (err != FDB_NO_ERR) {
        ESP_LOGV(TAG, "fdb_kv_set_blob('%s', len=%zu) failed: %d", key_str, save.data.size(), err);
        failed++;
        last_err = err;
        last_key = save.key;
        continue;
      }
      written++;
    } else {
      ESP_LOGV(TAG, "FDB data not changed; skipping %" PRIu32 "  len=%zu", save.key, save.data.size());
      cached++;
    }
  }
  s_pending_save.clear();

  if (failed > 0) {
    ESP_LOGE(TAG, "Writing %d items: %d cached, %d written, %d failed. Last error=%d for key=%" PRIu32,
             cached + written + failed, cached, written, failed, last_err, last_key);
  } else if (written > 0) {
    ESP_LOGD(TAG, "Writing %d items: %d cached, %d written, %d failed", cached + written + failed, cached, written,
             failed);
  } else {
    ESP_LOGV(TAG, "Writing %d items: %d cached, %d written, %d failed", cached + written + failed, cached, written,
             failed);
  }

  return failed == 0;
}

bool LibreTinyPreferences::is_changed_(fdb_kvdb_t db, const NVSData &to_save, const char *key_str) {
  struct fdb_kv kv;
  fdb_kv_t kvp = fdb_kv_get_obj(db, key_str, &kv);
  if (kvp == nullptr) {
    ESP_LOGV(TAG, "fdb_kv_get_obj('%s'): nullptr - the key might not be set yet", key_str);
    return true;
  }

  // Check size first - if different, data has changed
  if (kv.value_len != to_save.data.size()) {
    return true;
  }

  // Most preferences are small, use stack buffer with heap fallback for large ones
  SmallBufferWithHeapFallback<256> stored_data(kv.value_len);
  fdb_blob_make(&this->blob, stored_data.get(), kv.value_len);
  size_t actual_len = fdb_kv_get_blob(db, key_str, &this->blob);
  if (actual_len != kv.value_len) {
    ESP_LOGV(TAG, "fdb_kv_get_blob('%s') len mismatch: %zu != %zu", key_str, actual_len, (size_t) kv.value_len);
    return true;
  }

  // Compare the actual data
  return memcmp(to_save.data.data(), stored_data.get(), kv.value_len) != 0;
}

bool LibreTinyPreferences::reset() {
  ESP_LOGD(TAG, "Erasing storage");
  s_pending_save.clear();
#ifdef USE_LIBRETINY_RTC_PREFERENCES_STORAGE
  // Invalidate RTC-backed preferences too (their checksums will no longer match).
  for (size_t i = 0; i < RTC_PREF_SIZE_WORDS; i++)
    *rtc_pref_word(i) = 0;
#endif

  fdb_kv_set_default(&this->db);
  fdb_kvdb_deinit(&this->db);
  return true;
}

static LibreTinyPreferences s_preferences;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

LibreTinyPreferences *get_preferences() { return &s_preferences; }

void setup_preferences() {
  s_preferences.open();
  global_preferences = &s_preferences;
}

}  // namespace esphome::libretiny

namespace esphome {
ESPPreferences *global_preferences;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
}  // namespace esphome

#endif  // USE_LIBRETINY
