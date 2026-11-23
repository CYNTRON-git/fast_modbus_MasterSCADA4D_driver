# Готовность к испытаниям, 22.11.2026

Подготовлен кандидат драйвера и отдельные проекты MasterSCADA. На контроллер 192.168.1.135 они не установлены. ARM MasterPLC/API прошёл run07 под QEMU; работа физических событий, UART и native API на контроллере остаётся непроверенной. Координатор SA сохраняет управление стендом; текущие его обновления не считаются передачей аппаратного окна.

Дополнение 23.11, 00:08 МСК: эмулированная ARM-загрузка кандидата с сохранёнными библиотеками live34421 **PASS**. Выполнены dlopen/dlsym/dlclose; InitAddin не вызывался. Это закрывает этап загрузчика, но не API/HIL.

## Проверенная подготовка

- Полный проект: 3 линии, 12 модулей МР, 2 ДТВ, СЭ‑02М и Carel c.pCOmini; 554 входных канала. Carel работает обычным RTU на общей линии, DisableEvents=True. Все команды выходов запрещены.
- Windows: core 15 групп / 200216 assertions, native-регрессия 13/13, полный проект — 554 значения/качества и 15 источников событий, обычный Carel. RTU-only вариант проверен отдельно: изменения всех 16 имитаторов получены, FC70=0. Сборщик прочитал 631 вход/диагностику в обоих вариантах.
- ARM run03: vendor GCC 13.3.0 / SDK35699, compile/link/ELF и host ASan/UBSan PASS. 17 общих исходных файлов/заголовков совпадают с Windows. ARM SHA256 `1145afc8d7aa7bb6881a7ffee834a6e24c6969e348c08f5e6dd04371ee29959d`; Windows DLL `3436cab7cdf4eee1ee041e7bc052e4532f143f1a56fd85997c4dd416c11c43e7`.
- Статическая проверка сохранённых ELF live34421: полный граф зависимостей, 163 сильных и 2 слабых символа разрешены, известных конфликтов версий нет; сверка с GNU readelf на 33 файлах. Это не доказательство выполнения API/ABI.

Рабочие базы Сколково не менялись. Проекты создавались штатным C# API скрытой IDE; сохранены native ZIP проекта/библиотеки/контроллера, проверки повторного открытия и физической целостности Firebird. Не копировать живые FDB файловыми командами.

## Первый аппаратный проект

Чтобы первое окно COM4 не затрагивало остальные линии, добавлен `-Canary` в native builder и `--canary` в аудит. Созданы два отдельных проекта: только `/dev/ttyS5`, 115200, MR12AI@12, SN235561385, 38 входов. COM2/COM3 и остальные типы удалены из профиля до создания узлов. После native-экспорта варианты отличаются ровно одним DisableEvents.

| Вариант | ProjectId | Config SHA256 |
|---|---|---|
| RTU | `9d8c09fb-6643-42e7-8cbe-899164ff3b5c` | `24290270939b437e9324ab0ddef305595b07ad77fcfb1ec17d88e8cf84d32ec1` |
| Events | `cf081e86-98ab-41b5-8c0b-5ef02622cf8d` | `70543c1e4245852843e0a0b25883dcf2736a6ea914c90046a1f522e6f287b4f5` |

Оба прошли native compile/reopen/audit и Firebird checks. Эти два экспорта не исполнялись на .135; локальные тесты полной конфигурации подтверждают карту, но не заменяют аппаратное испытание.

Порядок: сначала отдельно согласованная no-UART проверка загрузки/API с новым baseline и MemAvailable ≥131072 KiB; затем отдельный COM4 lease, RTU60s → events120s. В сообщении координатора замер 20:26:55 UTC — 112244 KiB; порог не выполнен. Ради памяти службы не останавливать. FC70 меняет подписки даже при AllowWrites=False, поэтому восстановление подписок согласуется отдельно. Без наблюдаемого изменения через событие результат INCONCLUSIVE. Root выполняет штатное восстановление проекта/плагинов/владения портом; энергию и offsets не откатывать.

## QEMU: отдельная проверка загрузки ARM

Цель — выполнить `dlopen(RTLD_NOW|RTLD_LOCAL)`, найти экспорт InitAddin, затем `dlclose` в ARM-процессе с сохранённым live loader и библиотеками. InitAddin не вызывается. Даже PASS означает только эмулированную загрузку, не работу API MasterPLC и не опрос железа.

