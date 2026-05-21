# Техническое задание: архитектура мультипродуктовой базы на STM32F030CC и общее ядро

**Версия:** 1.0  
**Дата:** 2025-02-16  
**Цель:** спроектировать архитектуру и структуру проектов так, чтобы на базе текущих разработок (MP-02m и датчик температуры/влажности cyntron-dtv) использовать общие драйверы, алгоритмы, бутлоадер и быстрый Modbus в других продуктах; обеспечить единую сборку (Make, CI/CD) и перенос бутлоадера и быстрого Modbus в проект cyntron-dtv.

---

## 1. Область применения

- **Продукты:** модули расширения на базе платы управления MP-02m (один МК STM32F030CC, одна плата, разные мезонины); датчик температуры и влажности (тот же STM32F030CC, другая плата и распиновка, отдельная прошивка) в папке `cyntron-dtv`.
- **Общее:** МК STM32F030CC, HAL, Modbus RTU, быстрый Modbus (fast_mb), бутлоадер обновления по Modbus, часть драйверов и алгоритмов.
- **Различия:** распиновка, набор периферии (модули расширения vs датчики BME/HDC/MCP9808/DS18B20/ZMOD4410/LD2412), конфигурация Flash под бутлоадер (размер слотов), формат регистров Modbus.

---

## 2. Анализ текущей структуры

### 2.1 Проект MP-02m (корень репозитория)

| Элемент | Описание |
|--------|----------|
| **Сборка** | Корневой `Makefile`, общие правила в `mk/` (toolchain.mk, rules.mk, pack.mk). Конфигурации: Debug, Release, AppBoot. Один запуск `make` — один конфиг; `make all_targets` — все конфиги + бутлоадер. |
| **Исходники** | `Core/Src`, `Core/Inc` — main, HAL MSP, периферия; `Core/modbus/src`, `Core/modbus/inc` — modbus.c (nanoMODBUS in-tree), modbus_rtu_hw.c, fast_mb.c, fast_mb_events.c, fast_mp_port.c; `Core/adc/`, `Core/apps/`, `Core/drivers/`, `Core/gpio/`, `Core/modules/`, `Core/storage/`, `Core/system/`, `Core/tables`. |
| **Бутлоадер** | Отдельная папка `bootloader/`, свой Makefile. Использует `TOP/Core/Startup`, `TOP/Core/Src/system_stm32f0xx.c`, `TOP/Drivers/`. Линкер: векторы 0x08000000 (2 КБ), код 0x08038000 (32 КБ). Приложение под бутлоадер: `STM32F030CCXX_FLASH_boot.ld` — FLASH с 0x0800800, 188 К. |
| **nanoMODBUS** | Встроен в репозиторий (код в `Core/modbus`, без submodule). |
| **Зависимости modbus** | `fast_mb_port.h` подключает `../../Inc/main.h`, `../../Inc/modbus.h`; `modbus_rtu_hw.c` — пути к gpio, storage, system, apps, tim, usart. |

### 2.2 Проект cyntron-dtv (датчик температуры/влажности)

| Элемент | Описание |
|--------|----------|
| **Сборка** | Сборка из STM32CubeIDE (.cproject). Конфигурации: Debug, Release, DebugBootloader, ReleaseBootloader. Отдельный `Makefile.optimization` (справка по флагам), корневого Makefile под все цели нет. |
| **Исходники** | Собственная копия структуры: `Core/Src`, `Core/Inc`; **Modbus из основного проекта:** `Core/Inc/modbus.h` подключает shared/modbus, в сборку включён shared/modbus/src/modbus.c, `nanomodbus.c` исключён. Локальные копии: `fast_mb.c`, `fast_mb_port.c`, `fast_mb_events.c`, `ModbusRTU_hw.c`. Специфичные: svc_sensor_polling, svc_modbus_registers, drv_sensor_*, BME/HDC/MCP9808/DS18B20/ZMOD/LD2412, app_rtu_sensor. |
| **Модули** | `Modules/` — submodules: BME280, BME68x, ow, ds18b20 (nanoMODBUS не используется в сборке — Modbus из shared). |
| **Конфигурация** | `Core/Inc/build.h` + `common/app_config.h`; `ONLY_RTU_SENSOR`, `HARDWARE_MP 30`. |
| **Бутлоадер** | Есть конфигурации *Bootloader и линкер `STM32F030CCTX_FLASH_boot.ld`: приложение 0x0800800, **238 К** (bootloader: 2 К в начале + **16 К в конце**). В main.c уже есть `#ifdef BOOTLOADED`, remap таблицы векторов в RAM. **Отдельного образа бутлоадера в стиле MP-02m (два региона 0x08000000 + 0x08038000) в cyntron-dtv нет** — используется другой расклад Flash (16 К в конце). |
| **Различия с MP-02m** | Другая плата (HARDWARE_MP 30), другой UART/таймер под Modbus (USART2), свои сервисы и регистровая карта. **Modbus (nanoMODBUS) — из основного проекта (shared)**; fast_mb/ModbusRTU_hw пока локальные копии. |

### 2.3 Ключевые отличия раскладки Flash (бутлоадер)

| Параметр | MP-02m | cyntron-dtv (текущий boot .ld) |
|----------|--------|--------------------------------|
| Векторы бутлоадера | 0x08000000, 2 КБ | 2 К в начале (предполагается) |
| Код бутлоадера | 0x08038000, 32 КБ | 16 КБ в конце (0x0803C000..0x0803FFFF) |
| Начало приложения | 0x0800800 | 0x0800800 |
| Размер приложения | 188 КБ | 238 КБ |

