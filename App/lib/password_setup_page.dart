import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'dart:convert';

import 'configs.dart';
import 'login.dart';

class PasswordSetupPage extends StatefulWidget {
  final String email;
  const PasswordSetupPage({super.key, required this.email});

  @override
  State<PasswordSetupPage> createState() => _PasswordSetupPageState();
}

class _PasswordSetupPageState extends State<PasswordSetupPage> {
  // Controllers live in State (was: created inside build() on a StatelessWidget —
  // leaked a new set on every rebuild and wiped the user's typed text).
  final _passwordController = TextEditingController();
  final _repeatPasswordController = TextEditingController();
  final _validationCodeController = TextEditingController();
  bool _submitting = false;

  @override
  void dispose() {
    _passwordController.dispose();
    _repeatPasswordController.dispose();
    _validationCodeController.dispose();
    super.dispose();
  }

  Future<void> _validateAndSetPassword() async {
    final password = _passwordController.text;
    final repeatPassword = _repeatPasswordController.text;
    final validationCode = _validationCodeController.text;

    if (password != repeatPassword) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text("رمز عبور و تکرار آن مطابقت ندارند.")),
      );
      return;
    }

    setState(() => _submitting = true);
    try {
      final response = await http.post(
        Uri.parse('$serverUrl/Registeration_App'),
        headers: {
          'Content-Type': 'application/json',
          'Packet': 'Validation_Code',
          'User-Agent': 'App',
          'Username': widget.email,
        },
        body: jsonEncode({
          "Validation_Code": int.tryParse(validationCode) ?? 0,
          "Set_Password": password,
        }),
      ).timeout(const Duration(seconds: 20));

      if (!mounted) return;
      if (response.statusCode == 200) {
        // Account created — route to login (is-logged-in is set only by a real
        // /Login_App success, the single source of truth).
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text("حساب کاربری ایجاد شد. اکنون وارد شوید.")),
        );
        Navigator.pushAndRemoveUntil(
          context,
          MaterialPageRoute(builder: (context) => const LoginPage()),
          (route) => false,
        );
      } else {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text("خطا در ثبت رمز عبور: ${response.statusCode}")),
        );
      }
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('خطای شبکه — دوباره تلاش کنید')),
      );
    } finally {
      if (mounted) setState(() => _submitting = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      appBar: AppBar(
        backgroundColor: Colors.green,
        title: const Text(
          "تنظیم گذرواژه",
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
              crossAxisAlignment: CrossAxisAlignment.center,
              children: [
                const Text(
                  ".لطفا یک گذرواژه مناسب انتخاب کنید",
                  style: TextStyle(
                      fontSize: 20,
                      color: Colors.white,
                      fontWeight: FontWeight.bold),
                ),
                const SizedBox(height: 20),
                TextField(
                  style: const TextStyle(color: Colors.white),
                  controller: _passwordController,
                  decoration: InputDecoration(
                    labelText: "Password:",
                    labelStyle: const TextStyle(color: Colors.white),
                    filled: false,
                    border: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(10),
                    ),
                  ),
                  obscureText: true,
                ),
                const SizedBox(height: 15),
                TextField(
                  style: const TextStyle(color: Colors.white),
                  controller: _repeatPasswordController,
                  decoration: InputDecoration(
                    labelText: "Repeat Password:",
                    labelStyle: const TextStyle(color: Colors.white),
                    filled: false,
                    border: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(10),
                    ),
                  ),
                  obscureText: true,
                ),
                const SizedBox(height: 15),
                TextField(
                  style: const TextStyle(color: Colors.white),
                  controller: _validationCodeController,
                  decoration: InputDecoration(
                    labelText: "Validation Code:",
                    labelStyle: const TextStyle(color: Colors.white),
                    filled: false,
                    border: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(10),
                    ),
                  ),
                  keyboardType: TextInputType.number,
                ),
                const SizedBox(height: 20),
                ElevatedButton(
                  onPressed: _submitting
                      ? null
                      : () {
                          if (_passwordController.text.isNotEmpty &&
                              _repeatPasswordController.text.isNotEmpty &&
                              _validationCodeController.text.isNotEmpty) {
                            _validateAndSetPassword();
                          } else {
                            ScaffoldMessenger.of(context).showSnackBar(
                              const SnackBar(
                                content: Text("لطفا تمام فیلدها را پر کنید."),
                              ),
                            );
                          }
                        },
                  style: ElevatedButton.styleFrom(
                    backgroundColor: Colors.blue,
                    padding:
                        const EdgeInsets.symmetric(horizontal: 50, vertical: 15),
                    shape: RoundedRectangleBorder(
                      borderRadius: BorderRadius.circular(10),
                    ),
                  ),
                  child: _submitting
                      ? const SizedBox(
                          height: 20,
                          width: 20,
                          child: CircularProgressIndicator(
                              strokeWidth: 2, color: Colors.white),
                        )
                      : const Text(
                          "ثبت",
                          style: TextStyle(
                              fontSize: 18,
                              color: Colors.white,
                              fontWeight: FontWeight.bold),
                        ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
