# Запуск на Linux и Windows 10/11

## Готовые сборки

Workflow **Linux and Windows** собирает Linux-архив и переносимый Windows ZIP.
Откройте вкладку [Actions](https://github.com/PaperGooseRus/StartApp/actions),
выберите успешный запуск для нужной ветки и скачайте соответствующий artifact.
Скачивание artifacts GitHub может требовать входа в аккаунт.

- **Windows 10/11 x64:** распакуйте ZIP целиком в любую папку и запустите
  `StartApp-Windows-x64/startapp.exe`. GTK4, DLL, схемы настроек и данные
  включены в папку. MSYS2 нужен для сборки, для запуска готового ZIP он не нужен.
  В командной строке используйте `startapp-cli.exe`.
- **Linux x64:** архив CI собирается в Ubuntu 24.04. Распакуйте и запустите
  `bin/startapp`. GTK4 >= 4.12 и совместимые системные библиотеки должны быть
  установлены. Для другого дистрибутива надёжнее собрать программу на нём.
  Локальный архив, собранный в Debian 13, рассчитан на Debian 13: он не является
  универсальным статическим Linux-пакетом.

Графический сеанс обязателен для окна приложения. Консольный `--inspect`
работает без дисплея. Windows-пакет рассчитан на x64, не ARM64 и не 32-bit.

## Сборка на Linux

Ubuntu 24.04 / Debian 13:

```sh
sudo apt-get update
sudo apt-get install build-essential pkg-config libgtk-4-dev cmake ninja-build
sh scripts/build-linux.sh
./build/linux/startapp /путь/к/проекту
```

Скрипт выполняет сборку Release, тесты ядра и создаёт
`build/linux/StartApp-0.2.0-Linux-x86_64.tar.gz` (имя архитектуры зависит от машины).

Установка бинарника, ярлыка меню и иконки для своего пользователя:

```sh
cmake --install build/linux --prefix "$HOME/.local"
"$HOME/.local/bin/startapp"
```

Для системной установки используйте выбранный системный prefix и соответствующие
права. Старый Makefile остаётся доступен: `make -j2`, `make test`, `make run`.

GUI-проверка в среде без дисплея:

```sh
sudo apt-get install xvfb xauth dbus-daemon
xvfb-run -a -s '-screen 0 1600x1000x24' dbus-run-session -- \
  ctest --test-dir build/linux -R '^gui$' --output-on-failure
```

## Сборка на Windows 10/11 x64

1. Установите [MSYS2](https://www.msys2.org/) и откройте терминал **MSYS2 UCRT64**.
2. Обновите MSYS2 по его инструкции. Если обновление требует закрыть терминал,
   снова откройте UCRT64 и завершите обновление.
3. Установите инструменты и GTK4 из одного окружения UCRT64:

```sh
pacman -Syu
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-gtk4 \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-binutils
```

Перейдите к исходникам. Например, для `C:\Projects\StartApp`:

```sh
cd /c/Projects/StartApp
bash scripts/build-windows.sh
./build/windows/startapp.exe
```

Скрипт собирает GUI и консольный вариант, выполняет тесты ядра и создаёт
`dist/StartApp-Windows-x64.zip`. Для повторной упаковки сначала переместите или
удалите предыдущую сгенерированную папку `dist/StartApp-Windows-x64`.
Скрипт не перезаписывает её автоматически.

Проверка интерфейса в обычном графическом сеансе Windows:

```sh
ctest --test-dir build/windows -R '^gui$' --output-on-failure
```

После распаковки переносимой сборки в PowerShell:

```powershell
& 'C:\Apps\StartApp-Windows-x64\startapp.exe' 'C:\Projects\мой проект'
& 'C:\Apps\StartApp-Windows-x64\startapp-cli.exe' --inspect 'C:\Projects\мой проект'
```

Оставляйте рядом с EXE все DLL и каталоги `share`, `etc`, `lib`. Не копируйте
один EXE отдельно. Программа находит данные относительно расположения EXE,
поэтому рабочий каталог и пробелы/русские символы в пути не требуют настройки.
Графическая версия не открывает консольное окно. Для диагностических сообщений
и JSON есть консольная версия. На Windows по умолчанию выбран Cairo renderer;
пользовательское значение `GSK_RENDERER` сохраняется.

Поддерживаемая сборка Windows использует GCC из UCRT64; Visual Studio/MSVC
не поддерживается текущими GLib cleanup-макросами. Не смешивайте DLL из UCRT64,
MINGW64, CLANG64 или из случайных GTK-архивов.

## Проверки и упаковка

CI запускает ядро и GUI отдельно на Ubuntu и Windows. Дополнительно Windows ZIP
распаковывается в другой каталог с пробелами; обе версии EXE проходят smoke-test
с PATH без MSYS2. Так проверяется комплектность DLL и переносимость данных.
Artifacts загружаются только после успешных обязательных проверок.

Тесты символических ссылок на Windows могут быть пропущены, если Windows не
разрешает их создание без Developer Mode/повышенных прав. Проверка POSIX-прав
0640 выполняется только на Linux; остальные проверки сохранения выполняются
на обеих платформах. Результаты CTest и скриншоты публикуются отдельными artifacts.

Windows-пакет включает транзитивные DLL, динамические GdkPixbuf-модули с их DLL,
переносимый относительный cache загрузчиков, GTK-схемы, иконки и лицензии
зависимостей. Pacman сохраняет штатную проверку подписей пакетов.

Установка и упаковка не выполняют команды из открываемых пользовательских
проектов. Для функциональных ограничений анализа см. [README](../README.md).
