// Wi-Fi provisioning screen.
//
// Talks to the device's wifi_setup_server.py (running on 192.168.4.1:8080
// while the device is in setup mode). Flow shown to the user:
//
//   step 1: instruct the user to press the button on the device
//   step 2: instruct the user to switch their phone's Wi-Fi to the
//           "ElderlyCare_<id>" network
//   step 3: scan nearby networks, pick one, type password, submit
//   step 4: confirmation screen — once posted, the device tears down its AP
//           and joins the chosen network. Phone won't reach 192.168.4.1
//           anymore; user should reconnect to their own Wi-Fi.
import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;

const String _kSetupBase = 'http://192.168.4.1:8080';
const Duration _kHttpTimeout = Duration(seconds: 12);

class WifiSetupPage extends StatefulWidget {
  const WifiSetupPage({super.key});

  @override
  State<WifiSetupPage> createState() => _WifiSetupPageState();
}

class _WifiSetupPageState extends State<WifiSetupPage> {
  int _step = 0;
  bool _busy = false;
  String? _error;

  List<_Network> _networks = [];
  _Network? _selected;
  final TextEditingController _passwordCtrl = TextEditingController();

  @override
  void dispose() {
    _passwordCtrl.dispose();
    super.dispose();
  }

  Future<void> _scan() async {
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      final res = await http
          .get(Uri.parse('$_kSetupBase/scan'))
          .timeout(_kHttpTimeout);
      if (res.statusCode != 200) {
        throw 'سرور دستگاه پاسخ ${res.statusCode} داد';
      }
      final body = jsonDecode(res.body) as Map<String, dynamic>;
      final nets = (body['networks'] as List)
          .map((e) => _Network.fromJson(e as Map<String, dynamic>))
          .toList();
      setState(() {
        _networks = nets;
        _busy = false;
        _step = 2;
      });
    } catch (e) {
      setState(() {
        _busy = false;
        _error = 'اتصال به دستگاه برقرار نشد. '
            'مطمئن شوید گوشی به وای‌فای ElderlyCare_… متصل است.\n($e)';
      });
    }
  }

  Future<void> _connect() async {
    if (_selected == null) return;
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      final res = await http
          .post(
            Uri.parse('$_kSetupBase/connect'),
            headers: const {'Content-Type': 'application/json'},
            body: jsonEncode({
              'ssid': _selected!.ssid,
              'password': _passwordCtrl.text,
            }),
          )
          .timeout(const Duration(seconds: 45));
      final body = jsonDecode(res.body) as Map<String, dynamic>;
      if (res.statusCode == 200) {
        setState(() {
          _busy = false;
          _step = 3;
        });
      } else {
        throw body['detail'] ?? body['error'] ?? 'خطای نامشخص';
      }
    } catch (e) {
      setState(() {
        _busy = false;
        _error = 'اتصال شبکه برقرار نشد. رمز عبور را بررسی کنید.\n($e)';
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
          'راه‌اندازی وای‌فای دستگاه',
          style: TextStyle(color: Colors.white, fontWeight: FontWeight.bold),
        ),
        leading: IconButton(
          icon: const Icon(Icons.arrow_back, color: Colors.white),
          onPressed: () => Navigator.pop(context),
        ),
      ),
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: _buildStep(),
        ),
      ),
    );
  }

  Widget _buildStep() {
    switch (_step) {
      case 0: return _stepPressButton();
      case 1: return _stepJoinAP();
      case 2: return _stepPickNetwork();
      case 3: return _stepDone();
      default: return const SizedBox.shrink();
    }
  }

  Widget _stepPressButton() {
    return _StepCard(
      icon: Icons.touch_app,
      title: '۱. دکمه‌ی دستگاه را فشار دهید',
      body: 'برای حالت تنظیم وای‌فای، دکمه‌ی روی دستگاه را یک بار فشار دهید. '
          'چراغ دستگاه قرمز خواهد شد.',
      actionLabel: 'دکمه را فشار دادم',
      onAction: () => setState(() => _step = 1),
    );
  }

  Widget _stepJoinAP() {
    return _StepCard(
      icon: Icons.wifi,
      title: '۲. به شبکه‌ی دستگاه متصل شوید',
      body: 'از تنظیمات گوشی خود، شبکه‌ای با نام «ElderlyCare_…» را انتخاب کنید '
          '(بدون رمز). سپس به این صفحه بازگردید.',
      actionLabel: _busy ? null : 'اسکن شبکه‌های اطراف',
      onAction: _busy ? null : _scan,
      error: _error,
      busy: _busy,
    );
  }

  Widget _stepPickNetwork() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text(
          '۳. شبکه‌ی وای‌فای خود را انتخاب کنید',
          style: TextStyle(
              fontSize: 18, fontWeight: FontWeight.bold, color: Colors.white),
        ),
        const SizedBox(height: 12),
        Expanded(
          child: Material(
            color: Colors.white,
            borderRadius: BorderRadius.circular(8),
            child: ListView.separated(
              shrinkWrap: true,
              itemCount: _networks.length,
              separatorBuilder: (_, __) => const Divider(height: 1),
              itemBuilder: (_, i) {
                final n = _networks[i];
                final selected = _selected?.ssid == n.ssid;
                return ListTile(
                  leading: Icon(
                    n.signal > 60
                        ? Icons.wifi
                        : n.signal > 30
                            ? Icons.network_wifi_2_bar
                            : Icons.network_wifi_1_bar,
                    color: selected ? Colors.green : Colors.grey,
                  ),
                  title: Text(n.ssid),
                  subtitle: Text(
                      '${n.signal}%${n.secure ? ' • محافظت‌شده' : ' • باز'}'),
                  trailing: selected
                      ? const Icon(Icons.check_circle, color: Colors.green)
                      : null,
                  onTap: () => setState(() => _selected = n),
                );
              },
            ),
          ),
        ),
        if (_selected != null) ...[
          const SizedBox(height: 12),
          TextField(
            controller: _passwordCtrl,
            obscureText: true,
            decoration: InputDecoration(
              hintText: _selected!.secure ? 'رمز عبور وای‌فای' : 'باز — بدون رمز',
              filled: true,
              fillColor: Colors.white,
              border: OutlineInputBorder(
                  borderRadius: BorderRadius.circular(8),
                  borderSide: BorderSide.none),
            ),
            enabled: _selected!.secure,
          ),
        ],
        if (_error != null) ...[
          const SizedBox(height: 8),
          Text(_error!,
              style: const TextStyle(color: Colors.redAccent)),
        ],
        const SizedBox(height: 12),
        SizedBox(
          width: double.infinity,
          height: 48,
          child: ElevatedButton(
            onPressed: (_selected == null || _busy) ? null : _connect,
            style: ElevatedButton.styleFrom(
              backgroundColor: Colors.green,
              foregroundColor: Colors.white,
              shape: RoundedRectangleBorder(
                  borderRadius: BorderRadius.circular(8)),
            ),
            child: _busy
                ? const CircularProgressIndicator(color: Colors.white)
                : const Text('اتصال', style: TextStyle(fontSize: 16)),
          ),
        ),
      ],
    );
  }

  Widget _stepDone() {
    return _StepCard(
      icon: Icons.check_circle,
      iconColor: Colors.green,
      title: '۴. دستگاه به شبکه متصل شد',
      body: 'دستگاه با موفقیت به وای‌فای انتخاب‌شده متصل شد. '
          'حالا گوشی خود را به وای‌فای معمول‌تان بازگردانید.',
      actionLabel: 'بستن',
      onAction: () => Navigator.pop(context),
    );
  }
}

