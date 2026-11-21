"""Regressao de performance, estabilidade e anti-congelamento da UI.

Estes testes cobrem todas as otimizacoes de leveza e as causas reais do
congelamento branco ("STATUS_APPLICATION_HANG" / -805306369):
- `_watch_audio_devices` nao pode adquirir `_pa_lock` na thread do Tkinter.
- `_resolve_input_device` deve soltar `_pa_lock` durante o probe e parar no
  primeiro microfone saudavel.
- Chamadas bloqueantes (`_inject_history_text`, `_start_listening`,
  `SettingsDialog.show`) nunca podem travar o loop de eventos do Tk.
- Workers em background nunca podem chamar metodos de widgets Tk diretamente;
  devem sempre despachar via `root.after`.
- Quando `RegisterHotKey` funciona no kernel do Windows, nao deve instalar
  hook global `pynput` (`WH_KEYBOARD_LL`).
- O cliente Groq deve usar timeout finito e liberar buffers grandes sem bugs.
"""

import os
import sys
import threading
import time
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import voice_dictation as vd
import settings_dialog as sd


def _minimal_app():
    app = object.__new__(vd.VoiceDictationApp)
    app._closing = False
    app.is_listening = False
    app.is_visible = False
    app._is_processing = False
    app._worker_active = False
    app._mic_active = False
    app._audio_level = 0.0
    app._start_requested = False
    app._session_id = 1
    app._status_gen = 1
    app._mic_problem = None
    app._pa = None
    app._pa_lock = threading.RLock()
    app._resolve_lock = threading.Lock()
    app._groq_lock = threading.Lock()
    app._history_lock = threading.Lock()
    app._groq_client = None
    app._last_groq_key = None
    app._dev_count = 2
    app._streams_open = 0
    app._pa_refresh_pending = False
    app._cached_dev_idx = None
    app._cached_rate = None
    app._prewarm_running = False
    app._target_hwnd = None
    app._target_focus = None
    app._target_label = ""
    app._target_locked = False
    app._last_timer_sec = None
    app._status_mapped = False
    app._last_status_pos = None
    app._last_anim_state = None
    app._retry_start_count = 0
    app._no_activate_hwnds = set()
    app._timer_job = None
    app._anim_id = None
    app._record_started_at = None
    app.history = []
    app.settings = {"language": "pt-BR", "engine": "Groq (Whisper API)", "groq_api_key": "k"}
    app.stop_event = threading.Event()
    app.root = mock.MagicMock()
    app.canvas = mock.MagicMock()
    app.sw_canvas = mock.MagicMock()
    app._sw_label = 1
    app._mic_glow = 2
    app._mic_circle = 3
    app.recognizer = mock.MagicMock()
    return app


class UiThreadNonBlockingTest(unittest.TestCase):
    """Garante que o loop principal Tk nunca trave em locks, joins ou pastes."""

    def test_watch_audio_devices_never_blocks_when_pa_lock_is_held(self):
        app = _minimal_app()
        prewarms = []
        app._prewarm_async = lambda: prewarms.append("prewarm")
        app._os_input_device_count = lambda: 3  # mudou de 2 -> 3

        locked = threading.Event()
        release = threading.Event()

        def hold_lock():
            with app._pa_lock:
                locked.set()
                release.wait(timeout=2.0)

        t = threading.Thread(target=hold_lock, daemon=True)
        t.start()
        self.assertTrue(locked.wait(timeout=1.0))

        started = time.perf_counter()
        try:
            app._watch_audio_devices()
        finally:
            release.set()
            t.join(timeout=1.0)

        elapsed = time.perf_counter() - started
        self.assertLess(elapsed, 0.15, "Monitor de audio travou esperando _pa_lock")
        self.assertEqual(prewarms, ["prewarm"])
        app.root.after.assert_called()

    def test_watch_audio_devices_does_not_prewarm_while_recording(self):
        app = _minimal_app()
        prewarms = []
        app._prewarm_async = lambda: prewarms.append("prewarm")
        app._os_input_device_count = lambda: 5
        app.is_listening = True

        app._watch_audio_devices()
        self.assertEqual(prewarms, [])

    def test_inject_history_text_runs_in_background_thread(self):
        app = _minimal_app()
        started = threading.Event()
        unblock = threading.Event()
        caller_threads = []

        def blocking_type(text):
            caller_threads.append(threading.current_thread().ident)
            started.set()
            unblock.wait(timeout=2.0)

        app._type_text = blocking_type
        app._set_status = lambda *a, **k: None

        t0 = time.perf_counter()
        app._inject_history_text("frase de teste")
        call_duration = time.perf_counter() - t0

        self.assertLess(call_duration, 0.1, "_inject_history_text bloqueou a UI")
        self.assertTrue(started.wait(timeout=1.0))
        self.assertNotEqual(caller_threads[0], threading.current_thread().ident)
        unblock.set()

    def test_start_listening_defers_instead_of_blocking_join_if_worker_thread_alive(self):
        app = _minimal_app()
        alive_thread = mock.MagicMock()
        alive_thread.is_alive.return_value = True
        app._listen_thread = alive_thread

        t0 = time.perf_counter()
        app._start_listening()
        elapsed = time.perf_counter() - t0

        self.assertLess(elapsed, 0.05)
        alive_thread.join.assert_not_called()
        app.root.after.assert_called_with(20, app._start_listening)

    def test_recognition_loop_does_not_touch_tk_directly_from_worker_thread(self):
        app = _minimal_app()
        direct_calls = []
        app._stop_anim = lambda: direct_calls.append("stop_anim")
        app._cancel_record_timer = lambda: direct_calls.append("cancel_timer")
        app._recognition_loop_inner = lambda session: None

        posted = []
        app.root.after = lambda delay, fn, *args: posted.append((delay, fn, args))

        t = threading.Thread(target=app._recognition_loop)
        t.start()
        t.join(timeout=1.0)

        self.assertEqual(direct_calls, [], "Worker nao deve chamar metodos Tk direto")
        posted_fns = [fn for _d, fn, _a in posted]
        self.assertIn(app._cancel_record_timer, posted_fns)
        self.assertIn(app._stop_anim, posted_fns)


