# -*- coding: utf-8 -*-
"""
Modbus RTU client for MP-02m flasher.
Functions: 0x03 Read Holding Registers, 0x06 Write Single Register,
0x10 Write Multiple Registers.
CRC16: polynomial 0xA001, init 0xFFFF; в кадре — как Wiren Board / Modbus RTU: LSB first (<H).
Кадры 0xFD 0x46/0x60 (сканирование, 0x08 по серийному) — тот же формат CRC.
Broadcast: только 255 (0xFF), как в основном проекте и бутлоадере.
"""
import struct
from typing import Optional, Tuple, List, Callable

# Широковещательный адрес Modbus: 255 (MP-02m), 0 (Wiren Board), 0xFD (быстрый Modbus / расширенный протокол WB).
BROADCAST_ADDR = 0xFF
BROADCAST_ADDR_WB = 0
BROADCAST_ADDR_FD = 0xFD  # Wiren Board extended / быстрый Modbus (wb-modbus-ext-scanner)

# Быстрый Modbus (Wiren Board): адрес 0xFD, функция 0x46 (старый 0x60 — legacy).
# Обращение к устройству по серийному: 0xFD 0x46 0x08 [serial 4B BE] [inner PDU] CRC.
# Ответ: 0xFD 0x46 0x09 [serial 4B BE] [inner response] CRC.
# Сканирование WB extended: запрос 0xFD 0x46 0x01 + CRC (5 байт); ответ 0xFD 0x46 0x03 [serial 4B BE] [addr 1B] CRC (10 байт).
BL_FAST_MODBUS_ADDR = 0xFD
BL_FAST_MODBUS_FUNC = 0x46
BL_FAST_MODBUS_FUNC_LEGACY = 0x60
BL_FAST_MODBUS_EMULATE_REQ = 0x08
BL_FAST_MODBUS_EMULATE_RSP = 0x09
WB_EXT_SCAN_START = 0x01
WB_EXT_SCAN_NEXT = 0x02   # продолжение сканирования (следующее устройство по арбитражу)
WB_EXT_SCAN_RESP = 0x03
WB_EXT_SCAN_END = 0x04   # конец сканирования
WB_EXT_SCAN_FRAME_LEN = 10

# CRC16 Modbus: poly 0xA001, init 0xFFFF
def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc

def build_read_holding_registers(slave: int, start_addr: int, count: int) -> bytes:
    """Function 0x03: Read Holding Registers."""
    if not (1 <= count <= 125):
        raise ValueError("count must be 1..125")
    body = struct.pack(">BHH", 0x03, start_addr & 0xFFFF, count)
    frame = bytes([slave]) + body
    crc = crc16_modbus(frame)
    return frame + struct.pack("<H", crc)


def build_read_holding_registers_body(start_addr: int, count: int) -> bytes:
    """Тело PDU 0x03 без адреса (для обёртки 0x46 0x08)."""
    if not (1 <= count <= 125):
        raise ValueError("count must be 1..125")
    return struct.pack(">BHH", 0x03, start_addr & 0xFFFF, count)

def build_write_single_register(slave: int, reg_addr: int, value: int) -> bytes:
    """Function 0x06: Write Single Register. value is 16-bit."""
    body = struct.pack(">BHH", 0x06, reg_addr & 0xFFFF, value & 0xFFFF)
    frame = bytes([slave]) + body
    crc = crc16_modbus(frame)
    return frame + struct.pack("<H", crc)


def build_write_single_register_body(reg_addr: int, value: int) -> bytes:
    """Тело PDU 0x06 без адреса (для обёртки 0x46 0x08)."""
    return struct.pack(">BHH", 0x06, reg_addr & 0xFFFF, value & 0xFFFF)

def build_write_multiple_registers(
    slave: int, start_addr: int, values: List[int]
) -> bytes:
    """Function 0x10: Write Multiple Registers. values as 16-bit, sent big-endian. byte_count в 1 байт (макс 255), макс 127 reg."""
    count = len(values)
    if not (1 <= count <= 127):
        raise ValueError("count must be 1..127")
    byte_count = count * 2
    body = struct.pack(">BHHB", 0x10, start_addr & 0xFFFF, count & 0xFFFF, byte_count)
    body += b"".join(struct.pack(">H", v & 0xFFFF) for v in values)
    frame = bytes([slave]) + body
    crc = crc16_modbus(frame)
    return frame + struct.pack("<H", crc)


def build_write_multiple_registers_body(
    start_addr: int, values: List[int]
) -> bytes:
    """Тело PDU 0x10 без адреса (для обёртки 0x46 0x08)."""
    count = len(values)
    if not (1 <= count <= 127):
        raise ValueError("count must be 1..127")
    byte_count = count * 2
    body = struct.pack(">BHHB", 0x10, start_addr & 0xFFFF, count & 0xFFFF, byte_count)
    body += b"".join(struct.pack(">H", v & 0xFFFF) for v in values)
    return body


