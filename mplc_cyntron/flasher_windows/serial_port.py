# -*- coding: utf-8 -*-
"""
Serial port wrapper for Modbus RTU: open with given baud/parity/stopbits,
send request and read response with timeout.
"""
import serial
import serial.tools.list_ports
from typing import Optional, List, Tuple, Callable
import time

# Для разбора нескольких ответов на один broadcast
try:
    from . import modbus_rtu
except ImportError:
    import modbus_rtu  # type: ignore

PARITY_MAP = {"N": serial.PARITY_NONE, "E": serial.PARITY_EVEN, "O": serial.PARITY_ODD}
STOP_MAP = {1: serial.STOPBITS_ONE, 2: serial.STOPBITS_TWO}


def list_com_ports() -> List[Tuple[str, str]]:
    """Return list of (port_name, description)."""
    return [(p.device, p.description or p.device) for p in serial.tools.list_ports.comports()]


def open_port(
    port: str,
    baudrate: int = 9600,
    parity: str = "N",
    stopbits: int = 2,
) -> serial.Serial:
    """Open COM port. parity N/E/O, stopbits 1 or 2."""
    return serial.Serial(
        port=port,
        baudrate=baudrate,
        bytesize=serial.EIGHTBITS,
        parity=PARITY_MAP.get(parity, serial.PARITY_NONE),
        stopbits=STOP_MAP.get(stopbits, serial.STOPBITS_TWO),
        timeout=0.05,
        write_timeout=2.0,
    )


def send_receive(
    ser: serial.Serial,
    request: bytes,
    response_timeout_ms: int = 2000,
) -> Optional[bytes]:
    """
    Send request, read response. Modbus RTU: пауза после TX (переключение RS-485),
    затем чтение до таймаута или до паузы в приходе данных.
    Returns response bytes or None on timeout.
    """
    ser.reset_input_buffer()
    ser.write(request)
    ser.flush()
    # Короткая пауза для переключения RS-485 в RX; чтение начинаем сразу, чтобы не потерять ранний ответ (бутлоадер отвечает через ~10–15 ms).
    char_time = (1 + (ser.bytesize or 8) + (2 if ser.stopbits == 2 else 1)) / ser.baudrate
    if ser.parity and ser.parity != "N":
        char_time += 1 / ser.baudrate
    post_send_delay = max(0.001, min(0.02, char_time * 3.5 + 0.002))  # 1–20 ms: читаем как можно раньше
    time.sleep(post_send_delay)
    deadline = time.perf_counter() + (response_timeout_ms / 1000.0)
    chunks = []
    last_recv = time.perf_counter()
    while time.perf_counter() < deadline:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting)
            chunks.append(chunk)
            last_recv = time.perf_counter()
            # Ранний выход: как только сформирован целый Modbus-кадр, не ждём тишину/дедлайн.
            data_now = b"".join(chunks)
            # Убираем эхо запроса из начала (часть адаптеров отзеркаливает TX в RX).
            if len(request) > 0 and len(data_now) >= len(request) and data_now[:len(request)] == request:
                data_now = data_now[len(request):]
            if len(data_now) >= 5:
                func = data_now[1]
                # Ответ быстрого Modbus 0xFD 0x46 0x09 [serial 4B] [inner PDU без addr] CRC. Заголовок 7 B (FD 46 09 + serial). Бутлоадер: inner 5 B для 0x10/0x06.
                if len(data_now) >= 8 and data_now[0] == 0xFD and data_now[1] == 0x46 and data_now[2] == 0x09:
                    fc_inner = data_now[7]
                    if fc_inner in (0x06, 0x10):
                        inner_len = 5
                    elif fc_inner == 0x03 and len(data_now) > 8:
                        inner_len = 2 + data_now[8]
                    else:
                        inner_len = 8
                    need = 7 + inner_len + 2  # 7 = header, 2 = CRC
                    if len(data_now) >= need:
                        break
                # Исключение: [addr, func|0x80, ex] + CRC
                if (func & 0x80) and len(data_now) >= 5:
                    break
                # Ответ записи (0x06/0x10): фиксировано 8 байт
                if func in (0x06, 0x10) and len(data_now) >= 8:
                    break
                # Ответ чтения (0x03): [addr, func, byte_count, data..., crc]
                if func == 0x03 and len(data_now) >= 3:
                    frame_len = 3 + data_now[2] + 2
                    if len(data_now) >= frame_len:
                        break
        else:
            # Выход по тишине только при полном Modbus-кадре (не по 5+ байтам), иначе теряем ответ или возвращаем обрезок → таймаут/ошибка.
            if chunks and (time.perf_counter() - last_recv) > 0.02:
                data_after_strip = b"".join(chunks)
                if len(request) > 0 and len(data_after_strip) >= len(request) and data_after_strip[:len(request)] == request:
                    data_after_strip = data_after_strip[len(request):]
                if len(data_after_strip) >= 5:
                    func = data_after_strip[1]
                    if len(data_after_strip) >= 8 and data_after_strip[0] == 0xFD and data_after_strip[1] == 0x46 and data_after_strip[2] == 0x09:
                        fc_inner = data_after_strip[7]
                        if fc_inner in (0x06, 0x10):
                            inner_len = 5
                        elif fc_inner == 0x03 and len(data_after_strip) > 8:
                            inner_len = 2 + data_after_strip[8]
                        else:
                            inner_len = 8
                        if len(data_after_strip) >= 7 + inner_len + 2:
                            break
                    if (func & 0x80) and len(data_after_strip) >= 5:
                        break
                    if func in (0x06, 0x10) and len(data_after_strip) >= 8:
                        break
                    if func == 0x03 and len(data_after_strip) >= 3:
                        frame_len = 3 + data_after_strip[2] + 2
                        if len(data_after_strip) >= frame_len:
                            break
            time.sleep(0.001)
    if not chunks:
        return None
    data = b"".join(chunks)
    # Убрать эхо своего запроса из начала буфера (некоторые USB-RS485 отдают TX в RX)
    if len(request) > 0 and len(data) >= len(request) and data[:len(request)] == request:
        data = data[len(request):]
    if len(data) < 5:
        return None  # после отсечения эха нет полного ответа
    return data


