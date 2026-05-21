# Анализ причин зависаний и ошибок (Modbus IRQ/DMA, бутлоадер)

Документ фиксирует типичные причины зависаний и перехода в Error_Handler, аналогичные уже исправленным (broadcast → nano_RecieveMode из ISR, бутлоадер без таймаута при входе по magic).

## 1. Вызов тяжёлой инициализации из прерывания (ISR)

### 1.1 nano_RecieveMode() и reload_rs485

**Проблема:** В `nano_RecieveMode()` (fast_mp_port.c) при `reload_rs485 != 0` выполняются `HAL_UART_DeInit()`, `ModbusUart_Init()`, `fast_mb_init()`, `MX_TIM_FastMB_Init()` — это нельзя вызывать из контекста прерывания (HAL и таймеры к этому не рассчитаны → HAL error → Error_Handler).

**Где вызывается nano_RecieveMode из ISR (режим MODBUS_MODE_IRQ):**

| Контекст | Файл:функция | Исправление |
|----------|--------------|-------------|
| TIM6 10 ms | modbus_rtu_hw.c: rtu_broadcast_tick() | только флаг rtu_broadcast_do_in_main; poll в main (см. 1.2) |
| TimerFastMB | fast_mb.c: Timer_FastModbus() — broadcast (addr 0) | skip_reload добавлен |
| TimerFastMB | fast_mb.c: Timer_FastModbus() — mb_poll_events (arbitrage_loss) | skip_reload добавлен |
| TimerFastMB | fast_mb.c: Timer_FastModbus() — mb_run_arbitrage (arbitrage_loss) | skip_reload добавлен |
| UART RX complete | fast_mb.c: UART_RxCplt() — overflow input buffer | skip_reload добавлен |

**Итог:** Перед каждым вызовом `nano_RecieveMode()` из прерывания нужно вызывать `fast_mp_port_set_skip_reload(1)`, после — `fast_mp_port_set_skip_reload(0)`. Reload выполнится при следующем входе в `nano_RecieveMode` из основного цикла.

### 1.2 Broadcast: nmbs_server_poll и nano_RecieveMode из rtu_broadcast_tick (TIM6 ISR)

**Проблема (исправлена):** `rtu_broadcast_tick()` вызывается из обработчика TIM6 (10 мс). В нём выполнялись `nmbs_server_poll()` и при необходимости `nano_RecieveMode()` — вызов HAL UART (Transmit/Receive) из ISR приводил к HAL error (gState/RxState) и переходу в Error_Handler (IPSR≠0).

**Исправление:** В `rtu_broadcast_tick()` при наступлении дедлайна только выставляется флаг `rtu_broadcast_do_in_main`. Функция `rtu_broadcast_process_pending()` вызывается из main loop и выполняет копирование буфера, `nmbs_server_poll()`, при необходимости `nano_RecieveMode()` или `MB_RecieveFrame()` (DMA). Таким образом, тяжёлая обработка broadcast выполняется не из ISR.

**Дополнительно:** Перед вызовом `nmbs_server_poll()` в `rtu_broadcast_process_pending()` необходимо прервать текущий приём UART (`HAL_UART_AbortReceive`): после приёма broadcast приём уже перезапущен (DMA — в RxEventCallback, IRQ — ожидание следующего кадра). Иначе при попытке передачи ответа UART остаётся в BUSY_RX, HAL переходит в некорректное состояние (gState/RxState) и вызывается Error_Handler из main (IPSR=0). В DMA-режиме то же условие добавлено в `MB_SendFrame()` перед запуском передачи.

### 1.3 Режим DMA: MB_RecieveFrame() из HAL_UART_TxCpltCallback

В `modbus_rtu_hw.c` при `MODBUS_MODE == MODBUS_MODE_DMA` в `HAL_UART_TxCpltCallback()` раньше вызывался `MB_RecieveFrame()`, внутри которого при `must_reload_rs485 != 0` выполнялись DeInit/Init из прерывания.

**Реализовано:** В колбэке при `reload_rs485` выставляется только флаг `reload_rs485_pending`; полный reload (DeInit/Init) не выполняется. В колбэке делается только переключение на приём и перезапуск `HAL_UARTEx_ReceiveToIdle_DMA`. При следующем вызове `MB_RecieveFrame()` из main loop (после обработки кадра) в начале функции проверяется `reload_rs485_pending`, выставляется `must_reload_rs485`, и выполняется полная переинициализация UART.

---

## 2. critical_stop() = Error_Handler() из ISR

