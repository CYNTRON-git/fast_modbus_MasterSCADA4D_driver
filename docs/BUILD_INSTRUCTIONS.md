# Инструкция по сборке протокола Fast Modbus RTU

## Подготовка окружения для сборки

### 1. Установка необходимых пакетов

Выполните следующие команды для установки необходимых инструментов сборки:

```bash
# Установка кросс-компилятора для ARM
sudo apt-get update
sudo apt-get install g++-arm-linux-gnueabihf

# Установка инструментов сборки
sudo apt-get install build-essential

# Предоставление прав на выполнение скрипта сборки
chmod +x makedrv.sh
```

### 2. Подготовка файлов API

#### 2.1. Перенос файлов API с Windows на Linux

1. На компьютере с Windows найдите директорию с API:
   ```
   C:\Program Files\MPSSoft\MasterSCADA 4D<номер версии>\API
   ```
   Или используйте альтернативный путь (например, `C:\mplc\api`)

2. Скопируйте все файлы из этой директории в произвольную папку на компьютере с ОС Linux.
   Рекомендуемый путь: `/home/user/src/API/platform/linux/api/`

3. Убедитесь, что структура папок соответствует следующей:
   ```
   /home/user/src/API/platform/linux/api/
   ├── makedrv.sh
   ├── modbus_rtu_protocol.h
   ├── modbus_rtu_protocol.cpp
   ├── fast_modbus_protocol.h
   ├── fast_modbus_protocol.cpp
   ├── mplc_cyntron.h
   ├── mplc_cyntron.cpp
   ├── dllmain.cpp
   └── ... (другие файлы API)
   ```

#### 2.2. Копирование библиотек (.so файлов)

1. Создайте директорию для библиотек:
   ```bash
   mkdir -p /home/user/src/API/platform/linux/api/mplc_lib_so
   ```

2. Скопируйте следующие `.so` файлы из папки исполнительной системы целевой платформы `/opt/mplc4`:
   ```bash
   cp /opt/mplc4/masterplc.so /home/user/src/API/platform/linux/api/mplc_lib_so/
   cp /opt/mplc4/mplc_archive.so /home/user/src/API/platform/linux/api/mplc_lib_so/
   cp /opt/mplc4/mplcshare.so /home/user/src/API/platform/linux/api/mplc_lib_so/
   cp /opt/mplc4/opcua.so /home/user/src/API/platform/linux/api/mplc_lib_so/
   cp /opt/mplc4/liblua.so /home/user/src/API/platform/linux/api/mplc_lib_so/
   cp /opt/mplc4/mplc_events.so /home/user/src/API/platform/linux/api/mplc_lib_so/
   ```

   Или выполните одной командой:
   ```bash
   cp /opt/mplc4/{masterplc.so,mplc_archive.so,mplcshare.so,opcua.so,liblua.so,mplc_events.so} \
      /home/user/src/API/platform/linux/api/mplc_lib_so/
   ```

### 3. Подготовка тулчейна

1. Скачайте и распакуйте нужный тулчейн для целевой платформы в директорию `/opt`:
   ```bash
   # Пример для ARM платформы
   cd /opt
   # Распакуйте архив с тулчейном
   tar -xzf toolchain-arm-linux-gnueabihf.tar.gz
   ```

2. Убедитесь, что тулчейн доступен в PATH или укажите путь к нему в скрипте сборки.

## Сборка протокола

### Выполнение сборки

1. Перейдите в директорию с исходными файлами:
   ```bash
   cd /home/user/src/API/platform/linux/api/
   ```

2. Выполните скрипт сборки с указанием целевой платформы:
   ```bash
   # Для платформы Linux ARMv7 (hard float)
   ./makedrv.sh linux-armv7hf
   
   # Для других платформ используйте соответствующий ключ:
   # ./makedrv.sh linux-x64    # для Linux x64
   # ./makedrv.sh linux-armv6  # для Linux ARMv6
   ```

3. После успешной сборки будет создан файл библиотеки:
   ```
   mplc_protocol_ca02m.so
   ```
   (или другое имя, в зависимости от настроек скрипта сборки)

## Установка на целевую систему

### 1. Копирование библиотеки на целевую систему

Используйте `scp` для копирования скомпилированной библиотеки на целевую систему:

