import 'package:flutter/material.dart';
import 'dart:convert';
import 'dart:async';

import 'db.dart';
import 'FullScreenImage.dart';
import 'video_player_screen.dart';
import 'services/media_crypto.dart';

import 'dart:io';


import 'stream.dart';

class DeviceDetailsPage extends StatefulWidget {
  final Map<String, dynamic> device;

  const DeviceDetailsPage({super.key, required this.device});

  @override
  State<DeviceDetailsPage> createState() => _DeviceDetailsPageState();
}


class _DeviceDetailsPageState extends State<DeviceDetailsPage> {
  List<Map<String, dynamic>> events = [];
  bool _loading = true;
  StreamSubscription? _subscription;

  @override
  void initState() {
    super.initState();
    // Viewing the device's events = the fall alerts are now seen → clear the
    // red badge, then nudge the device cards to re-check.
    markTypeRead(widget.device['device_id'].toString(), 'fall')
        .then((_) => NotificationStream().notify());
    _loadEvents();
    // New events land in the local DB first (heartbeat) and the stream just
    // says "something changed" — so RE-QUERY instead of the old blind
    // events.addAll(newEvents), which duplicated rows forever and wasn't even
    // filtered to this device.
    _subscription = EventStream().stream.listen((_) => _loadEvents());
  }

  Future<void> _loadEvents() async {
    try {
      final rows = await getDeviceEvents(widget.device['device_id']);
      // FALL VIEW = fall events ONLY. Defensively drop any non-fall row (e.g. a
      // legacy/mis-tagged "Wake up word") by inspecting its message type, so the
      // view shows fall events and nothing else.
      final falls = rows.where((e) {
        try {
          final t = jsonDecode(e['event_text'] ?? '{}')['Message Type'];
          return t == null || t == 'Event';
        } catch (_) {
          return true; // unparseable → keep so the error card still surfaces it
        }
      }).toList();
      if (!mounted) return;
      setState(() { events = falls; _loading = false; });
    } catch (_) {
      if (!mounted) return;
      setState(() => _loading = false);
    }
  }

  @override
  void dispose() {
    _subscription?.cancel(); // لغو فقط این شنونده
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF37474F),
      appBar: AppBar(
        backgroundColor: Colors.green,
        title: Text(
          widget.device['device_name'] ?? 'جزئیات دستگاه',
          style: const TextStyle(
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
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Expanded(
              child: Builder(
                builder: (context) {
                  if (_loading) {
                    return const Center(child: CircularProgressIndicator());
                  } else if (events.isEmpty) {
                    return const Center(
                        child: Text('هیچ رویدادی برای این دستگاه ثبت نشده است.',
                            style: TextStyle(color: Colors.white)));
                  } else {
                    return ListView.builder(
                      itemCount: events.length,
                      itemBuilder: (context, index) {
                        final event = events[index];

                        Map<String, dynamic> eventData;
                        try {
                          eventData = jsonDecode(event['event_text']);
                        } catch (e) {
                          return Card(
                            color: Colors.red[50],
                            margin: const EdgeInsets.symmetric(vertical: 8),
                            child: const Padding(
                              padding: EdgeInsets.all(12),
                              child: Text('❌ خطا در پردازش داده‌های رویداد',
                                  style: TextStyle(color: Colors.red)),
                            ),
                          );
                        }

                        // 📌 تشخیص نوع پیام
                        final messageType =
                            eventData['Message Type'] ?? 'Event';

                        if (messageType == "Wake up word") {
                          // حالت Wake up word
                          return WakeUpWordCard(
                            event: event,
                            onMarkAsRead: updateEventStatusAsRead,
                          );
                        } else {
                          // حالت Event (کدی که از قبل داشتی)
                          return EventCard(
                            event: event,
                            onMarkAsRead: updateEventStatusAsRead,
                          );
                        }
                      },
                    );
                  }
                },
              ),
            ),
          ],
        ),
      ),
    );
  }
}


class EventCard extends StatefulWidget {
  final Map<String, dynamic> event;
  final Future<void> Function(String) onMarkAsRead;

  const EventCard({
    Key? key,
    required this.event,
    required this.onMarkAsRead,
  }) : super(key: key);

  @override
  _EventCardState createState() => _EventCardState();
}

class _EventCardState extends State<EventCard> {
  bool isUnread = true;

  @override
  void initState() {
    super.initState();
    isUnread = widget.event['event_status'] == "unread";
  }

