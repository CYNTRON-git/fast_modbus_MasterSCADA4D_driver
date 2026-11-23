# Изолированная проверка на .135 — подготовлено, не запускалось

Выполняется совместно с агентом СА‑02М только после нового явного окна. Текущий HOLD распространяется и на этот сценарий. Проект платформы 147 с адресами исключительно `tcp://127.0.0.1:15503..15505` находится в отдельном ARM-loopback экспорте. Аппаратный проект с `/dev/ttyS*` здесь не используется.

Дополнение 23.11 после QEMU run06: ELF NEEDED-closure недостаточна для API-теста. В сохранённом минимальном наборе отсутствует mplc_fcgi.so, повторяются ошибки Lua; 554 канала не проверены. До нового native пакета требуется подтвердить полный состав штатных динамических модулей и runtime data **той же сборки live34421**, проверить его в эмуляции и закрепить hashes. Данные из случайного vendor-архива другой сборки не подмешивать; лицензии и production project не копировать. Приведённый ниже рецепт остаётся проектом процедуры, а не разрешением или готовым к исполнению пакетом. Порог памяти и лимиты по-прежнему не менять самостоятельно.

По явному согласию пользователя недостающие штатные ELF получены и проверены в run07 (подробности в HIL_READINESS_20261122.md). Полный статический граф36 ELF/6845 сильных ссылок проходит; 23.11 09:56МСК ARM MasterPLC/API под QEMU успешно проверил554 канала/19Fault/15источников событий и обычный Carel. Эмуляция завершена, hardware остаётся непроверенным. Перед native этапом требуется отдельное согласование и формирование нового пакета из exact closure run07; прежний memory/ownership HOLD не снят. В неподтверждённый native runtime ничего не устанавливать.

1. Перед окном повторить актуальный `restoration_baseline.py`: состояния 13 служб (включая nginx и фазовый энергоучёт), PID/перезапуски, хеши MQTT/координатора/политики стеков/конфигурации фазового учёта и двух существующих MPLC-плагинов. Проверить фактические дочерние PID MPLC: unit active/exited с MainPID=0 не означает остановленный runtime. Начальный снимок 11 служб — 22.11.2026 16:13:28 МСК; он не заменяет новый baseline. Динамические показания энергии не откатываются и не сравниваются как неизменяемый файл.
2. На Windows собрать новый пакет `/var/tmp/fmb-no-uart-<ID окна>`: проверенный ARM-loopback `controller.zip`, native audit/nodes/manifest, `no_uart_runner.py`, `offline_mock.py`, stdlib-клиент `tools/ms4d_api.py`, кандидат `.so` SHA `1145afc8d7aa7bb6881a7ffee834a6e24c6969e348c08f5e6dd04371ee29959d`. На плате создать только этот новый частный каталог. В `runtime/` скопировать обычные ELF `/opt/mplc4/*.so*`, `/opt/mplc4/mplc` и `/opt/mplc4/nginx/sbin/nginx`; существующий FastModbus исключить и поместить кандидат только в частную копию. Не копировать лицензии, проект, конфигурации и секреты production. Записать SHA всех скопированных входов в `bundle-manifest.json`; извлечение ZIP разрешает только относительные `cfg/` без `..`/symlink. Каталог `run/` содержит конфигурацию отдельного тестового projectId; `web/` — локальный nginx API 8047 → FastCGI31202, PID и temporary paths только внутри пакета.
3. Владелец входов частного пакета — root, без добавления групп/пользователей. `runtime`, скрипты и `web/conf` доступны только для чтения/исполнения; существующий `nobody` владеет только `output`, `run`, `web/logs`, `web/temp`, `web/htdocs`. Никаких записей в `/opt/mplc4`, `/etc`, production cfg и никаких leases/остановок MQTT/Rules/PLC.
4. Проверить MemAvailable ≥128 MiB. В baseline доступно примерно 118 MiB, поэтому запуск сейчас запрещён также ресурсным preflight. Не освобождать память остановкой служб/сбросом кэша; новое состояние и бюджет согласуются в окне. Ограничения своего unit: 64 MiB максимум, high48 MiB, swap0, CPU20%, 48 tasks, runtime180s; при превышении тест считается неуспешным, лимит самостоятельно не повышается.
5. Сначала запуск `--stage load` в приведённом unit. Он делает `dlopen(RTLD_NOW|RTLD_LOCAL)` и проверяет экспорт InitAddin, но не вызывает его: результат LOAD_PASS означает только загрузку, не успешную работу API. Затем, только если агент принял результат и ресурсы, отдельный новый unit `--stage native`: настоящий MasterPLC вызывает API драйвера и опрашивает 16 локальных имитаторов, проверяет 554 значения, 15 источников событий и обычный Carel. Physical RS-485 остаётся недоступным.