class AudioResolutionPerformanceTest(unittest.TestCase):
    """Garante probe rapido e zero contencao de _pa_lock durante E/S de audio."""

    def test_resolve_short_circuits_on_first_healthy_device(self):
        app = _minimal_app()
        devices = {
            1: ({"name": "Mic 1", "defaultSampleRate": 44100.0}, "MME"),
            2: ({"name": "Mic 2", "defaultSampleRate": 48000.0}, "DirectSound"),
            3: ({"name": "Mic 3", "defaultSampleRate": 48000.0}, "Windows WASAPI"),
        }
        app.settings = {"microphone_name": "Mic 1", "microphone_index": None}
        app._get_pa = lambda: mock.MagicMock()
        app._input_devices = lambda pa: devices

        probed = []

        def probe(dev_idx, rate, chunks=8):
            probed.append((dev_idx, rate))
            return 450, True, None

        app._probe_input_device = probe
        dev_idx, rate, _note, healthy = app._resolve_input_device()

        self.assertTrue(healthy)
        self.assertEqual(dev_idx, 1)
        self.assertEqual(len(probed), 1, "Nao deveria sondar outros devices apos achar um saudavel")

    def test_pa_lock_is_not_held_during_probe_io(self):
        app = _minimal_app()
        devices = {
            1: ({"name": "Mic 1", "defaultSampleRate": 44100.0}, "MME"),
        }
        app.settings = {"microphone_name": "Mic 1", "microphone_index": None}
        app._get_pa = lambda: mock.MagicMock()
        app._input_devices = lambda pa: devices

        lock_free_from_other_thread = []

        def probe(dev_idx, rate, chunks=8):
            # Verifica se OUTRA thread consegue pegar _pa_lock sem esperar o probe
            def try_lock():
                got = app._pa_lock.acquire(blocking=False)
                lock_free_from_other_thread.append(got)
                if got:
                    app._pa_lock.release()

            t = threading.Thread(target=try_lock)
            t.start()
            t.join(timeout=1.0)
            return 450, True, None

        app._probe_input_device = probe
        app._resolve_input_device()
        self.assertEqual(lock_free_from_other_thread, [True])