  // Lazily fetch the fall clip from the server (only on tap), then play it.
  // Shows a blocking spinner during the download.
  Future<void> _downloadAndPlay(BuildContext context) async {
    showDialog(
      context: context,
      barrierDismissible: false,
      builder: (_) => const Center(child: CircularProgressIndicator()),
    );
    final path = await downloadFallVideo(
      deviceId: widget.event['device_id'].toString(),
      messageId: widget.event['event_id'].toString(),
    );
    if (!context.mounted) return;
    Navigator.pop(context); // close spinner
    if (path == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('دانلود ویدیو ناموفق بود')),
      );
      return;
    }
    Navigator.push(
      context,
      MaterialPageRoute(builder: (_) => VideoPlayerScreen(videoPath: path)),
    );
    // Viewing the fall clip counts as seeing the fall → mark it read (same as
    // the "رویداد جدید" button) so the card flips to ✓✓ "خوانده شده".
    if (isUnread) {
      await widget.onMarkAsRead(widget.event['event_id'].toString());
      if (mounted) setState(() => isUnread = false);
      NotificationStream().notify();
    }
  }

  @override
  Widget build(BuildContext context) {

                          Map<String, dynamic> eventData;
                          try {
                            eventData = jsonDecode(widget.event['event_text']);
                          } catch (e) {
                            // در صورت بروز خطا در پارس کردن JSON
                            return Card(
                              color: Colors.red[50],
                              margin: const EdgeInsets.symmetric(vertical: 8),
                              child: const Padding(
                                padding: EdgeInsets.all(12),
                                child: Text('❌ خطا در پردازش داده‌های رویداد',
                                    style: TextStyle(color: Colors.red)),
                              ),
                            );
                          }


                          // 🚨 مشخص کردن سطح خطر. Danger Level may arrive as
                          // an int, a num, or a numeric String from the server;
                          // coerce defensively so the build never throws.
                          final _dl = eventData['Danger Level'];
                          final int dangerLevel = _dl is int
                              ? _dl
                              : (_dl is num
                                  ? _dl.toInt()
                                  : int.tryParse('$_dl') ?? 1);
                          String dangerLevelFarsi = "خطر";
                          Color dangerColor;
                          IconData dangerIcon;

                          // Color scheme mirrors the Pi-side pain_pipeline.cpp:
                          //   2 = PAIN/fall confirmed         -> red
                          //   1 = UNDECIDED / unclear status  -> yellow
                          //   0 = FINE / no serious danger    -> green
                          switch (dangerLevel) {
                            case 2:
                              dangerLevelFarsi = "خطرناک";
                              dangerColor = Colors.red;
                              dangerIcon = Icons.warning_amber_rounded;
                              break;
                            case 1:
                              dangerLevelFarsi = "وضعیت نامشخص";
                              dangerColor = Colors.yellow.shade700;
                              dangerIcon = Icons.error_outline;
                              break;
                            case 0:
                              dangerLevelFarsi = "بدون خطر جدی";
                              dangerColor = Colors.green;
                              dangerIcon = Icons.check_circle_outline;
                              break;
                            default:
                              dangerColor = Colors.grey;
                              dangerIcon = Icons.info_outline;
                          }



    return Card(
      color: Colors.white,
      margin: const EdgeInsets.symmetric(vertical: 8),
      elevation: 4,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
      ),
      clipBehavior: Clip.antiAlias,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Full-width severity banner — the "notification" cue.
          Container(
            width: double.infinity,
            color: dangerColor,
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
            child: Row(
              children: [
                Icon(dangerIcon, color: Colors.white, size: 22),
                const SizedBox(width: 8),
                Text(
                  dangerLevelFarsi,
                  style: const TextStyle(
                    fontSize: 16,
                    fontWeight: FontWeight.bold,
                    color: Colors.white,
                  ),
                ),
              ],
            ),
          ),

          Padding(
            padding: const EdgeInsets.all(12),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                // 🔔 عنوان رویداد (neutral color, banner already carries severity).
                // The device sends the English "Fall Detection"; show Persian.
                Text(
                  (eventData['Event Title'] == 'Fall Detection'
                          ? 'هشدار سقوط'
                          : eventData['Event Title']) ??
                      eventData['Message Title'] ?? 'رویداد ناشناخته',
                  style: const TextStyle(
                    fontSize: 18,
                    fontWeight: FontWeight.bold,
                    color: Colors.black87,
                  ),
                ),

                const SizedBox(height: 8),

                                  // ⏰ زمان رویداد
                                  Row(
                                    children: [
                                      const Icon(Icons.access_time,
                                          color: Colors.black54, size: 20),
                                      const SizedBox(width: 6),
                                      Text(
                                        eventData['Message Time'] ?? 'نامشخص',
                                        style: const TextStyle(
                                            fontSize: 14,
                                            color: Colors.black87),
                                      ),
                                    ],
                                  ),

                                  const SizedBox(height: 10),

                                  // 📷 نمایش تصویر رویداد (در صورت وجود)
                                  if (widget.event['event_image'] != null &&
                                      widget.event['event_image'].isNotEmpty)
                                    InkWell(
                                      onTap: () {
                                        Navigator.push(
                                          context,
                                          MaterialPageRoute(
                                            builder: (context) =>
                                                FullScreenImage_static(
                                                    imagePath:
                                                        widget.event['event_image']),
                                          ),
                                        );
                                      },
                                      child: Hero(
                                        tag: 'evimg-${widget.event['event_id']}',
                                        child: ClipRRect(
                                          borderRadius:
                                              BorderRadius.circular(8),
                                          child: Image.file(
                                            File(widget.event['event_image']),
                                            height: 150,
                                            width: double.infinity,
                                            // Event thumbnails render at ~360x150
                                            // on most phones — cap decode at 2x.
                                            cacheHeight: 300,
                                            fit: BoxFit.cover,
                                            errorBuilder:
                                                (context, error, stackTrace) {
                                              return Container(
                                                height: 150,
                                                width: double.infinity,
                                                color: Colors.grey[200],
                                                child: const Icon(
                                                    Icons.broken_image,
                                                    size: 50,
                                                    color: Colors.grey),
                                              );
                                            },
                                          ),
                                        ),
                                      ),
                                    ),


                                  // 🎥 ویدیو رویداد — فقط وقتی روی سرور موجود است
                                  // ('remote'). با لمس، کلیپ دانلود و پخش می‌شود
                                  // (تا آن لحظه دانلود نمی‌شود تا فهرست سبک بماند).
                                  if (widget.event['event_video'] == 'remote')
                                    InkWell(
                                      onTap: () => _downloadAndPlay(context),
                                      child: Container(
                                        margin: const EdgeInsets.only(top: 8),
                                        padding: const EdgeInsets.all(8),
                                        decoration: BoxDecoration(
                                          color: Colors.black12,
                                          borderRadius: BorderRadius.circular(8),
                                        ),
                                        child: Row(
                                          children: const [
                                            Icon(Icons.play_circle_fill, color: Colors.red),
                                            SizedBox(width: 8),
                                            Text('دانلود و پخش ویدیو',
                                                style: TextStyle(fontSize: 14)),
                                          ],
                                        ),
                                      ),
                                    ),

            
            if (isUnread)
              Align(
                alignment: Alignment.centerRight,
                child: ElevatedButton.icon(
                  onPressed: () async {
                    await widget
                        .onMarkAsRead(widget.event['event_id'].toString());
                    if (!mounted) return;
                    setState(() {
                      isUnread = false;
                    });
                    NotificationStream().notify();
                  },
                  icon: Icon(Icons.mark_email_read, color: Colors.white),
                  label: Text(
                    'رویداد جدید',
                    style: TextStyle(color: Colors.white),
                  ),
                  style: ElevatedButton.styleFrom(
                    backgroundColor: Colors.red,
                    shape: RoundedRectangleBorder(
                      borderRadius: BorderRadius.circular(8),
                    ),
                    padding: EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                  ),
                ),
              )
            else
              Align(
                alignment: Alignment.centerRight,
                child: ElevatedButton.icon(
                  onPressed: null,
                  icon: Icon(Icons.done_all, color: Colors.grey),
                  label: Text(
                    'خوانده شده',
                    style: TextStyle(color: Colors.grey),
                  ),
                  style: ElevatedButton.styleFrom(
                    backgroundColor: Colors.grey[300],
                    shape: RoundedRectangleBorder(
                      borderRadius: BorderRadius.circular(8),
                    ),
                    padding: EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                  ),
                ),
              )
          ],
        ),
      ),
        ],
      ),
    );
  }
}




