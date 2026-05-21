# Исправления протокола согласно документации Wiren Board

## Выявленные и исправленные несоответствия

### 1. ✅ Адрес для расширенных команд

**Было:** Использовался адрес `0x00` (стандартный broadcast)  
**Стало:** Используется адрес `0xFD` для расширенных команд Wiren Board

**Причина:** Согласно документации, устройства могут отвечать по адресу `0x00` на неизвестные команды, поэтому расширенный режим работает через зарезервированный адрес `0xFD`.

### 2. ✅ Структура команд сканирования

**Было:** Прямая команда `0x46` для сканирования  
**Стало:** Команда `0x46` с субкомандами:
- `0x01` - Начало сканирования
- `0x02` - Продолжение сканирования  
- `0x03` - Ответ на сканирование
- `0x04` - Конец сканирования

**Реализация:**
- `BuildBroadcastScanRequest()` - отправляет `0xFD 0x46 0x01`
- `BuildScanContinueRequest()` - отправляет `0xFD 0x46 0x02` (новый метод)
- `ParseScanResponse()` - ожидает ответ с `0x46 0x03`

### 3. ✅ Структура команд событий

**Было:** 
- `0x12` - запрос событий
- `0x13` - настройка событий

**Стало:**
- `0x10` - запрос событий (субкоманда `0x10`)
- `0x11` - передача событий (субкоманда `0x11`)
- `0x12` - ответ если события отсутствуют (субкоманда `0x12`)
- `0x18` - настройка отправки событий (субкоманда `0x18`)

**Реализация:**
- **Запрос событий 0x10 (WB):** кадр **10 байт**: `0xFD 0x46 0x10 min_slave_id max_data_len confirm_slave_id confirm_flag` + CRC (CRC по 8 байтам). Реализовано в `BuildEventRequestRequest(min_slave_id, max_data_len, confirm_slave_id, confirm_flag)`.
- **Ответ 0x11:** `slave_id 0x46 0x11 flag event_count data_len` + события (каждое: len_payload, type, id BE, payload LE) + CRC. Приём в `ReceiveFrame`, разбор в `ParseEventResponse`.
- **Ответ 0x12:** `0xFD 0x46 0x12` + CRC (5 байт). Приём в `ReceiveFrame`.
- `RequestEvents(events, confirm_slave_id, confirm_flag, out_confirm_slave_id, out_confirm_flag)` — для подтверждения приёма событий по WB передаёт/возвращает confirm для следующего опроса.
- `BuildEventConfigRequest()` - отправляет `0x46 0x18` с настройками

### 4. ✅ Формат ответа на сканирование (приведён к flasher_windows/modbus_rtu.py)

**Было:** `Адрес + 0x46 + 0x03 + Длина + Данные + CRC` (переменная длина)  
**Стало:** фиксированный кадр **10 байт**: `0xFD 0x46/0x60 0x03 [serial 4B BE] [modbus_addr 1B] CRC`

**Изменения:**
- Первый байт всегда `0xFD` (расширенный адрес)
- Функция `0x46` или legacy `0x60`
- Субкоманда `0x03`
- Серийный номер — 4 байта Big-Endian (байты 3–6)
- Адрес Modbus устройства — 1 байт (байт 7)
- CRC — 2 байта, LSB first (как в Modbus RTU)
- Константа `WB_EXT_SCAN_FRAME_LEN = 10`
- В `ReceiveFrame` добавлена ветка для приёма этого кадра (чтение ровно 10 байт)
- `CheckFastModbusSupport` обновлён: парсит ответ через `ParseScanResponse` и проверяет `response[7] == slave_id`

### 5. ✅ Формат ответа на события

**Было:** `Адрес + 0x12 + Количество + События + CRC`  
**Стало:** `Адрес + 0x46 + Субкоманда + Количество + События + CRC`

**Изменения:**
- Субкоманда `0x11` для передачи событий
- Субкоманда `0x12` для отсутствия событий
- Каждое событие теперь включает тип регистра (6 байт вместо 5)

### 6. ✅ Алгоритм сканирования

**Было:** Одна команда, ожидание всех ответов  
**Стало:** Многоэтапный процесс:
1. Команда начала сканирования (`0x01`)
2. Ожидание 3.5 фрейма молчания для арбитража
3. Чтение ответов
4. Команда продолжения сканирования (`0x02`) пока есть устройства
5. Обработка команды конца сканирования (`0x04`)

**Реализация:**
- Учет времени арбитража в зависимости от скорости передачи
- Циклическое сканирование до получения команды конца (`0x04`)

### 7. ✅ Проверка поддержки быстрого Modbus

**Было:** Проверка ответа на команду `0x46`  
**Стало:** Проверка ответа с субкомандой `0x03` (ответ на сканирование)

## Новые константы

```cpp
constexpr uint8_t MODBUS_EXTENDED_ADDRESS = 0xFD;  // Адрес для расширенных команд
constexpr uint8_t FUNC_EXTENDED = 0x46;            // Основная команда расширенных функций
constexpr uint8_t FUNC_EXTENDED_LEGACY = 0x60;     // Legacy (wb-modbus-ext-scanner)
constexpr uint8_t FUNC_FAST_SET_ADDRESS = 0x41;    // Смена адреса (разрешение коллизий)
constexpr size_t WB_EXT_SCAN_FRAME_LEN = 10;      // Длина кадра ответа на сканирование

// Субкоманды сканирования
constexpr uint8_t SUB_CMD_SCAN_START = 0x01;
constexpr uint8_t SUB_CMD_SCAN_CONTINUE = 0x02;
constexpr uint8_t SUB_CMD_SCAN_RESPONSE = 0x03;
constexpr uint8_t SUB_CMD_SCAN_END = 0x04;

// Субкоманды событий
constexpr uint8_t SUB_CMD_EVENT_REQUEST = 0x10;
constexpr uint8_t SUB_CMD_EVENT_TRANSMIT = 0x11;
constexpr uint8_t SUB_CMD_EVENT_NONE = 0x12;
constexpr uint8_t SUB_CMD_EVENT_CONFIG = 0x18;
```

## Реализовано в C++ (обращение по серийному 0x08/0x09)

1. **Работа с устройствами по серийному номеру:**
   - Запрос: `0xFD 0x46 0x08 [serial 4B BE] [inner PDU] CRC`
   - Ответ: `0xFD 0x46 0x09 [serial 4B BE] [inner response] CRC`
   - Методы: `ReadHoldingRegistersBySerial()`, `WriteSingleRegisterBySerial()`, `WriteMultipleRegistersBySerial()`
   - Внутренние: `BuildFastModbusRequest()`, `BuildReadHoldingRegistersBody()`, `BuildWriteSingleRegisterBody()`, `BuildWriteMultipleRegistersBody()`, `ParseFastModbusResponse()`, `ExchangeBySerial()`
   - В `ReceiveFrame` добавлена ветка для приёма кадра 0x09 (переменная длина по функции внутреннего ответа)

2. **Полная настройка событий:**
   - Текущая реализация `BuildEventConfigRequest()` упрощенная
   - Полная версия должна поддерживать диапазоны регистров и приоритеты

3. **Подтверждение приема событий:**
   - Согласно документации, требуется подтверждение приема событий

## Источники

- [Официальная документация Wiren Board](https://github.com/wirenboard/wb-modbus-ext-scanner/blob/main/docs/protocol.ru.md)
- [Статья на Habr о быстром Modbus](https://habr.com/ru/companies/wirenboard/articles/772308/)