```bash
# Замените следующие параметры на актуальные:
# - user - имя пользователя на целевой системе
# - 192.168.10.107 - IP адрес целевой системы
# - /opt/mplc4/ - путь установки MasterSCADA на целевой системе

scp /home/user/src/API/platform/linux/api/mplc_protocol_ca02m.so \
    root@192.168.10.107:/opt/mplc4/
```

Или используйте альтернативный метод копирования (USB, сетевой диск и т.д.)

### 2. Перезапуск службы MasterSCADA

После копирования библиотеки перезапустите службу MasterSCADA на целевой системе:

```bash
# На целевой системе выполните:
sudo systemctl restart mplc4
```

Или через SSH:
```bash
ssh root@192.168.10.107 "systemctl restart mplc4"
```

### 3. Проверка работы

1. Проверьте статус службы:
   ```bash
   ssh root@192.168.10.107 "systemctl status mplc4"
   ```

2. Проверьте логи на наличие ошибок:
   ```bash
   ssh root@192.168.10.107 "journalctl -u mplc4 -n 50"
   ```

3. Убедитесь, что протокол загружен корректно и доступен в конфигурации MasterSCADA.

## Структура файлов проекта

```
API/platform/linux/api/
├── makedrv.sh                    # Скрипт сборки
├── modbus_rtu_protocol.h         # Заголовочный файл основного протокола
├── modbus_rtu_protocol.cpp       # Реализация основного протокола
├── fast_modbus_protocol.h        # Заголовочный файл обертки протокола
├── fast_modbus_protocol.cpp      # Реализация обертки протокола
├── mplc_cyntron.h                # Заголовочный файл инициализации
├── mplc_cyntron.cpp              # Реализация инициализации
├── dllmain.cpp                   # Точка входа DLL
├── mplc_lib_so/                  # Директория с библиотеками
│   ├── masterplc.so
│   ├── mplc_archive.so
│   ├── mplcshare.so
│   ├── opcua.so
│   ├── liblua.so
│   └── mplc_events.so
└── ... (другие файлы API)
```

## Требования к системе сборки

- **ОС**: Linux (Ubuntu, Debian или совместимые)
- **Компилятор**: g++-arm-linux-gnueabihf (для ARM платформы)
- **Инструменты**: build-essential
- **Права доступа**: права на выполнение скрипта makedrv.sh

## Требования к целевой системе

- **ОС**: Linux с поддержкой целевой архитектуры (ARMv7, x64 и т.д.)
- **MasterSCADA 4D**: установленная и настроенная система
- **Права доступа**: root или sudo для копирования файлов и перезапуска службы

## Устранение проблем

### Ошибка компиляции: "command not found: arm-linux-gnueabihf-g++"

**Решение**: Убедитесь, что установлен кросс-компилятор:
```bash
sudo apt-get install g++-arm-linux-gnueabihf
```

### Ошибка: "cannot find -lmplcshare"

**Решение**: Убедитесь, что все необходимые `.so` файлы скопированы в директорию `mplc_lib_so/` и пути к ним указаны правильно в скрипте сборки.

### Ошибка при загрузке библиотеки на целевой системе

**Решение**: 
1. Проверьте архитектуру целевой системы:
   ```bash
   uname -m
   ```
2. Убедитесь, что библиотека скомпилирована для правильной архитектуры
3. Проверьте права доступа к файлу:
   ```bash
   ls -l /opt/mplc4/mplc_protocol_ca02m.so
   ```

### Протокол не появляется в списке доступных

**Решение**:
1. Проверьте логи MasterSCADA на наличие ошибок загрузки
2. Убедитесь, что имя протокола в коде соответствует ожидаемому
3. Проверьте регистрацию протокола через макрос `MPLC_PROTOCOL_TYPE`

## Дополнительные команды

### Очистка результатов сборки

```bash
cd /home/user/src/API/platform/linux/api/
make clean
# или удалите файлы вручную:
rm -f *.o *.so
```

### Проверка зависимостей библиотеки

```bash
# На целевой системе
ldd /opt/mplc4/mplc_protocol_ca02m.so
```

### Просмотр информации о библиотеке

```bash
# На целевой системе
file /opt/mplc4/mplc_protocol_ca02m.so
readelf -h /opt/mplc4/mplc_protocol_ca02m.so
```

## Примечания

- Имя выходного файла библиотеки может отличаться в зависимости от настроек скрипта `makedrv.sh`
- Убедитесь, что версия API на системе сборки совместима с версией на целевой системе
- При обновлении MasterSCADA может потребоваться пересборка протокола с новыми версиями API
