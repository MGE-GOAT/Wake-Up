// Change the Wi-Fi of an ALREADY-registered device, over BLE, WITHOUT a factory
// reset (the device keeps its identity). Flow:
//   1. Pick which of your devices to move to a new network.
//   2. Put that device in setup mode (hold its button ~3s → LED blue), scan BLE,
//      pick it.
//   3. Pick the new Wi-Fi + password → joinWifi. On wifi_ok we commit with the
//      device's EXISTING id+password (from DeviceSecretStore) so it reconnects
//      under the same identity — no re-registration on the server.
//
// Reuses BleProvisioningService (same as adding a device); the only difference
// vs setup is we don't generate/register new credentials.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import 'db.dart';
import 'services/ble_provisioning.dart';
import 'services/secure_storage.dart';

class SwitchWifiPage extends StatefulWidget {
  const SwitchWifiPage({super.key});

  @override
  State<SwitchWifiPage> createState() => _SwitchWifiPageState();
}

class _SwitchWifiPageState extends State<SwitchWifiPage> {
  int _step = 0; // 0 pick device · 1 scan/pick BLE · 2 pick wifi · 3 done

  final _ble = BleProvisioningService();
  StreamSubscription? _bleStatusSub;
  StreamSubscription? _bleDoneSub;

  bool _busy = false;
  String? _error;
  String _bleStatus = '';

  List<Map<String, dynamic>> _devices = [];
  String? _deviceId;
  String? _devicePw; // existing device password (kept → identity preserved)

  List<DiscoveredDevice> _discovered = [];
  List<Map<String, dynamic>> _networks = [];
  String? _selectedSsid;
  String _selectedSecurity = '';
  final _wifiPwController = TextEditingController();

  @override
  void initState() {
    super.initState();
    _loadDevices();
    _bleStatusSub = _ble.status.stream.listen((s) {
      if (!mounted) return;
      setState(() => _bleStatus = s);
      if (s == 'wifi_ok' && _step == 2) {
        // Wi-Fi joined → finalize by re-committing the EXISTING identity.
        _commitExistingIdentity();
      } else if (s.startsWith('wifi_fail') && _step == 2) {
        setState(() {
          _busy = false;
          _error =
              "اتصال Wi-Fi ناموفق: ${s.substring(s.indexOf(':') + 1).trim()}";
        });
      }
    });
  }

  @override
  void dispose() {
    _bleStatusSub?.cancel();
    _bleDoneSub?.cancel();
    _ble.disconnect();
    _wifiPwController.dispose();
    super.dispose();
  }

  Future<void> _loadDevices() async {
    final devs = await getDevices();
    if (!mounted) return;
    setState(() => _devices = devs);
  }

  // ── Step 0: choose which registered device + load its stored credentials ──
  Future<void> _pickDevice(String id) async {
    final pw = await DeviceSecretStore.read(id);
    if (!mounted) return;
    if (pw == null || pw.isEmpty) {
      setState(() => _error =
          "رمز این دستگاه روی این گوشی ذخیره نشده است؛ تغییر شبکه بدون آن ممکن نیست (از گوشیِ زمان ثبت استفاده کنید).");
      return;
    }
    setState(() {
      _deviceId = id;
      _devicePw = pw;
      _error = null;
      _step = 1;
    });
  }

  // ── Step 1: BLE scan + connect ───────────────────────────────────────────
  Future<bool> _ensureBlePerms() async {
    await [Permission.bluetoothScan, Permission.bluetoothConnect].request();
    final ok = await Permission.bluetoothScan.isGranted &&
        await Permission.bluetoothConnect.isGranted;
    if (ok) return true;
    final loc = await Permission.location.request();
    return loc.isGranted;
  }

  Future<void> _scanDevices() async {
    if (!await _ensureBlePerms()) {
      setState(() => _error =
          "دسترسی به Bluetooth داده نشد. روشن بودن Bluetooth و دسترسی برنامه را بررسی کنید.");
      return;
    }
    setState(() {
      _busy = true;
      _error = null;
      _discovered = [];
    });
    try {
      final found = await _ble.scanForDevices();
      if (!mounted) return;
      setState(() {
        _busy = false;
        _discovered = found;
        if (found.isEmpty) {
          _error =
              "دستگاهی یافت نشد. دکمه دستگاه را ۳ ثانیه نگه دارید تا چراغ آبی شود، سپس دوباره جستجو کنید.";
        }
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _busy = false;
        _error = "خطا در جستجو: $e";
      });
    }
  }

