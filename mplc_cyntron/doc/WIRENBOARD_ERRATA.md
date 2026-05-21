# Учёт errata Wiren Board в проекте MP-02m

Документ описывает, как в проекте учтены известные ошибки (errata) устройств Wiren Board (WB-MR6C, WB-MAP3E, WB-MSW, WB-MCM8 и др.), связанные с протоколом Modbus RTU и расширением «Быстрый Modbus».

Источники:
- [WB-MR6C v.3: Errata](https://wiki.wirenboard.com/wiki/WB-MR6C_v.3:_Errata)
- [WB-MR6C v.2: Errata](https://wiki.wirenboard.com/wiki/WB-MR6C_v.2:_Errata)
- [WB-MAP3E: Errata](https://wiki.wirenboard.com/wiki/WB-MAP3E:_Errata)
- [WB-MSW v.4 / v.3: Errata](https://wiki.wirenboard.com/wiki/WB-MSW_v.4:_Errata)
- [WB-MCM8: Errata](https://wiki.wirenboard.com/wiki/WB-MCM8:_Errata)

---

## ERRMODBUS001 / ERRMR11: Ответ на адрес 0xFD

**Суть:** Устройства с «Быстрым Modbus» отвечали на *любой* пакет с адресом 0xFD как на стандартный Modbus (в т.ч. MODBUS_ERR_ILLEGAL_FUNCTION), мешая сторонним устройствам на одной шине.

**Исправление в проекте:** Пакеты с первым байтом 0xFD не передаются в `nmbs_server_poll()` — они обрабатываются только как Быстрый Modbus (0x46/0x60) в `fast_mb`. Стандартный Modbus на 0xFD не отвечает.

**Файлы:** `Core/modbus/src/modbus_rtu_hw.c` (проверка перед вызовом `nmbs_server_poll`).

---

## ERRMODBUS002: Ответ на сканирование командой 0x46

**Суть:** При запросе сканирования по команде 0x46 устройство отвечало командой 0x60; должно отвечать той же командой, которой был запрос (0x46 или 0x60).

**Исправление в проекте:** В ответах на сканирование (`answer_scan`, `end_scan`) используется код функции из запроса: `old_arbitrage` выставляется в `fast_mb` по второму байту пакета (0x46 или 0x60), и в ответ подставляется тот же код.

**Файлы:** `Modules/nanoMODBUS/nanomodbus.c` (`answer_scan`, `end_scan`); логика установки `old_arbitrage` — `Modules/nanoMODBUS/examples/stm32_irq/Core/Src/fast_mb.c`.

---

## ERRMODBUS003: Запись невалидных значений в регистры

**Суть:** При записи значений 256..65535 в некоторые регистры (в т.ч. адрес устройства — регистр 128) происходило отсечение старших бит (значение по модулю 256).

**Исправление в проекте:** Для регистра 128 (адрес Modbus) допускаются только значения 1..247; при `value == 0` или `value > 247` возвращается NMBS_EXCEPTION_ILLEGAL_DATA_VALUE. Запись 256 и выше не выполняется.

**Файлы:** `Core/modbus/src/modbus_rtu_hw.c`, обработчик `handle_write_single_register`, case 128.

---

## ERRMODBUS004: Ошибки в ответах с битовыми полями

**Суть:** Ответы по coil/discrete регистрам могли содержать неверные значения в старших битах.

**Статус:** Упаковка битовых полей выполняется библиотекой nanoMODBUS и callback'ами (`handle_read_coils`, `handle_read_discrete_inputs`) через `nmbs_bitfield_write`/`put_1(bitfield[i])`. При появлении аномалий по coil/discrete стоит проверить порядок байт и границы в этих обработчиках.

---

## ERRMODBUS006: Ответы на запросы с неверным битом чётности

**Суть:** Устройство отвечало на запросы с неверным битом чётности, хотя должно их игнорировать.

**Исправление в проекте:** Перед обработкой принятых данных в обработчике IDLE проверяется `huart->ErrorCode`. При любой ошибке UART (в т.ч. HAL_UART_ERROR_PE — ошибка чётности) кадр не обрабатывается, ответ не формируется, выполняется сброс приёма и очистка флагов ошибок.

**Файлы:** `Core/modbus/src/modbus_rtu_hw.c` (RXEVENT_IDLE, `HAL_UART_ErrorCallback`).

---

## Прочие errata

Остальные пункты errata (реле, монитор питания, частота, EEPROM, конкретные датчики и т.п.) относятся к другим продуктам Wiren Board и к аппаратной части; в прошивке MP-02m отдельно не учитываются.
