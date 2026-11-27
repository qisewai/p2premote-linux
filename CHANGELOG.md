# Changelog

All notable changes to P2P Remote are documented in this file.

## 0.2.0

- Unified host service lifecycle on Windows and Linux (single system service with client-alive dormant mode and optional unattended mode).
- Linux host plugin parity: remote terminal (POSIX PTY), file transfer, shortcut macros, keyboard lock sync, and clipboard/file-clipboard relay.
- Built-in signed auto-update framework (GitHub Releases manifest polling, Ed25519 + SHA-256 verification, atomic file swap with rollback, and Windows/Linux system service co-update).
- Added signed BSDIFF40 incremental desktop updates with full-package fallback and tag-triggered GitHub release builds.
