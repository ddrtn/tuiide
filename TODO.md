# TUI IDE — план доработок

Документ составлен после аудита меню, редактора, CMake, clangd, GDB/MI и тестов.
Текущая версия уже умеет редактировать несколько UTF-8-файлов, создавать C/C++
проекты и файлы, собирать CMake-проекты, выполнять базовые LSP-операции и
отлаживать программу. Ниже перечислены недостающие функции и места, где команда
формально присутствует, но молча ничего не делает или даёт неполный результат.

## P0 — исправить неполные и «пустые» команды

- [x] Ввести единое состояние команд и динамически отключать пункты меню, когда
  нет документа/проекта, clangd не готов, сборка идёт либо GDB не остановлен.
- [x] Для каждой команды показывать понятный результат: `not available`,
  `in progress`, `no results` или текст ошибки. Не оставлять пользователя перед
  пустым диалогом или без реакции.
- [x] Добавить пояснения для недоступных File, CMake, LSP, Run и Debug-команд,
  вызванных горячими клавишами, даже если соответствующий пункт меню отключён.
- [x] Исправить базовый LSP lifecycle: ждать `initialize`, отправлять правильный
  `languageId` (`c`/`cpp`) и percent-encode URI с пробелами и UTF-8.
- [x] Показывать ошибки запуска, инициализации и аварийного завершения clangd;
  отличать JSON-RPC error от корректного пустого результата.
- [x] Доработать **Completion**: использовать `textEdit`/`insertText`, detail и
  документацию, сохранять порядок clangd и отбрасывать ответ устаревшей ревизии.
- [x] Добавить отображение completion kind и отдельную поддержку signature help.
- [x] Показывать **Symbol information** в отдельном прокручиваемом окне, включая
  явное сообщение «информация отсутствует», а не только дописывать текст в Output.
- [x] Для **Definition**, **References** и **Rename** обрабатывать пустой ответ и
  JSON-RPC error без открытия пустого picker/dialog.
- [x] Перед rename показывать preview и не менять закрытые файлы без подтверждения.
- [x] Заменить простой **Find** полноценной панелью: Find next/previous, Replace,
  Replace all, case/whole word/regex; добавить поиск и замену по проекту.
- [x] Перед Build/Run/Debug сохранять все изменённые файлы проекта или запрашивать
  подтверждение; отмена сохранения должна отменять команду.
- [x] Проверять состояние отладчика для Pause/Next/Step/Finish и сообщать причину,
  если команда сейчас невозможна.
- [x] Добавить Debug Stop и Restart. После завершения inferior корректно обновлять
  статус, стек, locals и доступность команд.
- [x] Добавить PTY-тест, проходящий каждый пункт меню в допустимом и недопустимом
  контексте; отдельно проверить ответы LSP «нет результатов» и ошибки процесса.
  PTY suite покрывает все верхние меню, модальные normal/cancel/error сценарии,
  пустые/ошибочные LSP-ответы и интерактивные GDB-операции.
  - [x] Добавить обход всех девяти верхних меню, видимый Project Refresh и
    normal/empty clipboard-сценарии; сохранить полный configure/build/run smoke.
  - [x] Выполнить из PTY оставшиеся изменяющие проект команды и каждый модальный
    диалог в normal/cancel/error вариантах.
    - [x] Покрыть создание каталога, rename/move и удаление пустого каталога:
      normal, cancel и отказ для пути вне проекта без изменений на диске.
    - [x] Покрыть шаблон заголовочного файла и удаление файла: normal, cancel,
      duplicate-path error и запрет удаления изменённого открытого документа.
    - [x] Покрыть New Project и Project Settings: normal, cancel и безопасный
      error-путь с проверкой файловой системы и сохранённых настроек.
    - [x] Покрыть Window/Help: справку, About, фильтр Problems, copy/clear,
      защиту последней sidebar-панели и сброс размеров.
    - [x] Покрыть Tools: shortcut table, настройку shortcut, темы и цвета в
      normal/cancel/error и no-project вариантах с проверкой сохранения.
    - [x] Покрыть Search: find next/previous, go-to-line, project search,
      replace и Problems в normal/cancel/error вариантах.
    - [x] Покрыть оставшиеся LSP/Debug диалоги.
      - [x] Проверить Hover, Definition, References, Rename и Code Actions через
        fake clangd: normal/empty/error, отмену preview и неизменность файла.
      - [x] Проверить Signature Help, Workspace Symbols и Call/Type Hierarchy
        через меню Tools: normal/empty/error и отмену выбора результата.
      - [x] Проверить breakpoint properties, watches, evaluate/set variable,
        disassembly и memory: normal/cancel/error и сохранение debug session.