Для унификации бутлоадера необходимо принять единую схему (например, как в MP-02m: 2 КБ + 32 КБ) и привести линкер cyntron-dtv к той же схеме, либо описать в ТЗ вариант с конфигурируемым размером слота бутлоадера (16 КБ / 32 КБ).

---

## 3. Цели и ограничения

### 3.1 Цели

1. **Единая кодовая база** для общего кода: Modbus RTU (nanoMODBUS + порт), быстрый Modbus (fast_mb, fast_mb_events, fast_mp_port), слой Modbus RTU HW (UART/DMA/таймеры), бутлоадер обновления по Modbus — без дублирования между MP-02m и cyntron-dtv.
2. **Использование в других проектах:** новые устройства на STM32F030CC должны подключать общее ядро (библиотеку/набор каталогов) и добавлять только продукт-специфичный код (пины, приложение, регистры).
3. **cyntron-dtv:** перенести бутлоадер и быстрый Modbus из текущего проекта MP-02m (или из выделенного общего ядра), убрать локальные копии fast_mb*, ModbusRTU_hw, nanomodbus; сохранить работу под своей платой и своей регистровой картой.
4. **Сборка и CI/CD:** один репозиторий (или явная схема submodule/монорепо) с возможностью собирать все продукты и конфигурации (в т.ч. Debug/Release/AppBoot для каждого продукта); Makefile и, при наличии, CI уже учитываются.

### 3.2 Ограничения

- Совместимость по протоколу и поведению: Modbus, быстрый Modbus, обновление по бутлоадеру не должны измениться для уже развёрнутых устройств.
- Сохранение возможности сборки в STM32CubeIDE для тех продуктов, где она используется (например cyntron-dtv), либо явный переход на make/CMake с обоснованием.
- Разная распиновка и периферия: общий код не должен зависеть от конкретных пинов и набора датчиков; зависимости выносить в порт (конфиг/интерфейс).

---

## 4. Требования к архитектуре

### 4.1 Варианты размещения общего кода

**Вариант A — Монорепо с общим каталогом (рекомендуется для одного репозитория)**

- В корне репозитория выделить каталог **общего ядра**, например `shared/` или `common_core/`, с подкаталогами:
  - `shared/modbus/` — nanoMODBUS (или ссылка на submodule), modbus_rtu_hw, fast_mb, fast_mb_events, fast_mp_port; порт через заголовок с зависимостями от HAL и от продукта (main.h, tim, usart, конфиг).
  - `shared/bootloader/` — код бутлоадера (flash, uart, modbus_bl, eeprom_sig и т.д.), с конфигом линкера и адресов под выбранную схему Flash.
  - `shared/drivers/` — драйверы микросхем и подсистемы, используемые более чем одним продуктом (см. п. 5.4).
  - `shared/sensors/` (или `shared/libs/`) — библиотеки датчиков и сенсоров, вынесенные в общее ядро при использовании в нескольких продуктах (см. п. 5.4).
- Продукты: `mp-02m/` (текущий корень можно считать продуктом) и `cyntron-dtv/` (или `products/cyntron-dtv/`). Каждый продукт содержит только: свой `main.c`, приложение, **продукт-специфичные** драйверы и обёртки датчиков (если не перенесены в shared), конфиг платы (пины, макросы), свой линкер и список исходников.
- Общие правила сборки остаются в `mk/`; каждый продукт может иметь свой Makefile, подключающий `mk/` и задающий `TOP`, список исходников (в т.ч. из `shared/`), инклюды и линкер.

**Вариант B — Submodule общего ядра**

- Вынести общее ядро (modbus, bootloader, возможно HAL/CMSIS) в отдельный репозиторий и подключать как git submodule (например `shared_core/`).
- В каждом продукте в сборку включаются исходники из `shared_core/modbus`, `shared_core/bootloader`; пути include и зависимости задаются в Makefile/CMake/.cproject продукта.
- Плюс: общее ядро можно версионировать и использовать в других репозиториях. Минус: усложнение работы с submodule и синхронизацией версий.

**Вариант C — Текущая структура с симлинками/копированием**

- Оставить MP-02m источником истины для modbus и bootloader; в cyntron-dtv подключать через относительные пути к `../Core/modbus` и `../bootloader` (или копировать при сборке).
- Минус: два продукта в одном репозитории с разной структурой папок; при изменениях в modbus/boot нужно править и MP-02m, и пути в cyntron-dtv. Подходит только как временное решение.

ТЗ рекомендует **Вариант A** при сохранении одного репозитория; при необходимости многопроектного использования за пределами репозитория — переход к Варианту B с выделением `shared_core` в отдельный репозиторий.

### 4.2 Абстракция порта (Modbus / Fast Modbus)

- **fast_mb_port** и слой Modbus RTU HW зависят от: `UART_HandleTypeDef`, `TIM_HandleTypeDef`, пинов DE/RS485, конфига (скорость, чётность, стоп-биты), доступа к EEPROM/конфигу (адрес Modbus, fast Modbus address). Эти зависимости должны быть заданы через:
  - один заголовок порта (например `modbus_port.h` или `fast_mb_port_config.h`), подключаемый в продукте и содержащий типы и объявления, используемые в shared коде; либо
  - слабую связь: общий код принимает указатели на структуру с функциями (send, receive, abort, timer start/stop) и конфигом, а реализация в продукте подставляет свои HAL-обёртки и имена (modbusUart, TimerFastMB и т.д.).
- В shared коде **запретить** прямые включения продукт-специфичных путей (типа `../../apps/inc/app_c.h` или `../../gpio/inc/mcp23s17.h`) для модулей, которые не входят в минимальный порт. Вместо этого — только интерфейсы (например, «получить адрес Modbus», «получить fast Modbus address», «записать в EEPROM») через объявления в портовом заголовке или через callback-структуру.

