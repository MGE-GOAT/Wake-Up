#include "media_crypto.hpp"

#include <algorithm>
#include <cstring>

#if HAVE_SODIUM
#include <sodium.h>
#endif

namespace mc {

namespace {

#if HAVE_SODIUM
// HKDF-SHA256 (RFC 5869). libsodium added native crypto_kdf_hkdf_sha256_*
// only in 1.0.20 — Debian Trixie ships 1.0.18, so we roll our own on top
// of crypto_auth_hmacsha256_* (which is in 1.0.18). This matches RFC 5869
// byte-for-byte with the Python + Dart sides.
bool hkdf_sha256(const uint8_t* salt, size_t salt_len,
                 const uint8_t* ikm,  size_t ikm_len,
                 const uint8_t* info, size_t info_len,
                 uint8_t* out, size_t out_len) {
    constexpr size_t kHashLen = crypto_auth_hmacsha256_BYTES;  // 32

    // Extract: PRK = HMAC-SHA256(salt, IKM). RFC says if salt is empty,
    // use zero-byte string of HashLen length — we always pass non-empty
    // salts so this branch is defensive only.
    uint8_t prk[kHashLen];
    {
        uint8_t zero_salt[kHashLen] = {0};
        crypto_auth_hmacsha256_state st;
        crypto_auth_hmacsha256_init(&st,
            salt_len ? salt    : zero_salt,
            salt_len ? salt_len : kHashLen);
        crypto_auth_hmacsha256_update(&st, ikm, ikm_len);
        crypto_auth_hmacsha256_final(&st, prk);
    }

    // Expand: T(N) = HMAC-SHA256(PRK, T(N-1) || info || N), starting T(0)=""
    uint8_t t[kHashLen];
    size_t  produced = 0;
    uint8_t counter  = 1;
    while (produced < out_len) {
        crypto_auth_hmacsha256_state st;
        crypto_auth_hmacsha256_init(&st, prk, kHashLen);
        if (counter > 1) crypto_auth_hmacsha256_update(&st, t, kHashLen);
        if (info_len)    crypto_auth_hmacsha256_update(&st, info, info_len);
        crypto_auth_hmacsha256_update(&st, &counter, 1);
        crypto_auth_hmacsha256_final(&st, t);

        const size_t take = std::min(kHashLen, out_len - produced);
        std::memcpy(out + produced, t, take);
        produced += take;
        counter  += 1;
        if (counter == 0) { sodium_memzero(prk, sizeof(prk)); return false; }  // L too big
    }
    sodium_memzero(prk, sizeof(prk));
    sodium_memzero(t,   sizeof(t));
    return true;
}

bool derive_root_key(const std::string& device_id,
                     const std::string& device_password,
                     uint8_t root[kKeyBytes]) {
    // ikm = sha256(password)
    uint8_t pw_hash[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(pw_hash,
        reinterpret_cast<const uint8_t*>(device_password.data()),
        device_password.size());
    static const char* info = "elderly-care/media-root/v1";
    bool ok = hkdf_sha256(
        reinterpret_cast<const uint8_t*>(device_id.data()), device_id.size(),
        pw_hash, sizeof(pw_hash),
        reinterpret_cast<const uint8_t*>(info), std::strlen(info),
        root, kKeyBytes);
    sodium_memzero(pw_hash, sizeof(pw_hash));
    return ok;
}

bool derive_blob_key(const uint8_t root[kKeyBytes],
                     const uint8_t salt[kSaltBytes],
                     uint8_t out[kKeyBytes]) {
    static const char* info = "elderly-care/media-blob/v1";
    return hkdf_sha256(salt, kSaltBytes, root, kKeyBytes,
                       reinterpret_cast<const uint8_t*>(info), std::strlen(info),
                       out, kKeyBytes);
}

bool ensure_sodium() {
    static bool inited = (sodium_init() >= 0);
    return inited;
}
#endif  // HAVE_SODIUM

}  // namespace

std::vector<uint8_t> encrypt_for_subscriber(
        const std::string& device_id,
        const std::string& device_password,
        const std::vector<uint8_t>& plaintext,
        std::string* err) {
#if !HAVE_SODIUM
    if (err) *err = "libsodium not compiled in";
    return {};
#else
    if (!ensure_sodium()) { if (err) *err = "sodium_init failed"; return {}; }

    uint8_t root[kKeyBytes];
    if (!derive_root_key(device_id, device_password, root)) {
        if (err) *err = "root key derivation failed"; return {};
    }

    std::vector<uint8_t> out(kSaltBytes + kNonceBytes
                             + plaintext.size() + kTagBytes);
    uint8_t* salt  = out.data();
    uint8_t* nonce = salt + kSaltBytes;
    uint8_t* ct    = nonce + kNonceBytes;

    randombytes_buf(salt,  kSaltBytes);
    randombytes_buf(nonce, kNonceBytes);

    uint8_t blob_key[kKeyBytes];
    if (!derive_blob_key(root, salt, blob_key)) {
        sodium_memzero(root, sizeof(root));
        if (err) *err = "blob key derivation failed"; return {};
    }

    unsigned long long ct_len = 0;
    int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        ct, &ct_len,
        plaintext.data(), plaintext.size(),
        nullptr, 0,         // no AAD
        nullptr,
        nonce, blob_key);

    sodium_memzero(root,     sizeof(root));
    sodium_memzero(blob_key, sizeof(blob_key));

    if (rc != 0) { if (err) *err = "AEAD encrypt failed"; return {}; }
    out.resize(kSaltBytes + kNonceBytes + ct_len);
    return out;
#endif
}

std::vector<uint8_t> decrypt_from_subscriber(
        const std::string& device_id,
        const std::string& device_password,
        const std::vector<uint8_t>& wire,
        std::string* err) {
#if !HAVE_SODIUM
    if (err) *err = "libsodium not compiled in";
    return {};
#else
    if (!ensure_sodium()) { if (err) *err = "sodium_init failed"; return {}; }
    if (wire.size() < kSaltBytes + kNonceBytes + kTagBytes) {
        if (err) *err = "blob too short"; return {};
    }
    const uint8_t* salt  = wire.data();
    const uint8_t* nonce = salt + kSaltBytes;
    const uint8_t* ct    = nonce + kNonceBytes;
    const size_t   ct_len= wire.size() - kSaltBytes - kNonceBytes;

    uint8_t root[kKeyBytes], blob_key[kKeyBytes];
    if (!derive_root_key(device_id, device_password, root)
        || !derive_blob_key(root, salt, blob_key)) {
        sodium_memzero(root, sizeof(root));
        if (err) *err = "key derivation failed"; return {};
    }

    std::vector<uint8_t> pt(ct_len - kTagBytes);
    unsigned long long pt_len = 0;
    int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        pt.data(), &pt_len, nullptr,
        ct, ct_len, nullptr, 0,
        nonce, blob_key);

    sodium_memzero(root,     sizeof(root));
    sodium_memzero(blob_key, sizeof(blob_key));

    if (rc != 0) { if (err) *err = "AEAD decrypt failed (MAC mismatch?)"; return {}; }
    pt.resize(pt_len);
    return pt;
#endif
}

}  // namespace mc
