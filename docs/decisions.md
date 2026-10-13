# LAN Phone Camera Driver — Decision Records

Decisions are numbered DEC-nnn here. The parent project records the same choices from its side as ADR-021 to ADR-024 (`docs/decisions.md` there).

## DEC-001 — User-mode virtual camera that reads the server's MJPEG stream

**Status:** Accepted

**Decision:** Both cameras pull frames from `http://<host>:<port>/stream/<room>?pin=<pin>` over loopback with WinHTTP and decode with WIC. No kernel driver, no capture code in this repository.

**Reasoning:** The server already publishes that endpoint (it exists for OpenCV and ffmpeg), it needs no custom protocol, and a user-mode DLL needs no driver signing.

**Consequences:** One JPEG decode per frame; the server must be running and the room active; latency is a JPEG round trip, not zero-copy.

## DEC-002 — Two camera implementations: DirectShow and Media Foundation

**Status:** Accepted

**Decision:** Ship `LanCamFilter.dll` (DirectShow) and `LanCamMfSource.dll` (Media Foundation virtual camera).

**Reasoning:** The Windows Camera app is a UWP / Media Foundation application and enumerates only kernel-streaming and Media Foundation virtual cameras; a DirectShow-only camera can never appear there (verified: the Windows Runtime enumeration returned 0 devices). Older and Chromium-based software still uses DirectShow.

**Consequences:** Two code paths share `MjpegDecoder` and the session files. The Media Foundation camera needs Windows 11 (`MFCreateVirtualCamera`); the DirectShow camera works on Windows 10 too.

## DEC-003 — Media Foundation camera: system lifetime, activation object, HKEY_CLASSES_ROOT

**Status:** Accepted

**Decision:** Register with `MFVirtualCameraLifetime_System` and `MFVirtualCameraAccess_AllUsers` (persists across reboots, no helper process); the CLSID is a class factory that hands out an `IMFActivate`, which creates the media source; COM keys are written through `HKEY_CLASSES_ROOT`.

**Reasoning:** The Frame Server activates the registered CLSID as `IMFActivate` (observed in `mfsource.log`: first `IMFActivate`, then `IMFMediaSourceEx`). `HKLM/SOFTWARE/Classes` could not be opened for write even when elevated on the development PC, while `HKEY_CLASSES_ROOT` works.

**Consequences:** Installation needs administrator rights. The source runs as LocalService inside the Frame Server, so it cannot read the user's profile.

## DEC-004 — Session data in two places

**Status:** Accepted

**Decision:** `lancam://` writes the room/PIN/host/port to `%LOCALAPPDATA%/LanCamera/session.ini` (DirectShow, runs as the user) and `%ProgramData%/LanCamera/session.ini` (Media Foundation, runs as LocalService). The installer creates the ProgramData folder with modify access for Users and LOCAL SERVICE.

**Reasoning:** LocalService has no access to the user's profile; ProgramData is shared.

**Consequences:** Any local user can change the room/PIN the camera uses (acceptable for this project; security is de-prioritised). The PIN is stored in plain text in both files.

## DEC-005 — Shared-memory bridge is dead code

**Status:** Open (wire it or delete it)

**Context:** `include/shm_bridge.h`, `lancam-setup.exe --bridge` (a WinSock daemon on `127.0.0.1:7779`) and a shared-memory reader in the DirectShow filter were built to avoid the JPEG round trip. In the parent project, `POST /api/vcam/bridge-frame` receives JPEG frames and republishes them as MJPEG; nothing connects to port 7779 or starts `--bridge`, so the reader never has data and the filter always uses HTTP. The parent's ADR-022 describes the intended design and carries a correction.

**Options:** wire it (server forwards raw frames to the daemon, daemon started by the setup tool; the Media Foundation source would need a global, ACL'd mapping because it runs in the Frame Server) or delete it.

## DEC-006 — One-file Inno Setup installer; setup tool does the registration

**Status:** Accepted

**Decision:** Distribute `lan-camera-setupv<version>.exe` built with Inno Setup 6. Inno copies files and provides the uninstaller; `lancam-setup.exe --install/--uninstall` does all registration, so the same logic works when the tool is run directly.

**Reasoning:** The earlier zip needed extraction and a manual run, failed silently when started from inside the archive, and gave no uninstaller. Keeping registration in the tool (not in Inno `[Registry]`) avoids duplicating the Media Foundation logic and lets it verify the result.

**Consequences:** Building needs Inno Setup 6 and Visual Studio 2022. The installer is unsigned (SmartScreen may warn).

## DEC-007 — Static C runtime and dynamic lookup of `MFCreateVirtualCamera`

**Status:** Accepted

**Decision:** Build all binaries with `/MT`. Resolve `MFCreateVirtualCamera` with `GetProcAddress` from `mfsensorgroup.dll`.

**Reasoning:** A clean Windows has no Visual C++ redistributable; with `/MD` the setup tool cannot start or the Frame Server cannot load the DLL. A previous dynamic lookup used `mfplat.dll`, which does not export the function, so the Windows camera was silently skipped on every machine (hidden on the development PC by an earlier install). Looking in the right DLL at run time keeps the tool startable on Windows 10, where the function is absent.

**Consequences:** DLLs are about 150-200 KB each. On Windows 10 the setup tool reports "Windows camera needs Windows 11".

## DEC-008 — In-use DLLs are renamed aside, not overwritten

**Status:** Accepted

**Decision:** When a destination DLL cannot be deleted or overwritten, rename it to `<name>.oldN` and schedule it for deletion at reboot, then copy the new file (done by both the Inno script and the setup tool).

**Reasoning:** Chromium/Electron applications (Chrome, Claude, VS Code) load every DirectShow camera DLL when they enumerate devices, and the Frame Server maps the Media Foundation DLL. Windows allows renaming a mapped DLL but not overwriting it.

**Consequences:** `*.old*` files can sit in `C:/Program Files/LanCamera` until the next reboot. Applications keep using the old DLL until restarted.

## Open decisions

- Wire or delete the shared-memory bridge (DEC-005).
- Code signing for the installer and DLLs.
- Whether to support more resolutions/frame rates and camera controls.
- Whether Windows 10 users need anything beyond the DirectShow camera.
