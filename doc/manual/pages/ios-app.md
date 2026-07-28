---
title: The Mapper App for iOS and iPadOS
keywords: iOS, iPadOS, iPhone, iPad, Files
last_modified_date: 27 July 2026
nav_order: 0.05
---

OpenOrienteering Mapper supports iPhone and iPad devices running iOS/iPadOS
18.0 or newer. It uses the touch-mode interface and Apple's native Files and
location services while retaining the same map formats and editing model as
Mapper on other platforms.

## Opening maps from Files

Use **File > Open** to choose a map with the system document picker. Mapper can
also be launched by opening a supported document from the Files app or another
application. The selected document stays in its original Files location;
Mapper does not silently replace it with a private working copy.

This in-place model applies to documents in **On My iPhone** or **On My iPad**,
iCloud Drive, and compatible third-party Files providers. Download a cloud
document before going offline, and allow its provider to finish synchronizing
before moving the document between devices.

Mapper follows a provider rename or move of the open document. If the provider
deletes it, Mapper keeps the map in memory and preserves recovery data, but the
next save must use **Save As** to create a new document.

## Saving and exporting

**Save** writes the map back to the same Files document. **Save As** first asks
for the map format in a native action sheet, then presents the Files export
picker for the filename and destination. Keep the filename extension shown for
the selected format. You can choose a different name, folder, iCloud Drive
location, or compatible provider. Cancelling either step leaves the current
map and its unsaved state unchanged.

Creating a map asks for its Files format and destination before the editor is
entered. This gives the new map a durable provider identity before iOS can
suspend the app. Cancelling that initial destination picker abandons the new
draft and returns to Mapper's home screen.

If a cloud document changes elsewhere, Mapper reloads it when there are no
local edits. When both versions have changed, Mapper asks before discarding or
overwriting work. Keep independent backups of important fieldwork even when a
provider supplies version history.

Access to the main map does not automatically grant access to every external
template referenced by it. Select external templates through Files so Mapper
can preserve an exact security-scoped bookmark. Changes to an external
template are not committed as part of the main map's coordinated save. Use
**Save template** in the Templates view: Mapper refuses to overwrite a file
whose bytes or provider versions changed since load and offers **Save a Copy**
when the original grant is stale or unavailable.

iOS grants durable access to the selected file, not to an unselected family of
GIS sidecars. External templates therefore must be one self-contained Files
document: a Mapper/OCD map, GPX, GeoPackage, or GeoJSON file. Use GeoPackage for
raster or vector data that would otherwise need multiple files. Mapper rejects
raw images, shapefiles, MapInfo datasets, VRTs, KML, and other potentially
compound inputs instead of silently dropping world files, masks, indexes,
schemas, or referenced resources. Mapper-created painted images and tracks are
safe in its private draft storage until you explicitly promote them.

New painted-image and GPS templates begin in Mapper's private draft storage;
**Save template** promotes them to a permanent Files location. Mapper updates
private recovery snapshots when it autosaves or moves to the background and
offers to restore an interrupted external-template edit on the next open.
Resolve any unsaved-template warning before closing the project. **Save As**
forks these private resources too, so editing or promoting a draft from the new
document cannot alter the source document's draft.

## Location and heading

Mapper does not ask for location access at first launch. The first use of a
location feature triggers the standard **While Using the App** permission
request. You can deny it and continue editing normally, or enable access later
in the iOS Settings app.

The iOS build uses the device's location and heading services, and its compass
uses the device motion sensors. iOS may show the corresponding motion-purpose
text when the compass first needs those sensors. Mapper does not request
background location access, so iOS may pause track recording when Mapper is in
the background or the screen is locked. Keep Mapper active when a continuous
track is important.

For higher-accuracy fieldwork, open **Settings > GNSS**, choose **Bluetooth LE
receiver**, select the receiver, and enable **Connect when live GNSS starts**.
The iOS transport supports receivers and bridges which expose the Nordic UART
Service (NUS). The live GNSS control in the map editor connects the selected
receiver and uses its decoded position, accuracy, fix quality, and correction
status in Mapper's existing position, track-recording, and point-placement
tools.

NTRIP correction profiles are configured on the same settings page. Choose
TLS whenever the caster supports it; Mapper verifies the server certificate
and stores the profile password in the Apple Keychain rather than application
preferences. Enable **Use NTRIP corrections** to send the correction stream
back to the connected receiver. The live GNSS detail view shows connection,
fix, satellite, accuracy, correction-age, and NTRIP status. Test the receiver
and caster before leaving network coverage.

## Online imagery and Map Hub

Online imagery works as a tiled template. In the Templates view, add online
imagery from an OIC catalog or define an advanced direct XYZ/TMS URL template,
including its zoom range, tile size, and any required HTTP Referer. Tiles are
cached for reuse, but coverage which has not already been viewed still needs a
network connection. See [Online imagery](online-imagery.md) for source details
and limitations.

Open **Map Hub** from Mapper's home screen to connect an account and open a
project or assignment. Map Hub credentials are stored in the Apple Keychain.
Managed work is saved as a native `.omap` workspace; use the map actions to
checkpoint the exact file revision and submit it for review. Keep local
fieldwork in Files and confirm a successful checkpoint before relying on the
server copy.

## Current platform limits

Mapper edits one map document at a time and remains an ordinary application,
not a replacement Files browser. Provider availability, offline downloads, and
external-template permissions remain under iOS and the selected provider's
control. Support for selecting a whole external dataset package can be added in
a future document model without weakening this one-file safety boundary. Some
keyboard- and mouse-oriented instructions elsewhere in this
manual do not apply to the touch interface; see
[The Touch Mode User Interface](touch-mode.md) for the mobile controls.