def build_fast_modbus_request(serial: int, inner_pdu: bytes) -> bytes:
    """Быстрый Modbus: запрос к устройству по серийному. 0xFD 0x46 0x08 [serial 4B BE] inner_pdu CRC."""
    serial_be = struct.pack(">I", serial & 0xFFFFFFFF)
    frame = bytes([BL_FAST_MODBUS_ADDR, BL_FAST_MODBUS_FUNC, BL_FAST_MODBUS_EMULATE_REQ]) + serial_be + inner_pdu
    crc = crc16_modbus(frame)
    return frame + struct.pack("<H", crc)


def parse_fast_modbus_response(
    data: bytes,
    expected_serial: Optional[int] = None,
    log_cb: Optional[Callable[[str], None]] = None,
) -> Tuple[Optional[int], Optional[bytes], Optional[str]]:
    """
    Разбор ответа 0xFD 0x46 0x09 [serial 4B BE] inner_response CRC.
    Returns (serial, inner_payload, error). inner_payload — для 0x03 тело (byte_count + data); для 0x06/0x10 — None при успехе.
    """
    def _log(msg: str) -> None:
        if log_cb:
            log_cb(msg)

    if len(data) < 9:
        _log(f"parse_fast_modbus_response: len={len(data)} < 9")
        return None, None, "Слишком короткий ответ 0x46"
    if data[0] != BL_FAST_MODBUS_ADDR or data[1] != BL_FAST_MODBUS_FUNC or data[2] != BL_FAST_MODBUS_EMULATE_RSP:
        return None, None, "Не ответ 0xFD 0x46 0x09"
    serial = struct.unpack(">I", data[3:7])[0]
    if expected_serial is not None and (serial & 0xFFFFFFFF) != (expected_serial & 0xFFFFFFFF):
        return serial, None, f"Серийный в ответе 0x{serial:08X}, ожидался 0x{expected_serial:08X}"
    inner = data[7:-2]
    crc = crc16_modbus(data[:-2])
    if struct.pack("<H", crc) != data[-2:]:
        return None, None, "Ошибка CRC в ответе 0x46"
    if len(inner) < 1:
        return serial, None, None
    func = inner[0]
    if func & 0x80:
        return serial, None, f"Исключение Modbus: код {inner[2] if len(inner) >= 3 else 0}"
    if func == 0x03:
        if len(inner) < 3:
            return None, None, "Неверная длина ответа 0x03 в 0x46"
        byte_count = inner[1]
        if len(inner) != 2 + byte_count:
            return None, None, "Неверная длина payload 0x03"
        return serial, inner[1:], None
    if func in (0x06, 0x10):
        return serial, None, None
    return serial, None, f"Неизвестная функция в ответе 0x46: {func}"


def _build_wb_ext_scan_cmd(legacy: bool, subcmd: int) -> bytes:
    """Широковещательная команда сканирования: 0xFD 0x46/0x60 <subcmd> + CRC (5 байт). subcmd: 0x01 start, 0x02 next, 0x04 end."""
    func_byte = BL_FAST_MODBUS_FUNC_LEGACY if legacy else BL_FAST_MODBUS_FUNC
    frame = bytes([BL_FAST_MODBUS_ADDR, func_byte, subcmd])
    crc = crc16_modbus(frame)
    return frame + struct.pack("<H", crc)


def build_wb_ext_scan_start(legacy: bool = False) -> bytes:
    """Широковещательный запрос начала сканирования WB extended: 0xFD 0x46 0x01 + CRC (5 байт). legacy=True → 0x60."""
    return _build_wb_ext_scan_cmd(legacy, WB_EXT_SCAN_START)


def build_wb_ext_scan_next(legacy: bool = False) -> bytes:
    """Продолжение сканирования (следующее устройство): 0xFD 0x46 0x02 + CRC. По протоколу WB после ответа устройства мастер шлёт 0x02 для ответа следующего."""
    return _build_wb_ext_scan_cmd(legacy, WB_EXT_SCAN_NEXT)


def build_wb_ext_scan_end(legacy: bool = False) -> bytes:
    """Конец сканирования: 0xFD 0x46 0x04 + CRC."""
    return _build_wb_ext_scan_cmd(legacy, WB_EXT_SCAN_END)


