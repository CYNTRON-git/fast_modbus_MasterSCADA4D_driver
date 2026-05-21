# Перенос нового продукта на STM32F030CC с общим ядром (shared)

Краткий порядок подключения продукта к shared/modbus и сборке под бутлоадер.

## 1. Структура

- В репозитории общее ядро в **shared/** (modbus, при необходимости drivers/sensors).
- Продукт — отдельная папка (например `cyntron-dtv/`) со своим `Core/`, `main.c`, приложением и конфигом платы.

## 2. Портовый заголовок modbus (обязательно)

В продукте нужен заголовок, который shared/modbus подключает вместо путей продукта. Рекомендуемое имя: **Core/Inc/modbus_port.h**.

В нём должны быть:

- Включения: `main.h`, `usart.h`, `tim.h`, доступ к EEPROM/конфигу (например `ee24.h`), заголовок слоя Modbus RTU HW продукта (если свой).
- Объявления (extern), требуемые shared:
  - `get_baudrate(void)`, `get_modbusaddress(void)`;
  - `ModbusUart_Init(void)`;
  - `MX_TIM_FastMB_Init(uint8_t timer_mode, nmbs_t *nmbs)`;
  - `reload_rs485`, `packet_sended` (volatile);
  - при использовании broadcast/inactivity: `config` (tConfig), `inactivity_modbus`, `set_counter_inactivity(uint8_t)`.
- Тип `nmbs_t` и прочие типы modbus — из `modbus.h` (shared/modbus/inc); портовый заголовок может включать его через `#include "modbus.h"` при корректном порядке include path.

Пример структуры порта для MP-02m: `Core/Inc/modbus_port.h` в корне репозитория (продукт MP-02m).

## 3. Сборка

- **Include path:** добавить `$(TOP)/shared/modbus/inc` и путь к заголовку Modbus RTU HW продукта (в MP-02m — `Core/modbus/inc` для modbus_rtu_hw.h, fast_mb_events.h).
- **Исходники:** в сборку продукта включить:
  - `shared/modbus/src/modbus.c`, `shared/modbus/src/fast_mb.c`, `shared/modbus/src/fast_mp_port.c`, `shared/modbus/src/fast_mb_events.c`;
  - из продукта — слой Modbus RTU HW (реализация modbus_rtu_restart_receive, modbus_rtu_send_frame и др., см. doc/CYNTRON_DTV_SHARED_MIGRATION.md). В MP-02m: `Core/modbus/src/modbus_rtu_hw.c`; в продуктах без модулей расширения (как cyntron-dtv) — тот же shared fast_mb_events.c + портовый stub для module_handle_t и module_get_active() (всегда NULL).
- Не компилировать локальные копии modbus.c, fast_mb.c, fast_mp_port.c, fast_mb_events.c после перехода на shared.

## 4. Линкер под бутлоадер (единая схема с MP-02m)

- Приложение под бутлоадер: начало **0x0800800**, размер приложения **188 КБ**.
- Линкер: отдельный скрипт (например `STM32F030CCXX_FLASH_boot.ld`) с `FLASH (rx) : ORIGIN = 0x0800800, LENGTH = 188K`.
- В конфигурации AppBoot: макрос `BOOTLOADED`, remap таблицы векторов в RAM при старте (см. README и существующий main.c MP-02m).

## 5. Конфигурации

- **Debug / Release** — без BOOTLOADED, полный размер Flash по умолчанию.
- **AppBoot** — с BOOTLOADED и линкером на 188 КБ; образ для объединения с бутлоадером.

## 6. Перенос драйвера/библиотеки в shared (без копирования всего файла вручную)

- **Скрипт:** `python scripts/move_to_shared.py SRC DEST` — копирует файл в shared (пути относительно корня репо). Пример: `python scripts/move_to_shared.py Core/drivers/foo.c shared/drivers/foo/src/foo.c`.
- После копирования: поправить только `#include` и зависимости в перенесённом файле; добавить в Makefile продукта(ов) путь к `shared/.../src/*.c` и `-I shared/.../inc`; обновить `doc/SHARED_DRIVERS_SENSORS_MATRIX.md` и при необходимости `shared/drivers/README.md`.

## 7. Документация

- Матрица драйверов/датчиков и размещение в shared: `doc/SHARED_DRIVERS_SENSORS_MATRIX.md`.
- Полное ТЗ архитектуры: `doc/TZ_MULTIPRODUCT_ARCHITECTURE_AND_SHARED_CORE.md`.
