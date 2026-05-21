#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Сборка MR-02m-Flasher.exe с помощью PyInstaller.
Запуск из корня проекта: python flasher_windows/build_exe.py
Или из папки flasher_windows: python build_exe.py (скрипт сам перейдёт в корень)
"""
import os
import subprocess
import sys
from pathlib import Path

def main():
    root = Path(__file__).resolve().parent.parent  # корень проекта
    os.chdir(root)
    spec = root / "flasher_windows" / "MR-02m-Flasher.spec"
    if not spec.exists():
        print("Не найден файл MR-02m-Flasher.spec")
        return 1
    try:
        import PyInstaller.__main__
    except ImportError:
        print("Установите PyInstaller: pip install pyinstaller")
        return 1
    # Запуск сборки: pyinstaller flasher_windows/MR-02m-Flasher.spec
    args = [
        sys.executable, "-m", "PyInstaller",
        "--noconfirm",
        "--clean",
        str(spec),
    ]
    r = subprocess.run(args, cwd=str(root))
    if r.returncode == 0:
        exe = root / "dist" / "MR-02m-Flasher.exe"
        print(f"\nГотово: {exe}")
        print("Файл прошивки MR-02m_<версия>.elf или .bin положите в папку dist/ рядом с exe.")
    return r.returncode

if __name__ == "__main__":
    sys.exit(main())
