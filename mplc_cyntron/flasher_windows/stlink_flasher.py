# -*- coding: utf-8 -*-
"""
Прошивка и верификация через ST-Link (st-flash, прищепка).

Цикл автоматического режима: ожидание подключения цели → прошивка combined.bin
с серийным номером по 0x080000C4 (4 байта, little-endian) → верификация → лог → ожидание следующего.

Требуется: st-flash (stlink tools) в PATH, например из https://github.com/stlink-org/stlink
Команды: st-flash write <file> 0x08000000, st-flash read <file> 0x08000000 <size>
"""
from __future__ import annotations

import struct
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Optional, Tuple, Callable

# Серийный номер в образе: смещение от начала Flash (0x08000000) = 0xC4, 4 байта LE
SERIAL_OFFSET_IN_IMAGE = 0xC4
FLASH_START = 0x08000000
FLASH_SIZE_BYTES = 256 * 1024


def _run(cmd: list, timeout: int = 60, log: Optional[Callable[[str], None]] = None) -> Tuple[bool, str]:
    try:
        r = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            encoding="utf-8",
            errors="replace",
        )
        out = (r.stdout or "").strip() + "\n" + (r.stderr or "").strip()
        if log and out:
            log(out)
        return r.returncode == 0, out
    except FileNotFoundError:
        msg = f"Команда не найдена: {cmd[0]}. Установите stlink (st-flash) и добавьте в PATH."
        if log:
            log(msg)
        return False, msg
    except subprocess.TimeoutExpired:
        msg = "Таймаут выполнения st-flash."
        if log:
            log(msg)
        return False, msg
    except Exception as e:
        msg = str(e)
        if log:
            log(msg)
        return False, msg


def find_stflash() -> Optional[str]:
    """Проверить наличие st-flash в PATH."""
    ok, _ = _run(["st-flash", "--version"], timeout=5)
    return "st-flash" if ok else None


def detect_target(log: Optional[Callable[[str], None]] = None) -> bool:
    """Проверить, видит ли ST-Link подключённую цель (чип)."""
    # Чтение 4 байт с начала Flash — при отсутствии цели команда завершится с ошибкой
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        tmp = f.name
    try:
        ok, _ = _run(["st-flash", "read", tmp, hex(FLASH_START), "4"], timeout=10, log=log)
        return ok
    finally:
        try:
            Path(tmp).unlink(missing_ok=True)
        except OSError:
            pass


def inject_serial_into_bin(template_bin: bytes, serial: int) -> bytes:
    """Подставить серийный номер (4 байта, little-endian) по смещению 0xC4 в образе."""
    if len(template_bin) < SERIAL_OFFSET_IN_IMAGE + 4:
        raise ValueError("Образ слишком короткий для серийного номера (нужно хотя бы 0xC8 байт)")
    buf = bytearray(template_bin)
    struct.pack_into("<I", buf, SERIAL_OFFSET_IN_IMAGE, serial & 0xFFFFFFFF)
    return bytes(buf)


def program_and_verify(
    combined_bin_path: Path,
    serial: int,
    log: Optional[Callable[[str], None]] = None,
) -> Tuple[bool, str]:
    """
    Записать образ с серийным номером по 0x08000000 и проверить запись по адресу 0xC4.
    Возвращает (успех, сообщение).
    """
    if not combined_bin_path.exists():
        return False, f"Файл не найден: {combined_bin_path}"
    template = combined_bin_path.read_bytes()
    if len(template) != FLASH_SIZE_BYTES:
        return False, f"Ожидается образ 256 КБ, получено {len(template)} байт"
    try:
        image = inject_serial_into_bin(template, serial)
    except ValueError as e:
        return False, str(e)

    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(image)
        f.flush()
        to_flash = Path(f.name)
    try:
        if log:
            log(f"Прошивка: {to_flash} (серийный 0x{serial:08X}) по 0x{FLASH_START:X}...")
        ok, out = _run(
            ["st-flash", "write", str(to_flash), hex(FLASH_START)],
            timeout=120,
            log=log,
        )
        if not ok:
            return False, f"Ошибка записи: {out}"

        # Верификация: читаем 0x200 байт с 0x08000000 и проверяем байты по 0xC4
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as rf:
            read_back_path = Path(rf.name)
        try:
            ok2, out2 = _run(
                ["st-flash", "read", str(read_back_path), hex(FLASH_START), "0x200"],
                timeout=30,
                log=log,
            )
            if not ok2:
                return False, f"Ошибка чтения при верификации: {out2}"
            read_data = read_back_path.read_bytes()
            if len(read_data) < SERIAL_OFFSET_IN_IMAGE + 4:
                return False, "Прочитано слишком мало данных для верификации"
            read_serial = struct.unpack_from("<I", read_data, SERIAL_OFFSET_IN_IMAGE)[0]
            if read_serial != (serial & 0xFFFFFFFF):
                return False, f"Верификация: ожидался серийный 0x{serial:08X}, в Flash 0x{read_serial:08X}"
        finally:
            read_back_path.unlink(missing_ok=True)

        if log:
            log(f"Верификация OK: серийный 0x{serial:08X}")
        return True, f"Прошито и проверено, серийный 0x{serial:08X}"
    finally:
        to_flash.unlink(missing_ok=True)
