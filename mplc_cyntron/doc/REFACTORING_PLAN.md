# План рефакторинга структуры проекта

## Новая структура папок

```
Core/
├── modules/              # Модули расширения (DO, DI, AI, AO, EN_METER)
│   ├── inc/
│   │   ├── module_interface.h      # Единый интерфейс модулей
│   │   ├── module_registry.h        # Реестр модулей
│   │   ├── module_factory.h         # Фабрика модулей
│   │   └── module_base.h            # Базовые функции
│   ├── src/
│   │   ├── module_registry.c
│   │   ├── module_factory.c
│   │   └── module_base.c
│   └── implementations/             # Реализации конкретных модулей
│       ├── module_do6di8.c/h
│       ├── module_do16.c/h
│       ├── module_do4di6.c/h
│       ├── module_do6.c/h
│       ├── module_di14.c/h
│       ├── module_ai12.c/h
│       ├── module_ao12.c/h
│       ├── module_ao6ai6.c/h
│       └── module_en_meter.c/h
│
├── modbus/               # Все что связано с Modbus
│   ├── inc/
│   │   ├── modbus_rtu_hw.h          # ModbusRTU_hw.h (переименован)
│   │   ├── fast_mb.h                # Fast Modbus
│   │   └── fast_mb_port.h
│   └── src/
│       ├── modbus_rtu_hw.c          # ModbusRTU_hw.c (переименован)
│       ├── fast_mb.c
│       └── fast_mp_port.c
│
├── adc/                  # Все что связано с ADC
│   ├── inc/
│   │   ├── adc_hal.h                # HAL ADC (adc.h)
│   │   ├── ads1220_c.h              # Внешний АЦП ADS1220
│   │   ├── ads1x20.h
│   │   ├── adc_worker_task_c.h      # Worker для ADC
│   │   ├── ai_channel_base_c.h      # Базовый канал AI
│   │   ├── ai_channel_c.h           # Каналы AI
│   │   ├── mux_c.h                  # Мультиплексор
│   │   └── KalmanFilter.h           # Фильтр Калмана
│   └── src/
│       ├── adc_hal.c                # adc.c
│       ├── ads1220_c.c
│       ├── ads1x20.c
│       ├── adc_worker_task_c.c
│       ├── ai_channel_base_c.c
│       ├── ai_channel_c.c
│       ├── mux_c.c
│       └── KalmanFilter.c
│
├── gpio/                 # GPIO и периферия
│   ├── inc/
│   │   ├── gpio_hal.h               # GPIO HAL (gpio.h)
│   │   ├── mcp23s17.h               # MCP23S17.h
│   │   ├── light_indicator.h
│   │   ├── button_handler.h
│   │   └── button.h
│   └── src/
│       ├── gpio_hal.c               # gpio.c
│       ├── mcp23s17.c               # MCP23S17.c
│       ├── light_indicator.c
│       ├── button_handler.c
│       └── button.c
│
├── storage/              # Хранение данных
│   ├── inc/
│   │   ├── eeprom_manager.h
│   │   ├── ee24.h
│   │   └── i2c.h
│   └── src/
│       ├── eeprom_manager.c
│       ├── ee24.c
│       └── i2c.c
│
├── system/               # Системные функции
│   ├── inc/
│   │   ├── system_timer.h
│   │   ├── system_init.h
│   │   └── debug_output.h
│   └── src/
│       ├── system_timer.c
│       ├── system_init.c
│       └── debug_output.c
│
├── hal/                  # HAL конфигурация
│   ├── inc/
│   │   ├── dma.h
│   │   ├── spi.h
│   │   ├── i2c_hal.h                 # i2c.h (HAL часть)
│   │   ├── usart.h
│   │   ├── tim.h
│   │   ├── adc_hal.h                 # adc.h (HAL часть)
│   │   └── wwdg.h
│   └── src/
│       ├── dma.c
│       ├── spi.c
│       ├── usart.c
│       ├── tim.c
│       └── wwdg.c
│
├── drivers/              # Драйверы устройств
│   ├── inc/
│   │   ├── atm90e32.h                # Энергосчетчик
│   │   └── ad53x4.h                  # ЦАП
│   └── src/
│       ├── atm90e32.c
│       └── ad53x4.c
│
├── apps/                 # Прикладной уровень
│   ├── inc/
│   │   └── app_c.h
│   └── src/
│       └── app_c.c
│
├── tables/               # Таблицы преобразования (уже есть)
│   ├── table_k_type.c/h
│   ├── table_pt100.c/h
│   ├── table_pt1000.c/h
│   └── table_ntc10k.c/h
│
├── sys/                  # Системные утилиты (уже есть)
│   ├── dassert.h
│   ├── irq_utils.hpp
│   └── logging.hpp
│
├── Startup/              # Startup код (уже есть)
│   └── startup_stm32f030c8tx.s
│
├── Inc/                  # Общие заголовки
│   ├── main.h
│   ├── stm32f0xx_hal_conf.h
│   ├── stm32f0xx_it.h
│   └── FreeRTOSConfig.h
│
└── Src/                  # Общие исходники
    ├── main.c
    ├── stm32f0xx_hal_msp.c
    ├── stm32f0xx_it.c
    ├── system_stm32f0xx.c
    ├── syscalls.c
    └── sysmem.c
```

## Преимущества новой структуры

1. **Логическая группировка** - файлы сгруппированы по функциональности
2. **Удобная навигация** - легко найти нужный код
3. **Модульность** - каждый компонент изолирован
4. **Расширяемость** - легко добавлять новые модули/драйверы
5. **Чистота** - разделение HAL, драйверов и прикладного кода

## План миграции

### Этап 1: Создание структуры папок и интерфейсов
1. Создать новые папки
2. Создать интерфейсы модулей (module_interface.h)
3. Создать реестр модулей (module_registry.h/c)

### Этап 2: Миграция Modbus
1. Переместить ModbusRTU_hw.c/h в modbus/
2. Переместить fast_mb.c/h в modbus/
3. Обновить includes

### Этап 3: Миграция ADC
1. Переместить ADC файлы в adc/
2. Обновить includes

### Этап 4: Миграция GPIO
1. Переместить GPIO файлы в gpio/
2. Обновить includes

### Этап 5: Миграция модулей
1. Создать реализации модулей в modules/implementations/
2. Реализовать интерфейсы для каждого модуля
3. Зарегистрировать в реестре

### Этап 6: Обновление основного кода
1. Обновить main.c для использования нового API
2. Удалить старые switch-case
3. Обновить все includes