def _modbus_frame_length(data: bytes, offset: int) -> int:
    """Длина одного Modbus RTU кадра в data начиная с offset. 0 если не хватает данных или неизвестный тип."""
    d = data[offset:]
    if len(d) < 5:
        return 0
    func = d[1]
    if func & 0x80:
        return 5
    if func == 0x03 and len(d) >= 3:
        return 3 + d[2] + 2
    if func in (0x06, 0x10):
        return 8
    return 0


def _has_complete_frame(data: bytes) -> bool:
    """Есть ли в начале data хотя бы один полный Modbus RTU кадр (после отсечения эха)."""
    if len(data) < 5:
        return False
    func = data[1]
    if func & 0x80:
        return len(data) >= 5
    if func == 0x03 and len(data) >= 3:
        return len(data) >= 3 + data[2] + 2
    if func in (0x06, 0x10):
        return len(data) >= 8
    return False


def send_receive_all(
    ser: serial.Serial,
    request: bytes,
    response_timeout_ms: int = 2000,
    silence_ms: float = 35,
) -> List[Tuple[int, Optional[bytes]]]:
    """
    Один запрос (например broadcast) — принять ответы от всех устройств на линии.
    Читает до истечения response_timeout_ms и пока приходят данные; после тишины silence_ms мс
    разбирает все полные Modbus-кадры в буфере.
    Возвращает список (адрес_устройства, payload) для каждого принятого ответа.
    """
    ser.reset_input_buffer()
    ser.write(request)
    ser.flush()
    char_time = (1 + (ser.bytesize or 8) + (2 if ser.stopbits == 2 else 1)) / ser.baudrate
    if ser.parity and ser.parity != "N":
        char_time += 1 / ser.baudrate
    post_send_delay = max(0.001, min(0.02, char_time * 3.5 + 0.002))
    time.sleep(post_send_delay)
    deadline = time.perf_counter() + (response_timeout_ms / 1000.0)
    chunks = []
    last_recv = time.perf_counter()
    while time.perf_counter() < deadline:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting)
            chunks.append(chunk)
            last_recv = time.perf_counter()
        else:
            if chunks and (time.perf_counter() - last_recv) >= (silence_ms / 1000.0):
                data_so_far = b"".join(chunks)
                if len(request) > 0 and len(data_so_far) >= len(request) and data_so_far[: len(request)] == request:
                    data_so_far = data_so_far[len(request) :]
                if not data_so_far or _has_complete_frame(data_so_far):
                    break
            time.sleep(0.001)
    if not chunks:
        return []
    data = b"".join(chunks)
    if len(request) > 0 and len(data) >= len(request) and data[: len(request)] == request:
        data = data[len(request) :]
    if len(data) < 5:
        return []
    results: List[Tuple[int, Optional[bytes]]] = []
    offset = 0
    while offset < len(data):
        if data[offset] < 1 or data[offset] > 247:
            offset += 1
            continue
        addr, payload, err = modbus_rtu._parse_response_from(data, offset)
        if err is not None:
            offset += 1
            continue
        results.append((addr, payload))
        flen = _modbus_frame_length(data, offset)
        if flen <= 0:
            offset += 1
            continue
        offset += flen
    return results


