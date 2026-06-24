import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'dart:convert';
import 'dart:async';
import 'dart:io';
import 'package:sqflite/sqflite.dart';
import 'package:path/path.dart';

import 'package:shared_preferences/shared_preferences.dart';
import 'configs.dart';
import 'stream.dart';
import 'services/secure_storage.dart';

late Database database;

Future<void> initializeDatabase() async {
  final dbPath = await getDatabasesPath();
  final path = join(dbPath, 'app_database.db');

  database = await openDatabase(
    path,
    version: 7,
    onCreate: (db, version) async {
      await db.execute('''
        CREATE TABLE devices(
          device_id TEXT PRIMARY KEY,
          device_password TEXT,
          device_name TEXT,
          last_image TEXT
        )
      ''');

      await db.execute('''
        CREATE TABLE events(
          event_id INTEGER PRIMARY KEY,
          device_id TEXT,
          event_text TEXT,
          event_image TEXT,
          event_video TEXT,
          event_time TEXT,
          event_status TEXT,
          event_type TEXT,
          FOREIGN KEY(device_id) REFERENCES devices(device_id)
        )
      ''');

      await _createPillsTable(db);
    },
    onUpgrade: (db, oldVersion, newVersion) async {
      // Guards MUST run in ascending version order — later migrations depend on
      // columns/tables created by earlier ones (e.g. v7 reads device_password,
      // which v3 adds), so a multi-version jump (v1→v7) would otherwise crash.
      if (oldVersion < 2) {
        await _createPillsTable(db);
      }
      if (oldVersion < 3) {
        await db.execute('ALTER TABLE devices ADD COLUMN device_password TEXT');
      }
      if (oldVersion < 4) {
        // Pill schedule moves from "daily fixed times" to "course of treatment":
        // start_at_utc + interval_seconds + total_count + consumptions_done.
        // The old `times` JSON column + `count` are dropped. Easiest: wipe
        // and recreate (test data only).
        await db.execute('DROP TABLE IF EXISTS pills');
        await _createPillsTable(db);
      }
      if (oldVersion < 5) {
        // Pills become per-device (each device has its own course list). Add
        // device_id; old global pills had no owner, so wipe + recreate
        // (test data only).
        await db.execute('DROP TABLE IF EXISTS pills');
        await _createPillsTable(db);
      }
      if (oldVersion < 6) {
        // event_type lets us split unread counts by kind (fall / voice / call)
        // for the per-device colour-coded badges. Legacy rows (NULL) are
        // treated as 'fall'.
        await db.execute('ALTER TABLE events ADD COLUMN event_type TEXT');
      }
      if (oldVersion < 7) {
        // Security hardening: the device password must not live in plaintext
        // SQLite. Lift any legacy values into DeviceSecretStore (Keystore),
        // then scrub the column. The column itself stays (sqlite column drops
        // need a table rebuild) but is never written again.
        final rows = await db.query('devices',
            columns: ['device_id', 'device_password'],
            where: 'device_password IS NOT NULL');
        for (final r in rows) {
          final id = r['device_id'] as String?;
          final pw = r['device_password'] as String?;
          if (id != null && pw != null && pw.isNotEmpty) {
            // Don't clobber an existing secure entry (it's authoritative).
            if (await DeviceSecretStore.read(id) == null) {
              await DeviceSecretStore.save(id, pw);
            }
          }
        }
        await db.execute('UPDATE devices SET device_password = NULL');
      }
    },
  );
}

Future<void> _createPillsTable(Database db) async {
  await db.execute('''
    CREATE TABLE pills(
      pill_id           INTEGER PRIMARY KEY AUTOINCREMENT,
      device_id         TEXT NOT NULL,     -- pills are per-device (own course list)
      name              TEXT NOT NULL,
      start_at_utc      TEXT NOT NULL,    -- ISO 8601 UTC, sent to server as-is
      interval_seconds  INTEGER NOT NULL,
      total_count       INTEGER NOT NULL,
      audio_path        TEXT
    )
  ''');
}

/// Insert a pill course locally + fire-and-forget sync to every registered
/// device's server-side record.
///
/// `startAtUtc` must be ISO 'YYYY-MM-DD HH:MM:SS' in UTC (app converts from
/// the user's Jalali pick before calling). `intervalSeconds` is the gap
/// between doses (e.g. 8h = 28800). `totalCount` is the total number of
/// doses in the course.
Future<int> insertPill({
  required String deviceId,
  required String name,
  required String startAtUtc,
  required int intervalSeconds,
  required int totalCount,
  String? audioPath,
}) async {
  final pillId = await database.insert(
    'pills',
    {
      'device_id':        deviceId,
      'name': name,
      'start_at_utc':     startAtUtc,
      'interval_seconds': intervalSeconds,
      'total_count':      totalCount,
      'audio_path':       audioPath,
    },
  );
  unawaited(_syncPillToDevice(
    deviceId: deviceId,
    pillId: pillId,
    name: name,
    startAtUtc: startAtUtc,
    intervalSeconds: intervalSeconds,
    totalCount: totalCount,
    audioPath: audioPath,
  ));
  return pillId;
}

