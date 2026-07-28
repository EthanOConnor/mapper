# Platform support

The forward product currently supports:

- Linux x86-64
- macOS on Apple silicon
- Windows x86-64
- Android arm64-v8a, API 28 or newer
- iOS and iPadOS 18.0 or newer on arm64 iPhone and iPad

Every supported desktop target must build, test, and package in GitHub Actions.
Android must build both an APK and an AAB, and release candidates require a
physical-device surface and document-access smoke test. iOS must build the
official Qt x86_64 simulator slice and unsigned arm64 archives against both the
minimum-runtime compatibility lane and the current hosted Xcode SDK; signed
iOS release candidates require the separate physical acceptance below.

## iOS

iOS is a maintained native UIKit target, with iOS/iPadOS 18.0 as the deployment
minimum and device families set to iPhone and iPad. The implementation uses
the official Qt 6.11.1 arm64 device and x86_64 simulator binaries. Vello renders
through its UIKit raw-window handle and Metal backend; this is neither a
software-rendering compatibility target nor Mac Catalyst.

The application keeps its Qt lifecycle and uses Mapper's native UIKit
`UIDocumentPickerViewController` bridge for main-document open and export. It is not a
`UIDocumentBrowserViewController` application. Documents selected from Files
remain open in place: Mapper persists the security-scoped bookmark with public
Apple APIs and owns the active file through a `UIDocument` subclass. That
provides coordinated reads and writes plus presentation callbacks for provider
changes, moves, deletion, and conflict state. A provider-initiated save is
refused while the QWidget-owned map is dirty; the explicit GUI save path
serializes it on the main thread.

Hosted arm64 artifacts are deliberately unsigned and cannot be installed as a
release. A release owner must supply the Apple signing identity, provisioning
profile, team, and export policy outside the repository. A signed candidate is
not accepted until it passes `test/manual/ios-release-acceptance.md` on a
physical iPhone and iPad, including Metal interaction, lifecycle, and local and
cloud document-provider round trips. No physical-device pass is claimed merely
from the maintained cross-build or simulator run.

## Physical output

PDF and raster output are covered by automated reference tests. Physical
Windows printing remains a release-candidate acceptance gate because driver
coordinate mapping and native printer properties cannot be established by a
headless hosted runner alone. See `test/manual/windows-print-acceptance.md` for
the repeatable procedure and evidence record.
