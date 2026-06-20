// First-time device onboarding via BLE.
//
// Wizard steps:
//   0. instructions — "press button on device for 3 sec until blue LED"
//   1. scan + connect — flutter_blue_plus scans for "ElderlyCare-Setup",
//      pairs (Just Works), discovers GATT
//   2. Wi-Fi scan + pick + password — companion reads `wifi_scan` char,
//      user picks + types password, writes `wifi_join`. Waits for status
//      notification: wifi_ok | wifi_fail
//   3. credentials — user picks device-id + 32+ char password (or accepts
//      generated default). On submit:
//        - companion writes `commit_setup` over BLE → device writes id.txt
//        - companion POSTs /Register_Device to the server with the same creds
//        - both must succeed for the device to be usable
//   4. success — back to dashboard
//
// Falls back gracefully if BLE permissions denied / no device found, with
// a manual "type ID + password" path for advanced users who can SSH into
// the Pi and write id.txt themselves.
import 'dart:async';
import 'dart:math';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'configs.dart';
import 'db.dart';
import 'services/ble_provisioning.dart';
import 'services/device_api.dart';
import 'services/secure_storage.dart';

class SetupNewDevicePage extends StatefulWidget {
  const SetupNewDevicePage({super.key});

  @override
  State<SetupNewDevicePage> createState() => _SetupNewDevicePageState();
}

class _SetupNewDevicePageState extends State<SetupNewDevicePage> {
  int _step = 0;

  final _ble = BleProvisioningService();
  StreamSubscription? _bleStatusSub;
  StreamSubscription? _bleDoneSub;

  bool _busy = false;
  String? _error;
  String _bleStatus = '';

  List<Map<String, dynamic>> _networks = [];
  List<DiscoveredDevice> _discovered = [];
  String? _selectedSsid;
  String _selectedSecurity = '';
  final _wifiPwController = TextEditingController();

  final _idController       = TextEditingController();
  final _passwordController = TextEditingController();
  final _nameController     = TextEditingController();
  bool _hidePassword = false;

  @override
  void initState() {
    super.initState();
    _passwordController.text = _generateStrongPassword();
    _bleStatusSub = _ble.status.stream.listen((s) {
      if (!mounted) return;
      setState(() => _bleStatus = s);
      // Advance from "join wifi" step once we see wifi_ok. Critically, clear
      // _busy here: _submitWifi set it true and waits on this async status
      // event to advance — without clearing it, step 3's Confirm button stays
      // disabled/spinning forever (the "stuck after network login" bug).
      if (s == 'wifi_ok' && _step == 2) {
        setState(() { _step = 3; _busy = false; _error = null; });
      } else if (s.startsWith('wifi_fail') && _step == 2) {
        // Surface the failure and re-enable the Wi-Fi form to retry.
        setState(() { _busy = false; _error = "اتصال Wi-Fi ناموفق: ${s.substring(s.indexOf(':') + 1).trim()}"; });
      }
    });
  }

  @override
  void dispose() {
    _bleStatusSub?.cancel();
    _bleDoneSub?.cancel();
    _ble.disconnect();
    _idController.dispose();
    _passwordController.dispose();
    _nameController.dispose();
    _wifiPwController.dispose();
    super.dispose();
  }

  /// 32-char URL-safe-ish random string. ~190 bits entropy.
  String _generateStrongPassword() {
    const charset =
        'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
    final r = Random.secure();
    return List.generate(32, (_) => charset[r.nextInt(charset.length)]).join();
  }

  // ── Step 1: scan (list devices) then connect to the chosen one ─────────
  // Android 12+ uses BLUETOOTH_SCAN + BLUETOOTH_CONNECT. Because the manifest
  // declares BLUETOOTH_SCAN with neverForLocation, ACCESS_FINE_LOCATION is
  // NOT required and reports "denied" on 12+ — so we must NOT gate on it.
  Future<bool> _ensureBlePerms() async {
    await [Permission.bluetoothScan, Permission.bluetoothConnect].request();
    final ok = await Permission.bluetoothScan.isGranted &&
               await Permission.bluetoothConnect.isGranted;
    if (ok) return true;
    final loc = await Permission.location.request();   // legacy <12 fallback
    return loc.isGranted;
  }

