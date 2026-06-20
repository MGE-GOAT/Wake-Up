import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'dart:convert';
import 'dart:async';
import 'package:sqflite/sqflite.dart';
import 'package:path/path.dart';

import 'package:shared_preferences/shared_preferences.dart';
import 'configs.dart';
import 'password_setup_page.dart';

import 'login.dart';
// add_device_page was replaced by add_device_entry_page in Phase 2.
import 'db.dart';
import 'devices.dart';
import 'dart:io';
import 'heartbeat.dart';
import 'FullScreenImage.dart';
import 'settings_page.dart';
import 'search_devices.dart';
import 'package:firebase_messaging/firebase_messaging.dart';
import 'package:firebase_core/firebase_core.dart';

import 'package:flutter_local_notifications/flutter_local_notifications.dart';

@pragma('vm:entry-point')
Future<void> _firebaseMessagingBackgroundHandler(RemoteMessage message) async {
  await NotificationService.instance.setupFlutterNotifications();
  await NotificationService.instance.showNotification(message);
}

class NotificationService {
  NotificationService._();
  static final NotificationService instance = NotificationService._();
  final _messaging = FirebaseMessaging.instance;
  final _localNotifications = FlutterLocalNotificationsPlugin();
  bool _isFlutterLocalNotificationsInitialized = false;
  String? _fcmToken;

  /// True only if this device obtained an FCM token (i.e. Firebase is
  /// reachable). When false (common on networks that can't reach Google),
  /// push will never arrive, so the heartbeat poll raises LOCAL notifications
  /// instead — see HeartbeatService. Used to avoid double notifications on
  /// devices where FCM does work.
  bool get pushAvailable => _fcmToken != null;

  Future<String?> getFirebaseToken() async {
    return await _messaging.getToken();
  }

  /// Raise a system-tray notification locally (no FCM round-trip). Lets the
  /// heartbeat surface fall/voice/call alerts on devices that can't reach
  /// Firebase. [colorHex] like "#FF0000".
  Future<void> showLocalNotification({
    required String title,
    required String body,
    String colorHex = '#FF0000',
    // Per-type channel so Android groups + lets the user tune importance/sound
    // per category. Defaults to the fall (max-importance) channel.
    String channelId = 'fall_channel',
    String channelName = 'هشدار سقوط',
  }) async {
    final importance = channelId == 'pill_channel'
        ? Importance.low
        : channelId == 'voice_channel'
            ? Importance.defaultImportance
            : channelId == 'call_channel'
                ? Importance.high
                : Importance.max;
    await setupFlutterNotifications();
    await _localNotifications.show(
      DateTime.now().microsecondsSinceEpoch ~/ 1000 & 0x7fffffff,
      title,
      body,
      NotificationDetails(
        android: AndroidNotificationDetails(
          channelId,
          channelName,
          channelDescription: 'Noban: $channelName',
          importance: importance,
          priority:
              importance == Importance.low ? Priority.low : Priority.high,
          icon: '@drawable/ic_notification',
          color: _hexToColor(colorHex),
          styleInformation: BigTextStyleInformation(body), // show full body + device line
        ),
      ),
    );
  }

  /// Best-effort notification setup. MUST NOT throw or hang: it runs before
  /// runApp() in main(), so any uncaught error or indefinite await here means
  /// the whole app never renders (black screen). On restricted networks
  /// getToken() throws "SERVICE_NOT_AVAILABLE" / connection-refused, which
  /// previously killed startup — so every step is guarded and getToken is
  /// time-boxed. Push just stays disabled until the network recovers.
  Future<void> initialize() async {
    try {
      FirebaseMessaging.onBackgroundMessage(_firebaseMessagingBackgroundHandler);
      await _requestPermission();
      await _setupMessageHandlers();
    } catch (e) {
      print('NotificationService.initialize (non-token) failed: $e');
    }
    // getToken can throw or hang when FCM servers are unreachable. Time-box it
    // and swallow failures — notifications are optional; rendering is not.
    try {
      _fcmToken = await _messaging.getToken().timeout(const Duration(seconds: 8));
      // FCM token obtained (value intentionally NOT logged — it is sensitive).
    } catch (e) {
      _fcmToken = null;
      print('FCM getToken unavailable (push disabled, heartbeat will raise local notifications): $e');
    }
  }

