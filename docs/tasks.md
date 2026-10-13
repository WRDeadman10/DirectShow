# LAN Phone Camera Driver — Task Backlog

Checked items were tested on the development PC (Windows 11, build 26200) unless noted. Update this file in the same change as the code.

## T1 — DirectShow camera

- [x] Source filter `LanCamFilter.dll` with one output pin, `IAMStreamConfig` and `IKsPropertySet`, 1280x720 at 30 FPS RGB24.
- [x] MJPEG client (`include/mjpeg_decoder.h`): WinHTTP stream read, JPEG start/end marker scan, WIC decode to BGR24, automatic reconnect.
- [x] Registration with `FilterData` in `CLSID_VideoInputDeviceCategory`, plus `regsvr32` entry points (`DllRegisterServer`).
- [x] Fix: output was upside down (top-down frames in a bottom-up RGB24 sample); frames are flipped and letterboxed (`BlitTopDownToBottomUp`; tested in isolation for flip, portrait letterbox, 1080p to 720p).
- [x] Verified: ffmpeg lists the camera and captures live frames.
- [ ] Verify in OBS and Chrome (Electron apps already load the DLL, so enumeration works).

## T2 — Media Foundation camera

- [x] `LanCamMfSource.dll`: `IMFMediaSourceEx`, `IMFMediaStream2`, `IKsControl`, `IMFGetService`, `IMFActivate` factory, NV12 1280x720 at 30 FPS with a token-paced sample thread.
- [x] Class registered through `HKEY_CLASSES_ROOT`; virtual camera created with `MFCreateVirtualCamera` (system lifetime, all users, `KSCATEGORY_VIDEO_CAMERA`).
- [x] Machine-wide session file `%ProgramData%/LanCamera/session.ini` (Users and LOCAL SERVICE modify); `mfsource.log` for the Frame Server process.
- [x] Verified: WinRT enumeration lists "LAN Phone Camera (Windows Virtual Camera)"; Source Reader capture through the Frame Server returned 30.0 fps upright live frames; in-process source test also passes.
- [ ] Open the Windows Camera app window and confirm the live feed; test Teams, Zoom, Chrome, Unity.
- [ ] Test with a real phone (the tests used a mock sender and, once, the viewer pump path was not exercised).

## T3 — Setup tool

- [x] `lancam://` handler, runs as normal user (no UAC), writes both session files.
- [x] Manifest `asInvoker`; install path elevates through UAC once.
- [x] Install: preflight for missing DLLs, in-place registration (no self-copy), real `regsvr32` result, in-use DLL rename-aside, verification that Windows lists the camera, honest summary, exit codes 0/1/2, `setup.log`.
- [x] Fix: `MFCreateVirtualCamera` is resolved from `mfsensorgroup.dll` (the earlier lookup in `mfplat.dll` always failed, so the Windows camera was never registered on a fresh machine).
- [x] `--uninstall`, `--remove-mf`, `--silent`, `--bridge` modes.
- [ ] Exercise the failure message boxes (only the success path was run).

## T4 — Installer

- [x] Static C runtime (`/MT`) for all binaries; only Windows DLLs remain as dependencies.
- [x] Inno Setup script `installer/lan-camera.iss`; `build.bat` runs `ISCC` and writes `lan-camera-setupv<version>.exe` (version from the parent `package.json`, or `LANCAM_VERSION`, else 1.0.0).
- [x] Verified (silent): install exit 0 with camera listed; uninstall removes camera, DirectShow registration and `%ProgramData%/LanCamera`; reinstall works. A mapped `*.old*` DLL remains until reboot (expected).
- [ ] Clean Windows 11 machine/VM, visible wizard, upgrade over a running Chrome without a reboot prompt.
- [ ] Windows 10 and N-edition behavior.
- [ ] Code-sign the installer and DLLs.

## T5 — Open decisions and cleanup

- [ ] Shared-memory bridge (`shm_bridge.h`, `--bridge`, reader in the filter) is dead code: wire it or delete it (DEC-005).
- [ ] Replace deprecated `inet_addr` (warning C4996) in the bridge code or remove it with the bridge.
- [ ] Add automated checks: Frame Server capture test and installer smoke test (scripts used during development lived outside the repository).
- [ ] Camera controls through `IKsControl` (currently none), resolution options, a "no signal" image.
