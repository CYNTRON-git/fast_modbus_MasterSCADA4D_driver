# -*- coding: utf-8 -*-
"""
Windows GUI for MP-02m / MR-02m firmware update (RS-485, Modbus RTU).
Файл прошивки: .fw или .bin в папке программы.
"""
import sys
import os
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import Optional, List, Callable, Tuple
import tkinter as tk
from tkinter import ttk, messagebox, filedialog, scrolledtext

# Determine application directory (where exe or script lives); ensure imports work
if getattr(sys, "frozen", False):
    APP_DIR = Path(sys.executable).parent
else:
    _flasher_dir = Path(__file__).resolve().parent
    APP_DIR = _flasher_dir.parent  # project root (каталог, в котором лежит папка flasher_windows)
    _root = os.path.normpath(os.path.abspath(APP_DIR))
    # При запуске из flasher_windows/ в sys.path попадает только flasher_windows; для "from flasher_windows.xxx" нужен корень проекта
    _in_path = any(os.path.normpath(os.path.abspath(p)) == _root for p in sys.path)
    if not _in_path:
        sys.path.insert(0, _root)

from flasher_windows.serial_port import list_com_ports, open_port, send_receive, send_receive_wb_ext_scan
from flasher_windows import modbus_rtu
from flasher_windows.firmware import (
    find_firmware_files,
    load_firmware,
    load_bootloader_image,
    parse_version_from_filename,
    FLASH_APP_START,
    MAX_FIRMWARE_SIZE,
    BL_IMAGE_TOTAL_BYTES,
)
from flasher_windows.scanner import (
    scan_all,
    DeviceInfo,
    SCAN_BAUDRATES,
    SCAN_LINK_OPTIONS,
    SCAN_ADDR_MIN_DEFAULT,
    SCAN_ADDR_MAX_DEFAULT,
    SpeedConfig,
)
from flasher_windows.flash_protocol import (
    FlasherProtocol,
    BOOTLOADER_BAUDRATE,
    BOOTLOADER_DEFAULT_ADDR,
    BOOTLOADER_PARITY,
    BOOTLOADER_STOPBITS,
    DATA_BLOCK_BYTES,
    DEFAULT_SIGNATURE,
    REG_JUMP_APP,
    REG_SIGNATURE,
    REG_WB_EEPROM_ERASE,
    run_flash_sequence,
    run_flash_sequence_bootloader,
    run_flash_sequence_by_address,
    run_flash_bootloader_sequence_by_address,
    run_flash_sequence_wb,
    BOOTLOADER_BAUDRATE_WB,
    BOOTLOADER_STOPBITS_WB,
    BOOTLOADER_INFOBLOCK_TIMEOUT_MS_WB,
    BOOTLOADER_DATA_BLOCK_TIMEOUT_MS,
)

FAST_RESPONSE_TIMEOUT_MS = 500
INFO_RESPONSE_TIMEOUT_MS = 6000   # ответ на info-блок (0x1000): запас на RS-485 и обработку в бутлоадере
# Блоки данных 0x2000: минимальный достаточный таймаут для ускорения прошивки (115200 ≈ 20 мс на обмен + запись Flash).
DATA_BLOCK_RESPONSE_TIMEOUT_MS = 600  # ответ на блок данных (запись Flash может занять время)