// The pill sync/delete endpoints are subscriber-authenticated server-side, so
// every call must carry the user's account creds.
Future<Map<String, String>> _authHeaders() async {
  final creds = await AuthStore.creds();
  return {
    'Username': creds?.username ?? '',
    'Password': creds?.password ?? '',
  };
}

// Pills are per-device: sync only to the device this pill belongs to.
Future<void> _syncPillToDevice({
  required String deviceId,
  required int pillId,
  required String name,
  required String startAtUtc,
  required int intervalSeconds,
  required int totalCount,
  String? audioPath,
}) async {
  final url = Uri.parse('$serverUrl/Pill_Sync_App');
  final req = http.MultipartRequest('POST', url);
  req.headers.addAll(await _authHeaders());
  req.fields['text'] = jsonEncode({
    'device_id':        deviceId,
    'pill_id_local':    pillId,
    'name':             name,
    'start_at_utc':     startAtUtc,
    'interval_seconds': intervalSeconds,
    'total_count':      totalCount,
  });
  if (audioPath != null && File(audioPath).existsSync()) {
    req.files.add(await http.MultipartFile.fromPath('audio file', audioPath));
  }
  try {
    final res = await req.send().timeout(const Duration(seconds: 15));
    if (res.statusCode != 200) {
      print('[pill-sync] device=$deviceId HTTP ${res.statusCode}');
    }
  } catch (e) {
    print('[pill-sync] device=$deviceId failed: $e');
  }
}

Future<List<Map<String, dynamic>>> getPills(String deviceId) async {
  // `database` is dynamic, so query() returns dynamic; type the rows list
  // explicitly and the map's generic so toList() yields List<Map<String,
  // dynamic>>. Without this the implicit List<dynamic> threw a subtype error
  // at runtime and the pills page silently rendered empty.
  final List<dynamic> rows = await database.query('pills',
      where: 'device_id = ?', whereArgs: [deviceId], orderBy: 'pill_id DESC');
  return rows.map<Map<String, dynamic>>((r) => {
        'pill_id':          r['pill_id'],
        'device_id':        r['device_id'],
        'name':             r['name'],
        'start_at_utc':     r['start_at_utc'],
        'interval_seconds': r['interval_seconds'] ?? 0,
        'total_count':      r['total_count'] ?? 1,
        'audio_path':       r['audio_path'],
      }).toList();
}

Future<void> deletePill(int pillId, String deviceId) async {
  await database.delete('pills', where: 'pill_id = ?', whereArgs: [pillId]);
  // Purge the schedule from this device's server-side record. Best-effort.
  unawaited(_deletePillFromDevice(deviceId, pillId));
}

Future<void> _deletePillFromDevice(String deviceId, int pillId) async {
  // Retry a few times: if this never reaches the server, the schedule lives on
  // server-side and the device keeps reminding for a pill the user deleted.
  final auth = await _authHeaders();
  for (int attempt = 1; attempt <= 3; attempt++) {
    try {
      final res = await http.post(
        Uri.parse('$serverUrl/Pill_Delete_App'),
        headers: {'Content-Type': 'application/json', ...auth},
        body: jsonEncode({'device_id': deviceId, 'pill_id_local': pillId}),
      ).timeout(const Duration(seconds: 8));
      if (res.statusCode == 200) return;
      print('[pill-delete] device=$deviceId HTTP ${res.statusCode} (attempt $attempt)');
    } catch (e) {
      print('[pill-delete] device=$deviceId failed (attempt $attempt): $e');
    }
    if (attempt < 3) await Future.delayed(Duration(seconds: 2 * attempt));
  }
  print('[pill-delete] device=$deviceId GAVE UP — server may keep reminding');
}

/// Insert/replace a device row.
///
/// `devicePassword` is the 32+ char shared secret. It lives ONLY in
/// flutter_secure_storage (DeviceSecretStore) — never SQLite. The legacy
/// device_password column is scrubbed by the v7 migration and kept NULL.
Future<void> insertDevice(String deviceId, String devicePassword, String deviceName) async {
  // The password NEVER touches SQLite — Keystore only (v7 hardening).
  await DeviceSecretStore.save(deviceId, devicePassword);
  await database.insert(
    'devices',
    {
      'device_id': deviceId,
      'device_name': deviceName,
    },
    conflictAlgorithm: ConflictAlgorithm.replace,
  );

  // ارسال دستگاه‌های به‌روزرسانی شده به Stream
  final devices = await getDevices();
  DeviceStream().addDevices(devices);
}