**Определение:** В `main.h`: `#define critical_stop() Error_Handler()`.

**Проблема:** `Error_Handler()` делает `__disable_irq()`, затем цикл `while(1)` с `delay(1000)` (активная пауза по NOP) и переключение красного LED. Вызов из прерывания приводит к:

- длительной блокировке в ISR (delay — это миллионы NOP);
- отключению прерываний на всё время цикла;
- возможному срабатыванию watchdog или «мёртвому» устройству.

**Где вызывался critical_stop из контекста прерывания (режим IRQ):**

- **fast_mb.c:** все ветки в `UART_RxCplt()` и `Timer_FastModbus()` при ошибках HAL (TIM_Stop, TIM_ReInit, UART_AbortReceive, UART_Receive, TIM_Start, write_serial и т.д.) — десятки вызовов.
- **fast_mp_port.c:** в `write_serial()`, `Receive_Serial()`, `nano_RecieveMode()` при ошибках HAL (в т.ч. при вызове из ISR).
- **fast_mb_port.h:** в `clear_tim_flag()` — цикл ожидания флага таймера с счётчиком; при отсутствии флага после 50 итераций вызывается `critical_stop()` (может вызываться из `nano_RecieveMode` → из ISR).

**Было реализовано, откат:** Макрос заменялся на функцию с отложенным вызовом Error_Handler из main loop при вызове из ISR. После этого устройство перестало отдавать данные по Modbus (вероятно, частый вызов critical_stop из ISR → флаг → Error_Handler в main). Отложенный вариант откатан: снова `#define critical_stop() Error_Handler()`. В Error_Handler при COM_PORT_DEBUG выводится IPSR (0 = вызов из main, ≠0 = из ISR) для диагностики.

---

## 3. clear_tim_flag() в контексте ISR

**Код:** В `fast_mb_port.h` макрос/инлайн `clear_tim_flag()` ждёт установки `TIM_FLAG_UPDATE` в цикле с счётчиком (до 50 раз). Если флаг не появился — вызывается `critical_stop()`.

**Риск:** При вызове из `nano_RecieveMode()` (который теперь может вызываться из ISR с skip_reload) ожидание в цикле выполняется в прерывании. Если таймер по какой-то причине не выдаёт UPDATE (ошибка конфигурации, приоритеты, занятость шины), произойдёт спин и затем Error_Handler из ISR.

**Рекомендация:** Оставить как есть, но иметь в виду: при странных зависаниях после приёма broadcast/overflow/arbitrage в IRQ-режиме стоит проверить, не вызывается ли `clear_tim_flag` из ISR и не застревает ли он по таймеру. При рефакторинге можно вынести ожидание флага из ISR (например, отложенная проверка в main loop).

---

## 4. Бутлоадер: таймаут 120 с только при «режиме прошивки»

**Проблема (исправлена):** Таймаут 120 с без активности и переход в приложение срабатывали только при `in_firmware_update_mode == true`, т.е. после приёма info-блока. При входе по magic (рег. 129 или 0x10 0x1000) info не приходит → устройство оставалось в бутлоадере бесконечно (мигание красным).

**Исправление:** Введён флаг `s_entered_by_magic`; условие перехода по таймауту: `(in_firmware_update_mode || s_entered_by_magic) && 120s && app valid`. Подробно — в `doc/BOOTLOADER_ENTRY_AND_BROADCAST.md`.

---

## 5. Broadcast и массовый переход в бутлоадер

**Проблема (исправлена):** Кадр с broadcast-адресом обрабатывается всеми устройствами. Запись в рег. 129 или 0x10 по адресу 0x1000 по broadcast переводила все устройства в бутлоадер.

**Исправление:** В основном приложении при адресе запроса = broadcast переход в бутлоадер (magic + reset) не выполняется. **Broadcast-адрес задан 255 (0xFF)** в `NMBS_BROADCAST_ADDRESS` (Core/modbus/inc/modbus.h), чтобы искажённый первый байт (0x00) не вызывал ложных срабатываний на линиях без реального broadcast. Подробно — в `doc/BOOTLOADER_ENTRY_AND_BROADCAST.md`.

**Арбитраж по слотам (не долгий):** Ответ на broadcast откладывается на `addr*3` мс (макс. 30 мс), чтобы устройства отвечали по очереди. Дедлайн проверяется в TIM6 (каждые 10 мс) и **дополнительно в начале `rtu_broadcast_process_pending()`** при каждом проходе main — так слот соблюдается с точностью до периода main loop (~1 мс), а не до 10 мс. Иначе несколько устройств с адресами в одном 10 мс интервале (например 7, 8, 9 → 21, 24, 27 мс) могли бы выставить флаг в один тик и ответить одновременно (коллизия). Ограничения: задержка ответа не более 30 мс; `clear_tim_flag()` в Fast Modbus ждёт до 50 итераций (таймер — доли мс), затем Error_Handler при сбое таймера.