Конкретные свойства transient unit (ID окна и проверенный частный путь подставляются перед согласованием):

`no-uart/payload.zip` собирается на Windows через `prepare_no_uart.py`; его SHA — в `no-uart/receipt.json`. Пакет уже содержит отдельный ARM-loopback config, карту узлов, имитаторы, API-клиент и кандидат. `stage_no_uart.py` подготовлен, **не запускался**: только в согласованном окне он проверяет память/место, извлекает проверенный ZIP в новый каталог и копирует vendor ELF. Он не запускает ничего и не меняет службы. Пример команды после согласования: `python3 -I stage_no_uart.py --payload payload.zip --sha256 <SHA из receipt> --bundle /var/tmp/fmb-no-uart-WINDOW --confirmed-window WINDOW`. Повторное использование существующего каталога запрещено. Если проверка владельца, пути или vendor ELF не проходит, сохранить ошибку и согласовать исправление, не обходить проверку.

Снимки до/после сохранять отдельными именами: `restoration_baseline.py --output <новый файл>`; сравнение — `compare_baseline.py <до> <после> --output <новый отчёт>`. Оно проверяет также фактические дочерние PID runtime и не выполняет восстановительных команд. Нужна отдельная приёмка координатором свежести MQTT-кэша.

```sh
systemd-run --unit=fmb-no-uart-WINDOW-STAGE --wait --pipe --collect \
  -p Type=exec -p User=nobody -p Group=nogroup \
  -p NoNewPrivileges=yes -p 'CapabilityBoundingSet=' \
  -p PrivateDevices=yes -p DevicePolicy=closed -p PrivateNetwork=yes -p PrivateIPC=yes \
  -p ProtectSystem=strict -p ProtectHome=yes -p ProtectKernelTunables=yes \
  -p ProtectKernelModules=yes -p ProtectControlGroups=yes -p RestrictSUIDSGID=yes \
  -p InaccessiblePaths=/opt/mplc4 \
  -p 'RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6' \
  -p 'TemporaryFileSystem=/run:rw /tmp:rw' \
  -p ReadWritePaths=/var/tmp/fmb-no-uart-WINDOW \
  -p WorkingDirectory=/var/tmp/fmb-no-uart-WINDOW/runtime \
  -p MemoryMax=64M -p MemoryHigh=48M -p MemorySwapMax=0 -p CPUQuota=20% \
  -p TasksMax=48 -p RuntimeMaxSec=180 -p TimeoutStopSec=10 -p KillMode=control-group \
  -p OOMScoreAdjust=1000 -p Restart=no \
  -p Environment=FMB_REVIEWED_SANDBOX=1 \
  -p Environment=LD_LIBRARY_PATH=/var/tmp/fmb-no-uart-WINDOW/runtime \
  /usr/bin/python3 -E -s /var/tmp/fmb-no-uart-WINDOW/no_uart_runner.py \
  --bundle /var/tmp/fmb-no-uart-WINDOW --stage STAGE
```

`python3 -E -s` исключает настройки Python из окружения и пользовательские пакеты, сохраняя импорт проверенных соседних модулей. Входные скрипты принадлежат root и доступны только для чтения; nobody владеет только writable-папками.

Приёмка изоляции — фактические systemd properties, внутри процесса только интерфейс lo, NoNewPrivs=1/CapEff=0, нет ttyS/COM/RS-485/gpio/i2c/spi/mem в /dev. Runner прекращается, если проверка не проходит. Локальный API привязан только к loopback частной сети; UNIX-сокеты production закрыты частным /run. Ни один файл с аппаратным профилем не входит в тестовый bundle.

После любого исхода остановить только собственный transient unit, подтвердить cgroup пустым и отсутствие своих процессов. Сохранить result JSON, unit Result/ExecMainStatus/MemoryPeak/OOM, stdout/stderr, SHA. Повторить baseline production: состояния active/enabled, PID/NRestarts, хеши конфигураций/плагинов должны совпасть; проверить свежесть кэша с координатором. Обнаруженное отклонение означает провал приёмки, а не автоматический restart production. Тестовые файлы сохраняются как evidence; ничего рекурсивно не удаляется. Аппаратные события считаются непроверенными даже после успешного no-UART этапа.
