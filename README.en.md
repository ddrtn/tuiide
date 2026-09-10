# TUI IDE

[Русский](README.md) | [English](README.en.md)


**TUI IDE** is a full-screen IDE for C and C++ development in a Linux/amd64
terminal. It is written in C++20 and uses
[Final Cut](https://github.com/gansm/finalcut), CMake, clangd, and GDB/MI. The
source code and user interface use UTF-8.

## Features

- Editor highlighting for C, C++, and CMake (`CMakeLists.txt`, `*.cmake`), plus
  clangd semantic highlighting, diagnostics, completion, hover, definitions,
  and references.
- Document and project search and replace, `clang-format`, outline, code
  actions, symbol rename, and clangd hierarchy queries.
- Creation, import, and configuration of CMake projects, including targets,
  configure/build presets, generators, build directories, and parallel jobs.
- Asynchronous Configure, Build, Clean, and Rebuild commands with Problems and
  Output panels.
- CTest JSON discovery, all/selected/failed runs, test presets, and source
  navigation from failed tests in the Tests panel.
- Asynchronous file, CMake-target, or project analysis with clang-tidy,
  cppcheck, and IWYU, plus an isolated ASan/UBSan build.
- Named Run/Debug configurations support cloning and quick selection; programs
  run in an integrated PTY or external terminal with arguments, environment,
  and standard input.
- GDB debugging with breakpoints and logpoints, stack frames, threads, locals,
  watches, registers, expression evaluation, memory, and disassembly; the editor
  highlights the current stopped source line.
- Project tree and templates for C/C++ source files and classes that update the
  relevant CMake files.

## Requirements

Only Linux on amd64 is supported. On Debian or Ubuntu, install the dependencies:

```sh
sudo apt install g++ cmake libgpm-dev nlohmann-json3-dev clangd clang-format gdb clang-tidy cppcheck iwyu
```

clangd, clang-format, and GDB are required only for their corresponding
features. When a tool is unavailable, the IDE remains usable and reports the
missing feature in Output. Desktop clipboard integration optionally uses
`wl-clipboard` on Wayland or `xclip`/`xsel` on X11.

## Building

Run these commands from the repository root. Use a single build job on the
current development host:

```sh
git -C tuiide submodule update --init --recursive
cmake -S tuiide -B tuiide-build -DCMAKE_BUILD_TYPE=Release
cmake --build tuiide-build --parallel 1
ctest --test-dir tuiide-build --output-on-failure
```

Generated artifacts stay in `tuiide-build/`. Add
`-DTUIIDE_ENABLE_SANITIZERS=ON` for a sanitizer build. Additional checks are
available through `TUIIDE_ENABLE_CLANG_TIDY`, `TUIIDE_ENABLE_CPPCHECK`, and
`TUIIDE_ENABLE_LONG_TESTS`.

## Running

Open an existing CMake project, or create/import one from the **File** menu:

```sh
./tuiide-build/tuiide /path/to/project
./tuiide-build/tuiide --project /path/to/project --diagnostic \
  --log-file /tmp/tuiide.log
```

Command-line information is available without starting the terminal UI:

```sh
./tuiide-build/tuiide --help
./tuiide-build/tuiide --version
```

Project settings are stored in `.tuiide-project.json`; session and recovery
data are kept in the build directory. User shortcuts, theme, and colors are
stored under `$XDG_CONFIG_HOME/tuiide/` (normally `~/.config/tuiide/`).

## Using the Interface

Open the main menu with `F10` or `Alt+F/E/S/R/P/D/T/W/H`. Underlined letters
select commands inside an open menu. Configure global shortcuts through
**Tools → Configure shortcut**.

- `F5`, `F6`, `Ctrl+B`, and `F9`: debug, run, build, and toggle breakpoint.
- `F7`, `F8`, and `Alt+F8`: step into, step over (Next), and step out;
  `Ctrl+F8`: next diagnostic.
- `Ctrl+N`, `Ctrl+O`, `Ctrl+S`, and `Ctrl+W`: new, open, save, and close file.
- `Ctrl+F`: find/replace; `Ctrl+Space`: completion; `Alt+K`: command palette.
- `Alt+L`: manage Run/Debug configurations; `Alt+Shift+L`: quick selection.
- `Alt+PageUp/PageDown`: sidebar tabs;
  `Alt+Shift+PageUp/PageDown`: lower-panel tabs.

The sidebar contains **Open files**, **Project**, **Outline**, **Debug**,
**Breakpoints**, and **Tests**. Toggle individual pages from **Window**. In
Tests, press `Space` to run the selected test and `Enter` to open its first
failure location; discovery and grouped runs are under **Run → Tests**. The
lower area contains **Output**, **Problems**, **Build**, an interactive **Terminal**, and
**Analysis**. Start analyzers from **Tools → Run static checks**; recognized
diagnostics are also listed in Problems.

## Repository Layout

- `include/tuiide/`: public headers.
- `src/`: IDE core, Final Cut UI, and CMake/clangd/GDB integrations.
- `tests/`: unit, integration, and pseudo-terminal tests.
- `third_party/finalcut/`: pinned Final Cut checkout.
- `docs/`: manual page; `completions/`: Bash completion.
- `../tuiide-build/`: recommended out-of-source build directory.

## Installation and Packaging

```sh
cmake --install tuiide-build --prefix /opt/tuiide
/opt/tuiide/bin/tuiide --version

(cd tuiide-build && cpack -G TGZ)
(cd tuiide-build && cpack -G DEB)
```

The installation includes the executable, manual page, Bash completion, both
README languages, and the Final Cut license.

## Licenses

TUI IDE uses a vendored copy of Final Cut. Its license is available at
[`third_party/finalcut/LICENSE`](third_party/finalcut/LICENSE). Verification
requires a clean checkout, the expected `origin`, and a pinned commit reachable
from the previously fetched `refs/remotes/origin/main`:

```sh
cmake --build tuiide-build --target check-finalcut-vendor --parallel 1
ctest --test-dir tuiide-build --output-on-failure -L vendor
```

For a reproducible update, fetch the upstream branch first and then detach the
submodule at the reviewed commit without applying local patches:

```sh
git -C tuiide submodule update --init --recursive third_party/finalcut
git -C tuiide/third_party/finalcut fetch --prune origin main
git -C tuiide/third_party/finalcut switch --detach <commit>
git -C tuiide/third_party/finalcut status --short
git -C tuiide/third_party/finalcut merge-base --is-ancestor \
  <commit> refs/remotes/origin/main
```

The last two commands must produce no output and return status `0`. After
review, update `TUIIDE_FINALCUT_VERSION` and `TUIIDE_FINALCUT_COMMIT` in
`cmake/FinalCutVendor.cmake`, run both checks above, and commit the submodule
gitlink together with the pin file. Do not edit files under
`third_party/finalcut/` directly.
