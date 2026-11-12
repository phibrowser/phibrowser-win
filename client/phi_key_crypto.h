// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_KEY_CRYPTO_H_
#define PHIBROWSER_WIN_CLIENT_PHI_KEY_CRYPTO_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The sync key layer's primitives (docs/user-data-parity.md S3) — the Mac's
// PhiKeyCrypto + RecoveryCode, byte for byte, over BoringSSL (the framework
// kit already links it into Phi.exe). Stateless: nothing here persists a
// key. Host-clean (STL + BoringSSL) so test/model can run it.
//
// Wire formats (every envelope starts with the version byte 0x01):
//   device envelope   [01][HPKE enc:32][ChaCha20-Poly1305 ciphertext+tag]
//                     HPKE base mode, DHKEM(X25519, HKDF-SHA256) /
//                     HKDF-SHA256 / ChaCha20-Poly1305, info
//                     "phibrowser-sync-hpke-v1", empty AAD — CryptoKit's
//                     HPKE.Ciphersuite.Curve25519_SHA256_ChachaPoly.
//   symmetric envelope [01][nonce:12][AES-256-GCM ciphertext][tag:16]
//                     (CryptoKit's AES.GCM "combined"), for the recovery
//                     envelope and everything sealed with the ARK.
namespace phi_client::keys {

using Bytes = std::vector<uint8_t>;

inline constexpr uint8_t kEnvelopeVersion = 0x01;
inline constexpr size_t kKeyBytes = 32;

enum class CryptoError { kNone, kBadVersion, kBadFormat, kDecryptFailed };

// Cryptographically random bytes (BoringSSL's RAND_bytes).
Bytes RandomBytes(size_t count);
// The account root key: 32 random bytes.
Bytes GenerateArk();

// An X25519 key pair in CryptoKit's raw representation (32 + 32 bytes).
struct DeviceKeyPair {
  Bytes private_key;
  Bytes public_key;
};
DeviceKeyPair GenerateDeviceKeyPair();
// The public half of a raw X25519 private key; empty for a malformed one.
Bytes PublicKeyFor(const Bytes& private_key);

// Device envelopes (HPKE to a device's public key).
std::optional<Bytes> SealToPublicKey(const Bytes& plaintext,
                                     const Bytes& recipient_public_key);
std::optional<Bytes> OpenWithPrivateKey(const Bytes& envelope,
                                        const Bytes& private_key,
                                        CryptoError* error = nullptr);

// HKDF-SHA256(entropy, salt, "phibrowser-sync-recovery-v1") → 32 bytes.
Bytes DeriveRecoveryKey(const Bytes& entropy, const Bytes& salt);

// Symmetric envelopes (AES-256-GCM, random nonce).
std::optional<Bytes> SealWithSymmetric(const Bytes& plaintext,
                                       const Bytes& key);
std::optional<Bytes> OpenWithSymmetric(const Bytes& envelope,
                                       const Bytes& key,
                                       CryptoError* error = nullptr);

// The join verification code both devices show: SHA-256(public key)'s first
// 40 bits as 8 Crockford base32 characters, "XXXX-XXXX".
std::string VerificationCode(const Bytes& public_key);
// device_key_id: base64url(SHA-256(public key)[0:16]), unpadded.
std::string DeviceKeyId(const Bytes& public_key);

// Standard padded base64 (the /keys/v1 wire's binary fields; Swift's
// base64EncodedString / Data(base64Encoded:)).
std::string Base64Encode(const Bytes& data);
std::optional<Bytes> Base64Decode(std::string_view text);
// Lowercase hex (a profile key's passphrase for Chromium).
std::string HexLower(const Bytes& data);
Bytes Sha256(std::string_view data);
Bytes Sha256(const Bytes& data);
// SHA-1, for Chromium sync's client tag hash only (not a security use).
Bytes Sha1(std::string_view data);

// --- Recovery code (the Mac's RecoveryCode) --------------------------------
// 16 random bytes → 26 Crockford base32 characters + 1 checksum character
// (SHA-256(entropy)[0] & 31), shown in groups of five.
inline constexpr size_t kRecoveryEntropyBytes = 16;
struct RecoveryCode {
  std::string display;  // "XXXXX-XXXXX-XXXXX-XXXXX-XXXXX-XX"
  Bytes entropy;
};
RecoveryCode GenerateRecoveryCode();
// The display form of `entropy` (deterministic; GenerateRecoveryCode's).
std::string EncodeRecoveryCode(const Bytes& entropy);
// Tolerant parse: case, '-' and ' ' ignored, O→0, I/L→1; nullopt for a wrong
// length, a character outside the alphabet or a checksum mismatch.
std::optional<Bytes> DecodeRecoveryCode(std::string_view input);

namespace internal {
// The HPKE core behind SealToPublicKey, with the RFC 9180 knobs a
// known-answer test needs: `aad`, and `seed_for_testing` (the ephemeral
// private key, RFC 9180's skEm) when non-empty. Returns enc ‖ ciphertext.
std::optional<Bytes> HpkeSeal(const Bytes& plaintext,
                              const Bytes& aad,
                              const Bytes& recipient_public_key,
                              std::string_view info,
                              const Bytes& seed_for_testing);
std::optional<Bytes> HpkeOpen(const Bytes& enc,
                              const Bytes& ciphertext,
                              const Bytes& aad,
                              const Bytes& private_key,
                              std::string_view info);
}  // namespace internal

}  // namespace phi_client::keys

#endif  // PHIBROWSER_WIN_CLIENT_PHI_KEY_CRYPTO_H_
