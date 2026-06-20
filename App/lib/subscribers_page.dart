// Manage the list of subscribers for one device.
//
// Reached from DeviceHubPage. Shows the current 1-3 subscribers. The
// calling user can:
//   • leave the device themselves (/Unsubscribe_Device)
//   • kick another subscriber (/Replace_Subscriber with replace_target = X,
//     new_username = self — but since they're already subscribed, the
//     server will reject with "already_subscribed"; the proper kick UX
//     for an existing subscriber is "remove only" which is /Unsubscribe
//     on behalf of someone else, currently not modeled — see note below)
//
// Note: the current server contract only supports a NEW user kicking an
// EXISTING one (via Replace_Subscriber). For an existing subscriber to
// boot another, we'd need a separate /Boot_Subscriber endpoint. Out of
// scope for Phase 2; flag if you want it added.
import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'db.dart';
import 'services/device_api.dart';
import 'services/secure_storage.dart';

class SubscribersPage extends StatefulWidget {
  final String deviceId;
  const SubscribersPage({super.key, required this.deviceId});

  @override
  State<SubscribersPage> createState() => _SubscribersPageState();
}

class _SubscribersPageState extends State<SubscribersPage> {
  List<Map<String, dynamic>>? _subscribers;
  String? _error;
  String? _selfUsername;
  bool _leaving = false;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final creds = await AuthStore.creds();
    final username = creds?.username;
    final password = creds?.password;
    if (username == null || password == null) {
      setState(() => _error = "ابتدا وارد حساب کاربری شوید");
      return;
    }
    _selfUsername = username;

    final api = DeviceApi(username: username, password: password);
    final res = await api.deviceSubscribers(deviceId: widget.deviceId);
    if (!mounted) return;

    switch (res) {
      case ApiOk(body: final b):
        setState(() {
          _subscribers = (b['subscribers'] as List? ?? [])
              .cast<Map<String, dynamic>>();
        });
      case ApiError(error: final e):
        setState(() => _error = "خطا: $e");
      case ApiDeviceFull():
        setState(() => _error = "ناسازگاری در پاسخ سرور");
    }
  }

  Future<void> _leaveDevice() async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (_) => AlertDialog(
        title: const Text("خروج از دستگاه"),
        content: const Text(
            "از فهرست مشترکین این دستگاه حذف خواهید شد و دیگر رویدادهای آن را دریافت نمی‌کنید. ادامه؟"),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false),
              child: const Text("انصراف")),
          ElevatedButton(
            style: ElevatedButton.styleFrom(backgroundColor: Colors.red),
            onPressed: () => Navigator.pop(context, true),
            child: const Text("خروج",
                style: TextStyle(color: Colors.white)),
          ),
        ],
      ),
    );
    if (confirmed != true) return;

    setState(() => _leaving = true);

    final creds = await AuthStore.creds();
    if (creds == null) {
      setState(() { _leaving = false; _error = "ابتدا وارد شوید"; });
      return;
    }
    final username = creds.username;
    final password = creds.password;
    final devicePw = await DeviceSecretStore.read(widget.deviceId);
    if (devicePw == null) {
      setState(() {
        _leaving = false;
        _error = "رمز دستگاه یافت نشد — احتمالاً پیش‌تر از این دستگاه خارج شده‌اید.";
      });
      return;
    }
    final api = DeviceApi(username: username, password: password);
    final res = await api.unsubscribeDevice(
      deviceId:       widget.deviceId,
      devicePassword: devicePw,
    );

    if (!mounted) return;
    switch (res) {
      case ApiOk():
        await deleteDevice(widget.deviceId);
        await DeviceSecretStore.remove(widget.deviceId);
        if (!mounted) return;
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text("✅ از دستگاه خارج شدید")),
        );
        Navigator.popUntil(context, (r) => r.isFirst);
      case ApiError(error: final e, status: final s):
        setState(() { _leaving = false; _error = "خطا ($s): $e"; });
      case ApiDeviceFull():
        setState(() { _leaving = false; _error = "ناسازگاری"; });
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      appBar: AppBar(
        backgroundColor: Colors.green,
        title: const Text("مشترکین دستگاه",
            style: TextStyle(
                fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold)),
        toolbarHeight: 90.0,
      ),
      body: Padding(
        padding: const EdgeInsets.all(20),
        child: _error != null
            ? Center(child: Text(_error!,
                style: const TextStyle(color: Colors.white, fontSize: 16)))
            : _subscribers == null
                ? const Center(child: CircularProgressIndicator(color: Colors.white))
                : Column(
                    children: [
                      Text("شناسه دستگاه: ${widget.deviceId}",
                          style: const TextStyle(color: Colors.white70, fontSize: 14)),
                      const SizedBox(height: 10),
                      Text("${_subscribers!.length} / 3 مشترک",
                          style: const TextStyle(color: Colors.white, fontSize: 16)),
                      const SizedBox(height: 16),
                      Expanded(
                        child: ListView.separated(
                          itemCount: _subscribers!.length,
                          separatorBuilder: (_, __) => const SizedBox(height: 10),
                          itemBuilder: (_, i) {
                            final s = _subscribers![i];
                            final un = s['username'] as String;
                            final isSelf = un == _selfUsername;
                            return Material(
                              color: isSelf ? Colors.green.shade100 : Colors.white,
                              borderRadius: BorderRadius.circular(10),
                              child: ListTile(
                                leading: CircleAvatar(
                                    backgroundColor: Colors.green.shade300,
                                    child: Text(un.substring(0, 1).toUpperCase(),
                                        style: const TextStyle(color: Colors.white))),
                                title: Text(un,
                                    style: const TextStyle(
                                        color: Colors.black87,
                                        fontWeight: FontWeight.bold)),
                                subtitle: Text("از: ${s['joined_at']}",
                                    style: const TextStyle(color: Colors.black54)),
                                trailing: isSelf
                                    ? const Chip(label: Text("شما"))
                                    : null,
                              ),
                            );
                          },
                        ),
                      ),
                      const SizedBox(height: 12),
                      SizedBox(
                        width: double.infinity,
                        child: OutlinedButton.icon(
                          style: OutlinedButton.styleFrom(
                            side: const BorderSide(color: Colors.red),
                            padding: const EdgeInsets.symmetric(vertical: 14),
                          ),
                          onPressed: _leaving ? null : _leaveDevice,
                          icon: _leaving
                              ? const SizedBox(width: 16, height: 16,
                                  child: CircularProgressIndicator(strokeWidth: 2))
                              : const Icon(Icons.logout, color: Colors.red),
                          label: const Text("خروج از این دستگاه",
                              style: TextStyle(color: Colors.red, fontSize: 16)),
                        ),
                      ),
                    ],
                  ),
      ),
    );
  }
}