Первый inventory обнаружил отсутствие qemu-arm в закреплённом image9af56be3. Private run01 20:28:20–20:28:38 UTC остановился на установке: недоступны archive.ubuntu.com/security.ubuntu.com по HTTPS, apt-plan exit100. Пакеты не устанавливались, новый image не создавался, ARM-probe не запускался. Container ExitCode1/Pid0/OOMfalse, собственных running containers нет, слот возвращён.

Подготовлен offline run02: официальный qemu-user `1:6.2+dfsg-2ubuntu6.31`, прочие зависимости уже присутствуют. Windows проверил три подписи InRelease штатным gpgv; keyring SHA совпадает с ранее наблюдённым в закреплённом base. Сохраняется цепочка подписи → SHA256 Packages → SHA256 deb, как описано в [apt-secure Ubuntu](https://manpages.ubuntu.com/manpages/jammy/man8/apt-secure.8.html). Изменять ключи или отключать проверку нельзя. При установке повторяются проверки дат/сроков/amd64, базовых версий и точного плана без обновлений/удалений. Только новый private image/layer, обе фазы network none, SDK RO. Итог берётся из actual receipt, не из успешной подготовки пакета.

Run02 20:40:38.694–20:40:39.924 UTC: подписи, сроки, архитектура, SHA-цепочка и базовый набор прошли. APT simulate подтвердил только один новый пакет, без обновлений/удалений. Actual `apt --no-download` завершился до dpkg: `Internal Error, Pathname to install is not absolute 'qemu-user_1%3a6.2+dfsg-2ubuntu6.31_amd64.deb'`. ARM не запускался. Container Pid0/ExitCode1/OOMfalse; wrapper exit1; supervisor55987/wrapper55988/job55990 отсутствуют. Слот возвращён немедленно.

Run03 выполнен в новом окне 22.11 21:03:18.575–21:03:24.948 UTC (23.11 00:03 МСК). После прежней проверки подписи/хешей и apt simulate `dpkg --install` проверенного абсолютного пути успешно установил только qemu-user. Никаких force/ignore-depends, новых зависимостей или сети; before/after версии совпали. Новый частный образ `sha256:d347470e70da39e84211f9144482a3577fa354577a3094b88f8009dffc887bbe`, слой 118702080 байт. Общий base не менялся.

Компиляция ARM C-пробы прошла (SHA `c77e4526bdc6f817c735f2aad5d183e3779fe59e9ff8d46d656716b3473d6430`), вызов qemu-arm вернул1 без stdout/stderr. Install container exit0, test exit1; оба Pid0/OOMfalse, wrapper exit1, supervisor56431/wrapper56432/job56434 отсутствуют. Слот немедленно возвращён.

Разбор выявил дефект подготовки исполняемого входа: фактический SFTP mode загрузчика **0444**, SHA `18b9c26aa2cd0ffd13a4d68113ad8bdccfbdf11033ee5e9548bd88fe80620879`. В [QEMU6.2 prepare_binprm](https://github.com/qemu/qemu/blob/v6.2.0/linux-user/linuxload.c#L31-L47) такой файл отвергается до исполнения. Диагностика печатается перед `_exit`, что согласуется с пустым буферизованным stdout. Это дефект тестовой упаковки; работа драйвера ещё не доказана.

Run04 выполнен в отдельном согласованном окне: 22.11 21:08:12.180–21:08:12.933 UTC / 23.11 00:08 МСК, результат **EMULATED_ARM_LOAD_PASS_API_AND_HARDWARE_UNVERIFIED**. Прежняя C-проба без пересборки; копии probe/loader только в новом output с mode0555 и проверкой прежних SHA, frozen input остался0444. SDK не монтировался, установка не повторялась, network none/1CPU/512MiB/общий120s. ARM dlopen с RTLD_NOW, dlsym InitAddin и dlclose вернули успех; armExitCode0. InitAddin не вызывался, .135 не использовалась.

Контейнер Pid0/ExitCode0/OOMfalse/Runningfalse, wrapper56572 exit0; supervisor56571/wrapper56572/job56574 отсутствуют по ps. Слот возвращён сразу после результата. Все stdout/stderr, QEMU syscall trace и actual receipts сохранены. Точный candidate SHA не изменился. Исходный общий `run_arm_loader.py` также исправлен для дальнейшей подготовки: исполняемый loader копируется в private output; старые неизменяемые пакеты оставлены как evidence.

Передача завершена через SFTP без новых runtime jobs: `/Users/cyntron/cyntron-build/artifacts/fast-modbus-qemu-20261122-run04/HANDOFF.md`. Рядом `evidence-manifest.json`, SHA256 `bc9d426367b160868a1f996654998eaf808aef87d3d88c514143abab008bed86`: 15 файлов (live-input manifest, install receipts, actual container/wrapper receipts, stdout/trace и инструкция с точной рабочей командой). Все файлы и сам manifest побайтно сверены между Windows и Mac. Временный сбой auto-review403 устранён повтором того же разрешённого запроса; существующие evidence-файлы не перезаписывались.

Координатор SA независимо подтвердил 15/15 файлов и тот же manifest. По его свежему read-only baseline от 22.11 21:52:40 UTC / 23.11 00:52:40 МСК MemAvailable=118264 KiB (около115,5 MiB), что ниже порога131072 KiB. Production службы active, PID прежние, NRestarts0, Rules3501. Native no-UART и аппаратное окно остаются HOLD; порог не снижать, службы ради памяти не останавливать. Новых Driver Mac jobs не требуется. Это переданный координатором замер, а не повторное подключение агента драйвера к .135.

## Где лежат доказательства

### Подготовка нового эмулированного API-теста

По предложению координатора свободных Mac-окон подготовлен новый этап, отличный от успешной проверки загрузчика: запуск сохранённого ARM MasterPLC34421 с ARM-loopback проектом и локальными имитаторами. В наборе нет nginx, поэтому добавлен прямой клиент FastCGI (только loopback и методы сессии/чтения подписок; команды записи не разрешены). Его интеграционная проверка на отдельном Windows MasterPLC 22.11 23:48:58–23:49:36 UTC прошла: 554 значения и качества до/после15 событий, обычное изменение Carel, 0 выходных команд/адресных FC70 к Carel; собственный PID27976 завершён, потоки имитаторов остановлены.

Используется уже созданный private QEMU image d347470e, inputs/libs RO, собственный output, nonroot65534, network none, 1CPU/512MiB, container150s/job≤180s. SDK/лицензии production не копируются, .135 не используется. Проверяются идентичность проекта, private candidate в maps, 554 значения/качества,19Faults,15 событий и обычный Carel. Даже успешный результат будет эмулированной API-проверкой, а не native/HIL.

На review координатор обнаружил недостаточность guard владения Docker-контейнером. Исправлено до запуска: отдельный create с cidfile и nonce, проверка ID/Name/Image/task/nonce, start и cleanup только по подтверждённому ID с повторной проверкой. Ошибки inspect/daemon/parse не считаются отсутствием; неопределённый исход создания/очистки даёт FAILED_CLEANUP_UNVERIFIED. 11 локальных негативных тестов проверяют outage, коллизию имени, смену identity/nonce, неизвестный ID и отказ kill. Старые immutable receipts сохранены.

Run05 (пакет `qemu_api_prepared_20261122_235812Z`, manifest `82af177a99511934311525aed1fe6441f5061af5a646f330620950ad49f2ec17`) в отдельном разрешённом окне остановился до create: Docker CLI сообщил точный вариант `error: no such object: <имя>` в нижнем регистре, не входивший в разрешённые ответы guard. 22.11 23:59:49.404–23:59:49.473 UTC / 23.11 02:59:49 МСК; creationAttempted=false, контейнеров нет, wrapper63129 exit1, supervisor63127/job63131 завершены, слот сразу возвращён. API/ARM runtime не исполнялся.

Добавлен именно наблюдённый точный lowercase-ответ; произвольная нормализация и поиск подстроки не применяются. Чужое имя, дополнительный текст и другой exit остаются ошибками. Тесты guard11/11 и Python syntax43 PASS. Immutable **run06**: `qemu_api_prepared_20261123_000137Z`, manifest `a782cc2000916e9c1f1d9b400fba25340c43a5a2f83e733a37f65ac2b3b3524f`; API/FastCGI/MPLC/payload и ресурсные ограничения неизменны.

Run06 передан после CPU REVIEW, первый дедлайн старта пропущен без исполнения. По новому явному GO выполнен **один** прогон: 23.11 00:49:56.425–00:50:27.442 UTC / 03:49:56–03:50:27 МСК. Результат **FAILED_EMULATED_ARM_API_NOT_HARDWARE_TEST**: за 30 секунд API не подтвердил projectId. На всех трёх имитаторах запросов/чтений/событий/записей 0. При этом runtime.log подтверждает RT34421, `Addin initing : mplc_protocol_fast_modbus.so done 3`, чтение нужного проекта и регистрацию FastModbus. Это частичное свидетельство инициализации в эмуляции, но не PASS API/каналов/событий.

Контейнер `40e3d71210fb7f444dbe9a589310c9de081e156c145f930a1d2439685a68f867`: проверены ID/image/task/nonce, Pid0/Runningfalse/ExitCode1/OOMfalse, cleanupErrors пуст. Wrapper69696 exit1, supervisor69695/job69698 отсутствуют; слот сразу возвращён. Actual receipts и логи сохранены в `private_qemu_result_20261123_run06`, также на Mac `B/artifacts/fast-modbus-qemu-20261123-run06`.

В логе повторяется `Lua: Global function ERROR_HANDLER is absent`. В ELF-only набор не вошли динамические runtime-модули FastCGI/users/FB и runtime data. Точную причину Lua/API ещё требуется подтвердить; статическая NEEDED-closure не покрывает такие assets. Локальные vendor-архивы содержат mplc_fcgi.so, но их mplc/masterplc отличаются от сохранённого live34421: смешивать сборки нельзя. Запрошен путь к уже сохранённым штатным assets exact live34421 без обращения к контроллеру. Новые запуски не разрешены. В исходник теста добавлено сохранение ошибок GetState и maps при неуспешном старте, старый пакет не изменён. До проверки полного runtime новый пакет не замораживать и native no-UART не считать готовым к исполнению.

Владелец стенда отдельно разрешил чтение только двух точных runtime paths через SFTP. Первый запрос копирования был отклонён automatic approval review из-за возможных лицензионных/проектных данных; он не исполнялся. Безопасная metadata-only проверка 23.11 00:59:29–31 UTC подтвердила: `/opt/mplc4/mplc.dat` отсутствует, `/opt/mplc4/mplc_fcgi.so` — обычный root-owned файл 509640 байт. После исключения dat и уточнения риска разрешено чтение только ELF FastCGI: 00:59:59–01:00:01 UTC, lstat/fstat до/после совпали, ARM32 hard-float, SHA256 `3a13fedc00f5358230d731b9d101fb4251905e4f99b310b3976abbfeabbf1ba8`. Evidence `runtime-assets-readonly-20261123T005959Z/receipt.json`. SSH-команд, вызовов API, опроса UART, изменений файлов/служб не было.

Точная новая зависимость FastCGI — `mplc_users.so`, в прежнем наборе её нет. Статический анализ **другой** локальной vendor-сборки дополнительно показывает ERROR_HANDLER/LuaErrorHandlerMsg и DataSubscription в `mplc_data.so`, зависимость от `mplc_database.so`, а FT_LOCAL_LIB регистрирует `mplc_fb_lib.so`. Это основание запросить четыре точных штатных ELF, не доказательство совместимости чужой сборки. Список передан владельцу; эти новые файлы пока не читались. Dat/lic/settings/производственный проект/базы пользователей исключены. Следующий Mac/native/HIL запуск остаётся HOLD.

Запрос копирования четырёх дополнительных ELF также был отклонён automatic approval; координатор отдельно запросил явное согласие пользователя на конкретные файлы и частные Windows/Mac destinations. До ответа выполнена только разрешённая локальная проверка одного ранее не проверенного архива `Downloads/Telegram Desktop/linux-armv7hf/mplc4.tar.gz` (SHA `ae17a508b4f1f7585fa66a1361f0bbc467a0b15f7345a21bbaf15c096eed0295`): все четыре identity anchors mplc/masterplc/mplcshare/liblua отличаются от live34421. Ничего не извлекалось и не исполнялось; предыдущие архивы повторно не исследовались. Среди конкретных обнаруженных кандидатов exact-runtime комплекта не найдено.

23.11 пользователь прямо разрешил копирование четырёх ELF в Windows и Mac. SFTP-only чтение выполнено 06:45:05–10 UTC / 09:45:05–10 МСК: `runtime-assets-readonly-20261123T064505Z`. Статический граф выявил дополнительный `aggregation.so`, необходимый mplc_database.so; пользователь отдельно ответил «Да, скопировать aggregation.so». Этот файл прочитан 06:48:01–03 UTC / 09:48:01–03 МСК, evidence `runtime-assets-readonly-20261123T064801Z`. Для каждого файла lstat/fstat до/после совпали, тип ELF32 ARM hard-float и SHA проверены. Файлы скопированы и повторно проверены на Mac в `B/work/fast-modbus-runtime-34421/20261123T064505Z` и `.../20261123T064801Z`. Никаких удалённых команд, опроса, остановки служб и изменений production не выполнялось.

Подготовлен immutable run07: `qemu_api_prepared_20261123_065007Z`, manifest `65158ade4a7563cfed6d600f32d2d10304a60311dbaca64b7d941b7d89f2afe4`, runner `13b310edcf82106b3c0f8332e3916fc4a6b56b653b6750fbac524827fd7eed09`, archive `089ba42443729a758ec77faa7359dc1b2b6e9c47d338a4f4104407c51f1d689f`. Дополнение runtime включает fcgi/users/data/database/fb_lib/aggregation и ранее сохранённый libcurl.so.4; исходные frozen inputs и кандидат не менялись. `static-preflight.json` подтвердил SHA всех входов, полный граф **36 ELF / 6845 сильных ссылок / 0 неразрешённых**. Это только статическая проверка, не API/HIL PASS. Ресурсы прежние: QEMU d347470e, nonroot, network none, 1CPU/512MiB, container150s/job≤180s, без SDK/установок/.135. Точный пакет передан координатору на review и согласование окна; upload пакета и исполнение ещё не выполнялись.

После отдельного GO run07 выполнен один раз: **23.11 06:56:26.137–06:56:53.658 UTC / 09:56:26–09:56:53 МСК**, статус **EMULATED_ARM_MASTERPLC_API_554_CHANNELS_PASS_HARDWARE_UNVERIFIED**. Успешны InitAddin/регистрация/опрос кандидата, чтение 554 каналов до/после изменений и все19 Fault=false/Good в обеих стадиях. Получены изменения15 источников событий и обычного Carel; EventValues=30, Timeouts/BadFrames/WriteRequests/WriteErrors=0, addressed classicConfigs=0. Во время старта зарегистрированы122 сообщения Protocol fault, последнее06:56:43.665UTC; после завершения первичного опроса fault снят. Ошибка отсутствующего Lua ERROR_HANDLER больше не возникает. Лог не объявляется полностью свободным от ошибок.

Candidate/runtime в maps подтверждены,17 общих source/header совпали с ARM run03. Тестовый runtime завершён SIGTERM(-15) самим harness после приёмки; container/wrapper exit0. CID `3b88a4d7ce7624669ddf32e2cbd335354aeacf1fde3ce54e7d6ded84e09c44f5`: identity/image/task/nonce verified, Pid0/Runningfalse/OOMfalse. Wrapper75831/supervisor75830/job75833 отсутствуют, cleanupErrors пуст, слот немедленно возвращён. Immutable evidence: `private_qemu_result_20261123_run07/HANDOFF.md`,60 файлов, manifest `558826d28af07cdc32e0f370a94f8ab7bec4a3c6c7b4cf6108b633d432a7e88d`. Все554 значения проверялись harness, в результате сохранены count/bad-list и полные19 Fault samples каждой стадии; сырой массив554 значений не сохранялся. Native/HIL остаётся отдельным этапом по свежему baseline и UART lease, новых Mac jobs не требуется.

Относительно `Сколково/архив_правок/правки_2026-11-22/sa02m_живой_опрос/`:

| Папка | Назначение / manifest SHA256 |
|---|---|
| `20261122_132637Z` | Полный проект и драйвер: `ca673612847a9b10054684d6cb5c994f5721a21eb7bdd9bc69735427c7c7eace` |
| `rtu_comparison_20261122_135031Z` | RTU/events: `d327bc9746efd9d9473f95c4b49f584e50e88095309af73e95942e0d06b01830` |
| `next_window_20261122_203228Z` | 24 файла первого окна, COM4/native exports: `57bf8bcb5c2f631b7236f22f21535e2f38d5e83a4802bbab98ff46e648d42c3b` |
| `private_qemu_result_20261122_run01` | Неуспешная установка, stdout/stderr/actual receipt |
| `private_qemu_offline_job_20261122_203649Z` | Offline run02 inputs: `c871dff639d9a98e074bc552532b094606f0e22995baaaf193e23f7dbab29ae8` |
| `private_qemu_result_20261122_run02` | Подписи и план PASS, установка не выполнена; actual receipt/wrapper exit |
| `private_qemu_offline_job_20261122_204205Z` | Исправленный run03 inputs: `ca3b42234e49202ed6407f00b8963afe0fcb4d3d12fefba9f33973f30e8e5e57` |
| `private_qemu_result_20261122_run03` | Установка PASS, ARM-probe exit1, actual receipts и SFTP mode |
| `private_qemu_probe_20261122_210649Z` | Probe-only run04: `4158abc57c5c5b1541e1f05152c5474454116f467cf44e54612f54d2c942b28b` |
| `private_qemu_result_20261122_run04` | Эмулированная загрузка ARM PASS; result, stdout, syscall trace, container/wrapper receipts |

Прежние пакеты неизменяемы. Исправленный baseline поддерживает явный output path без создания каталогов по месту скопированного скрипта. Локальная проверка синтаксиса включает сгенерированные launcher/supervisor; запускать `tools/hardware_project/check_preparation_scripts.py`. Повторять уже пройденные сборки драйвера без новых изменений не требуется.
