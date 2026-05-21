# Анализ архитектуры модулей и предложения по улучшению

## Текущее состояние

### Проблемы текущей архитектуры

1. **Множественные switch-case по типу модуля**
   - Код разбросан по функциям: `init_mezonine_module()`, `run_update_input_data()`, `run_udpate_output_data()`, `set_light_by_mode()`, `clear_all_output()`
   - Каждая функция содержит большой switch-case с обработкой всех типов модулей
   - Дублирование логики между похожими модулями (DO6DI8, DO4DI6)

2. **Отсутствие единого интерфейса**
   - Каждый модуль обрабатывается индивидуально
   - Нет абстракции для операций: init, read, write, update
   - Прямой доступ к hardware (MCP23x17, DAC, ADC) из разных мест

3. **Смешение ответственности**
   - Инициализация, чтение, запись, индикация - все в одном файле `gpio.c`
   - Логика модулей смешана с логикой GPIO и индикации

4. **Условная компиляция**
   - `#ifdef INCLUDE_XXX` разбросаны по всему коду
   - Усложняет поддержку и тестирование

5. **Отсутствие расширяемости**
   - Добавление нового модуля требует изменений в множестве мест
   - Нет механизма плагинов или регистрации модулей

## Предлагаемая архитектура

### 1. Единый интерфейс модуля (Module Interface)

```c
// Core/Inc/module_interface.h

/**
 * @brief Типы ресурсов модуля
 */
typedef enum {
    MODULE_RESOURCE_DI,    // Discrete Input
    MODULE_RESOURCE_DO,    // Discrete Output
    MODULE_RESOURCE_AI,    // Analog Input
    MODULE_RESOURCE_AO,    // Analog Output
} module_resource_type_t;

/**
 * @brief Структура описания ресурсов модуля
 */
typedef struct {
    module_resource_type_t type;
    uint8_t count;          // Количество ресурсов
    uint16_t base_address;  // Базовый адрес в Modbus
} module_resource_t;

/**
 * @brief Структура конфигурации модуля
 */
typedef struct {
    mp02_t module_type;
    const char* name;
    module_resource_t* resources;
    uint8_t resource_count;
    spi_cs_t spi_cs;        // CS для SPI устройств
    // Другие специфичные параметры
} module_config_t;

/**
 * @brief Интерфейс операций модуля
 */
typedef struct module_ops {
    // Инициализация модуля
    bool (*init)(const module_config_t* config);
    
    // Чтение входов (DI, AI)
    bool (*read_inputs)(uint16_t* buffer, uint16_t max_count);
    
    // Запись выходов (DO, AO)
    bool (*write_outputs)(const uint16_t* buffer, uint16_t count);
    
    // Обновление состояния (периодический вызов)
    void (*update)(void);
    
    // Получение количества ресурсов
    uint8_t (*get_resource_count)(module_resource_type_t type);
    
    // Получение базового адреса ресурса
    uint16_t (*get_resource_base)(module_resource_type_t type);
    
    // Обработка ошибок
    mp02_error (*get_error)(void);
    
    // Индикация (для светодиодов)
    void (*set_led)(uint8_t channel, bool state);
} module_ops_t;

/**
 * @brief Дескриптор модуля
 */
typedef struct {
    const module_config_t* config;
    const module_ops_t* ops;
    void* private_data;  // Приватные данные модуля
    bool initialized;
} module_handle_t;
```

### 2. Реализация модулей (Module Implementations)

Каждый модуль реализуется в отдельном файле:

```
Core/modules/
├── module_base.h          // Базовые функции и утилиты
├── module_base.c
├── module_do6di8.h       // Модуль 6DO8DI
├── module_do6di8.c
├── module_do16.h         // Модуль 16DO
├── module_do16.c
├── module_ai12.h         // Модуль 12AI
├── module_ai12.c
├── module_ao12.h         // Модуль 12AO
├── module_ao12.c
├── module_en_meter.h     // Модуль энергосчетчика
└── module_en_meter.c
```

Пример реализации модуля:

```c
// Core/modules/module_do6di8.c

#include "module_do6di8.h"
#include "MCP23S17.h"

// Приватные данные модуля
typedef struct {
    spi_cs_t spi_cs;
    bool initialized;
} do6di8_private_t;

static bool do6di8_init(const module_config_t* config) {
    do6di8_private_t* priv = (do6di8_private_t*)config->private_data;
    
    if (!MCP23x17_reset(priv->spi_cs, mcp23x17_A_WRITE_B_READ)) {
        return false;
    }
    
    // Дополнительная инициализация...
    priv->initialized = true;
    return true;
}

static bool do6di8_read_inputs(uint16_t* buffer, uint16_t max_count) {
    // Реализация чтения DI
    uint8_t in_read;
    MCP23x17_read(get_debug_poll_inp(), SPI_CS2, MCP23x17_REG_BANK_0_GPIOB);
    WaitForReleaseSPI(50);
    in_read = MCP23x17_read_data(get_debug_poll_inp());
    
    for (uint8_t i = 0; i < 8 && i < max_count; i++) {
        buffer[i] = (in_read & 0x01) ? BOOLEAN2INT : 0;
        in_read >>= 1;
    }
    return true;
}

static bool do6di8_write_outputs(const uint16_t* buffer, uint16_t count) {
    // Реализация записи DO
    // ...
    return true;
}

// Экспорт операций модуля
const module_ops_t do6di8_ops = {
    .init = do6di8_init,
    .read_inputs = do6di8_read_inputs,
    .write_outputs = do6di8_write_outputs,
    .update = NULL,
    .get_resource_count = do6di8_get_resource_count,
    .get_resource_base = do6di8_get_resource_base,
    .get_error = do6di8_get_error,
    .set_led = do6di8_set_led,
};
```

