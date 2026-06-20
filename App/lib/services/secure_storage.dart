// Per-device password storage (the raw 32+ char shared secret between this
// app instance and the device). Lives in iOS Keychain / Android Keystore
// via flutter_secure_storage, never in SQLite or shared_preferences.
//
// Keyed by device_id so one app can store passwords for multiple devices.
// The server only ever sees sha256(password); the raw value here is what
// E2E media encryption (Phase 3) will derive its symmetric key from.
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// Caregiver ACCOUNT credentials. The password lives in iOS Keychain /
/// Android Keystore — never SharedPreferences (where it sat in plaintext
/// until the 2026-06 hardening pass). The username is a non-secret email
/// (shown in the UI) and stays in SharedPreferences; `creds()` is the
/// one-stop call every API site uses.
class AuthStore {
  static const _opts = AndroidOptions(encryptedSharedPreferences: true);
  static const _storage = FlutterSecureStorage(aOptions: _opts);
  static const _kPw = 'account_password';

  static Future<void> savePassword(String password) =>
      _storage.write(key: _kPw, value: password);

  static Future<String?> password() => _storage.read(key: _kPw);

  /// (username, password) or null when not logged in.
  static Future<({String username, String password})?> creds() async {
    final prefs = await SharedPreferences.getInstance();
    final u = prefs.getString('username');
    final p = await password();
    if (u == null || p == null) return null;
    return (username: u, password: p);
  }

  static Future<void> clear() => _storage.delete(key: _kPw);

  /// One-time migration: lift a legacy plaintext password out of
  /// SharedPreferences into secure storage, then delete the plaintext copy.
  /// Existing logins survive (isLoggedIn is a separate bool). Safe to call
  /// every launch — no-op once migrated.
  static Future<void> migrateFromPrefs() async {
    final prefs = await SharedPreferences.getInstance();
    final legacy = prefs.getString('password');
    if (legacy != null) {
      await savePassword(legacy);
      await prefs.remove('password');
    }
  }
}

class DeviceSecretStore {
  static const _opts = AndroidOptions(encryptedSharedPreferences: true);
  static const _storage = FlutterSecureStorage(aOptions: _opts);

  static String _key(String deviceId) => 'device_pw::$deviceId';

  /// Persist the raw password for `deviceId`. Call after a successful
  /// /Register_Device or /Subscribe_Device round-trip.
  static Future<void> save(String deviceId, String password) {
    return _storage.write(key: _key(deviceId), value: password);
  }

  /// Returns null if the user never subscribed to this device on this device.
  static Future<String?> read(String deviceId) {
    return _storage.read(key: _key(deviceId));
  }

  /// Drop the password (called from /Unsubscribe_Device handler or after a
  /// successful kick where this app was the kicked party).
  static Future<void> remove(String deviceId) {
    return _storage.delete(key: _key(deviceId));
  }
}
