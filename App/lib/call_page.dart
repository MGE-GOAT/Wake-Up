import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'package:flutter_webrtc/flutter_webrtc.dart';
import 'package:permission_handler/permission_handler.dart';
import 'configs.dart';
import 'services/secure_storage.dart';

/// Caregiver call screen, talking to the Janus VideoRoom SFU.
///
/// The app joins the device's Janus room as a PUBLISHER (audio only) and
/// SUBSCRIBES to the device's feed (video + audio). Janus forwards the RTP —
/// no transcoding, so the media is smooth. We speak the Janus WebSocket API
/// directly (Dart's built-in WebSocket) so we keep the pinned flutter_webrtc
/// 0.12.x — no janus_client dependency.
class CallPage extends StatefulWidget {
  final String deviceId;
  final String deviceName;
  const CallPage({super.key, required this.deviceId, required this.deviceName});

  @override
  State<CallPage> createState() => _CallPageState();
}

class _CallPageState extends State<CallPage> {
  void _safeSet(VoidCallback fn) { if (mounted) setState(fn); }

  final RTCVideoRenderer _remoteRenderer = RTCVideoRenderer();
  RTCPeerConnection? _pubPc;            // we publish caregiver audio
  RTCPeerConnection? _subPc;            // we subscribe to the device feed
  MediaStream? _localStream;
  WebSocket? _ws;
  int? _sessionId;
  int? _pubHandle;
  int? _subHandle;
  int? _myFeedId;
  int _room = 0;
  Timer? _keepalive;
  final Map<String, Completer<Map<String, dynamic>>> _txns = {};
  int _txnCounter = 0;
  bool _closing = false;
  String _u = '', _p = '';
  String _status = 'در حال آماده‌سازی...';

  @override
  void initState() { super.initState(); _start(); }

  @override
  void dispose() {
    _hangup(notifyServer: false);
    _remoteRenderer.dispose();
    super.dispose();
  }

  String _t() { _txnCounter++; return 'tx$_txnCounter'; }

  String _janusWsUrl() {
    final u = Uri.parse(serverUrl);
    return 'ws://${u.host}:8188';
  }

  Future<void> _start() async {
    await _remoteRenderer.initialize();
    final creds = await AuthStore.creds();
    _u = creds?.username ?? ''; _p = creds?.password ?? '';

    if (!(await Permission.microphone.request()).isGranted) {
      _safeSet(() => _status = 'دسترسی به میکروفون داده نشد');
      return;
    }

    // 1) Tell the server to start the call (device joins) + get the room.
    try {
      final r = await http.post(
        Uri.parse('$serverUrl/Call_Start'),
        headers: {'Content-Type': 'application/json', 'Username': _u, 'Password': _p},
        body: jsonEncode({'device_id': widget.deviceId}),
      ).timeout(const Duration(seconds: 12));
      _room = (jsonDecode(r.body)['room'] as num).toInt();
    } catch (_) {
      _fail('ارتباط با سرور برقرار نشد'); return;
    }

    // 2) Caregiver mic (audio only — one-sided video).
    _localStream = await navigator.mediaDevices
        .getUserMedia({'audio': true, 'video': false});

    // 3) Connect to Janus over WebSocket.
    try {
      _ws = await WebSocket.connect(_janusWsUrl(), protocols: ['janus-protocol'])
          .timeout(const Duration(seconds: 10));
    } catch (_) {
      _fail('اتصال به سرور تماس برقرار نشد'); return;
    }
    _ws!.listen(_onWsMessage, onDone: () { if (!_closing) _endFromPeer(); },
        onError: (_) { if (!_closing) _endFromPeer(); });

    // 4) Janus session + keepalive.
    final sess = await _send({'janus': 'create'});
    _sessionId = (sess['data']?['id'] as num?)?.toInt();
    // _send returns {} on a 15s timeout; without this check the call silently
    // proceeds with a null session and hangs on "در حال اتصال...".
    if (_sessionId == null) { _fail('اتصال به سرور تماس برقرار نشد'); return; }
    _keepalive = Timer.periodic(const Duration(seconds: 25),
        (_) => _send({'janus': 'keepalive'}).catchError((_) => <String, dynamic>{}));

    // 5) Attach VideoRoom (publisher handle) + join as publisher.
    final att = await _send({'janus': 'attach', 'plugin': 'janus.plugin.videoroom'});
    _pubHandle = (att['data']?['id'] as num?)?.toInt();
    if (_pubHandle == null) { _fail('اتصال به سرور تماس برقرار نشد'); return; }
    await _send({'janus': 'message', 'handle_id': _pubHandle,
      'body': {'request': 'join', 'room': _room, 'ptype': 'publisher', 'display': 'caregiver'}});
    _safeSet(() => _status = 'در حال اتصال...');
  }