---

## 6. Тяжёлые и блокирующие операции из таймеров/колбэков

- **HAL_Delay / длинные циклы в ISR** — в коде проекта HAL_Delay в прерываниях не используется; в Error_Handler используется собственная `delay()` (NOP-цикл) — из ISR это недопустимо (уже отмечено выше).
- **Сброс на заводские (light_indicator):** выполнение перенесено в main loop (EE24_Write и задержки не в ISR) — корректно.
- **system_timer (TIM6):** в обработчике вызываются `rtu_broadcast_tick()`, `update_input()`, логика индикации неактивности. Важно не добавлять в этот обработчик блокирующие вызовы (HAL_Delay, запись во внешнюю память без таймаута и т.п.).

---

## 7. Краткий чек-лист при доработках

- [x] Любой вызов `nano_RecieveMode()` из прерывания (IRQ) — обёрнут в `fast_mp_port_set_skip_reload(1)` / `fast_mp_port_set_skip_reload(0)`.
- [x] В DMA-режиме при смене параметров UART не выполнять DeInit/Init в колбэках прерываний; использовать флаг и main loop (reload_rs485_pending).
- [ ] Не вызывать из прерываний: Error_Handler — отложенный вариант откатан (ломало ответ Modbus). При срабатывании Error_Handler смотреть в лог: IPSR≠0 = вызов из ISR.
- [ ] При добавлении нового кода в `Timer_FastModbus`, `UART_RxCplt`, `rtu_broadcast_tick`, `HAL_UART_TxCpltCallback` проверять отсутствие тяжёлой инициализации и блокирующих операций.

Область применения: основное приложение (Debug/Release), режимы Modbus IRQ и DMA; бутлоадер — отдельно в BOOTLOADER_ENTRY_AND_BROADCAST.md.

---

## 8. Недостоверные данные (ошибки CRC, неверные тайминги) — режим IRQ

**Симптомы:** Пакеты приходят с ошибками или неверной контрольной суммой, не те временные задержки; данные есть, но недостоверные.

**Причины и исправления (режим MODBUS_MODE_IRQ, программное управление DE):**

1. **DE (RS-485) переключается в TX без задержки** — первый бит мог уйти до стабилизации линии.  
   **Исправление:** В `fast_mp_port.c` в `write_serial()` после `SetRS485Transmit()` добавлена короткая задержка (~100 циклов на 48 МГц ≈ 2 мкс) перед вызовом `UART_Transmit`.

2. **Переключение в приём сразу по TX complete** — колбэк срабатывает, когда последний байт записан в сдвиговый регистр UART; последний бит ещё на линии. Раннее переключение DE в RX искажает конец кадра.  
   **Исправление:** В `fast_mb.c` в `UART_TxCplt()` перед вызовом `nano_RecieveMode()` / `fastmodbus_RecieveMode()` добавлена задержка на время одного символа (10 бит при 8N1): `delay_cycles = 10 * SystemCoreClock / baud`, ограничена 100–15000 циклами.

3. **Флаги ошибок UART (PE, FE, NE, ORE) не сбрасывались** перед новым приёмом — следующий кадр мог видеть старый ErrorCode.  
   **Исправление:** В `fast_mp_port.c` в `fastmodbus_RecieveMode()` и `nano_RecieveMode()` перед запуском `UART_Receive` при `ErrorCode != HAL_UART_ERROR_NONE` выполняется `__HAL_UART_CLEAR_FLAG` и сброс `ErrorCode`.

При использовании **USART2_RTS_HARDWARE** (только в DMA) тайминги DE задаются в `HAL_RS485Ex_Init` (AssertionTime/DeassertionTime); в IRQ этот макрос не определён, DE управляется вручную — задержки выше применяются.

**Если на шине одно устройство:** кадры с адресом, отличным от нашего (например 4, 8, 32, 160), означают искажение первого байта (шум, граница кадра, тайминги DE). В логе при COM_PORT_DEBUG выводится строка «RX addr=N len=L (not ours - possible corruption if single device on bus)». Снизить такие срабатывания помогают задержки DE (п. 1–2 выше), экранирование/скрутка линии и проверка терминации RS‑485.
