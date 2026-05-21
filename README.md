# Fast Modbus Driver for MasterSCADA 4D

Пользовательский протокол **Fast Modbus** для системы MasterSCADA 4D — событийный опрос устройств по RS-485 с управлением приоритетами событий и защитой от спама частоменяющихся параметров.

> **Целевая платформа:** SA-02m / контроллеры с MasterSCADA 4D на Linux ARMv7hf  
> **Эталон:** прошивка [MR-02m](https://github.com/CYNTRON-git/MR-02m) (slave) + [MR-02m-flasher](https://github.com/CYNTRON-git/MR-02m-flasher) (master-reference), протокол Wiren Board Fast Modbus WB-extension

---

## Содержание

- [Обзор](#обзор)
- [Архитектура](#архитектура)
- [Структура проекта](#структура-проекта)
- [Протокол Fast Modbus](#протокол-fast-modbus)
- [Конфигурация в MasterSCADA 4D](#конфигурация-в-masterscada-4d)
- [Антиспам и фильтрация событий](#антиспам-и-фильтрация-событий)
- [Сборка](#сборка)
- [Установка на контроллер](#установка-на-контроллер)
- [CI/CD](#cicd)
- [Устранение проблем](#устранение-проблем)

---

## Обзор

Стандартный Modbus RTU работает по циклическому опросу: мастер запрашивает каждый регистр по расписанию. **Fast Modbus** инвертирует логику: устройство само уведомляет мастера о событии через механизм очереди событий (команды `0x10`/`0x11`/`0x12` функционального кода `0x46`).

**Ключевые возможности:**

| Возможность | Описание |
|---|---|
| Событийный опрос | Мастер опрашивает шину командой 0x10; слейв возвращает очередь событий (0x11) |
| Приоритеты событий | Каждому каналу назначается приоритет: `DISABLED` / `LOW` / `HIGH` через команду 0x18 |
| Антиспам | Per-channel фильтрация: deadband, min\_interval\_ms, burst window |
| Fallback RTU | Циклический опрос по стандартному Modbus RTU для устройств без Fast Modbus |
| Автосканирование | Обнаружение устройств по серийному номеру (0x01/0x02/0x04) |
| Запись в регистры | FC05 (coil) / FC06 (holding register) с проверкой IsNeedWrite и обратным масштабированием |
| Cross-platform | Работает на Linux (ARMv7hf/ARMv8/x64) и Windows |

---

## Архитектура

```
MasterSCADA 4D Runtime
        │
        ▼
FastModbusProtocol          ← один экземпляр на COM-порт
   │  owns: FmbTransport    ← RS-485 serial I/O, CRC-16, t3.5 timing
   │
   ├── FastModbusDeviceModule [addr=1, serial=0x...]   ← один слейв
   │      ├── FastModbusRegChannel [HOLDING, reg=0x0100, scale=0.1]
   │      ├── FastModbusRegChannel [INPUT,   reg=0x0101, scale=0.01]
   │      └── FastModbusRegChannel [COIL,    reg=0x0000]
   │
   └── FastModbusDeviceModule [addr=2, ...]
          └── ...
```

### Жизненный цикл каждого цикла `Execute()`

```
1. flush_pending_values()      — сброс отложенных значений (min_interval)
2. collect_writes()            — сбор OutVar → команды FC05/FC06
3. send_write() × N            — запись на шину (inter_frame_delay между ними)
4. poll_events()               — цикл 0x10 → 0x11/0x12 до тихой шины
   └── dispatch_event()        — маршрутизация события к каналу → on_event()
   └── on_event() → filter()   — deadband/min_interval/burst → push_value()
5. sync_device_priorities()    — 0x18 для каналов с prio_synced=false
6. fallback_poll() [опц.]      — RTU-поллинг по FallbackPollPeriodMs
```

---

## Структура проекта

```
fast_modbus_MasterSCADA4D_driver/
│
├── README.md                          ← этот файл
├── build.sh                           ← воспроизводимый скрипт сборки
├── .github/
│   └── workflows/
│       └── build.yml                  ← CI/CD: ARM build + syntax check
│
├── docs/
│   ├── BUILD_INSTRUCTIONS.md          ← инструкция по подготовке окружения
│   ├── FASTMODBUS_ANALYSIS.md         ← анализ протокола
│   ├── FAST_MODBUS_PROTOCOL_ANALYSIS.md
│   ├── MODBUS_AND_FAST_MODBUS_COMPLIANCE.md ← соответствие эталонам
│   ├── MODBUS_RTU_README.md
│   └── PROTOCOL_FIXES.md              ← лог исправлений по эталону MR-02m
│
└── API/                               ← git submodule (SDK MasterSCADA 4D)
    ├── include/                       ← заголовки SDK (не редактировать)
    ├── lib/                           ← библиотеки SDK
    ├── platform/linux/api/
    │   ├── Makefile                   ← сборочный Makefile
    │   ├── mplc_lib_so/               ← SDK .so (скопировать с контроллера)
    │   └── mplc_protocol_fast_modbus.so  ← результат сборки
    └── examples/
        └── mplc_protocol_fast_modbus/ ← ИСХОДНИКИ ДРАЙВЕРА
            ├── fmb_defs.h             ← константы протокола (subcommands, types, prio)
            ├── fmb_transport.h/.cpp   ← RS-485: open/close/send/recv, CRC-16, t3.5
            ├── fmb_frames.h/.cpp      ← builder/parser фреймов (scan, event, RTU)
            ├── fmb_event_filter.h     ← антиспам: deadband, min_interval, burst
            ├── fast_modbus_channel.h/.cpp  ← канал MS4 ↔ регистр Modbus
            ├── fast_modbus_module.h/.cpp   ← устройство (slave) на шине
            ├── fast_modbus_protocol.h/.cpp ← главный класс протокола
            ├── mplc_fast_modbus.cpp   ← регистрация в MS4, объявление свойств
            └── dllmain.cpp            ← точка входа SO
```

---

## Протокол Fast Modbus

### Формат фреймов (WB-extension, FC=0x46)

#### Сканирование устройств

```
Мастер → 0x04  : FD 46 04 CRC CRC                           (5 байт, сброс состояния)
Мастер → 0x01  : FD 46 01 CRC CRC                           (5 байт, начало скана)
Слейв  ← 0x03  : FD 46 03 [SN:4BE] [MB_ADDR] CRC CRC        (10 байт)
Мастер → 0x02  : FD 46 02 CRC CRC                           (5 байт, следующий)
  ... (повтор до таймаута)
Мастер → 0x04  : FD 46 04 CRC CRC                           (завершение цикла)
```

> Арбитраж реализован на стороне слейва (расчёт временного окна по серийному номеру).  
> Мастер не передаёт серийный номер в 0x01/0x02.

#### Событийный опрос

```
Мастер → 0x10 : FD 46 10 [min_slave] [max_data] [ack_slave] [ack_flag] CRC CRC   (9 байт)
Слейв  ← 0x11 : [slave] 46 11 [FLAG] [N] [DATA_LEN] [event×N] CRC CRC           (≥8 байт)
  или
Слейв  ← 0x12 : [slave] 46 12 [FLAG] CRC CRC                                     (6 байт, нет событий)
```

**Формат события (Format B, MR-02m ≥1.0.8.8):**

```
[TYPE:1] [REG_H:1] [REG_L:1] [VALUE:0/1/2 байт BE]
```

| TYPE | Значение | Payload |
|------|----------|---------|
| 0x00 | COIL     | 1 байт (0 или 1) |
| 0x01 | DISCRETE | 1 байт (0 или 1) |
| 0x02 | HOLDING  | 2 байт BE (signed int16) |
| 0x03 | INPUT    | 2 байт BE (signed int16) |
| 0x0F | REBOOT   | 0 байт |

**Подтверждение (ACK):** мастер эхирует `ack_slave`/`ack_flag` из последнего принятого 0x11.  
Слейв переключает флаг `0x55 ↔ 0xAA` только после совпадения ACK.

#### Конфигурация приоритетов (0x18)

```
Запрос  : [addr] 46 18 05 [TYPE] [REG_H] [REG_L] [COUNT:1] [PRIO] CRC CRC   (11 байт)
ACK     : [addr] 46 18 01 00 CRC CRC                                          (7 байт)
```

| PRIO | Константа | Вес арбитража |
|------|-----------|---------------|
| 0 | `FMB_PRIO_DISABLED` | Без событий |
| 1 | `FMB_PRIO_LOW`      | 0x06 (позднее) |
| 2 | `FMB_PRIO_HIGH`     | 0x01 (первым) |

---

## Конфигурация в MasterSCADA 4D

### Свойства протокола (узел FastModbus)

| Свойство | Тип | По умолч. | Описание |
|---|---|---|---|
| `PortName` | STRING | `/dev/ttyUSB0` | Путь к последовательному порту |
| `BaudRate` | INT | 9600 | Скорость (бод) |
| `Parity` | INT | 0 | 0=None, 1=Even, 2=Odd |
| `StopBits` | INT | 1 | Стоп-биты |
| `DataBits` | INT | 8 | Биты данных |
| `ResponseTimeoutMs` | INT | 200 | Таймаут ответа от слейва (мс) |
| `InterFrameDelayMs` | INT | 5 | Доп. задержка между фреймами сверх t3.5 (мс) |
| `EventPollIntervalMs` | INT | 50 | Интервал цикла опроса событий (мс) |
| `FallbackPollPeriodMs` | INT | 1000 | Период циклического RTU-поллинга fallback (мс) |
| `EnableFastModbus` | BOOL | true | false = только стандартный Modbus RTU |
| `AutoScan` | BOOL | false | Сканировать шину при старте для поиска серийных номеров |

### Свойства устройства (IOModule)

| Свойство | Описание |
|---|---|
| `ModbusAddr` | Адрес Modbus RTU (1..247) |
| `SerialNumber` | Серийный номер Fast Modbus (0 = не используется) |
| `UseSerial` | Адресация по серийному номеру вместо MB-адреса |

### Свойства канала (Channel)

| Свойство | Описание |
|---|---|
| `RegType` | 0=COIL, 1=DISCRETE, 2=HOLDING, 3=INPUT |
| `RegAddr` | Адрес регистра (hex: 0x0100) |
| `Scale` | Масштаб: `value = raw × scale + offset` |
| `Offset` | Смещение |
| `Prio` | 0=DISABLED, 1=LOW, 2=HIGH |
| `Deadband` | Минимальное изменение для передачи события |
| `MinIntervalMs` | Минимальный интервал между значениями (мс) |
| `BurstWindowMs` | Длина окна burst-ограничения (мс) |
| `BurstMaxEvents` | Максимум событий в burst-окне |

---

## Антиспам и фильтрация событий

`FmbEventFilter` реализует трёхуровневую защиту на каждый канал:

```
Событие с шины
      │
      ▼
[Deadband] ─── |Δvalue| < deadband? ──► Отбросить
      │
      ▼
[Burst window] ─── burst_count ≥ max? ──► Отложить в pending
      │
      ▼
[Min interval] ─── elapsed < min_interval? ──► Отложить в pending
      │
      ▼
Передать в MS4 + mark_committed()
```

**Флаг `pending`:** отложенное значение сбрасывается в следующем `flush_pending_values()`, как только ограничения ослабнут. Значение не теряется — передаётся с задержкой.

---

## Сборка

### Быстрый старт (WSL / Linux)

```bash
# 1. Клонировать с подмодулями
git clone --recurse-submodules <repo_url>

# 2. Установить кросс-компилятор (Ubuntu)
sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf make

# 3. Скопировать SDK .so с контроллера
scp root@<controller_ip>:/opt/mplc4/{masterplc.so,mplcshare.so,mplc_archive.so,opcua.so,liblua.so,mplc_events.so} \
    API/platform/linux/api/mplc_lib_so/

# 4. Собрать
./build.sh linux-armv7hf
```

### Поддерживаемые платформы

| Ключ | Компилятор | Целевая система |
|---|---|---|
| `linux-armv7hf` | `arm-linux-gnueabihf-g++` | SA-02m, MR-02m, Cortex-A7/A8/A9 HF |
| `linux-armv8` | `aarch64-linux-gnu-g++` | 64-бит ARM контроллеры |
| `linux-x64` | `g++` | Linux x86-64 (ПК, отладка) |

### Ручная сборка (без build.sh)

```bash
cd API/platform/linux/api
CXX=arm-linux-gnueabihf-g++ \
CC=arm-linux-gnueabihf-gcc \
MPLCLIBS=$(pwd)/mplc_lib_so \
DEPs="mplc_lib_so/masterplc.so mplc_lib_so/mplc_archive.so mplc_lib_so/mplcshare.so \
      mplc_lib_so/opcua.so mplc_lib_so/liblua.so mplc_lib_so/mplc_events.so" \
make -j$(nproc) mplc_protocol_fast_modbus
```

**Результат:** `API/platform/linux/api/mplc_protocol_fast_modbus.so`  
**Проверка:** `file mplc_protocol_fast_modbus.so` → `ELF 32-bit LSB shared object, ARM, EABI5`

---

## Установка на контроллер

```bash
# Скопировать .so на контроллер
scp API/platform/linux/api/mplc_protocol_fast_modbus.so \
    root@<controller_ip>:/opt/mplc4/

# Перезапустить службу
ssh root@<controller_ip> "systemctl restart mplc4"

# Проверить загрузку
ssh root@<controller_ip> "journalctl -u mplc4 -n 30 | grep -i fast"
```

После перезапуска в редакторе MasterSCADA 4D протокол `FastModbus` появится в списке доступных протоколов.

---

## CI/CD

`.github/workflows/build.yml` содержит два job-а:

### `build-armv7hf`
- Ubuntu runner с `arm-linux-gnueabihf-g++`
- Полная сборка с линковкой против SDK `.so`
- Артефакт `mplc_protocol_fast_modbus-armv7hf` хранится 30 дней
- SDK библиотеки кешируются через `actions/cache`

### `syntax-check-x64`
- Компиляция всех `.cpp` без линковки (не требует SDK `.so`)
- Ловит ошибки компиляции в любом коммите без доступа к закрытому SDK
- Запускается при любом push/PR

**Триггеры:** изменения в `API/examples/mplc_protocol_fast_modbus/**`, `API/include/**`, `build.sh`, `.github/workflows/build.yml`

---

## Устранение проблем

### Устройство не отвечает на события

1. Проверить `EventPollIntervalMs` — слишком малое значение нагружает шину
2. Убедиться, что приоритеты синхронизированы: `AutoScan=true` при первом запуске или перезагрузить устройство (REBOOT-событие автоматически переза­пустит 0x18)
3. Проверить `EnableFastModbus=true`

### Значения обновляются редко

- Уменьшить `MinIntervalMs` или `BurstMaxEvents` в настройках канала
- Проверить `Deadband`: если слишком большой — мелкие изменения отфильтровываются

### Циклический RTU не работает

- Убедиться, что `FallbackPollPeriodMs > 0`
- Для устройств без Fast Modbus установить `EnableFastModbus=false` на уровне устройства (свойство `UseSerial=false` и `SerialNumber=0`)

### Компилятор не найден

```bash
sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
```

### SDK библиотеки не найдены

```
ERROR: SDK libraries missing in API/platform/linux/api/mplc_lib_so/
```

Скопировать 6 файлов `.so` с работающего контроллера из `/opt/mplc4/`:
```bash
scp root@<ip>:/opt/mplc4/{masterplc,mplcshare,mplc_archive,opcua,liblua,mplc_events}.so \
    API/platform/linux/api/mplc_lib_so/
```

### Ошибка `index file smaller than expected` (git)

```bash
rm -f API/.git/index && cd API && git reset HEAD
```

---

## Ссылки

- [MasterSCADA 4D — документация по созданию протоколов](https://support.mps-soft.ru/Help-web/index.html)
- [Wiren Board Fast Modbus — спецификация протокола](https://github.com/wirenboard/wb-modbus-ext-scanner/blob/main/docs/protocol.ru.md)
- [MR-02m firmware (slave reference)](https://github.com/CYNTRON-git/MR-02m)
- [MR-02m-flasher (master reference)](https://github.com/CYNTRON-git/MR-02m-flasher)
- [PCA9536 driver example (API submodule origin)](https://github.com/CYNTRON-git/PCA9536-driver-for-MasterPLC)

---

## Лицензия

Проприетарный — CYNTRON. Все права защищены.
