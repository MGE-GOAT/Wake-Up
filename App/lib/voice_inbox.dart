import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'package:path_provider/path_provider.dart';
import 'package:path/path.dart' as p;
import 'package:shared_preferences/shared_preferences.dart';
import 'package:audioplayers/audioplayers.dart';
import 'configs.dart';
import 'stream.dart';
import 'db.dart';
import 'services/media_crypto.dart';
import 'services/secure_storage.dart';

class VoiceInboxTab extends StatefulWidget {
  final String deviceId;
  const VoiceInboxTab({super.key, required this.deviceId});

  @override
  State<VoiceInboxTab> createState() => _VoiceInboxTabState();
}

class _VoiceInboxTabState extends State<VoiceInboxTab> {
  List<Map<String, dynamic>> _messages = [];
  bool _loading = true;
  final AudioPlayer _player = AudioPlayer();
  int? _playingId;
  StreamSubscription<String>? _voiceSub;
  StreamSubscription<void>? _completeSub;
  final List<String> _tempFiles = [];
  // WhatsApp/Telegram-style "seen" set: ids of voice messages the caregiver has
  // actually played. Persisted per-device in SharedPreferences so the ✓✓ marker
  // survives restarts. (The blue badge still clears on open; this is the finer
  // per-message "listened" state, and it's per-caregiver.)
  Set<int> _seen = {};
  String get _seenKey => 'voice_seen_${widget.deviceId}';

  @override
  void initState() {
    super.initState();
    _refresh();
    _loadSeen();
    // Opening the inbox = voice messages seen → clear the blue badge.
    markTypeRead(widget.deviceId, 'voice')
        .then((_) => NotificationStream().notify());
    // Auto-refresh when the heartbeat reports a new voice message for this
    // device, so the inbox updates without a manual pull-to-refresh.
    _voiceSub = VoiceStream().stream.listen((deviceId) {
      if (deviceId == widget.deviceId && mounted) _refresh();
    });
  }

  @override
  void dispose() {
    _voiceSub?.cancel();
    _completeSub?.cancel();
    _player.dispose();
    for (final f in _tempFiles) {
      try { final file = File(f); if (file.existsSync()) file.deleteSync(); } catch (_) {}
    }
    super.dispose();
  }

  Future<void> _refresh() async {
    if (!mounted) return;
    setState(() => _loading = true);
    try {
      final creds = await AuthStore.creds();
      final username = creds?.username ?? '';
      final password = creds?.password ?? '';
      final r = await http.post(
        Uri.parse('$serverUrl/Voice_Messages_App'),
        headers: {
          'Content-Type': 'application/json',
          'Username': username,
          'Password': password,
        },
        body: jsonEncode({'device_id': widget.deviceId}),
      ).timeout(const Duration(seconds: 15));
      if (r.statusCode == 200) {
        final data = jsonDecode(r.body);
        if (mounted) {
          setState(() => _messages =
              List<Map<String, dynamic>>.from(data['messages'] ?? []));
        }
      } else if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(content: Text('خطا در دریافت پیام‌های صوتی')));
      }
    } catch (_) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(content: Text('خطا در ارتباط با سرور')));
      }
    }
    if (mounted) setState(() => _loading = false);
  }

  Future<void> _loadSeen() async {
    final prefs = await SharedPreferences.getInstance();
    final ids = prefs.getStringList(_seenKey) ?? const [];
    final parsed = ids.map(int.tryParse).whereType<int>().toSet();
    if (mounted) setState(() => _seen = parsed);
  }

  Future<void> _markSeen(int id) async {
    if (_seen.contains(id)) return;
    if (mounted) setState(() => _seen.add(id));
    final prefs = await SharedPreferences.getInstance();
    await prefs.setStringList(
        _seenKey, _seen.map((e) => e.toString()).toList());
  }

  Future<void> _play(Map<String, dynamic> m) async {
    final id = m['id'] as int;
    final b64 = m['audio_b64'] as String?;
    if (b64 == null || b64.isEmpty) return;
    Uint8List bytes = base64Decode(b64);
    // Voice clips are WAV. When the device encrypts them it sets encrypted=true
    // in the response (XChaCha20-Poly1305, same scheme as fall media); decrypt
    // before playback. Plaintext clips (encrypted!=true) play as-is.
    if (m['encrypted'] == true) {
      try {
        bytes = await decryptFromDevice(deviceId: widget.deviceId, wire: bytes);
      } catch (e) {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(content: Text('رمزگشایی پیام صوتی ناموفق بود')),
          );
        }
        return;
      }
    }
    final dir = await getTemporaryDirectory();
    final path = p.join(dir.path, 'voice_$id.wav');
    await File(path).writeAsBytes(bytes, flush: true);
    if (!_tempFiles.contains(path)) _tempFiles.add(path);
    if (!mounted) return;
    setState(() => _playingId = id);
    await _player.stop();
    _completeSub ??= _player.onPlayerComplete.listen((_) {
      if (mounted) setState(() => _playingId = null);
    });
    await _player.play(DeviceFileSource(path));
    await _markSeen(id); // played = seen (WhatsApp-style ✓✓)
  }

  @override
  Widget build(BuildContext context) {
    if (_loading) {
      return const Center(child: CircularProgressIndicator());
    }
    if (_messages.isEmpty) {
      return RefreshIndicator(
        onRefresh: _refresh,
        child: ListView(
          children: const [
            SizedBox(height: 200),
            Center(
              child: Text(
                'پیام صوتی‌ای دریافت نشده',
                style: TextStyle(color: Colors.white, fontSize: 18),
              ),
            ),
          ],
        ),
      );
    }
    return RefreshIndicator(
      onRefresh: _refresh,
      child: ListView.builder(
        padding: const EdgeInsets.all(12),
        itemCount: _messages.length,
        itemBuilder: (context, i) {
          final m = _messages[i];
          final id = m['id'] as int;
          final occurred = m['occurred'] as String? ?? '';
          final hasAudio = (m['audio_b64'] as String?)?.isNotEmpty == true;
          return Card(
            color: const Color(0xFF455A64),
            margin: const EdgeInsets.symmetric(vertical: 6),
            child: ListTile(
              leading: Icon(
                _playingId == id ? Icons.graphic_eq : Icons.mic,
                color: Colors.lightGreenAccent,
                size: 30,
              ),
              title: Text('پیام صوتی #$id',
                  style: const TextStyle(color: Colors.white, fontWeight: FontWeight.bold)),
              subtitle: Row(
                children: [
                  Expanded(
                    child: Text(occurred,
                        style: const TextStyle(color: Colors.white70)),
                  ),
                  Icon(
                    _seen.contains(id) ? Icons.done_all : Icons.done,
                    size: 16,
                    color: _seen.contains(id)
                        ? Colors.lightBlueAccent
                        : Colors.white38,
                  ),
                  const SizedBox(width: 4),
                  Text(
                    _seen.contains(id) ? 'دیده شد' : 'دیده نشده',
                    style: TextStyle(
                      fontSize: 12,
                      color: _seen.contains(id)
                          ? Colors.lightBlueAccent
                          : Colors.white38,
                    ),
                  ),
                ],
              ),
              trailing: IconButton(
                icon: Icon(
                  _playingId == id ? Icons.stop : Icons.play_arrow,
                  color: Colors.white,
                ),
                onPressed: hasAudio
                    ? () async {
                        if (_playingId == id) {
                          await _player.stop();
                          setState(() => _playingId = null);
                        } else {
                          await _play(m);
                        }
                      }
                    : null,
              ),
            ),
          );
        },
      ),
    );
  }
}
