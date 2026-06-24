import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'dart:convert';
import 'dart:async';
import 'package:sqflite/sqflite.dart';

import 'package:shared_preferences/shared_preferences.dart';
import 'configs.dart';
import 'call_page.dart';
import 'password_setup_page.dart';

import 'login.dart';
import 'add_device_entry_page.dart';
import 'services/mqtt_service.dart';
import 'db.dart';
import 'devices.dart';
import 'dart:io';
import 'heartbeat.dart';
import 'device_hub_page.dart';
import 'settings_page.dart';
import 'search_devices.dart';
import 'voice_message_button.dart';
import 'package:firebase_core/firebase_core.dart';

import 'firebase.dart';

import 'stream.dart';
import 'services/secure_storage.dart';

class MainPage extends StatefulWidget {
  const MainPage({super.key});

  @override
  State<MainPage> createState() => _MainPageState();
}

class _MainPageState extends State<MainPage> {
  final HeartbeatService _heartbeatService = HeartbeatService();
  List<Map<String, dynamic>> devices = [];
  String username = '';
  StreamSubscription? _subscription_1;

  @override
  void initState() {
    super.initState();
    _loadUserData();
    // Load devices, then ask each for a fresh snapshot so thumbnails populate
    // (and the E2E encrypt→decrypt path is exercised) shortly after connecting.
    _loadDevices().then((_) => _requestSnapshots());
    // Phase 5.1 — MQTT subscription is the primary push channel.
    // HeartbeatService stays armed as a safety net (now at 30s, not 150ms)
    // until we're confident MQTT delivery is 100% reliable on the user's
    // network. Once stable, delete the heartbeat line.
    MqttService.instance.start();
    _heartbeatService.startHeartbeat();
    _sendFirebaseTokenToServer();
    // گوش دادن به Stream
    _subscription_1 = DeviceStream().stream.listen((newDevices) {
      setState(() {
        devices = newDevices;
      });
    });
  }

  @override
  void dispose() {
    _heartbeatService.stopHeartbeat();
    _subscription_1?.cancel();
    super.dispose();
  }

  Future<void> _sendFirebaseTokenToServer() async {
    final token = await NotificationService.instance.getFirebaseToken();
    if (token != null) {
      await _sendConfigToServer(token);
    }
  }

  Future<void> _sendConfigToServer(String firebaseToken) async {
    final creds = await AuthStore.creds();
    if (creds == null) return; // not logged in — nothing to register
    final url = Uri.parse('$serverUrl/Config_App');
    final headers = {
      'Content-Type': 'application/json',
      'Password': creds.password,
      'User-Agent': 'App',
      'Username': creds.username,
    };
    final body = jsonEncode({
      'theme': 'dark',
      'notifications': 'true',
      'firebase_token': firebaseToken,
    });

    try {
      final response = await http
          .post(url, headers: headers, body: body)
          .timeout(const Duration(seconds: 15));
      if (response.statusCode == 200) {
        print('توکن با موفقیت به سرور ارسال شد.');
      } else {
        print('خطا در ارسال توکن: ${response.statusCode}');
      }
    } catch (e) {
      print('خطا در ارتباط با سرور: $e');
    }
  }

  Future<void> _loadUserData() async {
    final prefs = await SharedPreferences.getInstance();
    setState(() {
      username = prefs.getString('username') ?? 'نام کاربری نامعلوم';
    });
  }

  Future<void> _loadDevices() async {
    final loadedDevices = await getDevices();
    // sqflite query rows are read-only; copy to mutable maps so the rename
    // dialog can update device_name in place for instant UI refresh.
    final mutable =
        loadedDevices.map((d) => Map<String, dynamic>.from(d)).toList();
    // Urgency sort: whatever needs attention floats to the top.
    // call > fall > voice > idle (local unread counts; call also server-backed).
    for (final d in mutable) {
      final c = await getUnreadCountsByType(d['device_id'].toString());
      d['_urgency'] =
          (c['call'] ?? 0) * 1000 + (c['fall'] ?? 0) * 10 + (c['voice'] ?? 0);
    }
    mutable.sort((a, b) =>
        ((b['_urgency'] as int?) ?? 0).compareTo((a['_urgency'] as int?) ?? 0));
    if (!mounted) return;
    setState(() => devices = mutable);
  }

