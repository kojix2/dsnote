# macOS build/run investigation notes

This document summarizes the changes needed to build and run the current dsnote codebase on macOS. It lists Linux‑centric dependencies, possible replacements, and work items.

## Current assumptions (Linux‑heavy areas)

### 1) Audio device enumeration depends on PulseAudio
- `audio_device_manager` directly uses the PulseAudio API.
- On macOS this needs to be replaced with CoreAudio.

### 2) X11/Wayland‑dependent input, hotkeys, and clipboard
- Fake keyboard/paste: `fake_keyboard` depends on X11/XKB/Wayland/ydotool(uinput).
- Clipboard: `wl_clipboard` assumes `wl-copy/wl-paste`.
- Global hotkeys: QHotkey/X11Extras/X11 dependencies.

### 3) DBus‑based IPC
- App/service communication uses QtDBus and DBus XML adaptors.
- macOS does not ship DBus, so an alternative IPC is required.
  - Currently adjusted so it **does not exit when there is no DBus session** (see progress).

### 4) Build/install flow assumes ELF/Linux tools
- `patchelf` is used and `.so` placement/patching is assumed.
- Link flags like `--enable-new-dtags` assume GNU ld.
- macOS requires `install_name_tool` / `@rpath` / `.dylib`.

### 5) OS/arch detection mismatch
- `CMAKE_SYSTEM_PROCESSOR` might be `arm64` but match `.*arm.*`, causing `arm32` handling.

## Required changes (minimum to build)

### A. Add macOS branches in CMake
- Add `APPLE` branches to disable/replace Linux‑specific dependencies.
- Examples:
  - `libpulse`/`wayland-client`/`xkbcommon`/`Qt5X11Extras` should **not be required on macOS**.
  - Default `WITH_X11_FEATURES=OFF` on macOS.

### B. Isolate PulseAudio usage
- Abstract `audio_device_manager`, and provide a CoreAudio backend on macOS.
- Examples:
  - Enumerate input devices via CoreAudio `AudioObject` API.
  - Replace with QtMultimedia `QAudioDeviceInfo` (Qt5 constraints apply).

### C. Fake keyboard/paste on macOS
- Current implementation depends on X11/Wayland/ydotool/uinput.
- Use Accessibility APIs on macOS:
  - `CGEventCreateKeyboardEvent` for key injection.
  - `AXUIElement` for focus/UX integration.
- Split `fake_keyboard` into OS‑specific backends.

### D. Clipboard implementation on macOS
- Replace `wl_clipboard` with a macOS implementation using `QClipboard`.
- Disable Wayland CLI usage on macOS.

### E. Replace DBus usage
- Current DBus API targets Linux desktop environments.
- On macOS, use one of:
  - Local IPC via `QLocalServer/QLocalSocket`
  - macOS native: `NSDistributedNotificationCenter` / `NSXPCConnection`
  - Or disable DBus features on macOS.

### F. macOS install flow
- Remove `patchelf` dependency.
- Switch `.so` handling to `.dylib`/`.framework`.
- Use `install_name_tool` and `@rpath`.
- Consider building a `.app` bundle.

## Additional changes (feature parity)

### 1) Global hotkeys
- Implement macOS hotkeys via `RegisterEventHotKey` or Carbon/Quartz API.
- If QHotkey supports macOS, use it, but clean up any X11‑only branching.

### 2) Wayland/X11‑specific features
- Ignore Wayland/X11 settings (e.g., `is_wayland`, compose config) on macOS.
- Hide/disable UI options based on OS.

### 3) Audio I/O
- Prefer QtMultimedia for playback/recording.
- Use CoreAudio if low‑level control is needed.

## Concrete CMake change plan

- Add `if(APPLE)` in `CMakeLists.txt` to disable/replace Linux‑only deps.
- Split `WITH_DESKTOP` dependencies by OS.
- Split `link_opts` by OS; macOS uses `-Wl,-rpath,@executable_path/../Frameworks`.
- Skip `patchelf` in `install_desktop.cmake` on macOS.
- Convert `.so` handling in `BUILD_*` flow to `.dylib` on macOS.

### Draft CMake changes

#### 1) OS defaults
- For `if(APPLE)`, set:
  - `WITH_X11_FEATURES=OFF`
  - `BUILD_XDO=OFF`, `BUILD_QHOTKEY=OFF`, `BUILD_WL_CLIPBOARD=OFF`
  - `BUILD_XKBCOMMON=OFF`, `BUILD_QQC2_BREEZE_STYLE=OFF`

#### 2) Required vs optional deps
- Wrap `pkg_search_module(libpulse REQUIRED ...)` with `if(NOT APPLE)`.
- Do the same for `wayland-client`, `xkbcommon`, `xkbcommon-x11`, `Qt5X11Extras`.

#### 3) Link flags and RPATH
- Split `link_opts` by OS:
  - Linux: `-Wl,--enable-new-dtags` etc.
  - macOS: `-Wl,-rpath,@executable_path/../Frameworks` / `@loader_path`
  - Add Qt library directory to `CMAKE_BUILD_RPATH` / `CMAKE_INSTALL_RPATH`
    to resolve `@rpath/libQt5Multimedia.5.dylib`.

#### 4) Install flow branching
- Run `patchelf` only under `if(NOT APPLE)`.
- Add macOS flow using `install_name_tool` and `@rpath`.

