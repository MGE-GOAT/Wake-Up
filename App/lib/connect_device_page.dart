// Connect to an existing device (someone already onboarded it; you've been
// handed the device-id + 32-char password).
//
// Submits to /Subscribe_Device. Handles three outcomes:
//   • 201 / 200 — saves locally + secure_storage, returns to dashboard
//   • 409 device_full — routes to DeviceFullPage with the 3 current subs
//                       so the user can pick one to kick (Replace_Subscriber)
//   • 403 wrong_password / 404 device_not_found — inline error
import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'configs.dart';
import 'db.dart';
import 'device_full_page.dart';
import 'services/device_api.dart';
import 'services/secure_storage.dart';

class ConnectDevicePage extends StatefulWidget {
  const ConnectDevicePage({super.key});

  @override
  State<ConnectDevicePage> createState() => _ConnectDevicePageState();
}

class _ConnectDevicePageState extends State<ConnectDevicePage> {
  final _idController       = TextEditingController();
  final _passwordController = TextEditingController();
  final _nameController     = TextEditingController();
  bool _busy = false;
  String? _error;
  bool _hidePassword = true;

  @override
  void dispose() {
    _idController.dispose();
    _passwordController.dispose();
    _nameController.dispose();
    super.dispose();
  }

  Future<void> _submit() async {
    final id   = _idController.text.trim();
    final pw   = _passwordController.text;
    final name = _nameController.text.trim();
    if (id.isEmpty) {
      setState(() => _error = "شناسه دستگاه را وارد کنید");
      return;
    }
    if (pw.length < kDevicePasswordMinChars) {
      setState(() => _error =
          "رمز دستگاه باید حداقل $kDevicePasswordMinChars کاراکتر باشد");
      return;
    }

    setState(() { _busy = true; _error = null; });

    final creds = await AuthStore.creds();
    final username = creds?.username;
    final password = creds?.password;
    if (username == null || password == null) {
      setState(() { _busy = false; _error = "ابتدا وارد حساب کاربری شوید"; });
      return;
    }

    final api = DeviceApi(username: username, password: password);
    final result = await api.subscribeDevice(deviceId: id, devicePassword: pw);

    if (!mounted) return;

    switch (result) {
      case ApiOk():
        await _persistAndExit(id, pw, name.isEmpty ? id : name);
      case ApiDeviceFull(subscribers: final subs):
        // Route to kick-someone screen. If the user successfully replaces
        // someone, that screen pops with `true` and we finalize here.
        final replaced = await Navigator.push<bool>(
          context,
          MaterialPageRoute(builder: (_) => DeviceFullPage(
            deviceId:       id,
            devicePassword: pw,
            subscribers:    subs,
          )),
        );
        if (replaced == true) {
          await _persistAndExit(id, pw, name.isEmpty ? id : name);
        } else {
          setState(() => _busy = false);
        }
      case ApiError(error: final err, status: final code):
        setState(() {
          _busy = false;
          _error = _humanize(err, code);
        });
    }
  }

  Future<void> _persistAndExit(String id, String pw, String name) async {
    await insertDevice(id, pw, name);
    await DeviceSecretStore.save(id, pw);
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text("✅ .دستگاه با موفقیت افزوده شد")),
    );
    Navigator.pop(context);
  }

  String _humanize(String err, int code) {
    if (err == 'device_not_found') return "دستگاهی با این شناسه ثبت نشده";
    if (err == 'wrong_password')   return "رمز دستگاه اشتباه است";
    if (err.startsWith('network')) return "ارتباط با سرور برقرار نشد";
    return "خطا ($code): $err";
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      appBar: AppBar(
        backgroundColor: Colors.green,
        title: const Text("اتصال به دستگاه",
            style: TextStyle(
                fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold)),
        toolbarHeight: 90.0,
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(20),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            _Field(controller: _idController,       labelFa: "شناسه دستگاه"),
            const SizedBox(height: 14),
            _Field(controller: _passwordController, labelFa: "رمز دستگاه (حداقل ۳۲ کاراکتر)",
                obscure: _hidePassword,
                trailing: IconButton(
                  icon: Icon(_hidePassword ? Icons.visibility : Icons.visibility_off,
                      color: Colors.white70),
                  onPressed: () => setState(() => _hidePassword = !_hidePassword),
                )),
            const SizedBox(height: 14),
            _Field(controller: _nameController, labelFa: "نام نمایشی (اختیاری)"),
            const SizedBox(height: 24),
            if (_error != null)
              Container(
                padding: const EdgeInsets.all(12),
                margin: const EdgeInsets.only(bottom: 16),
                decoration: BoxDecoration(
                  color: Colors.red.shade800,
                  borderRadius: BorderRadius.circular(8),
                ),
                child: Text(_error!,
                    style: const TextStyle(color: Colors.white)),
              ),
            ElevatedButton(
              onPressed: _busy ? null : _submit,
              style: ElevatedButton.styleFrom(
                backgroundColor: Colors.green,
                padding: const EdgeInsets.symmetric(vertical: 16),
              ),
              child: _busy
                  ? const SizedBox(
                      width: 22, height: 22,
                      child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white))
                  : const Text("اتصال",
                      style: TextStyle(fontSize: 18, color: Colors.white)),
            ),
          ],
        ),
      ),
    );
  }
}

class _Field extends StatelessWidget {
  final TextEditingController controller;
  final String labelFa;
  final bool obscure;
  final Widget? trailing;
  const _Field({
    required this.controller,
    required this.labelFa,
    this.obscure = false,
    this.trailing,
  });

  @override
  Widget build(BuildContext context) {
    return TextField(
      controller: controller,
      obscureText: obscure,
      style: const TextStyle(color: Colors.white),
      decoration: InputDecoration(
        labelText: labelFa,
        labelStyle: const TextStyle(color: Colors.white70),
        suffixIcon: trailing,
        filled: true,
        fillColor: Colors.black26,
        border: OutlineInputBorder(
            borderRadius: BorderRadius.circular(8),
            borderSide: BorderSide.none),
      ),
    );
  }
}