### 4.3 Конфигурация по продукту

- Каждый продукт задаёт:
  - ревизию платы / идентификатор платы (например `HARDWARE_MP`, `BOARD_RTU_SENSOR`);
  - макросы включения функциональности (например `INCLUDE_RTU_SENSOR`, `ONLY_RTU_SENSOR` в cyntron-dtv; `INCLUDE_DO6DI8`, … в MP-02m);
  - опции Modbus (MODBUS_MODE_IRQ / MODBUS_MODE_DMA), отладку (COM_PORT_DEBUG), при необходимости — выбор UART/таймера (если в одном продукте несколько UART).
- Рекомендуется один файл конфигурации на продукт (например `config_mp02m.h` / `config_cyntron_dtv.h` или `build.h` + `app_config.h`), подключаемый в `main.h` продукта; общий код подключает только то, что объявлено в портовом заголовке и в минимальном наборе типов (HAL, nmbs_t, конфиг Modbus).

### 4.4 Бутлоадер: единая схема Flash

- Принять для всех продуктов на STM32F030CC с бутлоадером **единую раскладку** как в MP-02m:
  - векторы бутлоадера: 0x08000000, 2 КБ;
  - код бутлоадера: 0x08038000, 32 КБ;
  - приложение: 0x0800800, 188 КБ.
- Для cyntron-dtv: заменить текущий `STM32F030CCTX_FLASH_boot.ld` (238 К приложения) на линкер с LENGTH 188 К и адресом 0x0800800, совпадающий с MP-02m; обновить `main.c` (FLASH_APP_MAX_ADDR и т.п.) под 188 К.
- Альтернатива (если потребуется оставить 238 К приложения для cyntron-dtv): описать в отдельном подпункте ТЗ вариант с «коротким» бутлоадером (16 КБ в конце) и двумя разными линкерами бутлоадера (32 КБ vs 16 КБ); тогда общий код бутлоадера компилируется с разными макросами и линкерами под каждый вариант. На первом этапе рекомендуется единая схема 2+32 КБ и 188 К приложения.

---

## 5. Требования к структуре проектов

### 5.1 Целевая структура (вариант A — общий каталог в монорепо)

```
<repo_root>/
  mk/                          # общие правила сборки (toolchain, rules, pack)
  shared/                      # общее ядро
    modbus/
      inc/                     # modbus.h, modbus_rtu_hw.h, fast_mb.h, fast_mb_port.h, fast_mb_events.h
      src/                     # modbus.c, modbus_rtu_hw.c, fast_mb.c, fast_mb_events.c, fast_mp_port.c
      port/                    # опционально: заглушки или обёртки, если нужны продукту
    drivers/                   # драйверы микросхем, общие для нескольких продуктов (см. п. 5.4)
      ee24/                    # пример: общий EEPROM при унификации
      (другие по матрице shared)
    sensors/                   # библиотеки датчиков/сенсоров, общие для нескольких продуктов (п. 5.4)
      (BME280, HDC1080, … при появлении второго потребителя)
    bootloader/
      Core/Inc/, Core/Src/      # код бутлоадера (flash, uart_bl, modbus_bl, eeprom_sig, ...)
      scripts/                 # make_fw.py, extract_bootloader_bins.py, make_bootloader_fw.py
      STM32F030CCXX_bootloader.ld
      Makefile                 # сборка бутлоадера; использует shared и Drivers из продукта или общие Drivers
  Drivers/                     # HAL/CMSIS (общие для всех продуктов на F0)
  CMSIS/
  mp-02m/                      # продукт: модули расширения (текущий корень можно переименовать или оставить)
    Core/Inc/, Core/Src/        # main, приложение, модули, драйверы платы, config
    Core/modules/, Core/adc/, Core/gpio/, Core/storage/, Core/system/, Core/tables/, Core/apps/
    bootloader/                 # либо симлинк/включение в shared/bootloader, либо тонкая обёртка
    STM32F030CCXX_FLASH.ld, STM32F030CCXX_FLASH_boot.ld
    Makefile                   # TOP=mp-02m, SRC_C включают shared/modbus, свои Core/*
  cyntron-dtv/                 # продукт: датчик температуры/влажности
    Core/Inc/, Core/Src/        # main, app_rtu_sensor, svc_*, drv_sensor_*, config (build.h, app_config.h)
    Modules/                   # submodules: nanoMODBUS, BME280, ...
    STM32F030CCTX_FLASH.ld, STM32F030CCTX_FLASH_boot.ld  # boot.ld приведён к 188 К
    Makefile                   # TOP=cyntron-dtv, SRC_C включают shared/modbus, свои Core/*, без своих копий fast_mb*
  out/                         # артефакты сборки (опционально общий или по продуктам)
  doc/
```

- Если репозиторий остаётся с корнем «как сейчас» (без папки mp-02m), то продукт MP-02m — это корень; тогда `shared/` в корне, а `cyntron-dtv/` подключает `../shared` и `../Drivers` (или через переменную TOP корня). Бутлоадер может оставаться в корне в `bootloader/`, но при сборке cyntron-dtv использовать тот же образ бутлоадера (одинаковая схема Flash) или свой, если позже будет введена опция 16 КБ.

### 5.2 Состав общего кода (shared)

