# Стенд FastModbusBench для MasterSCADA 4D

Эти пять исходных файлов переданы с Windows 24 ноября 2026 года и соответствуют инструменту из аудита FastModbus от 22 ноября. Для работы нужны Windows x64, PowerShell 7, .NET 8 с поддержкой WPF и MasterSCADA 4D 1.3.10.35699.

В корне репозитория выполните `dotnet build tools/project/BenchProject.csproj -c Release`. Перед запуском закройте другие экземпляры редактора и убедитесь, что профиль не открывает рабочий проект автоматически. Создайте каталог `$env:TEMP/ms4d_fast_modbus_20261122` и выполните `pwsh -NoProfile -File tools/project/inspect_project.ps1 -Create`.

Скрипт создаёт отдельный тестовый проект для 12AI, использует loopback и отключает запись. Штатный компилятор выполняет `CheckProjectConfig` и `ExportConfig`. Путь результата записывается в `native-export-path.txt`. Сверьте `controller.zip`, строки `EXPORTED` и `COMPILE COMPLETE` в `project-inspect.log` и завершение собственного процесса. Параметр `-CompileProject` предназначен для созданного здесь каталога `FastModbusBench`.
