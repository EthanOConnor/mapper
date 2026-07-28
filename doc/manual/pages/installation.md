---
title: Installation
last_modified_date: 16 July 2026
nav_order: 0.02
---

OpenOrienteering Mapper runs on iOS and iPadOS, Android, Windows, macOS and
Linux. The maintained iPhone and iPad build requires iOS/iPadOS 18.0 or newer.
Visit <https://www.openorienteering.org/apps/mapper/> to find downloads.
Apart from stable releases, the web site offers packages from other branches
of development.

When the website is able to recognize the visitor's operating system,
the download button will point right to the recommended package, and
another link will bring you to the main page of this release.

The main page of a particular release is also the target of the download button
when the operating system cannot be identified. Apart from important notes,
it has an "Assets" section with the full list of alternative downloads
for Windows, macOS and Android. Signed iOS builds may instead be delivered
through TestFlight, the App Store, or another release-owner distribution
channel named in the release notes.


## Windows

The default download for Windows is the installation program for 64-bit systems
("...-Windows-x64.exe").
Most modern Windows computers will be able to run this version.

For older or otherwise constrained Windows computers, you may choose the 32-bit
version which has "Windows-x32" in it file name.

Apart from the installation program packages ("*.exe"), there are also plain
compressed archives ("*.zip") which can be run without installation, e.g.
from a USB stick.


## Android

The default download for Android is an APK package for ARM CPUs
("...-armeabi-v7a.apk")
Most Android devices will be able to run this version.

More modern ARM based devices will benefit from choosing the 64-bit variant
instead, named "...-arm64-v8a.apk".
There are also devices based on x86-type CPUs, so there are even more 32-bit
("...-x86.apk") and 64-bit ("...-x86_64.apk") variants.

If you are unsure of your CPU type follow these instructions:

<https://android.gadgethacks.com/how-to/android-basics-see-what-kind-processor-you-have-arm-arm64-x86-0168051/>


## iOS and iPadOS

The maintained iOS build supports arm64 iPhone and iPad devices running
iOS/iPadOS 18.0 or newer. Install it through the signed distribution channel
named by the release owner. An unsigned build archive from continuous
integration cannot be installed on a device, and the source repository does
not contain an Apple signing identity or provisioning profile.

After installation, Mapper uses Apple's Files interface instead of maintaining
a separate app-specific document library. See
[The Mapper app for iOS and iPadOS](ios-app.md) before moving an active mapping
project to an iPhone or iPad.


## macOS

For macOS, there is just a single download ("...-macOS.dmg").


## Linux

For Linux, the download links will guide you to Open Build Service where we
provide Linux packages for major distribution:

<https://software.opensuse.org/download.html?project=home%3Adg0yt&package=openorienteering-mapper>.
