# TUI IDE

[Русский](README.md) | [English](README.en.md)


**TUI IDE** — полноэкранная IDE для разработки на C и C++ в терминале Linux/amd64.
Она написана на C++20 и использует [Final Cut](https://github.com/gansm/finalcut),
CMake, clangd и GDB/MI. Исходный текст и интерфейс работают в UTF-8.

## Возможности

- Редактор с подсветкой C, C++, CMake (`CMakeLists.txt`, `*.cmake`), подсветкой
  clangd, диагностикой, completion, hover, переходами к определению и ссылкам.
- Поиск и замена в документе и проекте, форматирование через `clang-format`,
  outline, code actions, rename symbol и hierarchy-запросы clangd.
- Дополнительные clangd language insights через **Tools → Language insights**:
  inlay hints, подсветка символов, folding/selection ranges, code lens,
  completion resolve и include hierarchy с позициями в Output.
- Создание, импорт и настройка CMake-проектов; targets, configure/build presets,
  выбор генератора, build-каталога и количества задач сборки.
- Асинхронные Configure/Build/Clean/Rebuild, список проблем и журнал вывода.
- Интеграция CTest: JSON-discovery, запуск всех, выбранного или ранее упавших
  тестов, test presets и переход к строке сбоя из панели Tests.
- Асинхронный анализ текущего файла, CMake target или проекта через clang-tidy,
  cppcheck и IWYU; отдельная ASan/UBSan-сборка без изменения обычного build.
- Менеджер toolchain kits обнаруживает GCC/Clang, Ninja/Make, GDB/LLDB, sysroot
  и проектные CMake toolchain-файлы, проверяет версии и применяет kit к проекту.
- Именованные конфигурации Run/Debug с клонированием и быстрым выбором; запуск в
  интегрированном PTY или внешнем терминале, аргументы, среда и `stdin`.
- Отладка через GDB/MI или опциональный LLDB/DAP: breakpoints/logpoints, стек,
  потоки, локальные переменные,
  watches, registers, вычисление выражений, память и дизассемблер; текущая строка
  остановленного приложения выделяется в редакторе.
- Backend выбирается в **Project → Project Settings → Debugger**. Для LLDB
  установите `lldb-dap` (в старых поставках — `lldb-vscode`) либо укажите
  абсолютный путь к adapter. Attach, core dump и управление сигналами пока
  доступны только для GDB/MI.
- **Debug → Attach to process...** присоединяет GDB к выбранному Linux-процессу
  из `/proc`. Команда Stop сначала выполняет `-target-detach`, поэтому внешний
  процесс не завершается вместе с отладчиком.
- **Debug → Open core dump...** открывает выбранные executable и core-файл в
  режиме только чтения. Доступны стек, потоки, переменные, watches, registers,
  evaluate, память и дизассемблер; продолжение, шаги, сигналы и присваивание
  заблокированы.
- **Debug → Signal handling**: просмотр политик в Output, настройка stop/print/pass и
  отправка сигнала с подтверждением и продолжением процесса. `0` подавляет
  текущий сигнал. Политики действуют до завершения текущей сессии GDB;
  служебные SIGINT/SIGTRAP исключены из настройки обработки.
- Дерево проекта, шаблоны C/C++ файлов и C++ классов с обновлением CMake.

## Требования

Поддерживается только Linux на amd64. Для Debian/Ubuntu установите зависимости:

```sh
sudo apt install g++ cmake libgpm-dev nlohmann-json3-dev clangd clang-format gdb lldb clang-tidy cppcheck iwyu gcovr valgrind linux-perf
```

`clangd`, `clang-format`, GDB, `lldb-dap` и анализаторы нужны для соответствующих функций.
IDE запускается без них, но явно сообщает о недоступной возможности в панели Output. Для буфера
обмена рабочего стола дополнительно можно установить `wl-clipboard` (Wayland) либо
`xclip`/`xsel` (X11).

## Сборка

Команды выполняются из корня репозитория. На данном хосте сборку следует вести в
один поток:

```sh
git -C tuiide submodule update --init --recursive
cmake -S tuiide -B tuiide-build -DCMAKE_BUILD_TYPE=Release
cmake --build tuiide-build --parallel 1
ctest --test-dir tuiide-build --output-on-failure
```

Артефакты находятся только в `tuiide-build/`; исходный каталог не загрязняется.
Для диагностической сборки можно добавить
`-DTUIIDE_ENABLE_SANITIZERS=ON`. Дополнительные проверки доступны через
`TUIIDE_ENABLE_CLANG_TIDY`, `TUIIDE_ENABLE_CPPCHECK` и
`TUIIDE_ENABLE_LONG_TESTS`.

## Запуск

Откройте существующий CMake-проект или создайте/импортируйте его из меню **File**:

```sh
./tuiide-build/tuiide /путь/к/проекту
./tuiide-build/tuiide --project /путь/к/проекту --diagnostic \
  --log-file /tmp/tuiide.log
```

Полный список параметров доступен без запуска терминального интерфейса:

```sh
./tuiide-build/tuiide --help
./tuiide-build/tuiide --version
```

Настройки проекта сохраняются в `.tuiide-project.json`; сессия и recovery-данные
располагаются в каталоге сборки. Пользовательские сочетания клавиш, тема и цвета
сохраняются в `$XDG_CONFIG_HOME/tuiide/` (обычно `~/.config/tuiide/`).

## Работа в интерфейсе

Верхнее меню открывается `F10` либо `Alt+F/E/S/R/P/D/T/W/H`. Подчёркнутые буквы
действуют внутри открытого меню; глобальные сочетания команд настраиваются через
**Tools → Configure shortcut**.

- `F5`, `F6`, `Ctrl+B`, `F9` — debug, run, build и breakpoint.
- `F7`, `F8`, `Alt+F8` — step into, step over (Next) и step out; `Ctrl+F8` —
  следующая диагностика.
- `Ctrl+N`, `Ctrl+O`, `Ctrl+S`, `Ctrl+W` — новый файл, открыть, сохранить, закрыть.
- `Ctrl+F` — поиск/замена; `Ctrl+Space` — completion; `Alt+K` — command palette.
- `Alt+L` — менеджер конфигураций Run/Debug; `Alt+Shift+L` — быстрый выбор.
- `Alt+Shift+U` — добавить watch; `Alt+U` — вновь открыть закрытый файл.
- `Ctrl+L` зарезервирован Final Cut для перерисовки экрана и не назначается на
  команды IDE. Старые запрещённые назначения игнорируются с предупреждением;
  остальные пользовательские предпочтения сохраняются.
- `Alt+PageUp/PageDown` — вкладки боковой панели; `Alt+Shift+PageUp/PageDown` —
  вкладки нижней панели.

Слева находятся страницы **Open files**, **Project**, **Outline**, **Debug**,
**Breakpoints** и **Tests**. Их видимость управляется меню **Window**. В Tests
нажмите `Space`, чтобы запустить выбранный тест, и `Enter`, чтобы открыть первую
строку его сбоя. Discovery и групповые запуски находятся в **Run → Tests**.
Внизу размещены
**Output**, **Problems**, **Build**, интерактивный **Terminal** и **Analysis**.
Анализ запускается через **Tools → Run analysis / profile**; распознанные
сообщения также появляются в Problems. Coverage создаёт отдельную сборку
`.tuiide-coverage` внутри build-каталога и формирует отчёт gcovr. Valgrind и
perf запускают активную Run/Debug configuration. Непокрытые строки, stack
frames Valgrind и samples perf открываются из Problems клавишей `Enter`.
Выбор обнаруженного набора компилятора, генератора и отладчика находится в
**Tools → Toolchain kits**.

## Структура репозитория

- `include/tuiide/` — публичные заголовки.
- `src/` — ядро IDE, интерфейс Final Cut и интеграции CMake/clangd/GDB.
- `tests/` — unit-, integration- и PTY-тесты.
- `third_party/finalcut/` — закреплённая копия Final Cut.
- `docs/` — man-страница; `completions/` — Bash completion.
- `../tuiide-build/` — рекомендуемый внешний каталог сборки.

## Установка и пакетирование

```sh
cmake --install tuiide-build --prefix /opt/tuiide
/opt/tuiide/bin/tuiide --version

(cd tuiide-build && cpack -G TGZ)
(cd tuiide-build && cpack -G DEB)
```

Установка включает исполняемый файл, man-страницу, Bash completion, README и
лицензию Final Cut.

## Лицензии

TUI IDE использует vendored-копию Final Cut. Её лицензия находится в
[`third_party/finalcut/LICENSE`](third_party/finalcut/LICENSE). Проверка требует
чистый checkout, правильный `origin` и доступность закреплённого commit из ранее
полученной `refs/remotes/origin/main`:

```sh
cmake --build tuiide-build --target check-finalcut-vendor --parallel 1
ctest --test-dir tuiide-build --output-on-failure -L vendor
```

Для воспроизводимого обновления сначала получите upstream-ветку, затем
переключите submodule на проверенный commit без локальных патчей:

```sh
git -C tuiide submodule update --init --recursive third_party/finalcut
git -C tuiide/third_party/finalcut fetch --prune origin main
git -C tuiide/third_party/finalcut switch --detach <commit>
git -C tuiide/third_party/finalcut status --short
git -C tuiide/third_party/finalcut merge-base --is-ancestor \
  <commit> refs/remotes/origin/main
```

Последние две команды должны завершиться без вывода и с кодом `0`. После ревью
обновите `TUIIDE_FINALCUT_VERSION` и `TUIIDE_FINALCUT_COMMIT` в
`cmake/FinalCutVendor.cmake`, запустите обе проверки выше и закоммитьте gitlink
submodule вместе с файлом pin. Не вносите изменения непосредственно в исходники
`third_party/finalcut/`.