  // Ask each device to capture + send a fresh snapshot so the card thumbnail
  // populates shortly after connecting. This also exercises the E2E media
  // crypto path end-to-end: the device encrypts the JPEG (XChaCha20-Poly1305,
  // key derived from the device password), the app decrypts it on arrival
  // (heartbeat _handleNewMessage → _saveMediaToFile). Fire-and-forget; the
  // image arrives on the next /Message_Check_App poll and updates last_image.
  bool _snapshotsRequested = false;
  Future<void> _requestSnapshots({bool force = false}) async {
    // The auto-request on entry is one-shot (guarded); pull-to-refresh passes
    // force:true to re-ask every device for a fresh capture on demand.
    if (!force) {
      if (_snapshotsRequested) return;
      _snapshotsRequested = true;
    }
    final creds = await AuthStore.creds();
    if (creds == null) return;
    final username = creds.username;
    final password = creds.password;
    for (final d in devices) {
      final reqId = DateTime.now().microsecondsSinceEpoch % 1000000000;
      try {
        await http
            .post(Uri.parse('$serverUrl/Image_Request_App'),
                headers: {
                  'Username': username,
                  'Password': password,
                  'Content-Type': 'application/json',
                },
                body:
                    jsonEncode({'device_id': d['device_id'], 'req_id': reqId}))
            .timeout(const Duration(seconds: 10));
      } catch (_) {
        /* device offline / slow — thumbnail just stays placeholder */
      }
    }
  }

  // Pull-to-refresh handler: ask every device for a fresh snapshot, then poll
  // the server a few times so the new thumbnails (which arrive asynchronously
  // via the device→server→app path) get pulled and the cards update without
  // waiting for the 30 s heartbeat tick or an app reopen.
  Future<void> _onRefresh() async {
    await _requestSnapshots(force: true);
    // Devices need a moment to capture + upload; poll a few times to catch
    // them as they land. Each pollNow() runs /Message_Check_App which writes
    // the decoded image and pushes the refreshed list onto DeviceStream.
    for (int i = 0; i < 3; i++) {
      await Future.delayed(const Duration(seconds: 2));
      await _heartbeatService.pollNow();
    }
  }

