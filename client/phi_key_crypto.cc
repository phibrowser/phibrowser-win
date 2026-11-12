// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_key_crypto.h"

#include <array>

#include "third_party/boringssl/src/include/openssl/aead.h"
#include "third_party/boringssl/src/include/openssl/base64.h"
#include "third_party/boringssl/src/include/openssl/curve25519.h"
#include "third_party/boringssl/src/include/openssl/digest.h"
#include "third_party/boringssl/src/include/openssl/hkdf.h"
#include "third_party/boringssl/src/include/openssl/hpke.h"
#include "third_party/boringssl/src/include/openssl/rand.h"
#include "third_party/boringssl/src/include/openssl/sha.h"
#include "third_party/boringssl/src/include/openssl/sha2.h"

namespace phi_client::keys {

namespace {

constexpr std::string_view kHpkeInfo = "phibrowser-sync-hpke-v1";
constexpr std::string_view kRecoveryInfo = "phibrowser-sync-recovery-v1";
constexpr size_t kEncapBytes = 32;  // X25519's encapsulated key
constexpr size_t kGcmNonceBytes = 12;
constexpr size_t kGcmTagBytes = 16;
// Crockford base32: no I, L, O, U.
constexpr std::string_view kCrockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr size_t kRecoveryDataChars = 26;  // ceil(128 / 5)

const uint8_t* Ptr(std::string_view text) {
  return reinterpret_cast<const uint8_t*>(text.data());
}

void SetError(CryptoError* error, CryptoError value) {
  if (error) {
    *error = value;
  }
}

std::array<uint8_t, SHA256_DIGEST_LENGTH> Sha256Of(const Bytes& data) {
  std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{};
  SHA256(data.data(), data.size(), digest.data());
  return digest;
}

}  // namespace

Bytes RandomBytes(size_t count) {
  Bytes out(count);
  RAND_bytes(out.data(), out.size());
  return out;
}

Bytes GenerateArk() {
  return RandomBytes(kKeyBytes);
}

DeviceKeyPair GenerateDeviceKeyPair() {
  DeviceKeyPair pair;
  pair.private_key = RandomBytes(kKeyBytes);
  pair.public_key = PublicKeyFor(pair.private_key);
  return pair;
}

Bytes PublicKeyFor(const Bytes& private_key) {
  if (private_key.size() != kKeyBytes) {
    return Bytes();
  }
  Bytes public_key(kKeyBytes);
  X25519_public_from_private(public_key.data(), private_key.data());
  return public_key;
}

namespace internal {

std::optional<Bytes> HpkeSeal(const Bytes& plaintext,
                              const Bytes& aad,
                              const Bytes& recipient_public_key,
                              std::string_view info,
                              const Bytes& seed_for_testing) {
  if (recipient_public_key.size() != kKeyBytes) {
    return std::nullopt;
  }
  bssl::ScopedEVP_HPKE_CTX ctx;
  Bytes enc(EVP_HPKE_MAX_ENC_LENGTH);
  size_t enc_len = 0;
  const int ok =
      seed_for_testing.empty()
          ? EVP_HPKE_CTX_setup_sender(
                ctx.get(), enc.data(), &enc_len, enc.size(),
                EVP_hpke_x25519_hkdf_sha256(), EVP_hpke_hkdf_sha256(),
                EVP_hpke_chacha20_poly1305(), recipient_public_key.data(),
                recipient_public_key.size(), Ptr(info), info.size())
          : EVP_HPKE_CTX_setup_sender_with_seed_for_testing(
                ctx.get(), enc.data(), &enc_len, enc.size(),
                EVP_hpke_x25519_hkdf_sha256(), EVP_hpke_hkdf_sha256(),
                EVP_hpke_chacha20_poly1305(), recipient_public_key.data(),
                recipient_public_key.size(), Ptr(info), info.size(),
                seed_for_testing.data(), seed_for_testing.size());
  if (!ok || enc_len != kEncapBytes) {
    return std::nullopt;
  }
  enc.resize(enc_len);
  Bytes sealed(plaintext.size() + EVP_HPKE_CTX_max_overhead(ctx.get()));
  size_t sealed_len = 0;
  if (!EVP_HPKE_CTX_seal(ctx.get(), sealed.data(), &sealed_len, sealed.size(),
                         plaintext.data(), plaintext.size(), aad.data(),
                         aad.size())) {
    return std::nullopt;
  }
  sealed.resize(sealed_len);
  Bytes out = std::move(enc);
  out.insert(out.end(), sealed.begin(), sealed.end());
  return out;
}

std::optional<Bytes> HpkeOpen(const Bytes& enc,
                              const Bytes& ciphertext,
                              const Bytes& aad,
                              const Bytes& private_key,
                              std::string_view info) {
  if (private_key.size() != kKeyBytes || enc.size() != kEncapBytes) {
    return std::nullopt;
  }
  bssl::ScopedEVP_HPKE_KEY key;
  if (!EVP_HPKE_KEY_init(key.get(), EVP_hpke_x25519_hkdf_sha256(),
                         private_key.data(), private_key.size())) {
    return std::nullopt;
  }
  bssl::ScopedEVP_HPKE_CTX ctx;
  if (!EVP_HPKE_CTX_setup_recipient(
          ctx.get(), key.get(), EVP_hpke_hkdf_sha256(),
          EVP_hpke_chacha20_poly1305(), enc.data(), enc.size(), Ptr(info),
          info.size())) {
    return std::nullopt;
  }
  Bytes plain(ciphertext.size());
  size_t plain_len = 0;
  if (!EVP_HPKE_CTX_open(ctx.get(), plain.data(), &plain_len, plain.size(),
                         ciphertext.data(), ciphertext.size(), aad.data(),
                         aad.size())) {
    return std::nullopt;
  }
  plain.resize(plain_len);
  return plain;
}

}  // namespace internal

std::optional<Bytes> SealToPublicKey(const Bytes& plaintext,
                                     const Bytes& recipient_public_key) {
  std::optional<Bytes> sealed = internal::HpkeSeal(
      plaintext, Bytes(), recipient_public_key, kHpkeInfo, Bytes());
  if (!sealed) {
    return std::nullopt;
  }
  Bytes out{kEnvelopeVersion};
  out.insert(out.end(), sealed->begin(), sealed->end());
  return out;
}

std::optional<Bytes> OpenWithPrivateKey(const Bytes& envelope,
                                        const Bytes& private_key,
                                        CryptoError* error) {
  SetError(error, CryptoError::kNone);
  if (envelope.size() <= 1 + kEncapBytes) {
    SetError(error, CryptoError::kBadFormat);
    return std::nullopt;
  }
  if (envelope[0] != kEnvelopeVersion) {
    SetError(error, CryptoError::kBadVersion);
    return std::nullopt;
  }
  const Bytes enc(envelope.begin() + 1, envelope.begin() + 1 + kEncapBytes);
  const Bytes ciphertext(envelope.begin() + 1 + kEncapBytes, envelope.end());
  std::optional<Bytes> plain =
      internal::HpkeOpen(enc, ciphertext, Bytes(), private_key, kHpkeInfo);
  if (!plain) {
    SetError(error, CryptoError::kDecryptFailed);
  }
  return plain;
}

Bytes DeriveRecoveryKey(const Bytes& entropy, const Bytes& salt) {
  Bytes key(kKeyBytes);
  HKDF(key.data(), key.size(), EVP_sha256(), entropy.data(), entropy.size(),
       salt.data(), salt.size(), Ptr(kRecoveryInfo), kRecoveryInfo.size());
  return key;
}

std::optional<Bytes> SealWithSymmetric(const Bytes& plaintext,
                                       const Bytes& key) {
  if (key.size() != kKeyBytes) {
    return std::nullopt;
  }
  bssl::ScopedEVP_AEAD_CTX ctx;
  if (!EVP_AEAD_CTX_init(ctx.get(), EVP_aead_aes_256_gcm(), key.data(),
                         key.size(), kGcmTagBytes, nullptr)) {
    return std::nullopt;
  }
  const Bytes nonce = RandomBytes(kGcmNonceBytes);
  Bytes sealed(plaintext.size() + kGcmTagBytes);
  size_t sealed_len = 0;
  if (!EVP_AEAD_CTX_seal(ctx.get(), sealed.data(), &sealed_len, sealed.size(),
                         nonce.data(), nonce.size(), plaintext.data(),
                         plaintext.size(), nullptr, 0)) {
    return std::nullopt;
  }
  sealed.resize(sealed_len);
  Bytes out{kEnvelopeVersion};
  out.insert(out.end(), nonce.begin(), nonce.end());
  out.insert(out.end(), sealed.begin(), sealed.end());
  return out;
}

std::optional<Bytes> OpenWithSymmetric(const Bytes& envelope,
                                       const Bytes& key,
                                       CryptoError* error) {
  SetError(error, CryptoError::kNone);
  if (envelope.size() <= 1) {
    SetError(error, CryptoError::kBadFormat);
    return std::nullopt;
  }
  if (envelope[0] != kEnvelopeVersion) {
    SetError(error, CryptoError::kBadVersion);
    return std::nullopt;
  }
  // CryptoKit's SealedBox(combined:) refuses anything shorter than a nonce
  // and a tag; that surfaces as a decrypt failure on the Mac too.
  if (key.size() != kKeyBytes ||
      envelope.size() < 1 + kGcmNonceBytes + kGcmTagBytes) {
    SetError(error, CryptoError::kDecryptFailed);
    return std::nullopt;
  }
  const Bytes nonce(envelope.begin() + 1, envelope.begin() + 1 + kGcmNonceBytes);
  const Bytes sealed(envelope.begin() + 1 + kGcmNonceBytes, envelope.end());
  bssl::ScopedEVP_AEAD_CTX ctx;
  Bytes plain(sealed.size());
  size_t plain_len = 0;
  if (!EVP_AEAD_CTX_init(ctx.get(), EVP_aead_aes_256_gcm(), key.data(),
                         key.size(), kGcmTagBytes, nullptr) ||
      !EVP_AEAD_CTX_open(ctx.get(), plain.data(), &plain_len, plain.size(),
                         nonce.data(), nonce.size(), sealed.data(),
                         sealed.size(), nullptr, 0)) {
    SetError(error, CryptoError::kDecryptFailed);
    return std::nullopt;
  }
  plain.resize(plain_len);
  return plain;
}

std::string VerificationCode(const Bytes& public_key) {
  const auto digest = Sha256Of(public_key);
  uint64_t value = 0;
  for (size_t i = 0; i < 5; ++i) {  // 40 bits → 8 symbols
    value = (value << 8) | digest[i];
  }
  std::string chars(8, '0');
  for (size_t i = 8; i-- > 0;) {
    chars[i] = kCrockford[value & 0x1F];
    value >>= 5;
  }
  return chars.substr(0, 4) + "-" + chars.substr(4);
}

std::string DeviceKeyId(const Bytes& public_key) {
  const auto digest = Sha256Of(public_key);
  std::string id = Base64Encode(Bytes(digest.begin(), digest.begin() + 16));
  for (char& c : id) {
    if (c == '+') {
      c = '-';
    } else if (c == '/') {
      c = '_';
    }
  }
  while (!id.empty() && id.back() == '=') {
    id.pop_back();
  }
  return id;
}

std::string Base64Encode(const Bytes& data) {
  size_t length = 0;
  if (!EVP_EncodedLength(&length, data.size())) {
    return std::string();
  }
  Bytes out(length);
  const size_t written = EVP_EncodeBlock(out.data(), data.data(), data.size());
  return std::string(out.begin(), out.begin() + written);
}

std::optional<Bytes> Base64Decode(std::string_view text) {
  size_t max_length = 0;
  if (!EVP_DecodedLength(&max_length, text.size())) {
    return std::nullopt;
  }
  Bytes out(max_length);
  size_t length = 0;
  if (!EVP_DecodeBase64(out.data(), &length, out.size(), Ptr(text),
                        text.size())) {
    return std::nullopt;
  }
  out.resize(length);
  return out;
}

std::string HexLower(const Bytes& data) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (const uint8_t byte : data) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

Bytes Sha256(std::string_view data) {
  Bytes digest(SHA256_DIGEST_LENGTH);
  SHA256(Ptr(data), data.size(), digest.data());
  return digest;
}

Bytes Sha1(std::string_view data) {
  Bytes digest(SHA_DIGEST_LENGTH);
  SHA1(Ptr(data), data.size(), digest.data());
  return digest;
}

Bytes Sha256(const Bytes& data) {
  const auto digest = Sha256Of(data);
  return Bytes(digest.begin(), digest.end());
}

// --- Recovery code ---------------------------------------------------------

namespace {

char ChecksumChar(const Bytes& entropy) {
  return kCrockford[Sha256Of(entropy)[0] & 31];
}

// The Mac's encodeBase32: MSB-first 5-bit groups, the tail zero-padded,
// cut or padded with '0' to `count` characters.
std::string EncodeBase32(const Bytes& data, size_t count) {
  int bits = 0;
  uint64_t value = 0;
  std::string out;
  for (const uint8_t byte : data) {
    value = (value << 8) | byte;
    bits += 8;
    while (bits >= 5) {
      out.push_back(kCrockford[(value >> (bits - 5)) & 31]);
      bits -= 5;
    }
  }
  if (bits > 0) {
    out.push_back(kCrockford[(value << (5 - bits)) & 31]);
  }
  out.resize(count, '0');
  return out;
}

std::optional<Bytes> DecodeBase32(std::string_view text) {
  int bits = 0;
  uint64_t value = 0;
  Bytes out;
  for (const char c : text) {
    const size_t index = kCrockford.find(c);
    if (index == std::string_view::npos) {
      return std::nullopt;
    }
    value = (value << 5) | index;
    bits += 5;
    if (bits >= 8) {
      out.push_back(static_cast<uint8_t>((value >> (bits - 8)) & 0xFF));
      bits -= 8;
    }
  }
  if (out.size() < kRecoveryEntropyBytes) {
    return std::nullopt;
  }
  out.resize(kRecoveryEntropyBytes);
  return out;
}

}  // namespace

std::string EncodeRecoveryCode(const Bytes& entropy) {
  const std::string full =
      EncodeBase32(entropy, kRecoveryDataChars) + ChecksumChar(entropy);
  std::string grouped;
  for (size_t i = 0; i < full.size(); i += 5) {
    if (!grouped.empty()) {
      grouped.push_back('-');
    }
    grouped += full.substr(i, 5);
  }
  return grouped;
}

RecoveryCode GenerateRecoveryCode() {
  RecoveryCode code;
  code.entropy = RandomBytes(kRecoveryEntropyBytes);
  code.display = EncodeRecoveryCode(code.entropy);
  return code;
}

std::optional<Bytes> DecodeRecoveryCode(std::string_view input) {
  std::string normalized;
  for (char c : input) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
    switch (c) {
      case '-':
      case ' ':
      case 'U':  // Not in the alphabet; dropping it fails the length check.
        continue;
      case 'O':
        c = '0';
        break;
      case 'I':
      case 'L':
        c = '1';
        break;
      default:
        break;
    }
    normalized.push_back(c);
  }
  if (normalized.size() != kRecoveryDataChars + 1) {
    return std::nullopt;
  }
  std::optional<Bytes> entropy =
      DecodeBase32(std::string_view(normalized).substr(0, kRecoveryDataChars));
  if (!entropy || normalized[kRecoveryDataChars] != ChecksumChar(*entropy)) {
    return std::nullopt;
  }
  return entropy;
}

}  // namespace phi_client::keys