  Future<void> _requestPermission() async {
    final settings = await _messaging.requestPermission(
      alert: true,
      badge: true,
      sound: true,
      provisional: false,
      announcement: false,
      // carplay: false,
      criticalAlert: false,
    );
    print('Permission status: ${settings.authorizationStatus}');
  }

  Future<void> setupFlutterNotifications() async {
    if (_isFlutterLocalNotificationsInitialized) {
      return;
    }

    // android setup — one channel per alert type so Android groups them and the
    // user can tune importance/sound per category.
    const channels = <AndroidNotificationChannel>[
      AndroidNotificationChannel('fall_channel', 'هشدار سقوط',
          description: 'هشدار سقوط', importance: Importance.max),
      AndroidNotificationChannel('call_channel', 'درخواست تماس',
          description: 'درخواست تماس', importance: Importance.high),
      AndroidNotificationChannel('voice_channel', 'پیام صوتی',
          description: 'پیام صوتی جدید', importance: Importance.defaultImportance),
      AndroidNotificationChannel('pill_channel', 'دارو',
          description: 'وضعیت دارو', importance: Importance.low),
      // kept for back-compat with any FCM payloads still targeting it
      AndroidNotificationChannel('high_importance_channel',
          'High Importance Notifications',
          description: 'Important notifications.', importance: Importance.high),
    ];
    final androidPlugin = _localNotifications
        .resolvePlatformSpecificImplementation<
            AndroidFlutterLocalNotificationsPlugin>();
    for (final c in channels) {
      await androidPlugin?.createNotificationChannel(c);
    }

    const initializationSettingsAndroid =
        AndroidInitializationSettings('@mipmap/ic_launcher');

    final initializationSettings = InitializationSettings(
      android: initializationSettingsAndroid,
    );

    // flutter notification setup
    await _localNotifications.initialize(
      initializationSettings,
      onDidReceiveNotificationResponse: (details) {
        // handle notification response
      },
    );

    _isFlutterLocalNotificationsInitialized = true;
  }

Future<void> showNotification(RemoteMessage message) async {
  RemoteNotification? notification = message.notification;
  AndroidNotification? android = message.notification?.android;
  if (notification != null && android != null) {
    // Get the color from payload (default to red if not specified)
    String colorString = message.data['color'] ?? '#FF0000';
    Color notificationColor = _hexToColor(colorString);

    await _localNotifications.show(
      notification.hashCode,
      notification.title,
      notification.body,
      NotificationDetails(
        android: AndroidNotificationDetails(
          'high_importance_channel',
          'High Importance Notifications',
          channelDescription:
              'This channel is used for important notifications.',
          importance: Importance.high,
          priority: Priority.high,
          icon: '@drawable/ic_notification',
          color: notificationColor, // use dynamic color from server
        ),
        iOS: const DarwinNotificationDetails(
          presentAlert: true,
          presentBadge: true,
          presentSound: true,
        ),
      ),
      payload: message.data.toString(),
    );
  }
}

// Helper function to convert hex string to Color
Color _hexToColor(String hex) {
  hex = hex.replaceAll('#', '');
  if (hex.length == 6) hex = 'FF$hex'; // add alpha if missing
  return Color(int.parse(hex, radix: 16));
}

  Future<void> _setupMessageHandlers() async {
    // foreground message
    FirebaseMessaging.onMessage.listen((message) {
      showNotification(message);
    });

    // background message
    FirebaseMessaging.onMessageOpenedApp.listen(_handleBackgroundMessage);

    // opened app
    final initialMessage = await _messaging.getInitialMessage();
    if (initialMessage != null) {
      _handleBackgroundMessage(initialMessage);
    }
  }

  void _handleBackgroundMessage(RemoteMessage message) {
    if (message.data['type'] == 'chat') {
      // open chat screen
    }
  }
}

















// final FirebaseMessaging _firebaseMessaging = FirebaseMessaging.instance;

// Future<void> initFirebaseMessaging() async {
//   String? token = await _firebaseMessaging.getToken();
//   print("FCM Token: $token");

//   // ارسال توکن به سرور
//   sendTokenToServer(token);
// }

// void sendTokenToServer(String? token) {
//   // اینجا توکن را به سرور خود ارسال کنید
// }














