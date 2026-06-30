#!/usr/bin/env python3
"""
Voice Dictation Tool – Windows Floating Popup
Replicates Windows Win+H voice typing experience.
Configurable hotkey and microphone via Settings panel.
Uses Google's free Speech Recognition API for real-time transcription.
"""

import logging
import math
import os
import platform
import signal
import sys
import threading

# Fix Windows console encoding
if sys.stdout and hasattr(sys.stdout, 'reconfigure'):
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
import time
import tkinter as tk
import tempfile

import ctypes
import pyaudio
import speech_recognition as sr
from pynput import keyboard
from pynput.keyboard import Key

from settings_manager import load_settings, save_settings
from settings_dialog import SettingsDialog
import struct

try:
    import audioop
except ImportError:
    audioop = None

def get_rms(buffer, sample_width):
    if audioop is not None:
        try:
            return audioop.rms(buffer, sample_width)
        except Exception:
            pass
    # Fallback para Python 3.13+ (onde audioop foi removido) ou falhas
    if not buffer:
        return 0
    if sample_width == 2:
        fmt = f"<{len(buffer)//2}h"
        try:
            samples = struct.unpack(fmt, buffer)[::8]
            sum_squares = sum(s * s for s in samples)
            return int(math.sqrt(sum_squares / len(samples)))
        except Exception:
            return 0
    elif sample_width == 1:
        fmt = f"<{len(buffer)}B"
        try:
            samples = struct.unpack(fmt, buffer)[::8]
            sum_squares = sum((s - 128) * (s - 128) for s in samples)
            return int(math.sqrt(sum_squares / len(samples)))
        except Exception:
            return 0
    return 0


# ─── File Logging (essential for pythonw.exe) ────────────────────────────────

_LOG_DIR = os.path.dirname(os.path.abspath(__file__))
_LOG_FILE = os.path.join(_LOG_DIR, "voice_dictation.log")

logging.basicConfig(
    filename=_LOG_FILE,
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
    encoding="utf-8",
)
log = logging.getLogger("VoiceDictation")

# Also print to console if available
class _DualHandler:
    def write(self, msg):
        if msg.strip():
            log.info(msg.strip())
        if sys.__stdout__:
            try:
                sys.__stdout__.write(msg)
                sys.__stdout__.flush()
            except Exception:
                pass
    def flush(self):
        if sys.__stdout__:
            try:
                sys.__stdout__.flush()
            except Exception:
                pass

sys.stdout = _DualHandler()
sys.stderr = _DualHandler()

# ─── Configuration ───────────────────────────────────────────────────────────

SAMPLE_RATE = 16000
CHUNK_SIZE = 1024
PHRASE_TIME_LIMIT = 300       # 5 minutes - keeps listening until you stop
PAUSE_THRESHOLD = 60.0         # Allow long pauses (1 minute) before transcribing
ENERGY_THRESHOLD = 250
DYNAMIC_ENERGY = True

IS_WINDOWS = platform.system() == "Windows"
IS_LINUX = platform.system() == "Linux"

# ─── Hotkey parsing ──────────────────────────────────────────────────────────

HOTKEY_MAP = {
    "Alt": {keyboard.Key.alt_l, keyboard.Key.alt_r},
    "Ctrl": {keyboard.Key.ctrl_l, keyboard.Key.ctrl_r},
    "Shift": {keyboard.Key.shift_l, keyboard.Key.shift_r},
    "Win": {keyboard.Key.cmd, keyboard.Key.cmd_l, keyboard.Key.cmd_r},
}


def parse_hotkey(hotkey_str):
    """Parse hotkey string like 'Ctrl+Shift+D' into (modifier_sets, char)."""
    parts = hotkey_str.split("+")
    char = parts[-1].lower()
    mods = []
    for p in parts[:-1]:
        p_stripped = p.strip()
        if p_stripped in HOTKEY_MAP:
            mods.append(HOTKEY_MAP[p_stripped])
    return mods, char


# ─── Duplicate prevention ───────────────────────────────────────────────────