  void _fail(String msg) {
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(msg)));
      Navigator.pop(context);
    }
  }

  void _endFromPeer() { if (mounted) { _hangup(); Navigator.pop(context); } }

  /// Send a Janus message and await the matching response (by transaction).
  Future<Map<String, dynamic>> _send(Map<String, dynamic> msg) {
    final ws = _ws;
    if (ws == null || _closing) return Future.value(<String, dynamic>{});
    final tx = _t();
    msg['transaction'] = tx;
    if (_sessionId != null && msg['janus'] != 'create') msg['session_id'] = _sessionId;
    final c = Completer<Map<String, dynamic>>();
    _txns[tx] = c;
    ws.add(jsonEncode(msg));
    return c.future.timeout(const Duration(seconds: 15), onTimeout: () {
      _txns.remove(tx); return <String, dynamic>{};
    });
  }

  // Sync wrapper: exceptions thrown inside an `async void` stream listener
  // escape to the root zone and are silently dropped, leaving the call frozen on
  // "در حال اتصال...". Route them to _fail so the user gets feedback and an exit.
  void _onWsMessage(dynamic raw) {
    _handleWsEvent(raw).catchError((e) {
      if (mounted) _fail('خطای ارتباط تماس: $e');
    });
  }

  Future<void> _handleWsEvent(dynamic raw) async {
    if (_closing) return;   // ignore late Janus events after hangup (avoids null _ws)
    final Map<String, dynamic> m;
    try { m = jsonDecode(raw as String) as Map<String, dynamic>; } catch (_) { return; }
    final tx = m['transaction'] as String?;

    // Resolve the request future on success/error (ignore the interim 'ack').
    if (tx != null && _txns.containsKey(tx) && m['janus'] != 'ack' && m['janus'] != 'event') {
      _txns.remove(tx)!.complete(m);
      return;
    }
    if (m['janus'] != 'event') return;

    final data = (m['plugindata']?['data'] as Map<String, dynamic>?) ?? {};
    final jsep = m['jsep'] as Map<String, dynamic>?;
    final sender = (m['sender'] as num?)?.toInt();
    final vr = data['videoroom'];

    if (vr == 'joined') {
      _myFeedId = (data['id'] as num?)?.toInt();
      await _publish();
      for (final p in (data['publishers'] as List? ?? [])) {
        await _subscribe((p['id'] as num).toInt());
      }
    } else if (data['publishers'] != null) {
      for (final p in (data['publishers'] as List)) {
        await _subscribe((p['id'] as num).toInt());
      }
    } else if (data['leaving'] != null || data['unpublished'] != null) {
      _endFromPeer();
    }

    // jsep on the publisher handle = our publish answer; on the subscriber handle = its offer.
    if (jsep != null) {
      if (sender == _pubHandle) {
        await _pubPc?.setRemoteDescription(
            RTCSessionDescription(jsep['sdp'], jsep['type']));
        _safeSet(() => _status = 'برقرار شد');
      } else if (sender == _subHandle) {
        await _answerSubscriber(jsep);
      }
    }
  }

  Future<void> _publish() async {
    final pc = await createPeerConnection({
      'iceServers': [{'urls': 'stun:${Uri.parse(serverUrl).host}:3478'}],
    });
    _pubPc = pc;
    for (final track in _localStream!.getTracks()) {
      await pc.addTrack(track, _localStream!);
    }
    final offer = await pc.createOffer({'offerToReceiveAudio': false, 'offerToReceiveVideo': false});
    await pc.setLocalDescription(offer);
    await _waitIce(pc);
    final ld = await pc.getLocalDescription();
    await _send({'janus': 'message', 'handle_id': _pubHandle,
      'body': {'request': 'configure', 'audio': true, 'video': false},
      'jsep': {'type': ld!.type, 'sdp': ld.sdp}});
  }

  Future<void> _subscribe(int feedId) async {
    if (feedId == _myFeedId || _subHandle != null) return;  // one device feed
    final att = await _send({'janus': 'attach', 'plugin': 'janus.plugin.videoroom'});
    _subHandle = (att['data']?['id'] as num?)?.toInt();
    await _send({'janus': 'message', 'handle_id': _subHandle,
      'body': {'request': 'join', 'room': _room, 'ptype': 'subscriber',
               'streams': [{'feed': feedId}]}});
    // Janus replies with an offer (handled in _onWsMessage → _answerSubscriber).
  }

  Future<void> _answerSubscriber(Map<String, dynamic> offer) async {
    final pc = await createPeerConnection({
      'iceServers': [{'urls': 'stun:${Uri.parse(serverUrl).host}:3478'}],
    });
    _subPc = pc;
    pc.onTrack = (e) {
      if (e.streams.isNotEmpty) {
        _safeSet(() {
          _remoteRenderer.srcObject = e.streams.first;
          _status = 'برقرار شد';   // remote media arrived → show "connected"
        });
      }
      Helper.setSpeakerphoneOn(true);   // device audio → loudspeaker
    };
    await pc.setRemoteDescription(RTCSessionDescription(offer['sdp'], offer['type']));
    final answer = await pc.createAnswer();
    await pc.setLocalDescription(answer);
    await _waitIce(pc);
    final ld = await pc.getLocalDescription();
    await _send({'janus': 'message', 'handle_id': _subHandle,
      'body': {'request': 'start'}, 'jsep': {'type': ld!.type, 'sdp': ld.sdp}});
  }

  Future<void> _waitIce(RTCPeerConnection pc) async {
    if (pc.iceGatheringState ==
        RTCIceGatheringState.RTCIceGatheringStateComplete) return;
    final c = Completer<void>();
    pc.onIceGatheringState = (s) {
      if (s == RTCIceGatheringState.RTCIceGatheringStateComplete && !c.isCompleted) {
        c.complete();
      }
    };
    try { await c.future.timeout(const Duration(seconds: 5)); } catch (_) {}
  }

  Future<void> _hangup({bool notifyServer = true}) async {
    if (_closing) return;
    _closing = true;
    _keepalive?.cancel(); _keepalive = null;
    if (notifyServer) {
      try {
        await http.post(Uri.parse('$serverUrl/Call_Stop'),
          headers: {'Content-Type': 'application/json', 'Username': _u, 'Password': _p},
          body: jsonEncode({'device_id': widget.deviceId})).timeout(const Duration(seconds: 6));
      } catch (_) {}
    }
    try { await _localStream?.dispose(); } catch (_) {}
    _localStream = null;
    try { await _pubPc?.close(); } catch (_) {}
    try { await _subPc?.close(); } catch (_) {}
    _pubPc = null; _subPc = null;
    try { await _ws?.close(); } catch (_) {}
    _ws = null;
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      appBar: AppBar(
        backgroundColor: Colors.green,
        title: Text('تماس با ${widget.deviceName}',
            style: const TextStyle(color: Colors.white, fontWeight: FontWeight.bold)),
      ),
      body: Stack(children: [
        Positioned.fill(
          child: _remoteRenderer.srcObject == null
              ? const Center(child: Text('در حال اتصال...',
                  style: TextStyle(color: Colors.white, fontSize: 22)))
              : RTCVideoView(_remoteRenderer,
                  objectFit: RTCVideoViewObjectFit.RTCVideoViewObjectFitContain),
        ),
        Positioned(left: 0, right: 0, top: 8, child: Center(
          child: Container(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
            decoration: BoxDecoration(color: Colors.black54, borderRadius: BorderRadius.circular(16)),
            child: Text(_status, style: const TextStyle(color: Colors.white)),
          ),
        )),
        Positioned(bottom: 30, left: 0, right: 0, child: Center(
          child: FloatingActionButton(
            backgroundColor: Colors.red,
            child: const Icon(Icons.call_end),
            onPressed: () async { await _hangup(); if (mounted) Navigator.pop(context); },
          ),
        )),
      ]),
    );
  }
}
