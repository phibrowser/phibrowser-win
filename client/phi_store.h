// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_STORE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_STORE_H_

#include <windows.h>

#include <bcrypt.h>
#include <wincrypt.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "client/phi_file_io.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace phi_client {

// The ONE persistence layer for the client's own model files
// (docs/architecture-review.md §3.4 "PhiStore<T>", lane 4 §5). One file per
// domain, one value type per file, and per type a traits specialisation
// that says how the value is spelled on disk:
//
//   template <> struct PhiStoreTraits<T> {
//     static constexpr int kSchema;             // the schema this build writes
//     static constexpr const wchar_t* kFile;    // e.g. L"PhiPreferences.json"
//     static constexpr bool kProtected;         // DPAPI + HMAC (consent)
//     static base::DictValue Encode(const T&);
//     // Fills `out` from a file of schema `schema` (<= kSchema: migrate;
//     // > kSchema: a newer build's file, read what is recognised). False
//     // when the body is not this file's shape at all.
//     static bool Decode(const base::DictValue&, int schema, T* out);
//     // One-shot import of the pre-phase-3 files this domain was split
//     // from: fills the parts each present legacy file covers, renames
//     // that file to <name>.imported (never deletes), and returns true if
//     // anything was imported. Missing legacy files are not an error.
//     static bool Import(const std::wstring& dir, T* out);
//   };
//
// Header-only: the client is one exe, and a template over four value types
// needs no out-of-line home. No function-local statics, no inline
// variables with dynamic initialisation (-Wunique-object-duplication).
template <typename T>
struct PhiStoreTraits;

