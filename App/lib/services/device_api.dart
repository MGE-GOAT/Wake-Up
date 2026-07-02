// HTTP wrapper for the Phase 2 subscription endpoints.
//
// Each call sends:
//   Headers: Username + Password  (user's app account, from login.dart)
//   Body:    device_id + key_hash (hex SHA-256 of the 32+ char device pw)
//
// The server's per-route handler verifies both. The server never sees the
// raw device password — only its hash.
//
// Result wrappers are simple records:
//   ApiOk            — 2xx, payload as Map
//   ApiDeviceFull    — 409 from /Subscribe_Device with the 3 current subs
//   ApiError         — any other non-2xx, with status + parsed body
import 'dart:convert';
import 'package:crypto/crypto.dart' as crypto;
import 'package:http/http.dart' as http;

import '../configs.dart';

/// KEY_HASH = hex SHA-256 of the device password. This is the media key
/// material (see media_crypto.dart) — it is kept on the app and NEVER sent to
/// the server. Matches the device's `sha256_hex(password)` in C++.
String deviceKeyHash(String password) {
  return crypto.sha256.convert(utf8.encode(password)).toString();
}

/// AUTH = SHA-256(KEY_HASH) — the token the app sends to the server to prove
/// knowledge of the device password. Because it is a one-way hash of the media
/// key material, the server (which stores/receives it) can never derive the
/// media key. Matches the device's `sha256_hex(sha256_hex(password))` and the
/// server's stored device credential.
String deviceAuthToken(String password) {
  return crypto.sha256.convert(utf8.encode(deviceKeyHash(password))).toString();
}

sealed class ApiResult {
  const ApiResult();
}

class ApiOk extends ApiResult {
  final Map<String, dynamic> body;
  const ApiOk(this.body);
}

/// Returned by `subscribeDevice()` when the device already has 3 subscribers.
/// `subscribers` is the list of {username, joined_at} maps so the caller can
/// render the kick-someone screen.
class ApiDeviceFull extends ApiResult {
  final List<Map<String, dynamic>> subscribers;
  const ApiDeviceFull(this.subscribers);
}

class ApiError extends ApiResult {
  final int status;
  final String error;
  final Map<String, dynamic> body;
  const ApiError(this.status, this.error, this.body);
}

class DeviceApi {
  final String username;
  final String password; // user's app account password (NOT the device pw)

  DeviceApi({required this.username, required this.password});

  Map<String, String> get _headers => {
        'Username': username,
        'Password': password,
        'Content-Type': 'application/json',
      };

  Future<ApiResult> _post(String path, Map<String, dynamic> body) async {
    try {
      final r = await http
          .post(Uri.parse('$serverUrl$path'),
              headers: _headers, body: jsonEncode(body))
          .timeout(const Duration(seconds: 10));
      return _wrap(r);
    } catch (e) {
      return ApiError(0, 'network: $e', const {});
    }
  }

  Future<ApiResult> _get(String path, Map<String, String> query) async {
    try {
      final uri = Uri.parse('$serverUrl$path')
          .replace(queryParameters: query);
      final r = await http.get(uri, headers: _headers)
          .timeout(const Duration(seconds: 10));
      return _wrap(r);
    } catch (e) {
      return ApiError(0, 'network: $e', const {});
    }
  }

  ApiResult _wrap(http.Response r) {
    Map<String, dynamic> body;
    try {
      body = jsonDecode(r.body) as Map<String, dynamic>;
    } catch (_) {
      body = {'raw': r.body};
    }
    if (r.statusCode >= 200 && r.statusCode < 300) return ApiOk(body);
    if (r.statusCode == 409 && body['error'] == 'device_full') {
      final subs = (body['subscribers'] as List? ?? [])
          .cast<Map<String, dynamic>>();
      return ApiDeviceFull(subs);
    }
    return ApiError(r.statusCode,
        body['error']?.toString() ?? 'http_${r.statusCode}', body);
  }

  // ── Endpoint wrappers ────────────────────────────────────────────────────

  /// First-time setup. Creates device row + first subscription atomically.
  /// Returns ApiOk on 201, or ApiError(409, 'device_id_taken') if the id
  /// the user picked is already used by someone else.
  Future<ApiResult> registerDevice({
    required String deviceId,
    required String devicePassword,
  }) {
    return _post(registerDeviceEndpoint, {
      'device_id': deviceId,
      'key_hash':  deviceAuthToken(devicePassword),
    });
  }

  /// Join an existing device. Returns ApiDeviceFull if the device already
  /// has 3 subscribers (caller should route to kick UX). ApiError 403
  /// 'wrong_password' if the user typed the wrong device password.
  Future<ApiResult> subscribeDevice({
    required String deviceId,
    required String devicePassword,
  }) {
    return _post(subscribeDeviceEndpoint, {
      'device_id': deviceId,
      'key_hash':  deviceAuthToken(devicePassword),
    });
  }

  /// Kick `replaceTarget` out and add the calling user in their place.
  /// Atomic on the server. ApiError 404 'target_not_subscribed' if the
  /// target list has changed since we read it.
  Future<ApiResult> replaceSubscriber({
    required String deviceId,
    required String devicePassword,
    required String replaceTarget,
  }) {
    return _post(replaceSubscriberEndpoint, {
      'device_id':       deviceId,
      'key_hash':        deviceAuthToken(devicePassword),
      'replace_target':  replaceTarget,
    });
  }

  /// Leave a device. Requires knowing the device password so an attacker
  /// who learned a victim's app login can't kick them off devices.
  Future<ApiResult> unsubscribeDevice({
    required String deviceId,
    required String devicePassword,
  }) {
    return _post(unsubscribeDeviceEndpoint, {
      'device_id': deviceId,
      'key_hash':  deviceAuthToken(devicePassword),
    });
  }

  /// List current subscribers (must be a member). Powers the subscribers
  /// management screen.
  Future<ApiResult> deviceSubscribers({required String deviceId}) {
    return _get(deviceSubscribersEndpoint, {'device_id': deviceId});
  }
}
