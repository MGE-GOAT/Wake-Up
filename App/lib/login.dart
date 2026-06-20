import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'dart:convert';
import 'dart:async';
import 'package:sqflite/sqflite.dart';
import 'package:path/path.dart';

import 'package:shared_preferences/shared_preferences.dart';
import 'configs.dart';
import 'main_page.dart';
import 'signup_page.dart';
import 'db.dart';
import 'services/media_crypto.dart';
import 'services/secure_storage.dart';

class LoginPage extends StatefulWidget {
  const LoginPage({super.key});

  @override
  State<LoginPage> createState() => _LoginPageState();
}

class _LoginPageState extends State<LoginPage> {
  final TextEditingController emailController = TextEditingController();
  final TextEditingController passwordController = TextEditingController();
  bool _loading = false;

  @override
  void dispose() {
    emailController.dispose();
    passwordController.dispose();
    super.dispose();
  }

  /// Persist one historical event returned by /Login_App: decode+decrypt its
  /// media (fail-closed) and insert it as a 'read' event (history, not a fresh
  /// alert). Mirrors the heartbeat's Event handling.
  Future<void> _ingestHistoryEvent(
      String deviceId, Map<String, dynamic> ev) async {
    final messageId = ev['message_id'].toString();
    final messageText = ev['message_text'] ?? '{}';
    final messageImage = ev['message_image'];
    final bool hasVideo = ev['has_video'] == true;  // MP4 fetched on demand
    final messageTime = ev['message_occur_time'];

    bool encrypted = false;
    try {
      final mt = jsonDecode(messageText);
      encrypted = mt is Map && mt['encrypted'] == true;
    } catch (_) {}

    // Coerce to String defensively — these fields may be null or non-string.
    final imgB64 = messageImage is String ? messageImage : '';
    String? imagePath;
    if (imgB64.isNotEmpty) {
      imagePath = await saveIncomingMedia(
          base64Data: imgB64, fileName: '$deviceId-$messageId-image.png',
          deviceId: deviceId, encrypted: encrypted);
    }
    // 'remote' sentinel = downloadable clip on the server (lazy download on tap).
    await insertEvent(messageId, deviceId, messageText,
        imagePath ?? 'null', hasVideo ? 'remote' : 'null', messageTime ?? '', 'read');
  }

