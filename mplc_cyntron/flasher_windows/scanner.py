# -*- coding: utf-8 -*-
"""
Сканирование RS-485: сначала быстрый скан WB extended (0xFD 0x46 0x01) по всем выбранным скоростям, затем опрос диапазона адресов по всем скоростям. Порядок скоростей: 115200 → 38400 → 19200 → 9600.
"""
import time
from typing import List, Optional, Callable, Tuple, Set
from dataclasses import dataclass, replace

from . import modbus_rtu

BROADCAST_ADDR = modbus_rtu.BROADCAST_ADDR
# Адреса для broadcast: 255 (MP-02m), 0xFD (быстрый Modbus / расширенный WB)
BROADCAST_ADDRS = (modbus_rtu.BROADCAST_ADDR, modbus_rtu.BROADCAST_ADDR_FD)
from .serial_port import open_port, send_receive, send_receive_all, send_receive_wb_ext_scan
from .flash_protocol import FlasherProtocol

REG_PROBE_0 = 0          # быстрая проверка «есть ответ» (бутлоадер отдаёт 2 рег: 0x0001, 0x0000)
REG_PROBE_0_COUNT = 2
REG_SIGNATURE = 290
REG_SIGNATURE_COUNT = 12
REG_SERIAL_LO = 270
REG_SERIAL_HI = 271
REG_VERSION_MAJOR = 320
REG_VERSION_MINOR = 321
REG_VERSION_PATCH = 322
REG_VERSION_SUFFIX = 323
REG_BOOTLOADER_VER = 330
REG_BOOTLOADER_VER_COUNT = 8

SCAN_TIMEOUT_MS = 250   # таймаут ответа при скане (один запрос на адрес: reg 0, count 2)
BOOTLOADER_BAUD = 115200
# Пауза между опросами адресов при сканировании (мс). Modbus RTU: минимум 3.5 времени символа между кадрами;
# при 9600 бод 3.5×11 бит ≈ 4 мс, задаём 5 мс с запасом.
SCAN_DELAY_MS = 5
# Таймаут приёма ответов на один broadcast (устройства с арбитражем отвечают с задержкой)
BROADCAST_COLLECT_TIMEOUT_MS = 1500
# Резерв: число попыток broadcast «один запрос — один ответ», если приём всех ответов пуст
BROADCAST_FALLBACK_ATTEMPTS = 5
BROADCAST_FALLBACK_DELAY_S = 0.08
# Порядок скоростей: по убыванию (сначала быстрые)
SCAN_BAUDRATES = [115200, 38400, 19200, 9600]
DEFAULT_PARITY = "N"
DEFAULT_STOPBITS = 1   # 8N1
# Варианты параметров связи для выбора в UI: (чётность, стоп-биты) → подпись
SCAN_LINK_OPTIONS = [
    ("N", 1, "8N1"),
    ("E", 1, "8E1"),
    ("O", 1, "8O1"),
    ("E", 2, "8E2"),
    ("O", 2, "8O2"),
    ("N", 2, "8N2"),
]
# Тип: (baudrate, parity, stopbits)
SpeedConfig = Tuple[int, str, int]


def _default_speed_configs() -> List[SpeedConfig]:
    """Конфиги по умолчанию: все скорости, 8N1."""
    return [(b, DEFAULT_PARITY, DEFAULT_STOPBITS) for b in SCAN_BAUDRATES]


@dataclass
class DeviceInfo:
    address: int
    baudrate: int
    parity: str
    stopbits: int
    signature: str
    app_version: str  # "X.Y.Z.W" or "—"
    bootloader_version: str
    serial: int  # uint32
    in_bootloader: bool
    supports_fast_modbus: bool = False  # True если найдено в фазе 1 (0xFD 0x46); иначе только стандартный Modbus


