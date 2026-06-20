// Add a pill course of treatment.
//
// User picks (in Jalali calendar):
//   • start date + time (when the first dose fires)
//   • interval between doses (preset chips: 4/6/8/12/24h + custom in min)
//   • total dose count (stepper)
//   • optional audio reminder
//
// On save: convert Jalali → Gregorian → UTC, persist locally, POST to
// server. Server schedules the alarms; device plays the audio at each
// next_due moment (5-min replay until user says "قرص" → ack → next dose).
import 'package:flutter/material.dart';
import 'package:audioplayers/audioplayers.dart';
import 'package:path_provider/path_provider.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:persian_datetime_picker/persian_datetime_picker.dart';
import 'package:record/record.dart';
import 'package:shamsi_date/shamsi_date.dart';
import 'package:path/path.dart' as p;

import 'db.dart';

class AddPillPage extends StatefulWidget {
  final String deviceId;   // pill belongs to this device
  const AddPillPage({super.key, required this.deviceId});

  @override
  State<AddPillPage> createState() => _AddPillPageState();
}

class _AddPillPageState extends State<AddPillPage> {
  final TextEditingController _nameController = TextEditingController();
  final TextEditingController _doseCountController =
      TextEditingController(text: '21');
  final TextEditingController _customIntervalController =
      TextEditingController(); // minutes

  DateTime? _startAtLocal; // Gregorian/local, user picked via Jalali picker
  int _intervalMinutes = 8 * 60; // default 8h

  final AudioRecorder _recorder = AudioRecorder();
  final AudioPlayer _player = AudioPlayer();
  bool _isRecording = false;
  String? _audioPath;

  static const List<int> _presetMinutes = [
    4 * 60,   // 4h
    6 * 60,   // 6h
    8 * 60,   // 8h
    12 * 60,  // 12h
    24 * 60,  // 24h (daily)
  ];

  @override
  void dispose() {
    _nameController.dispose();
    _doseCountController.dispose();
    _customIntervalController.dispose();
    _recorder.dispose();
    _player.dispose();
    super.dispose();
  }

  Future<void> _pickStart() async {
    // persian_datetime_picker 3.x only ships a date picker — chain a regular
    // Material TimePicker for the hour/minute.
    final jalali = await showPersianDatePicker(
      context: context,
      initialDate: Jalali.now(),
      firstDate: Jalali(1400, 1, 1),
      lastDate: Jalali(1500, 12, 29),
    );
    if (jalali == null) return;
    if (!mounted) return;
    final time = await showTimePicker(
      context: context,
      initialTime: TimeOfDay.now(),
    );
    if (time == null) return;
    final g = jalali.toGregorian();
    setState(() => _startAtLocal = DateTime(
        g.year, g.month, g.day, time.hour, time.minute));
  }

  /// Jalali-string label for a Gregorian DateTime (display only).
  String _formatJalali(DateTime dt) {
    final j = Jalali.fromDateTime(dt);
    final two = (int n) => n.toString().padLeft(2, '0');
    return '${j.day} ${j.formatter.mN} ${j.year} — ${two(dt.hour)}:${two(dt.minute)}';
  }

  String _formatInterval(int minutes) {
    if (minutes % 60 == 0) {
      final hours = minutes ~/ 60;
      return hours == 24 ? 'هر روز' : 'هر $hours ساعت';
    }
    return 'هر $minutes دقیقه';
  }