Future<void> insertDeviceWithDefaultName(String deviceId, String devicePassword) async {
  await DeviceSecretStore.save(deviceId, devicePassword);
  await database.insert(
    'devices',
    {
      'device_id': deviceId,
      'device_name': 'Default Name',
    },
    conflictAlgorithm: ConflictAlgorithm.replace,
  );
}

/// Drop a device from the local DB. Called after a successful
/// /Unsubscribe_Device or when this app instance has been kicked.
/// Does NOT remove the secure_storage entry — call DeviceSecretStore.remove
/// separately (kept independent so a UI flow can present a "still kept
/// password" affordance if desired).
Future<void> deleteDevice(String deviceId) async {
  await database.delete('devices', where: 'device_id = ?', whereArgs: [deviceId]);
  await database.delete('events',  where: 'device_id = ?', whereArgs: [deviceId]);
  final devices = await getDevices();
  DeviceStream().addDevices(devices);
}

Future<void> updateDeviceName(String deviceId, String newDeviceName) async {
  await database.update(
    'devices',
    {
      'device_name': newDeviceName,
    },
    where: 'device_id = ?',
    whereArgs: [deviceId],
  );
  // Notify listeners so the device list re-reads the new name from the DB
  // (same pattern as deleteDevice). Without this, the rename persists to the DB
  // but the visible card keeps showing the old name until the next reload.
  final devices = await getDevices();
  DeviceStream().addDevices(devices);
}


Future<void> updateDeviceImage(String deviceId, String newDeviceImage) async {
  // Delete the previous decrypted snapshot so old thumbnails don't accumulate
  // as plaintext images in the app dir (they're replaced every snapshot poll).
  try {
    final old = await database.query('devices',
        columns: ['last_image'], where: 'device_id = ?', whereArgs: [deviceId]);
    final oldPath = old.isNotEmpty ? old.first['last_image'] as String? : null;
    if (oldPath != null && oldPath.isNotEmpty && oldPath != newDeviceImage) {
      final f = File(oldPath);
      if (await f.exists()) await f.delete();
    }
  } catch (_) {/* best-effort */}
  await database.update(
    'devices',
    {
      'last_image': newDeviceImage,
    },
    where: 'device_id = ?',
    whereArgs: [deviceId],
  );
  // Push the refreshed list onto DeviceStream so the main page rebuilds its
  // cards with the new last_image. Without this, the DB row updates but the
  // in-memory devices list stays stale and the thumbnail only refreshes on a
  // page re-entry / cold start — which read as "thumbnail not updating".
  final devices = await getDevices();
  DeviceStream().addDevices(devices);
}


// Local cap so the on-device events table + its UI list + saved media files
// don't grow forever. insertEvent runs on every heartbeat; the server caps its
// own copy, but once delivered & stored locally nothing pruned it before. Keep
// the newest N per device (mirrors the server's retention intent).
const int kMaxLocalEventsPerDevice = 50;

Future<void> insertEvent(String messageId, String deviceId, String messageText, String messageImage, String messageVideo, String messageTime, String messageStatus, {String eventType = 'fall', bool ifAbsent = false}) async {
  await database.insert(
    'events',
    {
      'event_id': messageId,
      'device_id': deviceId,
      'event_text': messageText,
      'event_image': messageImage,
      'event_video': messageVideo,
      'event_time': messageTime,
      'event_status': messageStatus,
      'event_type': eventType,
    },
    // ifAbsent (MQTT fast-path) → insert only if new, so it never clobbers the
    // richer heartbeat-delivered row (which carries the image).
    conflictAlgorithm: ifAbsent ? ConflictAlgorithm.ignore : ConflictAlgorithm.replace,
  );
  await _pruneLocalEvents(deviceId);
}

/// Per-device unread counts split by kind, for the colour-coded card badges:
/// returns {'fall': n, 'voice': n, 'call': n}. Legacy rows with NULL type count
/// as 'fall'. 'call' here = number of unanswered call requests (≥1 → show the
/// green call banner).
Future<Map<String, int>> getUnreadCountsByType(String deviceId) async {
  final rows = await database.rawQuery(
    "SELECT COALESCE(event_type,'fall') AS t, COUNT(*) AS c FROM events "
    "WHERE device_id = ? AND event_status = 'unread' GROUP BY t",
    [deviceId],
  );
  final out = {'fall': 0, 'voice': 0, 'call': 0};
  for (final r in rows) {
    final t = (r['t'] as String?) ?? 'fall';
    if (out.containsKey(t)) out[t] = (r['c'] as int?) ?? 0;
  }
  return out;
}

