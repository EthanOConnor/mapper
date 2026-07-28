# iOS physical release acceptance

Run this gate before accepting a signed iOS release candidate. Repeat it when
the Qt or Xcode toolchain, minimum OS, native document path, lifecycle handling,
Vello surface, or bundle/signing configuration changes. The hosted x86_64
simulator build and unsigned arm64 device build are prerequisites, not
substitutes for this gate.

## Candidate and device setup

- Start from the exact commit whose minimum-runtime and current-SDK iOS CI
  lanes passed. Record both job URLs and both unsigned archive artifact hashes,
  plus the minimum-runtime simulator artifact hash.
- Configure with `ios-release`, then archive and export the generated Xcode
  project with release-owner credentials supplied outside the repository.
  Record the Xcode/SDK, CMake, Qt, Rust, vcpkg baseline, Apple team, signing
  certificate, provisioning profile, export method, archive hash, and IPA hash.
- Verify the archive is arm64, has `IPHONEOS_DEPLOYMENT_TARGET=18.0`, targets
  iPhone and iPad, does not support Mac Catalyst, has the expected
  `org.openorienteering.mapper` identifier, declares location and motion usage,
  uses Qt's single-window-scene lifecycle, and contains no unexpected entitlements.
- Inspect the archived bundle and linked image for the AppIcon asset, expected
  statically linked Qt/plugin/runtime content, PROJ and GDAL data, examples,
  symbol sets, offline manual, and complete dependency notices before
  installing it.
- Use at least one physical iPhone and one physical iPad. Initial platform
  acceptance, and any later minimum-OS change, must include hardware on the
  oldest supported major version. Also cover the current supported OS when it
  differs. Record device model, architecture, OS build, storage state, and
  available memory.
- Prepare independent `.omap`, `.xmap`, and `.ocd` files in **On My iPhone** or
  **On My iPad** and in iCloud Drive. Use a third-party Files provider as an
  additional gate whenever that provider is named as supported for the
  release.

## Installation, UIKit, and lifecycle

1. Install the signed candidate through its intended distribution path. A
   development run directly from Xcode is diagnostic evidence, not release
   acceptance for an exported archive.
2. Launch after a clean install on both device families. Verify the app icon,
   launch transition, status bar, home screen, menus, dialogs, touch targets,
   text input, and system appearance contain no clipped or unreachable UI.
3. Exercise portrait and both landscape orientations on iPhone. Exercise all
   four declared orientations, multitasking sizes, and split view on iPad.
   Content and modal controls must remain inside the UIKit safe area; rotation
   must not create a duplicate or incorrectly sized Mapper window.
4. Create an unsaved edit, background and resume the app repeatedly, lock and
   unlock the device, then terminate it from the app switcher after it has been
   backgrounded. Settings and autosave state must be durable, relaunch recovery
   must be offered when appropriate, and no stale Metal surface may remain
   after resume.
5. Confirm Mapper does not request location at first launch. Exercise the
   location feature once with permission denied and once with permission
   granted; denial must leave editing usable and the granted path must update
   location/heading without a background-location claim. Activate the compass
   and exercise its motion sensors too; a privacy-key termination is a failure.
6. In **Settings > GNSS**, select a Nordic UART Service Bluetooth LE receiver.
   Confirm the iOS Bluetooth permission has clear purpose text; scan, connect,
   disconnect, reconnect, background, and resume. The application-scoped
   session must survive map-window changes, and live position, fix type,
   satellites, accuracy, track recording, and point placement must use the
   external solution without a duplicate system-location session.
7. Create a TLS NTRIP profile, test it, and enable corrections. Verify the
   caster certificate is validated, RTCM reaches the receiver, correction age
   and traffic update in GNSS details, GGA is sent when required, and an RTK
   fix is represented accurately. Relaunch and confirm the password is
   available from Keychain but absent from Mapper preferences. Repeat a
   rejected-certificate and bad-password case without exposing the password in
   logs or UI.