  Future<void> _connectToDevice(DiscoveredDevice d) async {
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      await _ble.connectTo(d.device);
      final nets = await _ble.scanWifi();
      if (!mounted) return;
      setState(() {
        _busy = false;
        _networks = nets;
        _step = 2;
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _busy = false;
        _error = "خطا در اتصال: $e";
      });
    }
  }

  // ── Step 2: send the new Wi-Fi ───────────────────────────────────────────
  Future<void> _submitWifi() async {
    if (_selectedSsid == null) {
      setState(() => _error = "یک شبکه انتخاب کنید");
      return;
    }
    final needsPw =
        !_selectedSecurity.contains('--') && _selectedSecurity.isNotEmpty;
    final pw = _wifiPwController.text;
    if (needsPw && pw.isEmpty) {
      setState(() => _error = "رمز Wi-Fi را وارد کنید");
      return;
    }
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      await _ble.joinWifi(_selectedSsid!, pw); // → status 'wifi_ok'/'wifi_fail'
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _busy = false;
        _error = "خطا: $e";
      });
    }
  }

  // ── Commit the EXISTING identity (keeps id.txt creds → no re-register) ────
  Future<void> _commitExistingIdentity() async {
    // Re-entrancy guard: some Android BLE stacks deliver 'wifi_ok' twice. Without
    // this, a second call cancels the first's _bleDoneSub mid-await, so the first
    // Completer never resolves → spurious 15s timeout error even though setup
    // succeeded. _busy is set synchronously below, so the second event bails here.
    if (_busy) return;
    final id = _deviceId, pw = _devicePw;
    if (id == null || pw == null) return;
    setState(() {
      _busy = true;
      _error = null;
    });
    final done = Completer<String>();
    _bleDoneSub?.cancel();
    _bleDoneSub = _ble.done.stream.listen((d) {
      if (!done.isCompleted) done.complete(d);
    });
    try {
      await _ble.commitSetup(id, pw);
      final result = await done.future.timeout(const Duration(seconds: 15));
      if (!mounted) return;
      if (!result.startsWith('ok')) {
        setState(() {
          _busy = false;
          _error = "دستگاه پاسخ خطا داد: $result";
        });
        return;
      }
      setState(() {
        _busy = false;
        _step = 3;
      });
    } on TimeoutException {
      if (!mounted) return;
      setState(() {
        _busy = false;
        _error = "دستگاه پاسخ نداد. اتصال BLE را بررسی کنید.";
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _busy = false;
        _error = "خطا: $e";
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return Directionality(
      textDirection: TextDirection.rtl,
      child: Scaffold(
        appBar: AppBar(title: const Text('تغییر شبکه دستگاه')),
        body: Padding(
          padding: const EdgeInsets.all(16),
          child: _busy && _step != 3
              ? Center(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      const CircularProgressIndicator(),
                      const SizedBox(height: 12),
                      Text(_bleStatus.isEmpty ? 'لطفاً صبر کنید…' : _bleStatus),
                    ],
                  ),
                )
              : _buildStep(),
        ),
      ),
    );
  }

  Widget _buildStep() {
    final err = _error == null
        ? const SizedBox.shrink()
        : Padding(
            padding: const EdgeInsets.only(top: 12),
            child: Text(_error!, style: const TextStyle(color: Colors.red)),
          );
    switch (_step) {
      case 0:
        return ListView(children: [
          const Text('۱) کدام دستگاه به شبکه جدید منتقل شود؟',
              style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
          const SizedBox(height: 8),
          if (_devices.isEmpty)
            const Text('دستگاهی ثبت نشده است.')
          else
            ..._devices.map((d) {
              final id = d['device_id'].toString();
              final name = (d['device_name']?.toString().isNotEmpty ?? false)
                  ? d['device_name'].toString()
                  : id;
              return Card(
                child: ListTile(
                  title: Text(name),
                  subtitle: Text('شناسه: $id'),
                  trailing: const Icon(Icons.chevron_left),
                  onTap: () => _pickDevice(id),
                ),
              );
            }),
          err,
        ]);
      case 1:
        return ListView(children: [
          const Text('۲) دستگاه را در حالت تنظیم قرار دهید',
              style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
          const SizedBox(height: 8),
          const Text(
              'دکمهٔ دستگاه را حدود ۳ ثانیه نگه دارید تا چراغ آبی روشن شود، سپس جستجو کنید.'),
          const SizedBox(height: 12),
          ElevatedButton.icon(
            icon: const Icon(Icons.bluetooth_searching),
            label: const Text('جستجوی دستگاه'),
            onPressed: _scanDevices,
          ),
          const SizedBox(height: 8),
          ..._discovered.map((d) => Card(
                child: ListTile(
                  leading: const Icon(Icons.memory),
                  title: Text(d.name),
                  subtitle: Text('قدرت سیگنال: ${d.rssi}'),
                  onTap: () => _connectToDevice(d),
                ),
              )),
          err,
        ]);
      case 2:
        final needsPw = !_selectedSecurity.contains('--') &&
            _selectedSecurity.isNotEmpty;
        return ListView(children: [
          const Text('۳) شبکهٔ جدید را انتخاب کنید',
              style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
          const SizedBox(height: 8),
          ..._networks.map((n) {
            final ssid = (n['ssid'] ?? '').toString();
            final sec = (n['security'] ?? '').toString();
            return RadioListTile<String>(
              value: ssid,
              groupValue: _selectedSsid,
              title: Text(ssid.isEmpty ? '(شبکهٔ مخفی)' : ssid),
              subtitle: Text('${n['signal'] ?? ''}  ·  ${sec.isEmpty ? 'باز' : sec}'),
              onChanged: (v) => setState(() {
                _selectedSsid = v;
                _selectedSecurity = sec;
              }),
            );
          }),
          if (needsPw)
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 8),
              child: TextField(
                controller: _wifiPwController,
                obscureText: true,
                decoration: const InputDecoration(
                    labelText: 'رمز Wi-Fi', border: OutlineInputBorder()),
              ),
            ),
          ElevatedButton(
            onPressed: _submitWifi,
            child: const Text('اتصال به شبکهٔ جدید'),
          ),
          err,
        ]);
      default: // 3 — done
        return Center(
          child: Column(mainAxisSize: MainAxisSize.min, children: [
            const Icon(Icons.check_circle, color: Colors.green, size: 72),
            const SizedBox(height: 12),
            const Text('شبکهٔ دستگاه با موفقیت تغییر کرد.',
                style: TextStyle(fontSize: 16)),
            const SizedBox(height: 4),
            const Text('دستگاه روی شبکهٔ جدید راه‌اندازی می‌شود.',
                style: TextStyle(color: Colors.grey)),
            const SizedBox(height: 20),
            ElevatedButton(
              onPressed: () => Navigator.of(context).pop(),
              child: const Text('پایان'),
            ),
          ]),
        );
    }
  }
}