  Future<void> _scanDevices() async {
    if (!await _ensureBlePerms()) {
      setState(() => _error =
          "دسترسی به Bluetooth داده نشد. در تنظیمات گوشی، دسترسی Bluetooth برنامه و روشن بودن Bluetooth را بررسی کنید.");
      return;
    }
    setState(() { _busy = true; _error = null; _discovered = []; });
    try {
      final found = await _ble.scanForDevices();
      if (!mounted) return;
      setState(() {
        _busy = false;
        _discovered = found;
        if (found.isEmpty) {
          _error = "دستگاهی یافت نشد. دکمه را ۳ ثانیه نگه دارید تا چراغ آبی روشن شود، سپس دوباره جستجو کنید.";
        }
      });
    } catch (e) {
      setState(() { _busy = false; _error = "خطا در جستجو: $e"; });
    }
  }

  Future<void> _connectToDevice(DiscoveredDevice d) async {
    setState(() { _busy = true; _error = null; });
    try {
      await _ble.connectTo(d.device);
      final nets = await _ble.scanWifi();
      if (!mounted) return;
      setState(() { _busy = false; _networks = nets; _step = 2; });
    } catch (e) {
      setState(() { _busy = false; _error = "خطا در اتصال: $e"; });
    }
  }

  // ── Step 2 (action): submit Wi-Fi creds ────────────────────────────────
  Future<void> _submitWifi() async {
    if (_selectedSsid == null) {
      setState(() => _error = "یک شبکه انتخاب کنید");
      return;
    }
    final needsPw = !_selectedSecurity.contains('--') && _selectedSecurity.isNotEmpty;
    final pw = _wifiPwController.text;
    if (needsPw && pw.isEmpty) {
      setState(() => _error = "رمز Wi-Fi را وارد کنید");
      return;
    }
    setState(() { _busy = true; _error = null; });
    try {
      await _ble.joinWifi(_selectedSsid!, pw);
      // Step 2 → Step 3 transition happens automatically when status stream
      // emits "wifi_ok" (see initState listener). If it emits "wifi_fail",
      // we surface it as an error.
    } catch (e) {
      setState(() { _busy = false; _error = "خطا: $e"; });
    }
  }

  // ── Step 3: commit id+password → BLE + server ──────────────────────────
  Future<void> _commitCredentials() async {
    final id   = _idController.text.trim();
    final pw   = _passwordController.text;
    final name = _nameController.text.trim();
    if (id.isEmpty) {
      setState(() => _error = "شناسه دستگاه را وارد کنید"); return;
    }
    if (pw.length < kDevicePasswordMinChars) {
      setState(() => _error =
          "رمز باید حداقل $kDevicePasswordMinChars کاراکتر باشد"); return;
    }
    setState(() { _busy = true; _error = null; });

    // (1) write to device over BLE first — if device can't persist, abort
    // before touching the server. Listen for `done` notification.
    final doneCompleter = Completer<String>();
    _bleDoneSub?.cancel();
    _bleDoneSub = _ble.done.stream.listen((d) {
      if (!doneCompleter.isCompleted) doneCompleter.complete(d);
    });

    try {
      await _ble.commitSetup(id, pw);
      final result = await doneCompleter.future
          .timeout(const Duration(seconds: 15));
      if (!mounted) return;
      if (!result.startsWith('ok')) {
        setState(() {
          _busy = false;
          _error = "دستگاه پاسخ خطا داد: $result";
        });
        return;
      }
    } on TimeoutException {
      setState(() {
        _busy = false;
        _error = "دستگاه پاسخ نداد. اتصال BLE را بررسی کنید.";
      });
      return;
    } catch (e) {
      setState(() { _busy = false; _error = "BLE error: $e"; });
      return;
    }

    // (2) device has id.txt now — register it on the server
    final creds = await AuthStore.creds();
    final username = creds?.username;
    final password = creds?.password;
    if (username == null || password == null) {
      setState(() { _busy = false; _error = "ابتدا وارد حساب کاربری شوید"; });
      return;
    }
    final api = DeviceApi(username: username, password: password);
    final res = await api.registerDevice(deviceId: id, devicePassword: pw);
    if (!mounted) return;
    switch (res) {
      case ApiOk():
        // Store the friendly name as-is (empty if the user skipped it). The
        // device list shows the id as a fallback + prompts to name it — we no
        // longer copy the id into the name field (that made the edit pencil
        // look like it was editing the id).
        await insertDevice(id, pw, name);
        await DeviceSecretStore.save(id, pw);
        await _ble.disconnect();
        setState(() { _busy = false; _step = 4; });
      case ApiError(error: final e, status: final s):
        setState(() {
          _busy = false;
          _error = e == 'device_id_taken'
              ? "این شناسه قبلاً ثبت شده — یک شناسه دیگر انتخاب کنید"
              : "خطای سرور ($s): $e";
        });
      case ApiDeviceFull():
        setState(() { _busy = false; _error = "ناسازگاری"; });
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      appBar: AppBar(
        backgroundColor: Colors.green,
        title: const Text("راه‌اندازی دستگاه جدید",
            style: TextStyle(
                fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold)),
        toolbarHeight: 90.0,
      ),
      body: Padding(
        padding: const EdgeInsets.all(20),
        child: _bodyForStep(),
      ),
    );
  }