def _fill_bootloader_info_by_serial(
    port: str,
    baud: int,
    parity: str,
    stopbits: int,
    serial: int,
    dev: "DeviceInfo",
) -> None:
    """Подтянуть сигнатуру и версию бутлоадера по серийному (0xFD 0x46 0x08), если обычный Modbus не вернул данные."""
    try:
        ser = open_port(port, baudrate=baud, parity=parity, stopbits=stopbits)
        try:
            proto = FlasherProtocol(
                lambda req: send_receive(ser, req, response_timeout_ms=800),
                log_cb=None,
                verbose_exchange_log=False,
            )
            sig, ver, err = proto.read_bootloader_info_by_serial(serial)
            if err:
                return
            if sig:
                dev.signature = sig.strip()
            if ver:
                dev.bootloader_version = ver.strip() or "—"
        finally:
            ser.close()
    except Exception:
        pass


def _read_regs(
    port: str,
    slave: int,
    start: int,
    count: int,
    baudrate: int,
    parity: str,
    stopbits: int,
    timeout_ms: int = SCAN_TIMEOUT_MS,
) -> Tuple[Optional[int], Optional[bytes]]:
    """Возвращает (адрес ответившего, payload) или (None, None). При broadcast (slave=255) в ответе приходит реальный адрес устройства (1–247)."""
    try:
        ser = open_port(port, baudrate=baudrate, parity=parity, stopbits=stopbits)
        try:
            if slave == BROADCAST_ADDR:
                time.sleep(0.06)  # дать линии стабилизироваться после открытия порта перед broadcast
            else:
                time.sleep(0.03)  # короткая пауза при адресном запросе (RS-485/устройство успевают после смены порта)
            req = modbus_rtu.build_read_holding_registers(slave, start, count)
            rsp = send_receive(ser, req, response_timeout_ms=timeout_ms)
            if rsp is None:
                return (None, None)
            # При broadcast не задаём expected_slave — в ответе будет реальный адрес (1, 2, …).
            expected = None if slave in BROADCAST_ADDRS else slave
            addr, payload, err = modbus_rtu.parse_response(rsp, expected_slave=expected)
            if addr is None:
                return (None, None)
            # Для broadcast (255 или 0) считаем успехом любой ответ с реальным адресом.
            if slave in BROADCAST_ADDRS:
                return (addr, payload if payload is not None else None)
            if err or payload is None:
                return (None, None)
            return (addr, payload)
        finally:
            ser.close()
    except Exception:
        return (None, None)


def _read_regs_broadcast(
    port: str,
    start: int,
    count: int,
    baudrate: int,
    parity: str,
    stopbits: int,
    timeout_ms: int = BROADCAST_COLLECT_TIMEOUT_MS,
) -> List[Tuple[int, Optional[bytes]]]:
    """
    Широковещательный запрос по адресам из BROADCAST_ADDRS (255, 0xFD) — принять ответы от всех устройств.
    Возвращает список (адрес, payload) без дубликатов по адресу.
    """
    try:
        ser = open_port(port, baudrate=baudrate, parity=parity, stopbits=stopbits)
        try:
            time.sleep(0.12)
            seen: Set[int] = set()
            result: List[Tuple[int, Optional[bytes]]] = []
            for broadcast_addr in BROADCAST_ADDRS:
                req = modbus_rtu.build_read_holding_registers(broadcast_addr, start, count)
                responses = send_receive_all(ser, req, response_timeout_ms=timeout_ms)
                for (addr, payload) in responses:
                    if 1 <= addr <= 247 and addr not in seen:
                        seen.add(addr)
                        result.append((addr, payload))
                if result and broadcast_addr == BROADCAST_ADDRS[0]:
                    time.sleep(0.05)
            return result
        finally:
            ser.close()
    except Exception:
        return []


# Число попыток быстрого скана для арбитража при нескольких устройствах на линии (ответы могут приходить с задержкой или коллидировать)
WB_EXT_SCAN_ATTEMPTS = 3
# Пауза между попытками (мс), чтобы шина освободилась
WB_EXT_SCAN_RETRY_DELAY_MS = 120
# Длительность тишины для завершения приёма (мс); больше — даёт время на ответ нескольких устройств
WB_EXT_SCAN_SILENCE_MS = 100