  Future<void> _logout(BuildContext context) async {
    // Stop BOTH push channels BEFORE wiping the DB — otherwise an in-flight
    // MQTT/heartbeat message would try to write to the just-deleted database
    // (and the old user's subscriptions would keep delivering).
    _heartbeatService.stopHeartbeat();
    await MqttService.instance.stop();
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove('username');
    await prefs.remove('password'); // legacy plaintext (pre-migration installs)
    await AuthStore.clear();
    await prefs.setBool('isLoggedIn', false);
    await deleteDatabaseFile();
    if (!context.mounted) return;
    Navigator.pushReplacement(
      context,
      MaterialPageRoute(builder: (context) => LoginPage()),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      appBar: AppBar(
        backgroundColor: Colors.green,
        title: const Text(
          "صفحه اصلی",
          style: TextStyle(
              fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold),
        ),
        toolbarHeight: 90.0,
      ),
      drawer: Drawer(
        child: ListView(
          padding: EdgeInsets.zero,
          children: [
            UserAccountsDrawerHeader(
              decoration: BoxDecoration(color: Colors.green),
              accountName: Text(username,
                  style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold)),
              // SECURITY: never render the account password in the UI. Show the
              // username here instead.
              accountEmail: Text(username, style: TextStyle(fontSize: 13)),
              currentAccountPicture: CircleAvatar(
                backgroundColor: Colors.white,
                child: Icon(Icons.person, color: Colors.green, size: 25),
              ),
            ),
            ListTile(
              leading: Icon(Icons.exit_to_app, color: Colors.red),
              title: Text('خروج از حساب کاربری',
                  style: TextStyle(color: Colors.red)),
              onTap: () => _logout(context),
            ),
            ListTile(
              leading: Icon(Icons.settings),
              title: Text('تنظیمات'),
              onTap: () {
                Navigator.pop(context);
                Navigator.push(context,
                    MaterialPageRoute(builder: (context) => SettingsPage()));
              },
            ),
            // "Setup device" removed — redundant with the "افزودن دستگاه"
            // (add device) button at the bottom of the page.
            // Pill reminders moved into each device's hub page (per-device,
            // not shared) — see DeviceHubPage.
          ],
        ),
      ),
      // Pull down to refresh every device's thumbnail on demand. AlwaysScrollable
      // physics lets the gesture work even when the list is short or empty.
      body: RefreshIndicator(
        color: Colors.green,
        onRefresh: _onRefresh,
        child: devices.isEmpty
            ? ListView(
                physics: const AlwaysScrollableScrollPhysics(),
                children: [
                  SizedBox(
                    height: MediaQuery.of(context).size.height * 0.7,
                    child: const Center(
                      child: Text(
                        '!هیچ دستگاهی ثبت نشده است',
                        style: TextStyle(fontSize: 20, color: Colors.white),
                      ),
                    ),
                  ),
                ],
              )
            : ListView.builder(
                physics: const AlwaysScrollableScrollPhysics(),
                itemCount: devices.length,
                itemBuilder: (context, index) {
                  final device = devices[index];
                  return DeviceCard(
                    // Stable identity: without a key, an async DeviceStream/MQTT
                    // rebuild swaps in new device maps and ListView.builder
                    // DISPOSES the card — if a dialog (rename) opened from this
                    // card's context is up, its inherited deps unmount and throw
                    // '_dependents.isEmpty'. A ValueKey makes the element update
                    // in place instead of being recreated.
                    key: ValueKey(device['device_id']),
                    device: device,
                    onTap: () {
                      Navigator.push(
                        context,
                        MaterialPageRoute(
                          builder: (context) => DeviceHubPage(device: device),
                        ),
                      );
                    },
                    onRemoved: () {
                      if (!mounted) return;
                      // Don't rebuild the device list while a dialog or pushed
                      // page is on top — disposing cards mid-dialog throws
                      // Flutter's '_dependents.isEmpty' assertion. The device is
                      // already deleted locally; skip the UI refresh now and let
                      // the next 15s poll (once we're back on this page, with no
                      // modal up) drop the card cleanly.
                      if (!(ModalRoute.of(context)?.isCurrent ?? true)) return;
                      final label = (device['device_name']?.toString().isNotEmpty ?? false)
                          ? device['device_name'].toString()
                          : device['device_id'].toString();
                      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
                        content: Text("دستگاه «$label» حذف شد (ریست یا لغو دسترسی)"),
                      ));
                      _loadDevices();
                    },
                  );
                },
              ),
      ),
      floatingActionButton: FloatingActionButton.extended(
        backgroundColor: Colors.green,
        icon: Icon(Icons.add, color: Colors.white),
        label: Text(
          'افزودن دستگاه',
          style: TextStyle(
              fontSize: 18, fontWeight: FontWeight.bold, color: Colors.white),
        ),
        onPressed: () async {
          // Routes to entry choice (Connect existing / Set up new), each
          // of which talks to the new Phase 2 subscription endpoints.
          await Navigator.push(
            context,
            MaterialPageRoute(builder: (context) => const AddDeviceEntryPage()),
          );
          _loadDevices(); // به‌روزرسانی لیست دستگاه‌ها پس از افزودن
        },
      ),
      floatingActionButtonLocation: FloatingActionButtonLocation.endFloat,
    );
  }
}

