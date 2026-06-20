import 'package:flutter/material.dart';

import 'call_page.dart';
import 'devices.dart';
import 'subscribers_page.dart';
import 'voice_inbox.dart';
import 'pills_page.dart';

/// Entry screen when the user taps a device card on the main page.
/// Three big tiles: Video Call, Fall Events, Audio Messages. The app bar
/// also exposes the subscriber-management screen (kick / leave).
///
/// The device card's thumbnail is just a non-interactive image — every
/// tap on the card lands here regardless of where the user pressed.
class DeviceHubPage extends StatelessWidget {
  final Map<String, dynamic> device;
  const DeviceHubPage({super.key, required this.device});

  @override
  Widget build(BuildContext context) {
    final deviceId = device['device_id'] as String;
    final deviceName = (device['device_name'] as String?) ?? 'دستگاه';

    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      appBar: AppBar(
        backgroundColor: Colors.green,
        title: Text(
          deviceName,
          style: const TextStyle(
              fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold),
        ),
        leading: IconButton(
          icon: const Icon(Icons.arrow_back, color: Colors.white),
          onPressed: () => Navigator.pop(context),
        ),
        actions: [
          IconButton(
            icon: const Icon(Icons.people, color: Colors.white),
            tooltip: 'مشترکین این دستگاه',
            onPressed: () => Navigator.push(
              context,
              MaterialPageRoute(
                builder: (_) => SubscribersPage(deviceId: deviceId),
              ),
            ),
          ),
        ],
      ),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            _HubTile(
              icon: Icons.video_call,
              iconColor: Colors.green,
              label: 'تماس تصویری',
              subtitle: 'گفت‌وگوی زنده با دستگاه',
              emphasized: true,
              onTap: () => Navigator.push(
                context,
                MaterialPageRoute(
                  builder: (_) =>
                      CallPage(deviceId: deviceId, deviceName: deviceName),
                ),
              ),
            ),
            const SizedBox(height: 16),
            _HubTile(
              icon: Icons.warning_amber_rounded,
              iconColor: Colors.redAccent,
              label: 'رویدادهای سقوط',
              onTap: () => Navigator.push(
                context,
                MaterialPageRoute(
                  builder: (_) => DeviceDetailsPage(device: device),
                ),
              ),
            ),
            const SizedBox(height: 16),
            _HubTile(
              icon: Icons.mic,
              iconColor: Colors.blueAccent,
              label: 'پیام‌های صوتی',
              onTap: () => Navigator.push(
                context,
                MaterialPageRoute(
                  builder: (_) => Scaffold(
                    backgroundColor: const Color(0xFF37474F),
                    appBar: AppBar(
                      backgroundColor: Colors.green,
                      title: const Text(
                        'پیام‌های صوتی',
                        style: TextStyle(
                            color: Colors.white, fontWeight: FontWeight.bold),
                      ),
                      toolbarHeight: 90.0,
                    ),
                    body: VoiceInboxTab(deviceId: deviceId),
                  ),
                ),
              ),
            ),
            const SizedBox(height: 16),
            // Pill reminders — per device (each device has its own course list).
            _HubTile(
              icon: Icons.medication,
              iconColor: Colors.lightGreen,
              label: 'یادآور قرص',
              subtitle: 'برنامه دارویی این دستگاه',
              onTap: () => Navigator.push(
                context,
                MaterialPageRoute(
                  builder: (_) =>
                      PillsPage(deviceId: deviceId, deviceName: deviceName),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _HubTile extends StatelessWidget {
  final IconData icon;
  final Color iconColor;
  final String label;
  final String? subtitle;
  final bool emphasized;
  final VoidCallback onTap;
  const _HubTile({
    required this.icon,
    required this.iconColor,
    required this.label,
    required this.onTap,
    this.subtitle,
    this.emphasized = false,
  });

  @override
  Widget build(BuildContext context) {
    // Emphasized tile: filled colored circle behind a white icon. Used for the
    // primary action (video call). Other tiles use a plain icon.
    final iconWidget = emphasized
        ? Container(
            width: 56,
            height: 56,
            decoration: BoxDecoration(
              color: iconColor,
              shape: BoxShape.circle,
            ),
            child: Icon(icon, size: 32, color: Colors.white),
          )
        : Icon(icon, size: 40, color: iconColor);

    return Material(
      color: Colors.white,
      borderRadius: BorderRadius.circular(12),
      elevation: 4,
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.all(20),
          child: Row(
            children: [
              iconWidget,
              const SizedBox(width: 16),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text(
                      label,
                      // Explicit dark color: the tile is white but the app's
                      // dark theme defaults text to light, which rendered the
                      // labels white-on-white (invisible).
                      style: const TextStyle(
                          fontSize: 18, fontWeight: FontWeight.bold,
                          color: Colors.black87),
                    ),
                    if (subtitle != null) ...[
                      const SizedBox(height: 4),
                      Text(
                        subtitle!,
                        style: const TextStyle(
                            fontSize: 13, color: Colors.grey),
                      ),
                    ],
                  ],
                ),
              ),
              const Icon(Icons.chevron_right, color: Colors.grey),
            ],
          ),
        ),
      ),
    );
  }
}
