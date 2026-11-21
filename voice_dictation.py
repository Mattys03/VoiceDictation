#!/usr/bin/env python3
"""
Voice Dictation Tool – Windows Floating Popup
Replicates Windows Win+H voice typing experience.
Configurable hotkey and microphone via Settings panel.
Uses Google's free Speech Recognition API for real-time transcription.
"""

import atexit
import gc
import logging
from logging.handlers import RotatingFileHandler
import math
import os
import platform
import re
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
from ctypes import wintypes

if platform.system() == "Windows":
    _kernel32 = ctypes.windll.kernel32
    _user32 = ctypes.windll.user32

    _kernel32.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
    _kernel32.GlobalAlloc.restype = ctypes.c_void_p
    _kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
    _kernel32.GlobalLock.restype = ctypes.c_void_p
    _kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    _kernel32.GlobalUnlock.restype = wintypes.BOOL

    _user32.OpenClipboard.argtypes = [wintypes.HWND]
    _user32.OpenClipboard.restype = wintypes.BOOL
    _user32.CloseClipboard.argtypes = []
    _user32.CloseClipboard.restype = wintypes.BOOL
    _user32.EmptyClipboard.argtypes = []
    _user32.EmptyClipboard.restype = wintypes.BOOL
    _user32.IsClipboardFormatAvailable.argtypes = [wintypes.UINT]
    _user32.IsClipboardFormatAvailable.restype = wintypes.BOOL
    _user32.GetClipboardData.argtypes = [wintypes.UINT]
    _user32.GetClipboardData.restype = ctypes.c_void_p
    _user32.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]
    _user32.SetClipboardData.restype = ctypes.c_void_p
    _user32.RegisterClipboardFormatW.argtypes = [wintypes.LPCWSTR]
    _user32.RegisterClipboardFormatW.restype = wintypes.UINT

import pyaudio
import speech_recognition as sr

from settings_manager import (
    load_settings,
    save_settings,
    device_name_matches,
    host_api_rank,
    is_loopback_device,
)
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
        try:
            samples = memoryview(buffer).cast("h")[::8]
            if not samples:
                return 0
            sum_squares = sum(s * s for s in samples)
            return int(math.sqrt(sum_squares / len(samples)))
        except Exception:
            return 0
    elif sample_width == 1:
        try:
            samples = memoryview(buffer)[::8]
            if not samples:
                return 0
            sum_squares = sum((s - 128) * (s - 128) for s in samples)
            return int(math.sqrt(sum_squares / len(samples)))
        except Exception:
            return 0
    return 0


def analyze_speech(rms_values):
    """Decide whether a recording actually contains speech.

    Returns (has_speech, peak, floor, threshold, chunks_above_threshold).

    The noise floor is taken from the quietest 20% of the WHOLE recording, not
    from the first chunks. That detail is what makes this reliable: when the
    user starts talking immediately the first chunks ARE speech, so a
    "beginning of recording" floor is really the voice level and any threshold
    derived from it becomes unreachable - the exact bug that made the app
    report "no speech detected" after half an hour of talking.

    The rule is deliberately biased towards transcribing: a silent clip that
    wrongly goes to Whisper costs nothing (Whisper returns no text and the
    existing hallucination filter drops it), while a real dictation wrongly
    discarded costs the whole message.
    """
    if not rms_values:
        return False, 0, 0, SPEECH_THRESHOLD_RMS, 0

    peak = max(rms_values)
    ordered = sorted(rms_values)
    pos = max(0, int(len(ordered) * NOISE_FLOOR_PERCENTILE) - 1)
    floor = ordered[pos]

    if peak < SPEECH_ABS_MIN_RMS:
        # Digital silence / muted input: nothing was said (or nothing arrived).
        return False, peak, floor, SPEECH_ABS_MIN_RMS, 0

    threshold = max(SPEECH_ABS_MIN_RMS,
                    min(int(floor * SPEECH_NOISE_RATIO), SPEECH_THRESHOLD_MAX))
    chunks = sum(1 for value in rms_values if value >= threshold)

    louder_than_room = peak >= floor * SPEECH_NOISE_RATIO
    clear_step = (peak - floor) >= SPEECH_MIN_STEP_RMS
    return bool(louder_than_room or clear_step), peak, floor, threshold, chunks


# ─── File Logging (essential for pythonw.exe) ────────────────────────────────

_LOG_DIR = os.path.dirname(os.path.abspath(__file__))
_LOG_FILE = os.path.join(_LOG_DIR, "voice_dictation.log")

class _SafeRotatingFileHandler(RotatingFileHandler):
    """Rotate logs without ever writing errors to sys.stderr.

    sys.stderr is redirected to _DualHandler, which logs again. If rotation
    fails (for example, because another process has the log open), logging an
    error to stderr would recurse until the stack overflows.
    """

    def handleError(self, record):
        pass


logging.raiseExceptions = False
_log_handler = _SafeRotatingFileHandler(
    _LOG_FILE,
    maxBytes=2 * 1024 * 1024,
    backupCount=3,
    encoding="utf-8",
)
_log_handler.setFormatter(logging.Formatter(
    "%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
))
log = logging.getLogger("VoiceDictation")
log.setLevel(logging.INFO)
log.addHandler(_log_handler)
log.propagate = False


# ─── Crash / exit diagnostics ────────────────────────────────────────────────
# The app runs under pythonw.exe, where a traceback would go to a nonexistent
# console. Without these hooks an unhandled error (or a plain exit) left no
# trace at all, so "Alt+H stopped working" was impossible to explain.

def _log_unhandled(exc_type, exc_value, exc_tb):
    if issubclass(exc_type, KeyboardInterrupt):
        return
    log.critical("Excecao nao tratada - o programa vai encerrar",
                 exc_info=(exc_type, exc_value, exc_tb))


def _log_thread_unhandled(args):
    log.error("Excecao nao tratada na thread %s (essa parte do programa parou de funcionar)",
              getattr(args.thread, "name", "?"),
              exc_info=(args.exc_type, args.exc_value, args.exc_traceback))


sys.excepthook = _log_unhandled
threading.excepthook = _log_thread_unhandled


@atexit.register
def _log_exit():
    # A missing "encerrando" line means the process was killed (Task Manager,
    # crash) instead of exiting by itself.
    log.info("Voice Dictation encerrando (pid=%s)", os.getpid())

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

# Speech detection. The old code estimated the "room noise floor" from the
# FIRST chunks of the recording and required speech to be 2x that value. When
# the user starts talking as soon as the recording opens (the normal case) the
# "noise floor" becomes their own voice, so the threshold becomes twice their
# own voice and the whole recording is thrown away as "no speech detected".
# The decision is now made from the WHOLE recording: in a real session the
# quiet part is the room, not the speech.
SPEECH_THRESHOLD_RMS = 120      # compatibility / very quiet microphones
SPEECH_ABS_MIN_RMS = 300        # hiss/thermal noise below this is never speech
SPEECH_NOISE_RATIO = 1.6        # speech must stand this much above the room floor
SPEECH_MIN_STEP_RMS = 1200      # ...or this much in absolute RMS
SPEECH_THRESHOLD_MAX = 4000     # never demand more than this (boosted inputs)
NOISE_FLOOR_PERCENTILE = 0.20   # quietest 20% of the session = room noise

# Hard cap on a single recording so a misbehaving driver cannot eat all RAM.
MAX_RECORD_SECONDS = 15 * 60

# How many times a session may rebuild the audio engine and reopen the
# microphone after a read failure before giving up. Device re-enumeration
# (USB unplug, drive dropping off the bus, driver reload) kills the open stream
# with a read error; retrying is what turns "microfone bugado" into a hiccup.
MAX_MIC_RECOVERIES = 3

# How often the background watcher compares OS device count against PortAudio.
DEVICE_WATCH_INTERVAL_MS = 4000

# Adaptive foreground polling intervals (150ms when popup/listening active, 300ms idle).
TARGET_POLL_ACTIVE_MS = 150
TARGET_POLL_IDLE_MS = 300

# Network timeouts so stalled internet / API calls never wedge the worker thread.
GROQ_REQUEST_TIMEOUT = 45.0
GOOGLE_OPERATION_TIMEOUT = 15

IS_WINDOWS = platform.system() == "Windows"
IS_LINUX = platform.system() == "Linux"


class GUITHREADINFO(ctypes.Structure):
    """Win32 GUITHREADINFO.

    Used to find out which CONTROL (text box, chat input...) currently has
    keyboard focus inside a window, which a top-level handle alone cannot tell.
    """

    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("flags", wintypes.DWORD),
        ("hwndActive", wintypes.HWND),
        ("hwndFocus", wintypes.HWND),
        ("hwndCapture", wintypes.HWND),
        ("hwndMenuOwner", wintypes.HWND),
        ("hwndMoveSize", wintypes.HWND),
        ("hwndCaret", wintypes.HWND),
        ("rcCaret", wintypes.RECT),
    ]

# ─── Hotkey parsing ──────────────────────────────────────────────────────────

HOTKEY_MAP = None


def _get_hotkey_map():
    global HOTKEY_MAP
    if HOTKEY_MAP is None:
        from pynput import keyboard as _kb
        HOTKEY_MAP = {
            "Alt": {_kb.Key.alt_l, _kb.Key.alt_r},
            "Ctrl": {_kb.Key.ctrl_l, _kb.Key.ctrl_r},
            "Shift": {_kb.Key.shift_l, _kb.Key.shift_r},
            "Win": {_kb.Key.cmd, _kb.Key.cmd_l, _kb.Key.cmd_r},
        }
    return HOTKEY_MAP


def parse_hotkey(hotkey_str):
    """Parse hotkey string like 'Ctrl+Shift+D' into (modifier_sets, char)."""
    hmap = _get_hotkey_map()
    parts = hotkey_str.split("+")
    char = parts[-1].lower()
    mods = []
    for p in parts[:-1]:
        p_stripped = p.strip()
        if p_stripped in hmap:
            mods.append(hmap[p_stripped])
    return mods, char


# ─── Duplicate prevention ───────────────────────────────────────────────────

def prevent_duplicate():
    is_silent = ("--silent" in sys.argv or "--background" in sys.argv)
    if IS_WINDOWS:
        try:
            import win32event, win32api
            h_event = win32event.OpenEvent(win32event.EVENT_MODIFY_STATE, False, "VoiceDictation_Toggle_Event")
            if h_event:
                if not is_silent:
                    win32event.SetEvent(h_event)
                    print("[OK] Instância existente notificada para alternar (toggle). Saindo.", flush=True)
                else:
                    print("[OK] Instância existente já em execução (--silent). Saindo.", flush=True)
                win32api.CloseHandle(h_event)
                sys.exit(0)
        except Exception:
            pass

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
def whisper_language_code(language):
    """Return the ISO-639-1 code for Groq Whisper, or None for auto-detect."""
    if language in ("pt", "pt-BR", "translate_en", "translate_es"):
        return "pt"
    if language in ("en", "en-US"):
        return "en"
    if language in ("es", "es-ES"):
        return "es"
    return None


