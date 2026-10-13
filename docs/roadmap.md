# LAN Phone Camera Driver — Roadmap

## Phase 1 — DirectShow camera (delivered)

**Objective:** a camera that DirectShow applications can select.

Delivered: in-process source filter, MJPEG client (WinHTTP + WIC), `lancam://` handler and setup tool, registration under the video input category, bottom-up flipped and letterboxed output.

**Exit criteria (met):** ffmpeg lists "LAN Phone Camera" and captures live, upright 1280x720 frames from a running stream.

## Phase 2 — Media Foundation camera (delivered)

**Objective:** make the camera visible to the Windows Camera app, Teams, Zoom, Chrome and Unity, which only enumerate Media Foundation / kernel-streaming cameras.

Delivered: `LanCamMfSource.dll` (media source + activation object), `MFCreateVirtualCamera` registration (system lifetime), machine-wide session file for the Frame Server (LocalService), source and setup logs.

**Exit criteria (met on the development PC):** WinRT video-capture enumeration lists the camera; a Media Foundation capture through the Frame Server returns 30 fps upright live frames.

## Phase 3 — One-file installer and fresh-machine hardening (delivered)

**Objective:** a user installs everything from one file on a clean Windows 11 PC.

Delivered: Inno Setup installer `lan-camera-setupv<version>.exe`, static C runtime, correct DLL for `MFCreateVirtualCamera`, setup tool preflight and honest summary, uninstall mode, in-use DLL replacement by rename, verification that Windows lists the camera.

**Exit criteria (met on the development PC):** silent install, uninstall (camera list empty, registrations and data folder removed) and reinstall all succeed. **Not yet met:** the same on a clean machine/VM through the visible wizard.

## Phase 4 — Verification on real targets (next)

- Clean Windows 11 VM: install via the wizard, open the Windows Camera app, confirm live phone video.
- Teams, Zoom, Chrome (`getUserMedia`), OBS and Unity: select the camera, confirm frames and framerate.
- Windows 10: confirm the installer reports "needs Windows 11" for the Windows camera and the DirectShow camera still works.
- Windows N edition (Media Feature Pack), antivirus/SmartScreen behavior.

## Phase 5 — Quality and polish (proposed)

- Decide the fate of the shared-memory path (wire it to remove the JPEG round trip, or delete `shm_bridge.h`, `--bridge` and the reader).
- Resolution/frame-rate options beyond 1280x720 at 30 FPS; better "no signal" image than black.
- Camera controls (`IKsControl` currently exposes none).
- Code-sign the DLLs and the installer.
- Automated tests: a capture test (Frame Server and DirectShow) and an installer smoke test in a VM.
