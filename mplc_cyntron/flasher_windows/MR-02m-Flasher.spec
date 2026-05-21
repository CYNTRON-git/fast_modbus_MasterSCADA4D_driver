# -*- mode: python ; coding: utf-8 -*-
# Сборка: из корня проекта выполнить:
#   pyinstaller flasher_windows/MR-02m-Flasher.spec
# Или: python flasher_windows/build_exe.py
#
# Результат: dist/MR-02m-Flasher.exe (один файл, без консоли)

# Скрипт и pathex заданы относительно папки, в которой лежит .spec (flasher_windows/)
a = Analysis(
    ['app_gui.py'],
    pathex=['..'],
    binaries=[],
    datas=[],
    hiddenimports=[
        'serial',
        'serial.tools.list_ports',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='MR-02m-Flasher',
    debug=False,
    bootloader_ignore_signatures=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=None,
)
