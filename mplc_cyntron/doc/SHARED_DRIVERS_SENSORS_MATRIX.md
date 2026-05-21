# Матрица драйверов и датчиков (shared / продукт)

Актуальная версия размещения кода по п. 5.4 и приложению C ТЗ `doc/TZ_MULTIPRODUCT_ARCHITECTURE_AND_SHARED_CORE.md`.

| Устройство / подсистема | MP-02m | cyntron-dtv | Кандидат в shared | Примечание |
|------------------------|--------|-------------|-------------------|------------|
| EEPROM (CAT24C02 / ee24) | shared/drivers/ee24 + Core/Inc обёртка, Core/storage | shared/drivers/ee24 + Core/Inc обёртка (ee24.h, ee24_conf.h) | да | **В shared:** shared/drivers/ee24 (inc+src). MP-02m и cyntron-dtv подключают shared; защита записи (eeprom_protect_write) — в продукте (EE24_Write_Protected в i2c.c). |
| nanoMODBUS / Modbus | shared/modbus (modbus.c, modbus.h) | shared/modbus (из основного проекта) | да | cyntron-dtv портирован на shared: modbus.h подключает shared, nanomodbus.c исключён из сборки; Modbus = из основного проекта |
| Modbus RTU HW, fast_mb, fast_mp_port | shared/modbus (все: modbus.c, modbus_rtu_hw.c, fast_mb.c, fast_mb_events.c, fast_mp_port.c) | shared fast_mb* + локальный ModbusRTU_hw (своя карта регистров) | да | MP-02m: полностью shared; cyntron-dtv: modbus.c + fast_mb* из shared, ModbusRTU_hw.c локально (см. CYNTRON_DTV_SHARED_MIGRATION.md) |
| MCP23S17 | shared/drivers/mcp23s17 + Core/Inc mcp23s17_port.h | — | да | В shared; MP-02m собирает из shared, порт в Core/Inc. |
| AD5324 (ad53x4) | shared/drivers/ad53x4 + Core/Inc ad53x4_port.h | — | да | В shared; MP-02m собирает из shared. |
| ATM90E32 | shared/drivers/atm90e32 + Core/Inc atm90e32_port.h | — | да | В shared; MP-02m собирает из shared. |
| ADS1220, MUX, AI-каналы | Core/adc | — | нет (пока) | Только MP-02m |
| Таблицы (термопары, NTC, PT, RTD) | shared/sensors/tables | — | да | В shared; MP-02m собирает из shared, -I shared/sensors/tables. |
| BME280, BME680 | — | shared/sensors/bme | да | cyntron-dtv: shared + порты bme280_port.h, bme68x_port.h, bme_common_port.h, bme280_driver.h, bme68x_driver.h (Core/Inc). |
| HDC1080, MCP9808 | — | shared/sensors/hdc1080, mcp9808 | да | cyntron-dtv собирает из shared; порты hdc1080_port.h, mcp9808_port.h (main.h, i2c.h). |
| DS18B20 | — | Modules/ds18b20 + drv_sensor_ds18b20 | при появлении в MP-02m | 1-Wire |
| ZMOD4410 | — | shared/sensors/zmod4410 | да | cyntron-dtv собирает из shared; порт zmod_port.h (main.h, i2c.h). |
| LD2412 | — | shared/sensors/ld2412 | да | cyntron-dtv собирает из shared; порт ld2412_port.h (main.h, usart.h, gpio.h). |
| eeprom_manager (слой поверх ee24) | Core/storage | логика в i2c.c / конфиг | опционально | При унификации формата конфига — общий слой в shared |

При появлении второго продукта, использующего устройство из столбца «только в одном продукте», соответствующий код переносится в shared с портовым заголовком; матрица обновляется. **Перенос файлов:** сначала копирование скриптом `scripts/move_to_shared.py` (см. doc/NEW_PRODUCT_PORTING.md), затем правки #include и сборки.