class _StepCard extends StatelessWidget {
  final IconData icon;
  final Color iconColor;
  final String title;
  final String body;
  final String? actionLabel;
  final VoidCallback? onAction;
  final String? error;
  final bool busy;
  const _StepCard({
    required this.icon,
    required this.title,
    required this.body,
    this.actionLabel,
    this.onAction,
    this.iconColor = Colors.green,
    this.error,
    this.busy = false,
  });

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.white,
      borderRadius: BorderRadius.circular(12),
      child: Padding(
        padding: const EdgeInsets.all(20),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(icon, size: 36, color: iconColor),
                const SizedBox(width: 12),
                Expanded(
                  child: Text(title,
                      style: const TextStyle(
                          fontSize: 18, fontWeight: FontWeight.bold)),
                ),
              ],
            ),
            const SizedBox(height: 12),
            Text(body, style: const TextStyle(fontSize: 14)),
            if (error != null) ...[
              const SizedBox(height: 12),
              Text(error!,
                  style: const TextStyle(color: Colors.redAccent)),
            ],
            const SizedBox(height: 16),
            if (actionLabel != null && onAction != null)
              SizedBox(
                width: double.infinity,
                height: 44,
                child: ElevatedButton(
                  onPressed: busy ? null : onAction,
                  style: ElevatedButton.styleFrom(
                    backgroundColor: Colors.green,
                    foregroundColor: Colors.white,
                    shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(8)),
                  ),
                  child: busy
                      ? const CircularProgressIndicator(color: Colors.white)
                      : Text(actionLabel!, style: const TextStyle(fontSize: 15)),
                ),
              ),
          ],
        ),
      ),
    );
  }
}

class _Network {
  final String ssid;
  final int signal;
  final bool secure;
  _Network({required this.ssid, required this.signal, required this.secure});
  factory _Network.fromJson(Map<String, dynamic> j) => _Network(
        ssid: j['ssid'] as String,
        signal: (j['signal'] as num).toInt(),
        secure: j['secure'] as bool,
      );
}
