// Caregiver → device voice note. Hold the button to record, release to review,
// then ✓ to send (played once on the device's amp) or ✗ to discard. Nothing is
// persisted: the recording lives in a temp file that's deleted on send/discard,
// and the server relays it transiently (deleted once the device fetches it).
//
// Recording uses autoGain + noiseSuppress so the captured audio is well-leveled
// and clean — the same reason the WebRTC call sounds good.
import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'package:path_provider/path_provider.dart';
import 'package:record/record.dart';

import 'configs.dart';                // serverUrl
import 'services/secure_storage.dart'; // AuthStore

class VoiceMessageButton extends StatefulWidget {
  final String deviceId;
  const VoiceMessageButton({super.key, required this.deviceId});

  @override
  State<VoiceMessageButton> createState() => _VoiceMessageButtonState();
}

class _VoiceMessageButtonState extends State<VoiceMessageButton> {
  final AudioRecorder _rec = AudioRecorder();
  String? _path; // recorded clip awaiting send/discard
  bool _recording = false;
  bool _sending = false;
  Duration _elapsed = Duration.zero;
  Timer? _timer;

  @override
  void dispose() {
    _timer?.cancel();
    _rec.dispose();
    _cleanup();
    super.dispose();
  }

  Future<void> _cleanup() async {
    final p = _path;
    _path = null;
    if (p != null) {
      try { await File(p).delete(); } catch (_) {}
    }
  }

  Future<void> _start() async {
    if (_recording || _sending) return;
    if (!await _rec.hasPermission()) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(content: Text('دسترسی میکروفون داده نشد')));
      }
      return;
    }
    await _cleanup(); // drop any previous unsent take
    final dir = await getTemporaryDirectory();
    final path = '${dir.path}/v2d_${DateTime.now().millisecondsSinceEpoch}.m4a';
    await _rec.start(
      const RecordConfig(autoGain: true, noiseSuppress: true),
      path: path,
    );
    if (!mounted) return;
    setState(() {
      _recording = true;
      _elapsed = Duration.zero;
    });
    _timer = Timer.periodic(const Duration(milliseconds: 200), (_) {
      if (mounted) setState(() => _elapsed += const Duration(milliseconds: 200));
    });
  }

  Future<void> _stop() async {
    if (!_recording) return;
    _timer?.cancel();
    String? path;
    try { path = await _rec.stop(); } catch (_) {}
    if (!mounted) return;
    // Accidental quick tap (<0.6 s) → discard, don't show the send prompt.
    if (path == null || _elapsed.inMilliseconds < 600) {
      setState(() => _recording = false);
      if (path != null) { try { await File(path).delete(); } catch (_) {} }
      return;
    }
    setState(() {
      _recording = false;
      _path = path;
    });
  }

  Future<void> _send() async {
    final p = _path;
    if (p == null || _sending) return;
    setState(() => _sending = true);
    try {
      final creds = await AuthStore.creds();
      if (creds == null) throw Exception('no creds');
      final req = http.MultipartRequest(
          'POST', Uri.parse('$serverUrl/Voice_To_Device'))
        ..headers['Username'] = creds.username
        ..headers['Password'] = creds.password
        ..fields['device_id'] = widget.deviceId
        ..files.add(await http.MultipartFile.fromPath('audio', p));
      final resp = await req.send().timeout(const Duration(seconds: 20));
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
          content: Text(resp.statusCode == 200
              ? 'پیام صوتی ارسال شد'
              : 'ارسال ناموفق (${resp.statusCode})')));
    } catch (_) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(content: Text('خطا در ارسال پیام صوتی')));
      }
    } finally {
      await _cleanup();
      if (mounted) setState(() => _sending = false);
    }
  }

  Future<void> _discard() async {
    await _cleanup();
    if (mounted) setState(() {});
  }

  String _fmt(Duration d) =>
      '${d.inSeconds}.${(d.inMilliseconds % 1000) ~/ 100}s';

  @override
  Widget build(BuildContext context) {
    if (_sending) {
      return const Padding(
        padding: EdgeInsets.symmetric(vertical: 10),
        child: Row(mainAxisAlignment: MainAxisAlignment.center, children: [
          SizedBox(
              width: 16, height: 16,
              child: CircularProgressIndicator(strokeWidth: 2)),
          SizedBox(width: 8),
          Text('در حال ارسال...'),
        ]),
      );
    }
    if (_path != null) {
      // recorded → confirm send (✓) or discard (✗)
      return Row(mainAxisAlignment: MainAxisAlignment.center, children: [
        const Text('ارسال پیام صوتی؟',
            style: TextStyle(color: Colors.black87, fontWeight: FontWeight.bold)),
        const SizedBox(width: 8),
        IconButton(
            onPressed: _send,
            icon: const Icon(Icons.check_circle, color: Colors.green, size: 34)),
        IconButton(
            onPressed: _discard,
            icon: const Icon(Icons.cancel, color: Colors.red, size: 34)),
      ]);
    }
    // idle / recording — hold to record (styled like the add-device button)
    return GestureDetector(
      onLongPressStart: (_) => _start(),
      onLongPressEnd: (_) => _stop(),
      onLongPressCancel: _stop,
      onTap: () {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
            content: Text('برای ضبط، دکمه را نگه دارید و صحبت کنید')));
      },
      child: AnimatedContainer(
        duration: const Duration(milliseconds: 150),
        height: _recording ? 58 : 44,
        decoration: BoxDecoration(
          color: _recording ? Colors.red : const Color(0xFF1565C0),
          borderRadius: BorderRadius.circular(12),
        ),
        alignment: Alignment.center,
        child: Row(mainAxisAlignment: MainAxisAlignment.center, children: [
          Icon(_recording ? Icons.mic : Icons.mic_none, color: Colors.white),
          const SizedBox(width: 8),
          Text(
            _recording
                ? 'در حال ضبط ${_fmt(_elapsed)} — رها کنید'
                : 'پیام صوتی (نگه دارید)',
            style: const TextStyle(
                color: Colors.white, fontWeight: FontWeight.bold),
          ),
        ]),
      ),
    );
  }
}