class HotkeyEfficiencyTest(unittest.TestCase):
    """Kernel RegisterHotKey nao pode manter hook pynput WH_KEYBOARD_LL duplicado."""

    def test_kernel_hotkey_success_skips_pynput_hook(self):
        app = _minimal_app()
        app.settings = {"hotkey": "Alt+H"}

        pynput_starts = []
        fake_pynput_kb = types.ModuleType("pynput.keyboard")

        class FakeGlobalHotKeys:
            def __init__(self, mapping):
                pynput_starts.append(mapping)
                self.daemon = True

            def start(self):
                pass

            def stop(self):
                pass

        fake_pynput_kb.GlobalHotKeys = FakeGlobalHotKeys
        fake_pynput = types.ModuleType("pynput")
        fake_pynput.keyboard = fake_pynput_kb

        reg_called = threading.Event()

        def fake_register(hwnd, hid, mod, vk):
            reg_called.set()
            return 1

        def fake_get_msg(msg_ptr, hwnd, fmin, fmax):
            time.sleep(0.05)
            return 0  # encerra loop limpo

        fake_user32 = mock.MagicMock()
        fake_user32.RegisterHotKey.side_effect = fake_register
        fake_user32.GetMessageW.side_effect = fake_get_msg
        fake_user32.UnregisterHotKey.return_value = 1

        with mock.patch.object(vd, "IS_WINDOWS", True), \
             mock.patch.object(vd, "_user32", fake_user32), \
             mock.patch.dict(sys.modules, {"pynput": fake_pynput, "pynput.keyboard": fake_pynput_kb}):
            app._setup_hotkey()
            self.assertTrue(reg_called.wait(timeout=1.0))
            if app._hotkey_thread_ref:
                app._hotkey_thread_ref.join(timeout=1.0)

        self.assertEqual(pynput_starts, [], "Nao deveria ligar WH_KEYBOARD_LL quando RegisterHotKey funciona")

    def test_kernel_hotkey_failure_activates_pynput_fallback(self):
        app = _minimal_app()
        app.settings = {"hotkey": "Alt+H"}

        pynput_started = threading.Event()
        fake_pynput_kb = types.ModuleType("pynput.keyboard")

        class FakeGlobalHotKeys:
            def __init__(self, mapping):
                self.mapping = mapping
                self.daemon = True

            def start(self):
                pynput_started.set()

            def stop(self):
                pass

        fake_pynput_kb.GlobalHotKeys = FakeGlobalHotKeys
        fake_pynput = types.ModuleType("pynput")
        fake_pynput.keyboard = fake_pynput_kb

        fake_user32 = mock.MagicMock()
        fake_user32.RegisterHotKey.return_value = 0  # falha todas tentativas

        with mock.patch.object(vd, "IS_WINDOWS", True), \
             mock.patch.object(vd, "_user32", fake_user32), \
             mock.patch.dict(sys.modules, {"pynput": fake_pynput, "pynput.keyboard": fake_pynput_kb}):
            app._setup_hotkey()
            self.assertTrue(pynput_started.wait(timeout=1.5),
                            "Deveria ativar fallback pynput se RegisterHotKey falhar")
            if app._hotkey_thread_ref:
                app._hotkey_thread_ref.join(timeout=1.0)


class TargetPollingAndUiCacheTest(unittest.TestCase):
    """Reduz alocacoes Win32 repetitivas e redesenhos redundantes do Tkinter."""

    def test_same_foreground_hwnd_skips_window_label_lookups(self):
        app = _minimal_app()
        label_calls = []
        app._window_label = lambda hwnd: label_calls.append(hwnd) or f"Janela {hwnd}"
        app._focused_control = lambda hwnd: 999

        fake_user32 = mock.MagicMock()
        fake_user32.GetForegroundWindow.return_value = 12345
        fake_user32.IsWindow.return_value = 1
        fake_user32.GetWindowThreadProcessId.side_effect = (
            lambda hwnd, pid_ref: setattr(pid_ref._obj, "value", os.getpid() + 100)
            if pid_ref is not None else 1
        )

        with mock.patch.object(vd, "IS_WINDOWS", True), \
             mock.patch.object(vd, "_user32", fake_user32):
            app._capture_target_window()
            app._capture_target_window()
            app._capture_target_window()

        self.assertEqual(len(label_calls), 1, "_window_label nao deve rodar toda vez no mesmo hwnd")

    def test_poll_interval_is_adaptive(self):
        app = _minimal_app()
        app._capture_target_window = lambda refresh_label=False: None
        scheduled = []
        app.root.after = lambda ms, fn: scheduled.append(ms)

        app.is_visible = False
        app.is_listening = False
        app._poll_target_window()

        app.is_visible = True
        app._poll_target_window()

        self.assertEqual(scheduled, [vd.TARGET_POLL_IDLE_MS, vd.TARGET_POLL_ACTIVE_MS])
        self.assertGreater(vd.TARGET_POLL_IDLE_MS, vd.TARGET_POLL_ACTIVE_MS)

    def test_apply_no_activate_caches_window_handle(self):
        app = _minimal_app()
        win = mock.MagicMock()
        win.winfo_id.return_value = 777

        fake_user32 = mock.MagicMock()
        fake_user32.GetAncestor.return_value = 778
        fake_user32.GetParent.return_value = 0
        fake_user32.GetWindowLongW.return_value = 0

        with mock.patch.object(vd, "IS_WINDOWS", True), \
             mock.patch.object(vd, "_user32", fake_user32):
            app._apply_no_activate(win)
            app._apply_no_activate(win)
            app._apply_no_activate(win)

        self.assertEqual(win.update_idletasks.call_count, 1)

    def test_record_timer_only_updates_canvas_when_second_changes(self):
        app = _minimal_app()
        app.is_listening = True
        app._is_processing = False
        app._record_started_at = 100.0

        with mock.patch.object(vd.time, "monotonic", side_effect=[105.1, 105.4, 106.1]):
            app._update_record_timer()
            app._update_record_timer()
            app._update_record_timer()

        self.assertEqual(app.sw_canvas.itemconfig.call_count, 2)