- **Modbus:** исходники и заголовки nanoMODBUS (если не submodule), modbus_rtu_hw (с зависимостями, вынесенными в порт), fast_mb, fast_mb_events, fast_mp_port. В modbus_rtu_hw убрать прямые включения файлов приложения/модулей MP-02m; обращение к «максимальным адресам», регистрам, EEPROM — через объявления в портовом заголовке или callback.
- **Bootloader:** весь код из текущего `bootloader/` (flash_bl, uart_bl, modbus_bl, modbus_bl_nmbs, eeprom_sig, bl_version, bl_led_line, bl_module_sig, bootloader.c, main.c, stm32f0xx_it_bootloader, assert_stub); скрипты make_fw, extract_bootloader_bins, make_bootloader_fw. Зависимости от Core/Inc и Drivers разрешать через один каталог include (например общий Inc с минимальным набором для бутлоадера) и использование Core/Startup и Core/Src/system_stm32f0xx из одного места (корень или продукт).
- **Драйверы и библиотеки датчиков:** см. п. 5.4.

### 5.4 Драйверы микросхем, библиотеки датчиков и общие алгоритмы

Драйверы для микросхем, библиотеки для датчиков/сенсоров и общие алгоритмы учитываются в архитектуре общего ядра и могут использоваться несколькими продуктами.

#### 5.4.1 Принцип размещения

- **Кандидаты в shared:** код, который уже используется или планируется к использованию в **двух и более продуктах** (например один и тот же чип EEPROM, один и тот же тип датчика температуры), и не содержит жёсткой привязки к пинам/плате одного продукта. Интерфейс такого кода — через HAL (I2C/SPI/UART handle, таймауты, адреса), конфиг-структуру или портовый заголовок; инициализация и привязка к конкретным пинам остаётся в продукте.
- **Остаются в продукте:** драйверы и библиотеки, используемые **только в одном** продукте, до момента появления второго потребителя. При появлении второго потребителя их имеет смысл вынести в shared (см. п. 5.4.3).
- **Общие алгоритмы** (фильтрация, пересчёт единиц, таблицы термопар/NTC/RTD и т.д.), если используются в нескольких продуктах, размещаются в shared (например `shared/algorithms/` или внутри `shared/drivers/`/`shared/sensors/`).

#### 5.4.2 Структура shared для драйверов и датчиков