## P1 — проекты и CMake

- [x] Добавить **File → Open Project**, список недавних проектов и стартовый экран
  без автоматического принятия текущего каталога за CMake-проект.
- [x] Проверять при открытии наличие `CMakeLists.txt` и не запускать clangd для
  произвольного каталога.
- [x] Добавить отдельный flow импорта каталога исходников без `CMakeLists.txt` и
  создания CMake-проекта непосредственно в выбранном каталоге.
- [x] Сделать диалог **Project Settings**: source/build directory, generator,
  toolchain, C/C++ compiler, standard, build type, environment и clangd arguments.
- [x] Сохранять все настройки проекта версионированно и атомарно; поддержать
  относительный build directory внутри проекта и правильный `.gitignore`.
- [x] Разделить Configure, Build, Rebuild, Clean и Cancel Build; показывать этап,
  exit code, длительность и прогресс. Число jobs по умолчанию брать из CPU хоста
  и разрешать переопределять в настройках проекта.
- [x] Убрать поиск «последнего executable» по каталогу сборки. Run/Debug должны
  использовать только выбранную цель CMake File API либо явную launch configuration.
- [x] Расширить Project tree до всех файлов проекта, добавить фильтры, refresh,
  rename/move, создание и удаление каталогов. Не скрывать превышение лимита файлов.
- [x] При rename/move безопасно обновлять точные ссылки во вложенных CMake-файлах;
  предусмотреть rollback при частичной ошибке.
- [x] Улучшить мастер проекта: зависимые поля C/C++, выпадающий список стандартов,
  выбор Ninja/Make, Debug/Release, install layout и немедленная проверка CMake.

## P1 — редактор

- [x] Реализовать экранные колонки через `wcwidth`: табуляция, широкие CJK-символы,
  emoji и combining marks; мышь, выделение и горизонтальная прокрутка не должны
  ставить курсор внутрь UTF-8 code point.
- [x] Добавить Save All, Close All/Close Others, reopen closed file и различимые
  подписи для файлов с одинаковым basename.
- [x] Исправить dirty-state после undo к сохранённой ревизии; заменить полные
  снимки документа на сгруппированные операции, чтобы ввод большого файла не
  расходовал память на копию при каждом символе.
- [x] Добавить умные отступы, настройку tab width/use spaces, парные скобки и
  кавычки, toggle comment, duplicate/move line и удаление строки.
- [x] Добавить форматирование документа/выделения через `clang-format` и отображать
  ошибку или отсутствие утилиты.
- [x] Сохранять BOM/EOL/final newline и права файла; использовать атомарную запись.
- [x] Отслеживать внешние изменения и удаление открытого файла; предложить reload,
  compare или keep. Добавить autosave/recovery после аварийного завершения.
- [x] Добавить breadcrumb/outline символов, minimap не требуется; приоритетнее
  быстрый список функций, классов и заголовков через LSP documentSymbol.

## P1 — запуск и отладка

- [x] Добавить launch configurations: executable/target, arguments, working
  directory, environment, stdin, pre-launch build и режим external terminal.
- [x] Реализовать интерактивный встроенный Terminal/Console на PTY, чтобы Run и
  debuggee могли читать stdin, получать размер терминала и сигналы.
- [x] Сделать отдельную панель Breakpoints: enable/disable/delete all, condition,
  hit count и logpoint; показывать подтверждённые/неразрешённые точки.
- [x] При выборе stack frame выполнять `-stack-select-frame`, затем обновлять locals
  и watches. Добавить дерево переменных с раскрытием структур/указателей.