class DeviceCard extends StatefulWidget {
  final Map<String, dynamic> device;
  final VoidCallback onTap;
  // Called when the server reports we no longer have access to this device
  // (factory-reset purge or our subscription was removed) → parent drops it.
  final VoidCallback? onRemoved;

  const DeviceCard({
    Key? key,
    required this.device,
    required this.onTap,
    this.onRemoved,
  }) : super(key: key);

  @override
  _DeviceCardState createState() => _DeviceCardState();
}

class _DeviceCardState extends State<DeviceCard> {
  // Per-type unread counts: {'fall': n (red), 'voice': n (blue), 'call': n}.
  Map<String, int> _unread = const {'fall': 0, 'voice': 0, 'call': 0};
  int get _fall => _unread['fall'] ?? 0;
  int get _voice => _unread['voice'] ?? 0;
  // Call banner is driven by the SERVER flag (cross-caregiver: clears for every
  // caregiver once anyone calls the device back), not the local unread count.
  bool _serverCallPending = false;
  bool get _callPending => _serverCallPending;
  bool _pillPending = false; // a due dose not yet taken (binary)
  bool _guestMode = false; // device in guest/sleep mode → NOT monitoring
  bool _isOffline = false; // device lost its heartbeat to the server
  bool _checking = false; // overlap guard for _checkPill → no piled-up polls
  String? _lastSeen; // last heartbeat time (for the offline banner)
  String username = '';
  StreamSubscription? _subscription;
  Timer? _stateTimer;

  @override
  void initState() {
    super.initState();
    _loadUserData();
    _checkUnreadEvents();
    _checkPill();
    _subscription = NotificationStream().stream.listen((_) {
      // Re-check on notifications; guard against firing after dispose.
      if (mounted) {
        _checkUnreadEvents();
        _checkPill();
      }
    });
    // Periodic re-poll: guest/pill/call state can change from the PHYSICAL
    // button, another caregiver, or the server — none of which raise a "new
    // message" notification — so without this the card's _guestMode/_pillPending
    // would go stale. Keeps every app in sync with the device's reported truth.
    _stateTimer = Timer.periodic(const Duration(seconds: 15), (_) {
      if (mounted) _checkPill();
    });
  }

  // Ask the server whether a dose is currently due-and-unacked (binary). One
  // pill command clears all due doses, so this is intentionally not a count.
  Future<void> _checkPill() async {
    if (_checking) return; // previous poll still running — skip this 15s tick
    _checking = true;
    try {
      final creds = await AuthStore.creds();
      if (creds == null) return;
      final u = creds.username;
      final p = creds.password;
      final r = await http
          .post(
            Uri.parse('$serverUrl/Pill_Status_App'),
            headers: {
              'Username': u,
              'Password': p,
              'Content-Type': 'application/json'
            },
            body: jsonEncode({'device_id': widget.device['device_id']}),
          )
          .timeout(const Duration(seconds: 10));
      // Auto-cleanup: the server deletes the device + all subscriptions on a
      // factory-reset purge (and on caregiver removal), so it then reports
      // not_subscribed (403). That is the definitive "remove this device" flag
      // forwarded to the phone — distinct from a transient offline (which is a
      // 200 with online:false) and from bad creds (403 "Invalid …"). Drop it.
      if (r.statusCode == 403) {
        try {
          final b = jsonDecode(r.body);
          if (b is Map && b['error'] == 'not_subscribed') {
            await deleteDevice(widget.device['device_id'].toString());
            if (mounted) widget.onRemoved?.call();
            return;
          }
        } catch (_) {/* non-JSON 403 — leave as-is */}
      }
      if (!mounted || r.statusCode != 200) return;
      final body = jsonDecode(r.body);
      final pillPending = body['pending'] == true;
      final callPending = body['call_pending'] == true;
      final guestMode = body['guest'] == true;
      // Offline only when the server explicitly says online==false on a good
      // 200 response. A missing field or a failed request leaves _isOffline as
      // is (see catch below), so an app-side network blip never shows a false
      // "device offline".
      final isOffline = body['online'] == false;
      final lastSeen =
          body['last_seen'] is String ? body['last_seen'] as String : null;
      if (pillPending != _pillPending ||
          callPending != _serverCallPending ||
          guestMode != _guestMode ||
          isOffline != _isOffline ||
          lastSeen != _lastSeen) {
        if (!mounted) return;
        setState(() {
          _pillPending = pillPending;
          _serverCallPending = callPending;
          _guestMode = guestMode;
          _isOffline = isOffline;
          _lastSeen = lastSeen;
        });
      }
    } catch (_) {/* best-effort */} finally {
      _checking = false;
    }
  }

