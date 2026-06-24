
import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'dart:convert';
import 'dart:async';
import 'package:sqflite/sqflite.dart';
import 'package:path/path.dart';

import 'package:shared_preferences/shared_preferences.dart';
import 'configs.dart';
import 'password_setup_page.dart';


// class SetUpPage extends StatelessWidget {
//   const SetUpPage({super.key});

//   @override
//   Widget build(BuildContext context) {
//     final TextEditingController emailController = TextEditingController();
//     final TextEditingController deviceIdController = TextEditingController();
//     final TextEditingController deviceKeyController = TextEditingController();


//     return Scaffold(
//       backgroundColor: const Color(0xFF37474F),
//       appBar: AppBar(
//         backgroundColor: Colors.green,
//         title: const Text(
//           "راه‌اندازی دستگاه",
//           style: TextStyle(
//               fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold),
//         ),
//         leading: IconButton(
//           icon: const Icon(Icons.arrow_back, color: Colors.white),
//           onPressed: () {
//             Navigator.pop(context);
//           },
//         ),
//         toolbarHeight: 90.0, // افزایش ضخامت نوار ابزار
//       ),
//       body: Padding(
//         padding: const EdgeInsets.all(16.0),
//         child: Center(
//           child: SingleChildScrollView(
//             child: Column(
//               mainAxisAlignment: MainAxisAlignment.center,
//               crossAxisAlignment: CrossAxisAlignment.center,
//               children: [
//                 TextField(
//                   style: const TextStyle(color: Colors.white),
//                   controller: emailController,
//                   decoration: InputDecoration(
//                     labelText: "Your Device ID:",
//                     labelStyle: const TextStyle(
//                         color: Color.fromARGB(255, 255, 255, 255)),
//                     filled: false,
//                     border: OutlineInputBorder(
//                       borderRadius: BorderRadius.circular(10),
//                     ),
//                   ),
//                   keyboardType: TextInputType.emailAddress,
//                 ),

//                 const SizedBox(height: 50),

//                 const Text(
//                   ".لطفا اطلاعات خود را وارد کنید",
//                   style: TextStyle(
//                       fontSize: 18,
//                       color: Colors.white,
//                       fontWeight: FontWeight.bold),
//                 ),
//                 const SizedBox(height: 20),
//                 TextField(
//                   style: const TextStyle(color: Colors.white),
//                   controller: emailController,
//                   decoration: InputDecoration(
//                     labelText: "WIFI SSID:",
//                     labelStyle: const TextStyle(
//                         color: Color.fromARGB(255, 255, 255, 255)),
//                     filled: false,
//                     border: OutlineInputBorder(
//                       borderRadius: BorderRadius.circular(10),
//                     ),
//                   ),
//                   keyboardType: TextInputType.emailAddress,
//                 ),
//                 const SizedBox(height: 15),
//                 TextField(
//                   style: const TextStyle(color: Colors.white),
//                   controller: deviceIdController,
//                   decoration: InputDecoration(
//                     labelText: "WIFI Password:",
//                     labelStyle: const TextStyle(
//                         color: Color.fromARGB(255, 255, 255, 255)),
//                     filled: false,
//                     border: OutlineInputBorder(
//                       borderRadius: BorderRadius.circular(10),
//                     ),
//                   ),
//                   keyboardType: TextInputType.text,
//                 ),
//                 const SizedBox(height: 15),
//                 TextField(
//                   style: const TextStyle(color: Colors.white),
//                   controller: deviceKeyController,
//                   decoration: InputDecoration(
//                     labelText: "Device Key:",
//                     labelStyle: const TextStyle(
//                         color: Color.fromARGB(255, 255, 255, 255)),
//                     filled: false,
//                     border: OutlineInputBorder(
//                       borderRadius: BorderRadius.circular(10),
//                     ),
//                   ),
//                   keyboardType: TextInputType.text,
//                 ),
//                 const SizedBox(height: 20),
//                 ElevatedButton(
//                   onPressed: () {
//                     if (emailController.text.isNotEmpty &&
//                         deviceIdController.text.isNotEmpty &&
//                         deviceKeyController.text.isNotEmpty) {
//                     } else {
//                       ScaffoldMessenger.of(context).showSnackBar(
//                         const SnackBar(
//                           content: Text("لطفا تمام فیلدها را پر کنید."),
//                         ),
//                       );
//                     }
//                   },
//                   style: ElevatedButton.styleFrom(
//                     backgroundColor: Colors.blue,
//                     padding: const EdgeInsets.symmetric(
//                         horizontal: 50, vertical: 15),
//                     shape: RoundedRectangleBorder(
//                       borderRadius: BorderRadius.circular(10),
//                     ),
//                   ),
//                   child: const Text(
//                     "ثبت",
//                     style: TextStyle(
//                         fontSize: 16,
//                         color: Colors.white,
//                         fontWeight: FontWeight.bold),
//                   ),
//                 ),
//               ],
//             ),
//           ),
//         ),
//       ),
//     );
//   }
// }






