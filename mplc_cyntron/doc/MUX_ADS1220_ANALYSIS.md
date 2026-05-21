# Сравнение логики MUX и ADS1220: stm32mp02 vs текущий проект

## 1. Даташит ADS1220 (ключевые моменты)

- **Запись в регистр конфигурации**: по даташиту TI, при записи в регистр конфигурации (WREG) может запускаться преобразование (в зависимости от режима). В коде обоих проектов есть комментарий: «Every write access to any configuration register also starts a conversion!».
- **Режим Single-Shot**: в `setup()` выставляется `ConvMode::SINGLE_SHOT` (регистр 1). Преобразование запускается по команде START (0x08) или при записи в регистр.
- **Результат**: 24 бита, знаковое (int32). Чтение по DRDY или по команде RDATA.
- **Порядок**: MUX → загрузка в MCP (OLATA/OLATB) → выдержка для установления аналоговой части → запуск преобразования (START) → ожидание DRDY → чтение результата.

## 2. Согласованность MUX и ADS1220 в stm32mp02 (reference)

### 2.1 adc_worker_task (reference)

- `start(config1, config2, single_conv)`:
  1. `mux_.setup(config1_, ch, is_pos)` — расчёт состояний портов MUX в памяти.
  2. `load_mux_mcp()` — запись в MCP23S17: CS2 OLATA, CS2 OLATB, CS3 OLATA; после каждой записи — read-back и проверка (с тем же контекстом: в reference для read используется `get_debug_poll_inp()` для вызова read и `get_debug_poll_outp()` для read_data — возможная несогласованность FIFO).
  3. `adc_.setup(config1_.adc_config)` — запись 4 регистров ADS1220; последняя запись по даташиту может запустить преобразование. Отдельный вызов `adc_.start()` в worker закомментирован.

- `second_start()`:
  1. `mux_.setup(config2_, ch, is_pos)`.
  2. `load_mux_mcp()`.
  3. `adc_.setup(config2_.adc_config)`.

### 2.2 Конечный автомат app (reference)

- **FIRST_MUX_SET**: при переходе *в* этот режим в `set_next_adc_mode(FIRST_MUX_SET, true)` вызывается `update_input()` (сдвиг канала), затем `ch_[i_]->start(pos_)` → worker.start() → MUX + load_mux_mcp + ads1220_setup. Таймер: `tickstart_ = HAL_GetTick()`. Затем переход в FIRST_MUX_SET_WAIT с `set_next_adc_mode(FIRST_MUX_SET_WAIT, true)` (снова сдвиг канала — в reference индекс сдвигается дважды за цикл, результат может относиться к другому каналу).
- **FIRST_MUX_SET_WAIT**:
  - Если `get_drdy()` — читают результат и перезаписывают `last_result` (преобразование, запущенное из setup, по сути отбрасывается).
  - После `MUX_WAIT_TIME` (55 ms) вызывают `adc_.start()` — запуск нужного преобразования после выдержки MUX.
  - Переход в FIRST_ADC_RES_WAIT.
- **FIRST_ADC_RES_WAIT**: ожидание DRDY, чтение, `keep_first_result()`, при двойном измерении — `second_start()`, переход в SECOND_MUX_SET_WAIT.
- **SECOND_MUX_SET_WAIT**: снова при DRDY читают (отбрасывают), после 55 ms — `adc_.start()`, переход в SECOND_ADC_RES_WAIT.
- **SECOND_ADC_RES_WAIT**: DRDY → чтение, `keep_second_result()`, `twice_compute()`, переход в FIRST_MUX_SET с `update_input()`.

Итог по reference: первое «полезное» преобразование для каждого шага — то, что запускается после 55 ms выдержки по `adc_.start()`, а не то, что могло стартовать из `ads1220_setup()`.

## 3. Текущий проект (MP-02m)

### 3.1 adc_worker_task_c.c

- Последовательность совпадает с reference: `mux_setup()` → `adc_worker_load_mux_mcp()` (CS2 OLATA/OLATB, CS3 OLATA + задержка 2 ms в конце) → `ads1220_setup()`. Ведётся верификация записи в MCP (для CS3 при несовпадении read-back только лог, без смены ошибки).
- Для верификации read-back используется один контекст `get_debug_poll_outp()` и один сохранённый результат (без двойного вызова read_data) — в отличие от reference, где использовались разные контексты для read/read_data.

### 3.2 app_c.c (конечный автомат)

