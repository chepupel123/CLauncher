🇬🇧 [English](README.md) | 🇷🇺 Русский | 🇰🇿 [Қазақша](README_KZ.md)

# CLauncher

Лёгкий нативный лаунчер Minecraft для Linux и Windows, написанный на C++20 с интерфейсом на Dear ImGui и OpenGL. Спроектирован и написан одним разработчиком под старое железо: без рекламы, без телеметрии, без магазина, без обязательного аккаунта для офлайн-игры.

<p align="center">
  <img src="screenshots/interface_ru_vanilla.png" alt="Интерфейс CLauncher (Vanilla)" width="700"/>
</p>

Требуется **OpenGL 3.3 Core** или новее. Работает на Linux (Debian, Ubuntu, Mint, Kali, Fedora, Arch, openSUSE, Alpine и других дистрибутивах) и Windows 10/11. Размер бинарного файла — около трёх мегабайт, лаунчер в режиме простоя потребляет примерно 30–50 МБ ОЗУ (память для самой Minecraft задаётся отдельно в интерфейсе). Основная машина для разработки и тестов — настольный ПК 2012 года. Это практический ориентир для старого железа, а не гарантия работы на абсолютно любой конфигурации.

## ЧТО УМЕЕТ

* **Установка Vanilla Minecraft прямо из лаунчера.** Клиент, библиотеки, ассеты и нативные библиотеки скачиваются автоматически с официальных серверов Mojang.
* **Поддержка Fabric (Minecraft 1.14+).** Fabric ставится через официальный инсталлятор (лаунчер сам запускает его `.jar`), версия загрузчика берётся актуальной с серверов Fabric Meta. Для более старых версий опция Fabric скрыта.
* **Оптимизационные моды одной галочкой.** Fabric API, Sodium, Lithium и FerriteCore. Версии модов подбираются автоматически под выбранную версию Minecraft через API Modrinth.
* **Защита Log4Shell.** Лаунчер автоматически добавляет аргумент `-Dlog4j2.formatMsgNoLookups=true` для всех версий Minecraft ниже 1.18.1, защищая вас от критической уязвимости (CVE-2021-44228).
* **Умная параллельная загрузка.** Адаптивный пул потоков анализирует количество файлов и ядра процессора, чтобы качать ассеты до 3–4 раз быстрее. Расчет скорости и ETA (оставшегося времени) стал точным и отзывчивым.
* **Отмена установки.** Вы можете безопасно закрыть окно лаунчера во время скачивания, и фоновый процесс установки корректно прервется, не запуская игру.

<p align="center">
  <img src="screenshots/interface_ru_fabric.png" alt="Интерфейс CLauncher (Fabric)" width="700"/>
</p>