  // Remote sleep/wake toggle. Queues a one-shot command on the server; the
  // device consumes it on its next heartbeat and reports its real state back,
  // which the next _checkPill poll re-syncs (so the UI converges to the device's
  // truth even if the command is missed). Optimistic update for snappy feedback.
  Future<void> _toggleGuest() async {
    final want = !_guestMode;
    try {
      final creds = await AuthStore.creds();
      if (creds == null) return;
      final u = creds.username;
      final p = creds.password;
      final r = await http
          .post(
            Uri.parse('$serverUrl/Set_Guest_App'),
            headers: {
              'Username': u,
              'Password': p,
              'Content-Type': 'application/json'
            },
            body: jsonEncode(
                {'device_id': widget.device['device_id'], 'guest': want}),
          )
          .timeout(const Duration(seconds: 10));
      if (!mounted || r.statusCode != 200) return;
      setState(() => _guestMode = want); // optimistic; poll below confirms
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
          content: Text(want
              ? 'دستور حالت خواب ارسال شد'
              : 'دستور فعال‌سازی نظارت ارسال شد')));
      // Re-check after the device has had time to consume + report back.
      Future.delayed(const Duration(seconds: 6), () {
        if (mounted) _checkPill();
      });
    } catch (_) {/* best-effort */}
  }

  @override
  void dispose() {
    _subscription?.cancel(); // لغو فقط این شنونده
    _stateTimer?.cancel();
    super.dispose();
  }

  Future<void> _loadUserData() async {
    final prefs = await SharedPreferences.getInstance();
    if (!mounted) return;
    setState(() {
      username = prefs.getString('username') ?? 'نام کاربری نامعلوم';
    });
  }

  Future<void> _checkUnreadEvents() async {
    final counts = await getUnreadCountsByType(widget.device['device_id']);
    if (!mounted) return;
    setState(() {
      _unread = counts;
    });
  }

  @override
  Widget build(BuildContext context) {
    final imagePath = widget.device['last_image'];
    return Card(
      color: Colors.white,
      margin: EdgeInsets.symmetric(vertical: 8, horizontal: 16),
      elevation: 4,
      // Clip children to the rounded shape so whichever banner is at the top is
      // rounded to match the card, and stacked banners stay flush (no white
      // corner gaps regardless of which/how many banners show).
      clipBehavior: Clip.antiAlias,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
      ),
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: widget.onTap,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            if (_isOffline) _offlineBanner(),
            if (_guestMode) _guestBanner(),
            // Pills + calls keep working in sleep/guest mode (only fall + camera
            // pause), so their banners must show in sleep too.
            if (_callPending) _callBanner(context),
            if (_pillPending) _pillDisclaimer(),
            Padding(
              padding: const EdgeInsets.all(12),
              child: Row(
                children: [
                  // 📸 device thumbnail + unread badge OVERLAID ON IT (so the red
                  // count never collides with the name/ID text in any direction).
                  Stack(
                    clipBehavior: Clip.none,
                    children: [
                      if (imagePath != null && imagePath.isNotEmpty)
                        Hero(
                          tag: imagePath,
                          child: ClipRRect(
                            borderRadius: BorderRadius.circular(8),
                            child: Image.file(
                              File(imagePath),
                              width: 80,
                              height: 80,
                              // cacheWidth/Height tells Flutter to decode AT the target
                              // size instead of full-res. 4k photo → 80x80 cached is
                              // ~16x less memory + ~5x faster paint. Critical for
                              // ListView scroll perf — used to OOM on Samsung A-series.
                              cacheWidth: 160, // 2x for hi-dpi
                              cacheHeight: 160,
                              fit: BoxFit.cover,
                              errorBuilder: (context, error, stackTrace) {
                                return Container(
                                  width: 80,
                                  height: 80,
                                  color: Colors.grey[200],
                                  child: Icon(Icons.broken_image,
                                      size: 40, color: Colors.grey),
                                );
                              },
                            ),
                          ),
                        )
                      else
                        Container(
                          width: 80,
                          height: 80,
                          decoration: BoxDecoration(
                            color: Colors.grey[200],
                            borderRadius: BorderRadius.circular(8),
                          ),
                          child: Icon(Icons.videocam,
                              size: 40, color: Colors.grey),
                        ),
                      // 🔴 unread falls (top corner) · 🔵 unread voice msgs (bottom).
                      if (_fall > 0)
                        Positioned(
                            top: -4,
                            right: -4,
                            child: _countBadge(_fall, Colors.red)),
                      if (_voice > 0)
                        Positioned(
                            bottom: -4,
                            right: -4,
                            child: _countBadge(_voice, Colors.blue)),
                    ],
                  ),
                  SizedBox(width: 16),

                  Expanded(
                    child: Stack(
                      children: [
                        Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Row(
                              children: [
                                Expanded(
                                  child: Text(
                                    // Show the friendly name; if it's unset or
                                    // equals the id (old blank-name fallback),
                                    // prompt the user to set one.
                                    (() {
                                      final n = widget.device['device_name']
                                          ?.toString();
                                      final id = widget.device['device_id']
                                          ?.toString();
                                      return (n == null || n.isEmpty || n == id)
                                          ? 'بدون نام (برای نام‌گذاری ✎ را بزنید)'
                                          : n;
                                    })(),
                                    // Explicit dark color: the card is white but
                                    // the app's dark theme defaults text to light,
                                    // so the name was rendering white-on-white
                                    // (invisible). The grey "ID:" line below showed
                                    // because it sets its own color.
                                    style: const TextStyle(
                                        fontWeight: FontWeight.bold,
                                        fontSize: 18,
                                        color: Colors.black87),
                                  ),
                                ),
                                IconButton(
                                  tooltip: _guestMode
                                      ? 'فعال‌سازی نظارت'
                                      : 'حالت خواب (توقف نظارت)',
                                  icon: Icon(
                                    _guestMode
                                        ? Icons.bedtime
                                        : Icons.bedtime_outlined,
                                    color: _guestMode
                                        ? const Color(0xFF455A64)
                                        : Colors.black54,
                                    size: 20,
                                  ),
                                  onPressed: _toggleGuest,
                                ),
                                IconButton(
                                  icon: Icon(
                                    Icons.edit,
                                    color: const Color.fromARGB(255, 0, 0, 0),
                                    size: 20,
                                  ),
                                  onPressed: () {
                                    _showEditNameDialog(context, widget.device,
                                        () => setState(() {}));
                                  },
                                ),
                              ],
                            ),
                            SizedBox(height: 4),
                            Padding(
                              padding: const EdgeInsets.all(8.0),
                              child: Text(
                                'ID: ${widget.device['device_id']}',
                                style: TextStyle(
                                    fontSize: 14, color: Colors.grey[600]),
                              ),
                            ),
                          ],
                        ),
                      ],
                    ),
                  ),
                  Icon(Icons.arrow_forward_ios, size: 20, color: Colors.grey),
                ],
              ),
            ),
            // Caregiver → device voice note (hold to record → ✓ send / ✗ discard).
            Padding(
              padding: const EdgeInsets.fromLTRB(12, 0, 12, 10),
              child: VoiceMessageButton(
                  deviceId: widget.device['device_id'].toString()),
            ),
          ],
        ),
      ),
    );
  }

  // Small circular count badge (red = falls, blue = voice messages).
  Widget _countBadge(int n, Color color) => Container(
        padding: const EdgeInsets.all(4),
        decoration: BoxDecoration(
          color: color,
          shape: BoxShape.circle,
          border: Border.all(color: Colors.white, width: 1.5),
        ),
        constraints: const BoxConstraints(minWidth: 20, minHeight: 20),
        child: Center(
          child: Text(n > 99 ? '99+' : '$n',
              style: const TextStyle(
                  color: Colors.white,
                  fontSize: 12,
                  fontWeight: FontWeight.bold)),
        ),
      );

  // Binary amber pill disclaimer: a due dose hasn't been taken. Informational
  // (not tappable) — it clears on the device when the elder takes the pill and
  // says the pill command (PillAck), reflected on the next status poll.
  // Device is in guest/sleep mode → detection paused. Make it unmistakable so a
  // caregiver never assumes falls are being watched while the device is asleep.
  // Device hasn't heartbeated to the server recently → it's offline (powered
  // off, crashed, or lost its network). Most important state on the card, so
  // it renders at the very top. Clears automatically on the next poll that
  // reports online==true.
  Widget _offlineBanner() => Container(
        color: const Color(0xFF616161), // grey = no connection
        padding: const EdgeInsets.symmetric(vertical: 8, horizontal: 12),
        child: Row(
          children: [
            const Icon(Icons.cloud_off, color: Colors.white, size: 20),
            const SizedBox(width: 8),
            Expanded(
              child: Text(
                  _lastSeen != null
                      ? 'دستگاه آفلاین است — آخرین اتصال: $_lastSeen'
                      : 'دستگاه آفلاین است',
                  style: const TextStyle(
                      color: Colors.white, fontWeight: FontWeight.bold)),
            ),
          ],
        ),
      );

  Widget _guestBanner() => Container(
        color: const Color(0xFF455A64), // slate grey = inactive/asleep
        padding: const EdgeInsets.symmetric(vertical: 8, horizontal: 12),
        child: const Row(
          children: [
            Icon(Icons.bedtime, color: Colors.white, size: 20),
            SizedBox(width: 8),
            Expanded(
              child: Text('حالت خواب — نظارت غیرفعال است',
                  style: TextStyle(
                      color: Colors.white, fontWeight: FontWeight.bold)),
            ),
          ],
        ),
      );

  Widget _pillDisclaimer() => Container(
        color: const Color(0xFFFFB300),
        padding: const EdgeInsets.symmetric(vertical: 6, horizontal: 12),
        child: const Row(
          children: [
            Icon(Icons.medication, color: Colors.black87, size: 18),
            SizedBox(width: 8),
            Expanded(
              child: Text('بیمار هنوز داروی موعد خود را مصرف نکرده است.',
                  style: TextStyle(
                      color: Colors.black87, fontWeight: FontWeight.bold)),
            ),
          ],
        ),
      );

  // Green "call requested" banner across the top of the card. Tapping it opens
  // the call screen directly and clears the pending-call flag for this device.
  Widget _callBanner(BuildContext context) {
    final id = widget.device['device_id']?.toString() ?? '';
    final name = (widget.device['device_name']?.toString().isNotEmpty ?? false)
        ? widget.device['device_name'].toString()
        : id;
    return Material(
      // No own corner radius — the Card's clipBehavior rounds the top banner and
      // keeps this flush when it sits below another banner (e.g. the sleep banner).
      color: const Color(0xFF2E7D32),
      child: InkWell(
        onTap: () async {
          await markTypeRead(id, 'call');
          if (mounted) _checkUnreadEvents();
          if (!context.mounted) return;
          Navigator.push(
            context,
            MaterialPageRoute(
              builder: (_) => CallPage(deviceId: id, deviceName: name),
            ),
          );
        },
        child: const Padding(
          padding: EdgeInsets.symmetric(vertical: 8, horizontal: 12),
          child: Row(
            children: [
              Icon(Icons.phone_in_talk, color: Colors.white, size: 20),
              SizedBox(width: 8),
              Expanded(
                child: Text('بیمار درخواست تماس کرده است، برای تماس لمس کنید.',
                    style: TextStyle(
                        color: Colors.white, fontWeight: FontWeight.bold)),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

void _showEditNameDialog(BuildContext context, Map<String, dynamic> device,
    [VoidCallback? onSaved]) {
  showDialog(
    context: context,
    // Anchor on the ROOT navigator so the dialog's element lifetime is not tied
    // to the (disposable) card context.
    useRootNavigator: true,
    builder: (context) => _EditNameDialog(device: device, onSaved: onSaved),
  );
}

/// Stateful so the [TextEditingController] is owned by a [State] and disposed in
/// [State.dispose] — which Flutter only calls AFTER the dialog route has fully
/// unmounted (exit transition complete). The previous version disposed the
/// controller in `showDialog(...).then(...)`, which fires the instant pop()
/// resolves while the fade-out is still rebuilding the TextField — re-attaching
/// a listener to the just-disposed controller ("TextEditingController used after
/// being disposed"), aborting the frame mid-rebuild and tripping the cascade
/// assertion `_dependents.isEmpty`. Owning it in State fixes the lifetime.
class _EditNameDialog extends StatefulWidget {
  const _EditNameDialog({required this.device, this.onSaved});

  final Map<String, dynamic> device;
  final VoidCallback? onSaved;

  @override
  State<_EditNameDialog> createState() => _EditNameDialogState();
}

class _EditNameDialogState extends State<_EditNameDialog> {
  late final TextEditingController _controller;
  bool _saving = false;

  @override
  void initState() {
    super.initState();
    final deviceId = widget.device['device_id']?.toString() ?? '';
    final current = widget.device['device_name']?.toString() ?? '';
    // If the stored name is just the id (the old blank-name fallback), start
    // empty so the user types a real display name instead of editing the id.
    _controller =
        TextEditingController(text: current == deviceId ? '' : current);
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  Future<void> _save() async {
    final newName = _controller.text.trim();
    if (newName.isEmpty) return;
    setState(() => _saving = true);
    // DB is the source of truth; updateDeviceName also pushes the refreshed list
    // onto DeviceStream, so the card updates itself — no in-place map mutation
    // (sqflite rows are read-only) and no manual parent refresh needed.
    await updateDeviceName(widget.device['device_id'], newName);
    if (!mounted) return;
    Navigator.of(context).pop();
  }

  @override
  Widget build(BuildContext context) {
    final deviceId = widget.device['device_id']?.toString() ?? '';
    return AlertDialog(
      title: const Text('ویرایش نام دستگاه'),
      content: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Device ID is fixed (set at provisioning) — show it read-only so
          // it's clear this dialog edits the friendly *name*, not the id.
          Text('شناسه دستگاه: $deviceId',
              style: const TextStyle(color: Colors.grey, fontSize: 13)),
          const SizedBox(height: 12),
          TextField(
            controller: _controller,
            autofocus: true,
            decoration: const InputDecoration(
                labelText: 'نام نمایشی', hintText: 'مثلاً اتاق مادربزرگ'),
          ),
        ],
      ),
      actions: [
        TextButton(
          onPressed: _saving ? null : () => Navigator.of(context).pop(),
          child: const Text('لغو', style: TextStyle(color: Colors.grey)),
        ),
        ElevatedButton(
          onPressed: _saving ? null : _save,
          child: const Text('ذخیره'),
        ),
      ],
    );
  }
}
