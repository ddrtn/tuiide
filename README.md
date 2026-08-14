# TUI IDE

TUI IDE is a Linux/amd64 terminal IDE for C and C++ built with C++20 and a
vendored checkout of [Final Cut](https://github.com/gansm/finalcut). The current release provides a
UTF-8 editor, lexical and clangd semantic C/C++ highlighting, a project browser, clangd completion/hover/
diagnostics, asynchronous CMake builds, program output, and GDB/MI debugging.
clangd requests distinguish unavailable, empty, and error results. Completion
shows symbol kinds and applies server-provided insertion edits only to the matching
document revision.
**Tools → Signature help** shows overloads, documentation, and the active parameter;
symbol information is shown in a scrollable dialog.
**Search → Find** provides persistent case-sensitive, whole-word, and regular-
expression options, cyclic next/previous navigation, capture-aware replacement,
and one-step Replace All. Project search lists file/line results; project-wide
replacement shows a file preview and requires confirmation before writing files.
The Debug menu can stop the complete GDB session or restart the selected executable.
Inferior completion is tracked separately from the GDB process, so an exited program
clears stale stack, local, thread, and register data while remaining ready for another run.
The lower workspace has separate Output and Console tabs. Integrated Run processes
and GDB inferiors use a Linux pseudo-terminal, receive the visible console dimensions
and `SIGWINCH`, and accept UTF-8 lines through `stdin>`. In the Console input,
`Ctrl+C` interrupts the program and `Ctrl+D` sends terminal EOF; outside an active
console they retain their normal IDE meanings. Run → Stop program terminates the
complete launched process group. ANSI styling is removed while carriage-return
progress output is rendered in place. External-terminal mode remains available.

The full-screen interface follows the classic Free Pascal/Turbo Vision layout:
a mnemonic menu bar at the top, editor and tool panels in the workspace, and a
compact status line at the bottom. The left sidebar is a tab widget containing
Open files, Project, Outline, Debug, and Breakpoints pages. When tabs do not fit,
use the `<`/`>` tab-bar controls or the Left/Right keys; the active tab is scrolled
into view automatically. Individual sidebar pages can be shown or hidden with
the checked entries in the Window menu. At least one page always remains visible.
Click a tab or use arrows while its tab bar
is focused, or choose a page from the Window menu. Press `F10` to focus the menu,
use arrows or highlighted letters to navigate, and press Enter to invoke a command.
Menu commands are enabled only when their required project, document, clangd,
build, or debugger state is available; unavailable keyboard commands report the
reason in Output instead of failing silently.
The lower tab bar separates the general Output log, a filterable Problems list,
raw Build output, and the interactive Terminal. Activate a Problems row with
Enter or the mouse to open its exact source position. The Window menu can clear
or copy the active lower panel and filter Problems by source, severity, or text.
Use the Window menu to make the sidebar wider/narrower or the lower panel
taller/shorter. Sizes are clamped so the editor and status bar remain usable,
stored in the per-project session, and restored the next time it is opened.
IDE dialogs open centered, fit a 60x18 terminal, and retain their active field
when the terminal is resized while a dialog is open.
Press `Alt+K` to open the searchable command palette. Tools > Configure shortcut
overrides a command key for the current project and stores it under `shortcuts`
in `.tuiide-project.json`; Tools > Shortcut conflicts shows the effective table.
Tools > Editor theme selects Dark, Light, or High contrast. Editor colors can
override syntax, semantic-token, diagnostic, breakpoint, gutter, and selection
roles. The editor uses richer colors only on 256-color terminals and falls back
to contrast-preserving 16/8-color equivalents.
Commands also report empty completion/diagnostic results, invalid line ranges,
preset-loading failures, process start failures, and final program exit codes.

The Debug sidebar marks the selected stack frame. Activating a frame sends an
explicit GDB frame-selection request, opens its source location, and refreshes
locals, watches, and registers in that context. Compound locals use `[+]`/`[-]`;
activate a variable row to expand or collapse its fields, array elements, and
nested values.
GDB/MI records are parsed structurally, including nested tuples/result lists and
C-string escapes. Console and target streams are decoded before display;
`^error` responses appear as readable `GDB error:` messages in Output instead of
raw protocol records.
Use **Debug → Evaluate expression** to inspect an arbitrary C/C++ expression in
the selected frame. **Debug → Set variable value** uses the selected local when
available, or asks for an expression, assigns a new value, and refreshes locals
and watches after GDB confirms the change.
**Debug → Disassembly** renders addresses, function offsets, and instructions
around an address such as `$pc`. **Debug → Memory** reads 1–4096 bytes from an
address expression such as `$sp` and displays a conventional hex/ASCII dump.
**File → Save All** saves every modified document while preserving the active
editor. Configure/Build, Run, and the first Debug start perform the same preflight;
cancelling a Save As dialog or encountering a write error cancels the project action.
The Project page presents all project files and empty directories as an expandable
tree, excluding only `.git` and the configured build directory. It has no silent
file-count cutoff. The Project menu provides Refresh, a case-insensitive path
filter, Rename/Move, new-directory creation, and safe deletion of empty directories.
`F2` renames the selected Project entry when the tree has focus; open documents and
clangd tracking follow the new path. Rename/Move also updates exact quoted and
unquoted path arguments in nested `CMakeLists.txt` files while preserving comments,
bracket arguments, and common-prefix paths. If any CMake write fails, earlier edits
and the filesystem move are rolled back.
Use **File → New Project** to create a CMake project. The wizard requires separate
project and build directories and can create either directory from its path picker.
It supports C or C++, executable/static/shared targets, language-standard selection,
`.h` or `.hpp` C++ headers, Ninja/Unix Makefiles/default generators, build types,
compiler warnings, README, `.gitignore`, and CTest setup. C-only choices disable
the C++ header field and replace the standard list with valid C standards. The
optional GNU install layout generates portable executable, library, and header
install rules. Generator, build type, standard, and the chosen build directory are
stored in the ignored `.tuiide-project.json` file and restored when the project is
opened again. After creation, the IDE immediately runs CMake configure and reports
validation errors in Output. **File → Close Project** closes all
documents (prompting for modified files), stops build, language-server, and debugger
processes, and leaves the IDE ready to create another project.
**File → Open Project** opens directories containing `CMakeLists.txt`. If the chosen
directory has C/C++ sources but no CMake project, the IDE offers a separate import
flow. Select the language, target type, standard, warnings, and an out-of-source
build directory; review the complete generated `CMakeLists.txt` before applying it.
The importer scans subdirectories, skips VCS/build metadata, never overwrites an
existing project, and rejects changes made between preview and confirmation.
**File → Recent Projects** reads a deduplicated list from
`$XDG_CONFIG_HOME/tuiide/recent-projects.json` (or `~/.config/tuiide/`) and omits
paths that no longer contain a CMake project.
**Tools → Project Settings** configures the out-of-source build directory, CMake
generator and build type, toolchain, C/C++ compilers and standards, environment
variables, parallel build jobs, and extra clangd arguments. Parallel jobs initially
use the host's hardware thread count and can be overridden per project. Settings are validated and atomically stored
in the versioned `.tuiide-project.json`; paths inside the project are kept relative.
Saving restarts clangd and applies the environment to configure, build, run, and
debug processes. CMake presets remain available and command-line project settings
override their corresponding configure values. A bounded `# BEGIN TUI IDE` block
in `.gitignore` ignores `.tuiide-project.json` and an in-project build directory;
changing settings refreshes only that block and preserves all user-written rules.
Select a file and press `Delete` to remove its exact path arguments from project
`CMakeLists.txt` files, with an optional separately confirmed deletion from disk.
Press `Insert` in Project (or use **File → New from template**) to create a C/C++
header, implementation, or a C++ class pair. Paths are relative to the project,
may include new subdirectories, never overwrite files, and are inserted into the
selected CMake target. If no target is selected, the nearest enclosing target is used.
The path chooser is rooted in the project, starts at the selected Project directory,
and provides Up, New directory, file-name, Create, and Cancel controls. The C++ class
wizard configures class and namespace names, base class/header, public/protected/private
inheritance, `final`, constructor/destructor generation, virtual destructor, and
copy/move operations. Header and implementation paths are chosen independently;
the generated implementation uses the correct relative include path.

`CMakeLists.txt` and `*.cmake` open as first-class editor documents with CMake
command, control-flow, property, variable, generator-expression, string, number,
and bracket-comment highlighting. `Ctrl+Space` offers CMake commands, standard
variables and identifiers found in the current file; CMake documents are not sent
to clangd.

Lexical highlighting is cached per line, including multi-line comment state.
Edits invalidate only the affected region and reuse unchanged suffixes; clangd
diagnostics and semantic tokens are indexed by line and refreshed by revision
rather than on every UI timer tick. A 50,000-line regression test covers
single-line edits, inserted lines, and block-comment propagation.
Editor cursor placement uses terminal display columns rather than UTF-8 byte
offsets. Tab stops, double-width CJK and emoji, and zero-width combining marks are
handled consistently by drawing, selection, vertical movement, mouse hit testing,
horizontal scrolling, and the status-line column. Viewport clipping and mouse
selection always snap to complete UTF-8 code points.
The Open files page uses the shortest distinguishing path suffix when files share
a basename, for example `src/main.cpp` and `tests/main.cpp`; multiple unsaved files
are numbered. **File → Close Others**, **Close All**, and **Reopen Closed** complement
Save All. Reopen Closed keeps the 20 most recent saved paths, restores the cursor,
deduplicates manually reopened files, and discards workspace history when the
project is unloaded.
Undo/redo history stores reversible text spans instead of full document snapshots.
Sequential typing, Backspace, and Delete are grouped while cursor movement,
structural edits, and Save form boundaries. Each history entry carries state IDs,
so undoing exactly to the last saved state clears the modified marker and redo sets
it again. Multi-range workspace edits remain one atomic undo/redo operation.
Project Settings also controls tab width and whether Tab inserts spaces or a tab.
Enter preserves indentation and indents matching brace, bracket, and parenthesis
pairs. The editor auto-closes delimiters and quotes, while Edit menu commands
toggle line comments, duplicate or move selected lines, and delete lines. Each
structural command is recorded as one reversible undo/redo operation.
**Tools → Format document** and **Format selection** run `clang-format` with the
active file as `--assume-filename`, so project `.clang-format` rules and the C/C++
language are detected correctly. Selected formatting uses whole-line ranges. The
result preserves the cursor and selection and is committed as one undoable edit;
missing tools and formatter diagnostics are reported in Output.
Documents are edited internally as UTF-8 with LF separators, while load/save
preserves an existing UTF-8 BOM, LF or CRLF convention, and the presence of the
final newline. The status line shows these properties. Saving writes and flushes
a uniquely named file in the destination directory, preserves existing Unix
permission bits, and atomically renames it over the destination; failed writes
leave both the document path and original file intact.
Open files are content-fingerprinted and checked periodically, so changes are
detected even when timestamp resolution is insufficient. External modification
offers Reload, Keep, or a comparison of the editor and disk text; deletion offers
Keep or comparison. Keep acknowledges that exact disk state, preventing repeated
prompts until the file changes or reappears. Every five seconds, modified saved
and untitled documents are atomically stored beside the build session. A later
startup offers to restore them as unsaved buffers or discard the recovery data;
normal project close and application exit remove the recovery file.
The left **Outline** page uses clangd `textDocument/documentSymbol` results to
show namespaces, classes, structs, functions, methods, fields, and other symbols
with hierarchy and kind labels. Clicking or pressing Enter jumps to the symbol's
UTF-16-aware selection position. Requests are debounced for 500 ms after edits,
tagged with the document path and revision, and stale responses are discarded.

## Dependencies

On Debian-based systems:

```sh
sudo apt install g++ cmake libgpm-dev nlohmann-json3-dev clangd clang-format gdb
```

For native desktop clipboard access, install `wl-clipboard` on Wayland or
`xclip`/`xsel` on X11. These helpers are optional because OSC 52 and the
in-process fallback require no extra package.

## Build and test

Run these commands from the repository root (the parent of `tuiide/`):

```sh
git -C tuiide submodule update --init --recursive
cmake -S tuiide -B tuiide-build -DCMAKE_BUILD_TYPE=Release
cmake --build tuiide-build --parallel 1
ctest --test-dir tuiide-build --output-on-failure
```

When `clangd`, GDB, and `libutil` are available, CTest also builds a temporary
CMake fixture and verifies semantic tokens, completion, diagnostics, GDB/MI
breakpoints/locals/watches/registers, and Final Cut startup through a
pseudo-terminal. Environments that prohibit `ptrace` explicitly skip only the
inferior-execution portion; run CTest with normal ptrace permissions for the
complete debugger check.

Launch the IDE with a CMake project directory:

```sh
./tuiide-build/tuiide /path/to/cpp-project
```

If the path is omitted, the IDE opens a no-project start screen; choose Open
Project, Recent Projects, or New Project from the File menu. User projects are
built in a sibling directory named `<project>-build`. Breakpoints, watch expressions,
and register visibility are persisted in
`<project>-build/.tuiide-session.json`; generated session state does not modify
the source tree.

`Alt+P` selects an enabled configure preset from `CMakePresets.json` or
`CMakeUserPresets.json`; includes, inheritance, conditions, environment/source
macros, and relative `binaryDir` values are resolved. The preset selection and
its build directory are remembered without writing state into the source tree.
`Alt+B` selects a build preset and automatically activates its associated
configure preset. Build-preset targets, configuration, clean/verbose settings,
environment, and native options are applied by CMake; the IDE overrides any
`jobs` value with the **Parallel jobs** value from Project Settings.
After the first configure, `Alt+T` lists executable targets discovered through
the CMake File API. The selected target/configuration is remembered in the
session file and is used by Build, Run, and Debug. Builds always retain
the configured parallel-job limit. **Run → Configure** only generates the build
tree; `F7` performs an incremental Build and configures automatically when needed.

Configure always requests `compile_commands.json`. clangd is started with the
selected configure preset's build directory and restarts when that directory changes.
The status line reports `CDB: entry` for an exact source entry, `CDB: fallback` when
clangd must infer flags (commonly for headers), and `CDB: missing` before configure.
Rebuild runs Clean → Build, while Clean and Cancel Build are available separately.
The status line and Output show the active stage, available percentage, duration,
and final exit code.
Run and Debug never scan the build directory for executable files. They use the
selected executable target reported by CMake File API. **Run → Launch
configuration** can instead provide an explicit executable and can set a working
directory and quoted argument list; leaving the executable empty keeps the CMake
target while applying the configured directory and arguments. Missing or
non-executable files are reported without starting a process.

## Keyboard shortcuts

| Key | Action |
|---|---|
| `Ctrl+N`, `Ctrl+O`, `Ctrl+S` | New, open, and save the active document |
| File → Save All | Save every modified document |
| File → Close Others / Close All / Reopen Closed | Manage groups of editor documents and restore the last closed file |
| File → New Project / Close Project | Create a configured C/C++ CMake project or unload the current project |
| Project menu | Refresh/filter the tree and manage files or directories |
| `Ctrl+W` | Close the active document |
| `Ctrl+E` | Focus the Project tree |
| `Insert` in Project | Create a file or C++ class from a template and update CMake |
| `Ctrl+PageUp`, `Ctrl+PageDown` | Switch open documents |
| `Ctrl+F`, `Ctrl+G` | Open Find/Replace and go to line |
| Search menu | Find next/previous, Replace, and search across the project |
| `Ctrl+Z`, `Ctrl+Y` | Undo and redo |
| `Shift+Arrows`, mouse drag | Select text |
| `Ctrl+A`, `Ctrl+C`, `Ctrl+X`, `Ctrl+V` | Select all, copy, cut, and paste through the system clipboard |
| `Ctrl+Space`, `F1` | Completion picker and hover information |
| Tools → Signature help | Show function overloads and the active parameter |
| `F2` | Preview and confirm renaming a symbol across the workspace |
| `F3`, `F4` | Go to definition and find references |
| `F7`, `F6` | Incremental build and run |
| Run menu | Configure, Rebuild, Clean, or cancel an active build |
| Run → Stop program | Terminate the integrated or external Run process |
| Run → Launch configuration | Choose a CMake target or executable, arguments, environment, stdin, pre-launch build, and an optional external terminal |
| `Alt+P` | Select a CMake configure preset |
| `Alt+B` | Select a compatible CMake build preset |
| `Alt+T` | Select a CMake executable target and configuration |
| `F8` | Go to the next build or clangd diagnostic |
| `Alt+E` | Open the combined build/clangd Problems picker |
| `F5` | Start or continue debugging |
| `Shift+F5` (`F17`) | Pause the debuggee |
| Debug → Stop / Restart | Terminate the GDB session or launch a clean replacement session |
| `F9` | Set a breakpoint on the current line |
| `F10` | Activate the main menu |
| `Ctrl+Q`, `Alt+X`, `Ctrl+D` | Exit the IDE |
| `Ctrl+C`, `Ctrl+D` in active Console input | Interrupt the PTY program or send EOF |
| `Ctrl+F10`, `F11`, `F12` | Next, step into, and finish |
| `Alt+W` | Add a GDB watch expression |
| `Delete` in Debug panel | Remove the selected watch expression |
| `Space`, `F2`, `Delete` in Breakpoints | Enable/disable, edit properties, or remove a breakpoint |
| `Delete` in Project panel | Remove the selected file from CMake, optionally also from disk |
| `Alt+R` | Show or hide the main amd64 registers |
See `TODO.md` for implemented features and planned improvements.

Language-server workspace edits are shown in a preview before they are applied. The IDE
supports version-checked text edits and transactional create, rename, and delete operations
inside the project root; failed multi-file edits restore staged files automatically.

Background clangd initialization, CMake operations, builds, program runs, and
debug sessions also report start/completion through short, non-modal banners;
full command output remains available in the lower Output and Build tabs.

Open files, Project, Debug, and Breakpoints provide context menus through a
right click or the keyboard Menu/Shift+F10 key. Their actions also expose direct
panel shortcuts such as Enter, Insert, Delete, F2, Space, and F5 where relevant.

The Tools menu exposes clangd Code Actions (`Alt+A`), Organize Includes, and
Switch Header/Source. Returned edits are revision-checked and use the same
multi-file preview and confirmation flow as Rename.

Workspace Symbols searches the clangd project index. Call Hierarchy combines
incoming and outgoing calls, while Type Hierarchy combines direct supertypes
and subtypes; selecting any result opens its source location. Document symbols
remain available continuously through the Outline sidebar.

Rapid editor changes are coalesced into a single LSP update after 150 ms. Interactive
requests flush pending text first, superseded requests are cancelled, and responses
for an older revision or a document that is no longer active are discarded.

## Clipboard behavior

Copy and cut always update the in-process clipboard. When available, the same
UTF-8 text is sent to `wl-copy`, `xclip`, or `xsel`; OSC 52 also supports remote
terminals and tmux. Paste reads from the Wayland/X11 helper and falls back to
the in-process value. OSC 52 copying is limited to 100 KiB and can be disabled
with `TUIIDE_OSC52=0`. Set `TUIIDE_CLIPBOARD_NATIVE=0` to disable desktop
helpers. OSC 52 paste queries are intentionally avoided so terminal replies
cannot be interpreted as editor keystrokes.

When GDB stops, the **Debug** panel lists watch expressions, optional amd64
registers, threads, stack frames, and local variables. Watches and visible
register state remain separate from the **Breakpoints** panel. That panel shows
enabled and disabled points together with GDB verification state. Press `Space`
to enable or disable the selected point, `F2` or Enter to edit its condition,
ignored-hit count, or log message, and `Delete` to remove it. A breakpoint with
a log message prints the message and continues automatically. Debug menu commands
also edit, remove, or clear points. All properties are persisted in the project
debug session; sessions written by older versions remain compatible. Registers
are refreshed after every stop; GDB evaluation errors appear inline.
Select a thread to make it current and refresh its stack, or select a frame to
open its source. Enter and mouse selection are both supported.