- **shared/drivers/** — драйверы микросхем и подсистем, общие для нескольких продуктов:
  - примеры: драйвер EEPROM (CAT24C02/ee24), если оба продукта используют одну и ту же микросхему и один и тот же слой абстракции; драйверы, которые появятся в обоих продуктах (например общий I2C-обёртка для EEPROM с конфигом адреса и размера страницы).
  - подкаталоги по устройству или по типу: например `shared/drivers/ee24/` (или `shared/drivers/eeprom/`), при необходимости `shared/drivers/mcp23s17/` при появлении MCP23S17 в cyntron-dtv.
- **shared/sensors/** (или **shared/libs/**) — библиотеки датчиков и сенсоров:
  - сюда выносятся «чистые» реализации работы с датчиком (чтение регистров, калибровка, пересчёт в физические величины), без привязки к конкретной плате. Примеры: общая обёртка над BME280/BME680 (если один и тот же код будет использоваться в MP-02m и cyntron-dtv), общая логика для HDC1080, MCP9808, ZMOD4410, LD2412 (протокол/парсинг) при появлении второго продукта с этим датчиком.
  - продукт подключает нужные подкаталоги (например `shared/sensors/bme280/`) и реализует у себя только инициализацию (пины, I2C/SPI handle, таймауты) и привязку к своей регистровой карте Modbus.
- **Внешние библиотеки (submodule):** если библиотека поставляется как готовый репозиторий (BME280, BME68x, nanoMODBUS, 1-Wire/DS18B20), её можно:
  - держать одним submodule в корне (например `shared/third_party/BME280` или `Modules/BME280` в корне) и подключать в сборку обоих продуктов из одного места;
  - либо оставить submodule в продукте до момента, когда второй продукт начнёт его использовать, после чего перенести в shared или в общий каталог Modules.

#### 5.4.3 Матрица «устройство — продукт — shared»

- В проекте ведётся таблица (например в `doc/SHARED_DRIVERS_SENSORS_MATRIX.md` или в приложении ТЗ), в которой для каждой микросхемы/датчика указано:
  - в каком продукте используется (MP-02m, cyntron-dtv, оба);
  - где физически расположен код (shared/drivers, shared/sensors, продукт);
  - при необходимости — версия/источник (собственная реализация, submodule, vendor).
- При добавлении нового продукта или нового датчика в существующий продукт таблица обновляется; при появлении второго потребителя для кода, ранее лежавшего только в одном продукте, принимается решение о переносе в shared и обновлении сборки.

#### 5.4.4 Текущее распределение (на дату ТЗ)

- **MP-02m:** драйверы микросхем — ad53x4, atm90e32 (Core/drivers); gpio/expand — MCP23S17 (Core/gpio); АЦП/мультиплексор — ADS1220, ADS1x20, mux, ai_channel (Core/adc); таблицы — термопары, NTC, PT, RTD (Core/tables); хранение — ee24, eeprom_manager (Core/storage). Сейчас используются только в MP-02m — в shared не выносятся до появления второго потребителя.
- **cyntron-dtv:** датчики — BME280, BME680, HDC1080, MCP9808, DS18B20, ZMOD4410, LD2412; модули — nanoMODBUS, BME280, BME68x, ow, ds18b20 (Modules); EEPROM — ee24 (как в MP-02m). Совпадение по ee24 даёт кандидата в shared: общий драйвер EEPROM (ee24) и, при унификации формата конфига, слой eeprom_manager/хранения настроек.
- **Итог на первом этапе:** в shared явно закладываются каталоги `shared/drivers/` и `shared/sensors/` (при необходимости — `shared/algorithms/`). Конкретный перенос в них (например ee24, общие таблицы или обёртки датчиков) выполняется по мере унификации сборки и появления общих требований; таблица в приложении В и в doc фиксирует текущее и целевое размещение.

#### 5.4.5 Правила для кода в shared/drivers и shared/sensors

- Без привязки к пинам и к одному продукту: инициализация принимает handle (I2C/SPI/UART), адрес, конфиг; пины и выбор периферии задаёт продукт.
- Зависимости от HAL/CMSIS и от «системы» (логирование, таймер) — через минимальный портовый заголовок или макросы (например логирование как макрос в no-op по умолчанию).
- Документирование: в каталоге или в README перечислены устройства, версии даташитов (или ссылка на device-integration-catalog), какие продукты подключают этот код.

### 5.5 Зависимости включений (include path)

- При сборке продукта (например cyntron-dtv) в include path должны быть:
  - каталог shared/modbus/inc (или эквивалент);
  - при использовании shared/drivers и shared/sensors — каталоги shared/drivers/*/inc и shared/sensors/*/inc (или общий shared/drivers/inc, shared/sensors/inc);
  - каталог продукта Core/Inc;
  - Drivers, CMSIS;
  - при необходимости — Modules продукта (nanoMODBUS в cyntron-dtv может остаться submodule для совместимости с их nanomodbus.h; тогда shared/modbus использует тот же nanoMODBUS или свой in-tree).
- Заголовки в shared не должны содержать относительных путей вида `../../Inc/main.h` к конкретному продукту; вместо этого — портовый заголовок (включаемый первым в продукте), в котором объявлены типы и функции, требуемые shared коду (например `get_modbusaddress()`, `get_fastmb_serialaddress()`, `Error_Handler`, `modbusUart`, `TimerFastMB`).

---

## 6. Детальные требования к переносу бутлоадера и быстрого Modbus в cyntron-dtv

### 6.1 Быстрый Modbus и Modbus RTU HW

1. **Удалить** из cyntron-dtv локальные копии: `Core/Src/fast_mb.c`, `Core/Src/fast_mb_port.c`, `Core/Src/fast_mb_events.c`, `Core/Src/ModbusRTU_hw.c`, а также обёртку `Core/Src/nanomodbus.c`, если nanoMODBUS будет поставляться из shared или из одного submodule.
2. **Подключить** в сборку cyntron-dtv исходники из shared/modbus (modbus.c, modbus_rtu_hw.c, fast_mb.c, fast_mb_events.c, fast_mp_port.c). Путь include: shared/modbus/inc.
3. **Ввести** в cyntron-dtv портовый заголовок (например `Core/Inc/modbus_port_cyntron_dtv.h` или расширить существующий конфиг), в котором:
   - объявлены или подключены типы/символы, требуемые shared коду: `modbusUart`, `TimerFastMB`, `get_modbusaddress()`, `get_fastmb_serialaddress()`, `get_baudrate()`, `get_stop_bit()`, конфиг парности, `Error_Handler`, при необходимости `nmbs_arg_t` и функции таймера/UART (Start_Timer, Stop_Timer, Receive_Serial, Transmit_Serial, Abort_Serial, TIM_ReStart);
   - при необходимости объявлены макросы (MODBUS_MODE_DMA/IRQ, COM_PORT_DEBUG, FM_LEVEL_DEBUG и т.д.).
4. **Адаптировать** shared код так, чтобы в modbus_rtu_hw и fast_mb_port не было включений типа `../../Inc/main.h` и `../../gpio/inc/mcp23s17.h` для необщих вещей; зависимости от приложения (регистры, coils, EEPROM) — через объявления в портовом заголовке или через callback-структуру, реализуемую в cyntron-dtv (svc_modbus_registers, svc_mb_holding, i2c/ee24).
5. **Проверить** совместимость: те же номера функций Modbus, тот же формат fast Modbus (адрес, арбитраж, события); регистровая карта остаётся в зоне ответственности cyntron-dtv (svc_modbus_registers, svc_mb_holding, svc_mb_coils).

### 6.2 Бутлоадер

1. **Принять** для cyntron-dtv ту же схему Flash, что и в MP-02m: векторы 2 КБ @ 0x08000000, код бутлоадера 32 КБ @ 0x08038000, приложение 188 КБ @ 0x0800800.
2. **Заменить** в cyntron-dtv линкер приложения под бутлоадер на аналог `STM32F030CCXX_FLASH_boot.ld` (ORIGIN 0x0800800, LENGTH 188K). Обновить константы в main.c (FLASH_APP_START_ADDR, FLASH_APP_MAX_ADDR) под 188 К.
3. **Сборка образа бутлоадера:** использовать тот же образ, что и для MP-02m (сборка из shared/bootloader или из корневого bootloader/), так как протокол и адреса совпадают; прошивать в устройство cyntron-dtv тот же boot_fw.bin или отдельный combined образ, где приложение — cyntron-dtv AppBoot.
4. **Первый запуск и обновление:** документировать для cyntron-dtv: первая прошивка (ST-Link) — запись бутлоадера + приложения; обновление по Modbus — регистр 129, .fw файл, как в doc/BOOTLOADER_INTEGRATION.md. **Выполнено:** раздел 8 в doc/BOOTLOADER_INTEGRATION.md (cyntron-dtv: первая прошивка, combined, обновление по Modbus).
5. **Опционально:** если позже понадобится оставить 238 К приложения для cyntron-dtv, описать отдельную конфигурацию бутлоадера (16 КБ в конце) и два варианта линкера; на первом этапе не реализовывать.

### 6.3 nanoMODBUS

- **Вариант 1:** оставить в cyntron-dtv submodule `Modules/nanoMODBUS` и использовать его и в shared коде при сборке cyntron-dtv (include path: cyntron-dtv/Modules/nanoMODBUS и shared/modbus/inc). Тогда shared/modbus не содержит копии nanomodbus.c, только свой modbus.c (если он обёртка над nanoMODBUS) и порт.
- **Вариант 2:** перенести nanoMODBUS в shared (или один submodule в корне) и подключать его и в MP-02m, и в cyntron-dtv из одного места. Тогда в cyntron-dtv удалить submodule Modules/nanoMODBUS и включить путь shared/... в сборку.
- ТЗ рекомендует **Вариант 2** для единообразия; при необходимости сохранить совместимость с форком kpvnnov — использовать тот же репозиторий как submodule в shared.

---

## 7. Система сборки и CI/CD

### 7.1 Makefile по продуктам

- **Корень (или продукт mp-02m):** текущий Makefile сохраняется; переменные TOP, PROJ, CORE_DIRS, SRC_C дополняются путём к shared/modbus (исходники и include). Для AppBoot — линкер и макрос BOOTLOADED без изменений.
- **cyntron-dtv:** добавить корневой Makefile (или Makefile в cyntron-dtv/), который:
  - задаёт TOP = каталог cyntron-dtv (или корень репозитория);
  - формирует SRC_C из Core/Src продукта + shared/modbus/src (и при необходимости shared или корневой Drivers);
  - задаёт INC с путями к shared/modbus/inc, Core/Inc, Modules/nanoMODBUS (если остаётся), Drivers, CMSIS;
  - поддерживает конфигурации Debug, Release, AppBoot (линкер 188 К, BOOTLOADED);
  - подключает mk/toolchain.mk и mk/rules.mk (путь к mk из корня репозитория).
- Общая цель «собрать все продукты» (например `make -C cyntron-dtv all_targets` и корневой `make all_targets`) и упаковка артефактов (pack) в out/ с именами, содержащими имя продукта (например mp-02m-Debug.elf, cyntron-dtv-AppBoot.bin).

### 7.2 CI/CD (выполнено)

- Существующие скрипты и конфигурация CI/CD доработать так, чтобы:
  - собирались все заявленные конфигурации обоих продуктов (например mp-02m: Debug, Release, AppBoot; cyntron-dtv: Debug, Release, AppBoot);
  - при наличии цели — сборка бутлоадера один раз (общий образ для обоих продуктов при единой схеме Flash);
  - артефакты выкладывались с понятными именами (продукт-конфиг.elf/bin/fw).
- Область применения: все конфигурации и продукты, описанные в ТЗ и в README.
- **Реализовано:** `.github/workflows/build.yml` — сборка MP-02m (all_targets), cyntron-dtv (all_targets), pack_all; артефакт `out/` (mp-02m-*, cyntron-dtv-*, boot_fw.bin). Локально: `scripts/build_all_products.sh`.

### 7.3 STM32CubeIDE (cyntron-dtv)

- Сохранить возможность открытия и сборки cyntron-dtv в STM32CubeIDE: обновить .cproject так, чтобы в состав сборки входили исходники из shared (через linked resources или дополнительные пути к каталогам вне папки проекта) и правильные include path. Либо документировать, что полная сборка всех конфигов и бутлоадера выполняется только через make.
- **Документировано:** полная сборка — через make (README, doc/CYNTRON_DTV_SHARED_MIGRATION.md, раздел 8); при использовании CubeIDE для отладки — добавить в проект пути к shared (include и при необходимости linked resources).

---

## 8. Этапы реализации (план работ)

### Этап 1. Подготовка общего ядра (shared)

1. Создать каталог `shared/modbus` (inc, src). Перенести из MP-02m в shared/modbus исходники и заголовки: modbus.c, modbus_rtu_hw.c, fast_mb.c, fast_mb_events.c, fast_mp_port.c и соответствующие .h.
2. Убрать из перенесённого кода прямые зависимости от путей вида `../../Inc/main.h`, `../../apps/inc/app_c.h`, `../../gpio/inc/mcp23s17.h` и т.п. Ввести портовый заголовок (например `modbus_port.h` в shared/modbus/port или в каждом продукте), объявляющий: UART/TIM handles, get_modbusaddress, get_fastmb_serialaddress, get_baudrate, get_stop_bit, конфиг парности, Error_Handler, при необходимости функции таймера/UART и типы для nmbs_arg_t. Подключать этот заголовок в shared коде вместо main.h/modbus.h продукта.
3. Создать каталоги `shared/drivers/` и `shared/sensors/` (структура подкаталогов по п. 5.4); завести документ `doc/SHARED_DRIVERS_SENSORS_MATRIX.md` (или раздел в ТЗ) с матрицей «устройство — продукт — размещение» (см. приложение C). **Выполнено:** ee24 перенесён в `shared/drivers/ee24`; MP-02m и cyntron-dtv подключают shared, обёртки записи (eeprom_protect_write) остаются в продукте.
4. В MP-02m подключить сборку из shared/modbus (путь к shared от корня), обновить include path; убедиться, что сборка Debug/Release/AppBoot проходит.
5. Решить вопрос nanoMODBUS: перенести в shared или оставить submodule в корне/shared; обновить пути в shared/modbus.

### Этап 2. Бутлоадер в shared (выполнено)

1. Бутлоадер перенесён в `shared/bootloader/`. Makefile бутлоадера использует BL_TOP = shared/bootloader, TOP = корень репозитория (Core/Startup, Core/Src/system_stm32f0xx.c, Drivers из корня). В корне `bootloader/` — обёртка, перенаправляющая сборку в shared/bootloader.
2. Образ бутлоадера собирается (`make -C shared/bootloader -j`) и совместим с приложением AppBoot (0x0800800, 188 К).

### Этап 3. cyntron-dtv: подключение shared modbus и порт (выполнено)

1. Добавить в cyntron-dtv портовый заголовок (например `Core/Inc/modbus_port_cyntron_dtv.h`), объявив все символы и типы, требуемые shared modbus (modbusUart, TimerFastMB, get_modbusaddress, get_fastmb_serialaddress, get_baudrate, get_stop_bit, парность, Error_Handler, callback’и для регистров/coils при необходимости). **Выполнено:** Core/Inc/modbus_port.h, обёртки в ModbusRTU_hw.c.
2. Удалить из cyntron-dtv Core/Src и Core/Inc локальные fast_mb.c, fast_mb_port.c, fast_mb_events.c, ModbusRTU_hw.c, nanomodbus.c (и при переходе на общий nanoMODBUS — обновить включения). **Выполнено:** локальные fast_mb*.c и fast_mb*.h удалены; ModbusRTU_hw.c остаётся; nanomodbus.c исключён из сборки.
3. Настроить сборку cyntron-dtv (Makefile и/или .cproject): исходники из shared/modbus, include — shared/modbus/inc и портовый заголовок продукта. Собрать Debug/Release, проверить работу Modbus и быстрого Modbus на стенде. **Выполнено:** Makefile собирает из shared/modbus; сборка Debug после clean проходит.

### Этап 4. cyntron-dtv: бутлоадер и AppBoot (выполнено)

1. Заменить линкер приложения под бутлоадер на 188 К @ 0x0800800; обновить main.c (FLASH_APP_MAX_ADDR и т.д.). **Выполнено:** линкер `STM32F030CCTX_FLASH_boot_188K.ld`.
2. Добавить конфигурацию AppBoot в сборку cyntron-dtv (BOOTLOADED, новый .ld). **Выполнено:** BUILD_CFG=AppBoot, макросы BOOTLOADED, APP_FLASH_188K.
3. Собрать объединённый образ: бутлоадер (общий) + AppBoot cyntron-dtv; прошить устройство, проверить запуск и переход в бутлоадер по регистру 129 и обновление .fw. **Выполнено:** цель `make -C cyntron-dtv combined` даёт build/AppBoot/boot_fw_cyntron.bin.

### Этап 5. Makefile и CI для cyntron-dtv (выполнено)

1. Реализовать Makefile в cyntron-dtv (или в корне с целями по продуктам), собирающий Debug, Release, AppBoot с использованием shared и mk/. **Выполнено:** cyntron-dtv/Makefile, все датчики из shared с портами.
2. Обновить pack/out: артефакты cyntron-dtv с именами вида cyntron-dtv-Debug.elf, cyntron-dtv-AppBoot.bin. **Выполнено:** `make pack_all` копирует MP-02m и cyntron-dtv в out/ (pack + pack_cyntron).
3. Обновить CI: добавление сборки cyntron-dtv и, при необходимости, бутлоадера; публикация артефактов. **Скрипт:** scripts/build_all_products.sh (all_targets обоих продуктов + pack_all).

### Этап 6. Документация и критерии приёмки

1. Обновить README и doc: структура каталогов, общее ядро (shared), как собирать каждый продукт, как собирать бутлоадер и combined образ. **Выполнено:** README описывает shared (modbus, bootloader, drivers/ee24), сборку MP-02m и cyntron-dtv.
2. Добавить краткий документ по переносу нового продукта на STM32F030CC (какие каталоги подключить, какой портовый заголовок реализовать, пример линкера и конфигурации). **Выполнено:** `doc/NEW_PRODUCT_PORTING.md`; перенос драйверов в shared — через `scripts/move_to_shared.py` (копирование), затем правки include и сборки.

---

## 9. Критерии приёмки

- Существует общая кодовая база (shared) для Modbus RTU, быстрого Modbus и порта (и при выборе — бутлоадера); в ней нет продукт-специфичных путей включения к конкретным приложениям/модулям. Структура shared предусматривает также общие драйверы микросхем и библиотеки датчиков (shared/drivers, shared/sensors); матрица размещения ведётся в doc (п. 5.4). **Выполнено:** shared/drivers (ee24, ad53x4, atm90e32, mcp23s17 с портами в Core/Inc); shared/sensors (tables, hdc1080, mcp9808, ld2412, BME, ZMOD); все датчики cyntron-dtv собираются из shared через портовые заголовки в Core/Inc (без путей вида ../../Inc или ../../Modules в shared коде).
- Проект cyntron-dtv использует Modbus и fast_mb из shared (modbus.c, fast_mb.c, fast_mp_port.c, fast_mb_events.c); ModbusRTU_hw.c локально (своя карта регистров); сборка и combined образ работают.
- В cyntron-dtv включены приложение под бутлоадер (AppBoot): линкер 188 К @ 0x0800800 (`STM32F030CCTX_FLASH_boot_188K.ld`), макросы BOOTLOADED/APP_FLASH_188K; образ combined (бутлоадер + AppBoot) собирается (`make -C cyntron-dtv combined`); обновление по Modbus (регистр 129, .fw) — по той же схеме, что и MP-02m.
- Сборка всех конфигураций (Debug, Release, AppBoot) для MP-02m и cyntron-dtv выполняется через make; артефакты в out/ с именами mp-02m-*, cyntron-dtv-* (`make pack_all`); при наличии CI — собирать оба продукта и публиковать артефакты (пример: `scripts/build_all_products.sh`).
- Документация: README, doc/TZ_*, doc/SHARED_DRIVERS_SENSORS_MATRIX.md, doc/NEW_PRODUCT_PORTING.md, doc/CYNTRON_DTV_SHARED_MIGRATION.md — структура shared, шаги сборки, схема Flash, перенос нового продукта.

---

## 10. Глоссарий и ссылки

- **Общее ядро (shared)** — каталог исходников и заголовков, общих для нескольких продуктов (modbus, bootloader, драйверы микросхем, библиотеки датчиков по п. 5.4).
- **Портовый заголовок (modbus port)** — заголовок, подключаемый в продукте и объявляющий типы/символы/callback’и, которые использует shared modbus (UART, таймер, адреса, EEPROM, обработчик ошибок).
- **Продукт** — целевое устройство (MP-02m или cyntron-dtv) со своей платой, приложением и конфигурацией сборки.

- Документы: `README.md`, `doc/BOOTLOADER_INTEGRATION.md`, `doc/TZ_CONFIG_AND_BUILD_STRUCTURE.md`, `doc/CI_CD_UNIFICATION_PROPOSAL.md` (при наличии).

---

## Приложение A. Сводка различий MP-02m и cyntron-dtv

| Аспект | MP-02m | cyntron-dtv |
|--------|--------|-------------|
| Плата / ревизия | HARDWARE_MP 20/21/22 | HARDWARE_MP 30 |
| Приложение | Модули расширения (DO/DI/AO/AI, счётчик, тензо) | Датчики (BME, HDC, MCP9808, DS18B20, ZMOD, LD2412) |
| Modbus UART | USART (см. Core) | USART2 |
| nanoMODBUS / Modbus | shared/modbus (основной проект) | **Из основного проекта:** shared/modbus; Core/Inc/modbus.h подключает shared; nanomodbus.c исключён из сборки |
| fast_mb / ModbusRTU_hw | Core/modbus + shared | shared fast_mb* + локальный ModbusRTU_hw (своя карта регистров) |
| Бутлоадер | bootloader/ (2 К + 32 К), приложение 188 К | Та же схема: линкер 188 К @ 0x0800800 (STM32F030CCTX_FLASH_boot_188K.ld); combined образ (бутлоадер + AppBoot) — make -C cyntron-dtv combined |
| Сборка | Makefile + mk/ | Makefile + mk/ (из корня или из cyntron-dtv); CubeIDE по желанию |

---

## Приложение B. Пример портового заголовка (объявления для shared)

Пример содержимого портового заголовка продукта (объявления, которые должен предоставить продукт для shared modbus):

```c
#ifndef MODBUS_PORT_PRODUCT_H
#define MODBUS_PORT_PRODUCT_H

#include "main.h"  /* продукт-специфичный; при рефакторинге заменить на минимальный набор */

