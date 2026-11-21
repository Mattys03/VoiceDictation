"""Regressao: o microfone precisa se recuperar sozinho de mudancas de dispositivo.

Contexto do bug: o programa ficava "bugado" sempre que o HD de 1TB (E:) caia do
barramento. O PortAudio congela a lista de dispositivos no momento em que o
PyAudio e' criado, entao depois de uma re-enumeracao o indice salvo apontava
para uma entrada morta, o programa insistia nela em TODA sessao
("[-9999] Unanticipated host error") e cada ditado terminava no fallback.

Estes testes cobrem as duas causas: nunca travar num dispositivo configurado
que nao captura, e reconstruir o motor de audio quando a lista muda.
"""

import os
import sys
import threading
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import voice_dictation as vd


REALTEK = "Microfone (Realtek HD Audio Mic input)"
FIFINE_MME = "Microfone (2- fifine Microphone"


class FakePa:
    """Stand-in minimo para pyaudio.PyAudio."""

    def __init__(self, devices, default_index=0):
        # devices: lista de (nome, indice_do_hostapi)
        self.devices = list(devices)
        self.default_index = default_index
        self.terminated = False

    def get_device_count(self):
        return len(self.devices)

    def get_device_info_by_index(self, i):
        name, host_api = self.devices[i]
        return {
            "name": name,
            "hostApi": host_api,
            "maxInputChannels": 1,
            "defaultSampleRate": 44100.0,
        }

    def get_host_api_info_by_index(self, i):
        return {"name": "MME" if i == 0 else "Windows WDM-KS"}

    def get_default_input_device_info(self):
        return {"index": self.default_index,
                "name": self.devices[self.default_index][0]}

    def terminate(self):
        self.terminated = True


def make_app(**settings):
    """VoiceDictationApp sem Tk, sem PyAudio de verdade e sem config em disco."""
    app = object.__new__(vd.VoiceDictationApp)
    app._pa_lock = threading.RLock()
    app._pa = None
    app._dev_count = None
    app._streams_open = 0
    app._pa_refresh_pending = False
    app._cached_dev_idx = None
    app._cached_rate = None
    app._mic_problem = None
    app.settings = dict(settings)
    return app


class ResolveInputDeviceTest(unittest.TestCase):
    """O mic configurado existe mas nao captura -> usar outro que funcione."""

    def _resolve(self, app, devices, probe, **settings):
        app.settings = dict(settings)
        app._get_pa = lambda: mock.MagicMock()
        app._input_devices = lambda pa: devices
        app._probe_input_device = probe
        saved = {}
        with mock.patch.object(vd, "save_settings",
                               lambda s: saved.update(dict(s))):
            result = app._resolve_input_device()
        return result, saved

    def test_configured_but_dead_device_does_not_block_a_working_one(self):
        app = make_app()
        devices = {
            1: ({"name": FIFINE_MME, "defaultSampleRate": 44100.0}, "MME"),
            25: ({"name": REALTEK, "defaultSampleRate": 44100.0}, "Windows WDM-KS"),
        }

        def probe(dev_idx, rate, chunks=12):
            if dev_idx == 25:
                return 0, False, "nao pode ser aberto ([-9999] Unanticipated host error)"
            return 500, True, None

        (dev_idx, rate, note, healthy), saved = self._resolve(
            app, devices, probe,
            microphone_index=25, microphone_name=REALTEK)

        self.assertTrue(healthy, note)
        self.assertEqual(dev_idx, 1, "deveria escolher o microfone que capta audio")

    def test_working_device_is_persisted_by_name_and_stale_index_is_dropped(self):
        app = make_app()
        devices = {
            1: ({"name": FIFINE_MME, "defaultSampleRate": 44100.0}, "MME"),
            25: ({"name": REALTEK, "defaultSampleRate": 44100.0}, "Windows WDM-KS"),
        }
        probe = lambda dev_idx, rate, chunks=12: (
            (0, False, "nao pode ser aberto") if dev_idx == 25 else (500, True, None))

        _result, saved = self._resolve(
            app, devices, probe,
            microphone_index=25, microphone_name=REALTEK)

        self.assertEqual(saved.get("microphone_name"), FIFINE_MME)
        self.assertIsNone(saved.get("microphone_index"),
                          "o indice numerico nao sobrevive a re-enumeracao")

    def test_same_microphone_on_a_better_backend_is_not_rewritten(self):
        """Os 4 backends do Windows sao o MESMO microfone: trocar de backend
        nao deve reescrever a configuracao do usuario."""
        app = make_app()
        devices = {
            25: ({"name": REALTEK, "defaultSampleRate": 44100.0}, "Windows WDM-KS"),
        }
        probe = lambda dev_idx, rate, chunks=12: (500, True, None)

        _result, saved = self._resolve(
            app, devices, probe,
            microphone_index=None, microphone_name=REALTEK)

        self.assertEqual(saved, {}, "nada deveria ter sido salvo")


