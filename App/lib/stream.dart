import 'dart:async';

class EventStream {
  static final EventStream _instance = EventStream._internal();
  final StreamController<List<Map<String, dynamic>>> _controller = StreamController.broadcast();

  factory EventStream() {
    return _instance;
  }

  EventStream._internal();

  Stream<List<Map<String, dynamic>>> get stream => _controller.stream;

  void addEvents(List<Map<String, dynamic>> events) {
    _controller.add(events);
  }

  void dispose() {
    _controller.close();
  }
}





class DeviceStream {
  static final DeviceStream _instance = DeviceStream._internal();
  final StreamController<List<Map<String, dynamic>>> _controller = StreamController.broadcast();

  factory DeviceStream() {
    return _instance;
  }

  DeviceStream._internal();

  Stream<List<Map<String, dynamic>>> get stream => _controller.stream;

  void addDevices(List<Map<String, dynamic>> devices) {
    _controller.add(devices);
  }

  void dispose() {
    _controller.close();
  }
}





/// Fires when a new voice message arrives for a device (carries the device_id),
/// so an open VoiceInboxTab can re-fetch instead of waiting for a manual pull.
class VoiceStream {
  static final VoiceStream _instance = VoiceStream._internal();
  final StreamController<String> _controller = StreamController.broadcast();

  factory VoiceStream() {
    return _instance;
  }

  VoiceStream._internal();

  Stream<String> get stream => _controller.stream;

  void notify(String deviceId) {
    _controller.add(deviceId);
  }

  void dispose() {
    _controller.close();
  }
}


class NotificationStream {
  static final NotificationStream _instance = NotificationStream._internal();
  final StreamController<void> _controller = StreamController.broadcast();

  factory NotificationStream() {
    return _instance;
  }

  NotificationStream._internal();

  Stream<void> get stream => _controller.stream;

  /// ارسال یک اعلان به تمام شنوندگان
  void notify() {
    _controller.add(null);
  }

  /// آزادسازی منابع
  void dispose() {
    _controller.close();
  }
}
