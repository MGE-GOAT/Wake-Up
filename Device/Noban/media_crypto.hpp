// Phase 3 — E2E media encryption (device side).
//
// Mirrors Server/media_crypto.py and App/lib/services/media_crypto.dart
// byte-for-byte. The PC server is a relay only — it never holds the key.
//
// Used by:
//   - main.cpp before POSTing to /Message_Device (fall images, voice msgs)
//   - main.cpp after pulling images from /Media/<file> for the AI vision
//     pipeline (Phase 3.1, future)
//
// Built against libsodium (apt: libsodium-dev). The full setup.sh adds it
// to the apt list. If libsodium is unavailable at build time, mc::HAVE_SODIUM
// is 0 and the helpers return errors — main.cpp falls back to plaintext
// transport with a one-time warning log.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mc {

#if __has_include(<sodium.h>)
#define HAVE_SODIUM 1
#else
#define HAVE_SODIUM 0
#endif

constexpr size_t kSaltBytes  = 16;
constexpr size_t kNonceBytes = 24;   // XChaCha20-Poly1305
constexpr size_t kKeyBytes   = 32;
constexpr size_t kTagBytes   = 16;

// Wire format: salt(16) || nonce(24) || ciphertext (plaintext_len + 16)
//
// device_id + device_password come from id.txt loaded at startup.
// Returns empty vector + sets *err on failure.
std::vector<uint8_t> encrypt_for_subscriber(
    const std::string& device_id,
    const std::string& device_password,
    const std::vector<uint8_t>& plaintext,
    std::string* err = nullptr);

std::vector<uint8_t> decrypt_from_subscriber(
    const std::string& device_id,
    const std::string& device_password,
    const std::vector<uint8_t>& wire,
    std::string* err = nullptr);

}  // namespace mc