  Widget _bodyForStep() {
    switch (_step) {
      case 0: return _step0Instructions();
      case 1: return _step1Connect();
      case 2: return _step2WifiPicker();
      case 3: return _step3Credentials();
      case 4: return _step4Success();
    }
    return const SizedBox();
  }

  // ── Step 0 — instructions ─────────────────────────────────────────────
  Widget _step0Instructions() {
    return Column(
      children: [
        const Icon(Icons.touch_app, size: 80, color: Colors.white70),
        const SizedBox(height: 20),
        const Text(
          "دکمه دستگاه را به مدت ۳ ثانیه نگه دارید تا چراغ آبی روشن شود — یعنی دستگاه آماده اتصال بلوتوث است. سپس روی «بعدی» بزنید.",
          style: TextStyle(fontSize: 16, color: Colors.white),
          textAlign: TextAlign.center,
        ),
        const Spacer(),
        ElevatedButton(
          onPressed: () => setState(() => _step = 1),
          style: ElevatedButton.styleFrom(
            backgroundColor: Colors.green,
            minimumSize: const Size.fromHeight(50),
          ),
          child: const Text("بعدی", style: TextStyle(fontSize: 18, color: Colors.white)),
        ),
      ],
    );
  }

  // ── Step 1 — scan + connect ────────────────────────────────────────────
  Widget _step1Connect() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        const Text(
          "دستگاه‌های در دسترس را جستجو کنید و دستگاه خود را انتخاب کنید:",
          style: TextStyle(color: Colors.white, fontSize: 15),
        ),
        const SizedBox(height: 12),
        ElevatedButton.icon(
          onPressed: _busy ? null : _scanDevices,
          icon: const Icon(Icons.bluetooth_searching, color: Colors.white),
          label: Text(_busy ? "در حال جستجو..." : "جستجوی دستگاه",
              style: const TextStyle(color: Colors.white)),
          style: ElevatedButton.styleFrom(
            backgroundColor: Colors.blue,
            minimumSize: const Size.fromHeight(50),
          ),
        ),
        const SizedBox(height: 12),
        Expanded(
          child: _busy
              ? const Center(child: CircularProgressIndicator(color: Colors.white))
              : _discovered.isEmpty
                  ? const Center(child: Text("هنوز دستگاهی پیدا نشده",
                      style: TextStyle(color: Colors.white54)))
                  : ListView.separated(
                      itemCount: _discovered.length,
                      separatorBuilder: (_, __) => const SizedBox(height: 6),
                      itemBuilder: (_, i) {
                        final d = _discovered[i];
                        return Material(
                          color: Colors.white,
                          borderRadius: BorderRadius.circular(8),
                          child: ListTile(
                            leading: Icon(
                              d.rssi >= -60 ? Icons.bluetooth_connected
                                            : Icons.bluetooth,
                              color: Colors.blue),
                            title: Text(d.name,
                                style: const TextStyle(
                                    color: Colors.black, fontWeight: FontWeight.bold)),
                            subtitle: Text("قدرت سیگنال: ${d.rssi} dBm",
                                style: const TextStyle(color: Colors.grey)),
                            trailing: const Icon(Icons.chevron_left, color: Colors.green),
                            onTap: () => _connectToDevice(d),
                          ),
                        );
                      },
                    ),
        ),
        if (_error != null) _errBox(_error!),
      ],
    );
  }

  // ── Step 2 — Wi-Fi picker ──────────────────────────────────────────────
  Widget _step2WifiPicker() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        const Text("یک شبکه Wi-Fi انتخاب کنید:",
            style: TextStyle(color: Colors.white, fontWeight: FontWeight.bold)),
        const SizedBox(height: 8),
        Expanded(
          child: _networks.isEmpty
              ? const Center(child: Text("شبکه‌ای یافت نشد",
                  style: TextStyle(color: Colors.white70)))
              : ListView.separated(
                  itemCount: _networks.length,
                  separatorBuilder: (_, __) => const SizedBox(height: 6),
                  itemBuilder: (_, i) {
                    final n = _networks[i];
                    // signal can arrive as num/double from the device JSON —
                    // `as int` would throw and kill the Wi-Fi picker.
                    final ssid = n['ssid']?.toString() ?? '';
                    final sig  = (n['signal'] as num?)?.toInt() ?? 0;
                    final sec  = (n['security'] as String?) ?? '';
                    final selected = ssid == _selectedSsid;
                    return Material(
                      color: selected ? Colors.green.shade700 : Colors.white,
                      borderRadius: BorderRadius.circular(8),
                      child: ListTile(
                        leading: Icon(
                          sig >= 70 ? Icons.signal_wifi_4_bar
                                    : sig >= 40 ? Icons.network_wifi
                                                : Icons.network_wifi_3_bar,
                          color: selected ? Colors.white : Colors.green,
                        ),
                        title: Text(ssid,
                            style: TextStyle(
                                color: selected ? Colors.white : Colors.black,
                                fontWeight: FontWeight.bold)),
                        subtitle: Text("سیگنال $sig٪  •  $sec",
                            style: TextStyle(
                                color: selected ? Colors.white70 : Colors.grey)),
                        onTap: () => setState(() {
                          _selectedSsid = ssid;
                          _selectedSecurity = sec;
                          _wifiPwController.clear();
                        }),
                      ),
                    );
                  },
                ),
        ),
        if (_selectedSsid != null) ...[
          const SizedBox(height: 14),
          TextField(
            controller: _wifiPwController,
            obscureText: true,
            style: const TextStyle(color: Colors.white),
            decoration: const InputDecoration(
              labelText: "رمز Wi-Fi (اگر دارد)",
              labelStyle: TextStyle(color: Colors.white70),
            ),
          ),
        ],
        if (_bleStatus.startsWith('wifi_fail') || _bleStatus == 'connecting')
          Padding(
            padding: const EdgeInsets.only(top: 8),
            child: Text("وضعیت: $_bleStatus",
                style: const TextStyle(color: Colors.white70)),
          ),
        if (_error != null) _errBox(_error!),
        const SizedBox(height: 8),
        ElevatedButton(
          onPressed: _busy ? null : _submitWifi,
          style: ElevatedButton.styleFrom(
            backgroundColor: Colors.green,
            minimumSize: const Size.fromHeight(50),
          ),
          child: _busy
              ? const SizedBox(width: 22, height: 22,
                  child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white))
              : const Text("اتصال به Wi-Fi",
                  style: TextStyle(color: Colors.white, fontSize: 16)),
        ),
      ],
    );
  }

  // ── Step 3 — credentials (id + password) ───────────────────────────────
  Widget _step3Credentials() {
    return SingleChildScrollView(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          const Text(
            "دستگاه به Wi-Fi متصل شد ✓\nحالا شناسه و رمز انتخابی خود را وارد کنید.",
            style: TextStyle(color: Colors.white, fontSize: 14),
          ),
          const SizedBox(height: 16),
          TextField(
            controller: _idController,
            style: const TextStyle(color: Colors.white),
            decoration: const InputDecoration(
              labelText: "شناسه دستگاه (مثلاً home-1)",
              labelStyle: TextStyle(color: Colors.white70),
            ),
          ),
          const SizedBox(height: 14),
          Row(
            children: [
              Expanded(
                child: TextField(
                  controller: _passwordController,
                  obscureText: _hidePassword,
                  style: const TextStyle(color: Colors.white),
                  decoration: InputDecoration(
                    labelText: "رمز ۳۲ کاراکتری",
                    labelStyle: const TextStyle(color: Colors.white70),
                    suffixIcon: IconButton(
                      icon: Icon(_hidePassword
                          ? Icons.visibility : Icons.visibility_off,
                          color: Colors.white70),
                      onPressed: () =>
                          setState(() => _hidePassword = !_hidePassword),
                    ),
                  ),
                ),
              ),
              IconButton(
                icon: const Icon(Icons.refresh, color: Colors.white),
                onPressed: () => setState(() =>
                    _passwordController.text = _generateStrongPassword()),
              ),
            ],
          ),
          const SizedBox(height: 14),
          TextField(
            controller: _nameController,
            style: const TextStyle(color: Colors.white),
            decoration: const InputDecoration(
              labelText: "نام نمایشی (اختیاری)",
              labelStyle: TextStyle(color: Colors.white70),
            ),
          ),
          const SizedBox(height: 18),
          if (_error != null) _errBox(_error!),
          ElevatedButton(
            onPressed: _busy ? null : _commitCredentials,
            style: ElevatedButton.styleFrom(
              backgroundColor: Colors.green,
              padding: const EdgeInsets.symmetric(vertical: 16),
            ),
            child: _busy
                ? const SizedBox(width: 22, height: 22,
                    child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white))
                : const Text("ثبت دستگاه",
                    style: TextStyle(color: Colors.white, fontSize: 18,
                        fontWeight: FontWeight.bold)),
          ),
        ],
      ),
    );
  }

  // ── Step 4 — success ───────────────────────────────────────────────────
  Widget _step4Success() {
    return Column(
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        const Icon(Icons.check_circle, size: 80, color: Colors.green),
        const SizedBox(height: 20),
        Text(
          "دستگاه «${_nameController.text.isEmpty ? _idController.text : _nameController.text}» با موفقیت ثبت شد",
          style: const TextStyle(color: Colors.white, fontSize: 18),
          textAlign: TextAlign.center,
        ),
        const SizedBox(height: 20),
        const Text(
          "رمز را در جایی امن نگه دارید — برای اشتراک‌گذاری با اعضای خانواده (حداکثر ۳ نفر) به آن نیاز دارند.",
          style: TextStyle(color: Colors.white70, fontSize: 14),
          textAlign: TextAlign.center,
        ),
        const SizedBox(height: 30),
        ElevatedButton(
          onPressed: () => Navigator.popUntil(context, (r) => r.isFirst),
          style: ElevatedButton.styleFrom(
            backgroundColor: Colors.green,
            minimumSize: const Size.fromHeight(50),
          ),
          child: const Text("بازگشت به صفحه اصلی",
              style: TextStyle(color: Colors.white, fontSize: 18)),
        ),
      ],
    );
  }

  Widget _errBox(String msg) => Container(
        margin: const EdgeInsets.symmetric(vertical: 12),
        padding: const EdgeInsets.all(12),
        decoration: BoxDecoration(
          color: Colors.red.shade800,
          borderRadius: BorderRadius.circular(8),
        ),
        child: Text(msg, style: const TextStyle(color: Colors.white)),
      );
}
