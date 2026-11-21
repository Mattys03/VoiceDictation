"""
Settings Dialog - Auto-save, no Save button.
Changes are applied immediately when you capture a hotkey or select a mic.
"""

import threading
import tkinter as tk
from settings_manager import (
    get_microphones,
    load_settings,
    save_settings,
    device_name_matches,
)


def _short_api(api_name):
    """Compact host API tag so duplicate entries are distinguishable."""
    low = (api_name or "").lower()
    for tag in ("mme", "wasapi", "directsound", "wdm-ks"):
        if tag in low:
            return tag.upper()
    return (api_name or "?")[:8]


class HotkeyCapture:
    def __init__(self, label, on_done):
        self.label = label
        self.on_done = on_done
        self._listener = None
        self._active = False
        self._held_mods = set()
        self._final_key = None

    _MOD_DISPLAY = {
        "Key.ctrl_l": "Ctrl", "Key.ctrl_r": "Ctrl",
        "Key.alt_l": "Alt", "Key.alt_r": "Alt",
        "Key.shift_l": "Shift", "Key.shift_r": "Shift",
        "Key.cmd": "Win", "Key.cmd_l": "Win", "Key.cmd_r": "Win",
    }
    _MOD_KEYS = {
        "Key.ctrl_l", "Key.ctrl_r", "Key.alt_l", "Key.alt_r",
        "Key.shift_l", "Key.shift_r", "Key.cmd", "Key.cmd_l", "Key.cmd_r",
    }

    def start(self):
        from pynput import keyboard as kb
        self._active = True
        self._held_mods = set()
        self._final_key = None
        self.label.config(text="  Pressione...  ", fg="#ffcc00")

        def on_press(key):
            if not self._active:
                return False
            ks = str(key)
            if ks in self._MOD_KEYS:
                self._held_mods.add(self._MOD_DISPLAY.get(ks, ks))
                preview = "+".join(sorted(self._held_mods)) + "+..."
                self.label.after(0, lambda: self.label.config(
                    text=f"  {preview}  ", fg="#ffcc00"))
            else:
                if hasattr(key, 'char') and key.char:
                    self._final_key = key.char.upper()
                elif hasattr(key, 'name') and key.name:
                    self._final_key = key.name.capitalize()
                else:
                    self._final_key = ks.replace("Key.", "").capitalize()

                if self._held_mods and self._final_key:
                    self._active = False
                    result = "+".join(sorted(self._held_mods)) + "+" + self._final_key
                    self.label.after(0, lambda r=result: self._finish(r))
                    return False

        def on_release(key):
            if not self._active:
                return False
            ks = str(key)
            if ks in self._MOD_KEYS:
                self._held_mods.discard(self._MOD_DISPLAY.get(ks, ks))

        self._listener = kb.Listener(on_press=on_press, on_release=on_release, suppress=True)
        self._listener.daemon = True
        self._listener.start()

    def _finish(self, result):
        self.label.config(text=f"  {result}  ", fg="#4dd964")
        self.stop()
        if self.on_done:
            self.on_done(result)

    def stop(self):
        self._active = False
        if self._listener:
            try:
                self._listener.stop()
            except Exception:
                pass
            self._listener = None


