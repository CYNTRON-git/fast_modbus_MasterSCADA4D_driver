# Fast Modbus Driver for MasterSCADA 4D

Пользовательский протокол **Fast Modbus** для системы MasterSCADA 4D — событийный опрос устройств по RS-485 с управлением приоритетами событий и защитой от спама частоменяющихся параметров.

> **Целевая платформа:** SA-02m / контроллеры с MasterSCADA 4D на Linux ARMv7hf  
> **Эталон:** внутренняя прошивка слейва и эталонная реализация мастера (приватные репозитории), протокол Wiren Board Fast Modbus WB-extension; сверка — `docs/MODBUS_AND_FAST_MODBUS_COMPLIANCE.md`, `docs/PROTOCOL_FIXES.md`

---

## Содержание

- [Обзор](#обзор)
- [Архитектура](#архитектура)
- [Алгоритм работы Execute()](#алгоритм-работы-execute)
- [Автодетекция устройств](#автодетекция-устройств)
- [Смешанная шина: FMB + обычный Modbus](#смешанная-шина-fmb--обычный-modbus)
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
| Автодетекция | Устройство автоматически определяется как FMB или generic Modbus через 0x18-пробу |
| Смешанная шина | FMB-устройства и обычные Modbus RTU работают на одной RS-485 шине одновременно |
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

---

## Алгоритм работы Execute()

Метод `Execute()` вызывается рантаймом MasterSCADA 4D каждые `TaskPeriod()` мс:

```
1. flush_pending_values()          — сброс отложенных значений (min_interval истёк)
2. collect_writes() + send_write() — OutVar → FC05/FC06, только изменившиеся
3. sync_device_priorities()        — 0x18-проба + конфигурация приоритетов
4. poll_events()                   — цикл 0x10 → 0x11/0x12 до тихой шины
   └── dispatch_event()            — маршрутизация события к каналу → on_event()
   └── on_event() → filter()       — deadband/min_interval/burst → push_value()
5. fallback_poll (по расписанию)   — RTU по FallbackPollPeriodMs (см. логику ниже)
```

> **Важно:** `sync_device_priorities()` (шаг 3) выполняется **до** опроса событий (шаг 4).  
> Это гарантирует, что 0x18-проба завершена до начала чтения событий в том же цикле.

---

## Автодетекция устройств

Протокол **автоматически** определяет, является ли устройство Fast Modbus совместимым, без необходимости вручную задавать тип устройства.

### Механизм детекции (0x18-проба)

Только WB Fast Modbus устройства отвечают на `FC=0x46, subcmd=0x18` корректным ACK:
```
[addr] 46 18 01 00 CRC CRC   (7 байт)
```
Обычные Modbus RTU устройства либо не отвечают (таймаут), либо возвращают ошибку `[addr] C6 xx CRC` (неизвестный функциональный код).

### Алгоритм пробы

```
sync_device_priorities() при каждом Execute(), пока probe не завершена:

  Для каждого канала с prio != DISABLED:
    → отправить 0x18
    ✓ ACK получен  → fmb_capable=true, fmb_probe_done=true (FMB-устройство)
    ✗ Таймаут/ошибка → prio_fail_count++
      если prio_fail_count >= 3 и ни одного успеха:
        → fmb_probe_done=true, fmb_capable=false (generic Modbus)
        → все каналы prio_synced=true (прекратить 0x18 навсегда)
        → вернуться к чистому RTU-поллингу
```

### Состояние устройства (`FastModbusDeviceModule`)

| Поле | Значение | Смысл |
|---|---|---|
| `fmb_capable=false` + `fmb_probe_done=false` | Начальное | Проба не завершена |
| `fmb_capable=true` + `fmb_probe_done=true` | FMB подтверждён | Eventos + RTU для DISABLED |
| `fmb_capable=false` + `fmb_probe_done=true` | Non-FMB | Только RTU fallback |

`AutoScan=true`: устройство, ответившее на 0x03 (scan response), мгновенно получает `fmb_capable=true` без ожидания 0x18-пробы.

**REBOOT-событие:** `fmb_capable` сохраняется (устройство только что прислало событие — оно точно FMB), сбрасываются только `prio_synced` для повторной отправки 0x18 после перезагрузки устройства.

---

## Смешанная шина: FMB + обычный Modbus

На одной RS-485 шине могут одновременно работать устройства разных типов. Протокол обрабатывает каждое устройство независимо.

### Логика выбора режима опроса (шаг 5 Execute)

```
для каждого устройства:

  EnableFastModbus=false?
  └─► fallback_poll() — полный RTU для всех каналов (FMB глобально выключен)

  fmb_probe_done=false или fmb_capable=false?
  └─► fallback_poll() — полный RTU пока неизвестно или подтверждён non-FMB

  fmb_capable=true?
  └─► fallback_poll_disabled_channels() — RTU только для prio=DISABLED каналов
      (остальные каналы получают данные через Fast Modbus события)
```

### Пример: три устройства на одной шине

```
Шина RS-485:
  ├── MR-02m addr=1  (FMB)          ← обнаружен через 0x18-пробу
  │      Temp  [prio=HIGH]   → события 0x11, обновление по изменению
  │      Mode  [prio=LOW]    → события 0x11
  │      UpTime [prio=DISABLED] → RTU FC03 каждые FallbackPollPeriodMs
  │
  ├── MR-02m addr=2  (FMB, AutoScan=true) ← обнаружен через scan 0x03
  │      Всё через события
  │
  └── Danfoss VFD addr=3 (generic RTU) ← 3×таймаут → fmb_capable=false
         Все регистры → RTU FC03 каждые FallbackPollPeriodMs
```

### Временна́я диаграмма цикла (все три устройства)

```
Каждые EventPollIntervalMs (50мс по умолчанию):
  │
  ├─ [0x10 broadcast] → addr=1 отвечает 0x11 (Temp изменилась)
  ├─ [ACK в след. 0x10] → addr=1 переключает флаг, данные подтверждены
  ├─ [0x12] → шина тихая, выход из цикла опроса
  │
Каждые FallbackPollPeriodMs (1000мс по умолчанию):
  ├─ addr=1: FC03 reg=UpTime    (только DISABLED-канал)
  ├─ addr=3: FC03 reg=Freq      (все каналы Danfoss)
  ├─ addr=3: FC03 reg=Current
  └─ addr=3: FC03 reg=Status
```

---

## Структура проекта

```
fast_modbus_MasterSCADA4D_driver/     ← этот репозиторий (драйвер Fast Modbus)
│
├── README.md
├── build.sh                           ← сборка mplc_protocol_fast_modbus.so
├── mplc_protocol_fast_modbus/         ← ИСХОДНИКИ ДРАЙВЕРА (в git)
│   ├── fmb_defs.h                     ← константы протокола
│   ├── fmb_transport.h/.cpp         ← RS-485, CRC-16, t3.5
│   ├── fmb_frames.h/.cpp              ← кадры Fast Modbus и RTU
│   ├── fmb_event_filter.h             ← антиспам
│   ├── fast_modbus_channel.h/.cpp
│   ├── fast_modbus_module.h/.cpp
│   ├── fast_modbus_protocol.h/.cpp
│   ├── mplc_fast_modbus.cpp           ← регистрация в MasterSCADA 4D
│   └── dllmain.cpp                    ← Windows DLL entry (Linux не используется)
│
├── platform/linux/api/
│   ├── Makefile                       ← сборка только fast_modbus
│   ├── mplc_lib_so/                   ← SDK .so с контроллера (не в git)
│   └── mplc_protocol_fast_modbus.so   ← результат сборки
│
├── API/                               ← локальный SDK MasterSCADA 4D (не в git)
│   ├── include/                       ← скопировать с ПК / из установки MS4
│   └── lib/
│
├── docs/                              ← анализ протокола и инструкции
└── .github/workflows/build.yml
```

> **Важно:** репозиторий [PCA9536-driver-for-MasterPLC](https://github.com/CYNTRON-git/PCA9536-driver-for-MasterPLC) — отдельный проект (драйвер GPIO PCA9536). К нему этот репозиторий **не привязан**. SDK `API/` кладётся локально из установки MasterSCADA 4D (см. `docs/BUILD_INSTRUCTIONS.md`).

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

| PRIO | Константа | Вес арбитража | Как читается мастером |
|------|-----------|---------------|-----------------------|
| 0 | `FMB_PRIO_DISABLED` | Без событий | RTU FC0x каждые `FallbackPollPeriodMs` |
| 1 | `FMB_PRIO_LOW`      | 0x06 (позднее) | Fast Modbus 0x11 событие |
| 2 | `FMB_PRIO_HIGH`     | 0x01 (первым)  | Fast Modbus 0x11 событие |

> `prio=DISABLED` означает: устройство **не** будет отправлять события для этого регистра,  
> но мастер **всё равно читает** его значение через стандартный Modbus RTU по расписанию.

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

**Внутренние поля состояния** (не задаются в редакторе, выставляются автоматически):

| Поле | Тип | Описание |
|---|---|---|
| `fmb_capable` | bool | Устройство подтверждено как Fast Modbus (через 0x18 или scan) |
| `fmb_probe_done` | bool | Детекция завершена (не повторять 0x18 бесконечно) |
| `prio_fail_count` | uint8 | Счётчик последовательных ошибок 0x18; сброс при успехе |
| `supports_fast_modbus` | bool | Устройство найдено через AutoScan (scan response 0x03) |

### Свойства канала (Channel)

| Свойство | Описание |
|---|---|
| `RegType` | 0=COIL, 1=DISCRETE, 2=HOLDING, 3=INPUT |
| `RegAddr` | Адрес регистра (hex: 0x0100) |
| `Scale` | Масштаб: `value = raw × scale + offset` |
| `Offset` | Смещение |
| `Prio` | 0=DISABLED (RTU fallback), 1=LOW (событие, низкий приоритет), 2=HIGH (событие, первым) |
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
# 1. Клонировать репозиторий
git clone https://github.com/CYNTRON-git/fast_modbus_MasterSCADA4D_driver.git
cd fast_modbus_MasterSCADA4D_driver

# 2. Положить SDK MasterSCADA 4D в API/ (см. docs/BUILD_INSTRUCTIONS.md)

# 3. Установить кросс-компилятор (Ubuntu)
sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf make

# 4. Скопировать SDK .so с контроллера
scp root@<controller_ip>:/opt/mplc4/{masterplc.so,mplcshare.so,mplc_archive.so,opcua.so,liblua.so,mplc_events.so} \
    platform/linux/api/mplc_lib_so/

# 5. Собрать
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
cd platform/linux/api
CXX=arm-linux-gnueabihf-g++ \
CC=arm-linux-gnueabihf-gcc \
MPLCLIBS=$(pwd)/mplc_lib_so \
DEPs="mplc_lib_so/masterplc.so mplc_lib_so/mplc_archive.so mplc_lib_so/mplcshare.so \
      mplc_lib_so/opcua.so mplc_lib_so/liblua.so mplc_lib_so/mplc_events.so" \
make -j$(nproc) mplc_protocol_fast_modbus
```

**Результат:** `platform/linux/api/mplc_protocol_fast_modbus.so`  
**Проверка:** `file mplc_protocol_fast_modbus.so` → `ELF 32-bit LSB shared object, ARM, EABI5`

---

## Установка на контроллер

```bash
# Скопировать .so на контроллер
scp platform/linux/api/mplc_protocol_fast_modbus.so \
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

**Триггеры:** изменения в `mplc_protocol_fast_modbus/**`, `platform/linux/api/Makefile`, `build.sh`, `.github/workflows/build.yml`

---

## Устранение проблем

### Устройство не отвечает на события

1. Проверить `EventPollIntervalMs` — слишком малое значение нагружает шину
2. Убедиться, что приоритеты синхронизированы: `AutoScan=true` при первом запуске или перезагрузить устройство (REBOOT-событие автоматически переза­пустит 0x18)
3. Проверить `EnableFastModbus=true`

### Значения обновляются редко

- Уменьшить `MinIntervalMs` или `BurstMaxEvents` в настройках канала
- Проверить `Deadband`: если слишком большой — мелкие изменения отфильтровываются

### Переменная с `prio=DISABLED` не обновляется

- Убедиться, что `FallbackPollPeriodMs > 0` (по умолчанию 1000 мс)
- Канал с `prio=DISABLED` читается раз в `FallbackPollPeriodMs`, не через события — это ожидаемое поведение

### Устройство долго не детектируется как FMB

- Проба занимает до `3 × ResponseTimeoutMs` (3 × 200 = 600 мс) перед вынесением вердикта
- Если `AutoScan=true` — детекция мгновенная (по ответу на scan-команду 0x03)
- Во время пробы устройство уже опрашивается через RTU fallback — данные не теряются

### Циклический RTU не работает для generic Modbus устройства

- Убедиться, что `FallbackPollPeriodMs > 0`
- Проверить, завершилась ли проба: устройство получает RTU fallback как только `fmb_probe_done=true`, `fmb_capable=false` (≤ 3 цикла)
- Если `EnableFastModbus=false` — RTU работает сразу для всех устройств без ожидания пробы

### Компилятор не найден

```bash
sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
```

### SDK библиотеки не найдены

```
ERROR: SDK libraries missing in platform/linux/api/mplc_lib_so/
```

Скопировать 6 файлов `.so` с работающего контроллера из `/opt/mplc4/`:
```bash
scp root@<ip>:/opt/mplc4/{masterplc,mplcshare,mplc_archive,opcua,liblua,mplc_events}.so \
    platform/linux/api/mplc_lib_so/
```

### Заголовки SDK не найдены при сборке

Скопировать каталог `API` из установки MasterSCADA 4D в корень проекта (`API/include`, `API/lib`). Подробно — `docs/BUILD_INSTRUCTIONS.md`.

---

## Ссылки

- [MasterSCADA 4D — документация по созданию протоколов](https://support.mps-soft.ru/Help-web/index.html)
- [Wiren Board Fast Modbus — спецификация протокола](https://github.com/wirenboard/wb-modbus-ext-scanner/blob/main/docs/protocol.ru.md)
- [Репозиторий драйвера](https://github.com/CYNTRON-git/fast_modbus_MasterSCADA4D_driver)

---

## Лицензия

Проприетарный — CYNTRON. Все права защищены.
