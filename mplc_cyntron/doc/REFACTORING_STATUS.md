# Статус рефакторинга проекта

## ✅ Выполнено

### 1. Создана структура папок
```
Core/
├── modules/              ✅ Создано
│   ├── inc/              ✅
│   ├── src/              ✅
│   └── implementations/  ✅
├── modbus/               ✅ Создано
│   ├── inc/              ✅
│   └── src/              ✅
├── adc/                  ✅ Создано
│   ├── inc/              ✅
│   └── src/              ✅
├── gpio/                 ✅ Создано
│   ├── inc/              ✅
│   └── src/              ✅
├── storage/              ✅ Создано
│   ├── inc/              ✅
│   └── src/              ✅
├── system/               ✅ Создано
│   ├── inc/              ✅
│   └── src/              ✅
├── hal/                  ✅ Создано
│   ├── inc/              ✅
│   └── src/              ✅
└── drivers/              ✅ Создано
    ├── inc/              ✅
    └── src/              ✅
```

### 2. Созданы интерфейсы модулей
- ✅ `Core/modules/inc/module_interface.h` - единый интерфейс для всех модулей
- ✅ `Core/modules/inc/module_registry.h` - реестр модулей
- ✅ `Core/modules/src/module_registry.c` - реализация реестра

### 3. Создан пример реализации модуля
- ✅ `Core/modules/implementations/module_do6di8.h` - заголовок модуля DO6DI8
- ✅ `Core/modules/implementations/module_do6di8.c` - реализация модуля DO6DI8

### 4. Начата миграция Modbus
- ✅ `Core/modbus/inc/modbus_rtu_hw.h` - создан (копия ModbusRTU_hw.h)

## 📋 Что нужно сделать дальше

### Приоритет 1: Завершить миграцию Modbus

**Файлы для копирования:**
1. `Core/Src/ModbusRTU_hw.c` → `Core/modbus/src/modbus_rtu_hw.c`
2. `Core/Inc/fast_mb.h` → `Core/modbus/inc/fast_mb.h`
3. `Core/Inc/fast_mb_port.h` → `Core/modbus/inc/fast_mb_port.h`
4. `Core/Src/fast_mb.c` → `Core/modbus/src/fast_mb.c`
5. `Core/Src/fast_mp_port.c` → `Core/modbus/src/fast_mp_port.c`

**Обновить includes в:**
- `Core/modbus/src/modbus_rtu_hw.c` - изменить `#include "ModbusRTU_hw.h"` на `#include "modbus_rtu_hw.h"`
- `Core/Src/main.c` - изменить `#include "ModbusRTU_hw.h"` на `#include "modbus/modbus_rtu_hw.h"`
- `Core/Src/light_indicator.c`
- `Core/Src/system_timer.c`
- `Core/Src/i2c.c`
- `Core/Src/usart.c`
- `Core/Src/tim.c`
- `Core/Src/stm32f0xx_it.c`

### Приоритет 2: Миграция ADC

**Файлы для копирования:**
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

### Приоритет 3: Миграция GPIO

**Файлы для копирования:**
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

### Приоритет 4: Создать остальные модули

По аналогии с `module_do6di8.c/h` создать:
- `module_do16.c/h`
- `module_do4di6.c/h`
- `module_do6.c/h`
- `module_di14.c/h`
- `module_ai12.c/h`
- `module_ao12.c/h`
- `module_ao6ai6.c/h`
- `module_en_meter.c/h`

### Приоритет 5: Обновить основной код

1. В `main.c` заменить вызовы `init_mezonine_module()` на использование реестра модулей
2. В `gpio.c` заменить switch-case на вызовы через интерфейс модулей
3. Обновить все includes во всех файлах проекта

## 📝 Рекомендации

1. **Постепенная миграция**: Перемещать файлы по одному модулю за раз
2. **Тестирование**: После каждого этапа проверять компиляцию
3. **Резервные копии**: Сохранить рабочую версию перед началом миграции
4. **Документация**: Обновлять документацию по мере миграции

## 🔧 Инструменты

Для массового перемещения файлов можно использовать:
- PowerShell скрипты
- Python скрипты
- Или вручную через IDE

## 📊 Прогресс

- ✅ Структура папок: 100%
- ✅ Интерфейсы модулей: 100%
- ✅ Пример модуля: 100%
- ⏳ Миграция файлов: 5%
- ⏳ Обновление includes: 0%
- ⏳ Реализация всех модулей: 10% (1 из 9)