  Future<void> _toggleRecord() async {
    if (_isRecording) {
      final path = await _recorder.stop();
      setState(() {
        _isRecording = false;
        _audioPath = path;
      });
      return;
    }
    final mic = await Permission.microphone.request();
    if (!mic.isGranted) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('دسترسی به میکروفون داده نشد')),
      );
      return;
    }
    final dir = await getApplicationDocumentsDirectory();
    final path = p.join(dir.path,
        'pill_${DateTime.now().millisecondsSinceEpoch}.m4a');
    if (await _recorder.hasPermission()) {
      // autoGain + noiseSuppress → leveled, clean audio (so the device plays it
      // at a clear, call-like volume instead of near-inaudible).
      await _recorder.start(
          const RecordConfig(autoGain: true, noiseSuppress: true), path: path);
      setState(() => _isRecording = true);
    }
  }

  Future<void> _playPreview() async {
    if (_audioPath == null) return;
    await _player.stop();
    await _player.play(DeviceFileSource(_audioPath!));
  }

  Future<void> _save() async {
    final name = _nameController.text.trim();
    if (name.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('نام قرص را وارد کنید')),
      );
      return;
    }
    if (_startAtLocal == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('زمان شروع را انتخاب کنید')),
      );
      return;
    }
    final doseCount = int.tryParse(_doseCountController.text.trim()) ?? 0;
    if (doseCount < 1) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('تعداد دوزها باید حداقل ۱ باشد')),
      );
      return;
    }
    int intervalMinutes = _intervalMinutes;
    if (_customIntervalController.text.trim().isNotEmpty) {
      final m = int.tryParse(_customIntervalController.text.trim());
      if (m == null || m < 1) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('بازه سفارشی باید عدد مثبت باشد')),
        );
        return;
      }
      intervalMinutes = m;
    }

    // Convert Jalali-picked local DateTime → UTC ISO string. Server stores
    // UTC, app re-renders Jalali on display.
    final startUtc = _startAtLocal!.toUtc();
    final iso = '${startUtc.year.toString().padLeft(4, '0')}-'
        '${startUtc.month.toString().padLeft(2, '0')}-'
        '${startUtc.day.toString().padLeft(2, '0')} '
        '${startUtc.hour.toString().padLeft(2, '0')}:'
        '${startUtc.minute.toString().padLeft(2, '0')}:'
        '${startUtc.second.toString().padLeft(2, '0')}';

    await insertPill(
      deviceId:        widget.deviceId,
      name: name,
      startAtUtc:      iso,
      intervalSeconds: intervalMinutes * 60,
      totalCount:      doseCount,
      audioPath:       _audioPath,
    );
    if (!mounted) return;
    Navigator.pop(context, true);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      appBar: AppBar(
        backgroundColor: Colors.green,
        title: const Text(
          'افزودن قرص',
          style: TextStyle(
              fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold),
        ),
        toolbarHeight: 90.0,
      ),
      body: Padding(
        padding: const EdgeInsets.all(16.0),
        child: SingleChildScrollView(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              const SizedBox(height: 12),
              TextField(
                controller: _nameController,
                style: const TextStyle(color: Colors.white),
                decoration: const InputDecoration(
                  labelText: 'نام قرص',
                  labelStyle: TextStyle(color: Colors.white70),
                  border: OutlineInputBorder(
                      borderSide: BorderSide(color: Colors.white)),
                  enabledBorder: OutlineInputBorder(
                      borderSide: BorderSide(color: Colors.white54)),
                ),
              ),
              const SizedBox(height: 24),
              const Text('زمان اولین مصرف:',
                  style: TextStyle(color: Colors.white, fontWeight: FontWeight.bold)),
              const SizedBox(height: 8),
              ElevatedButton.icon(
                onPressed: _pickStart,
                icon: const Icon(Icons.calendar_today, color: Colors.white),
                label: Text(
                  _startAtLocal == null
                      ? 'انتخاب تاریخ و ساعت شمسی'
                      : _formatJalali(_startAtLocal!),
                  style: const TextStyle(color: Colors.white),
                ),
                style: ElevatedButton.styleFrom(
                  backgroundColor: Colors.blue,
                  padding: const EdgeInsets.symmetric(vertical: 14),
                ),
              ),
              const SizedBox(height: 24),
              const Text('بازه زمانی بین دوزها:',
                  style: TextStyle(color: Colors.white, fontWeight: FontWeight.bold)),
              const SizedBox(height: 8),
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: _presetMinutes.map((m) {
                  final selected = _intervalMinutes == m &&
                      _customIntervalController.text.isEmpty;
                  return ChoiceChip(
                    label: Text(_formatInterval(m)),
                    selected: selected,
                    onSelected: (_) => setState(() {
                      _intervalMinutes = m;
                      _customIntervalController.clear();
                    }),
                    selectedColor: Colors.lightGreenAccent,
                  );
                }).toList(),
              ),
              const SizedBox(height: 8),
              TextField(
                controller: _customIntervalController,
                keyboardType: TextInputType.number,
                style: const TextStyle(color: Colors.white),
                decoration: const InputDecoration(
                  labelText: 'یا بازه سفارشی (دقیقه)',
                  labelStyle: TextStyle(color: Colors.white70),
                  border: OutlineInputBorder(
                      borderSide: BorderSide(color: Colors.white)),
                  enabledBorder: OutlineInputBorder(
                      borderSide: BorderSide(color: Colors.white54)),
                ),
                onChanged: (_) => setState(() {}),
              ),
              const SizedBox(height: 24),
              const Text('تعداد کل دوزها:',
                  style: TextStyle(color: Colors.white, fontWeight: FontWeight.bold)),
              const SizedBox(height: 8),
              TextField(
                controller: _doseCountController,
                keyboardType: TextInputType.number,
                style: const TextStyle(color: Colors.white),
                decoration: const InputDecoration(
                  labelText: 'مثلاً ۲۱ دوز (یک هفته، ۳ بار در روز)',
                  labelStyle: TextStyle(color: Colors.white70),
                  border: OutlineInputBorder(
                      borderSide: BorderSide(color: Colors.white)),
                  enabledBorder: OutlineInputBorder(
                      borderSide: BorderSide(color: Colors.white54)),
                ),
              ),
              const SizedBox(height: 28),
              const Text('صدای یادآور:',
                  style: TextStyle(color: Colors.white, fontWeight: FontWeight.bold)),
              const SizedBox(height: 8),
              Row(
                children: [
                  ElevatedButton.icon(
                    onPressed: _toggleRecord,
                    icon: Icon(_isRecording ? Icons.stop : Icons.mic),
                    label: Text(_isRecording ? 'توقف' : 'ضبط'),
                    style: ElevatedButton.styleFrom(
                      backgroundColor:
                          _isRecording ? Colors.redAccent : Colors.blue,
                      foregroundColor: Colors.white,
                    ),
                  ),
                  const SizedBox(width: 12),
                  if (_audioPath != null)
                    ElevatedButton.icon(
                      onPressed: _playPreview,
                      icon: const Icon(Icons.play_arrow),
                      label: const Text('پخش'),
                      style: ElevatedButton.styleFrom(
                        backgroundColor: Colors.green,
                        foregroundColor: Colors.white,
                      ),
                    ),
                ],
              ),
              if (_audioPath != null)
                const Padding(
                  padding: EdgeInsets.only(top: 8.0),
                  child: Text(
                    'فایل ضبط‌شده آماده است',
                    style: TextStyle(color: Colors.lightGreenAccent),
                  ),
                ),
              const SizedBox(height: 40),
              ElevatedButton(
                onPressed: _save,
                style: ElevatedButton.styleFrom(
                  backgroundColor: Colors.blue,
                  padding: const EdgeInsets.symmetric(vertical: 14),
                ),
                child: const Text(
                  'ذخیره',
                  style: TextStyle(
                      color: Colors.white,
                      fontSize: 18,
                      fontWeight: FontWeight.bold),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