class EngineRebuildTest(unittest.TestCase):
    """PortAudio congela a lista de dispositivos: ela precisa ser reconstruida.

    O sinal usado e' a contagem de dispositivos de captura do Windows, porque a
    instancia viva do PortAudio sempre devolve a lista antiga.
    """

    def test_engine_is_rebuilt_when_the_device_list_changes(self):
        app = make_app()
        first = FakePa([("Mic A", 0), ("Mic B", 0)])
        counts = [2]

        with mock.patch.object(vd.pyaudio, "PyAudio", return_value=first), \
             mock.patch.object(app, "_os_input_device_count", lambda: counts[0]):
            self.assertIs(app._get_pa(), first)
            app._cached_dev_idx = 21
            app._cached_rate = 44100
            # Mesma quantidade de dispositivos: nao reconstroi nada.
            self.assertIs(app._get_pa(), first)
            self.assertEqual(app._cached_dev_idx, 21)

            second = FakePa([("Mic A", 0), ("Mic B", 0), ("Mic C", 0)])
            counts[0] = 3
            with mock.patch.object(vd.pyaudio, "PyAudio", return_value=second):
                app._get_pa()

        self.assertIs(app._pa, second, "o motor deveria ter sido recriado")
        self.assertTrue(first.terminated)
        self.assertEqual(app._dev_count, 3)
        self.assertIsNone(app._cached_dev_idx, "indices antigos nao valem mais")
        self.assertIsNone(app._cached_rate)

    def test_engine_is_never_torn_down_while_a_stream_is_open(self):
        app = make_app()
        first = FakePa([("Mic A", 0)])
        counts = [1]

        with mock.patch.object(vd.pyaudio, "PyAudio", return_value=first), \
             mock.patch.object(app, "_os_input_device_count", lambda: counts[0]):
            app._get_pa()
            app._streams_open = 1
            second = FakePa([("Mic A", 0), ("Mic B", 0)])
            counts[0] = 2
            with mock.patch.object(vd.pyaudio, "PyAudio", return_value=second):
                app._reset_audio_engine()
                self.assertIs(app._pa, first, "nao pode derrubar um stream vivo")
                self.assertTrue(app._pa_refresh_pending)

                # Ao fechar o stream o rebuild adiado acontece.
                app._close_input_stream(mock.MagicMock())

        self.assertIs(app._pa, second)
        self.assertEqual(app._streams_open, 0)


class AcquireStreamTest(unittest.TestCase):
    """Cache morto -> descartar, reconstruir e reabrir em outro dispositivo."""

    def test_stale_cache_is_dropped_and_another_device_is_opened(self):
        app = make_app()
        app._cached_dev_idx = 25
        app._cached_rate = 44100
        calls = []

        def open_stream(dev_idx, rate):
            calls.append(("open", dev_idx, rate))
            if dev_idx == 25:
                raise OSError("[-9999] Unanticipated host error")
            return mock.MagicMock()

        app._open_input_stream = open_stream
        app._reset_audio_engine = lambda: calls.append(("reset",))
        app._resolve_input_device = lambda: (1, 44100, "fifine [MME]", True)

        stream, dev_idx, rate = app._acquire_stream()

        self.assertIsNotNone(stream)
        self.assertEqual(dev_idx, 1)
        self.assertIn(("reset",), calls, "o motor deveria ser reconstruido")
        self.assertEqual(app._cached_dev_idx, 1)
        self.assertIsNone(app._mic_problem)

    def test_falls_back_to_the_system_default_when_nothing_resolves(self):
        app = make_app()
        app._cached_dev_idx = None
        app._cached_rate = None
        opened = []

        def open_stream(dev_idx, rate):
            opened.append(dev_idx)
            if dev_idx is None:
                return mock.MagicMock()
            raise OSError("nope")

        app._open_input_stream = open_stream
        app._reset_audio_engine = lambda: None
        app._resolve_input_device = lambda: (25, 44100, "morto", False)

        stream, dev_idx, rate = app._acquire_stream()

        self.assertIsNotNone(stream)
        self.assertIsNone(dev_idx)
        self.assertEqual(opened[-1], None)


if __name__ == "__main__":
    unittest.main(verbosity=2)