def _wb_ext_scan(
    port: str,
    baudrate: int,
    parity: str,
    stopbits: int,
    timeout_ms: int = BROADCAST_COLLECT_TIMEOUT_MS,
    log_cb: Optional[Callable[[str], None]] = None,
) -> List[Tuple[int, int]]:
    """WB extended scan (0xFD 0x46 0x01): открыть порт, отправить запрос, собрать ответы 0xFD 0x46 0x03.
    Арбитраж: несколько попыток с объединением результатов по адресу (устройства могут отвечать с задержкой или коллидировать)."""
    merged: List[Tuple[int, int]] = []
    seen_addr: Set[int] = set()
    try:
        for attempt in range(WB_EXT_SCAN_ATTEMPTS):
            if attempt > 0:
                time.sleep(WB_EXT_SCAN_RETRY_DELAY_MS / 1000.0)
            try:
                ser = open_port(port, baudrate=baudrate, parity=parity, stopbits=stopbits)
            except Exception:
                break
            try:
                time.sleep(0.08)
                if log_cb and attempt > 0:
                    log_cb("  Быстрый скан попытка %d/%d" % (attempt + 1, WB_EXT_SCAN_ATTEMPTS))
                partial = send_receive_wb_ext_scan(
                    ser,
                    response_timeout_ms=timeout_ms,
                    silence_ms=WB_EXT_SCAN_SILENCE_MS,
                    try_legacy_0x60=True,
                    log_cb=log_cb,
                )
                for addr, serial in partial:
                    if addr not in seen_addr:
                        seen_addr.add(addr)
                        merged.append((addr, serial))
                if log_cb and partial:
                    addrs = sorted(set(a for a, _ in partial))
                    log_cb("  Быстрый скан попытка %d: найдено адреса %s" % (attempt + 1, addrs))
            finally:
                ser.close()
        return merged
    except Exception:
        return []


def _parse_serial(payload: Optional[bytes]) -> int:
    """Regs 270 (lo), 271 (hi); in response: reg 270 = payload[0:2] BE, reg 271 = payload[2:4] BE -> uint32 LE order."""
    if payload is None or len(payload) < 4:
        return 0
    lo = (payload[0] << 8) | payload[1]
    hi = (payload[2] << 8) | payload[3]
    return (hi << 16) | lo


def _parse_version_4(payload: Optional[bytes]) -> str:
    if payload is None or len(payload) < 8:
        return "—"
    # 320=MAJOR, 321=MINOR, 322=PATCH, 323=SUFFIX; each reg big-endian in response
    try:
        maj = (payload[0] << 8) | payload[1]
        mi = (payload[2] << 8) | payload[3]
        patch = (payload[4] << 8) | payload[5]
        suf = (payload[6] << 8) | payload[7]
        if suf & 0x8000:
            suf -= 0x10000
        return f"{maj}.{mi}.{patch}.{suf}"
    except Exception:
        return "—"


def _safe_str_from_bytes(raw: bytes, max_len: int = 0) -> str:
    """
    Безопасный вывод байтов в таблицу: latin-1 (без замены на U+FFFD),
    непечатаемые символы (в т.ч. 0x80–0xFF) заменяются точкой; обрезка по max_len.
    """
    if not raw:
        return ""
    s = raw.decode("latin-1")
    out = "".join(c if 32 <= ord(c) <= 126 else "." for c in s)
    out = out.rstrip(". ")
    if max_len and len(out) > max_len:
        out = out[:max_len]
    return out


def _parse_signature(payload: Optional[bytes]) -> str:
    """12 registers = 24 bytes; one byte per register (low byte). Safe display: only printable ASCII."""
    if payload is None or len(payload) < 24:
        return ""
    raw = bytes(payload[1::2][:12])
    return _safe_str_from_bytes(raw, max_len=12)


