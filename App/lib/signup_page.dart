// Account sign-up — first half of the two-step flow:
//   1. Here: user types email, POST /Registeration_App with Packet=Create_Account.
//      Server validates the email isn't taken, generates a 6-digit code,
//      emails it via Gmail SMTP, returns 200.
//   2. Next: PasswordSetupPage — user types the code from email + chooses
//      a password. POST /Registeration_App with Packet=Validation_Code.
//      Server creates the user row and returns 200; we then route back
//      to LoginPage so the user signs in normally.
//
// Sign-up is device-agnostic. Pairing with a real device happens AFTER
// login via the Phase 2 /Subscribe_Device / /Register_Device endpoints
// (driven by add_device_entry_page → connect_device_page / setup_new_device_page).
import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'dart:convert';

import 'configs.dart';
import 'password_setup_page.dart';

class SignUpPage extends StatefulWidget {
  const SignUpPage({super.key});

  @override
  State<SignUpPage> createState() => _SignUpPageState();
}

class _SignUpPageState extends State<SignUpPage> {
  final emailController = TextEditingController();
  bool _sending = false;     // shows spinner on the button
  String? _statusMsg;        // inline message shown above the button
  Color   _statusColor = Colors.transparent;

  @override
  void dispose() {
    emailController.dispose();
    super.dispose();
  }

  Future<void> _requestValidationCode() async {
    final email = emailController.text.trim();
    if (email.isEmpty || !email.contains('@')) {
      setState(() {
        _statusMsg = "ایمیل معتبر وارد کنید";
        _statusColor = Colors.red.shade700;
      });
      return;
    }
    setState(() {
      _sending = true;
      _statusMsg = "در حال ارسال کد تأیید... (ممکن است چند ثانیه طول بکشد)";
      _statusColor = Colors.blue.shade800;
    });

    try {
      final response = await http.post(
        Uri.parse('$serverUrl/Registeration_App'),
        headers: {
          'Content-Type': 'application/json',
          'Packet':       'Create_Account',
          'Username':     email,
        },
        body: jsonEncode({}),
      ).timeout(const Duration(seconds: 30));

      if (!mounted) return;
      if (response.statusCode == 200) {
        setState(() {
          _sending = false;
          _statusMsg = "کد تأیید به ایمیل شما ارسال شد. "
                       "اگر در صندوق ورودی پیدا نشد، پوشه‌ی Spam را هم بررسی کنید.";
          _statusColor = Colors.green.shade800;
        });
        // Brief pause so the user actually sees the success message before
        // we route to the password setup. Previously we navigated instantly
        // so the message was invisible.
        await Future.delayed(const Duration(seconds: 2));
        if (!mounted) return;
        Navigator.push(
          context,
          MaterialPageRoute(
            builder: (context) => PasswordSetupPage(email: email),
          ),
        );
      } else {
        String msg;
        try {
          final body = jsonDecode(response.body);
          final err  = body['error'] ?? '';
          msg = err == 'Username already exists'
              ? "این ایمیل قبلاً ثبت شده — وارد شوید"
              : "خطا: $err";
        } catch (_) {
          msg = "خطا در ارسال (${response.statusCode})";
        }
        setState(() {
          _sending = false;
          _statusMsg = msg;
          _statusColor = Colors.red.shade800;
        });
      }
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _sending = false;
        _statusMsg = 'خطای شبکه: $e';
        _statusColor = Colors.red.shade800;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      appBar: AppBar(
        backgroundColor: Colors.green,
        title: const Text(
          "ایجاد حساب کاربری",
          style: TextStyle(
              fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold),
        ),
        leading: IconButton(
          icon: const Icon(Icons.arrow_back, color: Colors.white),
          onPressed: () => Navigator.pop(context),
        ),
        toolbarHeight: 90.0,
      ),
      body: Padding(
        padding: const EdgeInsets.all(16.0),
        child: Center(
          child: SingleChildScrollView(
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                const Text(
                  "ایمیل خود را وارد کنید تا کد تأیید برایتان ارسال شود.",
                  textAlign: TextAlign.center,
                  style: TextStyle(
                      fontSize: 16,
                      color: Colors.white,
                      fontWeight: FontWeight.w500),
                ),
                const SizedBox(height: 24),
                TextField(
                  style: const TextStyle(color: Colors.white),
                  controller: emailController,
                  decoration: InputDecoration(
                    labelText: "ایمیل",
                    labelStyle: const TextStyle(color: Colors.white70),
                    border: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(10),
                    ),
                  ),
                  keyboardType: TextInputType.emailAddress,
                ),
                const SizedBox(height: 24),
                // Inline status — shows "sending...", success, or error
                // ABOVE the button so the user sees feedback without the
                // screen switching out from under them.
                if (_statusMsg != null)
                  Container(
                    margin: const EdgeInsets.only(bottom: 16),
                    padding: const EdgeInsets.all(12),
                    decoration: BoxDecoration(
                      color: _statusColor,
                      borderRadius: BorderRadius.circular(8),
                    ),
                    child: Text(
                      _statusMsg!,
                      textAlign: TextAlign.center,
                      style: const TextStyle(color: Colors.white, fontSize: 14),
                    ),
                  ),
                ElevatedButton(
                  onPressed: _sending ? null : _requestValidationCode,
                  style: ElevatedButton.styleFrom(
                    backgroundColor: Colors.green,
                    padding: const EdgeInsets.symmetric(vertical: 16),
                    shape: RoundedRectangleBorder(
                      borderRadius: BorderRadius.circular(10),
                    ),
                  ),
                  child: _sending
                      ? const SizedBox(
                          width: 22, height: 22,
                          child: CircularProgressIndicator(
                              strokeWidth: 2, color: Colors.white))
                      : const Text(
                          "ارسال کد تأیید",
                          style: TextStyle(
                              fontSize: 18,
                              color: Colors.white,
                              fontWeight: FontWeight.bold),
                        ),
                ),
                const SizedBox(height: 12),
                const Text(
                  "پس از تأیید ایمیل، رمز عبور را تنظیم می‌کنید و سپس می‌توانید وارد شوید.",
                  textAlign: TextAlign.center,
                  style: TextStyle(fontSize: 12, color: Colors.white70),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
