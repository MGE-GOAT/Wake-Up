// Phase 3 — End-to-end media encryption (app side).
//
// Mirrors Server/media_crypto.py and device-side media_crypto.cpp byte-for-
// byte. Encrypts outgoing images/voice before POSTing; decrypts incoming
// media before display.
//
// Scheme:
//   root_key = HKDF-SHA256(salt=device_id, ikm=sha256(device_password),
//                          info="elderly-care/media-root/v1", L=32)
//   per-blob:
//     salt  = 16 random bytes
//     nonce = 24 random bytes
//     key   = HKDF-SHA256(salt=salt, ikm=root_key,
//                         info="elderly-care/media-blob/v1", L=32)
//   wire format: salt || nonce || XChaCha20-Poly1305(key, nonce, plaintext)
//
// The device password is read from flutter_secure_storage (DeviceSecretStore).
// If unset (user hasn't joined this device's subscription yet), encrypt/decrypt
// throws and the caller surfaces an "ابتدا دستگاه را اضافه کنید" message.
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:cryptography/cryptography.dart';
import 'package:crypto/crypto.dart' as crypto;
import 'package:http/http.dart' as http;
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../configs.dart';
import 'secure_storage.dart';

const _rootInfo = 'elderly-care/media-root/v1';
const _blobInfo = 'elderly-care/media-blob/v1';

const int _kSaltBytes  = 16;
const int _kNonceBytes = 24;
const int _kKeyBytes   = 32;

final _aead = Xchacha20.poly1305Aead();
final _hkdf = Hkdf(hmac: Hmac.sha256(), outputLength: _kKeyBytes);

Future<SecretKey> _deriveRootKey(String deviceId, String devicePassword) async {
  final pwHash = crypto.sha256.convert(utf8.encode(devicePassword)).bytes;
  return _hkdf.deriveKey(
    secretKey: SecretKey(pwHash),
    nonce:     utf8.encode(deviceId), // HKDF "salt" param — pkg calls it nonce
    info:      utf8.encode(_rootInfo),
  );
}

Future<SecretKey> _deriveBlobKey(SecretKey rootKey, List<int> blobSalt) async {
  final raw = await rootKey.extractBytes();
  return _hkdf.deriveKey(
    secretKey: SecretKey(raw),
    nonce:     blobSalt,
    info:      utf8.encode(_blobInfo),
  );
}

/// Encrypt and return a single byte buffer: salt(16) || nonce(24) || ct+tag.
Future<Uint8List> encryptForDevice({
  required String deviceId,
  required Uint8List plaintext,
}) async {
  final pw = await DeviceSecretStore.read(deviceId);
  if (pw == null) {
    throw StateError('no device password — subscribe to the device first');
  }
  final rootKey = await _deriveRootKey(deviceId, pw);
  final salt    = _aead.newNonce().sublist(0, _kSaltBytes); // 16 random bytes
  // Xchacha20.newNonce returns 24 bytes — use a fresh call for the actual AEAD nonce.
  final blobKey = await _deriveBlobKey(rootKey, salt);
  final nonce   = _aead.newNonce();
  final secret  = await _aead.encrypt(
    plaintext, secretKey: blobKey, nonce: nonce,
  );
  // SecretBox = nonce + cipherText + mac. We control the wire layout
  // ourselves so the device + server stay simple — pack salt + nonce + (ct || tag).
  final out = BytesBuilder(copy: false)
    ..add(salt)
    ..add(nonce)
    ..add(secret.cipherText)
    ..add(secret.mac.bytes);
  return out.toBytes();
}

/// Decode base64 media and write it to the app documents dir, decrypting first
/// when [encrypted]. Returns the written file path, or null on failure — we do
/// NOT write a corrupt/ciphertext file when decryption fails (fail-closed), so
/// the UI shows "no media" instead of handing a broken blob to a player.
Future<String?> saveIncomingMedia({
  required String base64Data,
  required String fileName,
  required String deviceId,
  required bool encrypted,
}) async {
  Uint8List bytes;
  try {
    bytes = base64Decode(base64Data);
  } catch (_) {
    return null;
  }
  if (encrypted) {
    try {
      bytes = await decryptFromDevice(deviceId: deviceId, wire: bytes);
    } catch (e) {
      // ignore: avoid_print
      print('[crypto] decrypt failed for $fileName: $e');
      return null; // fail closed — don't persist ciphertext as if it were media
    }
  }
  final dir = await getApplicationDocumentsDirectory();
  final path = '${dir.path}/$fileName';
  await File(path).writeAsBytes(bytes);
  return path;
}

/// Download a fall clip on demand (called when the user taps an event), decrypt
/// it if encrypted, write a temp .mp4, and return its path — or null on any
/// failure. The clip isn't transferred until this is called, keeping the event
/// feed light.
Future<String?> downloadFallVideo({
  required String deviceId,
  required String messageId,
}) async {
  try {
    final creds = await AuthStore.creds();
    final username = creds?.username ?? '';
    final password = creds?.password ?? '';
    final r = await http.post(
      Uri.parse('$serverUrl/Fall_Video_App'),
      headers: {
        'Content-Type': 'application/json',
        'Username': username,
        'Password': password,
      },
      body: jsonEncode({'device_id': deviceId, 'message_id': messageId}),
    ).timeout(const Duration(seconds: 30));
    if (r.statusCode != 200) return null;
    final data = jsonDecode(r.body);
    final b64 = data['video_b64'] as String?;
    if (b64 == null || b64.isEmpty) return null;
    Uint8List bytes = base64Decode(b64);
    if (data['encrypted'] == true) {
      bytes = await decryptFromDevice(deviceId: deviceId, wire: bytes);
    }
    final dir = await getTemporaryDirectory();
    final path = '${dir.path}/fall_${deviceId}_$messageId.mp4';
    await File(path).writeAsBytes(bytes);
    return path;
  } catch (_) {
    return null;
  }
}

/// Reverse of [encryptForDevice]. Throws if MAC fails (tampered blob).
Future<Uint8List> decryptFromDevice({
  required String deviceId,
  required Uint8List wire,
}) async {
  if (wire.length < _kSaltBytes + _kNonceBytes + 16) {
    throw FormatException('blob too short');
  }
  final pw = await DeviceSecretStore.read(deviceId);
  if (pw == null) throw StateError('no device password');
  final rootKey = await _deriveRootKey(deviceId, pw);
  final salt    = wire.sublist(0, _kSaltBytes);
  final nonce   = wire.sublist(_kSaltBytes, _kSaltBytes + _kNonceBytes);
  final ctMac   = wire.sublist(_kSaltBytes + _kNonceBytes);
  final ct      = ctMac.sublist(0, ctMac.length - 16);
  final mac     = Mac(ctMac.sublist(ctMac.length - 16));
  final blobKey = await _deriveBlobKey(rootKey, salt);
  final pt = await _aead.decrypt(
    SecretBox(ct, nonce: nonce, mac: mac),
    secretKey: blobKey,
  );
  return Uint8List.fromList(pt);
}