// import 'package:flutter/material.dart';
// import 'package:http/http.dart' as http;
// import 'dart:convert';

// class SetUpPage extends StatelessWidget {
//   final Map<String, dynamic> deviceInfo;

//   const SetUpPage({super.key, required this.deviceInfo});

//   @override
//   Widget build(BuildContext context) {
//     final TextEditingController ssidController =
//         TextEditingController(text: deviceInfo['wifi_ssid'] ?? '');
//     final TextEditingController passwordController =
//         TextEditingController(text: deviceInfo['wifi_password'] ?? '');
//     final TextEditingController deviceKeyController =
//         TextEditingController(text: deviceInfo['key'] ?? '');

//     return Scaffold(
//       backgroundColor: const Color(0xFF37474F),
//       appBar: AppBar(
//         backgroundColor: Colors.green,
//         title: const Text(
//           "راه‌اندازی دستگاه",
//           style: TextStyle(
//               fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold),
//         ),
//         leading: IconButton(
//           icon: const Icon(Icons.arrow_back, color: Colors.white),
//           onPressed: () {
//             Navigator.pop(context);
//           },
//         ),
//         toolbarHeight: 90.0,
//       ),
//       body: Padding(
//         padding: const EdgeInsets.all(16.0),
//         child: Center(
//           child: SingleChildScrollView(
//             child: Column(
//               mainAxisAlignment: MainAxisAlignment.center,
//               crossAxisAlignment: CrossAxisAlignment.center,
//               children: [
//                 TextField(
//                   style: const TextStyle(color: Colors.white),
//                   controller: TextEditingController(text: deviceInfo['id']),
//                   readOnly: true,
//                   decoration: InputDecoration(
//                     labelText: "Your Device ID:",
//                     labelStyle:
//                         const TextStyle(color: Colors.white),
//                     filled: false,
//                     border: OutlineInputBorder(
//                       borderRadius: BorderRadius.circular(10),
//                     ),
//                   ),
//                 ),

//                 const SizedBox(height: 50),

//                 const Text(
//                   ".لطفا اطلاعات خود را وارد کنید",
//                   style: TextStyle(
//                       fontSize: 18,
//                       color: Colors.white,
//                       fontWeight: FontWeight.bold),
//                 ),
//                 const SizedBox(height: 20),
//                 TextField(
//                   style: const TextStyle(color: Colors.white),
//                   controller: ssidController,
//                   decoration: InputDecoration(
//                     labelText: "WIFI SSID:",
//                     labelStyle:
//                         const TextStyle(color: Colors.white),
//                     filled: false,
//                     border: OutlineInputBorder(
//                       borderRadius: BorderRadius.circular(10),
//                     ),
//                   ),
//                   keyboardType: TextInputType.text,
//                 ),
//                 const SizedBox(height: 15),
//                 TextField(
//                   style: const TextStyle(color: Colors.white),
//                   controller: passwordController,
//                   decoration: InputDecoration(
//                     labelText: "WIFI Password:",
//                     labelStyle:
//                         const TextStyle(color: Colors.white),
//                     filled: false,
//                     border: OutlineInputBorder(
//                       borderRadius: BorderRadius.circular(10),
//                     ),
//                   ),
//                   keyboardType: TextInputType.text,
//                 ),
//                 const SizedBox(height: 15),
//                 TextField(
//                   style: const TextStyle(color: Colors.white),
//                   controller: deviceKeyController,
//                   decoration: InputDecoration(
//                     labelText: "Device Key:",
//                     labelStyle:
//                         const TextStyle(color: Colors.white),
//                     filled: false,
//                     border: OutlineInputBorder(
//                       borderRadius: BorderRadius.circular(10),
//                     ),
//                   ),
//                   keyboardType: TextInputType.text,
//                 ),
//                 const SizedBox(height: 20),
//                 ElevatedButton(
//                   onPressed: () async {
//                     if (ssidController.text.isNotEmpty &&
//                         passwordController.text.isNotEmpty &&
//                         deviceKeyController.text.isNotEmpty) {
//                       final response = await http.post(
//                         Uri.parse('http://localhost:6000/config_device'),
//                         headers: {'Content-Type': 'application/json'},
//                         body: jsonEncode({
//                           "id": deviceInfo['id'],
//                           "key": deviceKeyController.text,
//                           "setup_status": 1,
//                           "wifi_password": passwordController.text,
//                           "wifi_ssid": ssidController.text,
//                         }),
//                       );

