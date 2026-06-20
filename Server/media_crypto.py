"""Phase 3 — End-to-end media encryption (server side).

The server is a relay, not a content store. Device → server → app and
app → server → device media (images, voice messages, call audio frames)
are encrypted with a key the server never sees. The server only stores
ciphertext + 24-byte nonce + per-blob salt; only the device and its
authorized subscribers (who share the device password) can decrypt.

Scheme:
- Per-device "media root key" = HKDF-SHA256(salt=device_id, ikm=device_password_sha256,
  info=b"elderly-care/media-root/v1", L=32)
- Per-blob key = HKDF-SHA256(salt=blob_salt_16B, ikm=root_key,
  info=b"elderly-care/media-blob/v1", L=32)
- AEAD = XChaCha20-Poly1305 (libsodium / pynacl)

This module exists only to *prove the scheme works end-to-end with shared
test vectors*. The server itself does not encrypt/decrypt user media —
it just shuttles bytes. Helpers below are used by:
- migration tests
- /Media health check (decrypts a known fixture if the device password is
  supplied in the Bearer header, otherwise 404s — never exposes plaintext)
"""
from __future__ import annotations

import hashlib
import hmac
import os
from dataclasses import dataclass

from nacl.bindings import (
    crypto_aead_xchacha20poly1305_ietf_decrypt,
    crypto_aead_xchacha20poly1305_ietf_encrypt,
)

# Domain-separation strings — change the version suffix if the scheme ever
# changes so old ciphertexts fail loud instead of silently mis-decrypting.
ROOT_INFO = b"elderly-care/media-root/v1"
BLOB_INFO = b"elderly-care/media-blob/v1"

NONCE_BYTES = 24  # XChaCha20-Poly1305 nonce
SALT_BYTES  = 16
KEY_BYTES   = 32


def _hkdf_sha256(salt: bytes, ikm: bytes, info: bytes, length: int = KEY_BYTES) -> bytes:
    """RFC 5869 HKDF-SHA256. Standalone — avoids pulling in cryptography just
    for this one primitive (pynacl is already a dep)."""
    # Extract
    if not salt:
        salt = b"\x00" * hashlib.sha256().digest_size
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    # Expand
    out, t, counter = b"", b"", 1
    while len(out) < length:
        t = hmac.new(prk, t + info + bytes([counter]), hashlib.sha256).digest()
        out += t
        counter += 1
    return out[:length]


def derive_root_key(device_id: str, device_password: str) -> bytes:
    """Per-device root. Computed from password (which the server only sees
    as a hash at /Register_Device time — see Phase 2). For the relay path
    the server never re-derives this; it's here for offline test vectors."""
    pw_hash = hashlib.sha256(device_password.encode("utf-8")).digest()
    return _hkdf_sha256(
        salt=device_id.encode("utf-8"),
        ikm=pw_hash,
        info=ROOT_INFO,
    )


def derive_blob_key(root_key: bytes, blob_salt: bytes) -> bytes:
    """Per-blob key. blob_salt is generated fresh per encryption + stored
    alongside the ciphertext."""
    assert len(blob_salt) == SALT_BYTES, f"salt must be {SALT_BYTES}B"
    return _hkdf_sha256(salt=blob_salt, ikm=root_key, info=BLOB_INFO)


@dataclass(frozen=True)
class EncryptedBlob:
    salt:       bytes  # 16 B
    nonce:      bytes  # 24 B
    ciphertext: bytes  # plaintext_len + 16 B tag

    def to_wire(self) -> bytes:
        """salt || nonce || ciphertext — concatenated for storage / transport."""
        return self.salt + self.nonce + self.ciphertext

    @classmethod
    def from_wire(cls, blob: bytes) -> "EncryptedBlob":
        if len(blob) < SALT_BYTES + NONCE_BYTES + 16:
            raise ValueError("blob too short")
        return cls(
            salt=blob[:SALT_BYTES],
            nonce=blob[SALT_BYTES:SALT_BYTES + NONCE_BYTES],
            ciphertext=blob[SALT_BYTES + NONCE_BYTES:],
        )


def encrypt_blob(root_key: bytes, plaintext: bytes, aad: bytes = b"") -> EncryptedBlob:
    salt  = os.urandom(SALT_BYTES)
    nonce = os.urandom(NONCE_BYTES)
    key   = derive_blob_key(root_key, salt)
    ct    = crypto_aead_xchacha20poly1305_ietf_encrypt(plaintext, aad, nonce, key)
    return EncryptedBlob(salt=salt, nonce=nonce, ciphertext=ct)


def decrypt_blob(root_key: bytes, blob: EncryptedBlob, aad: bytes = b"") -> bytes:
    key = derive_blob_key(root_key, blob.salt)
    return crypto_aead_xchacha20poly1305_ietf_decrypt(
        blob.ciphertext, aad, blob.nonce, key,
    )


# ── Test vector ────────────────────────────────────────────────────────────
# Used by both Flutter (test/media_crypto_test.dart) and the C++ device
# (tests/media_crypto_parity.cpp) to verify cross-implementation byte-for-byte.

TEST_DEVICE_ID = "test-device-001"
TEST_PASSWORD  = "correct-horse-battery-staple-1234567890ab"   # ≥32 chars
TEST_PLAINTEXT = b"hello from the device"
TEST_SALT      = bytes.fromhex("00112233445566778899aabbccddeeff")
TEST_NONCE     = bytes.fromhex("0102030405060708090a0b0c0d0e0f10" "1112131415161718")

# Reference outputs — verified once with pynacl 1.6.2. Dart `cryptography`
# pkg + C++ libsodium MUST reproduce these byte-for-byte. If any cell drifts,
# the implementations disagree and ship is blocked.
EXPECTED_ROOT_KEY  = bytes.fromhex(
    "a403bb82cbcf3d249b9118f507867eda020b9d2150b830c47e21f0e413792069")
EXPECTED_BLOB_KEY  = bytes.fromhex(
    "daeb61cd9e8f7d4a5b2f597ed0eff6f158a44ff33a0eb72b460905fe1a9a8965")
EXPECTED_CIPHERTEXT = bytes.fromhex(
    "94c079f22a0348282ad107c2ecf9e3a250889fe6741d64de40807686c17651c4c00dd4ef1b")


def _selftest():
    root = derive_root_key(TEST_DEVICE_ID, TEST_PASSWORD)
    key  = derive_blob_key(root, TEST_SALT)
    ct   = crypto_aead_xchacha20poly1305_ietf_encrypt(
        TEST_PLAINTEXT, b"", TEST_NONCE, key)
    pt   = crypto_aead_xchacha20poly1305_ietf_decrypt(ct, b"", TEST_NONCE, key)
    assert pt == TEST_PLAINTEXT
    print("root_key       :", root.hex())
    print("blob_key       :", key.hex())
    print("ciphertext     :", ct.hex())
    print("ciphertext_len :", len(ct))


if __name__ == "__main__":
    _selftest()
