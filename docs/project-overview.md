# LAN Phone Camera Driver — Project Overview

## 1. Purpose

A Windows virtual webcam named **"LAN Phone Camera"**. It lets any Windows application use a phone camera that is streamed by the LAN Camera Streaming server (the parent project, which mounts this repository as the `vcam/` submodule). The phone sends video to a browser viewer on the PC; the driver turns that stream into a camera that Windows applications can select.

This repository contains only the Windows side: two camera implementations, a setup tool, and an installer. It does not capture, encode or store video itself.

## 2. Primary user flow

1. The user installs the driver once with `lan-camera-setupv<version>.exe` (one UAC prompt).
2. On the PC they open the viewer page of the LAN Camera Streaming server and join the room the phone is streaming to.
3. They click **Set as Webcam**. The browser opens `lancam://stream?room=...&pin=...&host=...&port=...`; `lancam-setup.exe` stores the room, PIN and server address for the cameras (no UAC).
4. In any application they choose **LAN Phone Camera**: Windows Camera, Teams, Zoom, Chrome, OBS, Unity, ffmpeg.

## 3. Goals

- Appear as a normal camera to both camera stacks Windows applications use:
  - **Media Foundation** (Windows Camera app, Teams, Zoom, Chrome, Unity): `LanCamMfSource.dll`, a virtual camera registered with `MFCreateVirtualCamera`. Windows 11 only.
  - **DirectShow** (OBS, ffmpeg, Chrome/Electron apps, older software): `LanCamFilter.dll`.
- Deliver 1280x720 at 30 FPS whatever size or orientation the phone sends (letterboxed).
- Install, update and uninstall like normal software: one file, one UAC prompt, an entry in Windows "Installed apps", a clean uninstall.
- Work on a fresh Windows machine: no Visual C++ redistributable, no manual steps, honest error reporting with logs.
- Stay user-mode: no kernel driver, no code that needs test-signing.

## 4. Non-goals

- Capturing, encoding or recording video; audio.
- A kernel-mode camera driver; Windows 10 support for the Windows (Media Foundation) camera (the API does not exist there; the DirectShow camera still works).
- Code signing (not done yet: the installer and DLLs are unsigned).
- Multiple cameras or per-application settings.

## 5. Technology

| Area | Choice |
|---|---|
| Language / toolchain | C++17, MSVC (Visual Studio 2022, x64), static C runtime (`/MT`) |
| Frame source | The server's MJPEG stream `http://<host>:<port>/stream/<room>?pin=<pin>` over loopback, read with WinHTTP and decoded with the Windows Imaging Component |
| Windows camera | Media Foundation media source behind an `IMFActivate` class factory, hosted by the Windows Frame Server (LocalService) |
| DirectShow camera | In-process COM source filter registered in `CLSID_VideoInputDeviceCategory` |
| Setup tool | `lancam-setup.exe`: registration, `lancam://` handler, uninstall, logging |
| Installer | Inno Setup 6 script `installer/lan-camera.iss` -> `lan-camera-setupv<version>.exe` |

## 6. Dependencies on the parent project

- The parent server provides `/stream/<room>`, `POST /api/vcam/bridge-frame`, the "Set as Webcam" button and the "Download Driver" link, and embeds the finished installer in its own exe.
- The parent `build.bat` calls this repository's `build.bat`; the installer version is the parent's `package.json` version.
- This repository can be built on its own (`build.bat`); without a parent `package.json` it uses version `1.0.0` (or the `LANCAM_VERSION` environment variable) and writes the installer to `build/installer`.

## 7. Definition of done for the current scope

- Windows lists "LAN Phone Camera" after install and no longer after uninstall.
- A capture through the Frame Server and through DirectShow returns live, upright frames.
- The installer works from a single file on a clean Windows 11 machine, and reports problems instead of claiming success. (Verified on one development PC; a clean machine is still to be tested.)