//                       if (response.statusCode == 200) {
//                         ScaffoldMessenger.of(context).showSnackBar(
//                           const SnackBar(content: Text("تنظیمات با موفقیت ذخیره شد.")),
//                         );
//                         Navigator.pop(context);
//                         Navigator.pop(context);
//                       } else if (response.statusCode == 403) {
//                         ScaffoldMessenger.of(context).showSnackBar(
//                           const SnackBar(content: Text("دستگاه دیگر قابل تنظیم نیست.")),
//                         );
//                         Navigator.pop(context);
//                         Navigator.pop(context);
//                       } else {
//                         ScaffoldMessenger.of(context).showSnackBar(
//                           const SnackBar(content: Text("خطا در ارسال تنظیمات.")),
//                         );
//                       }
//                     } else {
//                       ScaffoldMessenger.of(context).showSnackBar(
//                         const SnackBar(
//                           content: Text("لطفا تمام فیلدها را پر کنید."),
//                         ),
//                       );
//                     }
//                   },
//                   style: ElevatedButton.styleFrom(
//                     backgroundColor: Colors.blue,
//                     padding: const EdgeInsets.symmetric(
//                         horizontal: 50, vertical: 15),
//                     shape: RoundedRectangleBorder(
//                       borderRadius: BorderRadius.circular(10),
//                     ),
//                   ),
//                   child: const Text(
//                     "ثبت",
//                     style: TextStyle(
//                         fontSize: 16,
//                         color: Colors.white,
//                         fontWeight: FontWeight.bold),
//                   ),
//                 ),
//               ],
//             ),
//           ),
//         ),
//       ),
//     );
//   }
// }












import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'dart:convert';
import 'dart:math';


class SetUpPage extends StatefulWidget {
  final Map<String, dynamic> deviceInfo;

  const SetUpPage({super.key, required this.deviceInfo});

  @override
  State<SetUpPage> createState() => _SetUpPageState();
}

class _SetUpPageState extends State<SetUpPage> {
  // Owned by State and disposed in dispose(). Previously these were built inside
  // build() of a StatelessWidget, so every rebuild allocated new controllers,
  // leaked the old ones' listeners, and wiped whatever the user had typed.
  late final TextEditingController _idController;
  late final TextEditingController ssidController;
  late final TextEditingController passwordController;
  late final TextEditingController deviceKeyController;

  @override
  void initState() {
    super.initState();
    _idController =
        TextEditingController(text: widget.deviceInfo['id']?.toString() ?? '');
    ssidController =
        TextEditingController(text: widget.deviceInfo['wifi_ssid'] ?? '');
    passwordController =
        TextEditingController(text: widget.deviceInfo['wifi_password'] ?? '');
    deviceKeyController =
        TextEditingController(text: widget.deviceInfo['key'] ?? '');
  }

  @override
  void dispose() {
    _idController.dispose();
    ssidController.dispose();
    passwordController.dispose();
    deviceKeyController.dispose();
    super.dispose();
  }

  String generateRandomKey() {
    const chars = '123456789';
    final rand = Random();
    return List.generate(16, (index) => chars[rand.nextInt(chars.length)]).join();
  }

