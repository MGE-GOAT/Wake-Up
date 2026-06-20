// Background poller for new server-side events (image snapshots, fall
// events, voice messages, wake-word triggers).
//
// THIS IS A BAND-AID. The real fix is Phase 5.1 MQTT migration — replace
// this whole file with a single mqtt_client subscription per user. Until
// then:
//   • 30 s interval (was 150 ms) — 200× less network/battery load
//   • Stops when the app is paused or detached (WidgetsBindingObserver)
//   • Reads username/password from prefs (was hardcoded for one user)
//   • Skips polling if not logged in
//
// Migrate by deleting this file and wiring `mqttSubscribe(user/<self>/notify)`
// → handler that calls the same _handleNewMessage().
import 'dart:async';
import 'dart:convert';
import 'package:flutter/widgets.dart';
import 'package:http/http.dart' as http;
import 'package:shared_preferences/shared_preferences.dart';

import 'configs.dart';
import 'db.dart';
import 'stream.dart';
import 'firebase.dart';
import 'services/media_crypto.dart';
import 'services/secure_storage.dart';

const Duration _kPollInterval = Duration(seconds: 30);
const Duration _kRequestTimeout = Duration(seconds: 15);

class HeartbeatService with WidgetsBindingObserver {
  Timer? _heartbeatTimer;
  bool _started = false;
  bool _stopping = false;  // set on stop → in-flight poll must not touch the (deleted) DB
  bool _polling = false;   // overlap guard → a slow poll can't pile up behind itself

  void startHeartbeat() {
    if (_started) return;
    _started = true;
    _stopping = false;
    WidgetsBinding.instance.addObserver(this);
    _arm();
  }

  void stopHeartbeat() {
    if (!_started) return;
    _started = false;
    _stopping = true;
    WidgetsBinding.instance.removeObserver(this);
    _disarm();
  }

  void _arm() {
    _heartbeatTimer ??= Timer.periodic(_kPollInterval, (_) => _sendHeartbeat());
  }

  /// Trigger one poll immediately (e.g. pull-to-refresh), instead of waiting
  /// for the next 30 s tick. Safe to call anytime; no-op effects if logged out.
  Future<void> pollNow() => _sendHeartbeat();

  void _disarm() {
    _heartbeatTimer?.cancel();
    _heartbeatTimer = null;
  }

