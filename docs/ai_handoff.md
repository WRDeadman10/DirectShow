# LAN Phone Camera Driver — AI Handoff

Last updated: 2026-10-13. This repository is the git submodule `vcam/` of LAN Camera Streaming. Read the parent's `AGENTS.md` for agent rules (documentation rules are its section 16; for C++ here, match the existing style: 4-space indent, K&R braces, no CRT-dependent features that need a redistributable). Parent docs: `../docs/architecture.md` §12 and ADR-021 to ADR-024.

## Current status

- Two cameras are implemented and verified on one Windows 11 PC: `LanCamFilter.dll` (DirectShow) and `LanCamMfSource.dll` (Media Foundation virtual camera). Windows lists "LAN Phone Camera"; Frame Server and ffmpeg captures return live, upright 1280x720 at 30 fps.
- `lancam-setup.exe` does registration, `lancam://` handling and uninstall; `lan-camera-setupv<version>.exe` (Inno Setup) wraps it. Silent install, uninstall and reinstall verified; exit code 0 and log `directshow=1 mf=0x0 listed=1`.
- The shared-memory bridge is dead code (DEC-005).
- Not verified: clean machine/VM, the visible wizard, the Windows Camera app window itself, Teams/Zoom/Unity, a real phone, Windows 10, N editions, antivirus/SmartScreen behavior.

## Read before implementation

1. `docs/architecture.md` — components, setup tool modes, data flow, installer, diagnostics.
2. `docs/decisions.md` — DEC-001 to DEC-008.
3. `docs/tasks.md` and `docs/roadmap.md` — open items.
4. `docs/project-overview.md` — scope.

## Key facts

- **Build:** `build.bat` (needs VS 2022 C++ and Inno Setup 6). Outputs in `build/` (`LanCamFilter.dll`, `LanCamMfSource.dll`, `lancam-setup.exe`) and the installer in `../dist/` when built inside the parent repository (else `build/installer`). Version: parent `package.json`, or `LANCAM_VERSION`, else 1.0.0.
- **Locked outputs:** an open elevated "LAN Camera Driver Setup" message box (non-`--silent` run) keeps `build/lancam-setup.exe` locked and the build fails at the link step; close it, or use `--silent` in tests.
- **Test without admin:** load `LanCamMfSource.dll` directly (`DllGetClassObject`) and read it with a Media Foundation Source Reader; the in-process test and a Frame Server capture test were written outside this repository (see tasks T5).
- **Verify the Windows camera:** WinRT `DeviceInformation.FindAllAsync(DeviceClass.VideoCapture)` must list the camera; `ffmpeg -list_devices true -f dshow -i dummy` lists the DirectShow one.
- **Logs:** `%ProgramData%/LanCamera/setup.log`, `mfsource.log`.
- **Pitfalls found:** `HKLM/SOFTWARE/Classes` write is denied (use `HKEY_CLASSES_ROOT`); `MFCreateVirtualCamera` lives in `mfsensorgroup.dll`; Chrome/Electron apps map `LanCamFilter.dll`; the Frame Server (LocalService) cannot read the user profile; the Frame Server asks the CLSID for `IMFActivate` first.
- **Git:** the remote URL of this submodule in the parent checkout contains a personal access token in local git config (not committed). Rotate it and use Git Credential Manager. Commit and push inside this repository first, then update the pointer in the parent.

## Modified files (last iteration)

`src/lancam_mfsource.cpp` (+ `.def`, new), `src/lancam_setup.cpp`, `src/lancam_filter.cpp`, `include/config_manager.h`, `installer/lan-camera.iss` (new), `build.bat`, and all of `docs/` (new).

## Next recommended task

Install on a clean Windows 11 VM through the wizard, open the Windows Camera app with a phone streaming (viewer page open, "Set as Webcam" clicked), and confirm live video; then Teams/Zoom/Unity. Record results in `docs/tasks.md` (T2, T4).

## Commands run and results

- `build.bat`: builds all three binaries with `/MT` and `lan-camera-setupv1.0.19.exe` (about 2.2 MB).
- Silent install `lan-camera-setupv1.0.19.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART`: exit 0; Windows lists the camera; Add/Remove Programs entry present.
- Silent uninstall (`unins000.exe /VERYSILENT`): exit 0; camera list empty; DirectShow class removed; `%ProgramData%/LanCamera` removed; one mapped `*.old*` DLL left until reboot.
- Frame Server capture (Source Reader, RGB32): 300 frames in 10 s, 1280x720, non-black, upright.
- `dumpbin /dependents`: only Windows DLLs.

## Change log (newest first)

- **2026-10-13** — Inno Setup installer; `--uninstall`; no self-copy; static runtime (`/MT`); `MFCreateVirtualCamera` looked up in `mfsensorgroup.dll`; honest setup summary with a "listed by Windows" check; preflight for missing DLLs.
- **2026-10-12** — Media Foundation camera (`LanCamMfSource.dll`), `IMFActivate` factory, ProgramData session, logs, registration through `HKEY_CLASSES_ROOT`, rename-aside for in-use DLLs; DirectShow output flipped and letterboxed; setup manifest changed to `asInvoker`, Camera app no longer launched on `lancam://`.
- **2026-10-12** — First version: DirectShow filter, MJPEG decoder, `lancam://` handler, shared-memory bridge (unused).
