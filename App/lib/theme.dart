// Centralized design tokens for the elderly-care app.
//
// Goal: every color, spacing value, and text style lives here. Visual changes
// (rebrand, dark/light, accessibility scaling) become one-file edits instead
// of grepping across 20 files.
//
// Currently a dark-only theme (#37474F slate background, green accent) to
// match what's already shipping. When we add light mode, expose a
// ThemeMode and pick palettes from MediaQuery.platformBrightness.
import 'package:flutter/material.dart';

class AppPalette {
  // Page background — slate grey that's easy on elderly eyes in low light
  // and gives high contrast for white text without being harsh black.
  static const Color background       = Color(0xFF37474F);
  static const Color surface          = Color(0xFF455A64);   // cards
  static const Color surfaceHighlight = Color(0xFF546E7A);   // selected list rows
  static const Color divider          = Color(0xFF263238);

  // Brand + intent colors.
  static const Color primary    = Color(0xFF2E7D32);  // Green — calm/safe
  static const Color primaryDim = Color(0xFF1B5E20);
  static const Color danger     = Color(0xFFC62828);  // Red — fall, kick
  static const Color warning    = Color(0xFFEF6C00);  // Orange — call pending
  static const Color info       = Color(0xFF0277BD);  // Blue — neutral action

  // Text on the dark surface.
  static const Color textPrimary   = Colors.white;
  static const Color textSecondary = Color(0xB3FFFFFF);   // 70% white
  static const Color textMuted     = Color(0x80FFFFFF);   // 50% white
  static const Color textOnPrimary = Colors.white;
}

class AppSpacing {
  // 4-pt grid — every spacing in the app is a multiple of 4.
  static const double xs = 4;
  static const double sm = 8;
  static const double md = 16;
  static const double lg = 24;
  static const double xl = 32;
  static const double xxl = 48;
}

class AppRadius {
  static const double small  = 6;
  static const double medium = 10;
  static const double large  = 16;
  static const double pill   = 999;
}

/// Responsive breakpoint for "tablet/landscape phone" vs "compact phone".
/// Pages should query this and switch single-column ↔ multi-column.
const double kCompactBreakpoint = 600;

bool isCompact(BuildContext context) =>
    MediaQuery.of(context).size.shortestSide < kCompactBreakpoint;

/// One [ThemeData] for the whole app. Wire in main.dart via
/// MaterialApp(theme: appTheme).
ThemeData get appTheme {
  // TODO Phase 5.3 follow-up: bundle Vazirmatn (assets/fonts/Vazirmatn-*.ttf)
  // and set `fontFamily: 'Vazirmatn'` here. Until the .ttf is checked in,
  // Roboto is the default — Android's font fallback chain still renders
  // Persian glyphs correctly via NotoNaskhArabic.
  const fontFamily = null;
  return ThemeData(
    brightness: Brightness.dark,
    primaryColor: AppPalette.primary,
    scaffoldBackgroundColor: AppPalette.background,
    fontFamily: fontFamily,
    colorScheme: const ColorScheme.dark(
      primary:   AppPalette.primary,
      secondary: AppPalette.info,
      surface:   AppPalette.surface,
      error:     AppPalette.danger,
    ),
    appBarTheme: const AppBarTheme(
      backgroundColor: AppPalette.primary,
      foregroundColor: AppPalette.textOnPrimary,
      toolbarHeight: 64,
      elevation: 2,
      titleTextStyle: TextStyle(
        fontFamily: fontFamily,
        fontSize: 20,
        fontWeight: FontWeight.bold,
        color: AppPalette.textOnPrimary,
      ),
    ),
    cardTheme: CardThemeData(
      color: AppPalette.surface,
      elevation: 2,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(AppRadius.medium),
      ),
      margin: const EdgeInsets.symmetric(
        vertical: AppSpacing.sm, horizontal: AppSpacing.md),
    ),
    elevatedButtonTheme: ElevatedButtonThemeData(
      style: ElevatedButton.styleFrom(
        backgroundColor: AppPalette.primary,
        foregroundColor: AppPalette.textOnPrimary,
        padding: const EdgeInsets.symmetric(
          vertical: AppSpacing.md, horizontal: AppSpacing.lg),
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(AppRadius.medium),
        ),
        textStyle: const TextStyle(
          fontSize: 16, fontWeight: FontWeight.bold,
        ),
      ),
    ),
    inputDecorationTheme: InputDecorationTheme(
      filled: true,
      fillColor: AppPalette.surface.withValues(alpha: 0.5),
      labelStyle: const TextStyle(color: AppPalette.textSecondary),
      border: OutlineInputBorder(
        borderRadius: BorderRadius.circular(AppRadius.medium),
        borderSide: BorderSide.none,
      ),
      contentPadding: const EdgeInsets.symmetric(
        vertical: AppSpacing.md, horizontal: AppSpacing.md),
    ),
    textTheme: const TextTheme(
      headlineMedium: TextStyle(
        fontFamily: fontFamily,
        fontSize: 22, fontWeight: FontWeight.bold,
        color: AppPalette.textPrimary,
      ),
      titleLarge: TextStyle(
        fontFamily: fontFamily,
        fontSize: 18, fontWeight: FontWeight.bold,
        color: AppPalette.textPrimary,
      ),
      bodyLarge: TextStyle(
        fontFamily: fontFamily,
        fontSize: 16, color: AppPalette.textPrimary,
      ),
      bodyMedium: TextStyle(
        fontFamily: fontFamily,
        fontSize: 14, color: AppPalette.textSecondary,
      ),
    ),
    snackBarTheme: const SnackBarThemeData(
      backgroundColor: AppPalette.surface,
      contentTextStyle: TextStyle(
        fontFamily: fontFamily, color: AppPalette.textPrimary,
      ),
    ),
  );
}
