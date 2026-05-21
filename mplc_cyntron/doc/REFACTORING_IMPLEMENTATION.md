# Детальный план реализации рефакторинга

## Структура папок создана

Созданы следующие папки:
- `Core/modules/` - модули расширения
- `Core/modbus/` - Modbus протокол
- `Core/adc/` - ADC и аналоговые каналы
- `Core/gpio/` - GPIO и периферия
- `Core/storage/` - EEPROM и хранение
- `Core/system/` - системные функции
- `Core/hal/` - HAL конфигурация
- `Core/drivers/` - драйверы устройств

## Созданные файлы

### Интерфейсы модулей
- ✅ `Core/modules/inc/module_interface.h` - единый интерфейс модулей
- ✅ `Core/modules/inc/module_registry.h` - реестр модулей
- ✅ `Core/modules/src/module_registry.c` - реализация реестра

## План миграции файлов

### Этап 1: Modbus (приоритет: высокий)

**Файлы для перемещения:**
```
Core/Inc/ModbusRTU_hw.h          → Core/modbus/inc/modbus_rtu_hw.h
Core/Src/ModbusRTU_hw.c          → Core/modbus/src/modbus_rtu_hw.c
Core/Inc/fast_mb.h               → Core/modbus/inc/fast_mb.h
Core/Inc/fast_mb_port.h          → Core/modbus/inc/fast_mb_port.h
Core/Src/fast_mb.c               → Core/modbus/src/fast_mb.c
Core/Src/fast_mp_port.c          → Core/modbus/src/fast_mp_port.c
```

**Файлы, которые нужно обновить (includes):**
- `Core/Src/main.c`
- `Core/Src/light_indicator.c`
- `Core/Src/system_timer.c`
- `Core/Src/i2c.c`
- `Core/Src/usart.c`
- `Core/Src/tim.c`
- `Core/Src/stm32f0xx_it.c`

### Этап 2: ADC (приоритет: высокий)

**Файлы для перемещения:**
```
Core/Inc/adc.h                   → Core/adc/inc/adc_hal.h
Core/Src/adc.c                   → Core/adc/src/adc_hal.c
Core/Inc/ads1220_c.h            → Core/adc/inc/ads1220_c.h
Core/Src/ads1220_c.c            → Core/adc/src/ads1220_c.c
Core/Inc/ads1x20.h              → Core/adc/inc/ads1x20.h
Core/Src/ads1x20.c               → Core/adc/src/ads1x20.c
Core/Inc/adc_worker_task_c.h    → Core/adc/inc/adc_worker_task_c.h
Core/Src/adc_worker_task_c.c    → Core/adc/src/adc_worker_task_c.c
Core/Inc/ai_channel_base_c.h    → Core/adc/inc/ai_channel_base_c.h
Core/Src/ai_channel_base_c.c   → Core/adc/src/ai_channel_base_c.c
Core/Inc/ai_channel_c.h         → Core/adc/inc/ai_channel_c.h
Core/Src/ai_channel_c.c         → Core/adc/src/ai_channel_c.c
Core/Inc/mux_c.h                 → Core/adc/inc/mux_c.h
Core/Src/mux_c.c                 → Core/adc/src/mux_c.c
Core/Inc/KalmanFilter.h         → Core/adc/inc/KalmanFilter.h
Core/Src/KalmanFilter.c         → Core/adc/src/KalmanFilter.c
```

### Этап 3: GPIO (приоритет: средний)

**Файлы для перемещения:**
```
Core/Inc/gpio.h                  → Core/gpio/inc/gpio_hal.h
Core/Src/gpio.c                  → Core/gpio/src/gpio_hal.c
Core/Inc/MCP23S17.h              → Core/gpio/inc/mcp23s17.h
Core/Src/MCP23S17.c              → Core/gpio/src/mcp23s17.c
Core/Inc/light_indicator.h       → Core/gpio/inc/light_indicator.h
Core/Src/light_indicator.c       → Core/gpio/src/light_indicator.c
Core/Inc/button_handler.h        → Core/gpio/inc/button_handler.h
Core/Src/button_handler.c        → Core/gpio/src/button_handler.c
Core/Inc/button.h                 → Core/gpio/inc/button.h
Core/Src/button.c                 → Core/gpio/src/button.c
```

### Этап 4: Storage (приоритет: средний)

**Файлы для перемещения:**
```
Core/Inc/eeprom_manager.h        → Core/storage/inc/eeprom_manager.h
Core/Src/eeprom_manager.c        → Core/storage/src/eeprom_manager.c
Core/Inc/ee24.h                   → Core/storage/inc/ee24.h
Core/Inc/i2c.h                    → Core/storage/inc/i2c.h
Core/Src/i2c.c                    → Core/storage/src/i2c.c
```

### Этап 5: System (приоритет: средний)

**Файлы для перемещения:**
```
Core/Inc/system_timer.h          → Core/system/inc/system_timer.h
Core/Src/system_timer.c           → Core/system/src/system_timer.c
Core/Inc/debug_output.h          → Core/system/inc/debug_output.h
Core/Src/debug_output.c          → Core/system/src/debug_output.c
```

### Этап 6: HAL (приоритет: низкий)

**Файлы для перемещения:**
```
Core/Inc/dma.h                    → Core/hal/inc/dma.h
Core/Src/dma.c                    → Core/hal/src/dma.c
Core/Inc/spi.h                    → Core/hal/inc/spi.h
Core/Src/spi.c                    → Core/hal/src/spi.c
Core/Inc/usart.h                  → Core/hal/inc/usart.h
Core/Src/usart.c                  → Core/hal/src/usart.c
Core/Inc/tim.h                    → Core/hal/inc/tim.h
Core/Src/tim.c                    → Core/hal/src/tim.c
Core/Inc/wwdg.h                   → Core/hal/inc/wwdg.h
Core/Src/wwdg.c                   → Core/hal/src/wwdg.c
```

### Этап 7: Drivers (приоритет: низкий)

**Файлы для перемещения:**
```
Core/Inc/atm90e32.h               → Core/drivers/inc/atm90e32.h
Core/Src/atm90e32.c               → Core/drivers/src/atm90e32.c
Core/Inc/ad53x4.h                 → Core/drivers/inc/ad53x4.h
Core/Src/ad53x4.c                 → Core/drivers/src/ad53x4.c
```

### Этап 8: Apps (приоритет: низкий)

**Файлы для перемещения:**
```
Core/Inc/app_c.h                  → Core/apps/inc/app_c.h
Core/Src/app_c.c                  → Core/apps/src/app_c.c
```

## Пример реализации модуля DO6DI8

Создать файлы:
- `Core/modules/implementations/module_do6di8.h`
- `Core/modules/implementations/module_do6di8.c`

Это будет пример для остальных модулей.

## Обновление includes

После перемещения файлов нужно обновить все `#include` директивы в проекте.

## Следующие шаги

1. Переместить файлы Modbus
2. Обновить includes для Modbus
3. Создать пример реализации модуля DO6DI8
4. Продолжить миграцию остальных файлов
