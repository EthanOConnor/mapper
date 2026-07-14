# Platform support

The forward product currently supports:

- Linux x86-64
- macOS on Apple silicon
- Windows x86-64
- Android arm64-v8a, API 28 or newer

Every supported desktop target must build, test, and package in GitHub Actions.
Android must build both an APK and an AAB, and release candidates require a
physical-device surface and document-access smoke test.

## iOS

iOS is not currently a supported target. The exploratory project proved an iOS
cross-build, but this product line has no maintained preset, package, runtime
surface acceptance, or release owner for it. Support will be reconsidered only
when those four pieces can be maintained together; historical iOS code is not
carried as an untested compatibility promise.

## Physical output

PDF and raster output are covered by automated reference tests. Physical
Windows printing remains a release-candidate acceptance gate because driver
coordinate mapping and native printer properties cannot be established by a
headless hosted runner alone. See `test/manual/windows-print-acceptance.md` for
the repeatable procedure and evidence record.