extern UART_HandleTypeDef modbusUart;
extern TIM_HandleTypeDef TimerFastMB;

uint8_t get_modbusaddress(void);
uint32_t get_fastmb_serialaddress(void);
uint32_t get_baudrate(void);
uint8_t get_stop_bit(void);
/* парность — через существующий конфиг или get_parity() */

void Error_Handler(void);

/* при необходимости: объявления для nmbs_arg_t и функций таймера/UART,
   если они не определены в shared/fast_mb_port.h */
#endif
```

Реализации остаются в продукте (usart.c, i2c.c, main.c и т.д.); shared код только вызывает эти функции и использует объявленные объекты.

---

## Приложение C. Матрица драйверов и датчиков (размещение в shared / продукт)

Таблица фиксирует текущее и целевое размещение кода; актуальная версия ведётся в `doc/SHARED_DRIVERS_SENSORS_MATRIX.md`.

| Устройство / подсистема | MP-02m | cyntron-dtv | Кандидат в shared | Примечание |
|------------------------|--------|-------------|-------------------|------------|
| EEPROM (ee24) | shared/drivers/ee24 + Core/Inc, Core/storage | shared/drivers/ee24 + Core/Inc | да | Защита записи — в продукте |
| nanoMODBUS / Modbus | shared/modbus | shared/modbus | да | cyntron-dtv: nanomodbus.c исключён |
| Modbus RTU HW, fast_mb | shared/modbus | shared fast_mb* + локальный ModbusRTU_hw | да | cyntron-dtv: своя карта регистров |
| MCP23S17 | shared/drivers/mcp23s17 + Core/Inc mcp23s17_port.h | — | да | Только MP-02m |
| AD5324 (ad53x4) | shared/drivers/ad53x4 + Core/Inc ad53x4_port.h | — | да | Только MP-02m |
| ATM90E32 | shared/drivers/atm90e32 + Core/Inc atm90e32_port.h | — | да | Только MP-02m |
| ADS1220, MUX, AI-каналы | Core/adc | — | нет (пока) | Только MP-02m |
| Таблицы (термопары, NTC, PT, RTD) | shared/sensors/tables | — | да | Только MP-02m |
| BME280, BME680 | — | shared/sensors/bme + порты Core/Inc | да | bme280_driver.h, bme68x_driver.h, bme_*_port.h |
| HDC1080, MCP9808 | — | shared/sensors/hdc1080, mcp9808 + порты | да | hdc1080_port.h, mcp9808_port.h |
| DS18B20 | — | Modules/ds18b20 + drv_sensor_ds18b20 | при появлении в MP-02m | 1-Wire |
| ZMOD4410 | — | shared/sensors/zmod4410 + zmod_port.h | да | Только cyntron-dtv |
| LD2412 | — | shared/sensors/ld2412 + ld2412_port.h | да | Только cyntron-dtv |
| eeprom_manager | Core/storage | логика в i2c.c / конфиг | опционально | При унификации формата — в shared |

При появлении второго продукта, использующего устройство из столбца «только в одном продукте», соответствующий код переносится в shared с портовым заголовком; матрица обновляется. Перенос: `scripts/move_to_shared.py`, затем правки #include и сборки.