  void showAlert(BuildContext context, String message) {
    showDialog(
      context: context,
      builder: (BuildContext context) {
        return AlertDialog(
          title: const Text("پیام"),
          content: Text(message),
          actions: [
            TextButton(
              child: const Text("باشه"),
              onPressed: () {
                Navigator.of(context).pop();
              },
            ),
          ],
        );
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      appBar: AppBar(
        backgroundColor: Colors.green,
        title: const Text(
          "راه‌اندازی دستگاه",
          style: TextStyle(
              fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold),
        ),
        leading: IconButton(
          icon: const Icon(Icons.arrow_back, color: Colors.white),
          onPressed: () {
            Navigator.pop(context);
          },
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
                TextField(
                  style: const TextStyle(color: Colors.white),
                  controller: _idController,
                  readOnly: true,
                  decoration: InputDecoration(
                    labelText: "Your Device ID:",
                    labelStyle: const TextStyle(color: Colors.white),
                    filled: true,
                    fillColor: Colors.grey.shade800,
                    border: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(10),
                      borderSide: const BorderSide(color: Colors.grey),
                    ),
                    disabledBorder: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(10),
                      borderSide: const BorderSide(color: Colors.grey),
                    ),
                  ),
                ),

                const SizedBox(height: 50),

                const Text(
                  ".لطفا اطلاعات خود را وارد کنید",
                  style: TextStyle(
                      fontSize: 18,
                      color: Colors.white,
                      fontWeight: FontWeight.bold),
                ),
                const SizedBox(height: 20),
                TextField(
                  style: const TextStyle(color: Colors.white),
                  controller: ssidController,
                  decoration: InputDecoration(
                    labelText: "WIFI SSID:",
                    labelStyle: const TextStyle(color: Colors.white),
                    filled: false,
                    border: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(10),
                    ),
                  ),
                  keyboardType: TextInputType.text,
                ),
                const SizedBox(height: 15),
                TextField(
                  style: const TextStyle(color: Colors.white),
                  controller: passwordController,
                  decoration: InputDecoration(
                    labelText: "WIFI Password:",
                    labelStyle: const TextStyle(color: Colors.white),
                    filled: false,
                    border: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(10),
                    ),
                  ),
                  keyboardType: TextInputType.text,
                ),
                const SizedBox(height: 15),
                Row(
                  children: [
                    Expanded(
                      child: TextField(
                        style: const TextStyle(color: Colors.white),
                        controller: deviceKeyController,
                        decoration: InputDecoration(
                          labelText: "Device Key:",
                          labelStyle: const TextStyle(color: Colors.white),
                          filled: false,
                          border: OutlineInputBorder(
                            borderRadius: BorderRadius.circular(10),
                          ),
                        ),
                        keyboardType: TextInputType.text,
                      ),
                    ),
                    const SizedBox(width: 10),
                    ElevatedButton(
                      onPressed: () {
                        deviceKeyController.text = generateRandomKey();
                      },
                      style: ElevatedButton.styleFrom(
                        backgroundColor: Colors.orange,
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(10),
                        ),
                      ),
                      child: const Text("تولید کلید تصادفی"),
                    ),
                  ],
                ),
                const SizedBox(height: 20),
                ElevatedButton(
                  onPressed: () async {
                    if (ssidController.text.isNotEmpty &&
                        passwordController.text.isNotEmpty &&
                        deviceKeyController.text.isNotEmpty) {
                      final response = await http.post(
                        Uri.parse('$device_serverUrl/config_device'),
                        headers: {'Content-Type': 'application/json'},
                        body: jsonEncode({
                          "id": widget.deviceInfo['id'],
                          "key": deviceKeyController.text,
                          "setup_status": 1,
                          "wifi_password": passwordController.text,
                          "wifi_ssid": ssidController.text,
                        }),
                      ).timeout(Duration(seconds: 100));

                      if (!context.mounted) return;
                      if (response.statusCode == 200) {
                        showAlert(context, "تنظیمات با موفقیت ذخیره شد.");
                        Navigator.pop(context);
                        Navigator.pop(context);
                      } else if (response.statusCode == 403) {
                        showAlert(context, "دستگاه دیگر قابل تنظیم نیست.");
                        Navigator.pop(context);
                        Navigator.pop(context);
                      } else {
                        showAlert(context, "خطا در ارسال تنظیمات.");
                      }
                    } else {
                      showAlert(context, "لطفا تمام فیلدها را پر کنید.");
                    }
                  },
                  style: ElevatedButton.styleFrom(
                    backgroundColor: Colors.blue,
                    padding: const EdgeInsets.symmetric(
                        horizontal: 50, vertical: 15),
                    shape: RoundedRectangleBorder(
                      borderRadius: BorderRadius.circular(10),
                    ),
                  ),
                  child: const Text(
                    "ثبت",
                    style: TextStyle(
                        fontSize: 16,
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