/// Mark all unread events of a given type read (e.g. when the user opens the
/// fall list, the voice inbox, or places the call back).
Future<void> markTypeRead(String deviceId, String eventType) async {
  await database.update(
    'events',
    {'event_status': 'read'},
    where: "device_id = ? AND event_status = 'unread' AND COALESCE(event_type,'fall') = ?",
    whereArgs: [deviceId, eventType],
  );
}

/// Delete events for [deviceId] beyond the newest [kMaxLocalEventsPerDevice]
/// (by event_time), and remove their saved image/video files from disk so the
/// app's storage stays bounded.
Future<void> _pruneLocalEvents(String deviceId) async {
  final List<dynamic> stale = await database.rawQuery(
    'SELECT event_id, event_image, event_video FROM events '
    'WHERE device_id = ? AND event_id NOT IN ('
    '  SELECT event_id FROM events WHERE device_id = ? '
    '  ORDER BY event_time DESC LIMIT ?'
    ')',
    [deviceId, deviceId, kMaxLocalEventsPerDevice],
  );
  if (stale.isEmpty) return;
  for (final r in stale) {
    for (final p in [r['event_image'], r['event_video']]) {
      if (p is String && p.isNotEmpty && p != 'null') {
        try {
          final f = File(p);
          if (await f.exists()) await f.delete();
        } catch (_) {/* best-effort file cleanup */}
      }
    }
  }
  final ids = stale.map((r) => r['event_id']).toList();
  final placeholders = List.filled(ids.length, '?').join(',');
  await database.rawDelete(
    'DELETE FROM events WHERE event_id IN ($placeholders)', ids);
}

Future<void> deleteAllEvents() async {
  await database.delete('events');
  // print('🗑️ همه رویدادها حذف شدند.');
}


Future<void> updateEventStatusAsRead(String messageId) async {
  await database.update(
    'events',
    {
      'event_status': "read",
    },
    where: 'event_id = ?',
    whereArgs: [messageId],
  );
  
}


// printAllDevices()/printAllEvents() removed — they dumped device IDs + media
// paths (PII) to logcat in release builds.

Future<List<Map<String, dynamic>>> getDevices() async {
  final rows = await database.query('devices');
  // sqflite query rows are READ-ONLY. Return mutable copies so every consumer
  // (DeviceStream subscribers, the urgency sort in _loadDevices, the rename
  // path) can update fields in place without "Unsupported operation:
  // read-only". Fixing it here covers all three addDevices() emit sites and any
  // future consumer.
  return rows.map((r) => Map<String, dynamic>.from(r)).toList();
}


Future<List<Map<String, dynamic>>> getEvents() async {
  final List<Map<String, dynamic>> events = await database.query('events');
  return events;
}


Future<List<Map<String, dynamic>>> getDeviceEvents(String deviceId) async {
  // FALL VIEW ONLY: show fall events exclusively. event_type NULL = legacy rows
  // (pre-v6, before the column existed) which were always falls. Wake/voice/call
  // events have their own tabs and must NOT appear here.
  final List<Map<String, dynamic>> events = await database.query(
    'events',
    where: "device_id = ? AND (event_type = 'fall' OR event_type IS NULL)",
    whereArgs: [deviceId],
    orderBy: 'event_time DESC',
  );
  return events;
}




// تابع حذف کامل دیتابیس
Future<void> deleteDatabaseFile() async {
  final dbPath = await getDatabasesPath();
  final path = join(dbPath, 'app_database.db');

  await deleteDatabase(path); // حذف کامل فایل دیتابیس
}


// Future<bool> hasUnreadEvents(String deviceId) async {
//   final result = await database.query(
//     'events',
//     where: 'device_id = ? AND event_status = ?',
//     whereArgs: [deviceId, 'unread'],
//   );
//   return result.isNotEmpty;
// }


Future<int> getUnreadEventsCount(String deviceId) async {
  final dbPath = await getDatabasesPath();
  final path = join(dbPath, 'app_database.db');

   if (!await databaseExists(path)) {
    return 0;
  }
  final result = await database.query(
    'events',
    where: 'device_id = ? AND event_status = ?',
    whereArgs: [deviceId, 'unread'],
  );
  return result.length; // تعداد رویدادهای خوانده نشده
}


