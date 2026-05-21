# Миграция cyntron-dtv на shared modbus

**Статус:** полная миграция выполнена. cyntron-dtv использует shared/modbus (modbus.c, fast_mb.c, fast_mp_port.c, fast_mb_events.c); слой Modbus RTU HW и регистровая карта остаются в продукте (ModbusRTU_hw.c).

**Выполнено:**
- **cyntron-dtv/Core/Inc/modbus.h** — обёртка над `shared/modbus/inc/modbus.h`, объявляет msg_buf_set/msg_buf_get/msg_buf_inc/msg_rec_reset (реализация в ModbusRTU_hw.c).
- **nanomodbus.c** исключён из сборки; в сборку включены **shared/modbus/src**: modbus.c, fast_mb.c, fast_mp_port.c, fast_mb_events.c.
- **modbus_port.h** — портовый заголовок: main.h, usart.h, tim.h, ee24.h, ModbusRTU_hw.h, module_interface_stub.h, shared modbus_rtu_hw.h, modbus.h; все extern для shared.
- **ModbusRTU_hw.c** — реализован API, ожидаемый shared fast_mb.c: modbus_rtu_restart_receive, modbus_rtu_send_frame, modbus_rtu_broadcast_allowed, rtu_broadcast_schedule, modbus_rtu_set_request_addr, modbus_rtu_report_unicast_poll, modbus_activity_tick_update; при COM_PORT_DEBUG — заглушки modbus_rtu_log_broadcast_hex, modbus_rtu_log_rx_frame. Для MODBUS_MODE_DMA используются MB_RecieveFrame/MB_SendFrame.
- **module_interface_stub.h** + **module_interface_stub.c** — типы module_config_t, module_handle_t, module_ops_t и `module_get_active()` (всегда NULL), чтобы shared fast_mb_events.c линковался без модулей расширения MP-02m. В main.h добавлены значения enum mp02_t (DO6DI8 … EN_METER) для компиляции fast_mb_events_is_discrete_module.
- **Makefile** cyntron-dtv: SRC_C из Core + shared/modbus/src; filter-out локальных fast_mb.c, fast_mb_port.c, fast_mb_events.c; INC с shared/modbus/inc первым; vpath для shared; Debug/Release/AppBoot и combined, pack_cyntron.

## 1. API слоя Modbus RTU HW (реализовано в ModbusRTU_hw.c)

- `modbus_rtu_restart_receive(void *nmbs, bool with_timer)` — IRQ: nano_RecieveMode(nmbs); DMA: MB_RecieveFrame().
- `modbus_rtu_send_frame(void *nmbs, const uint8_t *buf, uint16_t count)` — MB_SendFrame(buf, count, 0, NULL).
- `modbus_rtu_broadcast_allowed(void)` — return true.
- `rtu_broadcast_schedule`, `modbus_rtu_set_request_addr`, `modbus_rtu_report_unicast_poll` — заглушки (no-op).
- `modbus_activity_tick_update(void)` — inactivity_modbus = 0.
- При COM_PORT_DEBUG — заглушки modbus_rtu_log_broadcast_hex, modbus_rtu_log_rx_frame.

## 2. Сборка

- **Include:** `-I$(REPO)/shared/modbus/inc` первым, затем Core/Inc и т.д. (в Makefile через INC_BASE).
- **Исходники:** shared/modbus/src/modbus.c, fast_mb.c, fast_mp_port.c, fast_mb_events.c; из Core исключены fast_mb.c, fast_mb_port.c, fast_mb_events.c.
- **fast_mb_events:** берётся из shared; для линковки нужны module_handle_t и module_get_active() — предоставлены stub’ом (module_get_active() возвращает NULL).

## 3. Заголовки modbus

- В коде используется `#include "modbus.h"` (shared); порядок include обеспечивает shared/modbus/inc.

## 4. Типы и константы

- reload_rs485, packet_sended, tConfig, inactivity_modbus, set_counter_inactivity объявлены в modbus_port.h; реализация в продукте.

---

## 6. Линкер под бутлоадер (Этап 4 ТЗ)

Для единой схемы с MP-02m:

- Приложение под бутлоадер: начало **0x0800800**, размер **188 КБ** (вместо текущих 238 КБ в cyntron-dtv).
- Создать линкер-скрипт по образцу MP-02m: `STM32F030CCXX_FLASH_boot.ld` с `FLASH (rx) : ORIGIN = 0x0800800, LENGTH = 188K`.
- В main.c при конфигурации AppBoot: макрос `BOOTLOADED`, remap таблицы векторов в RAM при старте (см. README и main.c MP-02m).
- Объединённый образ: бутлоадер (из корня репозитория) + AppBoot cyntron-dtv — по тому же принципу, что и `make combined` в MP-02m.

## 7. Makefile и CI (Этап 5 ТЗ)

- **Makefile** в каталоге `cyntron-dtv/` реализован: TOP = cyntron-dtv, REPO = родитель (корень репозитория), подключены `mk/toolchain.mk`, `mk/rules.mk`, shared modbus, Modules (ow, ds18b20), Lib/Inc, линковка с `-L$(TOP)/Lib -liaq_2nd_gen`.
- Запуск: из каталога `cyntron-dtv` — `make` или `make BUILD_CFG=Debug` / `make Release`. Требуется наличие `Lib/libiaq_2nd_gen.a` для линковки.
- Конфигурации: **Debug, Release, AppBoot** (линкер 188 К: `STM32F030CCTX_FLASH_boot_188K.ld`, макросы `BOOTLOADED`, `APP_FLASH_188K`).
- **combined:** `make -C cyntron-dtv combined` — образ `build/AppBoot/boot_fw_cyntron.bin` (бутлоадер + AppBoot cyntron-dtv). Требуется предварительная сборка бутлоадера из корня: `make -C bootloader -j`.
- **pack:** `make -C cyntron-dtv pack_cyntron` — копирование в `out/`: cyntron-dtv-Debug.elf, cyntron-dtv-Release.elf, cyntron-dtv-AppBoot.bin, cyntron-dtv-boot_fw.bin.
- Из корня: `make cyntron_dtv`, `make cyntron_dtv_all`, `make cyntron_dtv_combined`, `make pack_all` (MP-02m + cyntron-dtv в out/).
- CI: при добавлении (например GitHub Actions) — собирать оба продукта (make all_targets, make -C cyntron-dtv all_targets), бутлоадер, pack_all; артефакты с именами продукт-конфиг (см. ТЗ п. 7.2). **Реализовано:** `.github/workflows/build.yml` — сборка MP-02m, cyntron-dtv, pack_all, выкладка `out/` артефактом.

## 8. STM32CubeIDE и полная сборка (ТЗ п. 7.3)

**Полная сборка** всех конфигураций (Debug, Release, AppBoot) и бутлоадера выполняется через **make** из корня или из каталога cyntron-dtv (см. README). Артефакты в `out/` создаются целью `make pack_all` после `make all_targets` и `make cyntron_dtv_all`.

**STM32CubeIDE** можно использовать для отладки и сборки одной конфигурации (например Debug): при этом в состав проекта должны входить исходники из shared (modbus, drivers/ee24, sensors/*) — через linked resources или дополнительные пути к каталогам вне папки проекта, а в C/C++ Build → Settings → Include paths — пути к `../shared/modbus/inc`, `../shared/drivers/ee24/inc`, `../shared/sensors/*/inc` и т.д. (относительно каталога cyntron-dtv). Либо документировать, что полная сборка всех конфигов и бутлоадера выполняется только через make (как в текущем состоянии).