## Metal rendering and editing

1. Open `examples/complete map.omap` and `examples/overprinting.omap`. Verify
   symbols, paths, text, raster templates, transparency, clipping, and
   overprinting against a desktop reference from the same commit.
2. Pan, pinch-zoom, rotate where supported, select, drag, reshape, draw, undo,
   redo, and change tools with fingers and an Apple Pencil where available.
   The native UIKit Vello surface must receive input and present through Metal;
   toolbar-only interaction or a blank/software fallback surface is a failure.
3. Rotate, resize on iPad, background, and resume while a dense map is visible.
   Verify scale, device-pixel ratio, frame freshness, and touch coordinates
   remain correct and that no old-size frame flashes persistently.
4. Create a small new map from a bundled symbol set. Verify Mapper requires a
   Files format and destination before entering the editor, and that cancelling
   this initial picker returns to the home screen without leaving an untitled
   document. Complete the flow, save some geometry, then reopen it on a desktop
   build from the same commit and verify its geometry and symbols.

## Native document-picker round trips

Mapper deliberately uses a UIKit `UIDocumentPickerViewController` bridge, not
a `UIDocumentBrowserViewController` application shell. Main-document Open and
the destination step of Save As must be the system Files picker while the
ordinary Mapper window remains the application lifecycle owner; Save As's
preceding format choice is a native action sheet.

1. From Mapper's Open action, select local `.omap`, `.xmap`, and `.ocd` files.
   Verify each supported format opens in place, the provider filename is shown,
   recent-document reopening works, and no private copy silently replaces the
   selected document.
2. Edit an `.omap`, save, close, reopen through the picker, and relaunch Mapper.
   The exact edit must survive all three transitions. Repeat from iCloud Drive
   after the file has fully downloaded, then while the device transitions
   offline and online.
3. Use Save As to the local container and iCloud Drive. The native export
   picker must preserve the chosen name and destination; reopen the exported
   document and compare it with the in-memory map. Cancel Open and Save As once
   each and verify the current map and unsaved state are unchanged.
4. From Files, open a Mapper document while Mapper has no active document. The
   `QFileOpenEvent` path must foreground Mapper and open it. Repeat with the
   same document already open, then with a different document open. The same
   document should foreground; the different document must not replace an
   unsaved active map without an explicit close/save decision.
5. Keep an iCloud document open in Mapper and change it from another app or
   device. With no local edits, Mapper must reload the coordinated provider
   change. With local edits, it must warn before discarding or overwriting
   either version; test both Cancel and the explicit destructive choice.
6. Move or rename the open file in Files. Mapper must follow the new identity
   and continue saving through it. Delete the open provider file; Mapper must
   retain the in-memory map, report the deletion, and allow recovery through
   Save As.
7. Repeat an edit/save cycle large enough to remain visibly busy while iCloud
   synchronizes. No partial file, provider-coordination error, stale bookmark,
   duplicate document, or data loss is acceptable. Close Mapper, reopen the
   file after a device restart, and verify the persisted security-scoped access
   path remains valid.
8. With unsaved work present, background or lock the device while each native
   picker is waiting. Relaunch after termination and verify the pre-picker
   recovery checkpoint contains the latest completed edit.
9. Create a recovery conflict, choose the private recovery once and the
   provider version once, then edit and terminate after each choice. The next
   launch must offer the newest completed edit rather than the older conflict
   generation.
10. Begin Save while another device changes the same provider document before
    the write starts. Mapper must either show the overwrite decision for that
    exact provider generation or refuse the compare-and-swap write; it must not
    silently replace an unseen newer generation.

## Bundled and external resources

1. Open the bundled offline manual with networking disabled and follow links
   between several sections. Open the licensing view and verify the generated
   Qt, vcpkg, and Rust notices are readable.