  // Pause polling while the app isn't visible — both saves battery and stops
  // wasted requests piling up while user has the app backgrounded.
  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    switch (state) {
      case AppLifecycleState.resumed:
        _arm();
        // Fire one immediately so resumed UI doesn't wait up to 30 s for refresh.
        unawaited(_sendHeartbeat());
      case AppLifecycleState.paused:
      case AppLifecycleState.detached:
      case AppLifecycleState.hidden:
        _disarm();
      case AppLifecycleState.inactive:
        // Brief transitional state (e.g. incoming system overlay) — keep
        // running, will return to resumed shortly.
        break;
    }
  }

  Future<void> _sendHeartbeat() async {
    if (_stopping || _polling) return; // stopping, or a poll already in flight
    _polling = true;
    try {
    final creds = await AuthStore.creds();
    if (creds == null) return; // not logged in
    final username = creds.username;
    final password = creds.password;

    try {
      final response = await http.post(
        Uri.parse('$serverUrl/Message_Check_App'),
        headers: {
          'Username': username,
          'Password': password,
          'Content-Length': '0',
        },
      ).timeout(_kRequestTimeout);

      if (_stopping) return;   // logout happened during the request → DB may be gone
      if (response.statusCode == 200) {
        final data = json.decode(response.body);
        final newMessages = data is Map ? data['new_messages'] : null;
        if (newMessages is List) {
          for (final message in newMessages) {
            // Per-message guard: a single malformed message must not drop the
            // rest of the batch.
            try {
              await _handleNewMessage(Map<String, dynamic>.from(message));
            } catch (e) {
              print('[heartbeat] skipped a message: $e');
            }
          }
        }
      } else {
        // Server bounced us — likely auth changed or server down. Logging at
        // 30s cadence is acceptable; previously this was 6.7x per second.
        // ignore: avoid_print
        print('[heartbeat] server returned ${response.statusCode}');
      }
    } catch (e) {
      // ignore: avoid_print
      print('[heartbeat] network: $e');
    }
    } finally {
      _polling = false;
    }
  }

  Future<void> _handleNewMessage(Map<String, dynamic> message) async {
    final deviceId = message['device_id'];
    final messageType = message['message_type'];
    final messageId = message['message_id'].toString();
    final messageText = message['message_text'];
    final messageImage = message['message_image'];
    // The server no longer inlines the (large) fall MP4 — it sends has_video,
    // and the app downloads the clip on demand via /Fall_Video_App when the
    // user taps the event. We store a 'remote' sentinel in event_video so the
    // UI knows a downloadable video exists.
    final bool hasVideo = message['has_video'] == true;
    final messageTime = message['message_occur_time'];

    // Phase 3.1 — media may be E2E-encrypted (XChaCha20-Poly1305). The device
    // sets "encrypted":true in the message text JSON; the bytes we base64-
    // decode are then ciphertext that we decrypt with the device password
    // from DeviceSecretStore before writing the displayable file.
    Map<String, dynamic> mt = {};
    try {
      final decoded = jsonDecode(messageText);
      if (decoded is Map) mt = Map<String, dynamic>.from(decoded);
    } catch (_) {}
    final bool encrypted = mt['encrypted'] == true;

    // On devices that can't reach Firebase (no FCM token), push never arrives,
    // so we raise the system-tray notification locally from this poll. Gated on
    // !pushAvailable to avoid double notifications where FCM already delivers.
    final bool raiseLocal = !NotificationService.instance.pushAvailable;

    if (messageType == 'image captured') {
      // Guard the base64 arg — a snapshot message with null/empty image would
      // otherwise throw on the required-String param (and get silently dropped).
      if (messageImage == null || (messageImage as String).isEmpty) return;
      final imagePath = await saveIncomingMedia(
          base64Data: messageImage, fileName: '$deviceId-$messageId.png',
          deviceId: deviceId, encrypted: encrypted);
      if (imagePath != null) await updateDeviceImage(deviceId, imagePath);
    } else if (messageType == 'Event' || messageType == 'Wake up word') {
      String? imagePath;
      if (messageImage != null && messageImage.isNotEmpty) {
        imagePath = await saveIncomingMedia(
            base64Data: messageImage, fileName: '$deviceId-$messageId-image.png',
            deviceId: deviceId, encrypted: encrypted);
      }
      // 'remote' = a clip exists on the server, download on tap. Not downloaded
      // now (keeps the feed light).
      final String videoMarker = hasVideo ? 'remote' : 'null';
      // Only a REAL fall is tagged 'fall' (it drives the red fall badge + the
      // Fall view). A fall's INNER "Message Type" is 'Event' or absent; other
      // 'Event' payloads (e.g. SLEEP/WAKE mode-change events) carry their own
      // inner type and must NOT inflate the fall badge. A bare "Wake up word" is
      // its own non-fall type. This mirrors the Fall-view filter in devices.dart.
      final innerType = mt['Message Type'];
      final bool isFall = messageType == 'Event' &&
          (innerType == null || innerType == 'Event');
      final String evType =
          isFall ? 'fall' : (messageType == 'Wake up word' ? 'wake' : 'event');
      await insertEvent(messageId, deviceId, messageText,
          imagePath ?? 'null', videoMarker, messageTime, 'unread',
          eventType: evType);
      EventStream().addEvents([
        {
          'event_id': messageId,
          'device_id': deviceId,
          'event_text': messageText,
          'event_image': imagePath,
          'event_video': videoMarker,
          'event_time': messageTime,
          'event_status': 'unread',
        },
      ]);
      NotificationStream().notify();
      if (raiseLocal && isFall) {
        // Fall severity → colored emoji in the title (One UI ignores accent
        // color), plus the color for OSes that honor it.
        final lvl = mt['Danger Level'];
        const colors = {0: '#33FF00', 1: '#FFA500', 2: '#FF0000'};
        const labels = {0: 'بدون خطر جدی', 1: 'وضعیت نامشخص', 2: 'خطر جدی'};
        const emoji = {0: '🟢', 1: '🟠', 2: '🔴'};
        await NotificationService.instance.showLocalNotification(
          title: '${emoji[lvl] ?? '⚪'} هشدار سقوط',
          body: 'میزان خطر: ${labels[lvl] ?? ''}\nدستگاه: $deviceId',
          colorHex: colors[lvl] ?? '#FF0000',
        );
      } else if (raiseLocal) {
        await NotificationService.instance.showLocalNotification(
          title: (mt['Event Title'] ?? 'رویداد جدید').toString(),
          body: '${mt['Message Text'] ?? ''}\nدستگاه: $deviceId',
          colorHex: '#33FF00',
        );
      }
    } else if (messageType == 'Voice') {
      // A new voice message landed. The audio is fetched on demand by the voice
      // inbox (/Voice_Messages_App); store an unread 'voice' event so it drives
      // the blue per-device badge (cleared when the user opens the inbox).
      await insertEvent(messageId, deviceId, messageText, 'null', 'null',
          messageTime, 'unread', eventType: 'voice');
      VoiceStream().notify(deviceId);
      NotificationStream().notify();
      if (raiseLocal) {
        await NotificationService.instance.showLocalNotification(
          title: 'پیام صوتی جدید',
          body: 'بیمار پیام صوتی فرستاد\nدستگاه: $deviceId',
          colorHex: '#33AAFF',
          channelId: 'voice_channel',
          channelName: 'پیام صوتی',
        );
      }
    } else if (messageType == 'CallRequest') {
      // Elderly user asked to be called. Surface it as an unread 'call' event so
      // it shows in history AND drives the green call banner on the card.
      await insertEvent(messageId, deviceId, messageText, 'null', 'null',
          messageTime, 'unread', eventType: 'call');
      EventStream().addEvents([
        {
          'event_id': messageId,
          'device_id': deviceId,
          'event_text': messageText,
          'event_image': null,
          'event_video': null,
          'event_time': messageTime,
          'event_status': 'unread',
        },
      ]);
      NotificationStream().notify();
      if (raiseLocal) {
        await NotificationService.instance.showLocalNotification(
          title: '📞 درخواست تماس',
          body: 'بیمار درخواست تماس کرده است — لطفاً تماس بگیرید\nدستگاه: $deviceId',
          colorHex: '#FF8800',
          channelId: 'call_channel',
          channelName: 'درخواست تماس',
        );
      }
    } else if (messageType == 'PillAck') {
      // Patient acknowledged taking their pill (wake "pill"). Informational —
      // raise a notification so the caregiver knows the dose was taken.
      NotificationStream().notify();
      if (raiseLocal) {
        await NotificationService.instance.showLocalNotification(
          title: '💊 قرص مصرف شد',
          body: 'بیمار قرص خود را خورد\nدستگاه: $deviceId',
          colorHex: '#33FF00',
          channelId: 'pill_channel',
          channelName: 'دارو',
        );
      }
    }
  }
}