def prevent_duplicate():
    lock_path = os.path.join(tempfile.gettempdir(), "voice_dictation.lock")
    lock_file = open(lock_path, "w")

    if IS_WINDOWS:
        import msvcrt
        try:
            msvcrt.locking(lock_file.fileno(), msvcrt.LK_NBLCK, 1)
            lock_file.write(str(os.getpid()))
            lock_file.flush()
        except (IOError, OSError):
            print("[!] Voice Dictation já está rodando. Saindo.")
            sys.exit(0)
    else:
        import fcntl
        try:
            fcntl.flock(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
            lock_file.write(str(os.getpid()))
            lock_file.flush()
        except IOError:
            print("[!] Voice Dictation já está rodando. Saindo.")
            sys.exit(0)

    return lock_file

# ─── Auto Punctuation ────────────────────────────────────────────────────────

_QUESTION_WORDS = (
    "como", "o que", "qual", "quais", "quem", "onde", "quando",
    "por que", "porque", "por quê", "cadê", "quanto",
    "quantos", "quantas", "quanta", "de quem", "pra que", "para que",
    "aonde"
)

_QUESTION_PHRASES = (
    "será que", "é que", "não é", "né", "tem como", "dá pra", "da pra",
    "você acha", "vc acha", "você gosta", "vc gosta", "você quer", "vc quer",
    "você sabe", "vc sabe", "você consegue", "vc consegue",
    "você pode", "vc pode", "posso", "poderia", "conseguiria",
    "você tem", "vc tem"
)

# Voice commands: say these words to insert punctuation
_VOICE_PUNCTUATION = {
    "vírgula": ",",
    "virgula": ",",
    "ponto final": ".",
    "ponto de exclamação": "!",
    "ponto de exclamacao": "!",
    "ponto de interrogação": "?",
    "ponto de interrogacao": "?",
    "dois pontos": ":",
    "ponto e vírgula": ";",
    "ponto e virgula": ";",
    "abre parênteses": "(",
    "abre parenteses": "(",
    "fecha parênteses": ")",
    "fecha parenteses": ")",
    "reticências": "...",
    "reticencias": "...",
    "travessão": " —",
    "travessao": " —",
    "nova linha": "\n",
    "parágrafo": "\n\n",
    "paragrafo": "\n\n",
}

# Words that typically get a comma BEFORE them
_COMMA_BEFORE = [
    "mas", "porém", "porem", "contudo", "entretanto", "todavia",
    "ou seja", "por exemplo", "enfim", "então", "entao",
    "porque", "pois", "portanto", "assim", "além disso",
    "além do mais", "aliás", "alias", "inclusive", "na verdade",
    "tipo", "sabe", "tá ligado", "ta ligado", "entendeu",
    "no entanto", "apesar de", "embora",
]


def _apply_voice_commands(text):
    """Replace spoken punctuation commands with actual symbols."""
    # Sort by length (longest first) to match multi-word commands first
    for cmd in sorted(_VOICE_PUNCTUATION.keys(), key=len, reverse=True):
        symbol = _VOICE_PUNCTUATION[cmd]
        # Match case-insensitive, with possible spaces around
        lower = text.lower()
        idx = lower.find(cmd)
        while idx != -1:
            # Remove the command text and insert the symbol
            before = text[:idx].rstrip()
            after = text[idx + len(cmd):].lstrip()
            if symbol in ("\n", "\n\n"):
                text = before + symbol + after
            else:
                text = before + symbol + " " + after
            lower = text.lower()
            idx = lower.find(cmd)
    return text


def _insert_commas(text):
    """Insert commas before common connector words."""
    words = text.split()
    result = []
    i = 0
    while i < len(words):
        # Check multi-word connectors first (2-3 words)
        matched = False
        for length in (3, 2):
            if i + length <= len(words):
                phrase = " ".join(words[i:i+length]).lower()
                # Strip existing punctuation for matching
                phrase_clean = phrase.rstrip(".,;:!?")
                if phrase_clean in _COMMA_BEFORE:
                    # Add comma before this connector if previous word doesn't have punctuation
                    if result and result[-1][-1] not in ".,;:!?—(":
                        result[-1] = result[-1] + ","
                    result.extend(words[i:i+length])
                    i += length
                    matched = True
                    break

        if not matched:
            word_lower = words[i].lower().rstrip(".,;:!?")
            if word_lower in _COMMA_BEFORE and i > 0:
                if result and result[-1][-1] not in ".,;:!?—(":
                    result[-1] = result[-1] + ","
            result.append(words[i])
            i += 1

    return " ".join(result)


def auto_punctuate(text):
    """Add capitalization, commas, and ending punctuation to raw speech text."""
    if not text:
        return text

    text = text.strip()

    # 1. Replace voice commands ("vírgula" -> ",")
    text = _apply_voice_commands(text)

    # 2. Insert commas before connectors
    text = _insert_commas(text)

    # 3. Capitalize first letter
    if text and text[0].isalpha():
        text = text[0].upper() + text[1:]

    # 4. Capitalize after . ! ? and newlines
    result = []
    capitalize_next = False
    for ch in text:
        if capitalize_next and ch.isalpha():
            result.append(ch.upper())
            capitalize_next = False
        else:
            result.append(ch)
        if ch in ".!?\n":
            capitalize_next = True
    text = "".join(result)

    # 5. Don't add ending punctuation if already has one
    text = text.rstrip()
    if text and text[-1] in ".!?;:…":
        return text

    # Default to period
    text += "."

    return text


def is_capslock_on():
    """Check if Caps Lock is currently active (Windows only)."""
    if IS_WINDOWS:
        try:
            return bool(ctypes.windll.user32.GetKeyState(0x14) & 1)
        except Exception:
            return False
    return False


# ─── Application ─────────────────────────────────────────────────────────────

class VoiceDictationApp:
    def __init__(self):
        self.is_listening = False
        self.is_visible = False
        self.stop_event = threading.Event()
        self.force_stop_audio = False

        self._audio_level = 0.0
        self._mic_active = False
        self._pulse_phase = 0.0
        self._is_processing = False
        self._stop_listening_fn = None
        self._drag_data = {"x": 0, "y": 0}
        self._anim_id = None
        self._target_hwnd = None  # Window handle where text will be pasted
        self._positioned = False  # Only calculate position once
        self.x = None
        self.y = None

        # Load settings
        self.settings = load_settings()

        # Speech recognizer
        self.recognizer = sr.Recognizer()
        self.recognizer.pause_threshold = PAUSE_THRESHOLD
        self.recognizer.energy_threshold = ENERGY_THRESHOLD
        self.recognizer.dynamic_energy_threshold = DYNAMIC_ENERGY
        self.recognizer.dynamic_energy_adjustment_damping = 0.15
        self.recognizer.dynamic_energy_ratio = 1.3
        self.recognizer.phrase_threshold = 0.15
        self.recognizer.non_speaking_duration = 60.0
        self.recognizer.operation_timeout = None
        self._status_gen = 0  # Generation counter for status updates

        self._build_ui()
        self._setup_hotkey()

        # Settings dialog
        self.settings_dialog = SettingsDialog(
            self.root, on_save_callback=self._on_settings_saved
        )

    # ── UI Construction ──────────────────────────────────────────────────

    def _build_ui(self):
        self.root = tk.Tk()
        self.root.withdraw()
        self.root.title("Voice Dictation")
        self.root.overrideredirect(True)
        self.root.attributes("-topmost", True)

        if IS_WINDOWS:
            self.root.attributes("-transparentcolor", "#010101")
            bg_color = "#010101"
        else:
            bg_color = "#1e2130"

        self.root.configure(bg=bg_color)

        self.WIDTH = 240
        self.HEIGHT = 130

        self.canvas = tk.Canvas(
            self.root, width=self.WIDTH, height=self.HEIGHT,
            bg=bg_color, highlightthickness=0, bd=0
        )
        self.canvas.pack()

        self._draw_background()

        # Drag handle
        self.canvas.create_rectangle(
            self.WIDTH // 2 - 20, 10,
            self.WIDTH // 2 + 20, 14,
            fill="#3a3d4d", outline="", tags="handle"
        )

        # Close button
        self._close_btn = self.canvas.create_text(
            self.WIDTH - 18, 14, text="✕",
            fill="#888899", font=("Segoe UI", 12, "bold"),
            tags="close"
        )
        self.canvas.tag_bind("close", "<Button-1>", self._on_close)
        self.canvas.tag_bind("close", "<Enter>",
                             lambda e: self.canvas.itemconfig(self._close_btn, fill="#ffffff"))
        self.canvas.tag_bind("close", "<Leave>",
                             lambda e: self.canvas.itemconfig(self._close_btn, fill="#888899"))

        # Mic button center
        self.mic_cx = self.WIDTH // 2
        self.mic_cy = 62
        self.mic_radius = 26
        self.history = []  # Store last 5 transcriptions
        
        # Gear icon (opens settings)
        self._gear = self.canvas.create_text(
            self.mic_cx - 55, self.mic_cy, text="⚙",
            fill="#6b6e80", font=("Segoe UI", 16), tags="gear"
        )
        self.canvas.tag_bind("gear", "<Button-1>", self._on_gear_click)
        self.canvas.tag_bind("gear", "<Enter>",
                             lambda e: self.canvas.itemconfig(self._gear, fill="#ffffff"))
        self.canvas.tag_bind("gear", "<Leave>",
                             lambda e: self.canvas.itemconfig(self._gear, fill="#6b6e80"))

        # Help icon (History)
        self._help = self.canvas.create_text(
            self.mic_cx + 55, self.mic_cy, text="?",
            fill="#6b6e80", font=("Segoe UI", 16), tags="help"
        )
        self.canvas.tag_bind("help", "<Button-1>", self._on_help_click)
        self.canvas.tag_bind("help", "<Enter>",
                             lambda e: self.canvas.itemconfig(self._help, fill="#ffffff"))
        self.canvas.tag_bind("help", "<Leave>",
                             lambda e: self.canvas.itemconfig(self._help, fill="#6b6e80"))

        # Mic glow + circle + icon
        self._mic_glow = self.canvas.create_oval(
            self.mic_cx - 34, self.mic_cy - 34,
            self.mic_cx + 34, self.mic_cy + 34,
            fill="", outline="", tags="mic_area"
        )
        self._mic_circle = self.canvas.create_oval(
            self.mic_cx - self.mic_radius, self.mic_cy - self.mic_radius,
            self.mic_cx + self.mic_radius, self.mic_cy + self.mic_radius,
            fill="#555870", outline="", tags="mic_area"
        )
        self._mic_icon = self.canvas.create_text(
            self.mic_cx, self.mic_cy, text="🎙",
            fill="white", font=("Segoe UI", 16), tags="mic_area"
        )
        self.canvas.tag_bind("mic_area", "<Button-1>", self._on_mic_click)

        # Status label
        self._status_text = self.canvas.create_text(
            self.WIDTH // 2, 100, text="",
            fill="#b0b3c5", font=("Segoe UI", 10), tags="status"
        )

        # Partial label
        self._partial_text = self.canvas.create_text(
            self.WIDTH // 2, 118, text="",
            fill="#7a7d90", font=("Segoe UI", 9, "italic"),
            width=self.WIDTH - 30, tags="partial"
        )

        # Drag bindings
        self.canvas.tag_bind("handle", "<Button-1>", self._drag_start)
        self.canvas.tag_bind("handle", "<B1-Motion>", self._drag_move)
        self.canvas.bind("<Button-1>", self._bg_click)
        self.canvas.bind("<B1-Motion>", self._drag_move)
        self.canvas.bind("<ButtonRelease-1>", self._drag_stop)

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        # Floating status window
        self.status_win = tk.Toplevel(self.root)
        self.status_win.withdraw()
        self.status_win.overrideredirect(True)
        self.status_win.attributes("-topmost", True)

        if IS_WINDOWS:
            self.status_win.attributes("-transparentcolor", "#010101")
            sw_bg = "#010101"
        else:
            sw_bg = "#1e2135"

        self.status_win.configure(bg=sw_bg)

        sw_canvas = tk.Canvas(
            self.status_win, width=140, height=34,
            bg=sw_bg, highlightthickness=0, bd=0
        )
        sw_canvas.pack()

        self._draw_rounded_rect(sw_canvas, 2, 2, 138, 32, 10, fill="#1e2135", outline="#3a3d55")

        self._sw_label = sw_canvas.create_text(
            70, 17, text="Ouvindo...",
            fill="#e0e2f0", font=("Segoe UI", 11, "bold")
        )
        self.sw_canvas = sw_canvas

        # Polling background thread for tracking target window
        if IS_WINDOWS:
            self._poll_target_window()

    def _poll_target_window(self):
        """Poll the foreground window to track where to paste text."""
        try:
            import ctypes.wintypes
            hwnd = ctypes.windll.user32.GetForegroundWindow()
            if hwnd:
                our_pid = os.getpid()
                pid = ctypes.wintypes.DWORD()
                ctypes.windll.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                if pid.value != our_pid:
                    self._target_hwnd = hwnd
        except Exception:
            pass
        # Poll every 200ms
        self.root.after(200, self._poll_target_window)

    def _draw_background(self):
        self._draw_rounded_rect(
            self.canvas, 2, 2, self.WIDTH - 2, self.HEIGHT - 2,
            18, fill="#1e2130", outline="#3a3d55"
        )

    def _draw_rounded_rect(self, canvas, x1, y1, x2, y2, r, **kwargs):
        points = [
            x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r,
            x2, y2 - r, x2, y2, x2 - r, y2, x1 + r, y2,
            x1, y2, x1, y2 - r, x1, y1 + r, x1, y1, x1 + r, y1,
        ]
        return canvas.create_polygon(points, smooth=True, **kwargs)

    # ── Settings ─────────────────────────────────────────────────────────

    def _on_gear_click(self, event):
        self.settings_dialog.show()

    def _on_settings_saved(self, new_settings):
        old_hotkey = self.settings.get("hotkey")
        self.settings = new_settings

        # Re-register hotkey with new settings
        self._setup_hotkey()

        print(f"[OK] Novo atalho: {new_settings['hotkey']}", flush=True)
        print(f"[OK] Mic: {new_settings.get('microphone_name') or 'padrao'}", flush=True)

    # ── Mic Animation ────────────────────────────────────────────────────

    def _update_mic_visual(self):
        if self._mic_active or self._is_processing:
            self._pulse_phase += 0.08
            if self._pulse_phase > 2 * math.pi:
                self._pulse_phase -= 2 * math.pi

            alpha_factor = 0.3 + 0.2 * math.sin(self._pulse_phase)

            if self._is_processing:
                color = "#d4a017" # Amber / Yellow
                glow_color = self._alpha_hex("#d4a017", alpha_factor)
                # Pulse the glow size dynamically during transcription for a super cool thinking/processing effect!
                glow_r = 34 + (0.5 + 0.3 * math.sin(self._pulse_phase * 1.5)) * 10
            else:
                color = "#4dd964" # Green
                glow_color = self._alpha_hex("#4dd964", alpha_factor)
                glow_r = 34 + self._audio_level * 10

            self.canvas.coords(
                self._mic_glow,
                self.mic_cx - glow_r, self.mic_cy - glow_r,
                self.mic_cx + glow_r, self.mic_cy + glow_r
            )
            self.canvas.itemconfig(self._mic_glow, fill=glow_color, outline="")
            self.canvas.itemconfig(self._mic_circle, fill=color)

            self._anim_id = self.root.after(33, self._update_mic_visual)
        else:
            self.canvas.itemconfig(self._mic_glow, fill="", outline="")
            self.canvas.itemconfig(self._mic_circle, fill="#555870") # Grey
            self._anim_id = None

    def _alpha_hex(self, hex_color, alpha):
        r = int(hex_color[1:3], 16)
        g = int(hex_color[3:5], 16)
        b = int(hex_color[5:7], 16)
        bg_r, bg_g, bg_b = 0x1e, 0x21, 0x30
        out_r = int(r * alpha + bg_r * (1 - alpha))
        out_g = int(g * alpha + bg_g * (1 - alpha))
        out_b = int(b * alpha + bg_b * (1 - alpha))
        return f"#{out_r:02x}{out_g:02x}{out_b:02x}"

    def _start_anim(self):
        if self._anim_id is None:
            self._pulse_phase = 0.0
            self._update_mic_visual()

    def _stop_anim(self):
        if self._anim_id is not None:
            self.root.after_cancel(self._anim_id)
            self._anim_id = None
        self._audio_level = 0.0
        self._mic_active = False
        self.canvas.itemconfig(self._mic_glow, fill="", outline="")
        self.canvas.itemconfig(self._mic_circle, fill="#555870")

    # ── Drag ─────────────────────────────────────────────────────────────

    def _bg_click(self, event):
        tags = self.canvas.gettags("current")
        if "close" in tags or "mic_area" in tags or "gear" in tags or "help" in tags:
            self._is_dragging = False
            return
        self._drag_start(event)

    def _drag_start(self, event):
        self._is_dragging = True
        if self.x is None or self.y is None:
            self.x = self.root.winfo_x()
            self.y = self.root.winfo_y()
        self._drag_data["x"] = event.x_root - self.x
        self._drag_data["y"] = event.y_root - self.y

    def _drag_move(self, event):
        if not getattr(self, "_is_dragging", False):
            return
        self.x = event.x_root - self._drag_data["x"]
        self.y = event.y_root - self._drag_data["y"]
        self.root.geometry(f"+{self.x}+{self.y}")
        self._update_status_pos()
        
    def _drag_stop(self, event):
        self._is_dragging = False

    # ── Positioning ──────────────────────────────────────────────────────

    def _position(self):
        """Position the window. On first call, center near bottom.
        On subsequent calls, verify the window is still on-screen
        (handles Win+P monitor changes) and recenter if needed."""
        screen_w = self.root.winfo_screenwidth()
        screen_h = self.root.winfo_screenheight()

        if self._positioned and self.x is not None and self.y is not None:
            # Check if current position is still visible
            # If the window is off-screen, recenter it
            if (self.x + self.WIDTH < 0 or self.x > screen_w or
                    self.y + self.HEIGHT < 0 or self.y > screen_h):
                self._positioned = False  # Force reposition

        if not self._positioned or self.x is None or self.y is None:
            self.x = (screen_w - self.WIDTH) // 2
            self.y = screen_h - self.HEIGHT - 80
            self._positioned = True

        self.root.geometry(f"{self.WIDTH}x{self.HEIGHT}+{self.x}+{self.y}")
        self.root.update_idletasks()

    def _update_status_pos(self):
        if not self.is_visible:
            self.status_win.withdraw()
            return
        if self.x is None or self.y is None:
            self.x = self.root.winfo_x()
            self.y = self.root.winfo_y()
        self.status_win.deiconify()
        x = self.x + (self.WIDTH - 140) // 2
        y = self.y - 44
        self.status_win.geometry(f"140x34+{x}+{y}")

    # ── Controls ─────────────────────────────────────────────────────────

    def _on_close(self, event=None):
        self._stop_listening()
        self._hide()

    def _on_mic_click(self, event):
        self.toggle_mic()

    def _on_help_click(self, event):
        menu = tk.Menu(self.root, tearoff=0, bg="#252840", fg="#e0e2f0", 
                       activebackground="#3a3d55", activeforeground="#ffffff", 
                       font=("Segoe UI", 9))
        
        if not self.history:
            menu.add_command(label="Nenhum histórico salvo.", state="disabled")
        else:
            menu.add_command(label="📋 HISTÓRICO (Clique para colar no último app)", state="disabled")
            menu.add_separator()
            # Show newest first
            for text in reversed(self.history):
                display_text = text if len(text) <= 50 else text[:47] + "..."
                menu.add_command(label=display_text, command=lambda t=text: self._inject_history_text(t))
                
        menu.post(event.x_root, event.y_root)

    def _inject_history_text(self, text):
        self._type_text(text)
        self._set_status("Texto colado!")
        self.root.after(2000, lambda: self._set_status("Pausado") if not self.is_listening else None)

    # ── Hotkey ───────────────────────────────────────────────────────────

    def _setup_hotkey(self):
        """Register global hotkey using Windows RegisterHotKey API (never dies)."""
        hotkey_str = self.settings.get("hotkey", "Alt+H")
        self._last_t = 0

        # Parse hotkey string into Windows modifier flags and virtual key
        _MOD_MAP = {"alt": 0x0001, "ctrl": 0x0002, "shift": 0x0004, "win": 0x0008}
        _VK_MAP = {chr(i): i for i in range(0x41, 0x5B)}  # A-Z
        _VK_MAP.update({str(i): 0x30 + i for i in range(10)})  # 0-9
        _VK_MAP.update({
            "f1": 0x70, "f2": 0x71, "f3": 0x72, "f4": 0x73,
            "f5": 0x74, "f6": 0x75, "f7": 0x76, "f8": 0x77,
            "f9": 0x78, "f10": 0x79, "f11": 0x7A, "f12": 0x7B,
            "space": 0x20, "enter": 0x0D, "tab": 0x09,
        })

        parts = [p.strip().lower() for p in hotkey_str.split("+")]
        mod_flags = 0
        vk_code = 0
        for part in parts:
            if part in _MOD_MAP:
                mod_flags |= _MOD_MAP[part]
            elif part in _VK_MAP:
                vk_code = _VK_MAP[part]
            elif len(part) == 1 and part.upper() in _VK_MAP:
                vk_code = _VK_MAP[part.upper()]

        if vk_code == 0:
            print(f"[!] Hotkey invalida: {hotkey_str}", flush=True)
            return

        HOTKEY_ID = 42069

        def _hotkey_thread():
            user32 = ctypes.windll.user32

            # Unregister if previously registered
            user32.UnregisterHotKey(None, HOTKEY_ID)

            # Register the hotkey (MOD_NOREPEAT = 0x4000 prevents repeated triggers)
            result = user32.RegisterHotKey(None, HOTKEY_ID, mod_flags | 0x4000, vk_code)
            if result:
                print(f"[OK] Hotkey {hotkey_str} registrada (RegisterHotKey, permanente)", flush=True)
            else:
                print(f"[!] Falha ao registrar {hotkey_str} via RegisterHotKey", flush=True)
                return

            # Message loop - Windows sends WM_HOTKEY here forever
            from ctypes import wintypes
            msg = wintypes.MSG()
            while True:
                try:
                    ret = user32.GetMessageW(ctypes.byref(msg), None, 0, 0)
                    if ret == 0 or ret == -1:
                        break
                    if msg.message == 0x0312:  # WM_HOTKEY
                        now = time.time()
                        if now - self._last_t > 0.5:
                            self._last_t = now
                            try:
                                # Call Tkinter safely
                                self.root.after(0, self.toggle)
                            except Exception as e:
                                print(f"[!] Erro ao acionar atalho (Tkinter): {e}", flush=True)
                except Exception as e:
                    print(f"[!] Erro no loop de atalho: {e}", flush=True)
                    time.sleep(1)

        # Kill previous thread if re-registering
        if hasattr(self, '_hotkey_thread_ref') and self._hotkey_thread_ref.is_alive():
            try:
                ctypes.windll.user32.UnregisterHotKey(None, HOTKEY_ID)
                ctypes.windll.user32.PostThreadMessageW(
                    self._hotkey_thread_ref.ident, 0x0012, 0, 0  # WM_QUIT
                )
            except Exception:
                pass
            time.sleep(0.3)

        t = threading.Thread(target=_hotkey_thread, daemon=True)
        t.start()
        self._hotkey_thread_ref = t

    # ── Core ─────────────────────────────────────────────────────────────

    def toggle(self):
        if self.is_visible:
            self._stop_listening()
            self._hide()
        else:
            if IS_WINDOWS:
                try:
                    self._target_hwnd = ctypes.windll.user32.GetForegroundWindow()
                except Exception:
                    self._target_hwnd = None
            self._show()
            self._start_listening()

    def toggle_mic(self):
        if self.is_listening:
            self._stop_listening()
        else:
            self._start_listening()

    def _show(self):
        self.is_visible = True
        self._position()
        self.root.deiconify()
        self._set_partial("")
        self._update_status_pos()

    def _hide(self):
        self.is_visible = False
        self.status_win.withdraw()
        self.root.withdraw()

    def _set_status(self, text, gen=None):
        # If a generation was passed, ignore stale updates
        if gen is not None and gen != self._status_gen:
            return
        self.sw_canvas.itemconfig(self._sw_label, text=text)
        if self.is_visible:
            self._update_status_pos()

    def _set_partial(self, text):
        self.canvas.itemconfig(self._partial_text, text=text)

    def _start_listening(self):
        if self.is_listening:
            return
        if self._is_processing:
            print("[!] Transcrição em andamento. Aguarde.", flush=True)
            self._set_status("Aguarde...")
            self._set_partial("Transcrevendo áudio anterior...")
            return

        self.is_listening = True
        self.stop_event.clear()
        self.force_stop_audio = False
        self._mic_active = True

        self._start_anim()
        self._set_status("Ouvindo...")
        self._update_status_pos()

        self._listen_thread = threading.Thread(
            target=self._recognition_loop, daemon=True
        )
        self._listen_thread.start()

    def _stop_listening(self):
        if not self.is_listening:
            return
        self.is_listening = False
        self._mic_active = False
        self.stop_event.set()   # <-- isso interrompe o loop de gravação
        if self._stop_listening_fn:
            self._stop_listening_fn(wait_for_stop=False)
            self._stop_listening_fn = None
        self._stop_anim()
        self._set_partial("")

    # ── Recognition Loop ─────────────────────────────────────────────────

    def _recognition_loop(self):
        import time
        mic_index = self.settings.get("microphone_index")
        mic_name = self.settings.get("microphone_name", "")
        language = self.settings.get("language", "pt-BR")

        # Resolve o índice do microfone pelo nome, se possível
        def resolve_index():
            if not mic_name:
                return mic_index
            try:
                names = sr.Microphone.list_microphone_names()
                for i, name in enumerate(names):
                    if name == mic_name:
                        print(f"[OK] Mic '{mic_name}' encontrado no index {i}", flush=True)
                        return i
            except Exception:
                pass
            return mic_index

        dev_idx = resolve_index()
        print(f"[DEBUG] Usando dispositivo de entrada: {dev_idx}", flush=True)

        # Abre o microfone com PyAudio
        p = pyaudio.PyAudio()
        stream = None
        try:
            stream = p.open(format=pyaudio.paInt16,
                            channels=1,
                            rate=SAMPLE_RATE,
                            input=True,
                            input_device_index=dev_idx if dev_idx is not None else None,
                            frames_per_buffer=CHUNK_SIZE)
        except Exception as e:
            print(f"[✗] Erro ao abrir microfone: {e}", flush=True)
            self.root.after(0, self._set_status, "Erro no mic")
            self.root.after(0, self._stop_listening)
            return

        frames = []
        gen = self._status_gen
        self.root.after(0, self._set_status, "Ouvindo...", gen)
        self.root.after(0, self._set_partial, "")
        print("[DEBUG] Gravando (aguardando segundo clique)...", flush=True)

        # Loop de gravação – SEM LIMITE DE TEMPO
        while not self.stop_event.is_set():
            try:
                data = stream.read(CHUNK_SIZE, exception_on_overflow=False)
                if data:
                    frames.append(data)
                    # Calcula RMS para animação e diagnóstico
                    rms = get_rms(data, 2)
                    self._audio_level = min(1.0, rms / 8000.0)
                    # Mostra nível a cada ~1 segundo (opcional, para debug)
                    if len(frames) % 16 == 0:  # 16 chunks ~ 1 segundo
                        print(f"  [DEBUG] RMS: {rms:.0f}  Level: {self._audio_level:.2f}", end='\r', flush=True)
            except Exception as e:
                print(f"[!] Erro leitura: {e}", flush=True)
                break

        # Fecha stream
        try:
            stream.stop_stream()
            stream.close()
        except:
            pass
        try:
            p.terminate()
        except:
            pass

        if not frames:
            print("\n[!] Nenhum frame capturado.", flush=True)
            self.root.after(0, self._set_status, "Pausado", gen)
            self.root.after(0, self._set_partial, "Sem áudio")
            return

        audio_data = b"".join(frames)
        audio_duration = len(audio_data) / (SAMPLE_RATE * 2)
        print(f"\n[DEBUG] Áudio gravado: {audio_duration:.2f} segundos", flush=True)

        # NÃO CORTA O ÁUDIO – mantém tudo o que foi falado
        # (A API pode rejeitar se for muito longo, mas você prefere arriscar)

        self._is_processing = True
        self.root.after(0, self._set_status, "Transcrevendo...", gen)
        self.root.after(0, self._set_partial, "⏳ ...")

        text = ""
        try:
            audio_obj = sr.AudioData(audio_data, SAMPLE_RATE, 2)
            engine = self.settings.get("engine", "Google (Gratuito)")
            if engine == "Groq (Whisper API)" and self.settings.get("groq_api_key"):
                from groq import Groq
                client = Groq(api_key=self.settings.get("groq_api_key").strip())
                wav_bytes = audio_obj.get_wav_data()
                whisper_lang = "pt" if language.startswith("pt") else language.split("-")[0]
                transcription = client.audio.transcriptions.create(
                    file=("audio.wav", wav_bytes),
                    model="whisper-large-v3-turbo",
                    language=whisper_lang,
                )
                text = transcription.text.strip()
            else:
                text = self.recognizer.recognize_google(audio_obj, language=language, show_all=False)
        except sr.UnknownValueError:
            print("[DEBUG] Áudio não compreendido", flush=True)
        except sr.RequestError as e:
            print(f"[✗] Erro API Google: {e}", flush=True)
        except Exception as e:
            print(f"[✗] Erro transcrição: {e}", flush=True)

        if text:
            import re
            text = text.strip()
            lower_text = text.lower()
            
            # Lista de alucinações curtas (ignorar apenas se for exatamente igual)
            hallucinations_exact = [
                "obrigado", "obrigada", "obrigado.", "obrigada.",
                "amara.org", "sônia ruberti", "legendas:", "legenda por",
                "tradução e sincronização", "e aí", "e ai", "ei", "alô", "olá", "tchau"
            ]
            
            # Só bloqueia se texto for muito curto (<=2) OU estiver exatamente na lista de alucinações
            is_hallucination = (len(text) <= 2) or (lower_text in hallucinations_exact)
            
            if is_hallucination:
                print(f"[!] Alucinação ignorada: '{text}'", flush=True)
                text = ""
            else:
                # Remove repetições de palavras consecutivas
                words = text.split()
                deduped = []
                for w in words:
                    if not deduped or w.lower() != deduped[-1].lower():
                        deduped.append(w)
                text = " ".join(deduped)
                
                if self.settings.get("auto_punctuation", True):
                    text = auto_punctuate(text)
                if is_capslock_on():
                    text = text.upper()
                print(f'  >> "{text}"', flush=True)
                self.history.append(text)
                if len(self.history) > 5:
                    self.history.pop(0)
                self._type_text(text)
        else:
            print("[✗] Nenhum texto retornado pela API.", flush=True)
            self.root.after(0, self._set_partial, "Não compreendido")
            time.sleep(1.5)

        self._is_processing = False
        self._audio_level = 0.0
        # Após transcrição, reseta o estado
        self.is_listening = False
        self._mic_active = False
        self._stop_anim()
        if self.is_visible:
            self.root.after(0, self._set_status, "Pausado", gen)
            self.root.after(0, self._set_partial, "Clique 🎙 para retomar")

    # ── Text injection (pynput - no clipboard) ───────────────────────────

    def _type_text(self, text):
        """Inject text using pynput. Never touches the clipboard."""
        if not text or not text.strip():
            return
        try:
            text_to_type = text + " "

            # Restore focus to the target window before typing
            if IS_WINDOWS and self._target_hwnd:
                try:
                    user32 = ctypes.windll.user32

                    # Attach our thread to target thread to allow focus change
                    our_thread = ctypes.windll.kernel32.GetCurrentThreadId()
                    target_thread = user32.GetWindowThreadProcessId(
                        self._target_hwnd, None
                    )
                    if our_thread != target_thread:
                        user32.AttachThreadInput(our_thread, target_thread, True)

                    user32.SetForegroundWindow(self._target_hwnd)
                    time.sleep(0.15)

                    if our_thread != target_thread:
                        user32.AttachThreadInput(our_thread, target_thread, False)

                except Exception as e:
                    print(f"[!] Erro ao restaurar foco: {e}", flush=True)

            from pynput.keyboard import Controller as KeyController, Key
            kb = KeyController()

            # Explicitly release modifier keys to prevent stuck/buggy key states in Windows
            if IS_WINDOWS:
                for key in [Key.alt, Key.alt_l, Key.alt_r, Key.ctrl, Key.ctrl_l, Key.ctrl_r, Key.shift, Key.shift_l, Key.shift_r, Key.cmd, Key.cmd_l, Key.cmd_r]:
                    try:
                        kb.release(key)
                    except Exception:
                        pass
                time.sleep(0.05) # Allow OS to register modifiers release

            kb.type(text_to_type)
            print(f"[OK] Texto digitado via pynput ({len(text)} chars)", flush=True)

        except Exception as e:
            print(f"[!] Erro ao injetar texto: {e}", flush=True)

    # ── Run ──────────────────────────────────────────────────────────────

    def run(self):
        self.root.mainloop()

    def quit(self):
        self._stop_listening()
        self.root.quit()
        self.root.destroy()


# ─── Entry Point ─────────────────────────────────────────────────────────────

def main():
    signal.signal(signal.SIGINT, signal.SIG_DFL)

    lock_ref = prevent_duplicate()

    try:
        app = VoiceDictationApp()
    except Exception as e:
        log.critical(f"Falha ao inicializar: {e}", exc_info=True)
        sys.exit(1)

    if not IS_WINDOWS:
        def term(s, f):
            app.quit()
        signal.signal(signal.SIGTERM, term)

    hotkey = app.settings.get("hotkey", "Alt+H")
    lang = app.settings.get("language", "pt-BR")
    mic_name = app.settings.get("microphone_name", "padrão")

    print(f"[OK] Voice Dictation ativo! Pressione {hotkey} para ditar.", flush=True)
    engine = app.settings.get("engine", "Google (Gratuito)")
    print(f"     Usando: {engine}", flush=True)
    print(f"     Idioma: {lang}", flush=True)
    print(f"     Mic: {mic_name or 'padrao do sistema'}", flush=True)
    print(f"     Log: {_LOG_FILE}", flush=True)

    try:
        app.run()
    except Exception as e:
        log.critical(f"Crash no mainloop: {e}", exc_info=True)


if __name__ == "__main__":
    main()
