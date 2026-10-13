UniBin Compiler 0.0.4 — Linux prototype
=======================================

This release moves the built-in Linux runtimes into actual C library files.
The compiler reads each imported library from lib/<name>.c, embeds that source
into the generated C translation unit, and compiles the combined result into a
native Linux executable. The generated C file can be kept with --emit-c.

Build the compiler from source:
    cc -std=c11 -O2 -Wall -Wextra unibinc.c -o unibinc

Compile a program and preserve generated C:
    ./unibinc examples/hello.ub -o hello --emit-c hello.generated.c
    ./hello

Use another library directory:
    ./unibinc program.ub -o program --lib-dir ./mylibs

Import behavior:
    $import base       Includes base timing/runtime helpers.
    $import gui-disp   Includes the Linux X11 display/window runtime.
    $import gui-comp   Includes GUI labels, buttons and message boxes.
                       This automatically includes gui-disp as a dependency.
    $import player     Includes audio playback via ffplay, with aplay fallback.
    $import my-library Includes lib/my-library.c (custom C library).

Libraries are bundled in the generated source and therefore into the final
executable; they are not needed beside the executable at runtime. They are
needed when compiling. Custom library files should contain C declarations and
implementation code, and must not define a second main() function. Custom
libraries are included after built-in libraries, in the order they are imported.
Built-in libraries are included in dependency order: base, gui-disp, gui-comp,
player. Repeated imports are ignored.

Linux build/runtime dependencies:
    - cc or gcc
    - libx11-dev to compile programs importing gui-disp or gui-comp
    - An X11 desktop/session to run GUI programs
    - ffplay for common audio formats; aplay is the fallback

On Debian/Ubuntu/Linux Mint:
    sudo apt install build-essential libx11-dev ffmpeg alsa-utils

Examples:
    examples/hello.ub        Console program using base
    examples/gui-demo.ub    Window, label and button demo
    examples/player-demo.ub Audio-player demo (replace sample.ogg with a file)
    examples/import-demo.ub Custom-library bundling demo

Library files:
    lib/base.c
    lib/gui-disp.c
    lib/gui-comp.c
    lib/player.c
    lib/demo.c

Current limitations:
    - This is a Linux prototype, not a DOS/Windows/BSD/mobile cross-compiler.
    - The GUI uses X11 and simple controls drawn by UniBin.
    - The player starts external Linux audio tools.
    - Imported C code is bundled, but a general UniBin-to-C API binding system
      for arbitrary custom-library functions is not implemented yet.
    - Language parsing and syntax rules remain the existing prototype behavior.