#### 5) Arch detection
- Treat `CMAKE_SYSTEM_PROCESSOR=arm64` as `arch_arm64`.

---

## macOS backend implementation plan

### Goal
Replace Linux‑specific I/O, IPC, and hotkey features with macOS‑native backends and make the GUI app usable.

### 1) Audio device enumeration (CoreAudio)
- **Target**: `audio_device_manager`
- **Approach**: Introduce an abstract interface (e.g., `IAudioDeviceManager`) and provide:
  - Linux: PulseAudio backend
  - macOS: CoreAudio backend
- **Implementation ideas**:
  - Use `AudioObjectGetPropertyData` for input device enumeration
  - Collect name/UID/sample rate
  - Use `AudioObjectAddPropertyListener` for hot‑plug

### 2) Fake keyboard/paste (macOS)
- **Target**: `fake_keyboard`
- **Approach**: Split into OS backends; use Quartz Event Services on macOS.
- **Implementation ideas**:
  - Key injection: `CGEventCreateKeyboardEvent` / `CGEventPost`
  - Paste: send `Cmd+V`
  - Requires Accessibility permission (prompt on first run)

### 3) Clipboard
- **Target**: `wl_clipboard`
- **Approach**: Replace with `QClipboard` on macOS.
- **Implementation ideas**:
  - Use `QGuiApplication::clipboard()` directly
  - Disable Wayland CLI calls on macOS

### 4) Global hotkeys
- **Target**: `global_hotkeys_manager`
- **Approach**: Add macOS hotkey implementation.
- **Implementation ideas**:
  - Carbon `RegisterEventHotKey` (legacy but stable)
  - Or add a library like `MASShortcut`
- **Note**: Key binding UI/permissions need macOS handling

### 5) IPC / single‑instance
- **Target**: `app_server` / DBus
- **Approach**: Abstract DBus and provide macOS alternative.
- **Implementation ideas**:
  - `QLocalServer/QLocalSocket` for single‑instance control
  - macOS notifications: `NSUserNotification` / `UNUserNotificationCenter`

### 6) Build/distribution
- **Target**: install/distribution flow
- **Approach**:
  - Use `macdeployqt` to bundle Qt dependencies
  - Build a `.app` bundle
  - Add `codesign` / `notarize`

---

## Suggested work order

1. **CMake branching** so macOS config succeeds
2. **Abstract PulseAudio** (macOS can be stubbed initially)
3. **Replace DBus/IPC on macOS** (single‑instance control)
4. **macOS input/clipboard backends**
5. **Bundle into .app**

### Progress
- Added `APPLE` branches to avoid macOS requiring `libpulse/wayland/xkbcommon/X11Extras/patchelf`.
- On macOS, `CMAKE_SKIP_RPATH=OFF` and Qt library path is appended to RPATH.
- Switched `cmake/rnnoise.cmake` from `cp --no-target-directory` to `cmake -E copy_directory` on macOS.
- Updated multiple ExternalProjects (`libnumbertext/espeak/vosk/ffmpeg/piper`) for macOS‑compatible copy commands.
- Disabled `BUILD_RHVOICE` / `BUILD_RHVOICE_MODULE` on macOS (utf8cpp issue).
- Updated `tools/make_espeakdata_module.sh` for macOS (shebang and `cp` compatibility).
- Removed `threads.h` dependency in `src/logger.cpp`; use `pthread_self()` on macOS.
- Changed `stat::st_ctim` to `st_ctime` in `src/tts_engine.cpp` for macOS build.
- Stubbified `src/april_engine.cpp/.hpp` behind `HAVE_APRILASR` on macOS.
- Moved `audio_device_manager::clean()` back under Linux only to avoid undefined members.
- Shared `m_force_bind` in `global_hotkeys_manager.hpp/.cpp` and fixed `reset_portal_connection()` linkage on non‑X11.
- Updated `cmake/piper.cmake` to use macOS onnxruntime `.dylib`.
- Disabled `strip --strip-all` on macOS in `CMakeLists.txt`.
- Adjusted `app_server` to avoid exiting when DBus session bus is unavailable.
- Updated `patches/bergamot.patch` for arm64 faiss disablement and sentencepiece enum range fix.
- Verified `dsnote` builds on macOS arm64.
- Verified `build-mac-2` with `-j8` and launch.

## Migration priorities

1. **Minimal build that runs**
   - Disable DBus/X11/Wayland/PulseAudio.
   - Confirm GUI launch.
2. **Enable audio I/O (CoreAudio)**
3. **macOS hotkeys/paste integration**
4. **Distribution (.app, notarization)**

## Key related files
- `CMakeLists.txt`
- `cmake/install_desktop.cmake`
- `cmake/dbus_api.cmake`
- `src/audio_device_manager.cpp/.hpp`
- `src/fake_keyboard.cpp/.hpp`
- `src/wl_clipboard.cpp/.hpp`
- `src/global_hotkeys_manager.cpp/.hpp`
- `src/app_server.cpp` / `src/speech_service.*`

---

## Summary
macOS support requires dependency replacements, OS‑specific backends, and a macOS‑appropriate install/bundle flow. Disabling Linux‑only features gets the app to launch, but core features like audio input, key injection, and hotkeys will not work until macOS backends are implemented. Prioritize **CoreAudio** support and **macOS IPC** as the next steps.
