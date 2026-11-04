# Building Power Moon Recomp on Windows (x86-64)

## One-time setup
Run in PowerShell from the repo root:

    powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1

This installs MSYS2 (UCRT64: clang, lld, make, SDL3, FFmpeg, Vulkan loader + headers, shaderc, Vulkan Memory
Allocator, validation layers), Python 3.12 for the tools (`tools\venv`: unicorn, capstone, lz4) and switches the clone
to LF line endings.

## Build
From an MSYS2 UCRT64 shell, or Git Bash with MSYS2 first on PATH:

    export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
    make gen        # once (and after changes to tools/): recompile the game from extracted/exefs into generated/
    make -j16       # -> build/PowerMoonRecomp.exe and the game code library build/odyssey_game.dll
    make dlls       # the MSYS2 DLLs next to the exe (starts from Explorer / a shortcut without MSYS2 on PATH)

`make runtime-objs` compiles the runtime only (quick check). `CONSOLE=1 make` links the exe with a console window
(otherwise its output goes to `build/PowerMoonRecomp.log` when no console or redirect is attached).

## Notes
- Guest window 0x70_0000_0000..0x73_2000_0000 is reserved with VirtualAlloc and committed per region (kernel
  regions, modules, HLE heap); stray first touches are committed by a vectored exception handler.
- AArch64 FP/SIMD instructions are lowered to C (`gen.py --simd=c`, the default on non-arm64 hosts).
- Renderer: Vulkan (runtime/nvn/vulkan); shaders are translated to GLSL and compiled with shaderc
  (runtime/nvn/maxwell/translate.c).
- If the guest window is taken at startup, the program re-executes itself once for a fresh address layout; the first
  process waits for it and passes its exit code on.

## Tests
- `tools/test_translate.sh`: shader translator fuzz (MSL output unchanged, GLSL compiles and validates).
- `tools/test_vk_pipelines.sh`: fuzzed shaders through the backend's compile/reflection into real pipelines.
- `tools/odyssey_run.sh`: build, boot into the game on a copy of a save (`build/testsave`), show the log.
- `ODYSSEY_VK_VALIDATION=1` with `VK_ADD_LAYER_PATH` pointing at `C:\msys64\ucrt64\bin`: Khronos validation layer.
