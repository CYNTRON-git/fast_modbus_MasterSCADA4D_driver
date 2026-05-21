# Соответствие реализации Modbus и быстрого Modbus документации и референсам

Проверка по:
- **Modbus over serial line V1.02** — [спецификация](https://ftp.owen.ru/ModbusCourse/00_ModbusSpecs/Modbus_over_serial_line_V1_02.pdf) (формат RTU, CRC, тайминги).
- **Расширение Wiren Board (быстрый Modbus)** — [protocol.ru.md](https://github.com/wirenboard/wb-modbus-ext-scanner/blob/main/docs/protocol.ru.md) (сканирование, 0x08/0x09, события, тайминги арбитража).
- **Прошивка WB** — [wb-mcu-fw-flasher](https://github.com/wirenboard/wb-mcu-fw-flasher) / [wb-mcu-fw-updater](https://github.com/wirenboard/wb-mcu-fw-updater) (0x08 по серийному, регистры бутлоадера).
- **Наш прошивальщик** — `flasher_windows/` (0xFD 0x46 0x08/0x09, сканирование 0x01/0x03, формат как WB).

Детали: `MODBUS_SERIAL_LINE_V1_02_COMPLIANCE.md`, `FAST_MODBUS_PROTOCOL_ANALYSIS.md`, `WB_FLASHER_VS_OURS.md`.

---

## 1. Стандартный Modbus RTU

| Область | Источник | Статус |
|--------|----------|--------|
| Кадр [Slave][PDU][CRC] | V1.02 | ✅ |
| CRC-16 0xA001, LSB first в кадре | V1.02 | ✅ |
| Big-endian в PDU (адреса, счётчики, регистры) | V1.02 | ✅ |
| T3.5 (IDLE / Receiver Timeout в DMA) | V1.02 | ✅ |
| Функции 0x01–0x06, 0x0F, 0x10 | V1.02 | ✅ |
| Исключения FC+0x80, коды 01–04 | V1.02 | ✅ |
| Адрес слейва 1..247 | V1.02 | ✅ |
| Ответ на broadcast (адрес 0) | V1.02 | ⚠️ Намеренное отклонение (ответ с реальным адресом для сканеров) |

---

## 2. Быстрый Modbus (0xFD, 0x46/0x60) — protocol.ru.md

### 2.1 Общее

| Элемент | Документ | Реализация | Статус |
|---------|----------|------------|--------|
| Адрес 0xFD (расширенный режим) | protocol.ru.md | buf[0]==0xFD в modbus_rtu_hw.c, fast_mb.c | ✅ |
| Функция 0x46 (0x60 deprecated) | protocol.ru.md | buf[1]==0x46 \|\| 0x60, ответ той же функцией | ✅ |
| CRC по полю до CRC (стандартный Modbus) | protocol.ru.md | nmbs_crc_calc, LSB first в кадре | ✅ |

### 2.2 Сканирование (0x01, 0x02, 0x03, 0x04)

| Элемент | Документ | Реализация | Статус |
|---------|----------|------------|--------|
| 0x01 — начало сканирования | FD 46 01 + CRC (5 байт) | fast_mb.c case 0x01, CRC по 3 байтам | ✅ |
| 0x02 — продолжение сканирования | FD 46 02 + CRC | case 0x02 | ✅ |
| 0x03 — ответ на сканирование | FD 46 03 + serial 4B BE + modbus_addr + CRC | answer_scan(): put_2(serial>>16), put_2(serial&0xFFFF), put_1(address_rtu) | ✅ |
| 0x04 — конец сканирования | FD 46 04 + CRC | end_scan() | ✅ |
| Арбитраж 32 окна, 4 бита приоритета + 28 бит serial | protocol.ru.md | make_arbitrage_data(): (0x6<<28)\|serial, (0xF<<28)\|serial | ✅ |

### 2.3 Работа по серийному (0x08, 0x09)

| Элемент | Документ | Реализация | Статус |
|---------|----------|------------|--------|
| Запрос 0x08 | FD 46 08 + serial 4B BE + PDU + CRC | check_fast_modbus 0x08, handle_emulate_standart (modbus.c) | ✅ |
| Ответ 0x09 | FD 46 09 + serial 4B BE + тело ответа + CRC | put_msg_header: 0xFD, 0x46/0x60, 0x09, put_2(serial>>16), put_2(serial&0xFFFF) | ✅ |
| Игнор при другом serial | — | msg.ignored = true при received_address != fastmodbus_address | ✅ |
| Запрет вложенного 0x46/0x60 | — | handle_req_fc: fc==0x46/0x60 → NMBS_ERROR_INVALID_REQUEST | ✅ |
| Обработка 0x08 в DMA | — | modbus_rtu_hw.c: при 0xFD и buf[2]==0x08 вызов nmbs_server_poll после fast_mb_enter_from_dma_frame | ✅ |

### 2.4 События (0x10, 0x11, 0x12)

| Элемент | Документ | Реализация | Статус |
|---------|----------|------------|--------|
| Запрос 0x10 | FD 46 10 + min_slave_id + max_data_len + confirm_slave_id + confirm_flag + CRC (10 байт) | fast_mb.c: buf_rec>=10, CRC по 8 байтам, confirm_slave_id=buf[6], confirm_flag=buf[7] | ✅ (исправлено) |
| Ответ 0x11 | slave_id, 46, 11, flag, event_count, data_len, события + CRC | answer_events(): packet[0]=address_rtu, 0x11, флаг, счётчик, данные | ✅ |
| Ответ 0x12 (событий нет) | FD 46 12 + CRC | send_no_events() | ✅ |
| Формат события | len, type, id BE, payload LE; типы 1–4 (coil/discrete/holding/input) | answer_events(), типы по документу | ✅ |

### 2.5 Настройка событий (0x18)

| Элемент | Документ | Реализация | Статус |
|---------|----------|------------|--------|
| Команда 0x18 по slave_id | addr 46 18 + длина + настройки + CRC | modbus.c: case 0x46/0x60 → субкоманда; 0x18 → handle_config_events_0x18 (разбор блоков type+addr+count+settings, ответ с масками LE) | ✅ |

### 2.6 Тайминги арбитража

| Элемент | Документ | Реализация | Статус |
|---------|----------|------------|--------|
| Ожидание начала: max(3.5 символа, 12 бит+800 мкс) | protocol.ru.md | compute_timer(): Arbitrage_Period (0x46), Arbitrage_Periodx60 (0x60) | ⚠️ Формулы сверять с документом |
| Окно: 12 бит + 50 мкс (кратно биту) | protocol.ru.md | Window_Period, Window_Periodx60 | ⚠️ |

---

## 3. Прошивальщик и бутлоадер

| Элемент | WB / наш flasher | Статус |
|---------|------------------|--------|
| Формат запроса по серийному | 0xFD 0x46 0x08 [serial 4B BE] inner PDU CRC | ✅ modbus_rtu.build_fast_modbus_request |
| Разбор ответа 0x09 | 0xFD 0x46 0x09 [serial 4B BE] inner CRC | ✅ parse_fast_modbus_response |
| Сканирование 0x01, разбор 0x03 | 0xFD 0x46 0x01+CRC; ответ 10 байт, serial BE, addr | ✅ build_wb_ext_scan_start, parse_wb_ext_scan_response |
| Регистры бутлоадера (129, 0x1000, 0x2000 и т.д.) | По wb-mcu-fw-flasher / BOOTLOADER_TZ | ✅ flash_protocol.py, бутлоадер |

---

## 4. Внесённые исправления (при верификации)

1. **0x10 (запрос событий):** По protocol.ru.md кадр 0x10 — 8 байт данных (FD 46 10 min_slave_id max_data_len confirm_slave_id confirm_flag) + 2 байта CRC = 10 байт. CRC считается по всем 8 байтам; confirm_slave_id и confirm_flag — байты 6 и 7. В коде было: CRC по 6 байтам, CRC в buf[6..7], confirm в buf[4..5]. Исправлено в `fast_mb.c`: buf_rec>=10, CRC по 8 байтам, received_crc=buf[8]|(buf[9]<<8), event_confirm_slave_id=buf[6], event_confirm_flag=buf[7]. В DMA для 0x10 минимальная длина кадра для входа в fast_mb изменена с 8 на 10 байт.

---

## 5. Краткая сводка

| Блок | Соответствие |
|------|--------------|
| Modbus RTU (кадр, CRC, функции, исключения) | ✅ кроме ответа на broadcast (по выбору) |
| Быстрый Modbus: 0xFD, 0x46/0x60, сканирование 0x01–0x04 | ✅ |
| Быстрый Modbus: 0x08/0x09 по серийному (в т.ч. DMA) | ✅ |
| Быстрый Modbus: события 0x10/0x11/0x12 (в т.ч. формат и CRC 0x10) | ✅ |
| Быстрый Modbus: 0x18 настройка событий | ✅ реализовано (modbus.c handle_config_events_0x18) |
| Тайминги арбитража | ⚠️ отдельные режимы 0x46/0x60 есть; формулы — сверять с protocol.ru.md |
| Прошивальщик (0x08/0x09, сканирование) | ✅ совместим с WB |

Итог: реализация Modbus и быстрого Modbus приведена в соответствие с документацией WB (protocol.ru.md) и с нашим прошивальщиком; расхождение по 0x10 исправлено; 0x18 реализован (unicast [addr] 46 18, ответ с битовыми масками по документу). Опционально: уточнение формул таймеров арбитража по документу; привязка сохранённых настроек 0x18 к fast_mb_events для реального включения/выключения генерации событий по регистрам.