class SettingsDialog:
    def __init__(self, parent, on_save_callback=None):
        self.parent = parent
        self.on_save = on_save_callback
        self.win = None
        self._capture = None
        self._api_key_job = None

    def show(self):
        if self.win and self.win.winfo_exists():
            self.win.lift()
            return

        self.settings = load_settings()

        self.win = tk.Toplevel(self.parent)
        self.win.title("Configuracoes")
        self.win.overrideredirect(True)
        self.win.attributes("-topmost", True)
        self.win.configure(bg="#1a1d2e")

        W, H = 330, 540
        sx = self.parent.winfo_screenwidth()
        sy = self.parent.winfo_screenheight()
        self.win.geometry(f"{W}x{H}+{(sx-W)//2}+{(sy-H)//2}")

        # ── Title bar ──
        title_frame = tk.Frame(self.win, bg="#14162a", height=34)
        title_frame.pack(fill="x")
        title_frame.pack_propagate(False)

        tk.Label(
            title_frame, text="  Configuracoes", fg="#e0e2f0",
            bg="#14162a", font=("Segoe UI", 10, "bold"), anchor="w", padx=8
        ).pack(side="left", fill="both", expand=True)

        close_btn = tk.Label(
            title_frame, text="X", fg="#888899", bg="#14162a",
            font=("Segoe UI", 11, "bold"), padx=10, cursor="hand2"
        )
        close_btn.pack(side="right")
        close_btn.bind("<Button-1>", lambda e: self._close())
        close_btn.bind("<Enter>", lambda e: close_btn.config(fg="#ff5555"))
        close_btn.bind("<Leave>", lambda e: close_btn.config(fg="#888899"))

        self._drag_data = {"x": 0, "y": 0}
        title_frame.bind("<Button-1>", self._drag_start)
        title_frame.bind("<B1-Motion>", self._drag_move)

        # ── Content ──
        content = tk.Frame(self.win, bg="#1a1d2e", padx=20, pady=14)
        content.pack(fill="both", expand=True)

        tk.Frame(content, bg="#2a2d45", height=1).pack(fill="x", pady=(0, 14))

        # ─── HOTKEY ───
        tk.Label(
            content, text="ATALHO", fg="#6b6e80", bg="#1a1d2e",
            font=("Segoe UI", 8, "bold"), anchor="w"
        ).pack(fill="x")

        hk_frame = tk.Frame(content, bg="#252840", highlightbackground="#3a3d55",
                            highlightthickness=1, padx=10, pady=8)
        hk_frame.pack(fill="x", pady=(4, 14))

        hk_row = tk.Frame(hk_frame, bg="#252840")
        hk_row.pack(fill="x")

        current_hk = self.settings.get("hotkey", "Alt+H")
        self._hk_label = tk.Label(
            hk_row, text=f"  {current_hk}  ", fg="#4dd964",
            bg="#1e2035", font=("Consolas", 13, "bold"), padx=8, pady=4
        )
        self._hk_label.pack(side="left")

        self._rec_btn = tk.Label(
            hk_row, text=" Gravar ", fg="#ffffff", bg="#3a3d55",
            font=("Segoe UI", 9, "bold"), padx=8, pady=4, cursor="hand2"
        )
        self._rec_btn.pack(side="right")
        self._rec_btn.bind("<Button-1>", lambda e: self._do_capture())
        self._rec_btn.bind("<Enter>", lambda e: self._rec_btn.config(bg="#4a4d65"))
        self._rec_btn.bind("<Leave>", lambda e: self._rec_btn.config(bg="#3a3d55"))

        tk.Label(
            hk_frame, text="Clique Gravar e pressione sua combinacao",
            fg="#555870", bg="#252840", font=("Segoe UI", 8), anchor="w"
        ).pack(fill="x", pady=(4, 0))

        # ─── MICROPHONE (Dropdown - auto-save on change) ───
        tk.Label(
            content, text="MICROFONE", fg="#6b6e80", bg="#1a1d2e",
            font=("Segoe UI", 8, "bold"), anchor="w"
        ).pack(fill="x", pady=(4, 0))

        mic_frame = tk.Frame(content, bg="#252840", highlightbackground="#3a3d55",
                             highlightthickness=1, padx=10, pady=10)
        mic_frame.pack(fill="x", pady=(4, 0))

        self.mics = []
        saved_name = self.settings.get("microphone_name") or ""
        saved_idx = self.settings.get("microphone_index")
        self._mic_options = ["Padrao do Sistema"]
        self._mic_entries = {"Padrao do Sistema": (None, "")}
        initial_opt = "Padrao do Sistema"
        if saved_name:
            short = saved_name[:34] + "..." if len(saved_name) > 34 else saved_name
            initial_opt = short
            self._mic_options.append(initial_opt)
            self._mic_entries[initial_opt] = (saved_idx, saved_name)

        self._populating_mics = True
        self.mic_var = tk.StringVar(value=initial_opt)
        self.mic_var.trace_add("write", lambda *_: self._save_mic())

        mic_menu = tk.OptionMenu(mic_frame, self.mic_var, *self._mic_options)
        mic_menu.config(
            bg="#1e2035", fg="#e0e2f0", font=("Segoe UI", 10),
            activebackground="#3a3d55", activeforeground="#ffffff",
            highlightthickness=0, relief="flat", padx=6, pady=4,
            cursor="hand2"
        )
        mic_menu["menu"].config(
            bg="#1e2035", fg="#e0e2f0", font=("Segoe UI", 10),
            activebackground="#4dd964", activeforeground="#1a1d2e",
            relief="flat"
        )
        mic_menu.pack(fill="x")
        self._mic_menu_widget = mic_menu
        self._populating_mics = False

        # Load microphones asynchronously so opening Settings never blocks Tk UI
        self._load_mics_async(win_ref=self.win)

        # ─── ENGINE SELECTION ───
        tk.Label(
            content, text="MOTOR DE TRANSCRIÇÃO", fg="#6b6e80", bg="#1a1d2e",
            font=("Segoe UI", 8, "bold"), anchor="w"
        ).pack(fill="x", pady=(12, 0))

        engine_frame = tk.Frame(content, bg="#252840", highlightbackground="#3a3d55",
                                highlightthickness=1, padx=10, pady=8)
        engine_frame.pack(fill="x", pady=(4, 0))

        self.engine_var = tk.StringVar(value=self.settings.get("engine", "Google (Gratuito)"))
        if self.engine_var.get() not in ["Google (Gratuito)", "Groq (Whisper API)"]:
            self.engine_var.set("Google (Gratuito)")

        self.engine_var.trace_add("write", lambda *_: self._on_engine_change())
        
        engine_menu = tk.OptionMenu(engine_frame, self.engine_var, "Google (Gratuito)", "Groq (Whisper API)")
        engine_menu.config(
            bg="#1e2035", fg="#e0e2f0", font=("Segoe UI", 10),
            activebackground="#3a3d55", activeforeground="#ffffff",
            highlightthickness=0, relief="flat", padx=6, pady=4,
            cursor="hand2"
        )
        engine_menu["menu"].config(
            bg="#1e2035", fg="#e0e2f0", font=("Segoe UI", 10),
            activebackground="#4dd964", activeforeground="#1a1d2e",
            relief="flat"
        )
        engine_menu.pack(fill="x")

        # API Key input (shown only if Groq is selected)
        self.api_key_frame = tk.Frame(engine_frame, bg="#252840")
        tk.Label(
            self.api_key_frame, text="Groq API Key:", fg="#888899", bg="#252840",
            font=("Segoe UI", 8)
        ).pack(side="left", padx=(0, 5))
        
        self.api_key_var = tk.StringVar(value=self.settings.get("groq_api_key", ""))
        self.api_key_var.trace_add("write", lambda *_: self._schedule_api_key_save())
        self.api_key_entry = tk.Entry(
            self.api_key_frame, textvariable=self.api_key_var, show="*",
            bg="#1a1d2e", fg="#e0e2f0", insertbackground="white",
            relief="flat", highlightthickness=1, highlightbackground="#3a3d55",
            font=("Consolas", 9)
        )
        self.api_key_entry.pack(side="left", fill="x", expand=True)

        self._key_visible = False
        self.toggle_eye_btn = tk.Label(
            self.api_key_frame, text="👁", fg="#888899", bg="#252840",
            font=("Segoe UI", 10), cursor="hand2", padx=5
        )
        self.toggle_eye_btn.pack(side="left")
        self.toggle_eye_btn.bind("<Button-1>", lambda e: self._toggle_api_visibility())
        self.toggle_eye_btn.bind("<Enter>", lambda e: self.toggle_eye_btn.config(fg="#ffffff"))
        self.toggle_eye_btn.bind("<Leave>", lambda e: self.toggle_eye_btn.config(fg="#888899"))

        if self.engine_var.get() == "Groq (Whisper API)":
            self.api_key_frame.pack(fill="x", pady=(8, 0))

        # ─── LANGUAGE SELECTION ───
        tk.Label(
            content, text="IDIOMA DA FALA", fg="#6b6e80", bg="#1a1d2e",
            font=("Segoe UI", 8, "bold"), anchor="w"
        ).pack(fill="x", pady=(12, 0))

        lang_frame = tk.Frame(content, bg="#252840", highlightbackground="#3a3d55",
                              highlightthickness=1, padx=10, pady=8)
        lang_frame.pack(fill="x", pady=(4, 0))

        self._lang_map = {
            "Português (Brasil)": "pt-BR",
            "Automático (detectar idioma)": "auto",
            "Traduzir para Inglês (PT ➔ EN)": "translate_en",
            "Traduzir para Espanhol (PT ➔ ES)": "translate_es",
        }
        self._lang_reverse = {v: k for k, v in self._lang_map.items()}

        current_lang = self.settings.get("language", "pt-BR")
        display_lang = self._lang_reverse.get(current_lang, "Português (Brasil)")

        self.lang_var = tk.StringVar(value=display_lang)
        self.lang_var.trace_add("write", lambda *_: self._on_lang_change())

        lang_menu = tk.OptionMenu(lang_frame, self.lang_var, *self._lang_map.keys())
        lang_menu.config(
            bg="#1e2035", fg="#e0e2f0", font=("Segoe UI", 10),
            activebackground="#3a3d55", activeforeground="#ffffff",
            highlightthickness=0, relief="flat", padx=6, pady=4,
            cursor="hand2"
        )
        lang_menu["menu"].config(
            bg="#1e2035", fg="#e0e2f0", font=("Segoe UI", 10),
            activebackground="#4dd964", activeforeground="#1a1d2e",
            relief="flat"
        )
        lang_menu.pack(fill="x")

        # ─── AUTO PUNCTUATION ───
        tk.Label(
            content, text="OPCOES DE TEXTO", fg="#6b6e80", bg="#1a1d2e",
            font=("Segoe UI", 8, "bold"), anchor="w"
        ).pack(fill="x", pady=(12, 0))

        punct_frame = tk.Frame(content, bg="#252840", highlightbackground="#3a3d55",
                               highlightthickness=1, padx=10, pady=8)
        punct_frame.pack(fill="x", pady=(4, 0))

        self.punct_var = tk.BooleanVar(value=self.settings.get("auto_punctuation", True))
        
        punct_cb = tk.Checkbutton(
            punct_frame, text=" Pontuacao Automatica",
            variable=self.punct_var, command=self._save_punct,
            bg="#252840", fg="#e0e2f0", selectcolor="#1e2035",
            activebackground="#252840", activeforeground="#ffffff",
            font=("Segoe UI", 9), cursor="hand2", relief="flat", highlightthickness=0
        )
        punct_cb.pack(anchor="w")

    def _on_lang_change(self):
        display = self.lang_var.get()
        code = self._lang_map.get(display, "pt-BR")
        self.settings["language"] = code
        save_settings(self.settings)
        print(f"[OK] Idioma alterado: {display} ({code})", flush=True)
        if self.on_save:
            self.on_save(self.settings)

    def _save_punct(self):
        """Auto-save auto-punctuation setting."""
        val = self.punct_var.get()
        self.settings["auto_punctuation"] = val
        save_settings(self.settings)
        print(f"[OK] Pontuacao automatica: {'Ativado' if val else 'Desativado'}", flush=True)
        if self.on_save:
            self.on_save(self.settings)

    def _on_engine_change(self):
        val = self.engine_var.get()
        self.settings["engine"] = val
        save_settings(self.settings)
        print(f"[OK] Motor alterado: {val}", flush=True)
        if val == "Groq (Whisper API)":
            self.api_key_frame.pack(fill="x", pady=(8, 0))
        else:
            self.api_key_frame.pack_forget()
        if self.on_save:
            self.on_save(self.settings)

    def _toggle_api_visibility(self):
        self._key_visible = not self._key_visible
        if self._key_visible:
            self.api_key_entry.config(show="")
            self.toggle_eye_btn.config(text="🙈")
        else:
            self.api_key_entry.config(show="*")
            self.toggle_eye_btn.config(text="👁")

    def _schedule_api_key_save(self):
        """Debounce: saving on every keystroke re-registered the global hotkey
        and re-probed the microphone dozens of times while the key was typed."""
        if self._api_key_job:
            try:
                self.win.after_cancel(self._api_key_job)
            except Exception:
                pass
        self._api_key_job = self.win.after(700, self._save_api_key)

    def _save_api_key(self):
        self._api_key_job = None
        val = self.api_key_var.get()
        if val == self.settings.get("groq_api_key", ""):
            return
        self.settings["groq_api_key"] = val
        save_settings(self.settings)
        # Don't print API key for security
        if self.on_save:
            self.on_save(self.settings)

    def _do_capture(self):
        if self._capture:
            self._capture.stop()
        self._rec_btn.config(text=" ...  ", bg="#d4a017")
        self._capture = HotkeyCapture(self._hk_label, self._on_hotkey_done)
        self._capture.start()

    def _on_hotkey_done(self, hotkey_str):
        """Auto-save hotkey immediately after capture."""
        self.settings["hotkey"] = hotkey_str
        save_settings(self.settings)
        print(f"[OK] Atalho salvo: {hotkey_str}", flush=True)
        self.win.after(0, lambda: self._rec_btn.config(text=" Gravar ", bg="#3a3d55"))
        if self.on_save:
            self.on_save(self.settings)

    def _load_mics_async(self, win_ref):
        """Enumerate microphones in background and populate OptionMenu safely."""
        def _worker():
            try:
                mics = get_microphones()
            except Exception:
                mics = []
            try:
                if win_ref and win_ref.winfo_exists():
                    win_ref.after(0, lambda m=mics, w=win_ref: self._apply_mics(m, w))
            except Exception:
                pass

        threading.Thread(target=_worker, daemon=True).start()

    def _apply_mics(self, mics, win_ref):
        if self.win is not win_ref:
            return
        try:
            if not (self.win and self.win.winfo_exists()):
                return
        except Exception:
            return

        self.mics = mics
        self._mic_options = ["Padrao do Sistema"]
        self._mic_entries = {"Padrao do Sistema": (None, "")}
        for idx, name, api in self.mics:
            short = name[:34] + "..." if len(name) > 34 else name
            label = f"{short} [{_short_api(api)}]"
            if label in self._mic_entries:  # keep labels unique
                label = f"{label} #{idx}"
            self._mic_options.append(label)
            self._mic_entries[label] = (idx, name)

        current_idx = self.settings.get("microphone_index")
        current_sel = 0
        if current_idx is not None:
            for i, opt in enumerate(self._mic_options):
                if self._mic_entries[opt][0] == current_idx:
                    current_sel = i
                    break
        if current_sel == 0 and self.settings.get("microphone_name"):
            for i, opt in enumerate(self._mic_options):
                if device_name_matches(self.settings["microphone_name"], self._mic_entries[opt][1]):
                    current_sel = i
                    break

        self._populating_mics = True
        try:
            menu = self._mic_menu_widget["menu"]
            menu.delete(0, "end")
            for opt in self._mic_options:
                menu.add_command(
                    label=opt,
                    command=tk._setit(self.mic_var, opt),
                )
            self.mic_var.set(self._mic_options[current_sel])
        finally:
            self._populating_mics = False

    def _save_mic(self):
        """Auto-save microphone immediately after selection."""
        if getattr(self, "_populating_mics", False):
            return
        selected = self.mic_var.get()
        # Look the option up in the label map: matching display strings was
        # ambiguous and saved the wrong device index.
        mic_idx, mic_name = self._mic_entries.get(selected, (None, ""))

        self.settings["microphone_index"] = mic_idx
        self.settings["microphone_name"] = mic_name
        save_settings(self.settings)
        print(f"[OK] Microfone salvo: {mic_name or 'padrao'} (idx={mic_idx})", flush=True)
        if self.on_save:
            self.on_save(self.settings)

    def _close(self):
        if self._capture:
            self._capture.stop()
        if self._api_key_job:
            try:
                self.win.after_cancel(self._api_key_job)
            except Exception:
                pass
            self._api_key_job = None
            self._save_api_key()
        if self.win:
            self.win.destroy()
            self.win = None

    def _drag_start(self, event):
        self._drag_data["x"] = event.x_root - self.win.winfo_x()
        self._drag_data["y"] = event.y_root - self.win.winfo_y()

    def _drag_move(self, event):
        x = event.x_root - self._drag_data["x"]
        y = event.y_root - self._drag_data["y"]
        self.win.geometry(f"+{x}+{y}")
