---
title: Storing Maps and Templates on Android Devices
keywords: Android
parent: The Mapper App for Android
nav_order: 0.3
---

## Storage locations

Android requires apps to use its system document picker for shared files. Use
**Open map** to choose a map, or **Save as** to create one. Mapper retains access
to documents chosen this way, so they remain available after restarting the
app.

Maps kept in shared folders such as `OOMapper` remain on the device when Mapper
is uninstalled. Files in `Android/data/org.openorienteering.mapper/files` are
app storage and are removed on uninstall, so do not use that location as the
only copy of important work.

### Upgrading an older installation

Older Mapper versions recorded direct paths such as
`/storage/emulated/0/OOMapper/map.omap`. Current Android versions no longer let
apps use those paths directly. When Mapper detects them, it asks you to choose
the existing `OOMapper` folder in Android's system picker. Choose the folder
itself. Mapper keeps the maps in place and reconnects recent maps and templates
below that folder.

Files outside the chosen folder may need to be selected once through the system
picker. Do not uninstall Mapper or clear its app data before reconnecting your
files.


## File transfer

You can transfer files from and to a PC via a USB cable. Android supports multiple file transfer protocols.

- MTP or another Android-compatible file transfer application can copy shared
  map folders between Android and a computer.

- Mass Storage makes the storage unavailable for apps for the duration of the connection with the PC.
  (Android also needs to terminate apps which are stored on the volume which is provided as mass storage.)
  Unlike MTP, mass storage does not depend on the media scanner, so all files are always visible.

Note that after mapping, you might want to transfer back not only the modified map but also GPX tracks and templates you painted on.


## Data loss prevention and recovery

*Remember to keep backups and to verify transferred files.*

In some situations, the Mapper app might not be able to properly save data and
shutdown as quickly as requested by the Android operating system, for example
when you start other apps or when the device runs out of power.
