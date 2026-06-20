import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'package:shamsi_date/shamsi_date.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'configs.dart';
import 'db.dart';
import 'add_pill_page.dart';
import 'services/secure_storage.dart';

/// Render "starts 20 مهر 1405 — 20:05, every 8h, 21 doses" from a pill row.
String _describeSchedule(Map<String, dynamic> p) {
  final iso = p['start_at_utc'] as String?;
  final intervalSec = p['interval_seconds'] as int? ?? 0;
  final total = p['total_count'] as int? ?? 0;
  String when = '—';
  if (iso != null && iso.isNotEmpty) {
    final dt = DateTime.parse(iso.replaceAll(' ', 'T') + 'Z').toLocal();
    final j = Jalali.fromDateTime(dt);
    final two = (int n) => n.toString().padLeft(2, '0');
    when = '${j.day} ${j.formatter.mN} ${j.year} — ${two(dt.hour)}:${two(dt.minute)}';
  }
  String every;
  if (intervalSec % 3600 == 0) {
    final h = intervalSec ~/ 3600;
    every = h == 24 ? 'هر روز' : 'هر $h ساعت';
  } else {
    every = 'هر ${(intervalSec / 60).round()} دقیقه';
  }
  return 'شروع: $when\n$every — $total دوز';
}

class PillsPage extends StatefulWidget {
  // Pills are per-device — this page shows/edits only this device's course list.
  final String deviceId;
  final String? deviceName;
  const PillsPage({super.key, required this.deviceId, this.deviceName});

  @override
  State<PillsPage> createState() => _PillsPageState();
}

class _PillsPageState extends State<PillsPage> {
  List<Map<String, dynamic>> _pills = [];
  // pill_id_local -> {'done': int, 'total': int}, fetched from the server
  // (consumptions_done is advanced device-side, not stored locally).
  Map<String, dynamic> _status = {};

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final rows = await getPills(widget.deviceId);
    if (mounted) setState(() => _pills = rows);
    await _loadStatus();
  }

  Future<void> _loadStatus() async {
    try {
      final creds = await AuthStore.creds();
      final username = creds?.username ?? '';
      final password = creds?.password ?? '';
      final r = await http.post(
        Uri.parse('$serverUrl/Pill_Status_App'),
        headers: {
          'Content-Type': 'application/json',
          'Username': username,
          'Password': password,
        },
        body: jsonEncode({'device_id': widget.deviceId}),
      ).timeout(const Duration(seconds: 10));
      if (r.statusCode == 200 && mounted) {
        final data = jsonDecode(r.body);
        setState(() => _status = (data['status'] as Map?)?.cast<String, dynamic>() ?? {});
      }
    } catch (_) {/* offline — cards just show the total without progress */}
  }

  Future<void> _delete(int id) async {
    await deletePill(id, widget.deviceId);
    await _load();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      appBar: AppBar(
        backgroundColor: Colors.green,
        title: Text(
          widget.deviceName != null && widget.deviceName!.isNotEmpty &&
                  widget.deviceName != widget.deviceId
              ? 'یادآور قرص — ${widget.deviceName}'
              : 'یادآور قرص',
          style: const TextStyle(fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold),
        ),
        toolbarHeight: 90.0,
      ),
      body: _pills.isEmpty
          ? const Center(
              child: Text(
                'هنوز قرصی اضافه نشده است',
                style: TextStyle(color: Colors.white, fontSize: 18),
              ),
            )
          : ListView.builder(
              padding: const EdgeInsets.all(12),
              itemCount: _pills.length,
              itemBuilder: (context, i) {
                final p = _pills[i];
                // Consumption progress: prefer server-reported done; total from
                // the server too, falling back to the locally-stored total.
                final key = p['pill_id'].toString();
                final st = _status[key] as Map?;
                // JSON numbers can decode as num/double — coerce, never `as int`.
                final total = (st?['total'] as num?)?.toInt()
                    ?? (p['total_count'] as num?)?.toInt() ?? 0;
                final done = (st?['done'] as num?)?.toInt() ?? 0;
                final bool complete = total > 0 && done >= total;
                return Card(
                  color: const Color(0xFF455A64),
                  margin: const EdgeInsets.symmetric(vertical: 6),
                  child: ListTile(
                    leading: const Icon(Icons.medication,
                        color: Colors.lightGreenAccent, size: 32),
                    title: Row(
                      children: [
                        Expanded(
                          child: Text(p['name'] as String,
                              style: const TextStyle(
                                  color: Colors.white, fontWeight: FontWeight.bold)),
                        ),
                        // "{done}/{total}" doses-taken counter. Turns green when
                        // the whole course is complete.
                        Container(
                          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                          decoration: BoxDecoration(
                            color: complete ? Colors.green : Colors.black26,
                            borderRadius: BorderRadius.circular(12),
                          ),
                          child: Text(
                            '$done/$total',
                            style: const TextStyle(
                                color: Colors.white,
                                fontWeight: FontWeight.bold,
                                fontSize: 14),
                          ),
                        ),
                      ],
                    ),
                    subtitle: Text(
                      _describeSchedule(p),
                      style: const TextStyle(color: Colors.white70),
                    ),
                    isThreeLine: true,
                    trailing: IconButton(
                      icon: const Icon(Icons.delete, color: Colors.redAccent),
                      onPressed: () => _delete(p['pill_id'] as int),
                    ),
                  ),
                );
              },
            ),
      floatingActionButton: FloatingActionButton.extended(
        backgroundColor: Colors.blue,
        icon: const Icon(Icons.add),
        label: const Text('افزودن قرص'),
        onPressed: () async {
          final added = await Navigator.push<bool>(
            context,
            MaterialPageRoute(builder: (_) => AddPillPage(deviceId: widget.deviceId)),
          );
          if (added == true) {
            await _load();
          }
        },
      ),
    );
  }
}