- **FIRST_MUX_SET**: при переходе в этот режим в `app_set_next_adc_mode(..., FIRST_MUX_SET, true)` вызывается `app_update_input()` (сдвиг канала), затем `ai_channel_start(&happ->channels[happ->i], happ->pos)` — по смыслу как в reference. При *выходе* из FIRST_MUX_SET делается переход в FIRST_MUX_SET_WAIT с `fast_next_step = false` — сдвиг канала не выполняется, чтобы результат первого преобразования относился к тому же каналу (исправление относительно reference).
- **FIRST_MUX_SET_WAIT**: при DRDY — чтение (отбрасывание преобразования от setup); после `MUX_SETTLE_TIME_MS` (75 ms) — `ads1220_start()`, переход в FIRST_ADC_RES_WAIT без сдвига канала.
- **FIRST_ADC_RES_WAIT**: по DRDY — чтение, `keep_first_result()`, при двойном измерении — `adc_worker_second_start()`, переход в SECOND_MUX_SET_WAIT без сдвига.
- **SECOND_MUX_SET_WAIT**: при DRDY — чтение; после 75 ms — `ads1220_start()`, переход в SECOND_ADC_RES_WAIT без сдвига.
- **SECOND_ADC_RES_WAIT**: по DRDY — чтение, `keep_second_result()`, `ai_channel_twice_compute(&happ->channels[happ->i], happ->pos)`, переход в FIRST_MUX_SET с `fast_next_step = true` (сдвиг канала один раз за цикл).

Итог: первый и второй результаты привязаны к одному и тому же каналу; сдвиг индекса канала выполняется только при переходе обратно в FIRST_MUX_SET.

### 3.3 MUX (mux_c.c) vs reference (mux.cpp)

- Структура портов совпадает: ext1_porta/ext1_portb (CS2), ext2_porta (CS3); биты sw_adc, mux1_en..mux4_en, mux_a0/a1, in1p..in6p/n, ain1/ain2, midpoint — те же.
- reset: все «enable» мультиплексоров в 1 (выключены), затем в `mux_setup` нужные сбрасываются в 0 для выбора канала. В текущем проекте это согласовано с reference.
- Порядок вызовов в setup: reset → adc_sw, midpoint → input divider (pos/neg) → current (pos/neg) → select_ch_pos, select_ch_neg — такой же, как в reference.

### 3.4 ADS1220 (ads1220_c.c) vs reference (ADS1220.cpp)

- Регистры 0–3 и команды (RESET, START, WREG, RREG) совпадают.
- `ads1220_setup()` записывает все 4 регистра (как в reference); комментарий про запуск преобразования при записи сохранён.
- Чтение результата: 3 байта, знаковое расширение до int32 — одинаково.

## 4. Отличия и исправления в текущем проекте

| Аспект | stm32mp02 | Текущий проект |
|--------|-----------|-----------------|
| Сдвиг канала | Дважды за цикл (при переходе в FIRST_MUX_SET и в FIRST_MUX_SET_WAIT) | Один раз за цикл (только при переходе в FIRST_MUX_SET) — результат и compute привязаны к одному каналу |
| Выдержка MUX | 55 ms (MUX_WAIT_TIME) | 75 ms (MUX_SETTLE_TIME_MS) + 2 ms после load_mux_mcp |
| Верификация MCP CS3 | set_error при несовпадении read-back | Только лог, без set_error (из-за ненадёжного read-back по CS3) |
| Контекст read при верификации MCP | inp для read, outp для read_data | Единый outp для read и read_data при верификации записи в OLAT |

## 5. Согласование с эталоном stm32mp02-1.2.7_6AI6AO (канал 4)

В текущем проекте загрузка MUX приведена к эталонному коду **stm32mp02-1.2.7_6AI6AO**:

- **Порядок записи:** как в эталоне — сначала **CS2** (OLATA, OLATB) с верификацией, затем при модуле 12AI **CS3** (OLATA) с чтением обратно и при необходимости логом (без установки ошибки при сбое read-back CS3).
- **Верификация:** при read-back используется контекст **get_debug_poll_inp()** для `MCP23x17_read()` и **get_debug_poll_outp()** для `MCP23x17_read_data()`, после чтения вызывается **WaitForReleaseSPI(50)** — как в эталоне.
- **Режим SPI для MCP:** в текущем проекте в `MCP23x17_write()` (режим POLL) перед транзакцией выставляется режим SPI для MCP (Mode 0), чтобы после обмена с ADS1220 (Mode 1) запись в MCP проходила корректно; в эталоне 6AI6AO этой установки в POLL нет.

## 6. Рекомендации

- Оставить текущую логику: один сдвиг канала за цикл, первый результат — от преобразования, запущенного после 75 ms выдержки (как в reference по смыслу, но с исправленной привязкой к каналу).
- Порядок записи MCP: **CS2 → CS3** (как в эталоне stm32mp02-1.2.7_6AI6AO).
- При отрицательном res1 и положительном res2 в формуле сопротивления уже добавлен обход (использование res2/|res1|). Если проблема сохранится — проверять аппаратуру CS3 и трассы MUX (особенно ext2_porta).
- При необходимости можно уменьшить `MUX_SETTLE_TIME_MS` до 55 ms для сближения с reference, если 75 ms избыточны по времени цикла.