class GroqTimeoutAndBufferCleanupTest(unittest.TestCase):
    """Timeouts explicitos e seguranca apos liberacao de buffer grande."""

    def test_get_groq_client_configures_timeout_and_reuses_instance(self):
        app = _minimal_app()
        calls = []

        class FakeGroq:
            def __init__(self, **kwargs):
                calls.append(kwargs)

        fake_mod = types.ModuleType("groq")
        fake_mod.Groq = FakeGroq

        with mock.patch.dict(sys.modules, {"groq": fake_mod}):
            c1 = app._get_groq_client("key-1")
            c2 = app._get_groq_client("key-1")
            c3 = app._get_groq_client("key-2")

        self.assertIs(c1, c2)
        self.assertIsNot(c1, c3)
        self.assertEqual(len(calls), 2)
        self.assertGreater(calls[0]["timeout"], 0)
        self.assertLessEqual(calls[0]["timeout"], 60.0)

    def test_phantom_filter_works_after_audio_buffer_released(self):
        app = _minimal_app()
        client = mock.MagicMock()
        client.audio.transcriptions.create.return_value = mock.MagicMock(text="Obrigado")
        app._get_groq_client = lambda k: client
        app._type_text = mock.MagicMock()

        pcm = b"\x10\x00" * 16000  # 1s (< 2s) -> palavra fantasma deve ser bloqueada sem erro
        app._transcribe_and_type(pcm, 16000, gen=1, speech_peak=500)

        app._type_text.assert_not_called()
        self.assertEqual(app.history, [])

    def test_settings_dialog_load_mics_async_populates_safely(self):
        dlg = object.__new__(sd.SettingsDialog)
        dlg.settings = {"microphone_index": 1, "microphone_name": "Mic Teste"}
        dlg._mic_options = ["Padrao do Sistema"]
        dlg._mic_entries = {"Padrao do Sistema": (None, "")}
        dlg._mic_menu_widget = {"menu": mock.MagicMock()}
        dlg.mic_var = mock.MagicMock()
        dlg._populating_mics = False

        done = threading.Event()
        win_ref = mock.MagicMock()
        win_ref.winfo_exists.return_value = True
        dlg.win = win_ref

        def fake_after(delay, fn):
            fn()
            done.set()

        win_ref.after = fake_after

        def slow_mics():
            time.sleep(0.15)
            return [(1, "Mic Teste", "MME"), (2, "Outro", "WASAPI")]

        with mock.patch.object(sd, "get_microphones", slow_mics):
            t0 = time.perf_counter()
            dlg._load_mics_async(win_ref)
            launch_elapsed = time.perf_counter() - t0
            self.assertLess(launch_elapsed, 0.05)
            self.assertTrue(done.wait(timeout=1.5))

        self.assertFalse(dlg._populating_mics)
        self.assertGreaterEqual(len(dlg._mic_options), 3)
        dlg.mic_var.set.assert_called()

    def test_get_rms_memoryview_fallback_and_lazy_parse_hotkey(self):
        pcm16 = b"\xe8\x03\x18\xfc" * 16  # +1000 / -1000 alternado
        with mock.patch.object(vd, "audioop", None):
            self.assertGreater(vd.get_rms(pcm16, 2), 900)
            self.assertEqual(vd.get_rms(b"", 2), 0)

        mods, char = vd.parse_hotkey("Ctrl+Shift+H")
        self.assertEqual(char, "h")
        self.assertEqual(len(mods), 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
