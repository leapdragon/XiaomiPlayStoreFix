# Xiaomi Play Store Fix

Fixes the Google Play Store on Xiaomi / Redmi / POCO phones running HyperOS or MIUI
(stock, Global, EEA or Xiaomi.eu) where it:

- crashes randomly while browsing or updating apps,
- gets stuck partway through downloads,
- pins the CPU (`com.android.vending` above 100% in `top`).

Magisk module, Zygisk only. No Xposed, LSPosed or Vector needed, and no app or notification.

## The cause
Since Android 14, apps can use a copy of Chrome's network library (Cronet) that ships inside the
ROM, in the Tethering system module (`/apex/com.android.tethering/lib64/libcronet.*.so`). Some
Xiaomi ROMs ship an old, buggy build (seen: `libcronet.114.0.5735.84.so`). The Play Store
uses it, and the process aborts with:

```
Abort message: 'fdsan: attempted to close file descriptor 0, expected to be unowned, actually owned by unique_fd ...'
  #03 /apex/com.android.tethering/lib64/libcronet.114.0.5735.84.so (net::SocketPosix::StopWatchingAndCleanUp)
```

The ROM signs that module itself, so Google Play system updates cannot replace it; only a
Xiaomi firmware update can, and older phones no longer get them.

## The fix
Inside Play Store processes only, the module makes the ROM's Cronet unavailable to Play Store:
`android.net.http.HttpEngine.Builder#build()` throws, and Play Store's
`HttpEngineNativeProvider` reports itself disabled. Play Store then uses its own fallback
network stack. Every other app is untouched.

Credit to [SongXiaoXi/VendingCronetOff](https://github.com/SongXiaoXi/VendingCronetOff), which
found this approach and does it through an Xposed framework. This module is an independent
implementation that needs only Magisk.

## Install
1. Magisk with **Zygisk** enabled (Magisk → Settings → Zygisk).
2. Magisk → Modules → Install from storage → `XiaomiPlayStoreFix-<version>.zip`.
3. Reboot.

Check it is active: `adb logcat -s XiaomiPlayStoreFix` should show
`Phase1: HttpEngine.Builder#build blocked` and `Phase2: HttpEngineNativeProvider disabled`
each time Play Store starts.

To undo it, disable or remove the module in Magisk and reboot.

## How it works
- `native/main.cpp`: the Zygisk module. In `com.android.vending` processes it loads the embedded
  LSPlant (`lsplant-standalone` 6.4) from a memfd and initialises it against libart's on-disk
  symbol table (pre-specialize), then loads the embedded dex (post-specialize). All other
  processes unload the module immediately.
- `java/xiaomiplaystorefix/Main.java`: the hooks. App-classloader hooks are installed from
  `Instrumentation#newApplication`, before Play Store code runs. Hooker classes must be public,
  because LSPlant's generated stub calls them from another package.

## License
MIT for this project's code (`LICENSE`). The zip embeds LSPlant (LGPL-3.0) and
And64InlineHook (MIT); see `THIRD_PARTY_NOTICES.md`.

## Build
`./build.sh` → `build/XiaomiPlayStoreFix-<version>.zip`. Needs JDK 17, Android SDK
(platform 35, build-tools) and NDK; defaults to `~/Android/jdk-17` and `~/Android/Sdk`.