class FlasherApp:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("Прошивка Модулей расширения ЦИНТРОН МР-02м")
        self.root.minsize(680, 420)
        # Иконка окна: favicon.ico в папке flasher_windows (или рядом с exe при сборке)
        for base in (Path(__file__).resolve().parent, APP_DIR):
            icon_path = base / "favicon.ico"
            if icon_path.is_file():
                try:
                    self.root.iconbitmap(str(icon_path))
                except Exception:
                    pass
                break

        self.devices: List[DeviceInfo] = []
        self.firmware_path: Optional[Path] = None
        self.firmware_image: Optional[bytes] = None
        self.firmware_size = 0
        self.firmware_version = ""
        self.firmware_signature_from_file: Optional[str] = None  # из load_firmware для .fw/.wbfw
        self.firmware_is_bootloader = False
        self.signature = DEFAULT_SIGNATURE
        self.cancel_requested = False
        self.scan_in_progress = False
        self.worker_thread: Optional[threading.Thread] = None

        self._log_file = None
        try:
            log_path = APP_DIR / "flasher_log.txt"
            self._log_file = open(log_path, "a", encoding="utf-8", errors="replace")
        except OSError:
            pass

        self._build_ui()
        def _on_close() -> None:
            if self._log_file is not None:
                try:
                    self._log_file.close()
                except OSError:
                    pass
                self._log_file = None
            self.root.destroy()
        self.root.protocol("WM_DELETE_WINDOW", _on_close)
        self._refresh_ports()
        self._refresh_firmware_list()
        self._size_window_to_content()

    def _log(self, msg: str) -> None:
        def _():
            ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            line = f"[{ts}] {msg}\n"
            self.log_text.insert(tk.END, line)
            self.log_text.see(tk.END)
            if self._log_file is not None:
                try:
                    self._log_file.write(line)
                    self._log_file.flush()
                except OSError:
                    pass
        self.root.after(0, _)

    def _size_window_to_content(self) -> None:
        """Установить размер окна по содержимому (без лишнего пустого места)."""
        self.root.update_idletasks()
        w = self.root.winfo_reqwidth()
        h = self.root.winfo_reqheight()
        # Учесть рамку и заголовок окна
        self.root.geometry(f"{max(720, w + 24)}x{max(480, h + 24)}")
        self.root.resizable(True, True)

    def _build_ui(self) -> None:
        PAD = 8
        main = ttk.Frame(self.root, padding=PAD)
        main.pack(fill=tk.BOTH, expand=True)

        # === Слева: только таблица найденных устройств ===
        left_f = ttk.Frame(main)
        left_f.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        tree_head_f = ttk.Frame(left_f)
        tree_head_f.pack(fill=tk.X, pady=(0, 2))
        ttk.Label(tree_head_f, text="Найденные устройства:").pack(side=tk.LEFT)
        self.btn_save_devices = ttk.Button(tree_head_f, text="Сохранить список", command=self._save_devices_list)
        self.btn_save_devices.pack(side=tk.LEFT, padx=(PAD, 0))
        columns = ("addr", "baud", "parity", "stop", "signature", "app_ver", "bl_ver", "serial")
        self.tree = ttk.Treeview(left_f, columns=columns, show="headings", height=12, selectmode="browse")
        headings = {"addr": "Адрес", "baud": "Скорость", "parity": "Чётность", "stop": "Стоп",
                    "signature": "Сигнатура", "app_ver": "ver. prog", "bl_ver": "ver. bootloader", "serial": "Серийный №"}
        widths = {"addr": 52, "baud": 68, "parity": 56, "stop": 44, "signature": 100, "app_ver": 88, "bl_ver": 96, "serial": 100}
        for c in columns:
            self.tree.heading(c, text=headings[c])
            self.tree.column(c, width=widths[c], anchor="center")
        tree_scroll = ttk.Scrollbar(left_f, orient=tk.VERTICAL, command=self.tree.yview)
        self.tree.configure(yscrollcommand=tree_scroll.set)
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, pady=(0, PAD))
        tree_scroll.pack(side=tk.RIGHT, fill=tk.Y, pady=(0, PAD))
        self.tree.bind("<<TreeviewSelect>>", self._on_device_select)
        self.tree.bind("<Double-1>", self._on_device_double_click)

        # === Справа: блок управления ===
        right_f = ttk.Frame(main)
        right_f.pack(side=tk.RIGHT, fill=tk.BOTH, padx=(PAD, 0))

        # --- Линия RS-485 ---
        line_f = ttk.LabelFrame(right_f, text=" Линия RS-485 ", padding=(PAD, PAD // 2))
        line_f.pack(fill=tk.X, pady=(0, PAD))
        port_f = ttk.Frame(line_f)
        port_f.pack(fill=tk.X)
        ttk.Label(port_f, text="COM-порт:").pack(side=tk.LEFT, padx=(0, 4))
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(port_f, textvariable=self.port_var, width=12, state="readonly")
        self.port_combo.pack(side=tk.LEFT, padx=(0, PAD))
        ttk.Button(port_f, text="Обновить порты", command=self._refresh_ports).pack(side=tk.LEFT, padx=(0, PAD))
        self.btn_scan = ttk.Button(port_f, text="Сканировать", command=self._start_scan)
        self.btn_scan.pack(side=tk.LEFT)
        scan_opts_f = ttk.LabelFrame(line_f, text=" Параметры сканирования ", padding=(PAD, PAD // 2))
        scan_opts_f.pack(fill=tk.X, pady=(PAD // 2, 0))
        scan_row1 = ttk.Frame(scan_opts_f)
        scan_row1.pack(fill=tk.X)
        ttk.Label(scan_row1, text="Скорости:").pack(side=tk.LEFT, padx=(0, 8))
        # По умолчанию: 9600, 19200, 115200 и 8N1
        default_bauds = {9600, 19200, 115200}
        self.scan_baud_vars: dict = {}
        for baud in SCAN_BAUDRATES:
            v = tk.BooleanVar(value=(baud in default_bauds))
            self.scan_baud_vars[baud] = v
            ttk.Checkbutton(scan_row1, text=str(baud), variable=v).pack(side=tk.LEFT, padx=(0, 4))
        scan_row2 = ttk.Frame(scan_opts_f)
        scan_row2.pack(fill=tk.X, pady=(2, 0))
        ttk.Label(scan_row2, text="Параметры связи:").pack(side=tk.LEFT, padx=(0, 8))
        self.scan_link_vars: dict = {}
        for (parity, stopbits, label) in SCAN_LINK_OPTIONS:
            key = (parity, stopbits)
            v = tk.BooleanVar(value=(key == ("N", 1)))  # по умолчанию только 8N1
            self.scan_link_vars[key] = v
            ttk.Checkbutton(scan_row2, text=label, variable=v).pack(side=tk.LEFT, padx=(0, 4))
        scan_row3 = ttk.Frame(scan_opts_f)
        scan_row3.pack(fill=tk.X, pady=(2, 0))
        ttk.Label(scan_row3, text="Диапазон адресов:").pack(side=tk.LEFT, padx=(0, 8))
        self.scan_addr_min_var = tk.StringVar(value=str(SCAN_ADDR_MIN_DEFAULT))
        self.scan_addr_max_var = tk.StringVar(value=str(SCAN_ADDR_MAX_DEFAULT))
        ttk.Label(scan_row3, text="от").pack(side=tk.LEFT, padx=(0, 2))
        ttk.Entry(scan_row3, textvariable=self.scan_addr_min_var, width=4).pack(side=tk.LEFT, padx=(0, 8))
        ttk.Label(scan_row3, text="до").pack(side=tk.LEFT, padx=(0, 2))
        ttk.Entry(scan_row3, textvariable=self.scan_addr_max_var, width=4).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Label(scan_row3, text="(1–247)").pack(side=tk.LEFT)
        scan_row4 = ttk.Frame(scan_opts_f)
        scan_row4.pack(fill=tk.X, pady=(2, 0))
        self.scan_fast_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            scan_row4,
            text="Быстрое сканирование (0xFD 0x46 0x01 по выбранным скоростям и параметрам связи)",
            variable=self.scan_fast_var,
        ).pack(side=tk.LEFT)

        # --- Обновление ---
        upd_f = ttk.LabelFrame(right_f, text=" Обновление ", padding=(PAD, PAD // 2))
        upd_f.pack(fill=tk.X, pady=(0, PAD))
        mode_f = ttk.Frame(upd_f)
        mode_f.pack(fill=tk.X, pady=2)
        ttk.Label(mode_f, text="Режим:").pack(side=tk.LEFT, padx=(0, PAD))
        self.update_mode_var = tk.StringVar(value="app")
        ttk.Radiobutton(mode_f, text="Прошивка приложения", variable=self.update_mode_var, value="app", command=self._on_update_mode_change).pack(side=tk.LEFT, padx=(0, 8))
        ttk.Radiobutton(mode_f, text="Прошивка бутлоадера", variable=self.update_mode_var, value="bootloader", command=self._on_update_mode_change).pack(side=tk.LEFT)
        dev_f = ttk.Frame(upd_f)
        dev_f.pack(fill=tk.X, pady=2)
        ttk.Label(dev_f, text="Устройство (адрес или серийный №):").pack(side=tk.LEFT, padx=(0, PAD))
        self.device_var = tk.StringVar()
        self.device_entry = ttk.Entry(dev_f, textvariable=self.device_var, width=18)
        self.device_entry.pack(side=tk.LEFT, padx=(0, PAD))
        # Явно разрешить вставку из буфера (Ctrl+V) в поле устройства
        self.device_entry.bind("<Control-v>", self._on_device_entry_paste)
        self.device_entry.bind("<Control-V>", self._on_device_entry_paste)
        fw_f = ttk.Frame(upd_f)
        fw_f.pack(fill=tk.X, pady=2)
        ttk.Label(fw_f, text="Файл прошивки:").pack(side=tk.LEFT, padx=(0, PAD))
        self.fw_var = tk.StringVar()
        self.fw_label = ttk.Label(fw_f, textvariable=self.fw_var, foreground="gray")
        self.fw_label.pack(side=tk.LEFT, padx=(0, PAD))
        ttk.Button(fw_f, text="Выбрать…", command=self._choose_firmware).pack(side=tk.LEFT, padx=(0, PAD))
        sig_f = ttk.Frame(upd_f)
        sig_f.pack(fill=tk.X, pady=2)
        ttk.Label(sig_f, text="Сигнатура:").pack(side=tk.LEFT, padx=(0, PAD))
        self.sig_var = tk.StringVar(value=DEFAULT_SIGNATURE)
        ttk.Entry(sig_f, textvariable=self.sig_var, width=14).pack(side=tk.LEFT)
        self.multi_bootloader_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            upd_f,
            text="Несколько устройств в загрузчике (выбор по серийному №)",
            variable=self.multi_bootloader_var,
        ).pack(anchor=tk.W, pady=2)
        self.recovery_mode_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            upd_f,
            text="Восстановление (устройство уже в загрузчике, без команды перехода; для WB — 9600 8N2)",
            variable=self.recovery_mode_var,
        ).pack(anchor=tk.W, pady=2)

        # --- Серийный № при прошивке выбранного + параметры после прошивки ---
        after_f = ttk.LabelFrame(right_f, text=" После прошивки ", padding=(PAD, PAD // 2))
        after_f.pack(fill=tk.X, pady=(0, PAD))
        ser_after_row = ttk.Frame(after_f)
        ser_after_row.pack(fill=tk.X, pady=2)
        ttk.Label(ser_after_row, text="Серийный № (при прошивке выбранного):").pack(side=tk.LEFT, padx=(0, 4))
        self.serial_after_var = tk.StringVar()
        ttk.Entry(ser_after_row, textvariable=self.serial_after_var, width=14).pack(side=tk.LEFT, padx=(0, PAD))
        ttk.Label(ser_after_row, text="(пусто — не менять)").pack(side=tk.LEFT)
        link_after_row = ttk.Frame(after_f)
        link_after_row.pack(fill=tk.X, pady=2)
        self.apply_link_after_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            link_after_row, text="Записать параметры связи в устройство (рег. 110–112, 128):",
            variable=self.apply_link_after_var,
        ).pack(side=tk.LEFT, padx=(0, PAD))
        ttk.Label(link_after_row, text="Скорость:").pack(side=tk.LEFT, padx=(PAD, 2))
        self.link_speed_var = tk.StringVar(value="9600")
        link_speed_combo = ttk.Combobox(link_after_row, textvariable=self.link_speed_var, values=["9600", "19200", "38400", "115200"], width=8, state="readonly")
        link_speed_combo.pack(side=tk.LEFT, padx=(0, 4))
        ttk.Label(link_after_row, text="Чётность:").pack(side=tk.LEFT, padx=(4, 2))
        self.link_parity_var = tk.StringVar(value="N")
        ttk.Combobox(link_after_row, textvariable=self.link_parity_var, values=["N", "E", "O"], width=4, state="readonly").pack(side=tk.LEFT, padx=(0, 4))
        ttk.Label(link_after_row, text="Стоп:").pack(side=tk.LEFT, padx=(4, 2))
        self.link_stop_var = tk.StringVar(value="1")
        ttk.Combobox(link_after_row, textvariable=self.link_stop_var, values=["1", "2"], width=3, state="readonly").pack(side=tk.LEFT)
        ttk.Label(link_after_row, text="Адрес:").pack(side=tk.LEFT, padx=(4, 2))
        self.link_addr_var = tk.StringVar(value="")
        ttk.Entry(link_after_row, textvariable=self.link_addr_var, width=4).pack(side=tk.LEFT, padx=(0, 2))
        ttk.Label(link_after_row, text="(1–247, пусто — не менять)").pack(side=tk.LEFT, padx=(0, 4))

        factory_row = ttk.Frame(after_f)
        factory_row.pack(fill=tk.X, pady=2)
        self.btn_factory_reset = ttk.Button(
            factory_row,
            text="Сброс до заводских настроек",
            command=self._factory_reset,
        )
        self.btn_factory_reset.pack(side=tk.LEFT)
        ttk.Label(factory_row, text="(рег. 1001 → EEPROM, применится в устройстве без перезагрузки)").pack(side=tk.LEFT, padx=(PAD, 0))
        self.btn_write_link = ttk.Button(
            factory_row, text="Записать параметры связи (без прошивки)",
            command=self._write_link_params_only,
        )
        def _toggle_btn_write_link(*args) -> None:
            if self.apply_link_after_var.get():
                self.btn_write_link.pack(side=tk.LEFT, padx=(PAD, 0))
            else:
                self.btn_write_link.pack_forget()
        self.apply_link_after_var.trace_add("write", _toggle_btn_write_link)
        _toggle_btn_write_link()

        # --- Серийные при «Обновить все» ---
        serial_f = ttk.LabelFrame(right_f, text=" Серийные при «Обновить все» ", padding=(PAD, PAD // 2))
        serial_f.pack(fill=tk.X, pady=(0, PAD))
        serial_row = ttk.Frame(serial_f)
        serial_row.pack(fill=tk.X, pady=2)
        self.serial_assign_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            serial_row, text="Присваивать по порядку с №:",
            variable=self.serial_assign_var,
        ).pack(side=tk.LEFT, padx=(0, PAD))
        self.serial_start_var = tk.StringVar(value="1")
        ttk.Entry(serial_row, textvariable=self.serial_start_var, width=12).pack(side=tk.LEFT, padx=(0, PAD))
        self.btn_assign_serials = ttk.Button(
            serial_row, text="Только присвоить серийные",
            command=self._start_assign_serials_only,
        )
        self.btn_assign_serials.pack(side=tk.LEFT)

        # --- Кнопки действий ---
        btn_f = ttk.Frame(right_f)
        btn_f.pack(fill=tk.X, pady=(0, PAD))
        self.btn_update = ttk.Button(btn_f, text="Обновить выбранное", command=self._start_update_one)
        self.btn_update.pack(side=tk.LEFT, padx=(0, PAD))
        self.btn_update_all = ttk.Button(btn_f, text="Обновить все", command=self._start_update_all)
        self.btn_update_all.pack(side=tk.LEFT, padx=(0, PAD))
        self.btn_cancel = ttk.Button(btn_f, text="Отмена", command=self._request_cancel, state=tk.DISABLED)
        self.btn_cancel.pack(side=tk.LEFT)

        # === Снизу справа: прогресс и лог ===
        bottom_f = ttk.Frame(right_f)
        bottom_f.pack(fill=tk.BOTH, expand=True)
        self.progress_var = tk.DoubleVar()
        self.progress_time_var = tk.StringVar()
        self.progress_label_var = tk.StringVar()
        ttk.Label(bottom_f, textvariable=self.progress_time_var).pack(anchor=tk.W, pady=(2, 0))
        ttk.Label(bottom_f, textvariable=self.progress_label_var).pack(anchor=tk.W, pady=(2, 0))
        self.progress = ttk.Progressbar(bottom_f, variable=self.progress_var, maximum=100)
        self.progress.pack(fill=tk.X, pady=(2, 4))
        log_head_f = ttk.Frame(bottom_f)
        log_head_f.pack(fill=tk.X, pady=(0, 2))
        ttk.Label(log_head_f, text="Лог:").pack(side=tk.LEFT)
        ttk.Button(log_head_f, text="Очистить", command=lambda: self.log_text.delete("1.0", tk.END)).pack(side=tk.RIGHT, padx=(8, 0))
        self.log_text = scrolledtext.ScrolledText(bottom_f, height=10, wrap=tk.WORD, state=tk.NORMAL, font=("Consolas", 9))
        self.log_text.pack(fill=tk.BOTH, expand=True)
        def _log_key(e):
            if (e.state & 0x4) and e.keysym.lower() in ("c", "a"):
                return
            return "break"
        self.log_text.bind("<KeyPress>", _log_key)
        # Явно: Ctrl+C — копировать в буфер
        def _log_copy(e):
            self.log_text.event_generate("<<Copy>>")
            return "break"
        self.log_text.bind("<Control-c>", _log_copy)
        self.log_text.bind("<Control-C>", _log_copy)
        self._log_menu = tk.Menu(self.log_text, tearoff=0)
        self._log_menu.add_command(label="Копировать", command=lambda: self.log_text.event_generate("<<Copy>>"))
        self._log_menu.add_command(label="Выделить всё", command=lambda: self.log_text.tag_add(tk.SEL, "1.0", tk.END))
        self.log_text.bind("<Button-3>", lambda e: self._log_menu.tk_popup(e.x_root, e.y_root))

    def _refresh_ports(self) -> None:
        ports = list_com_ports()
        self.port_combo["values"] = [p[0] for p in ports]
        if ports and not self.port_var.get():
            self.port_var.set(ports[0][0])

    def _refresh_firmware_list(self) -> None:
        found = find_firmware_files(APP_DIR)
        if not found:
            self.fw_var.set("Файл не найден. Поместите .fw или .bin в папку программы.")
            self.firmware_path = None
            self.firmware_image = None
            self.firmware_signature_from_file = None
            return
        # Prefer .fw, then .bin, then by mtime
        def order(p):
            suf = p[0].suffix.lower()
            return (0 if suf == ".fw" else 1, -p[0].stat().st_mtime)
        found.sort(key=order)
        self.firmware_path = found[0][0]
        self.fw_var.set(f"{self.firmware_path.name}  (загрузка…)")
        self._load_firmware_file()

    def _on_update_mode_change(self) -> None:
        self.firmware_image = None
        self.firmware_signature_from_file = None
        self.firmware_path = None
        self.firmware_is_bootloader = self.update_mode_var.get() == "bootloader"
        if self.firmware_is_bootloader:
            self.fw_var.set("Выберите .fw или .bin бутлоадера (34 КБ)")
        else:
            self._refresh_firmware_list()

    def _is_bootloader_image_file(self, path: Path) -> bool:
        """Файл по размеру и расширению похож на образ бутлоадера (34 КБ)."""
        if path.suffix.lower() not in (".fw", ".bin"):
            return False
        try:
            return path.stat().st_size == BL_IMAGE_TOTAL_BYTES
        except OSError:
            return False

    def _load_firmware_file(self) -> bool:
        if self.firmware_path is None or not self.firmware_path.exists():
            return False
        try:
            if self.firmware_is_bootloader:
                self.firmware_image = load_bootloader_image(self.firmware_path)
                self.firmware_size = len(self.firmware_image)
                self.firmware_version = parse_version_from_filename(self.firmware_path.name) or "?"
                self.fw_var.set(self.firmware_path.name)
                return True
            # Режим приложения: при выборе файла 34 КБ (.fw/.bin) — авто-переключение на бутлоадер без ошибки
            if self._is_bootloader_image_file(self.firmware_path):
                self.update_mode_var.set("bootloader")
                self.firmware_is_bootloader = True
                self.firmware_image = load_bootloader_image(self.firmware_path)
                self.firmware_size = len(self.firmware_image)
                self.firmware_version = parse_version_from_filename(self.firmware_path.name) or "?"
                self.fw_var.set(self.firmware_path.name)
                self._log("Выбран файл образа бутлоадера (34 КБ), переключено на режим «Прошивка бутлоадера».")
                return True
            self.firmware_image, self.firmware_size, self.firmware_version, sig_override = load_firmware(
                self.firmware_path
            )
            self.firmware_signature_from_file = sig_override
            self.fw_var.set(self.firmware_path.name)
            self._log("Загружен образ: %s, размер %d байт, версия (из образа): %s." % (
                self.firmware_path.name, self.firmware_size, self.firmware_version or "?"
            ))
            # Не сбрасывать сигнатуру, если пользователь уже выбрал устройство в таблице (поле заполнено)
            if sig_override is not None:
                current_sig = (self.sig_var.get() or "").strip()
                if not current_sig or current_sig.upper() == "NONE":
                    self.sig_var.set(sig_override)
            return True
        except Exception as e:
            err_str = str(e)
            # Режим бутлоадера, но файл — образ приложения (Reset_Handler в 0x0800800..0x08037FFF): переключить на приложение
            if self.firmware_is_bootloader and isinstance(e, ValueError) and "не в области кода бутлоадера" in err_str:
                try:
                    self.firmware_image, self.firmware_size, self.firmware_version, sig_override = load_firmware(
                        self.firmware_path
                    )
                    self.firmware_signature_from_file = sig_override
                    self.update_mode_var.set("app")
                    self.firmware_is_bootloader = False
                    self.fw_var.set(self.firmware_path.name)
                    if sig_override is not None:
                        current_sig = (self.sig_var.get() or "").strip()
                        if not current_sig or current_sig.upper() == "NONE":
                            self.sig_var.set(sig_override)
                    self._log("Выбран файл прошивки приложения — переключено на режим «Прошивка приложения».")
                    return True
                except Exception:
                    pass
            # Повторная попытка: если ошибка из-за размера в .fw и файл 34 КБ — это образ бутлоадера
            if isinstance(e, ValueError) and self._is_bootloader_image_file(self.firmware_path):
                try:
                    self.update_mode_var.set("bootloader")
                    self.firmware_is_bootloader = True
                    self.firmware_image = load_bootloader_image(self.firmware_path)
                    self.firmware_size = len(self.firmware_image)
                    self.firmware_version = parse_version_from_filename(self.firmware_path.name) or "?"
                    self.fw_var.set(self.firmware_path.name)
                    self._log("Выбран файл образа бутлоадера (34 КБ), переключено на режим «Прошивка бутлоадера».")
                    return True
                except Exception:
                    pass
            self._log(f"Ошибка загрузки прошивки: {e}")
            messagebox.showerror("Ошибка", err_str)
            return False

    def _choose_firmware(self) -> None:
        if self.firmware_is_bootloader:
            default_dir = APP_DIR / "bootloader" / "build"
            if not default_dir.is_dir():
                default_dir = APP_DIR / "bootloader"
            if not default_dir.is_dir():
                default_dir = APP_DIR
            path = filedialog.askopenfilename(
                title="Выберите образ бутлоадера (.fw или .bin, 34 КБ)",
                initialdir=default_dir,
                filetypes=[
                    ("Образ бутлоадера (.fw, .bin)", "*.fw;*.bin"),
                    ("FW", "*.fw"),
                    ("BIN", "*.bin"),
                    ("Все", "*.*"),
                ],
            )
        else:
            default_dir = APP_DIR / "build" / "AppBoot"
            if not default_dir.is_dir():
                default_dir = APP_DIR / "AppBoot"
            if not default_dir.is_dir():
                default_dir = APP_DIR.parent / "build" / "AppBoot"
            if not default_dir.is_dir():
                default_dir = APP_DIR
            path = filedialog.askopenfilename(
                title="Выберите файл прошивки",
                initialdir=default_dir,
                filetypes=[
                    ("Прошивка (.fw, .bin, .wbfw)", "*.fw;*.bin;*.wbfw"),
                    ("FW", "*.fw"),
                    ("BIN", "*.bin"),
                    ("Wiren Board (.wbfw)", "*.wbfw"),
                    ("Все", "*.*"),
                ],
            )
        if not path:
            return
        self.firmware_path = Path(path)
        self.fw_var.set(f"{self.firmware_path.name}  (загрузка…)")
        self._load_firmware_file()

    def _clear_table(self) -> None:
        for i in self.tree.get_children():
            self.tree.delete(i)

    def _save_devices_list(self) -> None:
        """Сохранить таблицу найденных устройств в текстовый файл в корень программы с выравниванием колонок."""
        if not self.devices:
            messagebox.showinfo("Список пуст", "Нет найденных устройств для сохранения. Выполните сканирование.")
            return
        path = APP_DIR / "devices_list.txt"
        headers = ("Адрес", "Скорость", "Чётность", "Стоп-биты", "Сигнатура", "Версия прошивки", "Версия загрузчика", "Серийный №")
        rows = []
        for d in sorted(self.devices, key=lambda x: (x.address, x.baudrate)):
            ser_str = f"0x{d.serial:08X}" if (d.serial and d.serial != 0xFFFFFFFF) else "—"
            rows.append((str(d.address), str(d.baudrate), d.parity, str(d.stopbits), d.signature or "—", d.app_version, d.bootloader_version, ser_str))
        col_widths = [len(h) for h in headers]
        for row in rows:
            for i, cell in enumerate(row):
                if i < len(col_widths):
                    col_widths[i] = max(col_widths[i], len(cell))
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(" ".join(h.ljust(col_widths[i]) for i, h in enumerate(headers)) + "\n")
                for row in rows:
                    f.write(" ".join((row[i] if i < len(row) else "").ljust(col_widths[i]) for i in range(len(headers))) + "\n")
            self._log(f"Список устройств сохранён: {path}")
            messagebox.showinfo("Готово", f"Список сохранён в файл:\n{path}")
        except OSError as e:
            self._log(f"Ошибка сохранения списка: {e}")
            messagebox.showerror("Ошибка", f"Не удалось сохранить файл:\n{e}")

    def _add_device_to_table(self, d: DeviceInfo) -> None:
        """Добавить или обновить устройство в таблице (вызывать из main thread). По ключу (адрес, скорость, чётность, стоп) — обновление строки при получении полных данных."""
        vals = (
            d.address,
            d.baudrate,
            d.parity,
            d.stopbits,
            d.signature or "—",
            d.app_version,
            d.bootloader_version,
            f"0x{d.serial:08X}" if d.serial else "—",
        )
        for i, child in enumerate(self.tree.get_children()):
            row = self.tree.item(child)["values"]
            if len(row) >= 4:
                try:
                    r_addr = int(row[0]) if row[0] is not None else None
                    r_baud = int(row[1]) if row[1] is not None else None
                    r_stop = int(row[3]) if row[3] is not None else None
                except (TypeError, ValueError):
                    r_addr = r_baud = r_stop = None
                if (r_addr, r_baud, row[2], r_stop) == (d.address, d.baudrate, d.parity, d.stopbits):
                    if i < len(self.devices):
                        self.devices[i] = d
                    self.tree.item(child, values=vals)
                    self.root.update_idletasks()
                    return
        self.devices.append(d)
        self.tree.insert("", tk.END, values=vals)
        self.root.update_idletasks()

    def _fill_table(self) -> None:
        self._clear_table()
        for d in self.devices:
            self.tree.insert("", tk.END, values=(
                d.address,
                d.baudrate,
                d.parity,
                d.stopbits,
                d.signature or "—",
                d.app_version,
                d.bootloader_version,
                f"0x{d.serial:08X}" if d.serial else "—",
            ))

    def _on_device_select(self, ev: tk.Event) -> None:
        sel = self.tree.selection()
        if not sel:
            return
        item = self.tree.item(sel[0])
        vals = item["values"]
        # По умолчанию подставляем сетевой адрес устройства (не серийный номер)
        if len(vals) >= 1:
            self.device_var.set(str(vals[0]))
        # Подставляем сигнатуру найденного по Modbus устройства (колонка "Сигнатура")
        if len(vals) >= 5:
            if vals[4] and str(vals[4]).strip() and str(vals[4]) != "—":
                self.sig_var.set(str(vals[4]).strip()[:12])
            else:
                self.sig_var.set("")

    def _on_device_double_click(self, ev: tk.Event) -> None:
        """Двойной клик: подставить серийный номер выбранного устройства (и сигнатуру) — прошивку не запускать."""
        sel = self.tree.selection()
        if not sel:
            return
        try:
            idx = self.tree.index(sel[0])
        except tk.TclError:
            return
        if idx < 0 or idx >= len(self.devices):
            return
        dev = self.devices[idx]
        if dev.serial and dev.serial != 0xFFFFFFFF:
            self.device_var.set("0x%08X" % dev.serial)
        else:
            self.device_var.set(str(dev.address))
        if len((self.tree.item(sel[0]))["values"]) >= 5:
            vals = self.tree.item(sel[0])["values"]
            if vals[4] and str(vals[4]).strip() and str(vals[4]) != "—":
                self.sig_var.set(str(vals[4]).strip()[:12])

    def _on_device_entry_paste(self, event: tk.Event) -> None:
        """Вставка из буфера в поле «Устройство» (адрес или серийный №)."""
        try:
            clip = self.root.clipboard_get()
        except tk.TclError:
            return
        if not isinstance(clip, str) or not clip:
            return
        # Только цифры и 0x; пробелы убираем (чтобы 0x0E0A 61A8 → 0x0E0A61A8)
        cleaned = "".join(c for c in clip.strip() if c in "0123456789aAbBcCdDeEfFxX")
        if not cleaned:
            return
        self.device_var.set(cleaned)
        try:
            event.widget.icursor(tk.END)
        except tk.TclError:
            pass
        return "break"  # не вызывать стандартную вставку повторно

    def _get_scan_speed_configs(self) -> Optional[List[SpeedConfig]]:
        """Собрать список (baudrate, parity, stopbits) по галочкам. None если ничего не выбрано.
        Сканирование только по выбранным параметрам связи (без добавления 9600 N2)."""
        bauds = [b for b in SCAN_BAUDRATES if self.scan_baud_vars[b].get()]
        links = [(p, s) for (p, s, _) in SCAN_LINK_OPTIONS if self.scan_link_vars[(p, s)].get()]
        if not bauds or not links:
            return None
        configs = [(b, p, s) for b in bauds for (p, s) in links]
        return configs

    def _get_scan_addr_range(self) -> Optional[tuple]:
        """Собрать диапазон адресов (addr_min, addr_max). None при неверном вводе."""
        try:
            a_min = int(self.scan_addr_min_var.get().strip())
            a_max = int(self.scan_addr_max_var.get().strip())
        except ValueError:
            return None
        if not (1 <= a_min <= 247 and 1 <= a_max <= 247):
            return None
        if a_min > a_max:
            a_min, a_max = a_max, a_min
        return (a_min, a_max)

    def _start_scan(self) -> None:
        if self.scan_in_progress:
            messagebox.showinfo(
                "Сканирование",
                "Сканирование уже выполняется. Нажмите «Отмена», дождитесь остановки, затем снова «Сканировать».",
            )
            return
        port = self.port_var.get().strip()
        if not port:
            messagebox.showwarning("Внимание", "Выберите COM-порт.")
            return
        speed_configs = self._get_scan_speed_configs()
        if not speed_configs:
            messagebox.showwarning(
                "Внимание",
                "Выберите хотя бы одну скорость и один набор параметров связи (например 8N1) для сканирования.",
            )
            return
        addr_range = self._get_scan_addr_range()
        if addr_range is None:
            messagebox.showwarning(
                "Внимание",
                "Диапазон адресов: введите числа от 1 до 247, «от» не больше «до».",
            )
            return
        addr_min, addr_max = addr_range
        self.scan_in_progress = True
        self._set_busy(True)
        self._log("Сканирование линии...")
        self.devices = []
        self._clear_table()

        def run():
            try:
                def on_found(dev: DeviceInfo):
                    self.root.after(0, lambda d=dev: self._add_device_to_table(d))
                def progress(a: int, t: int, current_addr: Optional[int] = None, current_config: Optional[tuple] = None):
                    pct = int(100 * a / t) if t else 0
                    label = f"Сканирование: {pct}%"
                    if current_addr is not None and current_config is not None:
                        baud, parity, stopbits = current_config
                        label += f"  |  {baud} {parity}{stopbits} адр. {current_addr}"
                    pct_f, label_f = pct, label
                    self.root.after(0, lambda: (self.progress_var.set(pct_f), self.progress_label_var.set(label_f)))
                scan_all(
                    port,
                    progress_cb=progress,
                    log_cb=self._log,
                    cancel_cb=lambda: self.cancel_requested,
                    on_device_found=on_found,
                    speed_configs=speed_configs,
                    addr_min=addr_min,
                    addr_max=addr_max,
                    fast_scan=self.scan_fast_var.get(),
                )
                self.root.after(0, self._on_scan_done)
            except Exception as e:
                self._log(f"Ошибка сканирования: {e}")
                self.root.after(0, lambda: self._on_scan_done())

        self.worker_thread = threading.Thread(target=run, daemon=True)
        self.worker_thread.start()

    def _on_scan_done(self) -> None:
        self.scan_in_progress = False
        # Таблица заполнялась по мере скана через _add_device_to_table; сортируем и перерисовываем
        self.devices.sort(key=lambda d: (d.address, d.baudrate))
        self._fill_table()
        self._set_busy(False)
        self.progress_var.set(0)
        self.progress_label_var.set("")
        self.progress_time_var.set("")
        self._log(f"Найдено устройств: {len(self.devices)}")
        self._log("Версия в таблице — с устройства (после прошивки выполните сканирование снова).")
        for d in self.devices:
            ser_str = f"0x{d.serial:08X}" if (d.serial and d.serial != 0xFFFFFFFF) else "—"
            self._log(
                f"  — адрес {d.address}, {d.baudrate} бод: серийный № {ser_str}, "
                f"версия пр. {d.app_version}, версия загрузчика {d.bootloader_version}, сигнатура «{d.signature or '—'}»"
            )

    # Серийный номер устройства — всегда 32 бита (uint32): 8 hex цифр, рег. 270–271 / 0xF0 / 1005.
    _SERIAL_MASK = 0xFFFFFFFF

    def _resolve_device(self) -> Tuple[Optional[DeviceInfo], bool]:
        """Resolve 'device for update' to address and link params.
        Returns (DeviceInfo or None, use_fast_modbus).
        use_fast_modbus=True — ввод был серийный номер (0x... или >247), прошивка по 0xFD 0x46 0x08."""
        raw = self.device_var.get().strip()
        if not raw:
            return None, False
        try:
            val = int(raw, 0)
        except ValueError:
            return None, False
        # Адрес Modbus (0–247): прошивка по адресу (247 в загрузчике)
        if 0 <= val <= 247:
            for d in self.devices:
                if d.address == val:
                    return d, False
            return DeviceInfo(
                address=val,
                baudrate=BOOTLOADER_BAUDRATE,
                parity=BOOTLOADER_PARITY,
                stopbits=BOOTLOADER_STOPBITS,
                signature="",
                app_version="—",
                bootloader_version="—",
                serial=0,
                in_bootloader=False,
            ), False
        # Серийный номер (32 бита): прошивка по быстрому Modbus (0xFD 0x46 0x08)
        serial_val = val & self._SERIAL_MASK
        for d in self.devices:
            if (d.serial & self._SERIAL_MASK) == serial_val:
                return d, True
        # Не найден в таблице — если в бутлоадере ровно одно устройство
        bl_devs = [d for d in self.devices if d.in_bootloader]
        if len(bl_devs) == 1:
            b = bl_devs[0]
            return DeviceInfo(
                address=b.address,
                baudrate=b.baudrate,
                parity=b.parity,
                stopbits=b.stopbits,
                signature=b.signature,
                app_version=b.app_version,
                bootloader_version=b.bootloader_version,
                serial=serial_val,
                in_bootloader=True,
            ), True
        return None, False

    def _set_busy(self, busy: bool) -> None:
        state = tk.DISABLED if busy else tk.NORMAL
        self.btn_scan["state"] = state
        self.btn_save_devices["state"] = state
        self.btn_update["state"] = state
        self.btn_update_all["state"] = state
        self.btn_assign_serials["state"] = state
        self.btn_write_link["state"] = state
        self.btn_factory_reset["state"] = state
        self.port_combo["state"] = "readonly" if not busy else tk.DISABLED
        self.btn_cancel["state"] = tk.NORMAL if busy else tk.DISABLED
        if not busy:
            self.cancel_requested = False

    def _request_cancel(self) -> None:
        self.cancel_requested = True

    def _start_update_one(self) -> None:
        dev, use_fast_modbus = self._resolve_device()
        if dev is None:
            messagebox.showwarning(
                "Внимание",
                "Введите адрес (0–247) или серийный номер устройства из таблицы.\n\n"
                "При выборе по серийному номеру сначала выполните сканирование — в режиме загрузчика "
                "серийный номер подставляется из Flash (рег. 270–271), после скана устройство можно выбрать по адресу или по серийному №.\n\n"
                "Если на линии несколько устройств в бутлоадере с одним адресом: прошейте по одному, выбрав устройство по серийному номеру (после скана он отображается в таблице)."
            )
            return
        self.firmware_is_bootloader = self.update_mode_var.get() == "bootloader"
        if not self.firmware_image:
            if not self._load_firmware_file():
                msg = "Выберите и загрузите файл прошивки."
                if self.firmware_is_bootloader:
                    msg = "Выберите образ бутлоадера (.fw или .bin, 34 КБ)."
                messagebox.showerror("Ошибка", msg)
                return
        serial_after = None
        raw_ser = self.serial_after_var.get().strip()
        if raw_ser:
            try:
                serial_after = int(raw_ser, 0) & 0xFFFFFFFF
            except ValueError:
                messagebox.showwarning("Внимание", "Серийный № должен быть числом (например 1 или 0x0E0A0001).")
                return
        apply_link = self.apply_link_after_var.get()
        link_speed = self.link_speed_var.get().strip() or "9600"
        link_parity = (self.link_parity_var.get().strip() or "N")[:1].upper()
        link_stop = int(self.link_stop_var.get().strip() or "1")
        if link_parity not in ("N", "E", "O"):
            link_parity = "N"
        if link_stop not in (1, 2):
            link_stop = 1
        link_addr = None
        try:
            a = int(self.link_addr_var.get().strip())
            if 1 <= a <= 247:
                link_addr = a
        except (ValueError, AttributeError):
            pass
        self._set_busy(True)
        if dev.signature and str(dev.signature).strip() and not dev.in_bootloader:
            self.signature = str(dev.signature).strip()[:12]
            self.sig_var.set(self.signature)
        else:
            self.signature = (self.sig_var.get() or DEFAULT_SIGNATURE)[:12]

        recovery = bool(self.recovery_mode_var.get())
        def run():
            self._do_flash_one(
                dev,
                serial_to_assign=serial_after,
                apply_link_after=apply_link,
                link_after_speed=link_speed,
                link_after_parity=link_parity,
                link_after_stop=link_stop,
                link_after_addr=link_addr,
                use_fast_modbus=use_fast_modbus,
                recovery=recovery,
            )
            self.root.after(0, lambda: self._set_busy(False))

        self.worker_thread = threading.Thread(target=run, daemon=True)
        self.worker_thread.start()

    def _write_link_params_only(self) -> None:
        """Записать параметры связи (рег. 110–112, 128) в выбранное устройство без прошивки."""
        dev, use_fast_modbus = self._resolve_device()
        if dev is None:
            messagebox.showwarning("Внимание", "Выберите устройство в таблице или введите адрес.")
            return
        # Запись по 0xFD 0x46 0x08 только если устройство найдено в фазе 1 (поддерживает быстрый Modbus). ВБ 19 и др. только в фазе 2 — пишем по адресу.
        if getattr(dev, "supports_fast_modbus", False) and dev.serial and (dev.serial & 0xFFFFFFFF) != 0xFFFFFFFF:
            use_fast_modbus = True
        port = self.port_var.get().strip()
        if not port:
            messagebox.showwarning("Внимание", "Выберите COM-порт.")
            return
        try:
            new_baud = int(self.link_speed_var.get().strip() or "9600")
        except ValueError:
            new_baud = 9600
        link_parity = (self.link_parity_var.get().strip() or "N")[:1].upper()
        if link_parity not in ("N", "E", "O"):
            link_parity = "N"
        try:
            link_stop = int(self.link_stop_var.get().strip() or "1")
        except ValueError:
            link_stop = 1
        if link_stop not in (1, 2):
            link_stop = 1
        new_addr: Optional[int] = None
        try:
            a = int(self.link_addr_var.get().strip())
            if 1 <= a <= 247:
                new_addr = a
        except (ValueError, AttributeError):
            pass
        self._set_busy(True)
        def run():
            err = self._write_app_link_params(
                port, dev, new_baud, link_parity, link_stop,
                new_address=new_addr, use_fast_modbus=use_fast_modbus,
            )
            self.root.after(0, lambda: self._set_busy(False))
            if err:
                self._log(f"Ошибка записи параметров: {err}")
                self.root.after(0, lambda: messagebox.showerror("Ошибка", err))
            else:
                self._log("Параметры связи и адрес записаны (рег. 110–112, 128), применятся после перезагрузки устройства.")
                self.root.after(0, lambda: messagebox.showinfo("Готово", "Параметры связи записаны."))
        self.worker_thread = threading.Thread(target=run, daemon=True)
        self.worker_thread.start()

    def _factory_reset(self) -> None:
        """Запросить сброс до заводских настроек (запись 1 в рег. 1001). Устройство выполнит load_default_config в main loop."""
        dev, use_fast_modbus = self._resolve_device()
        if dev is None:
            messagebox.showwarning("Внимание", "Выберите устройство в таблице или введите адрес.")
            return
        port = self.port_var.get().strip()
        if not port:
            messagebox.showwarning("Внимание", "Выберите COM-порт.")
            return
        self._set_busy(True)

        def run() -> None:
            err = self._do_factory_reset(port, dev, use_fast_modbus)
            self.root.after(0, lambda: self._set_busy(False))
            if err:
                self._log(f"Сброс до заводских: {err}")
                self.root.after(0, lambda: messagebox.showerror("Ошибка", err))
            else:
                self._log("Запрос сброса до заводских отправлен (рег. 1001). Устройство применит настройки без перезагрузки.")
                self.root.after(0, lambda: messagebox.showinfo("Готово", "Сброс до заводских настроек запрошен. Устройство применит настройки."))

        self.worker_thread = threading.Thread(target=run, daemon=True)
        self.worker_thread.start()

    def _do_factory_reset(self, port: str, dev: DeviceInfo, use_fast_modbus: bool) -> Optional[str]:
        """Отправить запись рег. 1001 = 1 (сброс EEPROM на заводские). Возвращает None или строку ошибки."""
        try:
            ser = open_port(port, baudrate=dev.baudrate, parity=dev.parity, stopbits=dev.stopbits)
        except Exception as e:
            return str(e)
        try:
            if use_fast_modbus and (dev.serial & 0xFFFFFFFF):
                body = modbus_rtu.build_write_single_register_body(REG_WB_EEPROM_ERASE, 1)
                req = modbus_rtu.build_fast_modbus_request(dev.serial & 0xFFFFFFFF, body)
                rsp = send_receive(ser, req, response_timeout_ms=2000)
                if rsp is None or len(rsp) < 4:
                    return "Таймаут ответа (рег. 1001 по серийному)"
                _, _, err = modbus_rtu.parse_fast_modbus_response(rsp, expected_serial=dev.serial)
                if err:
                    return err or "Ошибка ответа рег. 1001"
            else:
                req = modbus_rtu.build_write_single_register(dev.address, REG_WB_EEPROM_ERASE, 1)
                rsp = send_receive(ser, req, response_timeout_ms=2000)
                if rsp is None or len(rsp) < 4:
                    return "Таймаут ответа (рег. 1001)"
                addr, _, err = modbus_rtu.parse_response(rsp, expected_slave=dev.address)
                if err or addr is None:
                    return err or "Ошибка ответа рег. 1001"
            return None
        finally:
            ser.close()

    def _write_app_link_params(
        self,
        port: str,
        dev: DeviceInfo,
        baud: int,
        parity: str,
        stopbits: int,
        new_address: Optional[int] = None,
        use_fast_modbus: bool = False,
    ) -> Optional[str]:
        """Записать в приложение параметры связи (рег. 110, 111, 112) и при необходимости адрес (рег. 128). Возвращает None или строку ошибки.
        use_fast_modbus: по серийному номеру (0xFD 0x46 0x08), иначе — стандартный Modbus по адресу. При таймауте — повтор на целевой скорости."""
        # Рег 110: скорость (значение = baud/100: 96, 192, 384, 1152); 111: чётность; 112: стоп-биты; 128: адрес Modbus
        baud_val = baud // 100
        if baud_val not in (12, 24, 48, 96, 192, 384, 576, 1152):
            return f"Недопустимая скорость {baud}"
        parity_map = {"N": 0, "O": 1, "E": 2}
        parity_val = parity_map.get(parity.upper()[:1], 0)
        if stopbits not in (1, 2):
            stopbits = 1
        if new_address is not None and (new_address < 1 or new_address > 247):
            new_address = None
        if new_address is not None and new_address == dev.address:
            new_address = None  # тот же адрес — не пишем рег. 128 (избегаем лишней записи и возможной ошибки в прошивке)
        if use_fast_modbus and not (dev.serial & 0xFFFFFFFF):
            return "Для записи по серийному номеру выберите устройство из таблицы после сканирования."
        # Устройства WB применяют рег. 110 (скорость) сразу — пишем сначала 111, 112, затем 110, чтобы чётность и стоп-биты успели сохраниться до переинициализации UART.
        _link_write_timeout_ms = 2500
        def do_write_reg(ser, reg: int, val: int) -> Optional[str]:
            if use_fast_modbus:
                body = modbus_rtu.build_write_single_register_body(reg, val)
                req = modbus_rtu.build_fast_modbus_request(dev.serial & 0xFFFFFFFF, body)
                rsp = send_receive(ser, req, response_timeout_ms=_link_write_timeout_ms)
                if rsp is None or len(rsp) < 4:
                    return "Таймаут записи рег. %d" % reg
                _, _, err = modbus_rtu.parse_fast_modbus_response(rsp, expected_serial=dev.serial)
                return err
            req = modbus_rtu.build_write_single_register(dev.address, reg, val)
            rsp = send_receive(ser, req, response_timeout_ms=_link_write_timeout_ms)
            if rsp is None or len(rsp) < 4:
                return "Таймаут записи рег. %d" % reg
            _, _, err = modbus_rtu.parse_response(rsp, expected_slave=dev.address)
            return err
        last_error: Optional[str] = None
        for attempt in (0, 1):
            try:
                if attempt == 0:
                    ser = open_port(port, baudrate=dev.baudrate, parity=dev.parity, stopbits=dev.stopbits)
                else:
                    ser = open_port(port, baudrate=baud, parity=parity, stopbits=stopbits)
            except Exception as e:
                last_error = str(e) if attempt == 0 else f"{e} (повтор на {baud} бод)"
                continue
            try:
                ser.reset_input_buffer()
                time.sleep(0.05)
                err111 = do_write_reg(ser, 111, parity_val)
                if err111:
                    last_error = err111 or "Ошибка записи рег. 111"
                    if attempt == 0:
                        break
                    return last_error
                err112 = do_write_reg(ser, 112, stopbits)
                if err112:
                    last_error = err112 or "Ошибка записи рег. 112"
                    if attempt == 0:
                        break
                    return last_error
                err110 = do_write_reg(ser, 110, baud_val)
                if err110:
                    last_error = err110 or "Ошибка записи рег. 110"
                    if attempt == 0:
                        break
                    return last_error
                # Запись рег. 128 — только после перехода порта на новые 110/111/112 (WB применяет их сразу).
                if new_address is not None:
                    ser.close()
                    ser = None
                    try:
                        ser = open_port(port, baudrate=baud, parity=parity, stopbits=stopbits)
                    except Exception as e:
                        return "Не удалось открыть порт на новой скорости для записи рег. 128: " + str(e)
                    try:
                        if use_fast_modbus:
                            body128 = modbus_rtu.build_write_single_register_body(128, new_address)
                            req128 = modbus_rtu.build_fast_modbus_request(dev.serial & 0xFFFFFFFF, body128)
                            rsp128 = send_receive(ser, req128, response_timeout_ms=_link_write_timeout_ms)
                            if rsp128 is None or len(rsp128) < 4:
                                return "Таймаут записи рег. 128 (адрес) по серийному"
                            _, _, err128 = modbus_rtu.parse_fast_modbus_response(rsp128, expected_serial=dev.serial)
                            if err128:
                                return err128 or "Ошибка ответа записи рег. 128"
                        else:
                            req128 = modbus_rtu.build_write_single_register(dev.address, 128, new_address)
                            rsp128 = send_receive(ser, req128, response_timeout_ms=_link_write_timeout_ms)
                            if rsp128 is None or len(rsp128) < 4:
                                return "Таймаут записи рег. 128 (адрес)"
                            addr128, _, err128 = modbus_rtu.parse_response(rsp128, expected_slave=None)
                            if err128 or addr128 is None:
                                return err128 or "Ошибка ответа записи рег. 128"
                            if addr128 not in (dev.address, new_address):
                                return "Неверный ответ при записи адреса (рег. 128)"
                    finally:
                        ser.close()
                return None
            finally:
                if ser is not None:
                    ser.close()
        return last_error or "Таймаут записи рег. 110–112 (проверьте скорость и подключение)"

    def _do_flash_one(
        self,
        dev: DeviceInfo,
        serial_to_assign: Optional[int] = None,
        apply_link_after: bool = False,
        link_after_speed: str = "9600",
        link_after_parity: str = "N",
        link_after_stop: int = 1,
        link_after_addr: Optional[int] = None,
        use_fast_modbus: bool = False,
        recovery: bool = False,
    ) -> None:
        port = self.port_var.get().strip()
        if not port:
            self._log("Не выбран порт.")
            return
        # В info-блоке — сигнатура нижней платы (6do8di, 12ai, 14di …). При устройстве в загрузчике убираем суффикс _bl.
        if dev.signature and str(dev.signature).strip() and not dev.in_bootloader:
            sig_to_use = str(dev.signature).strip()[:12]
        else:
            sig_to_use = (self.sig_var.get() or "").strip()[:12] or self.signature
        if dev.in_bootloader and sig_to_use:
            if sig_to_use.endswith("_bl"):
                sig_to_use = sig_to_use.replace("_bl", "").strip() or DEFAULT_SIGNATURE
            if not sig_to_use:
                sig_to_use = DEFAULT_SIGNATURE
            # По имени файла можно подставить сигнатуру платы (например 12ai_1.0.0.fw → 12ai)
            if self.firmware_path and self.firmware_path.name.upper().startswith("MR-02M"):
                sig_to_use = "MR02M"
            elif self.firmware_path:
                name_upper = self.firmware_path.name.upper()
                for board in ("6DO8DI", "12AI", "14DI", "6AO6AI", "4DO6DI", "16DO", "6DO", "12AO", "MP02M", "MR02M"):
                    if board in name_upper:
                        sig_to_use = board
                        break
        try:
            is_wb_firmware = bool(
                self.firmware_path and self.firmware_path.suffix.lower() == ".wbfw"
            )
            in_bl_or_recovery = dev.in_bootloader or recovery
            if in_bl_or_recovery:
                if recovery:
                    self._log("Режим восстановления: устройство уже в загрузчике, команда перехода не отправляется.")
                else:
                    self._log("Устройство уже в режиме загрузчика, открываем порт 115200...")
            else:
                self._log(f"Перевод устройства {dev.address} в режим загрузчика...")
                ser = open_port(port, baudrate=dev.baudrate, parity=dev.parity, stopbits=dev.stopbits)
                try:
                    proto = FlasherProtocol(
                        lambda req: send_receive(ser, req, response_timeout_ms=2000),
                        log_cb=self._log,
                        verbose_exchange_log=False,
                    )
                    err = proto.enter_bootloader_wb(dev.address) if is_wb_firmware else proto.enter_bootloader(dev.address)
                    if err and "Таймаут" not in (err or ""):
                        self._log(f"Ошибка перехода в загрузчик: {err}")
                        self.root.after(0, lambda: messagebox.showerror("Ошибка", "Не удалось перевести устройство в режим загрузчика. Проверьте порт, адрес и питание."))
                        return
                    if err and "Таймаут" in (err or ""):
                        self._log("Устройство отправлено в режим bootloader.")
                finally:
                    ser.close()
                self._log("Ожидание 1 с после перезагрузки...")
                time.sleep(1)
                # При прошивке по серийному (0xFD 0x46) загрузчик должен успеть запуститься и прочитать serial из Flash — иначе первый запрос таймаут
                if use_fast_modbus and not is_wb_firmware:
                    time.sleep(4.0)
                    self._log("Ожидание готовности загрузчика (обмен по серийному)...")
                if is_wb_firmware:
                    time.sleep(1.5)
                    self._log("Wiren Board: ещё 1.5 с до 9600 8N2 (загрузчик готов)...")
            if is_wb_firmware:
                boot_baud = BOOTLOADER_BAUDRATE_WB
                boot_stopbits = BOOTLOADER_STOPBITS_WB
                self._log("Режим Wiren Board (.wbfw): загрузчик 9600 8N2.")
            else:
                boot_baud = BOOTLOADER_BAUDRATE
                boot_stopbits = BOOTLOADER_STOPBITS
            self._log(f"Скорость загрузчика: {boot_baud} бод, стоп-биты: {boot_stopbits}")
            ser = open_port(
                port, baudrate=boot_baud, parity=BOOTLOADER_PARITY, stopbits=boot_stopbits
            )
            try:
                time.sleep(1.5)
                def _send_recv_profiled(req: bytes):
                    if len(req) < 2:
                        return send_receive(ser, req, response_timeout_ms=FAST_RESPONSE_TIMEOUT_MS)
                    # Быстрый Modbus 0xFD 0x46 0x08: первый запрос/инфо — 3.5 с; блоки данных (длинный кадр) — 5 с как по адресу (ответ возвращается по готовности кадра в serial_port)
                    if req[0] == 0xFD and req[1] == 0x46:
                        to_ms = BOOTLOADER_DATA_BLOCK_TIMEOUT_MS if len(req) > 100 else 3500
                        return send_receive(ser, req, response_timeout_ms=to_ms)
                    if req[1] == 0x03:
                        return send_receive(ser, req, response_timeout_ms=INFO_RESPONSE_TIMEOUT_MS)
                    if req[1] == 0x10 and len(req) >= 6:
                        if req[2] == 0x10 and req[3] == 0x00:
                            # Запись info (0x1000): для WB загрузчик может отвечать дольше
                            info_to = max(INFO_RESPONSE_TIMEOUT_MS, BOOTLOADER_INFOBLOCK_TIMEOUT_MS_WB) if is_wb_firmware else INFO_RESPONSE_TIMEOUT_MS
                            return send_receive(ser, req, response_timeout_ms=info_to)
                        if req[2] == 0x20 and req[3] == 0x00:
                            # Как у WB: таймаут ответа загрузчика 5 с (BL_MINIMAL_RESPONSE_TIMEOUT) для WB и наших устройств
                            to = BOOTLOADER_INFOBLOCK_TIMEOUT_MS_WB if is_wb_firmware else BOOTLOADER_DATA_BLOCK_TIMEOUT_MS
                            return send_receive(ser, req, response_timeout_ms=to)
                    return send_receive(ser, req, response_timeout_ms=FAST_RESPONSE_TIMEOUT_MS)

                proto = FlasherProtocol(
                    _send_recv_profiled,
                    log_cb=self._log,
                    verbose_exchange_log=False,
                )
                if is_wb_firmware:
                    # Сигнатуру в прошивальщике не проверяем — пробуем зашивать то, что открыли (устройство само может вернуть исключение при несовпадении).
                    self._log("Прошивка Wiren Board: обмен по Modbus-адресу %d (0x1000 → 0x2000, блоки 136 B)." % dev.address)
                    flash_start_time = [time.perf_counter()]
                    flash_end_time = [None]

                    def progress_wb(b: int, t: int) -> None:
                        pct = int(100 * b / t) if t else 0
                        now = time.perf_counter()
                        if b == t and t > 0:
                            flash_end_time[0] = now
                        elapsed = now - flash_start_time[0]
                        eta = (t - b) * (elapsed / b) if b > 0 and t > 0 else 0
                        self.root.after(
                            0,
                            lambda: (
                                self.progress_var.set(pct),
                                self.progress_label_var.set("Прошивка WB: %d%%" % pct),
                                self.progress_time_var.set(
                                    "Прошло: %.0f с  |  Осталось: ~%.0f с" % (elapsed, eta)
                                ),
                            ),
                        )

                    err = run_flash_sequence_wb(
                        proto,
                        dev.address,
                        self.firmware_image,
                        progress_cb=progress_wb,
                        cancel_cb=lambda: self.cancel_requested,
                    )
                    if err:
                        self._log(err)
                        self.root.after(0, lambda: messagebox.showerror("Ошибка прошивки Wiren Board", err))
                        return
                    if flash_end_time[0]:
                        self._log("Время прошивки: %.1f с" % (flash_end_time[0] - flash_start_time[0]))
                    self._log("Отправка перехода в приложение (рег. 1004)...")
                    proto.write_multiple_registers(dev.address, REG_JUMP_APP, [1])
                    self._log("Прошивка Wiren Board завершена.")
                    self.root.after(0, lambda: messagebox.showinfo("Готово", "Устройство Wiren Board успешно обновлено."))
                    return
                # Наши устройства: прошивка по адресу 247 (обычный Modbus) или по серийному (0xFD 0x46 0x08). Выбор по серийному (двойной клик / ввод 0x...) → быстрый Modbus.
                use_by_address = not use_fast_modbus and dev.address and 1 <= dev.address <= 247
                if use_by_address:
                    self._log("Прошивка по Modbus-адресу %d (обычный Modbus, без 0xFD 0x46)." % BOOTLOADER_DEFAULT_ADDR)
                    info_sig = sig_to_use
                    flash_start_time = [None]
                    flash_end_time = [None]

                    def progress_addr(b, t):
                        pct = int(100 * b / t) if t else 0
                        label = "Бутлоадер: %d%%" % pct if self.firmware_is_bootloader else "Прошивка: %d%%" % pct
                        now = time.perf_counter()
                        if b > 0 and flash_start_time[0] is None:
                            flash_start_time[0] = now
                        if b == t and t > 0:
                            flash_end_time[0] = now
                        elapsed = (now - flash_start_time[0]) if flash_start_time[0] else 0
                        eta = (t - b) * (elapsed / b) if b > 0 and t > 0 and elapsed > 0 else 0
                        time_str = "Прошло: %.0f с  |  Осталось: ~%.0f с" % (elapsed, eta) if b > 0 else "Прошло: 0 с  |  Осталось: — с"
                        self.root.after(0, lambda: (
                            self.progress_var.set(pct),
                            self.progress_label_var.set(label),
                            self.progress_time_var.set(time_str),
                        ))

                    if self.firmware_is_bootloader:
                        err = run_flash_bootloader_sequence_by_address(
                            proto,
                            BOOTLOADER_DEFAULT_ADDR,
                            self.firmware_image,
                            info_sig,
                            progress_cb=progress_addr,
                            cancel_cb=lambda: self.cancel_requested,
                        )
                        if err:
                            self._log(err)
                            self.root.after(0, lambda: messagebox.showerror("Ошибка прошивки бутлоадера", err))
                            return
                        if flash_start_time[0] and flash_end_time[0]:
                            self._log("Время прошивки бутлоадера: %.1f с" % (flash_end_time[0] - flash_start_time[0]))
                        self._log("Прошивка бутлоадера завершена. Устройство выполнит сброс и запустит новый бутлоадер.")
                        self.root.after(0, lambda: messagebox.showinfo("Готово", "Бутлоадер обновлён. Устройство перезагружено."))
                        return
                    err = run_flash_sequence_by_address(
                        proto,
                        BOOTLOADER_DEFAULT_ADDR,
                        self.firmware_image,
                        info_sig,
                        progress_cb=progress_addr,
                        cancel_cb=lambda: self.cancel_requested,
                    )
                    if err:
                        self._log(err)
                        self.root.after(0, lambda: messagebox.showerror("Ошибка прошивки", err))
                        return
                    if flash_start_time[0] and flash_end_time[0]:
                        self._log("Время прошивки: %.1f с" % (flash_end_time[0] - flash_start_time[0]))
                    if serial_to_assign is not None:
                        self._log("Запись серийного номера 0x%08X (регистр 1005) на адрес %d..." % (serial_to_assign, BOOTLOADER_DEFAULT_ADDR))
                        ser_err = proto.write_serial_number(BOOTLOADER_DEFAULT_ADDR, serial_to_assign)
                        if ser_err:
                            self._log("Ошибка записи серийного: %s" % ser_err)
                        else:
                            self._log("Серийный номер записан.")
                    self._log("Пауза 1.2 с (ожидание завершения записи во Flash)...")
                    time.sleep(1.2)
                    self._log("Отправка перехода в приложение (регистр 1004)...")
                    jump_err = proto.jump_to_app(BOOTLOADER_DEFAULT_ADDR)
                    if jump_err:
                        self._log("Предупреждение jump: %s" % jump_err)
                    else:
                        self._log("Устройство выполнит сброс и запуск приложения (ожидание 2–3 с).")
                    if apply_link_after:
                        self._log("Ожидание 3 с до старта приложения...")
                        time.sleep(3)
                        try:
                            new_baud = int(link_after_speed)
                        except ValueError:
                            new_baud = 9600
                        err_link = self._write_app_link_params(
                            port, dev, new_baud, link_after_parity, link_after_stop,
                            new_address=link_after_addr, use_fast_modbus=use_fast_modbus,
                        )
                        if err_link:
                            self._log("Параметры связи не записаны: %s" % err_link)
                        else:
                            self._log("Параметры связи и адрес записаны (рег. 110–112, 128), применятся после перезагрузки.")
                    self._log("Прошивка завершена успешно.")
                    self.root.after(0, lambda: messagebox.showinfo("Готово", "Устройство успешно обновлено."))
                    return
                bootloader_serial = (dev.serial & 0xFFFFFFFF) if dev.serial else 0
                # После перезагрузки в бутлоадер серийный может отличаться (EEPROM/Flash): узнаём фактический serial по WB-сканированию.
                if use_fast_modbus and not is_wb_firmware:
                    scan_result = send_receive_wb_ext_scan(ser, response_timeout_ms=1200, silence_ms=50)
                    if scan_result:
                        # Предпочтение: тот же serial, что и у приложения; иначе — единственный или первый с адресом 247
                        match_serial = next((s for _a, s in scan_result if s == bootloader_serial), None)
                        if match_serial is not None:
                            bootloader_serial = match_serial
                        elif len(scan_result) == 1:
                            bootloader_serial = scan_result[0][1]
                            self._log("Серийный загрузчика по сканированию: 0x%08X." % bootloader_serial)
                        else:
                            addr247 = next((s for a, s in scan_result if a == 247), None)
                            if addr247 is not None:
                                bootloader_serial = addr247
                                self._log("Серийный загрузчика (адрес 247): 0x%08X." % bootloader_serial)
                if not bootloader_serial or bootloader_serial == 0xFFFFFFFF:
                    self._log("Для обмена с загрузчиком по 0xFD 0x46 нужен серийный номер. Выполните сканирование и выберите устройство по серийному №.")
                    self.root.after(0, lambda: messagebox.showerror(
                        "Ошибка",
                        "Серийный номер устройства неизвестен. Включите «Несколько устройств в загрузчике (выбор по серийному №)» только при нескольких устройствах; иначе выберите устройство по адресу из таблицы."
                    ))
                    return
                self._log("Обмен с загрузчиком по серийному номеру (0xFD 0x46 0x08), серийный 0x%08X." % bootloader_serial)
                time.sleep(0.35)
                self._log("Запрос информации загрузчика (рег. 290, 330) по серийному...")
                sig, bl_ver, err = proto.read_bootloader_info_by_serial(bootloader_serial)
                # После перезагрузки в бутлоадер устройство может отвечать с задержкой — повторить до 3 раз с паузой 2 с
                if err and not dev.in_bootloader:
                    for retry in range(2):
                        self._log("Повтор запроса информации загрузчика (после перезагрузки)...")
                        time.sleep(2.0)
                        sig, bl_ver, err = proto.read_bootloader_info_by_serial(bootloader_serial)
                        if not err:
                            break
                if err:
                    self._log(f"Не удалось прочитать информацию загрузчика: {err}")
                    self.root.after(0, lambda: messagebox.showerror("Ошибка", "Загрузчик не отвечает по серийному 0x%08X (0xFD 0x46). Проверьте, что устройство в режиме bootloader (115200 8N1)." % bootloader_serial))
                    return
                self._log(f"Сигнатура: {sig or '—'}, версия загрузчика: {bl_ver or '—'}")
                # Для info-блока используем сигнатуру, которую только что вернул загрузчик (совпадает с EEPROM)
                info_sig = (sig or "").strip()[:12] if sig and str(sig).strip() else sig_to_use
                time.sleep(0.3)  # пауза перед 0x10, чтобы бутлоадер успел обработать предыдущий обмен
                flash_start_time = [None]  # старт по первому переданному блоку (без паузы стирания)
                flash_end_time = [None]

                def progress(b, t):
                    pct = int(100 * b / t) if t else 0
                    label = "Бутлоадер: %d%%" % pct if self.firmware_is_bootloader else "Прошивка: %d%%" % pct
                    now = time.perf_counter()
                    if b > 0 and flash_start_time[0] is None:
                        flash_start_time[0] = now
                    if b == t and t > 0:
                        flash_end_time[0] = now
                    elapsed = (now - flash_start_time[0]) if flash_start_time[0] else 0
                    if b > 0 and t > 0 and elapsed > 0:
                        eta = (t - b) * (elapsed / b)
                        time_str = "Прошло: %.0f с  |  Осталось: ~%.0f с" % (elapsed, eta)
                    else:
                        time_str = "Прошло: 0 с  |  Осталось: — с"
                    self.root.after(
                        0,
                        lambda: (
                            self.progress_var.set(pct),
                            self.progress_label_var.set(label),
                            self.progress_time_var.set(time_str),
                        ),
                    )

                if self.firmware_is_bootloader:
                    self._log("Режим прошивки бутлоадера (34 КБ, 0x46 по серийному).")
                    err = run_flash_sequence_bootloader(
                        proto,
                        bootloader_serial,
                        self.firmware_image,
                        info_sig,
                        progress_cb=progress,
                        cancel_cb=lambda: self.cancel_requested,
                    )
                    if err:
                        self._log(err)
                        self.root.after(0, lambda: messagebox.showerror("Ошибка прошивки бутлоадера", err))
                        return
                    if flash_start_time[0] and flash_end_time[0]:
                        self._log("Время прошивки бутлоадера: %.1f с" % (flash_end_time[0] - flash_start_time[0]))
                    self._log("Прошивка бутлоадера завершена. Устройство выполнит сброс и запустит новый бутлоадер.")
                    self.root.after(0, lambda: messagebox.showinfo("Готово", "Бутлоадер обновлён. Устройство перезагружено."))
                    return
                # Прошивка приложения по серийному (0x46)
                err = run_flash_sequence(
                    proto,
                    bootloader_serial,
                    self.firmware_image,
                    info_sig,
                    progress_cb=progress,
                    cancel_cb=lambda: self.cancel_requested,
                    target_serial=None,
                )
                if err:
                    self._log(err)
                    self.root.after(0, lambda: messagebox.showerror("Ошибка прошивки", err))
                    return
                if flash_start_time[0] and flash_end_time[0]:
                    self._log("Время прошивки: %.1f с" % (flash_end_time[0] - flash_start_time[0]))
                if serial_to_assign is not None:
                    self._log(f"Запись серийного номера 0x{serial_to_assign:08X} (регистр 1005) по 0x46...")
                    ser_err = proto.write_serial_number_by_serial(bootloader_serial, serial_to_assign)
                    if ser_err:
                        self._log(f"Ошибка записи серийного: {ser_err}")
                    else:
                        self._log("Серийный номер записан.")
                self._log("Пауза 1.2 с (ожидание завершения записи последнего блока во Flash)...")
                time.sleep(1.2)
                self._log("Отправка команды перехода в приложение (регистр 1004 по 0x46)...")
                jump_err = proto.jump_to_app_with_console_diagnostics_by_serial(
                    bootloader_serial, self.firmware_image
                )
                if jump_err:
                    self._log(f"Предупреждение jump: {jump_err}")
                if apply_link_after:
                    self._log("Ожидание 3 с до старта приложения...")
                    time.sleep(3)
                    try:
                        new_baud = int(link_after_speed)
                    except ValueError:
                        new_baud = 9600
                    err_link = self._write_app_link_params(
                        port, dev, new_baud, link_after_parity, link_after_stop,
                        new_address=link_after_addr, use_fast_modbus=use_fast_modbus,
                    )
                    if err_link:
                        self._log(f"Параметры связи не записаны: {err_link}")
                    else:
                        self._log("Параметры связи и адрес записаны (рег. 110–112, 128), применятся после перезагрузки устройства.")
                self._log("Прошивка завершена успешно.")
                self.root.after(0, lambda: messagebox.showinfo("Готово", "Устройство успешно обновлено."))
            finally:
                ser.close()
        except Exception as e:
            self._log(f"Ошибка: {e}")
            self.root.after(0, lambda: messagebox.showerror("Ошибка", str(e)))
        finally:
            self.root.after(
                0,
                lambda: (
                    self.progress_var.set(0),
                    self.progress_label_var.set(""),
                    self.progress_time_var.set(""),
                ),
            )

    def _start_assign_serials_only(self) -> None:
        """Присвоить серийные номера по порядку (с заданного) без прошивки: переход в бутлоадер → запись reg 1005 → переход в приложение."""
        if not self.devices:
            messagebox.showwarning("Внимание", "Сначала выполните сканирование.")
            return
        try:
            serial_start = int(self.serial_start_var.get().strip(), 0)
        except ValueError:
            messagebox.showwarning("Внимание", "Начальный серийный № должен быть числом (например 1 или 0x0E0A0001).")
            return
        self._set_busy(True)
        sorted_devs = sorted(self.devices, key=lambda d: d.address)

        def run():
            self._do_assign_serials_only(sorted_devs, serial_start)
            self.root.after(0, lambda: self._set_busy(False))

        self.worker_thread = threading.Thread(target=run, daemon=True)
        self.worker_thread.start()

    def _do_assign_serials_only(self, devs: List[DeviceInfo], serial_start: int) -> None:
        port = self.port_var.get().strip()
        if not port:
            self._log("Не выбран порт.")
            return
        ok = 0
        fail = 0
        for i, dev in enumerate(devs):
            if self.cancel_requested:
                self._log("Отменено пользователем.")
                break
            serial_val = serial_start + i
            self._log(f"Устройство {i + 1}/{len(devs)} (адрес {dev.address}): присвоить серийный 0x{serial_val:08X}...")
            self.root.after(0, lambda i=i, t=len(devs): (self.progress_label_var.set(f"Серийные: {i+1}/{t}"), self.progress_var.set(100 * (i + 1) / t)))
            try:
                if not dev.in_bootloader:
                    ser = open_port(port, baudrate=dev.baudrate, parity=dev.parity, stopbits=dev.stopbits)
                    try:
                        proto = FlasherProtocol(lambda req: send_receive(ser, req, response_timeout_ms=2000))
                        err = proto.enter_bootloader(dev.address)
                        if err:
                            self._log(f"Ошибка перехода в загрузчик: {err}")
                            fail += 1
                            continue
                    finally:
                        ser.close()
                    time.sleep(1)
                ser = open_port(port, baudrate=BOOTLOADER_BAUDRATE, parity=BOOTLOADER_PARITY, stopbits=BOOTLOADER_STOPBITS)
                try:
                    time.sleep(1.5)
                    proto = FlasherProtocol(lambda req: send_receive(ser, req, response_timeout_ms=2000), log_cb=self._log)
                    bl_serial = (dev.serial & 0xFFFFFFFF) if dev.serial else 0
                    if not bl_serial or bl_serial == 0xFFFFFFFF:
                        self._log("Серийный устройства неизвестен — пропуск (нужен для 0x46).")
                        fail += 1
                        continue
                    err = proto.write_serial_number_by_serial(bl_serial, serial_val)
                    if err:
                        self._log(f"Ошибка записи серийного: {err}")
                        fail += 1
                        continue
                    proto.jump_to_app_by_serial(bl_serial)
                    ok += 1
                finally:
                    ser.close()
            except Exception as e:
                self._log(f"Ошибка: {e}")
                fail += 1
        self.root.after(0, lambda: (self.progress_var.set(0), self.progress_label_var.set(""), self.progress_time_var.set("")))
        self._log(f"Готово: присвоено {ok}, ошибок {fail}.")
        self.root.after(0, lambda: messagebox.showinfo("Серийные номера", f"Присвоено: {ok}, с ошибками: {fail}"))

    def _start_update_all(self) -> None:
        if not self.devices:
            messagebox.showwarning("Внимание", "Сначала выполните сканирование и убедитесь, что в таблице есть устройства.")
            return
        if not self.firmware_image:
            if not self._load_firmware_file():
                messagebox.showerror("Ошибка", "Выберите и загрузите файл прошивки.")
                return
        self.signature = (self.sig_var.get() or DEFAULT_SIGNATURE)[:12]
        self._set_busy(True)
        sorted_devs = sorted(self.devices, key=lambda d: d.address)

        def run():
            self._do_flash_all(sorted_devs)
            self.root.after(0, lambda: self._set_busy(False))

        self.worker_thread = threading.Thread(target=run, daemon=True)
        self.worker_thread.start()

    def _do_flash_all(self, devs: List[DeviceInfo]) -> None:
        port = self.port_var.get().strip()
        if not port:
            self._log("Не выбран порт.")
            return
        serial_assign = self.serial_assign_var.get()
        try:
            serial_start = int(self.serial_start_var.get().strip(), 0)
        except ValueError:
            serial_start = 1
        ok = 0
        fail = 0
        for i, dev in enumerate(devs):
            if self.cancel_requested:
                self._log("Отменено пользователем.")
                break
            self._log(f"Устройство {i + 1} из {len(devs)} (адрес {dev.address})...")
            self.root.after(0, lambda i=i, t=len(devs): (self.progress_label_var.set(f"Устройство {i+1} из {t}"), self.progress_var.set(100 * (i) / t)))
            serial_to_assign = (serial_start + i) if serial_assign else None
            try:
                self._do_flash_one(dev, serial_to_assign=serial_to_assign)
                ok += 1
            except Exception as e:
                self._log(f"Ошибка для адреса {dev.address}: {e}")
                fail += 1
                cont = [None]
                ev = threading.Event()
                self.root.after(0, lambda: (cont.__setitem__(0, messagebox.askyesno("Ошибка", f"Ошибка при прошивке устройства {dev.address}:\n{e}\n\nПродолжить с остальными?")), ev.set()))
                ev.wait()
                if not cont[0]:
                    break
        self.root.after(0, lambda: (self.progress_var.set(0), self.progress_label_var.set(""), self.progress_time_var.set("")))
        self._log(f"Итог: обновлено {ok}, ошибок {fail}.")
        self.root.after(0, lambda: messagebox.showinfo("Итог", f"Обновлено: {ok}, с ошибками: {fail}"))

    def run(self) -> None:
        self.root.mainloop()


def main():
    app = FlasherApp()
    app.run()


if __name__ == "__main__":
    main()
