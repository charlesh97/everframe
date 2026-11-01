# Everframe firmware releases and errata

This is the source of truth for released firmware, known defects, affected
hardware, workarounds, and fixed versions. A release is identified by both its
semantic version and its immutable ESP application build fingerprint.

## Release policy

1. Update `embedded/everframe-esp32/version.txt` using semantic versioning.
2. Run cloud tests, website type checking, and a clean ESP-IDF build.
3. Verify the embedded version and build fingerprint from the application image.
4. Tag the source commit `fw-vMAJOR.MINOR.PATCH`.
5. Publish the bootloader, partition table, application image, checksums, and
   release notes together. Never combine binaries from different builds.
6. Record confirmed defects below. Do not silently replace release artifacts;
   issue a patch version instead.

## Firmware 0.1.0

Status: initial hardware-tested release.

Supported hardware:

- Seeed Studio XIAO ESP32-S3 with 8 MB PSRAM and 8 MB flash.
- Waveshare 7.3-inch six-color e-paper panel using the Everframe wiring in the
  firmware documentation.

Known release-blocking errata: none.

Known limitations (intentional, not defects):

- Firmware installation is by USB; signed over-the-air updates are not yet
  implemented.
- Schedule targets are fixed UTC slots. The dashboard displays the equivalent
  local time.
- A short button press opens a five-minute cloud polling window.

## Errata entry format

For each confirmed issue, add:

- ID and title
- Severity: critical, high, medium, or low
- Affected firmware and hardware versions
- Symptoms and reproduction conditions
- Workaround
- Fixed firmware version and source commit
