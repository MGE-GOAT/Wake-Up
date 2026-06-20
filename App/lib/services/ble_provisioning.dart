// Phase 7 — BLE provisioning client (companion side).
//
// Scans for the device's GATT service ("ElderlyCare-Setup"), connects,
// reads the Wi-Fi scan list, sends chosen credentials, then sends the
// final {device_id, password} commit. Mirrors the topic + payload shapes
// in device-side ble_provisioning.py.
//
// Usage pattern (called from setup_new_device_page.dart):
//   final svc = BleProvisioningService();
//   await svc.scanAndConnect();
//   final networks = await svc.scanWifi();
//   await svc.joinWifi(ssid, psk);
//   await svc.commitSetup(deviceId, password);
//   await svc.disconnect();
//
// Status updates are exposed via the `status` Stream so the UI can show
// "scanning", "connecting", "wifi_ok", etc. without polling.
import 'dart:async';
import 'dart:convert';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

/// One discovered provisioning device (for the picker). [name] is the
/// advertised "ElderlyCare-Setup-<serial>"; [rssi] lets us sort closest-first.
class DiscoveredDevice {
  final BluetoothDevice device;
  final String name;
  final int rssi;
  DiscoveredDevice({required this.device, required this.name, required this.rssi});
}

class BleProvisioningService {
  // Must match ble_provisioning.py's UUIDs byte-for-byte.
  static final Guid svcUuid       = Guid('11111111-2222-3333-4444-555555555555');
  static final Guid charWifiScan  = Guid('11111111-2222-3333-4444-555555555001');
  static final Guid charWifiJoin  = Guid('11111111-2222-3333-4444-555555555002');
  static final Guid charStatus    = Guid('11111111-2222-3333-4444-555555555003');
  static final Guid charCommit    = Guid('11111111-2222-3333-4444-555555555004');
  static final Guid charDone      = Guid('11111111-2222-3333-4444-555555555005');

  // Must match ble_provisioning.py's DEVICE_NAME prefix ("Noban-Setup-...").
  static const String devicePrefix = 'Noban';

  BluetoothDevice? _device;
  final Map<Guid, BluetoothCharacteristic> _chars = {};
  // GATT notification subscriptions — cancelled on re-bind + disconnect so a
  // reconnect doesn't stack duplicate listeners and a late notification can't
  // fire status.add() after the controller is closed ("add after close").
  StreamSubscription<List<int>>? _statusCharSub;
  StreamSubscription<List<int>>? _doneCharSub;

  /// Emits human-readable status messages — "scanning", "connecting", and
  /// every notification from the device's `status` characteristic
  /// ("connecting", "wifi_ok", "wifi_fail: …", etc.). UI can listen and
  /// render. Stream is closed on disconnect().
  final StreamController<String> status = StreamController.broadcast();

  /// Emits the device's `done` notification once `commitSetup` completes.
  /// Either "ok" or "err: ...".
  final StreamController<String> done = StreamController.broadcast();

  /// Scan for nearby devices advertising svcUuid, pick the first match,
  /// pair (Just Works), discover services, cache characteristic handles.
  /// Throws TimeoutException if no device found within `scanTimeout`.
  /// Scan for all in-range provisioning devices and return them sorted by
  /// signal strength (closest first). Each entry carries the advertised
  /// name ("ElderlyCare-Setup-<serial>") so the user can pick the right one
  /// when several are advertising. De-duplicates by remote id.
  Future<List<DiscoveredDevice>> scanForDevices({
    Duration scanTimeout = const Duration(seconds: 6),
  }) async {
    status.add('scanning');
    // De-dup by advertised NAME (now unique per device: ElderlyCare-Setup-
    // <serial>). The Pi advertises on two BT addresses (public + a rotating
    // random/RPA), so keying by remoteId would list one physical device
    // twice. Keying by name collapses them; keep the strongest-signal entry
    // (that's the address we'll actually connect to).
    final byKey = <String, DiscoveredDevice>{};
    final sub = FlutterBluePlus.scanResults.listen((results) {
      for (final r in results) {
        final adv = r.advertisementData.advName;
        // Only list adverts that carry the full serial form
        // "ElderlyCare-Setup-XXXX". This excludes (a) the adapter's own name
        // ("pi"), (b) UUID-only adverts with no/empty name, and (c) stale
        // pre-serial "ElderlyCare-Setup" adverts cached from earlier runs —
        // all of which previously showed up as a phantom second device.
        if (!adv.startsWith('$devicePrefix-')) continue;
        // Dedup by name (unique per device). The Pi advertises on two BT
        // addresses; keep the strongest-signal one — that's what we connect to.
        final existing = byKey[adv];
        if (existing == null || r.rssi > existing.rssi) {
          byKey[adv] = DiscoveredDevice(device: r.device, name: adv, rssi: r.rssi);
        }
      }
    });
    await FlutterBluePlus.startScan(withServices: [svcUuid], timeout: scanTimeout);
    await Future.delayed(scanTimeout);
    await sub.cancel();
    await FlutterBluePlus.stopScan();
    final list = byKey.values.toList()..sort((a, b) => b.rssi.compareTo(a.rssi));
    status.add(list.isEmpty ? 'no_devices' : 'found');
    return list;
  }