namespace store_internal {

// A key file that does not decrypt is treated as absent: DPAPI is bound to
// the Windows user, so a copied profile cannot verify the old seals anyway.
inline std::optional<std::string> DpapiUnprotect(std::string_view blob) {
  DATA_BLOB in = {static_cast<DWORD>(blob.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
  DATA_BLOB out = {};
  if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    return std::nullopt;
  }
  std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
  ::LocalFree(out.pbData);
  return plain;
}

inline std::optional<std::string> DpapiProtect(std::string_view plain,
                                               const wchar_t* description) {
  DATA_BLOB in = {static_cast<DWORD>(plain.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
  DATA_BLOB out = {};
  if (!::CryptProtectData(&in, description, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    return std::nullopt;
  }
  std::string blob(reinterpret_cast<char*>(out.pbData), out.cbData);
  ::LocalFree(out.pbData);
  return blob;
}

// HMAC-SHA256 through CNG, as phi_auth.cc hashes: the client links bcrypt
// already and does not need Chromium's crypto/ for one MAC.
inline bool HmacSha256(std::string_view key,
                       std::string_view data,
                       std::string* out) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
    return false;
  }
  BCRYPT_HASH_HANDLE hash = nullptr;
  UCHAR digest[32] = {};
  bool ok = false;
  if (::BCryptCreateHash(alg, &hash, nullptr, 0,
                         reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                         static_cast<ULONG>(key.size()), 0) == 0) {
    ok = ::BCryptHashData(
             hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
             static_cast<ULONG>(data.size()), 0) == 0 &&
         ::BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    ::BCryptDestroyHash(hash);
  }
  ::BCryptCloseAlgorithmProvider(alg, 0);
  if (ok) {
    out->assign(reinterpret_cast<const char*>(digest), sizeof(digest));
  }
  return ok;
}

// Constant-time so a MAC check cannot be timed byte by byte.
inline bool MacEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

// The per-install HMAC key: 32 random bytes, DPAPI-wrapped, beside the
// file it seals. `create` makes one when there is none (or none that this
// user can open); a Load never creates, so a missing key fails closed. A key
// file that is there but locked is never replaced.
inline std::optional<std::string> LoadSealKey(const std::wstring& key_path,
                                              bool create) {
  std::string blob;
  const PhiReadStatus read = PhiReadFile(key_path, &blob);
  if (read == PhiReadStatus::kError) {
    return std::nullopt;
  }
  if (read == PhiReadStatus::kOk) {
    if (std::optional<std::string> key = DpapiUnprotect(blob)) {
      if (key->size() == 32) {
        return key;
      }
    }
    LOG(ERROR) << "[PhiStore] seal key unreadable "
               << base::WideToUTF8(key_path);
  }
  if (!create) {
    return std::nullopt;
  }
  std::string key(32, '\0');
  if (::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(key.data()),
                        static_cast<ULONG>(key.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    LOG(ERROR) << "[PhiStore] BCryptGenRandom failed";
    return std::nullopt;
  }
  std::optional<std::string> wrapped = DpapiProtect(key, L"PhiStoreKey");
  if (!wrapped || !PhiWriteFileAtomic(key_path, *wrapped)) {
    LOG(ERROR) << "[PhiStore] could not write seal key "
               << base::WideToUTF8(key_path);
    return std::nullopt;
  }
  LOG(INFO) << "[PhiStore] new seal key " << base::WideToUTF8(key_path);
  return key;
}

// The sealed envelope's field names (see PhiStore::Save).
inline constexpr char kSealedFormat[] = "dpapi+hmac-sha256";
inline constexpr char kSealedFormatKey[] = "sealed";
inline constexpr char kSealedBlobKey[] = "blob";
inline constexpr char kSealedMacKey[] = "mac";

}  // namespace store_internal

template <typename T>
class PhiStore {
 public:
  using Traits = PhiStoreTraits<T>;

  PhiStore() = default;
  PhiStore(const PhiStore&) = delete;
  PhiStore& operator=(const PhiStore&) = delete;

  // Binds the store to `<user_data_dir>\<kFile>`; nothing is read yet.
  void Init(const std::wstring& user_data_dir) {
    dir_ = user_data_dir;
    path_ = user_data_dir + L"\\" + Traits::kFile;
    // The seal key sits beside the file under the file's own stem
    // (PhiConsent.json -> PhiConsent.key).
    key_path_ = path_;
    const size_t dot = key_path_.rfind(L'.');
    if (dot != std::wstring::npos && dot > user_data_dir.size()) {
      key_path_.resize(dot);
    }
    key_path_ += L".key";
  }

  // `*out` always ends up usable:
  //   kMissing  no file — first run; Traits::Import fills what the legacy
  //             files still hold (then the result is saved so the next
  //             run is a plain kOk), else defaults.
  //   kCorrupt  the file (or its seal) did not check out and has been
  //             quarantined as <name>.corrupt-<ms>; defaults.
  //   kUnreadable  the file is there but could not be read (an antivirus
  //             or backup lock): defaults in memory, and every Save this
  //             session is refused so the user's file is never replaced.
  //   kOk       decoded. A body older than kSchema is migrated by Decode,
  //             topped up by Import (the legacy siblings it was split from
  //             may still be there) and rewritten in the current schema.
  PhiJsonFileStatus Load(T* out) {
    *out = T();
    loaded_ = base::DictValue();
    save_blocked_ = false;
    base::DictValue dict;
    int schema = 0;
    const PhiJsonFileStatus status = ReadDict(&dict, &schema);
    switch (status) {
      case PhiJsonFileStatus::kMissing:
        if (Traits::Import(dir_, out)) {
          LOG(INFO) << "[PhiStore] " << FileName() << " built from import";
          Save(*out);
        }
        return PhiJsonFileStatus::kMissing;
      case PhiJsonFileStatus::kCorrupt:
        LOG(ERROR) << "[PhiStore] " << FileName()
                   << " unreadable; using defaults";
        return PhiJsonFileStatus::kCorrupt;
      case PhiJsonFileStatus::kUnreadable:
        save_blocked_ = true;
        LOG(ERROR) << "[PhiStore] " << FileName()
                   << " could not be read; defaults this session, saves off";
        return PhiJsonFileStatus::kUnreadable;
      case PhiJsonFileStatus::kOk:
        break;
    }
    if (schema > Traits::kSchema) {
      // A newer build wrote it. Decode reads the fields it knows rather
      // than quarantining a good file; the next Save downgrades the stamp.
      LOG(WARNING) << "[PhiStore] " << FileName() << " schema " << schema
                   << " is newer than " << Traits::kSchema;
    }
    // Keys this build does not know (a newer build's, the Mac's) ride
    // along: Save writes the known ones over them.
    if constexpr (!Traits::kProtected) {
      loaded_ = dict.Clone();
    }
    if (!Traits::Decode(dict, schema, out)) {
      loaded_ = base::DictValue();
      *out = T();
      PhiQuarantineFile(path_, "body did not decode");
      LOG(ERROR) << "[PhiStore] " << FileName()
                 << " did not decode; using defaults";
      return PhiJsonFileStatus::kCorrupt;
    }
    if (schema < Traits::kSchema) {
      const bool imported = Traits::Import(dir_, out);
      LOG(INFO) << "[PhiStore] " << FileName() << " migrated " << schema
                << " -> " << Traits::kSchema
                << (imported ? " (with legacy import)" : "");
      Save(*out);
    }
    return PhiJsonFileStatus::kOk;
  }

  // Atomic (phi_file_io). A kProtected file is written as
  //   {"schema":N, "sealed":"dpapi+hmac-sha256", "blob":b64, "mac":b64}
  // where blob = DPAPI(plaintext JSON of Encode(value), schema stamped
  // inside too) and mac = HMAC-SHA256(seal key, that plaintext). DPAPI
  // hides the contents from other users; the HMAC, keyed by a file only
  // this install wrote, makes a same-user rewrite of the blob detectable
  // (lane 5 §4.6) — Load quarantines on a bad MAC rather than trusting it.
  bool Save(const T& value) {
    if (save_blocked_) {
      LOG(WARNING) << "[PhiStore] " << FileName()
                   << " not saved: it could not be read this session";
      return false;
    }
    base::DictValue dict = Traits::Encode(value);
    if constexpr (!Traits::kProtected) {
      // Top-level keys are replaced whole; unknown ones are kept.
      base::DictValue merged = loaded_.Clone();
      for (auto [key, entry] : dict) {
        merged.Set(key, entry.Clone());
      }
      loaded_ = merged.Clone();
      return PhiWriteJsonFile(path_, std::move(merged), Traits::kSchema);
    } else {
      dict.Set("schema", Traits::kSchema);
      std::string plain;
      if (!base::JSONWriter::Write(dict, &plain)) {
        LOG(ERROR) << "[PhiStore] serialise failed " << FileName();
        return false;
      }
      const std::optional<std::string> key =
          store_internal::LoadSealKey(key_path_, /*create=*/true);
      std::string mac;
      if (!key || !store_internal::HmacSha256(*key, plain, &mac)) {
        LOG(ERROR) << "[PhiStore] could not seal " << FileName();
        return false;
      }
      const std::optional<std::string> blob =
          store_internal::DpapiProtect(plain, Traits::kFile);
      if (!blob) {
        LOG(ERROR) << "[PhiStore] DPAPI failed for " << FileName()
                   << " err=" << ::GetLastError();
        return false;
      }
      base::DictValue envelope;
      envelope.Set(store_internal::kSealedFormatKey,
                   store_internal::kSealedFormat);
      envelope.Set(store_internal::kSealedBlobKey, base::Base64Encode(*blob));
      envelope.Set(store_internal::kSealedMacKey, base::Base64Encode(mac));
      return PhiWriteJsonFile(path_, std::move(envelope), Traits::kSchema);
    }
  }

  const std::wstring& path() const { return path_; }
  const std::wstring& dir() const { return dir_; }

 private:
  std::string FileName() const { return base::WideToUTF8(Traits::kFile); }

  // Reads the body dict (unsealing a kProtected file). A sealed file whose
  // envelope parses but whose seal does not is quarantined here — it is
  // corrupt in the sense that matters, whoever produced it.
  PhiJsonFileStatus ReadDict(base::DictValue* dict, int* schema) {
    base::DictValue outer;
    const PhiJsonFileStatus status = PhiReadJsonFile(path_, &outer, schema);
    if (status != PhiJsonFileStatus::kOk) {
      return status;
    }
    if constexpr (!Traits::kProtected) {
      *dict = std::move(outer);
      return PhiJsonFileStatus::kOk;
    } else {
      const char* reason = Unseal(outer, dict);
      if (reason) {
        PhiQuarantineFile(path_, reason);
        return PhiJsonFileStatus::kCorrupt;
      }
      // The stamp inside the seal is the one that counts: the outer one is
      // in the clear and could be rewritten to steer the migration.
      *schema = dict->FindInt("schema").value_or(0);
      return PhiJsonFileStatus::kOk;
    }
  }

  // Nullptr on success, else the reason to quarantine with.
  const char* Unseal(const base::DictValue& envelope, base::DictValue* body) {
    const std::string* format =
        envelope.FindString(store_internal::kSealedFormatKey);
    const std::string* blob_b64 =
        envelope.FindString(store_internal::kSealedBlobKey);
    const std::string* mac_b64 =
        envelope.FindString(store_internal::kSealedMacKey);
    if (!format || *format != store_internal::kSealedFormat || !blob_b64 ||
        !mac_b64) {
      return "not a sealed envelope";
    }
    std::string blob;
    std::string mac;
    if (!base::Base64Decode(*blob_b64, &blob) ||
        !base::Base64Decode(*mac_b64, &mac)) {
      return "sealed envelope is not base64";
    }
    const std::optional<std::string> plain =
        store_internal::DpapiUnprotect(blob);
    if (!plain) {
      return "DPAPI could not unwrap (other user, or edited)";
    }
    const std::optional<std::string> key =
        store_internal::LoadSealKey(key_path_, /*create=*/false);
    std::string expected;
    if (!key || !store_internal::HmacSha256(*key, *plain, &expected)) {
      return "no seal key for this file";
    }
    if (!store_internal::MacEquals(expected, mac)) {
      return "seal (HMAC) mismatch: file was edited";
    }
    std::optional<base::DictValue> parsed =
        base::JSONReader::ReadDict(*plain, base::JSON_PARSE_RFC);
    if (!parsed) {
      return "sealed body is not a JSON object";
    }
    *body = std::move(*parsed);
    return nullptr;
  }

  std::wstring dir_;
  std::wstring path_;
  std::wstring key_path_;
  // The file as last read or written (unsealed stores only).
  base::DictValue loaded_;
  // The last Load could not read the file: no Save may replace it.
  bool save_blocked_ = false;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_STORE_H_