### 3. Реестр модулей (Module Registry)

```c
// Core/Inc/module_registry.h

/**
 * @brief Реестр всех доступных модулей
 */
typedef struct {
    module_handle_t* modules;
    uint8_t module_count;
    module_handle_t* active_module;
} module_registry_t;

/**
 * @brief Инициализация реестра модулей
 */
void module_registry_init(void);

/**
 * @brief Регистрация модуля
 */
bool module_register(mp02_t type, const module_config_t* config, const module_ops_t* ops);

/**
 * @brief Получение активного модуля
 */
module_handle_t* module_get_active(void);

/**
 * @brief Установка активного модуля по типу
 */
bool module_set_active(mp02_t type);

/**
 * @brief Вызов операции для активного модуля
 */
bool module_init_active(void);
bool module_read_inputs_active(uint16_t* buffer, uint16_t max_count);
bool module_write_outputs_active(const uint16_t* buffer, uint16_t count);
void module_update_active(void);
```

### 4. Фабрика модулей (Module Factory)

```c
// Core/Inc/module_factory.h

/**
 * @brief Создание модуля по типу
 */
module_handle_t* module_create(mp02_t type);

/**
 * @brief Удаление модуля
 */
void module_destroy(module_handle_t* module);
```

### 5. Упрощение основного кода

После рефакторинга основной код становится простым:

```c
// В main.c
void init_mezonine_module() {
    mp02_t module_type = get_type_module();
    module_set_active(module_type);
    module_init_active();
}

// В system_timer.c
void run_update_input_data() {
    module_handle_t* module = module_get_active();
    if (module && module->ops->read_inputs) {
        uint16_t buffer[32];
        module->ops->read_inputs(buffer, 32);
        // Обработка данных...
    }
}

// В gpio.c
void run_udpate_output_data() {
    module_handle_t* module = module_get_active();
    if (module && module->ops->write_outputs) {
        uint16_t buffer[32];
        // Подготовка данных...
        module->ops->write_outputs(buffer, count);
    }
}
```

## Преимущества новой архитектуры

1. **Модульность**
   - Каждый модуль в отдельном файле
   - Легко добавлять новые модули
   - Легко тестировать модули изолированно

2. **Единый интерфейс**
   - Все модули используют одинаковый API
   - Упрощает основной код
   - Упрощает поддержку

3. **Расширяемость**
   - Добавление нового модуля = создание нового файла с реализацией интерфейса
   - Не требует изменений в основном коде

4. **Читаемость**
   - Логика каждого модуля изолирована
   - Нет больших switch-case
   - Понятная структура кода

5. **Тестируемость**
   - Модули можно тестировать независимо
   - Легко создавать mock-объекты для тестирования

6. **Условная компиляция**
   - Модули компилируются только если включены
   - Централизованное управление через `#ifdef INCLUDE_XXX`

## План миграции

### Этап 1: Создание интерфейса
1. Создать `module_interface.h` с определениями интерфейсов
2. Создать `module_registry.h/c` для управления модулями

### Этап 2: Миграция простых модулей
1. Начать с простых модулей (DO16, DI14)
2. Вынести логику в отдельные файлы модулей
3. Зарегистрировать в реестре

### Этап 3: Миграция сложных модулей
1. Мигрировать модули с MCP23x17 (DO6DI8, DO4DI6)
2. Мигрировать модули с DAC (AO12, AO6AI6)
3. Мигрировать модули с AI (AI12)

### Этап 4: Миграция специальных модулей
1. Мигрировать EN_METER
2. Обновить основной код для использования нового API

### Этап 5: Очистка
1. Удалить старые switch-case
2. Удалить дублирующийся код
3. Обновить документацию

## Дополнительные улучшения

### 1. Конфигурация через структуры

Вместо hardcoded значений использовать структуры конфигурации:

```c
static const module_config_t do6di8_config = {
    .module_type = DO6DI8,
    .name = "6DO8DI",
    .resources = (module_resource_t[]){
        {MODULE_RESOURCE_DI, 8, 16},
        {MODULE_RESOURCE_DO, 6, 1},
    },
    .resource_count = 2,
    .spi_cs = SPI_CS2,
};
```

### 2. Обработка ошибок

Единый механизм обработки ошибок для всех модулей:

```c
typedef struct {
    mp02_error error_code;
    const char* error_message;
    uint32_t error_timestamp;
} module_error_t;
```

### 3. Логирование

Единый механизм логирования для модулей:

```c
#define MODULE_LOG(module, level, fmt, ...) \
    MP_DEBUG_PRINT(level, "[%s] " fmt, module->config->name, ##__VA_ARGS__)
```

### 4. Валидация

Проверка корректности конфигурации модуля:

```c
bool module_validate_config(const module_config_t* config);
```

## Заключение

Предложенная архитектура обеспечивает:
- **Модульность**: каждый модуль изолирован
- **Расширяемость**: легко добавлять новые модули
- **Поддерживаемость**: понятная структура кода
- **Тестируемость**: модули можно тестировать независимо
- **Читаемость**: нет больших switch-case, логика разделена

Это позволит значительно упростить поддержку и развитие проекта.
