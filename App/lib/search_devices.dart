
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'package:flutter/material.dart';

// class SearchDevicePage extends StatefulWidget {
//   const SearchDevicePage({super.key});

//   @override
//   State<SearchDevicePage> createState() => _SearchDevicePageState();
// }

// class _SearchDevicePageState extends State<SearchDevicePage> {
//   List<Map<String, dynamic>> devices = [];
//   Timer? _scanTimer;

//   @override
//   void initState() {
//     super.initState();
//     _startScanning();
//   }

//   @override
//   void dispose() {
//     _scanTimer?.cancel(); // توقف اسکن هنگام خروج از صفحه
//     super.dispose();
//   }

//   // 🚀 تابع شروع اسکن دوره‌ای
//   void _startScanning() {
//     _scanTimer = Timer.periodic(const Duration(seconds: 3), (timer) {
//       _scanForDevices();

//     });
//   }

//   // 📥 بررسی فایل‌ها در مسیر مشخص‌شده
//   Future<void> _scanForDevices() async {
    
//   }

//   @override
//   Widget build(BuildContext context) {
//     return Scaffold(
//       backgroundColor: const Color(0xFF37474F),
//       appBar: AppBar(
//         backgroundColor: Colors.green,
//         title: const Text(
//           "جستجوی دستگاه‌ها",
//           style: TextStyle(
//               fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold),
//         ),
//         toolbarHeight: 90.0,
//       ),

//       // 🖼️ نمایش لیست دستگاه‌ها یا پیام عدم وجود دستگاه
//       body: devices.isEmpty
//           ? const Center(
//               child: Column(
//                 mainAxisAlignment: MainAxisAlignment.center,
//                 children: [
//                   CircularProgressIndicator(color: Colors.green),
//                   SizedBox(height: 16),
//                   Text(
//                     '...در حال جستجوی دستگاه‌ها',
//                     style: TextStyle(fontSize: 20, color: Colors.white),
//                   ),
//                 ],
//               ),
//             )
//           : ListView.builder(
//               itemCount: devices.length,
//               itemBuilder: (context, index) {
//                 final device = devices[index];

//                 return Card(
//                   color: Colors.white,
//                   margin: const EdgeInsets.symmetric(vertical: 8, horizontal: 16),
//                   elevation: 4,
//                   shape: RoundedRectangleBorder(
//                     borderRadius: BorderRadius.circular(12),
//                   ),
//                   child: InkWell(
//                     borderRadius: BorderRadius.circular(12),
//                     onTap: () {
//                       // Navigator.push(
//                       //   context,
//                       //   MaterialPageRoute(
//                       //     builder: (context) => SetUpPage(device: device),
//                       //   ),
//                       // );
//                     },
//                     child: Padding(
//                       padding: const EdgeInsets.all(12),
//                       child: Row(
//                         children: [
//                           const Icon(Icons.devices, color: Colors.green, size: 40),
//                           const SizedBox(width: 16),
//                           Expanded(
//                             child: Column(
//                               crossAxisAlignment: CrossAxisAlignment.start,
//                               children: [
//                                 Text(
//                                   "شناسه دستگاه: ${device['id']}",
//                                   style: const TextStyle(
//                                       fontSize: 18, fontWeight: FontWeight.bold),
//                                 ),
//                               ],
//                             ),
//                           ),
//                           const Icon(Icons.arrow_forward_ios,
//                               size: 20, color: Colors.grey),
//                         ],
//                       ),
//                     ),
//                   ),
//                 );
//               },
//             ),
//     );
//   }
// }




import 'dart:async';
import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'configs.dart';
import 'setup_page.dart';

class SearchDevicePage extends StatefulWidget {
  const SearchDevicePage({super.key});

  @override
  State<SearchDevicePage> createState() => _SearchDevicePageState();
}

class _SearchDevicePageState extends State<SearchDevicePage> {
  List<Map<String, dynamic>> devices = [];
  Timer? _scanTimer;
  bool _scanning = false; // overlap guard: skip a tick if a scan is in flight

  @override
  void initState() {
    super.initState();
    _startScanning();
  }

  @override
  void dispose() {
    _scanTimer?.cancel(); // توقف اسکن هنگام خروج از صفحه
    super.dispose();
  }

  // 🚀 تابع شروع اسکن دوره‌ای
  void _startScanning() {
    _scanTimer = Timer.periodic(const Duration(seconds: 3), (timer) {
      _scanForDevices();
    });
  }

  // 📥 ارسال درخواست HTTP برای یافتن دستگاه‌ها
  Future<void> _scanForDevices() async {
    if (_scanning) return; // a scan is already in flight — skip this 3s tick
    _scanning = true;
    try {
      final response = await http
          .get(
            Uri.parse('${device_serverUrl}/configurable_devices'),
            headers: {'Content-Type': 'application/json'},
          )
          .timeout(const Duration(seconds: 10)); // fail fast; never hang the timer

      if (response.statusCode == 200) {
        final List<dynamic> responseData = json.decode(response.body);
        if (!mounted) return;
        setState(() {
          devices = List<Map<String, dynamic>>.from(responseData);
        });
      } else {
        debugPrint('خطا در دریافت داده‌ها: ${response.statusCode}');
      }
    } catch (e) {
      debugPrint('خطا در برقراری ارتباط: $e');
    } finally {
      _scanning = false;
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      appBar: AppBar(
        backgroundColor: Colors.green,
        title: const Text(
          "جستجوی دستگاه‌ها",
          style: TextStyle(
              fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold),
        ),
        toolbarHeight: 90.0,
      ),

      // 🖼️ نمایش لیست دستگاه‌ها یا پیام عدم وجود دستگاه
      body: devices.isEmpty
          ? const Center(
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  CircularProgressIndicator(color: Colors.green),
                  SizedBox(height: 16),
                  Text(
                    '...در حال جستجوی دستگاه‌ها',
                    style: TextStyle(fontSize: 20, color: Colors.white),
                  ),
                ],
              ),
            )
          : ListView.builder(
              itemCount: devices.length,
              itemBuilder: (context, index) {
                final device = devices[index];

                return Card(
                  color: Colors.white,
                  margin: const EdgeInsets.symmetric(vertical: 8, horizontal: 16),
                  elevation: 4,
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(12),
                  ),
                  child: InkWell(
                    borderRadius: BorderRadius.circular(12),
                    onTap: () {
                      Navigator.push(
                        context,
                        MaterialPageRoute(
                          builder: (context) => SetUpPage(deviceInfo:device),
                        ),
                      );
                    },
                    child: Padding(
                      padding: const EdgeInsets.all(12),
                      child: Row(
                        children: [
                          const Icon(Icons.devices, color: Colors.green, size: 40),
                          const SizedBox(width: 16),
                          Expanded(
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                Text(
                                  "شناسه دستگاه: ${device['id']}",
                                  style: const TextStyle(
                                      fontSize: 18, fontWeight: FontWeight.bold),
                                ),
                                const SizedBox(height: 4),
                                Text(
                                  "وضعیت تنظیمات: ${device['setup_status'] == 0 ? 'تنظیم نشده' : 'تنظیم شده'}",
                                  style: const TextStyle(color: Colors.grey),
                                ),
                              ],
                            ),
                          ),
                          const Icon(Icons.arrow_forward_ios,
                              size: 20, color: Colors.grey),
                        ],
                      ),
                    ),
                  ),
                );
              },
            ),
    );
  }
}

