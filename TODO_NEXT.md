# TUI IDE — следующий этап доработок

Список составлен после сравнения TUI IDE с PyTUI IDE. Завершённый исторический
план остаётся в `TODO.md`; здесь собраны новые задачи в порядке приоритета.

## P0 — стабильность

- [x] Сделать разбор всех ответов clangd устойчивым к `null`, отсутствующим
  полям и неверным JSON-типам; показывать ошибку протокола вместо молчаливого
  игнорирования и покрыть completion/hover/diagnostics regression-тестами.
- [x] Перенести явную перерисовку активной страницы `SidebarTabs` после фонового
  обновления Build, Problems, Output и Debug; добавить PTY-регрессию.
- [ ] Усилить `AsyncProcess`: закрывать дескрипторы на всех error paths, хранить
  process group отдельно и удалять фоновых потомков после завершения лидера.
- [ ] Усилить `PseudoTerminal`: очищать частично созданные spawn-объекты,
  ограничивать размер терминала и проверить повторный start/stop.
- [ ] Проверять принадлежность закреплённого коммита Final Cut fetched upstream-
  ветке и документировать воспроизводимое обновление без локальных патчей.

## P1 — рабочие процессы

- [ ] Добавить сохраняемое меню Recent Files с дедупликацией, очисткой истории
  и удалением недоступных путей.
- [ ] Заменить единственную launch-настройку списком именованных Run/Debug
  configurations с созданием, клонированием, удалением и быстрым выбором.
- [ ] Интегрировать CTest: discovery через JSON, запуск всех/выбранных/упавших
  тестов, test presets, панель Tests и переход из failure в исходный файл.
- [ ] Добавить запуск clang-tidy, cppcheck, sanitizers и include-what-you-use для
  файла/target/проекта с выводом результатов в Analysis и Problems.
- [ ] Добавить менеджер toolchain/kits: обнаружение GCC/Clang, GDB/LLDB,
  Ninja/Make, sysroot и CMake toolchain files с проверкой версий.
- [ ] Расширить clangd: inlay hints, document highlights, folding ranges,
  completion resolve, selection ranges, code lens и include hierarchy.
- [ ] Расширить отладку: Attach to Process, core dump, сигналы inferior и
  опциональный LLDB backend через DAP.

## P2 — развитие и качество

- [ ] Добавить coverage, Valgrind и профилирование с навигацией по результатам.
- [ ] Добавить минимальную Git-панель: status, diff, stage и история файла.
- [ ] Включить умеренный large-file regression в обычный CTest, сохранив тяжёлый
  стресс-тест опциональным.
- [ ] Добавить fuzz/property-тесты JSON-RPC, GDB/MI, CMake File API и malformed
  UTF-8, а также отдельные цели статического анализа без полной пересборки.
- [ ] Ввести ресурсы локализации и переключение русского/английского интерфейса.
