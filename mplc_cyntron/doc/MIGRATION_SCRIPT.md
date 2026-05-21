# Скрипт для миграции файлов

## Выполнено вручную

### Modbus файлы
- ✅ `Core/modbus/inc/modbus_rtu_hw.h` - создан
- ✅ `Core/modbus/src/modbus_rtu_hw.c` - создан (начало с правильными includes)
- ⏳ Нужно добавить остальное содержимое из `Core/Src/ModbusRTU_hw.c` (строки 48-1444)

### Fast Modbus файлы
- ⏳ `Core/modbus/inc/fast_mb.h` - нужно скопировать
- ⏳ `Core/modbus/inc/fast_mb_port.h` - нужно скопировать  
- ⏳ `Core/modbus/src/fast_mb.c` - нужно скопировать и обновить includes
- ⏳ `Core/modbus/src/fast_mp_port.c` - нужно скопировать и обновить includes

## Команды для выполнения

### 1. Завершить modbus_rtu_hw.c
Добавить содержимое из `Core/Src/ModbusRTU_hw.c` начиная со строки 48 до конца в `Core/modbus/src/modbus_rtu_hw.c`

### 2. Скопировать Fast Modbus файлы
```powershell
# Заголовки
Copy-Item "Core\Inc\fast_mb.h" "Core\modbus\inc\"
Copy-Item "Core\Inc\fast_mb_port.h" "Core\modbus\inc\"

# Исходники (нужно обновить includes внутри)
Copy-Item "Core\Src\fast_mb.c" "Core\modbus\src\"
Copy-Item "Core\Src\fast_mp_port.c" "Core\modbus\src\"
```

### 3. Обновить includes в fast_mb.c
```c
// Было:
#include "ModbusRTU_hw.h"
// Станет:
#include "../inc/modbus_rtu_hw.h"
```

### 4. Обновить includes в fast_mp_port.c
```c
// Было:
#include "ModbusRTU_hw.h"
// Станет:
#include "../inc/modbus_rtu_hw.h"
```

## Примечание

Из-за большого размера файлов (1444 строки) полная автоматизация через инструменты затруднена. 
Рекомендуется:
1. Использовать IDE для массовой замены includes
2. Или выполнить миграцию вручную по частям
3. Или использовать sed/awk для автоматической замены