- [x] Заменить строковый разбор GDB/MI устойчивым парсером records/tuples/lists,
  корректно декодировать escapes и показывать MI errors пользователю.
- [x] Добавить expression evaluator, изменение значения переменной, disassembly и
  memory view как функции второго этапа; не смешивать их с базовой стабилизацией.

## P2 — интерфейс и удобство

- [x] Разделить нижнюю область на вкладки Output, Problems, Build и Terminal;
  добавить очистку, копирование, фильтрацию и переход по строке двойным кликом.
- [x] Сделать размеры sidebar/output изменяемыми и сохраняемыми в сессии; проверить
  все диалоги на терминалах 60×18 и на resize во время показа.
- [x] Добавить command palette, настраиваемые shortcuts и таблицу конфликтов клавиш.
- [x] Добавить настройки темы и цветов диагностики/семантических токенов, сохранив
  доступную контрастность в 8/16/256-color терминалах.
- [x] Показывать ненавязчивые notifications для фоновой индексации, configure,
  build и завершения программы вместо передачи всей обратной связи только в Output.
- [x] Добавить контекстные меню Project/Open files/Debug и доступные с клавиатуры
  эквиваленты всех их действий.

## P2 — расширение clangd

- [x] Поддержать code actions/quick fixes, organize includes и header/source switch.
- [x] Добавить workspaceSymbol, documentSymbol, call hierarchy и type hierarchy.
- [x] Обрабатывать `workspace/applyEdit`, file rename/create/delete и versioned edits.
- [x] Использовать compile commands выбранной CMake-конфигурации и явно показывать,
  когда файл не входит в compilation database.
- [x] Добавить debounce для `didChange`, cancellation устаревших запросов и защиту
  ответов от применения к уже переключённому документу.

## P3 — архитектура, качество и поставка

- [x] Разделить крупный `IdeWindow` на controllers/services для документов,
  проектов, build, LSP и debug; вынести диалоги из anonymous namespace.
  - [x] Вынести владение открытыми документами, активное состояние, дедупликацию
    путей и историю Reopen Closed в `DocumentSession`.
  - [x] Вынести формирование команд CMake configure/build/clean, presets, targets,
    configurations и parallel jobs в тестируемый `BuildCommandService`.
  - [x] Вынести владение корнем проекта, настройками и производными путями
    build/session/recovery в `ProjectSession`.
  - [x] Вынести общий центрируемый диалог и универсальные Prompt, Selection,
    Command Palette, Text и Confirm Text из `ide_window.cpp`.
  - [x] Вынести wizard-диалоги создания и импорта проекта в
    `project_dialogs.cpp`.
  - [x] Вынести диалоги выбора проектного файла и каталога, включая создание
    подкаталогов, в `project_dialogs.cpp`.
  - [x] Вынести `ProjectSettingsDialog` с адаптивной раскладкой и валидацией
    полей в PIMPL-реализацию `project_dialogs.cpp`.
  - [x] Вынести `LaunchSettingsDialog` с target/executable/stdin/environment и
    настройкой внешнего терминала в PIMPL-модуль `run_dialogs.cpp`.
  - [x] Вынести свойства breakpoint/logpoint и проверку ignore-hit count в
    PIMPL-модуль `debug_dialogs.cpp`.
  - [x] Вынести `SearchAction`, `SearchRequest` и Find/Replace UI в
    PIMPL-модуль `search_dialogs.cpp`.
  - [x] Вынести последний `ClassOptionsDialog`; в `ide_window.cpp` больше нет
    определений классов диалогов.
  - [x] Вынести владение operation/stage и переходы Configure→Build,
    Clean→Build в тестируемый `BuildWorkflow`.
  - [x] Перенести continuation Run/Debug после pre-launch build в
    `BuildWorkflow` и централизовать запуск этого сценария.
  - [x] Удалить transitional aliases operation/stage: изменять build-state можно
    только через API `BuildWorkflow`.
  - [x] Перенести progress и время operation/stage из `IdeWindow` в
    `BuildWorkflow`.
  - [x] Вынести сборку разорванных строк build output, распознавание progress и
    накопление compiler diagnostics в тестируемый `BuildOutputCollector`.
  - [x] Вынести активный build-каталог, configure/build presets, executable
    targets и восстановление их выбора в тестируемый `CMakeSession`.
  - [x] Завершить вынос project/build orchestration: объединить process,
    workflow, output collection, polling, completion и cancel в `BuildSession`.
  - [x] Вынести координацию LSP-запросов, revisions и feedback из окна.
    - [x] Вынести readiness/revisions, debounce и version guards Outline,
      completion/code actions, а также labels feedback в `LspUiController`.
    - [x] Централизовать сбор и маршрутизацию остальных LSP response payloads
      через типизированный `LspEventBatch`.
  - [x] Вынести синхронизацию debugger state и панелей в `DebugUiController`.
