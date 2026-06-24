import 'package:flutter/material.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'package:persian_datetime_picker/persian_datetime_picker.dart';
import 'package:http/http.dart' as http;
import 'dart:convert';
import 'dart:async';
import 'package:sqflite/sqflite.dart';
import 'package:path/path.dart';

import 'package:shared_preferences/shared_preferences.dart';

import 'main_page.dart';
import 'theme.dart';
import 'signup_page.dart';
import 'db.dart';
import 'login.dart';
import 'password_setup_page.dart';
import 'heartbeat.dart';
import 'services/secure_storage.dart';

import 'package:firebase_core/firebase_core.dart';
// import 'package:firebase_messaging/firebase_messaging.dart';
// import 'firebase_options.dart';
import 'firebase.dart';
// import 'package:flutter_local_notifications/flutter_local_notifications.dart';


void main() async {
  WidgetsFlutterBinding.ensureInitialized();

  // Every startup step is guarded: an uncaught throw here happens BEFORE
  // runApp(), so it would leave the user staring at a permanent black screen
  // (exactly the Firebase-getToken failure we hit on restricted networks).
  // Firebase/notifications are optional; the DB is required, so if it fails we
  // still render (the UI degrades) rather than showing nothing.
  try {
    await Firebase.initializeApp();
  } catch (e) {
    print('Firebase.initializeApp failed (continuing): $e');
  }
  try {
    await NotificationService.instance.initialize();
  } catch (e) {
    print('NotificationService.initialize failed (continuing): $e');
  }

  // One-time security migration: move the account password out of plaintext
  // SharedPreferences into Keystore/Keychain. No-op once migrated; guarded so
  // a secure-storage hiccup can never black-screen startup.
  try {
    await AuthStore.migrateFromPrefs();
  } catch (e) {
    print('AuthStore.migrateFromPrefs failed (continuing): $e');
  }

  bool isLoggedIn = false;
  try {
    final prefs = await SharedPreferences.getInstance();
    isLoggedIn = prefs.getBool('isLoggedIn') == true;
  } catch (e) {
    print('SharedPreferences read failed (continuing): $e');
  }

  bool dbOk = true;
  try {
    await initializeDatabase();
  } catch (e) {
    dbOk = false;
    print('initializeDatabase failed: $e');
  }

  // If the DB never opened, `late Database database` is unassigned and the first
  // getDevices() would throw LateInitializationError deep in the UI. Show a
  // clear recoverable error screen instead of crashing.
  if (!dbOk) {
    runApp(const _StartupErrorApp());
    return;
  }

  runApp(MyApp(isLoggedIn: isLoggedIn));
}

class _StartupErrorApp extends StatelessWidget {
  const _StartupErrorApp();

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      locale: const Locale('fa', 'IR'),
      home: const Directionality(
        textDirection: TextDirection.rtl,
        child: Scaffold(
          backgroundColor: Color(0xFF37474F),
          body: Center(
            child: Padding(
              padding: EdgeInsets.all(24),
              child: Text(
                'خطا در راه‌اندازی برنامه. لطفاً برنامه را ببندید و دوباره باز کنید. '
                'اگر مشکل ادامه داشت، برنامه را حذف و دوباره نصب کنید.',
                textAlign: TextAlign.center,
                style: TextStyle(color: Colors.white, fontSize: 18),
              ),
            ),
          ),
        ),
      ),
    );
  }
}


class MyApp extends StatelessWidget {
  final bool isLoggedIn;
  const MyApp({super.key, required this.isLoggedIn});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: appTheme,
      locale: const Locale('fa', 'IR'),
      supportedLocales: const [Locale('fa', 'IR'), Locale('en')],
      // Persian delegates FIRST so 'fa' resolves to the Jalali-aware
      // localizations from persian_datetime_picker (first match wins) — the
      // Global* fa localizations would format dates as Persian-script
      // GREGORIAN, which is exactly the "calendar isn't solar" bug.
      localizationsDelegates: const [
        PersianMaterialLocalizations.delegate,
        PersianCupertinoLocalizations.delegate,
        GlobalMaterialLocalizations.delegate,
        GlobalWidgetsLocalizations.delegate,
        GlobalCupertinoLocalizations.delegate,
      ],
      // The UI is Persian → force right-to-left app-wide. This fixes text
      // alignment, Row/Padding direction, and auto-mirrors directional icons,
      // which was the root cause of widgets rendering at the wrong edge.
      builder: (context, child) => Directionality(
        textDirection: TextDirection.rtl,
        child: child ?? const SizedBox.shrink(),
      ),
      home: isLoggedIn ? const MainPage() : const LoginPage(),
    );
  }
}














// class DatabaseHelper {
//   static final DatabaseHelper _instance = DatabaseHelper._internal();
//   factory DatabaseHelper() => _instance;

//   static Database? _database;

//   DatabaseHelper._internal();

//   Future<Database> get database async {
//     if (_database != null) return _database!;
//     _database = await _initDatabase();
//     return _database!;
//   }

//   Future<Database> _initDatabase() async {
//     final dbPath = await getDatabasesPath();
//     final path = join(dbPath, 'app_database.db');

//     return await openDatabase(
//       path,
//       version: 1,
//       onCreate: (db, version) async {
//         await db.execute('''
//           CREATE TABLE users(
//             id INTEGER PRIMARY KEY AUTOINCREMENT,
//             name TEXT,
//             email TEXT
//           )
//         ''');
//       },
//     );
//   }

//   Future<void> insertUser(String name, String email) async {
//     final db = await database;
//     await db.insert(
//       'users',
//       {'name': name, 'email': email},
//       conflictAlgorithm: ConflictAlgorithm.replace,
//     );
//   }

//   Future<List<Map<String, dynamic>>> getUsers() async {
//     final db = await database;
//     return await db.query('users');
//   }
// }

// void main() async {
//   final dbHelper = DatabaseHelper();

//   // افزودن کاربر
//   await dbHelper.insertUser('حسن', 'hasan@example.com');

//   // دریافت کاربران
//   final users = await dbHelper.getUsers();
//   print(users);
// }














// Future<void> saveData() async {
//   final prefs = await SharedPreferences.getInstance();
//   await prefs.setString('username', 'hasan');
//   print('Data saved');
// }

// Future<void> readData() async {
//   final prefs = await SharedPreferences.getInstance();
//   final username = prefs.getString('username');
//   print('Saved username: $username');
// }

// void main() async {
//   await saveData();
//   await readData();
// // }
