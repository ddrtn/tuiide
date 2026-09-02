# TUI IDE

**TUI IDE** — полноэкранная IDE для разработки на C и C++ в терминале Linux/amd64.
Она написана на C++20 и использует [Final Cut](https://github.com/gansm/finalcut),
CMake, clangd и GDB/MI. Исходный текст и интерфейс работают в UTF-8.

## Возможности

- Редактор с подсветкой C, C++, CMake (`CMakeLists.txt`, `*.cmake`), подсветкой
  clangd, диагностикой, completion, hover, переходами к определению и ссылкам.
- Поиск и замена в документе и проекте, форматирование через `clang-format`,
  outline, code actions, rename symbol и hierarchy-запросы clangd.
- Создание, импорт и настройка CMake-проектов; targets, configure/build presets,
  выбор генератора, build-каталога и количества задач сборки.
- Асинхронные Configure/Build/Clean/Rebuild, список проблем и журнал вывода.
- Запуск в интегрированном PTY или внешнем терминале, аргументы, переменные среды
  и интерактивный `stdin`.
- Отладка через GDB: breakpoints/logpoints, стек, потоки, локальные переменные,
  watches, registers, вычисление выражений, память и дизассемблер.
- Дерево проекта, шаблоны C/C++ файлов и C++ классов с обновлением CMake.

## Требования

Поддерживается только Linux на amd64. Для Debian/Ubuntu установите зависимости:

```sh
sudo apt install g++ cmake libgpm-dev nlohmann-json3-dev clangd clang-format gdb
```

`clangd`, `clang-format` и GDB нужны для соответствующих функций. IDE запускается
без них, но явно сообщает о недоступной возможности в панели Output. Для буфера
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

- `F5`, `F6`, `F7`, `F9` — debug, run, build и breakpoint.
- `Ctrl+N`, `Ctrl+O`, `Ctrl+S`, `Ctrl+W` — новый файл, открыть, сохранить, закрыть.
- `Ctrl+F` — поиск/замена; `Ctrl+Space` — completion; `Alt+K` — command palette.
- `Alt+PageUp/PageDown` — вкладки боковой панели; `Alt+Shift+PageUp/PageDown` —
  вкладки нижней панели.

Слева находятся страницы **Open files**, **Project**, **Outline**, **Debug** и
**Breakpoints**. Их видимость управляется меню **Window**. Внизу размещены
**Output**, **Problems**, **Build** и интерактивный **Terminal**.

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
[`third_party/finalcut/LICENSE`](third_party/finalcut/LICENSE). Перед обновлением
зависимости проверьте её закреплённую ревизию командой:

```sh
cmake --build tuiide-build --target check-finalcut-vendor --parallel 1
ctest --test-dir tuiide-build --output-on-failure -L vendor
```
