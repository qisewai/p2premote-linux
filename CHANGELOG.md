# Changelog

All notable changes to P2P Remote are documented in this file.

## 0.3.1

- Fixed Linux desktop updates that closed the app without reopening it. Release packages now include the required libraries in the GUI directory so existing clients can restart after updating.
- Linux portable updates now check that the new executable can start before replacing the current version. If the check fails, the current version stays available and the updater displays the error.

## 0.3.0

- Unified Linux X11 capture, input, audio, and clipboard around the active desktop session. Hosts without input injection can still share video in view-only mode.
- Added NV12, BGRA, and BGRx frame handling and a Linux host capability probe. Released held input when the desktop session changes.
- Shared reconnect, LAN authentication, drag recovery, and video quality negotiation between Windows and Linux. Improved cloud keepalive and first-frame feedback.
- Moved Qt rendering and host-service operations behind platform adapters. Added a UTF-8 C API with typed render targets and portable event polling while preserving the existing ABI.
- Fixed WinRT factory cleanup when LAN discovery stops and corrected archive checks for unsigned local Linux tarballs.

## 0.2.0

- Unified host service lifecycle on Windows and Linux (single system service with client-alive dormant mode and optional unattended mode).
- Linux host plugin parity: remote terminal (POSIX PTY), file transfer, shortcut macros, keyboard lock sync, and clipboard/file-clipboard relay.
- Built-in signed auto-update framework (GitHub Releases manifest polling, Ed25519 + SHA-256 verification, atomic file swap with rollback, and Windows/Linux system service co-update).
- Added signed BSDIFF40 incremental desktop updates with full-package fallback and tag-triggered GitHub release builds.
