// Entry-choice screen for adding a device. The user picks between:
//   • Connect to an existing device — they got the device-id + 32-char
//     password from whoever set it up. Routes to ConnectDevicePage.
//   • Set up a new device          — first-time onboarding via the button
//     hold + BLE Wi-Fi flow. Routes to SetupNewDevicePage.
//
// Both paths eventually hit different server endpoints
// (/Subscribe_Device vs /Register_Device) — that branching lives inside
// each target screen, so this one is pure navigation.
import 'package:flutter/material.dart';
import 'connect_device_page.dart';
import 'setup_new_device_page.dart';

class AddDeviceEntryPage extends StatelessWidget {
  const AddDeviceEntryPage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      appBar: AppBar(
        backgroundColor: Colors.green,
        title: const Text("افزودن دستگاه",
            style: TextStyle(
                fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold)),
        toolbarHeight: 90.0,
      ),
      body: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 32),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            _ChoiceCard(
              icon: Icons.link,
              titleFa: "اتصال به دستگاه موجود",
              bodyFa: "اگر شناسه و رمز دستگاه را از فرد دیگری دریافت کرده‌اید.",
              onTap: () => Navigator.push(
                context,
                MaterialPageRoute(builder: (_) => const ConnectDevicePage()),
              ),
            ),
            const SizedBox(height: 20),
            _ChoiceCard(
              icon: Icons.add_circle_outline,
              titleFa: "راه‌اندازی دستگاه جدید",
              bodyFa: "اولین بار است که دستگاه را راه‌اندازی می‌کنید — Wi-Fi و رمز را تنظیم کنید.",
              onTap: () => Navigator.push(
                context,
                MaterialPageRoute(builder: (_) => const SetupNewDevicePage()),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _ChoiceCard extends StatelessWidget {
  final IconData icon;
  final String titleFa;
  final String bodyFa;
  final VoidCallback onTap;
  const _ChoiceCard({
    required this.icon,
    required this.titleFa,
    required this.bodyFa,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.white,
      borderRadius: BorderRadius.circular(12),
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.all(20),
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Icon(icon, size: 40, color: Colors.green.shade700),
              const SizedBox(width: 16),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(titleFa,
                        style: const TextStyle(
                            fontSize: 18, fontWeight: FontWeight.bold)),
                    const SizedBox(height: 6),
                    Text(bodyFa,
                        style: TextStyle(
                            fontSize: 14, color: Colors.grey.shade700)),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