def _is_bootloader_mode(app_version: str) -> bool:
    """Режим загрузчика: бутлоадер отдаёт версию приложения 0.0.0.0; приложение — ненулевую (например 2.0.0.0)."""
    return app_version in ("—", "", "0.0.0.0")


def _parse_bootloader_ver(payload: Optional[bytes]) -> str:
    """
    Версия загрузчика: 8 регистров (строка по младшему байту), формат как в Wiren Board / бутлоадере
    (null-terminated). Обрезаем по первому 0x00, чтобы не показывать «1.5.7..#» вместо «1.5.7».
    """
    if payload is None or len(payload) < 4:
        return "—"
    # Сначала пробуем как строку (8 рег = 16 байт); при пустой/невалидной — «—»
    if len(payload) >= 16:
        raw = bytes(payload[1::2][:8])
        # Как в wb-mcu-fw-flasher: строка null-terminated, не выводить мусор после \0
        if b"\x00" in raw:
            raw = raw.split(b"\x00")[0]
        s = _safe_str_from_bytes(raw, max_len=16)
        if not s or s.strip() in ("-", ".", ""):
            return "—"
        return s
    # Иначе как uint32 из первых двух регистров (BE: hi, lo)
    lo = (payload[0] << 8) | payload[1]
    hi = (payload[2] << 8) | payload[3]
    u32 = (hi << 16) | lo
    if u32 == 0:
        return "—"  # Нет бутлоадера / не записано — не показывать как версию
    return f"0x{u32:08X}"


def scan_address(
    port: str,
    address: int,
    speed_configs: List[SpeedConfig],
    log_cb: Optional[Callable[[str], None]] = None,
    current_cb: Optional[Callable[[int, int, str, int], None]] = None,
) -> Optional[DeviceInfo]:
    """Один запрос на адрес: Read Holding reg 0, count 2; таймаут 250 мс. При ответе — дозапрос сигнатуры/версии/серийного."""
    for (baud, parity, stopbits) in speed_configs:
        if current_cb:
            current_cb(address, baud, parity, stopbits)
        _, pl0 = _read_regs(
            port, address, REG_PROBE_0, REG_PROBE_0_COUNT,
            baud, parity, stopbits, SCAN_TIMEOUT_MS
        )
        if pl0 is not None:
            _, pl_ser = _read_regs(
                port, address, REG_SERIAL_LO, 2,
                baud, parity, stopbits, SCAN_TIMEOUT_MS
            )
            if pl_ser is not None:
                serial_val = _parse_serial(pl_ser)
                _, pl_ver = _read_regs(
                    port, address, REG_VERSION_MAJOR, 4,
                    baud, parity, stopbits, SCAN_TIMEOUT_MS
                )
                _, sig_pl = _read_regs(
                    port, address, REG_SIGNATURE, REG_SIGNATURE_COUNT,
                    baud, parity, stopbits, SCAN_TIMEOUT_MS
                )
                _, bl_ver_pl = _read_regs(
                    port, address, REG_BOOTLOADER_VER, REG_BOOTLOADER_VER_COUNT,
                    baud, parity, stopbits, SCAN_TIMEOUT_MS
                )
                bl_ver = _parse_bootloader_ver(bl_ver_pl) if bl_ver_pl else "—"
                app_ver = _parse_version_4(pl_ver) if pl_ver else "—"
                return DeviceInfo(
                    address=address,
                    baudrate=baud,
                    parity=parity,
                    stopbits=stopbits,
                    signature=_parse_signature(sig_pl) if sig_pl else "",
                    app_version=app_ver,
                    bootloader_version=bl_ver,
                    serial=serial_val,
                    in_bootloader=_is_bootloader_mode(app_ver),
                )
            return DeviceInfo(
                address=address,
                baudrate=baud,
                parity=parity,
                stopbits=stopbits,
                signature="",
                app_version="—",
                bootloader_version="—",
                serial=0,
                in_bootloader=True,
            )
        # Try app: serial 270-271
        _, pl_ser = _read_regs(
            port, address, REG_SERIAL_LO, 2,
            baud, parity, stopbits, SCAN_TIMEOUT_MS
        )
        if pl_ser is not None:
            serial_val = _parse_serial(pl_ser)
            _, pl_ver = _read_regs(
                port, address, REG_VERSION_MAJOR, 4,
                baud, parity, stopbits, SCAN_TIMEOUT_MS
            )
            _, sig_pl = _read_regs(
                port, address, REG_SIGNATURE, REG_SIGNATURE_COUNT,
                baud, parity, stopbits, SCAN_TIMEOUT_MS
            )
            _, bl_ver_pl = _read_regs(
                port, address, REG_BOOTLOADER_VER, REG_BOOTLOADER_VER_COUNT,
                baud, parity, stopbits, SCAN_TIMEOUT_MS
            )
            bl_ver = _parse_bootloader_ver(bl_ver_pl) if bl_ver_pl else "—"
            app_ver = _parse_version_4(pl_ver)
            return DeviceInfo(
                address=address,
                baudrate=baud,
                parity=parity,
                stopbits=stopbits,
                signature=_parse_signature(sig_pl),
                app_version=app_ver,
                bootloader_version=bl_ver,
                serial=serial_val,
                in_bootloader=_is_bootloader_mode(app_ver),
            )
    return None