  /// Connect to a specific device chosen from [scanForDevices], discover the
  /// provisioning service + characteristics, and subscribe to notifications.
  Future<void> connectTo(BluetoothDevice device) async {
    _device = device;
    status.add('connecting');
    await _device!.connect(autoConnect: false, timeout: const Duration(seconds: 15));
    // NOTE: do NOT requestMtu() here. The writes (wifi_join ~42 B, commit
    // ~65 B) go through fine at the default MTU via ATT long writes, and
    // forcing an MTU change mid-session broke the wifi_scan characteristic
    // read (returned empty). The earlier "commit never arrived" was a UI
    // _busy bug, not an MTU problem.
    final services = await _device!.discoverServices();
    await _bindService(services);
  }

  /// Convenience: scan and connect to the strongest device (single-device
  /// homes). Throws TimeoutException if none found.
  Future<void> scanAndConnect({
    Duration scanTimeout = const Duration(seconds: 8),
  }) async {
    final found = await scanForDevices(scanTimeout: scanTimeout);
    if (found.isEmpty) {
      throw TimeoutException('no ElderlyCare-Setup device found');
    }
    await connectTo(found.first.device);
  }

  Future<void> _bindService(List<BluetoothService> services) async {
    final svc = services.firstWhere(
      (s) => s.uuid == svcUuid,
      orElse: () => throw StateError('provisioning service not found'),
    );
    for (final c in svc.characteristics) {
      _chars[c.uuid] = c;
    }

    // Subscribe to status + done notifications, forward into our streams.
    // Cancel any prior subscriptions first (a retried connect re-binds).
    await _statusCharSub?.cancel();
    await _doneCharSub?.cancel();
    final st = _chars[charStatus];
    if (st != null) {
      await st.setNotifyValue(true);
      _statusCharSub = st.lastValueStream.listen((bytes) {
        final s = utf8.decode(bytes, allowMalformed: true);
        if (s.isNotEmpty && !status.isClosed) status.add(s);
      });
    }
    final dn = _chars[charDone];
    if (dn != null) {
      await dn.setNotifyValue(true);
      _doneCharSub = dn.lastValueStream.listen((bytes) {
        final s = utf8.decode(bytes, allowMalformed: true);
        if (s.isNotEmpty && !done.isClosed) done.add(s);
      });
    }
    status.add('connected');
  }

  /// Read the Wi-Fi scan characteristic and parse the JSON list of
  /// {ssid, signal, security} maps. Sorted by signal strength descending.
  Future<List<Map<String, dynamic>>> scanWifi() async {
    final c = _chars[charWifiScan];
    if (c == null) throw StateError('not connected');
    final bytes = await c.read();
    try {
      final j = jsonDecode(utf8.decode(bytes));
      if (j is List) return j.cast<Map<String, dynamic>>();
    } catch (_) {}
    return const [];
  }

  /// Write the chosen Wi-Fi credentials. The device's status characteristic
  /// will emit "connecting" then either "wifi_ok" or "wifi_fail: ...".
  Future<void> joinWifi(String ssid, String psk) async {
    final c = _chars[charWifiJoin];
    if (c == null) throw StateError('not connected');
    final body = utf8.encode(jsonEncode({'ssid': ssid, 'psk': psk}));
    await c.write(body, withoutResponse: false);
  }

  /// Write the final device-id + 32+ char password. The device's `done`
  /// characteristic emits "ok" or "err: ...". Device will exit the sidecar
  /// shortly after.
  Future<void> commitSetup(String deviceId, String password) async {
    final c = _chars[charCommit];
    if (c == null) throw StateError('not connected');
    final body = utf8.encode(jsonEncode({
      'device_id': deviceId,
      'password':  password,
    }));
    await c.write(body, withoutResponse: false);
  }

  Future<void> disconnect() async {
    // Cancel notification subs BEFORE closing controllers so a late
    // notification can't call add() on a closed controller.
    await _statusCharSub?.cancel();
    await _doneCharSub?.cancel();
    _statusCharSub = null;
    _doneCharSub = null;
    try { await _device?.disconnect(); } catch (_) {}
    _device = null;
    _chars.clear();
    if (!status.isClosed) await status.close();
    if (!done.isClosed) await done.close();
  }
}