  Future<void> login(BuildContext context) async {
    if (_loading) return;            // ignore double-taps while a login is in flight
    final String email = emailController.text;
    final String password = passwordController.text;
    setState(() => _loading = true);

    try {
      final response = await http.post(
        Uri.parse('$serverUrl/Login_App'),
        headers: {
          'Password': password,
          'User-Agent': 'App',
          'Username': email,
          'Content-Length': '0',
        },
      ).timeout(const Duration(seconds: 20));

      if (response.statusCode == 200) {

        await initializeDatabase();
        // (removed printAllDevices() — it logged device rows incl. passwords to logcat)

        // Ingest the fall-event history the server returns on login so past
        // events + their videos appear after a fresh login/reinstall (the
        // heartbeat only delivers NEW unread events going forward).
        try {
          final body = jsonDecode(response.body);
          final subscribed = body is Map ? body['subscribed_devices'] : null;
          if (subscribed is Map) {
            for (final entry in subscribed.entries) {
              final deviceId = entry.key as String;
              final events = entry.value;
              if (events is! List) continue;
              for (final ev in events) {
                // Per-event guard: one malformed event must not abort ingest
                // of the rest.
                try {
                  await _ingestHistoryEvent(deviceId, Map<String, dynamic>.from(ev));
                } catch (e) {
                  print('[login] skipped a history event: $e');
                }
              }
            }
          }
        } catch (e) {
          // Non-fatal: login still proceeds even if history ingest fails.
          // ignore: avoid_print
          print('[login] event history ingest failed: $e');
        }

        final prefs = await SharedPreferences.getInstance();
        await prefs.setBool('isLoggedIn', true);
        await prefs.setString('username', email);
        // Password goes to Keystore/Keychain — NEVER plaintext prefs.
        await AuthStore.savePassword(password);

        if (!context.mounted) return;
        Navigator.pushReplacement(
          context,
          MaterialPageRoute(
            builder: (context) => MainPage(),
          ),
        );
      } else {
        if (!context.mounted) return;
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
              content: Text('خطا در ارسال اطلاعات: ${response.statusCode}')),
        );
      }
    } catch (e) {
      if (context.mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('خطای شبکه — دوباره تلاش کنید')),
        );
      }
    } finally {
      if (mounted) setState(() => _loading = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      body: SafeArea(
        child: Padding(
        padding: const EdgeInsets.all(16.0),
        child: Center(
          child: SingleChildScrollView(
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                const Text(
                  'به اپلیکیشن دستیار مراقبت از سالمندان خوش آمدید',
                  textAlign: TextAlign.center,
                  style: TextStyle(
                    fontSize: 22,
                    color: Colors.white,
                    fontWeight: FontWeight.bold,
                  ),
                ),
                const SizedBox(height: 20),
                const Text(
                  '.لطفا وارد حساب کاربری خود شوید',
                  style: TextStyle(
                    fontSize: 16,
                    color: Colors.white,
                    fontWeight: FontWeight.bold,
                  ),
                ),
                const SizedBox(height: 20),
                TextField(
                  controller: emailController,
                  style: const TextStyle(color: Colors.white),
                  decoration: InputDecoration(
                    labelText: 'Email:',
                    labelStyle: const TextStyle(
                        color: Color.fromARGB(255, 255, 255, 255)),
                    filled: false,
                    // fillColor: const Color.fromARGB(255, 55, 71, 79),
                    border: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(10),
                    ),
                  ),
                ),
                const SizedBox(height: 15),
                TextField(
                  controller: passwordController,
                  style: const TextStyle(color: Colors.white),
                  obscureText: true,
                  decoration: InputDecoration(
                    labelText: 'password:',
                    labelStyle: const TextStyle(color: Colors.white),
                    filled: false,
                    fillColor: Colors.white,
                    border: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(10),
                    ),
                  ),
                ),
                const SizedBox(height: 10),
                Align(
                  alignment: Alignment.centerRight,
                  child: TextButton(
                    onPressed: () {},
                    child: const Text(
                      'رمز خود را فراموش کرده‌ام',
                      style: TextStyle(
                        color: Colors.white,
                        decoration: TextDecoration.underline,
                      ),
                    ),
                  ),
                ),
                const SizedBox(height: 20),
                ElevatedButton(
                  onPressed: _loading ? null : () => login(context),
                  style: ElevatedButton.styleFrom(
                    backgroundColor: Colors.blue,
                    padding: const EdgeInsets.symmetric(
                      horizontal: 50,
                      vertical: 15,
                    ),
                    shape: RoundedRectangleBorder(
                      borderRadius: BorderRadius.circular(10),
                    ),
                  ),
                  child: _loading
                      ? const SizedBox(
                          height: 20, width: 20,
                          child: CircularProgressIndicator(
                              strokeWidth: 2, color: Colors.white))
                      : const Text('ورود',
                          style: TextStyle(fontSize: 18, color: Colors.white)),
                ),
                const SizedBox(height: 30),
                Image.asset(
                  'assets/images/elderly_care.png',
                  height: 280,
                ),
                const SizedBox(height: 30),
                Row(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    OutlinedButton(
                      onPressed: () {
                        Navigator.push(
                          context,
                          MaterialPageRoute(builder: (context) => SignUpPage()),
                        );
                      },
                      style: OutlinedButton.styleFrom(
                        side: const BorderSide(color: Colors.white),
                        // Match the login button's size so the two aren't
                        // mismatched / cramped on smaller phones.
                        padding: const EdgeInsets.symmetric(
                          horizontal: 50,
                          vertical: 15,
                        ),
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(10),
                        ),
                      ),
                      child: const Text(
                        'ثبت نام',
                        style: TextStyle(color: Colors.white, fontSize: 18),
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ),
      ),
      ),
    );
  }
}














