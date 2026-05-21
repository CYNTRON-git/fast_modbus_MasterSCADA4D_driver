# -*- coding: utf-8 -*-
"""
Счётчик прошитых устройств: сохранение в файл и загрузка при старте.
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Dict

FILENAME = "flasher_count.json"


def _counter_path(app_dir: Path) -> Path:
    return app_dir / FILENAME


def load_counters(app_dir: Path) -> Dict[str, int]:
    """Загрузить счётчики: {"total": N, "6DO8DI": n1, "12AI": n2, ...}."""
    p = _counter_path(app_dir)
    if not p.exists():
        return {"total": 0}
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
        if isinstance(data, dict):
            return {k: int(v) for k, v in data.items() if isinstance(v, (int, float))}
    except (OSError, json.JSONDecodeError):
        pass
    return {"total": 0}


def save_counters(app_dir: Path, counters: Dict[str, int]) -> None:
    """Сохранить счётчики в файл."""
    p = _counter_path(app_dir)
    try:
        p.write_text(json.dumps(counters, indent=2, ensure_ascii=False), encoding="utf-8")
    except OSError:
        pass


def increment_flash_count(app_dir: Path, module_type: str) -> Dict[str, int]:
    """Увеличить общий счётчик и счётчик по типу модуля; сохранить и вернуть актуальные счётчики."""
    counters = load_counters(app_dir)
    counters["total"] = counters.get("total", 0) + 1
    key = (module_type or "NONE").strip().upper()
    counters[key] = counters.get(key, 0) + 1
    save_counters(app_dir, counters)
    return counters