2. Create maps from representative bundled symbol sets and open all bundled
   examples. Load one raster GeoPackage and one GeoJSON external template
   selected through Files; reopen the map after relaunch and record whether the
   provider grants each template URL again. Also select a shapefile, VRT, and a
   raw raster with a world/auxiliary file; Mapper must reject each compound
   dataset clearly rather than load an incomplete primary file.
3. Move the selected external template in Files and reopen the map. Mapper must
   follow the persisted bookmark to the moved identity. Pan and zoom a large
   tiled raster GeoPackage while changing its provider copy; every visible tile must come
   from the coordinated generation loaded by Mapper, without mixed versions or
   post-eviction read failures.
4. Add a painted private template, record a same-day GPS track, and close one
   additional private template. Use Save As, then inspect and reopen both maps.
   Every active and closed private path in the new map must differ from the
   source, both resource sets must remain readable, and changing, promoting, or
   removing a resource in one document must not affect the other. Rename the
   open map and toggle GPS off and on; the existing same-day track must be
   reused rather than duplicated.
5. Promote a painted private template with **Save template**, make another
   template edit, save it in place, then close and reopen the map. The edit must
   survive. Change that Files item from another app after Mapper loads it, make
   a local template edit, and choose **Save template**: Mapper must refuse to
   overwrite the unseen generation and **Save a Copy** must preserve the local
   edit as an independent document.
6. Terminate Mapper after an autosaved private or external-template edit, then
   reopen the map and exercise both restoring and cancelling template recovery.
   After Cancel, perform an unrelated map edit/autosave and relaunch again; the
   deliberately retained template receipt must still be offered.
7. Attempt to close a map while a template edit is unsaved. Cancel must keep the
   map and recovery state intact; each explicit save, copy, or discard choice
   must produce the chosen result without silently cleaning the template.
8. Add an OIC source and a direct XYZ/TMS URL template, pan through the complete
   field extent online, then enable **Work offline for imagery** and revisit it.
   Cached coverage must render without a request; uncached coverage must remain
   absent without escaping offline mode. Exercise the same map after
   background/resume and a device relaunch.
9. Connect Map Hub from the home screen, open one managed assignment, save an
   edit to its native `.omap` workspace, checkpoint it, and submit the exact
   verified revision. Relaunch and confirm account and lease secrets remain in
   Keychain, stale-lease and server-newer conflicts preserve the local file,
   and an independent Files copy remains usable while offline.

## Evidence record

Attach screen recordings of the UIKit/Metal interaction and document-provider
cases, representative screenshots, exported map/output files, the archive and
IPA verification logs, and this completed record to the release candidate.

```text
commit:
minimum-runtime CI job / simulator artifact hash / unsigned archive hash:
current-SDK CI job / unsigned archive hash:
minimum-runtime Xcode and iOS SDK:
current-SDK Xcode and iOS SDK:
signed candidate Xcode and iOS SDK:
CMake / Qt / Rust:
vcpkg baseline:
archive and IPA SHA-256:
bundle identifier / short version / build number:
signing certificate / team / profile / export method:

iPhone model / OS build:
iPad model / OS build:
oldest-supported-major device:
current-major device:

clean install and launch:
safe areas / orientations / iPad multitasking:
suspend / resume / terminate / relaunch:
location denied / granted:
UIKit Metal rendering and editing:
Apple Pencil, if available:

local provider open / save / Save As / relaunch:
iCloud open / save / offline-online / relaunch:
Files launch event, same / different document:
external change, clean / locally modified:
provider move / rename / deletion recovery:
third-party provider, if claimed:

examples / symbol sets / GDAL template:
offline manual / dependency notices:
visual, lifecycle, provider, or data anomalies:

result: PASS / FAIL
tester and date:
```

The gate passes only when the exported, signed candidate completes the checks
on both device families with no unexplained data, provider, Metal, lifecycle,
or signing defect. Simulator-only evidence, an unsigned arm64 bundle, or a
physical run installed from a different commit is useful diagnosis but is not
release acceptance.
