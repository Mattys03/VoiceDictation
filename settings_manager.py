"""
Settings Manager for Voice Dictation
Handles persistent configuration via JSON file.
"""

import json
import os
import re


SETTINGS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.json")

DEFAULT_SETTINGS = {
    "hotkey": "Alt+H",
    "microphone_index": None,
    "microphone_name": "",
    "language": "auto",
    "auto_punctuation": True,
    "allow_ellipsis": True,
    "smart_review": False,
    "engine": "Google (Gratuito)",
    "groq_api_key": "",
    "theme": "dark",
    "auto_start": False,
}

# Windows exposes the SAME physical microphone once per audio backend (MME,
# DirectSound, WASAPI, WDM-KS). Those entries are not interchangeable for
# PortAudio: the DirectSound backend can hand back a frozen buffer that never
# blocks, so read() returns instantly and capture silently dies. MME (the WAVE
# mapper) is the most reliable backend for capture, so it is preferred.
HOSTAPI_PREFERENCE = ("mme", "windows wasapi", "windows wdm-ks", "windows directsound")

# System mappers / loopback drivers that only produce noise for dictation.
_SKIP_DEVICE_KEYWORDS = (
    "mapeador", "driver de captura", "mixagem", "stereo mix",
    "primary sound", "sound mapper",
)

# Prefix matching is used to reconcile MME's 31-character truncation, but it is
# only trusted for reasonably long names so short names cannot swallow others.
_MIN_PREFIX_MATCH_LEN = 12


_LOOPBACK_HINTS = (
    "cable output", "cable input", "stereo mix", "mixagem est",
    "mixagem estereo", "what u hear", "loopback", "voicemeeter out",
)


def is_loopback_device(name):
    """True for virtual capture devices that normally carry PC audio.

    Using one of these for dictation makes the application transcribe videos,
    calls and game audio instead of the physical microphone.
    """
    low = (name or "").lower()
    return any(hint in low for hint in _LOOPBACK_HINTS)


def host_api_rank(api_name):
    """Rank a host API name; lower is better, unknown backends sort last."""
    low = (api_name or "").lower()
    for pos, pref in enumerate(HOSTAPI_PREFERENCE):
        if pref in low:
            return pos
    return len(HOSTAPI_PREFERENCE)


def normalize_device_name(name):
    """Collapse the cosmetic differences between Windows audio backends.

    MME truncates names to 31 characters and marks duplicates with a "2- "
    prefix, while DirectSound/WASAPI report the full name, so the same
    microphone appears as both 'Microfone (2- fifine Microphone' and
    'Microfone (2- fifine Microphone)'. They must compare as equal.
    """
    n = (name or "").lower().strip()
    while n.endswith(")"):
        n = n[:-1].strip()
    n = re.sub(r"\(\s*\d+\s*-\s*", "(", n)
    return " ".join(n.split())


def device_name_matches(name_a, name_b):
    """True when two device names (possibly from different backends) are the
    same physical device."""
    a = normalize_device_name(name_a)
    b = normalize_device_name(name_b)
    if not a or not b:
        return False
    if a == b:
        return True
    short, long = (a, b) if len(a) <= len(b) else (b, a)
    return len(short) >= _MIN_PREFIX_MATCH_LEN and long.startswith(short)


def get_microphones():
    """Return [(index, name, host_api)] with one entry per physical device.

    The duplicates created by the different audio backends are collapsed by
    normalize_device_name, keeping the entry from the backend PortAudio handles
    best. This matters because the saved index is used as the recording device:
    listing the same mic four times made it impossible to tell them apart (and
    identical labels made the dialog save the wrong index).
    """
    import pyaudio
    p = pyaudio.PyAudio()
    best = {}  # normalized name -> (rank, index, name, host_api)

    try:
        for i in range(p.get_device_count()):
            try:
                info = p.get_device_info_by_index(i)
                if info.get("maxInputChannels", 0) <= 0:
                    continue

                name = info.get("name", f"Dispositivo {i}")
                if any(kw in name.lower() for kw in _SKIP_DEVICE_KEYWORDS):
                    continue

                api_name = p.get_host_api_info_by_index(
                    info.get("hostApi", -1)
                ).get("name", "")
                rank = host_api_rank(api_name)

                match = None
                for known in best:
                    if device_name_matches(known, name):
                        match = known
                        break

                if match is None:
                    best[normalize_device_name(name)] = (rank, i, name, api_name)
                elif rank < best[match][0]:
                    best[match] = (rank, i, name, api_name)

            except Exception:
                continue

    finally:
        p.terminate()

    return [(i, name, api) for _rank, i, name, api in sorted(best.values(), key=lambda t: t[1])]


def load_settings():
    """Load settings from JSON file, creating defaults if needed."""
    if os.path.exists(SETTINGS_FILE):
        try:
            with open(SETTINGS_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
            return {**DEFAULT_SETTINGS, **data}
        except Exception:
            pass
    return dict(DEFAULT_SETTINGS)


def save_settings(settings):
    """Save settings atomically so a crash cannot leave truncated JSON."""
    tmp_path = SETTINGS_FILE + ".tmp"
    try:
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(settings, f, indent=2, ensure_ascii=False)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp_path, SETTINGS_FILE)
    finally:
        if os.path.exists(tmp_path):
            try:
                os.remove(tmp_path)
            except OSError:
                pass