def build_whisper_kwargs(language, wav_payload):
    """Build a minimal Whisper request.

    Deliberately no `prompt`: Whisper treats the prompt as previous transcript
    text. On silence/noise it can continue that fake transcript and invent
    subtitle credits such as "Legenda Adriana Zanotto". Forcing the language is
    enough and is much safer.
    """
    kwargs = {
        "file": ("audio.wav", wav_payload),
        "model": "whisper-large-v3-turbo",
        "temperature": 0.0,
    }
    lang_code = whisper_language_code(language)
    if lang_code:
        kwargs["language"] = lang_code
    return kwargs


def trim_silent_edges(frames, rms_values, threshold,
                      pre_chunks=0, post_chunks=0):
    """Trim only the silent head/tail before sending audio to Whisper.

    Whisper hallucination is strongly correlated with trailing silence and
    low-level noise. The middle of the recording is preserved so pauses and
    natural speech rhythm are not altered.
    """
    if not frames or not rms_values:
        return b"", 0, 0
    count = min(len(frames), len(rms_values))
    active = [i for i in range(count) if rms_values[i] >= threshold]
    if not active:
        return b"", 0, 0
    start = max(0, active[0] - max(0, int(pre_chunks)))
    end = min(count, active[-1] + max(0, int(post_chunks)) + 1)
    return b"".join(frames[start:end]), start, end


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
        self._worker_active = False
        self._record_started_at = None
        self._timer_job = None
        self._listen_thread = None
        self._stop_listening_fn = None
        self._drag_data = {"x": 0, "y": 0}
        self._anim_id = None
        self._target_hwnd = None  # Window handle where text will be pasted
        self._target_focus = None  # Control inside that window that had keyboard focus
        self._target_label = ""   # Human readable target, for the log
        self._target_locked = False  # Frozen while a dictation is in progress
        self._start_requested = False  # Hotkey pressed while the previous one was transcribing
        self._positioned = False  # Only calculate position once
        self._session_id = 0  # Incremented per listening session
        self._mic_problem = None  # Why the microphone is not usable, if so
        self._closing = False
        self._own_pid = os.getpid()
        self._gui_info = GUITHREADINFO()
        self._gui_info.cbSize = ctypes.sizeof(GUITHREADINFO)
        self._no_activate_hwnds = set()
        self._last_timer_sec = None
        self._status_mapped = False
        self._last_status_pos = None
        self._last_anim_state = None
        self._retry_start_count = 0
        self._hotkey_gen = 0
        self._hotkey_stop = None
        self._pynput_listener = None
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
        self.recognizer.operation_timeout = GOOGLE_OPERATION_TIMEOUT
        self._status_gen = 0  # Generation counter for status updates
        self._groq_client = None
        self._last_groq_key = None
        self._groq_lock = threading.Lock()
        self._history_lock = threading.Lock()

        # Persistent audio subsystem & prewarm cache
        self._pa = None
        self._pa_lock = threading.RLock()
        self._resolve_lock = threading.Lock()
        self._dev_count = None        # OS capture-device count the engine was built with
        self._streams_open = 0        # live capture streams owned by this app
        self._pa_refresh_pending = False
        self._cached_dev_idx = None
        self._cached_rate = None
        self._prewarm_running = False

        self._build_ui()
        self._setup_hotkey()

        # Settings dialog
        self.settings_dialog = SettingsDialog(
            self.root, on_save_callback=self._on_settings_saved
        )

        # Pre-warm audio and Groq connection asynchronously in background
        self._prewarm_async()

        if IS_WINDOWS:
            self._start_ipc_listener()

        # Watch for audio devices being added/removed so the engine is rebuilt
        # and the microphone re-resolved by itself (see _watch_audio_devices).
        try:
            self.root.after(DEVICE_WATCH_INTERVAL_MS, self._watch_audio_devices)
        except Exception:
            pass

    def _ui_post(self, callback, *args):
        """Post a UI callback safely onto the Tk main thread."""
        if getattr(self, "_closing", False):
            return
        root = getattr(self, "root", None)
        if root is None:
            return
        try:
            root.after(0, callback, *args)
        except Exception:
            pass

    def _on_tk_exception(self, exc_type, exc_value, exc_tb):
        log.error("Excecao no loop Tk", exc_info=(exc_type, exc_value, exc_tb))

    def _start_ipc_listener(self):
        """Listen for IPC signal from desktop shortcut or batch file to toggle visibility."""
        def _ipc_thread():
            h_event = None
            try:
                import win32event, win32api
                h_event = win32event.CreateEvent(None, False, False, "VoiceDictation_Toggle_Event")
                while not getattr(self, "_closing", False):
                    res = win32event.WaitForSingleObject(h_event, 1000)
                    if getattr(self, "_closing", False):
                        break
                    if res == win32event.WAIT_OBJECT_0:
                        self._ui_post(self.toggle)
            except Exception as e:
                print(f"[!] Erro no listener IPC: {e}", flush=True)
            finally:
                if h_event:
                    try:
                        import win32api
                        win32api.CloseHandle(h_event)
                    except Exception:
                        pass

        t = threading.Thread(target=_ipc_thread, daemon=True)
        t.start()

    # ── UI Construction ──────────────────────────────────────────────────

    def _build_ui(self):
        self.root = tk.Tk()
        self.root.withdraw()
        self.root.report_callback_exception = self._on_tk_exception
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

        # (Text labels underneath mic removed per user request)
        self._status_text = None
        self._partial_text = None

        # Quick Language Switcher Button (Bottom Right)
        self._lang_pill = self._draw_rounded_rect(
            self.canvas, self.WIDTH - 86, 105, self.WIDTH - 8, 125, 8,
            fill="#252840", outline="#3a3d55", tags="lang_btn"
        )
        self._lang_btn = self.canvas.create_text(
            self.WIDTH - 47, 115, text="🌐 AUTO",
            fill="#a4a7bf", font=("Segoe UI", 8, "bold"), tags="lang_btn"
        )
        self.canvas.tag_bind("lang_btn", "<Button-1>", self._on_lang_badge_click)
        self.canvas.tag_bind(
            "lang_btn", "<Enter>",
            lambda e: (self.canvas.itemconfig(self._lang_btn, fill="#4dd964"),
                       self.canvas.itemconfig(self._lang_pill, outline="#4dd964")))
        self.canvas.tag_bind(
            "lang_btn", "<Leave>",
            lambda e: (self.canvas.itemconfig(self._lang_btn, fill="#a4a7bf"),
                       self.canvas.itemconfig(self._lang_pill, outline="#3a3d55")))
        self._update_lang_badge()

        # Drag bindings
        self.canvas.tag_bind("handle", "<Button-1>", self._drag_start)
        self.canvas.tag_bind("handle", "<B1-Motion>", self._drag_move)
        self.canvas.bind("<Button-1>", self._bg_click)
        self.canvas.bind("<B1-Motion>", self._drag_move)
        self.canvas.bind("<ButtonRelease-1>", self._drag_stop)

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        # Floating status window
        self.STATUS_W = 190
        self.STATUS_H = 38
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
            self.status_win, width=self.STATUS_W, height=self.STATUS_H,
            bg=sw_bg, highlightthickness=0, bd=0
        )
        sw_canvas.pack()

        self._draw_rounded_rect(
            sw_canvas, 2, 2, self.STATUS_W - 2, self.STATUS_H - 2, 11,
            fill="#1e2135", outline="#3a3d55"
        )

        self._sw_label = sw_canvas.create_text(
            self.STATUS_W // 2, self.STATUS_H // 2, text="Ouvindo...",
            fill="#e0e2f0", font=("Segoe UI", 11, "bold")
        )
        self.sw_canvas = sw_canvas

        if IS_WINDOWS:
            self._apply_no_activate(self.root)
            self._apply_no_activate(self.status_win)
            self._poll_target_window()

    def _apply_no_activate(self, window):
        """Ensure the popup never steals focus when shown or clicked.

        Tk wraps every toplevel: winfo_id() is a CHILD of the real top-level
        window, and WS_EX_NOACTIVATE only has an effect on the real one, so
        applying it to winfo_id() alone left the popup activatable. Clicking the
        mic button then pulled the foreground away from the app the user was
        dictating into.
        """
        if not IS_WINDOWS:
            return
        try:
            w = window.winfo_id()
            cache = getattr(self, "_no_activate_hwnds", None)
            cache_key = (id(window), w)
            if cache is not None and cache_key in cache:
                return

            window.update_idletasks()
            user32 = _user32
            GWL_EXSTYLE = -20
            WS_EX_NOACTIVATE = 0x08000000
            GA_ROOT = 2

            w = window.winfo_id()
            handles = {w, user32.GetAncestor(w, GA_ROOT), user32.GetParent(w)}
            for hwnd in handles:
                if not hwnd:
                    continue
                cur_style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
                if not (cur_style & WS_EX_NOACTIVATE):
                    user32.SetWindowLongW(hwnd, GWL_EXSTYLE, cur_style | WS_EX_NOACTIVATE)
            if cache is not None:
                cache.add((id(window), w))
        except Exception as e:
            print(f"[!] Erro ao aplicar WS_EX_NOACTIVATE: {e}", flush=True)

    def _get_groq_client(self, api_key):
        """Get or reuse persistent Groq client for faster requests with connection pooling."""
        if not api_key:
            return None
        lock = getattr(self, "_groq_lock", None)
        if lock is not None:
            lock.acquire()
        try:
            if self._groq_client is None or self._last_groq_key != api_key:
                try:
                    from groq import Groq
                    self._groq_client = Groq(
                        api_key=api_key,
                        timeout=GROQ_REQUEST_TIMEOUT,
                        max_retries=1,
                    )
                    self._last_groq_key = api_key
                except Exception as e:
                    print(f"[!] Erro ao instanciar Groq: {e}", flush=True)
                    return None
            return self._groq_client
        finally:
            if lock is not None:
                lock.release()

    def _translate_text(self, client, text, target_language_name):
        """Translate transcribed text into target language with high quality and zero latency."""
        if not text or not client:
            return text
        models_to_try = ["openai/gpt-oss-20b", "groq/compound-mini"]
        for model_name in models_to_try:
            try:
                res = client.chat.completions.create(
                    model=model_name,
                    messages=[
                        {
                            "role": "system",
                            "content": f"Translate the user text directly to natural, fluent {target_language_name}. If already in {target_language_name}, keep it as is. Output ONLY the translated text, without quotation marks, notes, or explanations."
                        },
                        {"role": "user", "content": text}
                    ],
                    temperature=0.0
                )
                translated = res.choices[0].message.content.strip()
                if translated:
                    return translated
            except Exception as e:
                print(f"[!] Erro tradução ({model_name}): {e}", flush=True)
        return text

    def _prewarm_async(self):
        """Refresh the microphone/Groq caches in the background, never twice at
        the same time (settings are saved on every keystroke, so this used to
        spawn a flock of threads that fought over the audio device)."""
        if self._prewarm_running:
            return
        self._prewarm_running = True

        def _worker():
            try:
                self._prewarm_all()
            finally:
                self._prewarm_running = False

        threading.Thread(target=_worker, daemon=True).start()

    def _prewarm_all(self):
        """Warm up the audio engine and the Groq client sequentially."""
        self._prewarm_audio()
        self._prewarm_groq()

    def _os_input_device_count(self):
        """Cheap, live count of capture devices as Windows currently sees them.

        PortAudio reports the device table it was built with, so asking the live
        PyAudio instance whether anything changed can never work: it always
        answers with the old list. The MME waveform counter, on the other hand,
        is refreshed by the OS as soon as a device is plugged or unplugged and
        costs microseconds to read, so it is the signal that decides when the
        engine has to be rebuilt.
        """
        if not IS_WINDOWS:
            return None
        try:
            return int(ctypes.windll.winmm.waveInGetNumDevs())
        except Exception:
            return None

    def _refresh_pa_locked(self, force=False):
        """Rebuild the PortAudio engine when the device list changed.

        Must be called with self._pa_lock held. PortAudio is never torn down
        while one of our streams is open (that would kill the reader thread), so
        in that case the rebuild is deferred until the stream is closed.
        """
        if self._pa is None:
            self._pa = pyaudio.PyAudio()
            self._dev_count = self._os_input_device_count()
            return self._pa

        if self._streams_open:
            if force:
                self._pa_refresh_pending = True
            return self._pa

        if not force:
            current = self._os_input_device_count()
            if current is None or current == self._dev_count:
                return self._pa
            print(f"[OK] Dispositivos de audio mudaram ({self._dev_count} -> {current}); "
                  f"recriando o motor de audio.", flush=True)

        try:
            self._pa.terminate()
        except Exception:
            pass
        self._pa = pyaudio.PyAudio()
        self._dev_count = self._os_input_device_count()
        # Indices are not stable across re-enumerations: whatever was cached
        # before may now point at a different physical device.
        self._cached_dev_idx = None
        self._cached_rate = None
        self._pa_refresh_pending = False
        return self._pa

    def _get_pa(self):
        """Return the shared PyAudio instance, rebuilding it after device changes."""
        with self._pa_lock:
            return self._refresh_pa_locked()

    def _reset_audio_engine(self):
        """Force a full PortAudio restart (used after a failed open/read)."""
        with self._pa_lock:
            self._refresh_pa_locked(force=True)

    def _open_input_stream(self, dev_idx, rate):
        """Open a capture stream, keeping count of the streams that are alive."""
        with self._pa_lock:
            pa = self._refresh_pa_locked()
            stream = pa.open(
                format=pyaudio.paInt16, channels=1, rate=rate, input=True,
                input_device_index=dev_idx, frames_per_buffer=CHUNK_SIZE
            )
            self._streams_open += 1
            return stream

    def _close_input_stream(self, stream):
        """Close a capture stream, applying any deferred engine rebuild."""
        if stream is None:
            return
        try:
            stream.stop_stream()
        except Exception:
            pass
        try:
            stream.close()
        except Exception:
            pass
        with self._pa_lock:
            if self._streams_open > 0:
                self._streams_open -= 1
            if self._streams_open == 0 and self._pa_refresh_pending:
                self._refresh_pa_locked(force=True)

    def _input_devices(self, pa):
        """Return {index: (info, host_api_name)} for every input device."""
        devices = {}
        for i in range(pa.get_device_count()):
            try:
                info = pa.get_device_info_by_index(i)
            except Exception:
                continue
            if info.get("maxInputChannels", 0) <= 0:
                continue
            try:
                api = pa.get_host_api_info_by_index(info["hostApi"]).get("name", "")
            except Exception:
                api = ""
            devices[i] = (info, api)
        return devices

    @staticmethod
    def _rates_to_try(native_rate):
        rates = [native_rate, 48000, 44100, 16000]
        seen = set()
        return [r for r in rates if r and not (r in seen or seen.add(r))]

    def _probe_input_device(self, dev_idx, rate, chunks=8):
        """Open a device and check that it really streams audio in real time.

        Returns (peak_rms, opened, problem); problem is None when the device
        looks healthy. Some PortAudio/Windows backend combinations (the
        DirectSound backend with a fifine USB mic, for instance) hand back a
        frozen buffer immediately instead of blocking, so comparing the
        wall-clock time against the amount of audio requested is what tells a
        live device apart from a dead one.
        """
        stream = None
        try:
            stream = self._open_input_stream(dev_idx, rate)
        except Exception as e:
            return 0, False, f"nao pode ser aberto ({e})"

        try:
            started = time.perf_counter()
            peaks = []
            first_chunk = None
            identical = True
            for _ in range(chunks):
                data = stream.read(CHUNK_SIZE, exception_on_overflow=False)
                if first_chunk is None:
                    first_chunk = data
                elif data != first_chunk:
                    identical = False
                peaks.append(get_rms(data, 2))
            elapsed = time.perf_counter() - started
        except Exception as e:
            return 0, True, f"falha na leitura ({e})"
        finally:
            self._close_input_stream(stream)

        expected = chunks * CHUNK_SIZE / float(rate)
        if elapsed < expected * 0.5:
            return 0, True, (f"nao captura em tempo real ({elapsed:.3f}s para "
                             f"{expected:.3f}s de audio)")
        if identical and max(peaks or [0]) == 0:
            return 0, True, "entrega silencio digital constante"
        return (max(peaks) if peaks else 0), True, None

    def _resolve_input_device(self):
        """Return (device_index, sample_rate, note, healthy) for the best input.

        The saved microphone decides WHICH physical microphone to use, but the
        backend serving it is chosen by probing: the same microphone appears on
        MME, DirectSound, WASAPI and WDM-KS, and some of those entries (WDM-KS,
        which is exclusive) silently refuse to open. Identity is matched by NAME
        first because the index is not stable: re-enumerating hardware renumbers
        the entries and a saved index then points at a dead or wrong device.
        """
        resolve_lock = getattr(self, "_resolve_lock", None)
        if resolve_lock is not None:
            resolve_lock.acquire()
        try:
            with self._pa_lock:
                pa = self._get_pa()
                devices = self._input_devices(pa)
                if not devices:
                    return None, None, "nenhum dispositivo de entrada encontrado", False

                default_idx = None
                try:
                    default_idx = pa.get_default_input_device_info()["index"]
                except Exception:
                    pass

            mic_index = self.settings.get("microphone_index")
            mic_name = self.settings.get("microphone_name") or ""
            was_configured = bool(mic_name) or mic_index is not None

            by_name, by_index, others = [], [], []
            for i, (info, _api) in devices.items():
                name = info.get("name", "")
                if mic_name and device_name_matches(mic_name, name):
                    by_name.append(i)
                elif mic_index is not None and i == mic_index:
                    by_index.append(i)
                else:
                    others.append(i)

            # Backend reliability decides the order inside each group; the probe
            # is only a validity filter, since ranking by measured level would be
            # random (one loud chunk makes the peak jump 100x).
            by_name.sort(key=lambda i: host_api_rank(devices[i][1]))
            others.sort(key=lambda i: host_api_rank(devices[i][1]))

            if by_name or by_index:
                note_prefix = ""
            elif was_configured:
                # Configured mic vanished (unplugged / re-enumerated): fall back
                # to the system default, then to any input device.
                note_prefix = "mic configurado nao encontrado; "
                if default_idx in devices:
                    others = [default_idx] + [i for i in others if i != default_idx]
                    note_prefix += "usando o padrao do sistema; "
                else:
                    note_prefix += "usando o primeiro disponivel; "
            else:
                # Nothing configured: follow the system default.
                note_prefix = ""
                if default_idx in devices:
                    others = [default_idx] + [i for i in others if i != default_idx]

            ordered = by_name + by_index + [i for i in others
                                            if i not in by_name and i not in by_index]

            # Stop on the FIRST healthy device in priority order: never waste
            # seconds probing remaining backends/devices under _pa_lock.
            results = []  # (healthy, peak, index, rate, api, problem)
            found_healthy = False
            for dev_idx in ordered:
                info, api = devices[dev_idx]
                native = int(info.get("defaultSampleRate", 48000) or 48000)
                for rate in self._rates_to_try(native):
                    peak, opened, problem = self._probe_input_device(dev_idx, rate)
                    results.append((problem is None, peak, dev_idx, rate, api, problem))
                    if problem is None:
                        found_healthy = True
                        break
                    if not opened:
                        continue   # unsupported rate: try the next one
                    break          # opened: remaining failures are per-device
                if found_healthy:
                    break

            for healthy, peak, dev_idx, rate, api, _problem in results:
                if not healthy:
                    continue
                name = devices[dev_idx][0].get("name", "?")
                note = f"{name} [{api}] peak={peak} {note_prefix}".strip()
                print(f"[OK] Microfone selecionado: idx={dev_idx} @ {rate}Hz ({note})",
                      flush=True)
                if was_configured:
                    self._adopt_device(name)
                return dev_idx, rate, note, True

            # Nothing is healthy: keep the most plausible device but say why.
            realtime = [r for r in results if r[5] and "tempo real" not in r[5]]
            pool = realtime or results
            _ok, peak, dev_idx, rate, api, problem = pool[0]
            name = devices[dev_idx][0].get("name", "?")
            note = f"{name} [{api}]: {problem} {note_prefix}".strip()
            print(f"[✗] Nenhum microfone capturou audio corretamente. "
                  f"Tentando idx={dev_idx} @ {rate}Hz -> {note}", flush=True)
            return dev_idx, rate, note, False
        finally:
            if resolve_lock is not None:
                resolve_lock.release()

    def _watch_audio_devices(self):
        """Notice devices being plugged/unplugged without ever blocking the Tk loop.

        Reads the OS capture-device counter with zero locks (microseconds) and
        dispatches engine rebuild/re-resolve to the background prewarm worker if
        the count changed.
        """
        if getattr(self, "_closing", False):
            return
        try:
            cur = self._os_input_device_count()
            before = self._dev_count
            changed = cur is not None and before is not None and cur != before
            if changed and not self.is_listening and not self._is_processing:
                self._prewarm_async()
        except Exception as e:
            print(f"[DEBUG] Monitor de audio: {e}", flush=True)
        finally:
            if not getattr(self, "_closing", False):
                try:
                    self.root.after(DEVICE_WATCH_INTERVAL_MS, self._watch_audio_devices)
                except Exception:
                    pass

    def _adopt_device(self, name):
        """Persist the microphone that actually worked.

        Windows renumbers audio devices whenever hardware is plugged/unplugged,
        so the numeric index saved by the settings dialog ages badly and can end
        up pointing at a dead or wrong entry. Remembering the working NAME (and
        dropping the stale index) lets the next session - and the next reconnect
        - go straight to a microphone known to capture audio.
        """
        if not name:
            return
        configured = self.settings.get("microphone_name") or ""
        if configured and device_name_matches(configured, name):
            return
        try:
            self.settings["microphone_name"] = name
            self.settings["microphone_index"] = None
            save_settings(self.settings)
            print(f"[OK] Microfone corrigido automaticamente para: {name}", flush=True)
        except Exception as e:
            print(f"[DEBUG] Nao foi possivel salvar o microfone ajustado: {e}", flush=True)

    def _prewarm_audio(self):
        """Resolve the microphone that actually captures audio and cache it."""
        try:
            self._get_pa()
            dev_idx, rate, note, healthy = self._resolve_input_device()

            if dev_idx is None:
                self._cached_dev_idx = None
                self._cached_rate = None
                self._mic_problem = note
                print(f"[✗] Microfone indisponivel: {note}", flush=True)
                return

            if not healthy:
                # Never cache a device that failed the probe: the fast path would
                # reopen it on every dictation and fail every single time.
                self._cached_dev_idx = None
                self._cached_rate = None
                self._mic_problem = note
                print(f"[!] Microfone instavel, sera reavaliado em cada ditado: {note}",
                      flush=True)
                return

            self._cached_dev_idx = dev_idx
            self._cached_rate = rate
            self._mic_problem = None
            print(f"[OK] Audio engine pre-aquecido ({rate}Hz, dev_idx={dev_idx})",
                  flush=True)
        except Exception as e:
            print(f"[!] Erro pre-aquecimento audio: {e}", flush=True)

    def _prewarm_groq(self):
        """Pre-warm Groq library, API client, and HTTPS connection pool in background."""
        try:
            groq_key = (self.settings.get("groq_api_key") or "").strip()
            if groq_key:
                client = self._get_groq_client(groq_key)
                if client:
                    client.models.list()
                    print("[OK] Groq client e conexão TLS pré-aquecidos", flush=True)
        except Exception as e:
            print(f"[DEBUG] Pré-aquecimento Groq: {e}", flush=True)

    def _window_label(self, hwnd):
        """Title + class of a window, for logs and status messages."""
        try:
            user32 = _user32 if IS_WINDOWS else ctypes.windll.user32
            length = user32.GetWindowTextLengthW(hwnd)
            title = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(hwnd, title, length + 1)
            cls = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, cls, 256)
            text = title.value.strip() or "sem titulo"
            return f"{text} [{cls.value}]"
        except Exception:
            return f"0x{int(hwnd):X}"

    def _focused_control(self, hwnd):
        """Control that owns keyboard focus inside (or as) `hwnd`, if known."""
        try:
            user32 = _user32 if IS_WINDOWS else ctypes.windll.user32
            thread_id = user32.GetWindowThreadProcessId(hwnd, None)
            gui = getattr(self, "_gui_info", None)
            if gui is None:
                gui = GUITHREADINFO()
                gui.cbSize = ctypes.sizeof(GUITHREADINFO)
            else:
                gui.hwndFocus = None
            if user32.GetGUIThreadInfo(thread_id, ctypes.byref(gui)) and gui.hwndFocus:
                return int(gui.hwndFocus)
        except Exception:
            pass
        return None

    def _target_is_alive(self):
        if not (IS_WINDOWS and self._target_hwnd):
            return False
        try:
            return bool(_user32.IsWindow(self._target_hwnd))
        except Exception:
            return False

    def _capture_target_window(self, refresh_label=False):
        """Remember which window - and which field inside it - the user is using.

        The top-level handle alone is not enough: in browsers and chat apps the
        text field is a child control, and knowing which control had focus is
        what keeps the paste in the exact chat the user clicked instead of
        "somewhere in that application".
        """
        if not IS_WINDOWS:
            return
        try:
            user32 = _user32
            hwnd = user32.GetForegroundWindow()
            if not hwnd:
                return
            if hwnd == self._target_hwnd:
                # Same external window: refresh focus handle cheaply, skip
                # string/buffer allocations unless explicitly requested.
                self._target_focus = self._focused_control(hwnd)
                if refresh_label:
                    self._target_label = self._window_label(hwnd)
                return

            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            own_pid = getattr(self, "_own_pid", None) or os.getpid()
            if pid.value == own_pid or not user32.IsWindow(hwnd):
                # Our own popup/settings: keep the previous destination.
                return

            self._target_hwnd = hwnd
            self._target_focus = self._focused_control(hwnd)
            self._target_label = self._window_label(hwnd)
        except Exception:
            pass

    def _poll_target_window(self):
        """Track the window that should receive the transcribed text."""
        if getattr(self, "_closing", False):
            return
        self._capture_target_window(refresh_label=False)
        interval = (
            TARGET_POLL_ACTIVE_MS
            if (self.is_visible or self.is_listening or self._is_processing)
            else TARGET_POLL_IDLE_MS
        )
        try:
            self.root.after(interval, self._poll_target_window)
        except Exception:
            pass

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
        old_mic = (self.settings.get("microphone_name"), self.settings.get("microphone_index"))
        self.settings = new_settings
        new_key = (new_settings.get("groq_api_key") or "").strip()
        key_changed = new_key != (self._last_groq_key or "")
        if key_changed:
            with self._groq_lock:
                self._groq_client = None
                self._last_groq_key = None
        new_mic = (new_settings.get("microphone_name"), new_settings.get("microphone_index"))
        mic_changed = new_mic != old_mic
        if mic_changed:
            self._cached_dev_idx = None
            self._cached_rate = None
        self._update_lang_badge()

        new_hotkey = new_settings.get("hotkey")
        if new_hotkey != old_hotkey:
            self._setup_hotkey()

        if mic_changed or key_changed:
            self._prewarm_async()

        mic_name = new_settings.get("microphone_name") or "padrao"
        print(f"[OK] Novo atalho: {new_settings['hotkey']}", flush=True)
        print(f"[OK] Mic: {mic_name}", flush=True)
        if is_loopback_device(mic_name):
            print(f"[!] AVISO: '{mic_name}' e um dispositivo virtual/loopback; "
                  f"ele pode capturar o audio do PC em vez da sua voz.", flush=True)
            self._set_status("Mic virtual: captura audio do PC")
            self.root.after(
                2500,
                lambda: self._set_status("Pausado")
                if not self.is_listening and not self._is_processing else None,
            )

    # ── Mic Animation ────────────────────────────────────────────────────

    def _update_mic_visual(self):
        if getattr(self, "_closing", False):
            self._anim_id = None
            return
        if self._mic_active or self._is_processing:
            if self.is_visible:
                self._pulse_phase += 0.11
                if self._pulse_phase > 2 * math.pi:
                    self._pulse_phase -= 2 * math.pi

                alpha_factor = 0.3 + 0.2 * math.sin(self._pulse_phase)

                if self._is_processing:
                    color = "#d4a017"
                    glow_color = self._alpha_hex("#d4a017", alpha_factor)
                    glow_r = 34 + (0.5 + 0.3 * math.sin(self._pulse_phase * 1.5)) * 10
                else:
                    color = "#4dd964"
                    glow_color = self._alpha_hex("#4dd964", alpha_factor)
                    glow_r = 34 + self._audio_level * 18

                glow_r = round(glow_r, 1)
                state = (color, glow_color, glow_r)
                if state != getattr(self, "_last_anim_state", None):
                    self._last_anim_state = state
                    self.canvas.coords(
                        self._mic_glow,
                        self.mic_cx - glow_r, self.mic_cy - glow_r,
                        self.mic_cx + glow_r, self.mic_cy + glow_r
                    )
                    self.canvas.itemconfig(self._mic_glow, fill=glow_color, outline="")
                    self.canvas.itemconfig(self._mic_circle, fill=color)

                self._anim_id = self.root.after(45, self._update_mic_visual)
            else:
                self._anim_id = self.root.after(200, self._update_mic_visual)
        else:
            self._last_anim_state = None
            self.canvas.itemconfig(self._mic_glow, fill="", outline="")
            self.canvas.itemconfig(self._mic_circle, fill="#555870")
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
        if self._anim_id is None and not getattr(self, "_closing", False):
            self._pulse_phase = 0.0
            self._last_anim_state = None
            self._update_mic_visual()

    def _stop_anim(self):
        if self._anim_id is not None:
            try:
                self.root.after_cancel(self._anim_id)
            except Exception:
                pass
            self._anim_id = None
        self._audio_level = 0.0
        self._mic_active = False
        self._last_anim_state = None
        try:
            self.canvas.itemconfig(self._mic_glow, fill="", outline="")
            self.canvas.itemconfig(self._mic_circle, fill="#555870")
        except tk.TclError:
            pass

    # ── Drag ─────────────────────────────────────────────────────────────

    def _bg_click(self, event):
        tags = self.canvas.gettags("current")
        if "close" in tags or "mic_area" in tags or "gear" in tags or "help" in tags or "lang_btn" in tags:
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
        self._update_status_pos(reposition_only=True)
        
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

    def _update_status_pos(self, reposition_only=False):
        try:
            if not self.is_visible:
                if getattr(self, "_status_mapped", False):
                    self.status_win.withdraw()
                    self._status_mapped = False
                return
            if self.x is None or self.y is None:
                self.x = self.root.winfo_x()
                self.y = self.root.winfo_y()

            if not getattr(self, "_status_mapped", False):
                self.status_win.deiconify()
                if IS_WINDOWS:
                    self._apply_no_activate(self.status_win)
                    try:
                        sw_hwnd = self.status_win.winfo_id()
                        _user32.ShowWindow(sw_hwnd, 4)  # SW_SHOWNOACTIVATE
                        _user32.SetWindowPos(
                            sw_hwnd, -1, 0, 0, 0, 0,
                            0x0001 | 0x0002 | 0x0010,
                        )
                    except Exception:
                        pass
                self._status_mapped = True
                self._last_status_pos = None

            x = self.x + (self.WIDTH - self.STATUS_W) // 2
            y = self.y - (self.STATUS_H + 10)
            pos = (x, y)
            if pos != getattr(self, "_last_status_pos", None):
                self._last_status_pos = pos
                if reposition_only:
                    self.status_win.geometry(f"+{x}+{y}")
                else:
                    self.status_win.geometry(f"{self.STATUS_W}x{self.STATUS_H}+{x}+{y}")
        except tk.TclError:
            # The Tk window can be destroyed while a queued after() callback is
            # still running during shutdown. That used to flood the log with
            # "bad window path name" tracebacks.
            pass

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

        lock = getattr(self, "_history_lock", None)
        if lock is not None:
            with lock:
                snapshot = list(self.history)
        else:
            snapshot = list(self.history)

        if not snapshot:
            menu.add_command(label="Nenhum histórico salvo.", state="disabled")
        else:
            menu.add_command(label="📋 HISTÓRICO (Clique para colar no último app)", state="disabled")
            menu.add_separator()
            # Show newest first
            for text in reversed(snapshot):
                display_text = text if len(text) <= 50 else text[:47] + "..."
                menu.add_command(label=display_text, command=lambda t=text: self._inject_history_text(t))
                
        menu.post(event.x_root, event.y_root)

    def _inject_history_text(self, text):
        self._set_status("Colando...")

        def _paste_worker():
            try:
                self._type_text(text)
                self._ui_post(self._on_history_pasted)
            except Exception as e:
                print(f"[!] Erro ao colar historico: {e}", flush=True)

        threading.Thread(target=_paste_worker, daemon=True).start()

    def _on_history_pasted(self):
        self._set_status("Texto colado!")
        self.root.after(2000, lambda: self._set_status("Pausado") if not self.is_listening and not self._is_processing else None)

    # ── Quick Language Selector ───────────────────────────────────────────

    def _update_lang_badge(self):
        lang = self.settings.get("language", "pt-BR")
        badge_text = "🌐 AUTO"
        if lang == "translate_en":
            badge_text = "➔ EN"
        elif lang == "translate_es":
            badge_text = "➔ ES"
        elif lang in ("pt-BR", "pt"):
            badge_text = "🇧🇷 PT-BR"
        if hasattr(self, '_lang_btn'):
            self.canvas.itemconfig(self._lang_btn, text=badge_text)

    def _on_lang_badge_click(self, event):
        menu = tk.Menu(
            self.root, tearoff=0, bg="#252840", fg="#e0e2f0",
            activebackground="#3a3d55", activeforeground="#4dd964",
            font=("Segoe UI", 9)
        )
        menu.add_command(label="🌍 IDIOMA / TRADUÇÃO", state="disabled")
        menu.add_separator()

        options = [
            ("🌐 Automático (detectar idioma)", "auto"),
            ("🔤 Traduzir para Inglês (PT ➔ EN)", "translate_en"),
            ("🇪🇸 Traduzir para Espanhol (PT ➔ ES)", "translate_es"),
            ("🇧🇷 Português (Brasil)", "pt-BR"),
        ]

        current = self.settings.get("language", "pt-BR")
        for label, code in options:
            prefix = "✓ " if code == current else "   "
            menu.add_command(
                label=f"{prefix}{label}",
                command=lambda c=code: self._set_language_fast(c)
            )

        menu.post(event.x_root, event.y_root)

    def _set_language_fast(self, code):
        self.settings["language"] = code
        save_settings(self.settings)
        self._update_lang_badge()
        display_names = {
            "auto": "Idioma: Automático",
            "translate_en": "Traduzir ➔ Inglês",
            "translate_es": "Traduzir ➔ Espanhol",
            "pt-BR": "Idioma: Português (Brasil)"
        }
        name = display_names.get(code, "Idioma alterado")
        self._set_status(name)
        self.root.after(
            1500,
            lambda: self._set_status("Pausado") if not self.is_listening and not self._is_processing else None)
        print(f"[OK] Idioma rápido alterado: {code}", flush=True)

    # ── Hotkey ───────────────────────────────────────────────────────────

    def _setup_hotkey(self):
        """Register global hotkey via kernel RegisterHotKey (pynput only as fallback).

        Running pynput's low-level WH_KEYBOARD_LL hook in parallel forces Windows
        to wake Python/GIL on every keystroke system-wide, causing input lag and
        stalling during PC load. Kernel RegisterHotKey uses zero global hooks.
        """
        hotkey_str = self.settings.get("hotkey", "Alt+H")
        self._last_t = 0.0
        self._hotkey_gen = getattr(self, "_hotkey_gen", 0) + 1
        gen = self._hotkey_gen

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

        def _on_hotkey_trigger():
            if gen != getattr(self, "_hotkey_gen", gen) or getattr(self, "_closing", False):
                return
            now = time.monotonic()
            if now - self._last_t > 0.4:
                self._last_t = now
                self._ui_post(self.toggle)

        def _start_pynput_fallback():
            if gen != getattr(self, "_hotkey_gen", gen) or getattr(self, "_closing", False):
                return
            try:
                from pynput import keyboard as pynput_kb
                pynput_map = {"alt": "<alt>", "ctrl": "<ctrl>", "shift": "<shift>", "win": "<cmd>"}
                spec_parts = [pynput_map.get(p, p) for p in parts]
                hotkey_spec = "+".join(spec_parts)
                listener = pynput_kb.GlobalHotKeys({hotkey_spec: _on_hotkey_trigger})
                listener.daemon = True
                listener.start()
                self._pynput_listener = listener
                print(f"[OK] Fallback hook global pynput ativo ({hotkey_spec})", flush=True)
            except Exception as e:
                print(f"[DEBUG] Fallback hook pynput: {e}", flush=True)

        # Tear down any previous pynput listener and signal old hotkey thread to exit
        if getattr(self, "_pynput_listener", None):
            try:
                self._pynput_listener.stop()
            except Exception:
                pass
            self._pynput_listener = None

        old_stop = getattr(self, "_hotkey_stop", None)
        if old_stop is not None:
            old_stop.set()
        old_thread = getattr(self, "_hotkey_thread_ref", None)
        if IS_WINDOWS and old_thread and old_thread.is_alive() and old_thread.ident:
            try:
                _user32.PostThreadMessageW(old_thread.ident, 0x0012, 0, 0)  # WM_QUIT
            except Exception:
                pass

        if not IS_WINDOWS:
            _start_pynput_fallback()
            return

        stop_flag = threading.Event()
        self._hotkey_stop = stop_flag

        def _hotkey_thread():
            user32 = _user32
            registered = False
            try:
                # Brief background retry so a previous thread's UnregisterHotKey finishes first.
                for attempt in range(6):
                    if stop_flag.is_set() or getattr(self, "_closing", False):
                        return
                    if user32.RegisterHotKey(None, HOTKEY_ID, mod_flags | 0x4000, vk_code):
                        registered = True
                        break
                    time.sleep(0.05)

                if registered:
                    print(f"[OK] Hotkey {hotkey_str} registrada (RegisterHotKey, kernel)", flush=True)
                else:
                    print(f"[!] Falha ao registrar {hotkey_str} via RegisterHotKey; usando fallback pynput", flush=True)
                    _start_pynput_fallback()
                    return

                msg = wintypes.MSG()
                while not stop_flag.is_set() and not getattr(self, "_closing", False):
                    try:
                        ret = user32.GetMessageW(ctypes.byref(msg), None, 0, 0)
                        if ret == 0 or ret == -1:
                            break
                        if msg.message == 0x0312:  # WM_HOTKEY
                            _on_hotkey_trigger()
                    except Exception as e:
                        print(f"[!] Erro no loop de atalho: {e}", flush=True)
                        time.sleep(1)
            finally:
                if registered:
                    try:
                        user32.UnregisterHotKey(None, HOTKEY_ID)
                    except Exception:
                        pass

        t = threading.Thread(target=_hotkey_thread, daemon=True)
        t.start()
        self._hotkey_thread_ref = t

    # ── Core ─────────────────────────────────────────────────────────────

    def toggle(self):
        """Hotkey: start dictating when idle, finish when already recording.

        This used to toggle the WINDOW's visibility, and the window stays on
        screen after a dictation ends. So the next press only HID it and the
        user's speech was never recorded - which looked exactly like "I clicked
        the chat, spoke, and nothing was transcribed". The hotkey now always
        means "dictate"; the X button hides the popup.
        """
        if self.is_listening:
            self._stop_listening()
            return

        # Destination = whatever the user is looking at right now.
        self._capture_target_window()
        self._show()

        if self._worker_active:
            # The previous worker may be finalising a stopped recording or
            # transcribing it. Starting another capture now would create two
            # streams and can paste the old text into the new session.
            self._start_requested = True
            self._set_status("Aguarde...")
            print("[!] Ditado anterior ainda finalizando; o novo foi enfileirado.", flush=True)
            return

        if self._is_processing:
            # Never drop the keypress: the previous transcription is still
            # running (long audio takes a while), so start as soon as it ends.
            self._start_requested = True
            self._set_status("Aguarde...")
            print("[!] Transcricao em andamento; o ditado comeca automaticamente "
                  "quando ela terminar.", flush=True)
            return

        self._start_listening()

    def toggle_mic(self):
        if self.is_listening:
            self._stop_listening()
        elif self._worker_active:
            self._start_requested = True
            self._set_status("Aguarde...")
            print("[!] Ditado anterior ainda finalizando; o novo foi enfileirado.", flush=True)
        elif self._is_processing:
            self._start_requested = True
            self._set_status("Aguarde...")
            print("[!] Transcricao em andamento; o ditado comeca automaticamente "
                  "quando ela terminar.", flush=True)
        else:
            self._start_listening()

    def _show(self):
        self.is_visible = True
        self._position()
        self.root.deiconify()
        if IS_WINDOWS:
            self._apply_no_activate(self.root)
            try:
                hwnd = self.root.winfo_id()
                ctypes.windll.user32.ShowWindow(hwnd, 4)  # SW_SHOWNOACTIVATE
                ctypes.windll.user32.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)
            except Exception:
                pass
        else:
            self.root.lift()
            self.root.attributes("-topmost", True)
        self._update_status_pos()

    def _hide(self):
        self.is_visible = False
        self._status_mapped = False
        self._last_timer_sec = None
        try:
            self.status_win.withdraw()
        except tk.TclError:
            pass
        try:
            self.root.withdraw()
        except tk.TclError:
            pass

    def _cancel_record_timer(self):
        if self._timer_job is not None:
            try:
                self.root.after_cancel(self._timer_job)
            except Exception:
                pass
            self._timer_job = None
        self._last_timer_sec = None

    def _update_record_timer(self):
        self._timer_job = None
        if not self.is_listening or getattr(self, "_closing", False):
            return
        if self._record_started_at is not None and not self._is_processing:
            elapsed = max(0, int(time.monotonic() - self._record_started_at))
            if elapsed != getattr(self, "_last_timer_sec", None):
                self._last_timer_sec = elapsed
                try:
                    self.sw_canvas.itemconfig(
                        self._sw_label,
                        text=f"Ouvindo  {elapsed // 60:02d}:{elapsed % 60:02d}",
                    )
                except tk.TclError:
                    return
        self._timer_job = self.root.after(250, self._update_record_timer)

    def _set_status(self, text, gen=None):
        # If a generation was passed, ignore stale updates
        if gen is not None and gen != self._status_gen:
            return
        try:
            self.sw_canvas.itemconfig(self._sw_label, text=text)
        except tk.TclError:
            return
        if self.is_visible:
            self._update_status_pos()

    def _set_partial(self, text):
        pass  # Text underneath mic permanently removed per user request

    def _start_listening(self):
        if self.is_listening:
            return
        if self._worker_active:
            self._start_requested = True
            self._set_status("Aguarde...")
            print("[!] Transcrição em andamento; novo ditado enfileirado.", flush=True)
            return
        if self._is_processing:
            print("[!] Transcrição em andamento. Aguarde.", flush=True)
            self._set_status("Aguarde...")
            return

        # Wait asynchronously if the previous thread has not finished exiting yet
        # (never block the Tk main thread with thread.join).
        old_thread = getattr(self, '_listen_thread', None)
        if old_thread and old_thread.is_alive():
            retries = getattr(self, '_retry_start_count', 0)
            if retries < 20:
                self._retry_start_count = retries + 1
                self.root.after(20, self._start_listening)
                return
        self._retry_start_count = 0

        # Freeze the paste destination for this session. Only look for a new one
        # when there is none yet (or it died): the background watcher already keeps
        # it up to date with the last window clicked, and re-capturing here
        # avoids stale buffers. Refresh the title label once cleanly at start.
        if not self._target_hwnd or not self._target_is_alive():
            self._capture_target_window()
        elif IS_WINDOWS:
            self._target_label = self._window_label(self._target_hwnd)
        self._target_locked = False
        self._session_id += 1
        self._status_gen = self._session_id
        self._mic_problem = None
        self._last_timer_sec = None
        if self._target_label:
            print(f"[OK] Deste ditado: {self._target_label}", flush=True)

        self.is_listening = True
        self._worker_active = True
        self._record_started_at = time.monotonic()
        self.stop_event.clear()
        self.force_stop_audio = False
        self._mic_active = True

        self._start_anim()
        self._set_status("Ouvindo...")
        self._update_status_pos()
        self._update_record_timer()

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
        self._cancel_record_timer()
        if self._stop_listening_fn:
            self._stop_listening_fn(wait_for_stop=False)
            self._stop_listening_fn = None
        self._stop_anim()
        self._set_partial("")

    # ── Recognition Loop ─────────────────────────────────────────────────

    def _transcribe_and_type(self, audio_data, sample_rate, gen,
                             force_keep_listening=False, speech_peak=None):
        if not audio_data or len(audio_data) < 1200:
            return

        self._is_processing = True
        self.root.after(0, self._start_anim)
        self.root.after(0, self._set_status, "Transcrevendo...", gen)
        self.root.after(0, self._set_partial, "⏳ ...")

        raw_len = len(audio_data)
        text = ""
        language = self.settings.get("language", "pt-BR")
        try:
            audio_obj = sr.AudioData(audio_data, sample_rate, 2)
            engine = self.settings.get("engine", "Groq (Whisper API)")
            groq_key = (self.settings.get("groq_api_key") or "").strip()

            if engine == "Groq (Whisper API)" and groq_key:
                client = self._get_groq_client(groq_key)
                if client:
                    def _transcribe_wav(wav_payload):
                        kwargs = build_whisper_kwargs(language, wav_payload)
                        transcription = client.audio.transcriptions.create(**kwargs)
                        raw_t = transcription.text.strip()
                        if language == "translate_en":
                            return self._translate_text(client, raw_t, "English")
                        if language == "translate_es":
                            return self._translate_text(client, raw_t, "Spanish")
                        return raw_t

                    # Groq Whisper API limit is 25MB. At 16kHz 16-bit mono, 20MB is ~10.9 minutes.
                    CHUNK_LIMIT_BYTES = 20 * 1024 * 1024
                    if raw_len <= CHUNK_LIMIT_BYTES:
                        wav_bytes = audio_obj.get_wav_data()
                        text = _transcribe_wav(wav_bytes)
                        wav_bytes = None
                    else:
                        # Safely slice long audio (>10 mins) into chunks aligned to 2-byte PCM
                        step = CHUNK_LIMIT_BYTES - (CHUNK_LIMIT_BYTES % 2)
                        chunks = [audio_data[i:i + step] for i in range(0, raw_len, step)]
                        parts = []
                        total_chunks = len(chunks)
                        for idx, chunk in enumerate(chunks):
                            self._ui_post(self._set_partial, f"⏳ {idx+1}/{total_chunks}...")
                            c_obj = sr.AudioData(chunk, sample_rate, 2)
                            p_text = _transcribe_wav(c_obj.get_wav_data())
                            c_obj = None
                            if p_text:
                                parts.append(p_text)
                        chunks = None
                        text = " ".join(parts)

            # Fallback to Google if Groq was not used or failed
            if not text:
                google_lang = "pt-BR" if language in ["auto", "translate_en", "translate_es"] else language
                text = self.recognizer.recognize_google(audio_obj, language=google_lang, show_all=False)
            audio_obj = None

        except sr.UnknownValueError:
            print("[DEBUG] Áudio não compreendido", flush=True)
        except Exception as e:
            print(f"[✗] Erro na transcrição: {e}", flush=True)
        finally:
            audio_data = None
            if raw_len > 2 * 1024 * 1024:
                gc.collect()

        if text:
            text = text.strip()
            lower_text = text.lower().strip()
            clean_word = lower_text.strip(".,;:!?—–()[]\"' ")

            subtitle_watermarks = [
                "amara.org", "sônia ruberti", "sonia ruberti",
                "legendas:", "legenda por", "legendas por", "legendado por",
                "tradução e sincronização", "traducao e sincronizacao",
                "obrigado por assistir", "obrigada por assistir",
                "thanks for watching", "thank you for watching",
                "subtitles by", "subtitles created by", "closed captions",
                "inscreva-se no canal", "se inscreva no canal",
                "deixe seu like", "deixe o seu like", "deixe o like",
                "curta e compartilhe", "ative o sininho",
                "todos os direitos reservados", "transcribed by",
                "translated by", "assistir ao vídeo", "assistir a este vídeo",
                "transcribe verbatim", "spoken language without translating",
                "transcreva exatamente", "sem traduzir nada"
            ]
            phantom_words = [
                "obrigado", "obrigada", "thank you", "thanks", "you",
                "bye", "bye bye", "goodbye", "tchau", "valeu", "falou",
                "alô", "alo", "olá", "ola", "ei", "e aí", "e ai",
                "the end", "fim", "so", "yes", "no", "transcreva", "transcribe"
            ]

            is_hallucination = False
            if any(w in lower_text for w in subtitle_watermarks):
                for w in subtitle_watermarks:
                    text = re.sub(re.escape(w), "", text, flags=re.IGNORECASE).strip()
                lower_text = text.lower().strip()
                clean_word = lower_text.strip(".,;:!?—–()[]\"' ")

            if len(clean_word) <= 1 or text in [".", "..", "...", "?", "!", "—", "-", "…"]:
                is_hallucination = True
            elif lower_text.startswith(("[", "(")) and lower_text.endswith(("]", ")")):
                is_hallucination = True
            elif (clean_word in phantom_words and
                  (raw_len < sample_rate * 2 or
                   (speech_peak is not None and speech_peak < 800))):
                is_hallucination = True

            if is_hallucination:
                print(f"[!] Alucinação de silêncio bloqueada: '{text}'", flush=True)
                text = ""
            else:
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

                lock = getattr(self, "_history_lock", None)
                if lock is not None:
                    with lock:
                        self.history.append(text)
                        if len(self.history) > 5:
                            self.history.pop(0)
                else:
                    self.history.append(text)
                    if len(self.history) > 5:
                        self.history.pop(0)

                try:
                    self._type_text(text)
                except Exception as e:
                    print(f"[!] Falha ao inserir o texto no aplicativo de destino: {e}",
                          flush=True)
                preview = text[:35] + "..." if len(text) > 35 else text
                self._ui_post(self._set_partial, preview)
        else:
            print("[✗] Nenhum texto retornado.", flush=True)
            self._ui_post(self._set_partial, "Não compreendido")

        self._is_processing = False
        self._audio_level = 0.0

        if force_keep_listening and not self.stop_event.is_set():
            self._ui_post(self._set_status, "Ouvindo...", gen)
        else:
            self.is_listening = False
            self._mic_active = False
            self._ui_post(self._stop_anim)
            if self.is_visible:
                self._ui_post(self._set_status, "Pausado", gen)
                if not text:
                    self._ui_post(self._set_partial, "Clique 🎙 para gravar")

        # The queued-start handling lives in _recognition_loop's finally block
        # so it also runs when a recording has no speech or fails.

    # ── Recognition Loop ─────────────────────────────────────────────────

    def _recognition_loop(self):
        """Wrapper that always leaves the app in a consistent state.

        A read failure used to leave is_listening=True, which made every later
        hotkey press silently do nothing and looked exactly like a dead
        microphone.
        """
        session = self._session_id
        try:
            self._recognition_loop_inner(session)
        finally:
            if self._session_id == session:
                self.is_listening = False
                self._mic_active = False
                self._is_processing = False
                self._audio_level = 0.0
                self._target_locked = False
                self._ui_post(self._cancel_record_timer)
                self._ui_post(self._stop_anim)
                self._worker_active = False
                if self._start_requested:
                    self._start_requested = False
                    print("[OK] Iniciando o ditado que ficou na fila.", flush=True)
                    self._ui_post(self._start_listening)

    def _acquire_stream(self):
        """Open the best available microphone stream.

        Order: cached device (fast path) -> freshly resolved device -> system
        default. Returns (stream, device_index, sample_rate); stream is None when
        nothing could be opened.
        """
        dev_idx = self._cached_dev_idx
        rate = self._cached_rate

        # Fast path: this device was already validated during pre-warm (<40ms).
        if dev_idx is not None and rate is not None:
            try:
                stream = self._open_input_stream(dev_idx, rate)
                print(f"[OK] Microfone aberto instantaneamente (cache: {rate}Hz, idx={dev_idx})",
                      flush=True)
                return stream, dev_idx, rate
            except Exception as e:
                print(f"[DEBUG] Falha no fast path mic: {e}", flush=True)
                # The cached index is stale: drop it and rebuild the engine so
                # the re-enumerated device table is picked up.
                self._cached_dev_idx = None
                self._cached_rate = None
                self._reset_audio_engine()

        # The cached device is gone (unplugged / re-enumerated): probe again and
        # use whichever entry actually captures audio.
        dev_idx, rate, note, healthy = self._resolve_input_device()
        self._mic_problem = None if healthy else note
        if dev_idx is not None:
            try:
                stream = self._open_input_stream(dev_idx, rate)
                self._cached_dev_idx = dev_idx
                self._cached_rate = rate
                print(f"[OK] Microfone aberto ({rate}Hz, idx={dev_idx})", flush=True)
                return stream, dev_idx, rate
            except Exception as e:
                print(f"[!] Não foi possível abrir o microfone: {e}", flush=True)
                self._mic_problem = f"nao foi possivel abrir o microfone ({e})"
                self._reset_audio_engine()

        # Last resort: let PortAudio pick the system default.
        for r in self._rates_to_try(48000):
            try:
                stream = self._open_input_stream(None, r)
                print(f"[OK] Fallback mic padrão aberto ({r}Hz)", flush=True)
                return stream, None, r
            except Exception:
                continue

        return None, None, None

    def _recognition_loop_inner(self, session):
        stream, dev_idx, actual_rate = self._acquire_stream()

        if stream is None:
            print("[✗] Erro fatal: Não foi possível abrir nenhum dispositivo de áudio.", flush=True)
            self._mic_problem = self._mic_problem or "nenhum dispositivo de audio pode ser aberto"
            self.root.after(0, self._set_status, "Erro no mic", session)
            self.root.after(0, self._stop_listening)
            return

        gen = self._status_gen
        self.root.after(0, self._set_status, "Ouvindo...", gen)

        # Unlimited recording mode: keep recording until the user stops (hotkey
        # or mic click) and transcribe once at the end.
        phrase_frames = []
        total_bytes = 0
        rms_values = []          # every chunk's level: the speech decision uses all of them
        frames_read = 0
        loop_start = time.perf_counter()
        max_bytes = int(actual_rate * 2 * MAX_RECORD_SECONDS)
        aborted = False
        recoveries = 0

        while not self.stop_event.is_set() and self._session_id == session:
            try:
                data = stream.read(CHUNK_SIZE, exception_on_overflow=False)
            except Exception as e:
                print(f"[!] Erro leitura de áudio: {e}", flush=True)
                self._mic_problem = f"falha ao ler o microfone ({e})"
                # The device was very likely re-enumerated under us (headset
                # unplugged, disk resetting the bus). Rebuild the engine and
                # reopen instead of throwing the whole session away: this is what
                # used to leave the microphone "bugado" until a restart.
                self._close_input_stream(stream)
                stream = None
                new_stream = new_rate = None
                if recoveries < MAX_MIC_RECOVERIES:
                    recoveries += 1
                    print(f"[!] Tentando recuperar o microfone "
                          f"(tentativa {recoveries}/{MAX_MIC_RECOVERIES})...", flush=True)
                    time.sleep(0.15)
                    self._cached_dev_idx = None
                    self._cached_rate = None
                    self._reset_audio_engine()
                    new_stream, _new_idx, new_rate = self._acquire_stream()
                if new_stream is None:
                    aborted = True
                    break
                if new_rate != actual_rate:
                    # Mixing two sample rates in one buffer would corrupt the
                    # audio: keep what was captured and transcribe it now.
                    print("[!] Microfone reconectado em outra taxa; "
                          "transcrevendo o áudio já capturado.", flush=True)
                    stream = new_stream
                    break
                stream = new_stream
                # The gap must not be counted as "audio arriving too slowly".
                frames_read = 0
                loop_start = time.perf_counter()
                continue
            if not data:
                continue

            rms = get_rms(data, 2)
            # Highly responsive mic pulse visualizer
            self._audio_level = min(1.0, max(0.0, (rms - 40) / 1000.0))

            rms_values.append(rms)
            phrase_frames.append(data)
            total_bytes += len(data)

            frames_read += CHUNK_SIZE
            elapsed = time.perf_counter() - loop_start
            # A backend that hands back audio faster than real time is not
            # capturing anything: stop instead of spinning and filling RAM with
            # gigabytes of frozen buffer.
            if frames_read > actual_rate * (elapsed + 3.0):
                print("[✗] O driver devolveu áudio mais rápido que o tempo real - "
                      "gravação cancelada (backend de áudio incompatível).", flush=True)
                self._mic_problem = "o driver de audio nao captura em tempo real"
                aborted = True
                break
            if total_bytes >= max_bytes:
                print(f"[!] Limite de {MAX_RECORD_SECONDS // 60} min de gravação atingido; "
                      f"transcrevendo o que já foi gravado.", flush=True)
                break

        self._close_input_stream(stream)

        if self._session_id != session:
            # The user already started a new dictation: drop this buffer so the
            # old text is not pasted on top of the new one.
            phrase_frames.clear()
            rms_values.clear()
            print("[!] Sessão de gravação substituída por uma nova; áudio descartado.",
                  flush=True)
            return

        if aborted or not phrase_frames:
            phrase_frames.clear()
            rms_values.clear()
            detail = self._mic_problem or "nenhum audio foi capturado"
            print(f"[✗] Gravação descartada: {detail}", flush=True)
            self._ui_post(self._set_status, "Sem áudio!", gen)
            return

        has_speech, peak, floor, threshold, speech_chunks = analyze_speech(rms_values)
        duration = len(rms_values) * CHUNK_SIZE / float(actual_rate or 1)

        if has_speech:
            pre_chunks = max(1, int(0.35 * actual_rate / CHUNK_SIZE))
            post_chunks = max(2, int(0.60 * actual_rate / CHUNK_SIZE))
            audio_payload, trim_start, trim_end = trim_silent_edges(
                phrase_frames, rms_values, threshold,
                pre_chunks=pre_chunks, post_chunks=post_chunks,
            )
            phrase_frames.clear()
            rms_values.clear()
            trimmed_duration = len(audio_payload) / max(1, actual_rate * 2)
            if not audio_payload or len(audio_payload) < 1200:
                print("[!] O recorte removeu todo o audio util; nada sera enviado.",
                      flush=True)
                self._ui_post(self._set_status, "Sem fala detectada", gen)
                return
            # Resample to 16kHz for Whisper (saves 3x bandwidth, extends duration to 13+ mins per 25MB)
            sample_rate = actual_rate
            if actual_rate != 16000 and audioop is not None:
                try:
                    audio_payload, _ = audioop.ratecv(audio_payload, 2, 1, actual_rate, 16000, None)
                    sample_rate = 16000
                except Exception as e:
                    print(f"[!] Erro ao converter sample rate: {e}", flush=True)
                    sample_rate = actual_rate

            print(f"[OK] Fala detectada ({duration:.1f}s, recorte={trimmed_duration:.1f}s, "
                  f"blocos={trim_start}..{trim_end}, pico={peak}, piso de ruido={floor}); "
                  f"enviando para transcricao.", flush=True)
            self._transcribe_and_type(
                audio_payload, sample_rate, gen,
                force_keep_listening=False, speech_peak=peak,
            )
        else:
            phrase_frames.clear()
            rms_values.clear()
            # Only reachable when the input really was silent: the peak never got
            # above the absolute minimum, or nothing stood out from the room.
            detail = self._mic_problem or "nenhuma fala detectada"
            print(f"[!] Sem fala detectada ({duration:.1f}s, pico={peak}, "
                  f"piso de ruido={floor}, limite={threshold}, "
                  f"blocos acima do limite={speech_chunks}). {detail}", flush=True)
            self._ui_post(self._set_status, "Sem fala detectada", gen)

    def _inject_unicode_keystrokes(self, text):
        """
        Directly inject Unicode characters into active window via Win32 SendInput.
        Does NOT touch clipboard, does NOT pollute Win+V history.
        """
        if not text:
            return False

        try:
            user32 = ctypes.windll.user32

            class KEYBDINPUT(ctypes.Structure):
                _fields_ = [
                    ("wVk", ctypes.c_ushort),
                    ("wScan", ctypes.c_ushort),
                    ("dwFlags", ctypes.c_ulong),
                    ("time", ctypes.c_ulong),
                    ("dwExtraInfo", ctypes.c_size_t),
                ]

            class MOUSEINPUT(ctypes.Structure):
                _fields_ = [
                    ("dx", ctypes.c_long),
                    ("dy", ctypes.c_long),
                    ("mouseData", ctypes.c_ulong),
                    ("dwFlags", ctypes.c_ulong),
                    ("time", ctypes.c_ulong),
                    ("dwExtraInfo", ctypes.c_size_t),
                ]

            class HARDWAREINPUT(ctypes.Structure):
                _fields_ = [
                    ("uMsg", ctypes.c_ulong),
                    ("wParamL", ctypes.c_ushort),
                    ("wParamH", ctypes.c_ushort),
                ]

            class INPUT_UNION(ctypes.Union):
                _fields_ = [
                    ("mi", MOUSEINPUT),
                    ("ki", KEYBDINPUT),
                    ("hi", HARDWAREINPUT),
                ]

            class INPUT(ctypes.Structure):
                _fields_ = [
                    ("type", ctypes.c_ulong),
                    ("union", INPUT_UNION),
                ]

            INPUT_KEYBOARD = 1
            KEYEVENTF_UNICODE = 0x0004
            KEYEVENTF_KEYUP = 0x0002

            # Release modifier keys (Alt, Ctrl, Shift, Win) if currently held
            for vk in (0x12, 0x11, 0x10, 0x5B, 0x5C):
                if user32.GetAsyncKeyState(vk) & 0x8000:
                    user32.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)

            utf16_units = text.encode("utf-16-le")
            code_units = [int.from_bytes(utf16_units[i:i+2], "little") for i in range(0, len(utf16_units), 2)]

            inputs = []
            for cu in code_units:
                # Key Down
                inp_down = INPUT(type=INPUT_KEYBOARD)
                inp_down.union.ki = KEYBDINPUT(
                    wVk=0,
                    wScan=cu,
                    dwFlags=KEYEVENTF_UNICODE,
                    time=0,
                    dwExtraInfo=0
                )
                inputs.append(inp_down)

                # Key Up
                inp_up = INPUT(type=INPUT_KEYBOARD)
                inp_up.union.ki = KEYBDINPUT(
                    wVk=0,
                    wScan=cu,
                    dwFlags=KEYEVENTF_UNICODE | KEYEVENTF_KEYUP,
                    time=0,
                    dwExtraInfo=0
                )
                inputs.append(inp_up)

            n = len(inputs)
            arr = (INPUT * n)(*inputs)
            cb_size = ctypes.sizeof(INPUT)
            res = user32.SendInput(n, arr, cb_size)
            return res == n
        except Exception as e:
            print(f"[!] SendInput error: {e}", flush=True)
            return False


    # ── Text injection (Direct Keystroke Injection via SendInput) ─────────

    def _focus_target_window(self, timeout=0.4):
        """Bring the target window to the foreground.

        Returns True only when the target really owns the foreground: Windows
        refuses foreground changes from a background process, and without
        checking, the Ctrl+V below would be delivered to whatever window happens
        to be focused instead of the chat the user clicked.
        """
        if not (IS_WINDOWS and self._target_hwnd):
            return False
        try:
            user32 = _user32
            target = self._target_hwnd

            if not user32.IsWindow(target):
                print("[!] A janela de destino não existe mais.", flush=True)
                self._target_hwnd = None
                return False

            if user32.GetForegroundWindow() == target:
                return True

            if user32.IsIconic(target):
                user32.ShowWindow(target, 9)  # SW_RESTORE
            elif not user32.IsWindowVisible(target):
                user32.ShowWindow(target, 5)  # SW_SHOW

            cur_fg = user32.GetForegroundWindow()
            app_thread = _kernel32.GetCurrentThreadId()
            cur_thread = user32.GetWindowThreadProcessId(cur_fg, None) if cur_fg else 0
            target_thread = user32.GetWindowThreadProcessId(target, None)

            attached = []
            for th in (cur_thread, target_thread):
                if th and th != app_thread and th not in attached:
                    if user32.AttachThreadInput(app_thread, th, True):
                        attached.append(th)
            try:
                user32.SetForegroundWindow(target)
                user32.BringWindowToTop(target)

                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if user32.GetForegroundWindow() == target:
                        return True
                    time.sleep(0.02)
            finally:
                for th in attached:
                    user32.AttachThreadInput(app_thread, th, False)

            print(f"[!] Não foi possível focar a janela de destino (0x{target:X}); "
                  f"o texto pode ir para outro aplicativo.", flush=True)
            return False
        except Exception as e:
            print(f"[!] Erro ao focar janela: {e}", flush=True)
            return False

    def _get_clipboard_text_safe(self):
        """Lê texto atual do clipboard com segurança e sem travar."""
        if not IS_WINDOWS:
            return None
        try:
            user32 = ctypes.windll.user32
            kernel32 = ctypes.windll.kernel32
            for _ in range(5):
                if user32.OpenClipboard(None):
                    break
                time.sleep(0.01)
            else:
                return None
            try:
                if not user32.IsClipboardFormatAvailable(13):  # CF_UNICODETEXT
                    return None
                h = user32.GetClipboardData(13)
                if not h:
                    return None
                p = kernel32.GlobalLock(h)
                if not p:
                    return None
                try:
                    return ctypes.wstring_at(p)
                finally:
                    kernel32.GlobalUnlock(h)
            finally:
                user32.CloseClipboard()
        except Exception:
            return None

    def _set_clipboard_text_safe(self, text, exclude_history=True):
        """Define o texto do clipboard com segurança total de 64 bits."""
        if not IS_WINDOWS:
            return False
        try:
            user32 = ctypes.windll.user32
            kernel32 = ctypes.windll.kernel32
            for _ in range(5):
                if user32.OpenClipboard(None):
                    break
                time.sleep(0.01)
            else:
                return False
            try:
                user32.EmptyClipboard()
                if text:
                    enc = text.encode("utf-16-le") + b"\x00\x00"
                    h = kernel32.GlobalAlloc(0x0002, len(enc))  # GMEM_MOVEABLE
                    if not h:
                        return False
                    p = kernel32.GlobalLock(h)
                    if not p:
                        return False
                    ctypes.memmove(p, enc, len(enc))
                    kernel32.GlobalUnlock(h)
                    user32.SetClipboardData(13, h)

                    if exclude_history:
                        cf_history = user32.RegisterClipboardFormatW("CanIncludeInClipboardHistory")
                        cf_cloud = user32.RegisterClipboardFormatW("CanUploadToCloudClipboard")
                        cf_exclude = user32.RegisterClipboardFormatW("ExcludeClipboardContentFromMonitorProcessing")
                        for fmt in (cf_history, cf_cloud, cf_exclude):
                            if fmt:
                                hf = kernel32.GlobalAlloc(0x0002, 4)
                                if hf:
                                    pf = kernel32.GlobalLock(hf)
                                    if pf:
                                        ctypes.memset(pf, 0, 4)
                                        kernel32.GlobalUnlock(hf)
                                        user32.SetClipboardData(fmt, hf)
                return True
            finally:
                user32.CloseClipboard()
        except Exception as e:
            print(f"[!] Erro ao definir clipboard: {e}", flush=True)
            return False

    def _paste_isolated_instant(self, text):
        """
        Cola o texto 'no seco' instantaneamente via Ctrl+V:
        1. Salva o que o usuário tinha no clipboard intacto.
        2. Configura a transcrição com flags de exclusão do Win+V (CanIncludeInClipboardHistory=0).
        3. Dispara Ctrl+V para colar em 0ms (sem digitar letra por letra).
        4. Restaura o clipboard anterior do usuário logo em seguida (o Ctrl+V e Win+V do usuário permanecem intocados).
        """
        if not IS_WINDOWS:
            return False

        # 1. Backup do texto original do clipboard
        saved_text = self._get_clipboard_text_safe()

        # 2. Injeta texto com flags de exclusão total do histórico
        if not self._set_clipboard_text_safe(text, exclude_history=True):
            return False

        time.sleep(0.015)

        # 3. Disparar Ctrl+V instantâneo
        user32 = ctypes.windll.user32
        VK_CONTROL = 0x11
        VK_V = 0x56
        KEYEVENTF_KEYUP = 0x0002

        # Liberar qualquer tecla modificadora presa
        for vk in (0x12, 0x11, 0x10, 0x5B, 0x5C):
            if user32.GetAsyncKeyState(vk) & 0x8000:
                user32.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)

        user32.keybd_event(VK_CONTROL, 0, 0, 0)
        user32.keybd_event(VK_V, 0, 0, 0)
        time.sleep(0.02)
        user32.keybd_event(VK_V, 0, KEYEVENTF_KEYUP, 0)
        user32.keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0)
        print(f"[OK] Texto colado a seco instantaneamente via Ctrl+V ({len(text)} chars)", flush=True)

        # 4. Restaurar clipboard original em background após o app destino consumir a colagem (60ms)
        def _restore_worker():
            time.sleep(0.06)
            self._set_clipboard_text_safe(saved_text, exclude_history=True)

        threading.Thread(target=_restore_worker, daemon=True).start()
        return True

    def _type_text(self, text):
        """
        Insere o texto no aplicativo/chat de destino instantaneamente ('no seco').
        1. Utiliza colagem atômica que isola o Windows V e restaura o Ctrl+V do usuário.
        2. Mantém o histórico salvo exclusivamente no menu do Voice Dictation.
        """
        if not text or not text.strip():
            return

        text_to_inject = text + " "

        if IS_WINDOWS:
            user32 = ctypes.windll.user32
            cur_fg = user32.GetForegroundWindow()
            cur_pid = wintypes.DWORD()
            if cur_fg:
                user32.GetWindowThreadProcessId(cur_fg, ctypes.byref(cur_pid))

            # Se o usuário já está clicado em um chat/navegador (qualquer janela externa),
            # adota essa janela com 100% de prioridade como o alvo exato onde o cursor está:
            if cur_fg and cur_pid.value != os.getpid() and user32.IsWindow(cur_fg):
                self._target_hwnd = cur_fg
                self._target_focus = self._focused_control(cur_fg)
                self._target_label = self._window_label(cur_fg)
            else:
                # O usuário clicou no botão verde do Voice Dictation para parar:
                # restaura o foco para o último chat/janela externa que ele clicou antes:
                self._focus_target_window()

            time.sleep(0.04)

        if self._target_label:
            field = f" (campo focado 0x{self._target_focus:X})" if self._target_focus else ""
            print(f"[OK] Inserindo texto em: {self._target_label}{field}", flush=True)

        # 1. Colagem instantânea 'a seco' com isolamento total do Win+V e restauração do Ctrl+V
        injected = False
        if IS_WINDOWS:
            injected = self._paste_isolated_instant(text_to_inject)

        # 2. Fallback de segurança caso Ctrl+V falhe no app
        if not injected:
            if IS_WINDOWS:
                injected = self._inject_unicode_keystrokes(text_to_inject)
                if injected:
                    print(f"[OK] Texto digitado via SendInput fallback ({len(text)} chars)", flush=True)

        # 3. Fallback secundário via pynput
        if not injected:
            try:
                from pynput.keyboard import Controller as KeyController
                kb = KeyController()
                kb.type(text_to_inject)
                print(f"[OK] Texto digitado via pynput fallback ({len(text)} chars)", flush=True)
            except Exception as e:
                print(f"[!] Erro no fallback: {e}", flush=True)

    # ── Run ──────────────────────────────────────────────────────────────

    def run(self):
        self.root.mainloop()

    def quit(self):
        self._closing = True
        self._stop_listening()
        self._target_locked = False
        if getattr(self, "_pynput_listener", None):
            try:
                self._pynput_listener.stop()
            except Exception:
                pass
            self._pynput_listener = None
        if getattr(self, "_hotkey_stop", None) is not None:
            self._hotkey_stop.set()
        ht = getattr(self, "_hotkey_thread_ref", None)
        if IS_WINDOWS and ht and ht.is_alive() and ht.ident:
            try:
                _user32.PostThreadMessageW(ht.ident, 0x0012, 0, 0)
            except Exception:
                pass
        with self._pa_lock:
            if self._pa is not None:
                try:
                    self._pa.terminate()
                except Exception:
                    pass
                self._pa = None
            self._dev_count = None
            self._streams_open = 0
            self._pa_refresh_pending = False
            self._cached_dev_idx = None
            self._cached_rate = None
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
    if is_loopback_device(mic_name):
        print("[!] AVISO: o microfone selecionado e virtual/loopback; "
              "ele pode capturar o audio do PC em vez da voz.", flush=True)
    print(f"     Log: {_LOG_FILE}", flush=True)

    try:
        app.run()
    except Exception as e:
        log.critical(f"Crash no mainloop: {e}", exc_info=True)


if __name__ == "__main__":
    main()
