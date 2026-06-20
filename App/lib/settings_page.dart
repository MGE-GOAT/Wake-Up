import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'dart:convert';
import 'dart:async';
import 'package:sqflite/sqflite.dart';
import 'package:path/path.dart';

import 'package:shared_preferences/shared_preferences.dart';
import 'configs.dart';
import 'password_setup_page.dart';
import 'db.dart';
import 'switch_wifi_page.dart';


// // صفحه تنظیمات (فرضی)
// class SettingsPage extends StatelessWidget {
//   @override
//   Widget build(BuildContext context) {
//     return Scaffold(
//       appBar: AppBar(
//         title: Text('تنظیمات'),
//       ),
//       body: Center(
//         child: Text('صفحه تنظیمات'),
//       ),
//     );
//   }
// }



// صفحه تنظیمات
class SettingsPage extends StatelessWidget {
  const SettingsPage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('تنظیمات'),
      ),
      body: ListView(
        children: [
          // Change a device's Wi-Fi over BLE (keeps its identity — no factory reset)
          ListTile(
            leading: const Icon(Icons.wifi_protected_setup, color: Colors.blue),
            title: const Text('تغییر شبکه دستگاه'),
            subtitle: const Text('انتقال یک دستگاه به شبکهٔ Wi-Fi جدید'),
            trailing: const Icon(Icons.chevron_left),
            onTap: () => Navigator.of(context).push(
              MaterialPageRoute(builder: (_) => const SwitchWifiPage()),
            ),
          ),
          const Divider(),
          ListTile(
            leading: const Icon(Icons.delete_forever, color: Colors.red),
            title: const Text('پاک کردن همه رویدادها'),
            onTap: () async {
            // نمایش تاییدیه قبل از پاک کردن
            final confirm = await showDialog<bool>(
              context: context,
              builder: (context) => AlertDialog(
                title: const Text('تایید حذف'),
                content: const Text('آیا مطمئن هستید که می‌خواهید همه رویدادها حذف شوند؟'),
                actions: [
                  TextButton(
                    onPressed: () => Navigator.of(context).pop(false),
                    child: const Text('لغو'),
                  ),
                  TextButton(
                    onPressed: () => Navigator.of(context).pop(true),
                    child: const Text('تایید'),
                  ),
                ],
              ),
            );

            if (confirm == true) {
              await deleteAllEvents(); // فراخوانی تابع حذف همه رویدادها
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('همه رویدادها پاک شدند.')),
              );
            }
          },
          ),
        ],
      ),
    );
  }
}
