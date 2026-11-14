# SkellyTune — Windows JUCE build fixed

The real **JUCE 8.0.4 Release VST3 and Standalone now build successfully** on this Windows x64 machine using llvm-mingw/Clang 23.1.3. This supersedes the failed-native-build status in the earlier engineering report. DSP results and remaining pitch-quality limitations are unchanged.

## What failed and what changed

The silent `juceaide` exit `0xC0000139` was an unresolved `TaskDialogIndirect` import: the executable selected legacy Common Controls without an embedded v6 manifest. Embedding the manifest fixed helper startup. A native import checker reproduced the missing entry point; the same import was present in Python's already-activated environment, which explains why the earlier Python check missed it.

The subsequent actual plugin build exposed additional JUCE/toolchain compatibility issues. The project now applies a small, version-checked dependency patch and explicit Windows link settings:

- Embed Common Controls v6 manifests in `juceaide`, the VST3 metadata helper, the Standalone, and the VST3 DLL (correct executable/DLL resource IDs).
- Link the Windows libraries that this compiler does not obtain from MSVC `#pragma comment(lib, ...)`, including kernel-streaming GUID definitions.
- Use `alignof(Type)` in JUCE's sample buffer, explicit Direct2D interface UUIDs, and correct array-reference forwarding for the deprecated channel-layout constructors.
- Supply the missing Windows Media error-header include and use the documented caret-position values at the three JUCE call sites. The compiler SDK itself is pristine; no SDK edits are required.
- Include `cmake/PatchJuceMinGW.cmake`, the minimal JUCE patch, manifest/target helpers, and `scripts/build-windows-mingw.ps1` so this is reproducible rather than a manual dependency edit.

The patch applies to pristine JUCE 8.0.4 files and is idempotent. Both LF and CRLF source variants were checked. A mismatching dependency fails configuration instead of being silently overwritten. `FETCHCONTENT_SOURCE_DIR_JUCE` overrides are supported as well as the normal FetchContent patch step.

**JUCE 8.0.4 upstream no longer supports MinGW.** This is a project-owned compatibility path, tested with the stated toolchain. The MSVC build path remains unchanged and was not tested here. Use a clean dependency tree if disabling `SKELLYTUNE_MINGW_COMPAT` after previously enabling it.

## Verification actually performed

1. Release `SkellyTune.exe` and the complete `SkellyTune.vst3` bundle compiled and linked successfully.
2. JUCE's post-build VST3 helper loaded the module and generated `Contents/Resources/moduleinfo.json`, identifying the audio component, controller, and compatibility classes.
3. The new SDK smoke host loaded the **actual VST3 binary**, called `InitDll`/`GetPluginFactory`, created and initialized the component, queried `IAudioProcessor`, configured stereo buses, processed 120 × 512-sample stereo blocks at 48 kHz, checked finite/nonzero output, and terminated/released the component and module successfully. Result: **latency 788 samples; output energy 1943.48; exit 0**.
4. The existing DSP suite passed **30/30 tests** again. No production DSP code or parameter IDs changed in this build-fix follow-up.
5. Standalone import inspection shows Windows/system runtime DLLs, with no separate LLVM/libc++/libwinpthread DLL requirement.

This is a native build and VST3 processing smoke pass. **FL Studio loading, editor interaction, session recall, transport/PDC, and long-running DAW behavior remain unverified.** The Standalone executable was built; no audio-device/UI session is claimed as tested.

## Delivered binaries

`SkellyTune-Windows-x64.zip` contains:

```text
VST3/SkellyTune.vst3/Contents/x86_64-win/SkellyTune.vst3
VST3/SkellyTune.vst3/Contents/Resources/moduleinfo.json
Standalone/SkellyTune.exe
Verification/...
```

Keep the **entire outer `SkellyTune.vst3` bundle** together when placing it in a VST3 search location. No system plugin folder was modified during this task.

## Rebuild

Use the updated source archive and an x64 llvm-mingw toolchain, CMake, Ninja, and Git:

```powershell
.\scripts\build-windows-mingw.ps1 `
  -ToolchainRoot C:\path\to\llvm-mingw `
  -CMake C:\path\to\cmake.exe `
  -Ninja C:\path\to\ninja.exe
```

The script fetches JUCE through CMake by default. To use a local pristine JUCE 8.0.4 tree, add `-JuceSource C:\path\to\JUCE-8.0.4`. Compatibility edits are confined to that dependency tree; the DSP headers are untouched.

After building, the optional real-binary smoke host can be built and run:

```powershell
cmake --build build-windows-mingw --target skellytune_vst3_smoke
.\build-windows-mingw\skellytune_vst3_smoke.exe `
  .\build-windows-mingw\HumHouseVocalTune_artefacts\Release\VST3\SkellyTune.vst3\Contents\x86_64-win\SkellyTune.vst3
```

Build output and smoke results are included under `Verification/`. Hashes accompany the binary package.
