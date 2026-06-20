// Phase 5.1 — MQTT subscriber for the elderly-care app.
//
// One client per logged-in user. Connects on login, subscribes to:
//   • user/<self>/notify              — generic per-user notifications
//   • device/<id>/event/fall          — for each device the user is subscribed to
//   • device/<id>/event/voice         — same
//   • device/<id>/event/wake_command  — same
//   • device/<id>/state/pill          — pill alarm state changes (badge clear)
//   • device/<id>/state/call_pending  — "in call by X" badge
//
// Replaces the 30-second HTTP polling in heartbeat.dart. The Phase 5.1
// migration is incremental: HeartbeatService and MqttService coexist for
// one or two releases; once MQTT proves stable, heartbeat.dart gets deleted.
//
// Reconnects automatically with exponential backoff (mqtt_client default).
// Client disconnects when the user logs out or the app is detached.
import 'dart:async';
import 'dart:convert';

import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../configs.dart';
import '../db.dart';
import '../stream.dart';
import 'secure_storage.dart';

/// Broker host — resolved from --dart-define MQTT_HOST, falling back to
/// the same host as SERVER_URL so dev/prod toggling stays in one flag.
final String _mqttHost = const String.fromEnvironment(
  'MQTT_HOST',
  defaultValue: '',
).isNotEmpty
    ? const String.fromEnvironment('MQTT_HOST')
    : Uri.parse(serverUrl).host;

const int _mqttPort = int.fromEnvironment('MQTT_PORT', defaultValue: 1883);

typedef _PayloadHandler = void Function(String topic, Map<String, dynamic> payload);

class MqttService {
  MqttService._();
  static final MqttService instance = MqttService._();

  MqttServerClient? _client;
  String? _username;
  final Set<String> _subscribedTopics = {};
  bool _wantConnected = false;   // true between start() and stop()
  int _retryDelay = 2;           // initial-connect backoff (seconds)

  /// Connect + subscribe. Idempotent — calling twice with the same user
  /// is a no-op. After login, call this from MainPage's initState.
  Future<void> start() async {
    final prefs = await SharedPreferences.getInstance();
    final username = prefs.getString('username');
    if (username == null) return;
    _wantConnected = true;
    if (_client != null && _username == username) return;
    _username = username;

    final c = MqttServerClient(_mqttHost, 'app-$username-${DateTime.now().millisecondsSinceEpoch}')
      ..port = _mqttPort
      ..keepAlivePeriod = 30
      ..autoReconnect = true
      ..resubscribeOnAutoReconnect = true
      ..logging(on: false)
      ..onAutoReconnect = () {
        // ignore: avoid_print
        print('[mqtt] auto-reconnecting to $_mqttHost:$_mqttPort');
      }
      ..onConnected = _onConnected
      ..onDisconnected = () {
        // ignore: avoid_print
        print('[mqtt] disconnected');
      };

    // Assign _client BEFORE connect(): mqtt_client fires onConnected (→ _subscribe)
    // synchronously DURING connect(), so _client must already be set or every
    // subscription silently no-ops. On failure we reset _client/_username so the
    // idempotency guard doesn't block retries.
    _client = c;
    try {
      await c.connect();
    } catch (e) {
      // ignore: avoid_print
      print('[mqtt] initial connect failed: $e — retry in ${_retryDelay}s');
      _client = null;
      _username = null;
      final d = _retryDelay;
      _retryDelay = _retryDelay * 2 > 60 ? 60 : _retryDelay * 2;  // backoff, cap 60s
      Future.delayed(Duration(seconds: d), () {
        if (_wantConnected && _client == null) start();   // server back up → reconnect
      });
      return;
    }
    _retryDelay = 2;                       // connected → reset backoff
    c.updates?.listen(_onMessage);
  }

  Future<void> stop() async {
    _wantConnected = false;
    _client?.disconnect();
    _client = null;
    _subscribedTopics.clear();
    _username = null;
  }

  // Plain void callback for mqtt_client — delegates to an async impl whose
  // errors are caught (an uncaught throw in an `async void` is silently
  // dropped and would leave the per-device topics unsubscribed).
  void _onConnected() {
    _onConnectedAsync().catchError((e) {
      // ignore: avoid_print
      print('[mqtt] _onConnected error: $e');
    });
  }