- [x] Ввести структурированный event/log API вместо прямого `appendOutput()` из
  всех сценариев и централизовать состояние фоновых процессов.
  - [x] Вынести историю Output/Build, channel/source/severity, sequence/revision,
    очистку и ограничение размера в тестируемый `EventLog`; перевести основные
    Build/Run/Debug/LSP потоки на структурированные события.
  - [x] Объединить process/PTY lifecycle обычного запуска и PTY-консоль GDB в
    отдельный `RunSession`, сохранив разные состояния Run и Debug Console.
  - [x] Перевести оставшиеся project/editor/system сообщения с compatibility
    wrapper на явные source/severity.
- [x] Покрыть unit-тестами историю undo/dirty, atomic save, URI, LSP errors,
  GDB/MI parser, project import/settings и rollback CMake-операций.
  - [x] Проверить ветвление undo/redo, clean-state после save, BOM/CRLF,
    permissions, отсутствие временных файлов и неуспешный Save As.
  - [x] Проверить Unicode/reserved file URI, malformed URI и типизированное
    извлечение ошибок JSON-RPC без исключений на повреждённом ответе.
  - [x] Проверить вложенные GDB/MI records, escapes, stream/error records и
    детерминированный отказ на оборванном вводе.
  - [x] Проверить import/settings, stale preview, validation и rollback
    файловых и CMake-aware операций без частично применённых изменений.
- [x] Расширить integration tests: C и C++, paths с пробелами/Unicode, Ninja и
  Makefiles, несколько targets/configurations, external file changes и recovery.
  - [x] Проверить C++ smoke project в Unicode/space path и отдельную C17 matrix:
    Unix Makefiles/Debug, Ninja/Release, два executable targets, File API и запуск.
  - [x] Проверить внешнее изменение/удаление открытого файла и полный recovery
    lifecycle через пользовательский TUI-сценарий.
  - [x] Пройти через PTY импорт C++ каталога, создание build-каталога, preview,
    configure/build, launch arguments и интерактивный stdin для Run и Debug.
- [x] Добавить ASan/UBSan-конфигурацию, clang-tidy/cppcheck и отдельный длительный
  тест больших файлов; обычная сборка на разработческой машине остаётся однопоточной.
- [x] Добавить `--help`, `--version`, `--project`, лог-файл/diagnostic mode и ясные
  сообщения об отсутствующих clangd, CMake, GDB и clipboard helpers.
- [x] Подготовить установку/пакетирование для Linux amd64, man page и проверку
  обновления vendored Final Cut без локальных патчей.

## Критерии полноценной базовой версии

- [x] Ни один доступный пункт меню не остаётся без видимого результата.
- [x] Новый или импортированный C/C++ проект проходит configure/build/run/debug
  только из интерфейса, включая аргументы программы и интерактивный stdin.
- [x] Search/Replace, completion, hover, navigation, rename, formatting и quick fix
  имеют рабочие normal/empty/error сценарии.
- [x] Переключение и закрытие проектов не оставляет процессов, документов,
  диагностик, targets, breakpoints или настроек от предыдущего проекта.
- [x] Полный CTest проходит; PTY suite проверяет меню, основные диалоги и жизненный
  цикл проекта, а ограничения ptrace отмечаются как явный skip.