def scan_broadcast(port: str, log_cb: Optional[Callable[[str], None]] = None) -> Optional[DeviceInfo]:
    """Один запрос на адрес 255 (reg 0, count 2), таймаут 250 мс; в ответе — реальный адрес устройства (1–247)."""
    for baud in [115200, 9600]:
        addr0, pl0 = _read_regs(
            port, BROADCAST_ADDR, REG_PROBE_0, REG_PROBE_0_COUNT,
            baud, DEFAULT_PARITY, DEFAULT_STOPBITS, SCAN_TIMEOUT_MS
        )
        if pl0 is not None and addr0 is not None:
            return DeviceInfo(
                address=addr0,
                baudrate=baud,
                parity=DEFAULT_PARITY,
                stopbits=DEFAULT_STOPBITS,
                signature="",
                app_version="—",
                bootloader_version="—",
                serial=0,
                in_bootloader=True,
            )
    return None


def _broadcast_probe_bauds(
    port: str,
    speed_configs: List[SpeedConfig],
    log_cb: Optional[Callable[[str], None]] = None,
    on_device_found: Optional[Callable[["DeviceInfo"], None]] = None,
    cancel_cb: Optional[Callable[[], bool]] = None,
) -> Tuple[List[SpeedConfig], Set[Tuple[int, int, str, int]], List["DeviceInfo"]]:
    """
    Быстрый скан: WB extended (0xFD 0x46 0x01) на каждый конфиг. По каждому найденному адресу — scan_address и on_device_found.
    Возвращает (responsive_configs, broadcast_seen, list_of_devices).
    """
    responsive: List[SpeedConfig] = []
    broadcast_seen: Set[Tuple[int, int, str, int]] = set()
    devices_found: List[DeviceInfo] = []
    timeout_ms = BROADCAST_COLLECT_TIMEOUT_MS
    for (baud, parity, stopbits) in speed_configs:
        if cancel_cb and cancel_cb():
            break
        cfg = f"{baud} {parity}{stopbits}"
        responses: List[Tuple[int, Optional[int], Optional[bytes]]] = []  # (addr, serial_from_wb_scan, payload)
        # Только WB extended scan (0xFD 0x46 0x01) — единый алгоритм как у Wiren Board; старый broadcast (255/0xFD read reg) убран.
        if log_cb:
            log_cb(f"[{cfg}] WB extended: запрос 0xFD 0x46 0x01 (сканирование).")
        wb_scan = _wb_ext_scan(port, baud, parity, stopbits, timeout_ms, log_cb=log_cb)
        seen_addr = {item[0] for item in responses}
        if wb_scan:
            for addr, serial in wb_scan:
                if addr not in seen_addr:
                    seen_addr.add(addr)
                    responses.append((addr, serial, None))
            if log_cb:
                log_cb(f"[{cfg}] WB extended: найдены адреса {sorted({a for (a, _) in wb_scan})}.")
        if responses and log_cb:
            addrs = sorted({item[0] for item in responses})
            log_cb(f"[{cfg}] WB extended: ответы от адресов {addrs}.")
        for item in responses:
            addr = item[0]
            serial_from_wb = item[1] if len(item) > 1 else None
            broadcast_seen.add((addr, baud, parity, stopbits))
            # Сразу показываем в таблице: адрес и серийный (если есть), остальное дополним после опроса.
            placeholder = DeviceInfo(
                address=addr,
                baudrate=baud,
                parity=parity,
                stopbits=stopbits,
                signature="",
                app_version="—",
                bootloader_version="—",
                serial=serial_from_wb if serial_from_wb is not None else 0,
                in_bootloader=True,
            )
            if on_device_found:
                on_device_found(placeholder)
            # Сброс буфера перед опросом (остатки после WB scan могут мешать приёму ответа по обычному Modbus).
            try:
                ser = open_port(port, baudrate=baud, parity=parity, stopbits=stopbits)
                ser.reset_input_buffer()
                ser.close()
            except Exception:
                pass
            dev = scan_address(port, addr, [(baud, parity, stopbits)], log_cb)
            if dev is not None:
                if serial_from_wb is not None and (dev.serial == 0 or dev.serial == 0xFFFFFFFF):
                    dev.serial = serial_from_wb
                if not dev.signature and not dev.bootloader_version and serial_from_wb is not None:
                    # Бутлоадер может отвечать только на 0xFD 0x46 0x08 — подтягиваем рег. 290/330 по серийному.
                    _fill_bootloader_info_by_serial(port, baud, parity, stopbits, serial_from_wb, dev)
                devices_found.append(dev)
                if on_device_found:
                    on_device_found(dev)
                if log_cb:
                    ser_str = f"0x{dev.serial:08X}" if (dev.serial and dev.serial != 0xFFFFFFFF) else "—"
                    log_cb(
                        f"[{cfg}] WB extended: адрес {addr} — серийный № {ser_str}, "
                        f"версия пр. {dev.app_version}, версия загрузчика {dev.bootloader_version}, сигнатура «{dev.signature or '—'}»"
                    )
            else:
                if serial_from_wb is not None and not placeholder.signature and not placeholder.bootloader_version:
                    _fill_bootloader_info_by_serial(port, baud, parity, stopbits, serial_from_wb, placeholder)
                devices_found.append(placeholder)
                if on_device_found:
                    on_device_found(placeholder)
                if log_cb:
                    log_cb(f"[{cfg}] WB extended: адрес {addr} (данные уточняются при опросе).")
        if responses:
            responsive.append((baud, parity, stopbits))
    return responsive, broadcast_seen, devices_found