def parse_wb_ext_scan_response(
    data: bytes, offset: int = 0
) -> Tuple[Optional[int], Optional[int], int]:
    """
    Разбор ответа на сканирование WB: 0xFD 0x46/0x60 0x03 [serial 4B BE] [modbus_addr 1B] CRC (10 байт).
    Returns (modbus_addr, serial, frame_len). frame_len=0 при ошибке.
    """
    d = data[offset:]
    if len(d) < WB_EXT_SCAN_FRAME_LEN:
        return None, None, 0
    if d[0] != BL_FAST_MODBUS_ADDR or d[2] != WB_EXT_SCAN_RESP:
        return None, None, 0
    if d[1] not in (BL_FAST_MODBUS_FUNC, BL_FAST_MODBUS_FUNC_LEGACY):
        return None, None, 0
    crc = crc16_modbus(d[: WB_EXT_SCAN_FRAME_LEN - 2])
    if struct.pack("<H", crc) != d[WB_EXT_SCAN_FRAME_LEN - 2 : WB_EXT_SCAN_FRAME_LEN]:
        return None, None, 0
    serial = struct.unpack(">I", d[3:7])[0]
    modbus_addr = d[7]
    if not (1 <= modbus_addr <= 247):
        return None, None, 0
    return modbus_addr, serial, WB_EXT_SCAN_FRAME_LEN


def _parse_response_from(data: bytes, offset: int = 0) -> Tuple[Optional[int], Optional[bytes], Optional[str]]:
    """Разбор одного кадра с позиции offset. Без поиска по буферу."""
    d = data[offset:]
    if len(d) < 5:
        return None, None, "Слишком короткий ответ"
    slave = d[0]
    func = d[1]
    if func & 0x80:
        # Исключение: [адрес, функция|0x80, код] + CRC (3 байта под CRC)
        if len(d) < 5:
            return None, None, "Ошибка Modbus (короткий кадр)"
        crc = crc16_modbus(d[:3])
        crc_le = struct.pack("<H", crc)
        crc_be = struct.pack(">H", crc)
        if crc_le != d[3:5] and crc_be != d[3:5]:
            # Устойчивость: часть устройств/загрузчиков шлёт CRC в нестандартном виде — принимаем кадр исключения по форме
            if 1 <= slave <= 247 and len(d) >= 5:
                return slave, None, f"Исключение Modbus: код {d[2]}"
            return None, None, "Ошибка CRC в ответе"
        return slave, None, f"Исключение Modbus: код {d[2]}"
    if func == 0x03:
        byte_count = d[2]
        frame_len = 3 + byte_count + 2
        if len(d) < frame_len:
            return None, None, "Неверная длина ответа 0x03"
        payload = d[3 : 3 + byte_count]
        crc = crc16_modbus(d[: 3 + byte_count])
        if struct.pack("<H", crc) != d[3 + byte_count : frame_len]:
            return None, None, "Ошибка CRC в ответе"
        return slave, payload, None
    if func in (0x06, 0x10):
        if len(d) < 8:
            return None, None, "Неверная длина ответа записи"
        crc = crc16_modbus(d[:6])
        crc_le = struct.pack("<H", crc)
        crc_be = struct.pack(">H", crc)
        if crc_le != d[6:8] and crc_be != d[6:8]:
            return None, None, "Ошибка CRC в ответе"
        return slave, None, None
    return None, None, f"Неизвестная функция ответа: {func}"


def parse_response(
    data: bytes,
    expected_slave: Optional[int] = None,
    log_cb: Optional[Callable[[str], None]] = None,
) -> Tuple[Optional[int], Optional[bytes], Optional[str]]:
    """
    Parse Modbus RTU response. Returns (slave, payload, error_message).
    Если задан log_cb(msg: str), пишет детальный лог разбора (для отладки).
    """
    def _log(msg: str) -> None:
        if log_cb:
            log_cb(msg)

    if len(data) < 5:
        _log(f"parse_response: len={len(data)} < 5 → Слишком короткий ответ")
        return None, None, "Слишком короткий ответ"
    _log(f"parse_response: RX len={len(data)} hex={data[:80].hex()}{'...' if len(data)>40 else ''}")
    # Сначала пробуем с начала
    slave, payload, err = _parse_response_from(data, 0)
    _log(f"parse_response: offset=0 slave={slave} func={data[1] if len(data)>1 else None} err={err!r}")
    if err is None and (expected_slave is None or slave == expected_slave):
        return slave, payload, None
    # Исключение Modbus (код 04 и др.) — валидный ответ, возвращаем его вызывающему (не "Неверный ответ")
    if err is not None and err.startswith("Исключение Modbus:") and slave is not None:
        if expected_slave is None or slave == expected_slave:
            return slave, None, err
    # Поиск начала кадра
    search_max = min(len(data) - 4, 64)
    for i in range(1, search_max):
        if data[i] < 1 or data[i] > 247:
            continue
        f = data[i + 1]
        if f not in (0x03, 0x83, 0x06, 0x10, 0x86, 0x90):
            continue
        slave, payload, err = _parse_response_from(data, i)
        _log(f"parse_response: offset={i} slave={data[i]} func=0x{f:02x} err={err!r}")
        if err is not None:
            continue
        if expected_slave is not None and slave != expected_slave:
            continue
        return slave, payload, None
    _log(f"parse_response: кадр не найден после перебора offset 1..{search_max-1} → Неверный ответ")
    return None, None, "Неверный ответ"