# Таймаут ожидания одного ответа 0x03 после 0x01/0x02 (мс). Устройства с большим адресом (напр. WB 19) могут отвечать позже; после 0x02 в буфере может прийти 0x04 (эхо/артефакт) — игнорируем и ждём 0x03.
WB_EXT_SCAN_SINGLE_RESPONSE_MS = 420
# Максимум устройств за один цикл 0x01→0x02→…→0x04 (защита от зацикливания).
WB_EXT_SCAN_MAX_DEVICES_PER_CYCLE = 250


def send_receive_wb_ext_scan(
    ser: serial.Serial,
    response_timeout_ms: int = 1200,
    silence_ms: float = 50,
    try_legacy_0x60: bool = True,
    log_cb: Optional[Callable[[str], None]] = None,
) -> List[Tuple[int, int]]:
    """
    WB extended scan по протоколу WB: 0x01 (начало) → приём 0x03 → 0x02 (продолжение) → приём 0x03 → … → 0x04 (конец).
    Устройства отвечают по одному (арбитраж); без 0x02/0x04 находится только первое устройство.
    Возвращает список (modbus_addr, serial) без дубликатов по адресу.
    """
    def _log(msg: str) -> None:
        if log_cb:
            log_cb(msg)

    def _read_one_response(deadline: float) -> Optional[Tuple[int, int]]:
        chunks = []
        while time.perf_counter() < deadline:
            if ser.in_waiting:
                chunks.append(ser.read(ser.in_waiting))
            time.sleep(0.005)
        data = b"".join(chunks) if chunks else b""
        if data:
            _log("  Быстрый скан RX (%d байт): %s" % (len(data), data.hex()))
        i = data.find(0xFD) if data else -1
        while i >= 0 and i <= len(data) - modbus_rtu.WB_EXT_SCAN_FRAME_LEN:
            addr, serial, flen = modbus_rtu.parse_wb_ext_scan_response(data, i)
            if addr is not None:
                return (addr, serial or 0)
            # Уже ответившее устройство (напр. 4) на 0x02 может прислать 0x04 — пропускаем, ждём 0x03 от следующего (напр. 19).
            if i + 5 <= len(data) and data[i + 2] == modbus_rtu.WB_EXT_SCAN_END:
                i += 5
                continue
            i = data.find(0xFD, i + 1)
        return None

    def _run_cycle(legacy: bool) -> List[Tuple[int, int]]:
        cycle_result: List[Tuple[int, int]] = []
        single_ms = WB_EXT_SCAN_SINGLE_RESPONSE_MS
        req_start = modbus_rtu.build_wb_ext_scan_start(legacy=legacy)
        req_next = modbus_rtu.build_wb_ext_scan_next(legacy=legacy)
        req_end = modbus_rtu.build_wb_ext_scan_end(legacy=legacy)
        ser.reset_input_buffer()
        _log("  Быстрый скан TX (0x01): %s" % req_start.hex())
        ser.write(req_start)
        ser.flush()
        char_time = (1 + (ser.bytesize or 8) + (2 if ser.stopbits == 2 else 1)) / ser.baudrate
        if ser.parity and ser.parity != "N":
            char_time += 1 / ser.baudrate
        time.sleep(max(0.001, min(0.02, char_time * 3.5 + 0.002)))
        deadline = time.perf_counter() + (single_ms / 1000.0)
        r = _read_one_response(deadline)
        if r is not None:
            cycle_result.append(r)
            _log("  Быстрый скан кадр: адрес %d, серийный 0x%08X" % (r[0], r[1]))
        for _ in range(WB_EXT_SCAN_MAX_DEVICES_PER_CYCLE - 1):
            ser.reset_input_buffer()
            _log("  Быстрый скан TX (0x02): %s" % req_next.hex())
            ser.write(req_next)
            ser.flush()
            time.sleep(max(0.001, min(0.015, char_time * 3.5 + 0.002)))
            deadline = time.perf_counter() + (single_ms / 1000.0)
            r = _read_one_response(deadline)
            if r is None:
                break
            cycle_result.append(r)
            _log("  Быстрый скан кадр: адрес %d, серийный 0x%08X" % (r[0], r[1]))
        ser.write(req_end)
        ser.flush()
        _log("  Быстрый скан TX (0x04 end)")
        return cycle_result

    seen: set = set()
    result: List[Tuple[int, int]] = []
    for legacy in (False, True):
        if legacy and not try_legacy_0x60:
            continue
        partial = _run_cycle(legacy=legacy)
        for addr, serial in partial:
            if addr not in seen:
                seen.add(addr)
                result.append((addr, serial))
    return result