# Допустимый диапазон адресов Modbus RTU (1–247)
SCAN_ADDR_MIN_DEFAULT = 1
SCAN_ADDR_MAX_DEFAULT = 10


def scan_all(
    port: str,
    progress_cb: Optional[Callable[..., None]] = None,
    log_cb: Optional[Callable[[str], None]] = None,
    cancel_cb: Optional[Callable[[], bool]] = None,
    on_device_found: Optional[Callable[["DeviceInfo"], None]] = None,
    speed_configs: Optional[List[SpeedConfig]] = None,
    addr_min: int = SCAN_ADDR_MIN_DEFAULT,
    addr_max: int = SCAN_ADDR_MAX_DEFAULT,
    fast_scan: bool = True,
) -> List[DeviceInfo]:
    """Сканирование: при fast_scan=True — сначала быстрый WB extended (0xFD 0x46 0x01) по всем скоростям и параметрам, затем опрос диапазона; при fast_scan=False — только опрос диапазона addr_min–addr_max по выбранным скоростям."""
    if speed_configs is None or len(speed_configs) == 0:
        speed_configs = _default_speed_configs()
    addr_min = max(1, min(addr_min, 247))
    addr_max = max(1, min(addr_max, 247))
    if addr_min > addr_max:
        addr_min, addr_max = addr_max, addr_min
    # Порядок по убыванию скорости
    baud_order = {b: i for i, b in enumerate(SCAN_BAUDRATES)}
    config_order = sorted(
        speed_configs,
        key=lambda c: (baud_order.get(c[0], 999), c[1], c[2]),
    )
    devices: List[DeviceInfo] = []
    seen_addrs: Set[int] = set()
    num_addrs = addr_max - addr_min + 1
    total = len(config_order) * num_addrs
    if log_cb:
        log_cb(f"Сканирование: порядок по скорости {[f'{b} {p}{s}' for (b, p, s) in config_order]}.")
    # Фаза 1: быстрое сканирование (если включено) по выбранным скоростям и параметрам связи
    if fast_scan:
        if log_cb:
            log_cb("Фаза 1. Быстрое сканирование (0xFD 0x46 0x01 по выбранным скоростям и параметрам связи).")
        for cfg in config_order:
            if cancel_cb and cancel_cb():
                break
            _, _, devices_from_wb = _broadcast_probe_bauds(
                port, [cfg], log_cb,
                on_device_found=on_device_found,
                cancel_cb=cancel_cb,
            )
            for d in devices_from_wb:
                if d.address not in seen_addrs:
                    d1 = replace(d, supports_fast_modbus=True)
                    devices.append(d1)
                    seen_addrs.add(d.address)
                    if on_device_found:
                        on_device_found(d1)
        if log_cb:
            log_cb("Фаза 1: только устройства с поддержкой 0xFD 0x46. Устройства WB (только стандартный Modbus) будут в фазе 2.")
    if log_cb:
        log_cb(f"Фаза 2: опрос диапазона адресов {addr_min}..{addr_max} по выбранным скоростям и параметрам связи.")
    # Фаза 2: опрос диапазона по всем скоростям
    for config_idx, (baud, parity, stopbits) in enumerate(config_order):
        if cancel_cb and cancel_cb():
            break
        cfg = (baud, parity, stopbits)
        for addr in range(addr_min, addr_max + 1):
            if cancel_cb and cancel_cb():
                break
            done = config_idx * num_addrs + (addr - addr_min) + 1
            if progress_cb:
                progress_cb(done, total, addr, cfg)

            def _current_cb(a: int, b: int, p: str, s: int) -> None:
                if progress_cb:
                    progress_cb(config_idx * num_addrs + (a - addr_min) + 1, total, a, (b, p, s))

            dev = scan_address(port, addr, [cfg], log_cb, current_cb=_current_cb)
            if dev is not None:
                if addr not in seen_addrs:
                    devices.append(dev)
                    seen_addrs.add(addr)
                    if on_device_found:
                        on_device_found(dev)
                if log_cb:
                    ser_str = f"0x{dev.serial:08X}" if (dev.serial and dev.serial != 0xFFFFFFFF) else "—"
                    log_cb(
                        f"Modbus RTU (сканирование {addr_min}–{addr_max}): адрес {addr}, {dev.baudrate} бод; "
                        f"серийный № {ser_str}, версия пр. {dev.app_version}, версия загрузчика {dev.bootloader_version}"
                    )
            if SCAN_DELAY_MS > 0:
                time.sleep(SCAN_DELAY_MS / 1000.0)
    devices.sort(key=lambda d: (d.address, d.baudrate))
    return devices