* **Свои моды.** Кнопка «Мои моды» открывает папку модов конкретной версии Fabric. Любой `.jar`-файл, положенный туда вручную, загрузится при запуске игры. У каждой версии Minecraft своя папка модов, поэтому моды разных версий не перемешиваются между собой.
* **Ресурспаки.** Кнопка «Ресурспаки» открывает общую папку ресурспаков. Положите туда zip-архив, а затем включите его в меню наборов ресурсов в самой игре.
* **Автоматическое управление Java.** Лаунчер больше не зависит от системных пакетных менеджеров (apt, dnf, pacman) и не требует `sudo`. Он всегда скачивает портативную JRE (8, 17, 21 или 25 под выбранную версию Minecraft) с серверов Adoptium в свою папку. Работает из коробки на любом дистрибутиве.
* **Проверка целостности.** Скачиваемые файлы игры, а также моды проверяются по SHA1 (через OpenSSL) там, где Mojang или Modrinth предоставляют контрольную сумму. Повреждённые файлы перекачиваются автоматически.
* **Защита от двойного запуска.** Если случайно запустить CLauncher дважды, второе окно сразу закрывается — два процесса не качают ассеты в одну и ту же папку.
* **Сохранение сессии.** Никнейм, выбранная версия Minecraft, загрузчик модов, память и язык сохраняются между запусками.
* **Корректная работа с окном.** При нажатии Play окно лаунчера скрывается, пока идёт игра, и автоматически возвращается после выхода из Minecraft.
* **Отзывчивый интерфейс.** Вся установка идёт в фоновом потоке, окно не зависает. Вы можете переключать язык интерфейса прямо на лету, даже во время загрузки.
* **Два языка интерфейса.** Английский и русский, переключение на лету.
* **Логирование в файл.** Всё, что выводит лаунчер, пишется в `clauncher.log` в папке `~/.clauncher/logs/` (или `%APPDATA%\CLauncher\logs\` на Windows).
* **Безопасный запуск.** Игра запускается через `execv` на Linux и `CreateProcessW` на Windows, без привлечения командной оболочки. Длинный classpath передаётся через argfile.

## ПОДДЕРЖИВАЕМЫЕ ВЕРСИИ

* **Vanilla и Fabric:** все релизы ветки 1.x из манифеста Mojang, вплоть до линейки 1.21.x (Fabric — начиная с 1.14.x).
* **Линейка 26.x** теперь полностью поддерживается! Она требует Java 25 и современный инструментарий Fabric, которые лаунчер теперь автоматически скачивает и настраивает.

**Требования к системе:** любой современный дистрибутив Linux с glibc 2.35+ и **OpenGL 3.3 Core**, либо Windows 10 и 11. Готовый AppImage требует glibc 2.35 или новее.

## СКАЧАТЬ

Готовые сборки лежат на [странице Releases](https://github.com/chepupel123/CLauncher/releases):

* `CLauncher-Linux.zip` — Linux x86_64. Внутри — самодостаточный `CLauncher-x86_64.AppImage`.
* `CLauncher-Windows-x64.zip` — Windows 10/11 x64 (бета). Внутри — `CLauncher.exe` вместе с необходимыми DLL (GLFW/curl/OpenSSL) и сертификатами.

Если хотите собрать из исходников — инструкция ниже.

## БЫСТРЫЙ СТАРТ НА LINUX

**Debian / Ubuntu / Mint / Kali / Pop!_OS:**
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config unzip \
  libglfw3-dev libgl1-mesa-dev libcurl4-openssl-dev libssl-dev \
  nlohmann-json3-dev libx11-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev fonts-dejavu-core

Fedora / RHEL / CentOS Stream:
bash

sudo dnf install -y gcc-c++ cmake git pkg-config unzip \
  glfw-devel mesa-libGL-devel libcurl-devel openssl-devel \
  nlohmann-json-devel libX11-devel libXrandr-devel \
  libXinerama-devel libXcursor-devel dejavu-sans-fonts

Arch / Manjaro / EndeavourOS:
bash

sudo pacman -S --needed base-devel cmake git pkg-config unzip \
  glfw mesa curl openssl nlohmann-json libx11 libxrandr \
  libxinerama libxcursor ttf-dejavu

Затем скачайте проект и соберите:
bash

git clone https://github.com/chepupel123/CLauncher.git
cd CLauncher
rm -rf build
mkdir build
cd build
cmake ..
make -j$(nproc)

Запустите: ./CLauncher
БЫСТРЫЙ СТАРТ НА WINDOWS

Понадобится MSYS2. Скачайте установщик с сайта msys2.org и установите в C:\msys64. Откройте терминал MSYS2 UCRT64 (не MINGW64!) и выполните:
bash

pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-curl \
  mingw-w64-ucrt-x86_64-openssl mingw-w64-ucrt-x86_64-glfw \
  mingw-w64-ucrt-x86_64-nlohmann-json make

Сборка:
bash

mkdir build
cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja

Появится файл CLauncher.exe. В релизном zip уже лежат все необходимые DLL и cacert.pem.
КАК ЭТО РАБОТАЕТ ВНУТРИ

    MinecraftInstaller скачивает клиент, библиотеки, ассеты и нативы.

    Fabric (опционально). Запускается инсталлятор, скачиваются Fabric API, Sodium, Lithium и FerriteCore.

    JavaManager. Скачивает и распаковывает нужную портативную JRE (8, 17, 21 или 25) с Adoptium.

    JavaLauncher. Запускает игру через execv (Linux) или CreateProcessW (Windows).

ГДЕ ЧТО ЛЕЖИТ

На Linux:

    Игра, миры, моды: ~/.minecraft/

    Настройки лаунчера, скачанная Java: ~/.clauncher/ (в частности ~/.clauncher/runtime/)

    Лог лаунчера: ~/.clauncher/logs/clauncher.log

На Windows:

    Игра, миры, моды: %USERPROFILE%\.minecraft\

    Настройки лаунчера, Java: %APPDATA%\CLauncher\

    Лог лаунчера: %APPDATA%\CLauncher\logs\clauncher.log

ИЗВЕСТНЫЕ ОГРАНИЧЕНИЯ

    Windows-исполняемый файл не подписан, поэтому SmartScreen может показать предупреждение при первом запуске («Подробнее» → «Выполнить в любом случае»).

    Первая установка небыстрая (тысячи файлов). Последующие запуски проверяют файлы по SHA1 и качают только недостающее.

    Кириллица в нике не принимается намеренно (только латиница, цифры и _).

ЕСЛИ ЧТО-ТО НЕ РАБОТАЕТ

    Ошибка GLFW init failed. Не установлен GLFW или нет графической сессии.

    Вопросительные знаки вместо текста. Установите пакет шрифтов fonts-dejavu-core (Debian/Ubuntu), dejavu-sans-fonts (Fedora), ttf-dejavu (Arch).

    Игра вылетает сразу после старта. Удалите несовместимый мод из папки mods.

    Загрузка остановилась. Нажмите Play снова: уже скачанные файлы сохранятся, докачаются только недостающие.