  Future<void> _onConnectedAsync() async {
    // ignore: avoid_print
    print('[mqtt] connected to $_mqttHost:$_mqttPort');
    _subscribe('user/$_username/notify');
    // per-device topics — re-fetched on every (re)connect to pick up newly
    // subscribed devices.
    final devices = await getDevices();
    for (final d in devices) {
      final id = d['device_id'] as String;
      _subscribe('device/$id/event/fall');
      _subscribe('device/$id/event/voice');
      _subscribe('device/$id/event/wake_command');
      _subscribe('device/$id/state/pill');
      _subscribe('device/$id/state/call_pending');
    }
  }

  void _subscribe(String topic) {
    final c = _client;
    if (c == null) return;
    if (_subscribedTopics.contains(topic)) return;
    c.subscribe(topic, MqttQos.atLeastOnce);
    _subscribedTopics.add(topic);
  }

  /// Add a new device to the subscription set without reconnecting. Call
  /// after Subscribe_Device / Register_Device completes.
  void subscribeDeviceTopics(String deviceId) {
    _subscribe('device/$deviceId/event/fall');
    _subscribe('device/$deviceId/event/voice');
    _subscribe('device/$deviceId/event/wake_command');
    _subscribe('device/$deviceId/state/pill');
    _subscribe('device/$deviceId/state/call_pending');
  }

  void _onMessage(List<MqttReceivedMessage<MqttMessage>> messages) {
    for (final m in messages) {
      final topic = m.topic;
      final payload = m.payload as MqttPublishMessage;
      final raw = MqttPublishPayload.bytesToStringAsString(payload.payload.message);
      Map<String, dynamic> body;
      try {
        body = jsonDecode(raw) as Map<String, dynamic>;
      } catch (_) {
        body = {'raw': raw};
      }
      _route(topic, body);
    }
  }

  void _route(String topic, Map<String, dynamic> body) {
    // ignore: avoid_print
    print('[mqtt] $topic ← ${body.length} fields');
    if (topic.endsWith('/event/fall')) {
      // Persist to the local DB (ifAbsent → don't clobber the heartbeat's richer
      // row) so DeviceDetailsPage, which renders from the DB, shows the fall
      // immediately instead of waiting for the next heartbeat poll.
      insertEvent(
        body['message_id'].toString(),
        _deviceIdFromTopic(topic),
        '{}', '', '',
        (body['occur_time'] ?? '').toString(),
        'unread',
        eventType: 'fall',
        ifAbsent: true,
      ).whenComplete(() => NotificationStream().notify());
      NotificationStream().notify();
    } else if (topic.endsWith('/event/voice')) {
      // Voice tab refreshes itself when it sees the notification stream tick.
      NotificationStream().notify();
    } else if (topic.endsWith('/state/pill') ||
               topic.endsWith('/state/call_pending') ||
               topic.endsWith('/event/wake_command')) {
      // Generic UI refresh — screens listening on NotificationStream re-fetch.
      NotificationStream().notify();
    } else if (topic == 'user/$_username/notify') {
      // Server pushes {"type":"device_removed","device_id":...} when a device is
      // factory-reset or superseded by a re-provision — remove it locally now so
      // it disappears from the list without a re-login.
      if (body['type'] == 'device_removed') {
        final removedId = body['device_id']?.toString();
        if (removedId != null && removedId.isNotEmpty) {
          _handleDeviceRemoved(removedId);
          return;
        }
      }
      NotificationStream().notify();
    }
  }

  Future<void> _handleDeviceRemoved(String deviceId) async {
    try {
      await deleteDevice(deviceId);
      await DeviceSecretStore.remove(deviceId);
    } catch (_) {/* best-effort local cleanup */}
    try {
      DeviceStream().addDevices(await getDevices());  // refresh the list UI
    } catch (_) {}
    NotificationStream().notify();
  }

  String _deviceIdFromTopic(String topic) {
    // device/<id>/event/fall → <id>
    final parts = topic.split('/');
    return parts.length >= 2 ? parts[1] : '';
  }
}
