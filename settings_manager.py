"""
Settings Manager for Voice Dictation
Handles persistent configuration via JSON file.
"""

import json
import os


SETTINGS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.json")

DEFAULT_SETTINGS = {
    "hotkey": "Alt+H",
    "microphone_index": None,
    "microphone_name": "",
    "language": "pt-BR",
    "auto_punctuation": True,
}


def get_microphones():
    """Return list of (index, name) for REAL connected input devices.
    Filters to only WASAPI devices (modern Windows API) to avoid duplicates.
    Falls back to MME if no WASAPI devices found.
    """
    import pyaudio
    p = pyaudio.PyAudio()
    mics = []
    wasapi_mics = []
    mme_mics = []

    try:
        # Find WASAPI host API index
        wasapi_idx = None
        for i in range(p.get_host_api_count()):
            api = p.get_host_api_info_by_index(i)
            if "WASAPI" in api.get("name", ""):
                wasapi_idx = i
                break

        for i in range(p.get_device_count()):
            try:
                info = p.get_device_info_by_index(i)
                if info.get("maxInputChannels", 0) <= 0:
                    continue

                name = info.get("name", f"Dispositivo {i}")
                api_idx = info.get("hostApi", -1)
                api_info = p.get_host_api_info_by_index(api_idx)
                api_name = api_info.get("name", "")

                # Skip system mappers and virtual/capture drivers
                skip_keywords = [
                    "Mapeador", "Driver de captura",
                    "Mixagem", "Stereo Mix",
                    "primary sound", "sound mapper",
                ]
                if any(kw.lower() in name.lower() for kw in skip_keywords):
                    continue

                if "WASAPI" in api_name:
                    wasapi_mics.append((i, name))
                elif "MME" in api_name:
                    mme_mics.append((i, name))

            except Exception:
                continue

        # Prefer WASAPI, fallback to MME
        mics = wasapi_mics if wasapi_mics else mme_mics

    finally:
        p.terminate()

    return mics


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
    """Save settings to JSON file."""
    with open(SETTINGS_FILE, "w", encoding="utf-8") as f:
        json.dump(settings, f, indent=2, ensure_ascii=False)
