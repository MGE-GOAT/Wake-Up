// Shown when /Subscribe_Device returns 409 device_full.
//
// The 3 existing subscribers are passed in from ConnectDevicePage (already
// fetched by the 409 body). User taps one + confirms; we POST
// /Replace_Subscriber which atomically kicks that user and adds the caller.
//
// Pops with `true` on success (caller finalizes the local insert),
// `false` if the user backs out.
import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'services/device_api.dart';
import 'services/secure_storage.dart';

class DeviceFullPage extends StatefulWidget {
  final String deviceId;
  final String devicePassword;
  final List<Map<String, dynamic>> subscribers;

  const DeviceFullPage({
    super.key,
    required this.deviceId,
    required this.devicePassword,
    required this.subscribers,
  });

  @override
  State<DeviceFullPage> createState() => _DeviceFullPageState();
}

class _DeviceFullPageState extends State<DeviceFullPage> {
  String? _selectedUsername;
  bool _busy = false;
  String? _error;

  Future<void> _kickAndJoin() async {
    if (_selectedUsername == null) return;

    final confirmed = await showDialog<bool>(
      context: context,
      builder: (_) => AlertDialog(
        title: const Text("تأیید حذف"),
        content: Text(
            "${_selectedUsername!} از مشترکین این دستگاه حذف خواهد شد و شما جایگزین می‌شوید. به آن‌ها اطلاع داده خواهد شد."),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false),
              child: const Text("انصراف")),
          ElevatedButton(
            style: ElevatedButton.styleFrom(backgroundColor: Colors.red),
            onPressed: () => Navigator.pop(context, true),
            child: const Text("حذف و جایگزینی",
                style: TextStyle(color: Colors.white)),
          ),
        ],
      ),
    );
    if (confirmed != true) return;

    setState(() { _busy = true; _error = null; });

    final creds = await AuthStore.creds();
    final username = creds?.username;
    final password = creds?.password;
    if (username == null || password == null) {
      setState(() { _busy = false; _error = "ابتدا وارد حساب کاربری شوید"; });
      return;
    }

    final api = DeviceApi(username: username, password: password);
    final res = await api.replaceSubscriber(
      deviceId:       widget.deviceId,
      devicePassword: widget.devicePassword,
      replaceTarget:  _selectedUsername!,
    );

    if (!mounted) return;
    switch (res) {
      case ApiOk():
        Navigator.pop(context, true);
      case ApiError(error: final err, status: final code):
        setState(() {
          _busy = false;
          _error = "خطا ($code): $err";
        });
      case ApiDeviceFull():
        // Shouldn't happen here, but handle defensively.
        setState(() { _busy = false; _error = "ناسازگاری: دستگاه دوباره پر شد"; });
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      appBar: AppBar(
        backgroundColor: Colors.orange.shade800,
        title: const Text("دستگاه پر است",
            style: TextStyle(
                fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold)),
        toolbarHeight: 90.0,
      ),
      body: Padding(
        padding: const EdgeInsets.all(20),
        child: Column(
          children: [
            const Text(
              "این دستگاه به سه نفر متصل است (حداکثر مجاز). برای اتصال شما، یکی باید حذف شود.",
              textAlign: TextAlign.center,
              style: TextStyle(fontSize: 16, color: Colors.white),
            ),
            const SizedBox(height: 20),
            Expanded(
              child: ListView.separated(
                itemCount: widget.subscribers.length,
                separatorBuilder: (_, __) => const SizedBox(height: 10),
                itemBuilder: (_, i) {
                  final s = widget.subscribers[i];
                  final un = s['username'] as String;
                  return Material(
                    color: _selectedUsername == un
                        ? Colors.green.shade700
                        : Colors.white,
                    borderRadius: BorderRadius.circular(10),
                    child: InkWell(
                      borderRadius: BorderRadius.circular(10),
                      onTap: () => setState(() => _selectedUsername = un),
                      child: Padding(
                        padding: const EdgeInsets.all(16),
                        child: Row(
                          children: [
                            CircleAvatar(
                                backgroundColor: Colors.green.shade100,
                                child: Text(un.substring(0, 1).toUpperCase())),
                            const SizedBox(width: 14),
                            Expanded(
                              child: Column(
                                crossAxisAlignment: CrossAxisAlignment.start,
                                children: [
                                  Text(un,
                                      style: TextStyle(
                                          fontSize: 16,
                                          fontWeight: FontWeight.bold,
                                          color: _selectedUsername == un
                                              ? Colors.white : Colors.black)),
                                  Text("از: ${s['joined_at']}",
                                      style: TextStyle(
                                          fontSize: 12,
                                          color: _selectedUsername == un
                                              ? Colors.white70 : Colors.grey)),
                                ],
                              ),
                            ),
                            if (_selectedUsername == un)
                              const Icon(Icons.check_circle, color: Colors.white),
                          ],
                        ),
                      ),
                    ),
                  );
                },
              ),
            ),
            if (_error != null)
              Padding(
                padding: const EdgeInsets.only(bottom: 12),
                child: Container(
                  padding: const EdgeInsets.all(12),
                  decoration: BoxDecoration(
                    color: Colors.red.shade800,
                    borderRadius: BorderRadius.circular(8),
                  ),
                  child: Text(_error!,
                      style: const TextStyle(color: Colors.white)),
                ),
              ),
            SizedBox(
              width: double.infinity,
              child: ElevatedButton(
                onPressed: _selectedUsername == null || _busy ? null : _kickAndJoin,
                style: ElevatedButton.styleFrom(
                  backgroundColor: Colors.red,
                  padding: const EdgeInsets.symmetric(vertical: 16),
                ),
                child: _busy
                    ? const SizedBox(
                        width: 22, height: 22,
                        child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white))
                    : const Text("حذف انتخاب‌شده و جایگزینی",
                        style: TextStyle(fontSize: 18, color: Colors.white)),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