class WakeUpWordCard extends StatefulWidget {
  final Map<String, dynamic> event;
  final Future<void> Function(String) onMarkAsRead;

  const WakeUpWordCard({
    Key? key,
    required this.event,
    required this.onMarkAsRead,
  }) : super(key: key);

  @override
  _WakeUpWordCardState createState() => _WakeUpWordCardState();
}

class _WakeUpWordCardState extends State<WakeUpWordCard> {
  bool isUnread = true;

  @override
  void initState() {
    super.initState();
    isUnread = widget.event['event_status'] == "unread";
  }

  @override
  Widget build(BuildContext context) {
    Map<String, dynamic> eventData;
    try {
      eventData = jsonDecode(widget.event['event_text']);
    } catch (e) {
      return Card(
        color: Colors.red[50],
        margin: const EdgeInsets.symmetric(vertical: 8),
        child: const Padding(
          padding: EdgeInsets.all(12),
          child: Text('❌ خطا در پردازش داده‌های Wake up word',
              style: TextStyle(color: Colors.red)),
        ),
      );
    }

    return Card(
      color: Colors.white,
      margin: const EdgeInsets.symmetric(vertical: 8),
      elevation: 4,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
      ),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            // 🔔 عنوان پیام
            Row(
              children: [
                const Icon(Icons.record_voice_over,
                    color: Colors.blue, size: 28),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    eventData['Event Title'] ?? eventData['Message Title'] ?? 'پیام Wake up word',
                    style: const TextStyle(
                        fontSize: 18,
                        fontWeight: FontWeight.bold,
                        color: Colors.blue),
                  ),
                ),
              ],
            ),

            const SizedBox(height: 8),

            // ⏰ زمان پیام
            Row(
              children: [
                const Icon(Icons.access_time,
                    color: Colors.black54, size: 20),
                const SizedBox(width: 6),
                Text(
                  eventData['Message Time'] ?? 'نامشخص',
                  style: const TextStyle(
                      fontSize: 14, color: Colors.black87),
                ),
              ],
            ),

            const SizedBox(height: 10),

            // 📷 نمایش تصویر پیام (در صورت وجود)
            if (widget.event['event_image'] != null &&
                widget.event['event_image'].isNotEmpty)
              InkWell(
                onTap: () {
                  Navigator.push(
                    context,
                    MaterialPageRoute(
                      builder: (context) => FullScreenImage_static(
                          imagePath: widget.event['event_image']),
                    ),
                  );
                },
                child: Hero(
                  tag: 'evimg2-${widget.event['event_id']}',
                  child: ClipRRect(
                    borderRadius: BorderRadius.circular(8),
                    child: Image.file(
                      File(widget.event['event_image']),
                      height: 150,
                      width: double.infinity,
                      cacheHeight: 300,
                      fit: BoxFit.cover,
                      errorBuilder: (context, error, stackTrace) {
                        return Container(
                          height: 150,
                          width: double.infinity,
                          color: Colors.grey[200],
                          child: const Icon(Icons.broken_image,
                              size: 50, color: Colors.grey),
                        );
                      },
                    ),
                  ),
                ),
              ),

            const SizedBox(height: 10),

            // 📝 متن پیام
            Container(
              width: double.infinity,
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: Colors.blue[50],
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: Colors.blue.shade200),
              ),
              child: Text(
                eventData['Message Text'] ?? 'بدون متن',
                style: const TextStyle(fontSize: 16, color: Colors.black87),
                textAlign: TextAlign.right,
                textDirection: TextDirection.rtl,
              ),
            ),
            const SizedBox(height: 12),

            // ✅ دکمه وضعیت خوانده شده
            if (isUnread)
              Align(
                alignment: Alignment.centerRight,
                child: ElevatedButton.icon(
                  onPressed: () async {
                    await widget
                        .onMarkAsRead(widget.event['event_id'].toString());
                    if (!mounted) return;
                    setState(() {
                      isUnread = false;
                    });
                    NotificationStream().notify();
                  },
                  icon: const Icon(Icons.mark_email_read, color: Colors.white),
                  label: const Text(
                    'پیام جدید',
                    style: TextStyle(color: Colors.white),
                  ),
                  style: ElevatedButton.styleFrom(
                    backgroundColor: Colors.red,
                    shape: RoundedRectangleBorder(
                      borderRadius: BorderRadius.circular(8),
                    ),
                    padding: const EdgeInsets.symmetric(
                        horizontal: 12, vertical: 8),
                  ),
                ),
              )
            else
              Align(
                alignment: Alignment.centerRight,
                child: ElevatedButton.icon(
                  onPressed: null,
                  icon: const Icon(Icons.done_all, color: Colors.grey),
                  label: const Text(
                    'خوانده شده',
                    style: TextStyle(color: Colors.grey),
                  ),
                  style: ElevatedButton.styleFrom(
                    backgroundColor: Colors.grey[300],
                    shape: RoundedRectangleBorder(
                      borderRadius: BorderRadius.circular(8),
                    ),
                    padding: const EdgeInsets.symmetric(
                        horizontal: 12, vertical: 8),
                  ),
                ),
              ),
          ],
        ),
      ),
    );
  }
}
