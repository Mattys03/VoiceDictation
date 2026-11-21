"""Regressao: deteccao de fala e destino do texto.

Bug 1 (reportado): "fico meia hora falando e ele diz que nao detectou fala".
O codigo antigo estimava o piso de ruido a partir dos PRIMEIROS blocos da
gravacao e exigia que a fala fosse 2x esse piso. Como o usuario comeca a falar
assim que a gravacao abre, o "piso de ruido" era a propria voz e o limite virava
2x a voz - inalcancavel. O log comprova: piso=10127, limite=20254.

Bug 2 (reportado): apertar Alt+H nao comecava um ditado novo. O atalho
alternava a VISIBILIDADE da janela, e a janela continua na tela depois de um
ditado; o proximo Alt+H apenas escondia a janela e a fala nunca era gravada.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import voice_dictation as vd


class AnalyzeSpeechTest(unittest.TestCase):
    def test_a_real_dictation_is_never_discarded(self):
        """Reproduz o caso do log: piso ~10000 (voz) e pico ~20000.

        O algoritmo antigo calculava limite = piso * 2 = ~20254 e descartava
        tudo. O novo tem que aceitar.
        """
        # Fala continua: 30% dos blocos mais altos, resto em nivel de voz.
        values = [10000 + (i % 37) * 25 for i in range(700)]
        values += [18000 + (i % 11) * 250 for i in range(300)]

        has_speech, peak, floor, threshold, chunks = vd.analyze_speech(values)

        self.assertTrue(has_speech, f"fala descartada (pico={peak}, piso={floor})")
        self.assertGreaterEqual(peak, 20000)
        self.assertGreater(chunks, 0)

    def test_speech_from_the_very_first_chunk_is_detected(self):
        """O usuario fala antes de qualquer pausa: nao existe 'inicio silencioso'."""
        values = [15000 + (i % 23) * 400 for i in range(200)]

        has_speech, peak, floor, _threshold, _chunks = vd.analyze_speech(values)

        self.assertTrue(has_speech, f"pico={peak}, piso={floor}")

    def test_silent_input_with_loud_hiss_is_not_speech(self):
        """Entrada quente (ruido alto e constante) sem ninguem falando."""
        values = [10000 + (i % 9) * 40 for i in range(400)]

        has_speech, peak, floor, _threshold, _chunks = vd.analyze_speech(values)

        self.assertFalse(has_speech, f"ruido constante virou fala (pico={peak})")

    def test_digital_silence_is_not_speech(self):
        self.assertFalse(vd.analyze_speech([0] * 500)[0])

    def test_low_level_hiss_is_not_sent_to_whisper(self):
        """A 162-RMS noise burst used to become the hallucination 'Thank you.'"""
        self.assertFalse(vd.analyze_speech([162] * 500)[0])

    def test_empty_recording_is_not_speech(self):
        self.assertFalse(vd.analyze_speech([])[0])

    def test_normal_speech_on_a_quiet_microphone_is_detected(self):
        values = [90, 110, 95, 100] * 50 + [1400, 1800, 900, 2200, 1600]

        self.assertTrue(vd.analyze_speech(values)[0])

    def test_pauses_do_not_raise_the_threshold(self):
        """Metade da gravacao em silencio: o piso deve ser o silencio, nao a voz."""
        values = [0] * 200 + [5000, 9000, 7000, 11000] * 50

        has_speech, peak, floor, _threshold, _chunks = vd.analyze_speech(values)

        self.assertTrue(has_speech)
        self.assertEqual(floor, 0, "o piso deveria ser o trecho silencioso")


class LanguageSelectionTest(unittest.TestCase):
    def test_portuguese_is_forced_for_groq_whisper(self):
        self.assertEqual(vd.whisper_language_code("pt-BR"), "pt")
        self.assertEqual(vd.whisper_language_code("pt"), "pt")
        self.assertEqual(vd.whisper_language_code("translate_en"), "pt")
        self.assertIsNone(vd.whisper_language_code("auto"))

    def test_whisper_request_has_no_prompt(self):
        """A prompt is treated as fake previous text and can seed hallucinations."""
        kwargs = vd.build_whisper_kwargs("pt-BR", b"wav")
        self.assertEqual(kwargs["language"], "pt")
        self.assertNotIn("prompt", kwargs)

    def test_whisper_auto_mode_omits_language(self):
        kwargs = vd.build_whisper_kwargs("auto", b"wav")
        self.assertNotIn("language", kwargs)
        self.assertNotIn("prompt", kwargs)


class SilenceTrimTest(unittest.TestCase):
    def test_only_silent_edges_are_trimmed(self):
        frames = [b"0", b"1", b"2", b"3", b"4", b"5"]
        levels = [10, 20, 900, 800, 15, 12]
        payload, start, end = vd.trim_silent_edges(
            frames, levels, threshold=300, pre_chunks=1, post_chunks=1)
        self.assertEqual(payload, b"1234")
        self.assertEqual((start, end), (1, 5))

    def test_no_speech_is_not_sent(self):
        payload, start, end = vd.trim_silent_edges(
            [b"a", b"b"], [10, 20], threshold=300, pre_chunks=2, post_chunks=2)
        self.assertEqual(payload, b"")
        self.assertEqual((start, end), (0, 0))


class HotkeySemanticsTest(unittest.TestCase):
    """Alt+H deve significar 'ditar', nao 'mostrar/esconder a janelinha'."""

    def _app(self):
        app = object.__new__(vd.VoiceDictationApp)
        app.is_listening = False
        app.is_visible = True      # a janela fica na tela depois de um ditado
        app._is_processing = False
        app._worker_active = False
        app._start_requested = False
        app.calls = []
        app._capture_target_window = lambda: app.calls.append("capture")
        app._show = lambda: app.calls.append("show")
        app._hide = lambda: app.calls.append("hide")
        app._start_listening = lambda: app.calls.append("start")
        app._stop_listening = lambda: app.calls.append("stop")
        app._set_status = lambda *a, **k: None
        return app

    def test_visible_but_idle_popup_starts_a_new_dictation(self):
        app = self._app()
        app.toggle()
        self.assertIn("start", app.calls)
        self.assertNotIn("hide", app.calls,
                         "o atalho nao pode apenas esconder a janela")

    def test_hotkey_while_listening_stops(self):
        app = self._app()
        app.is_listening = True
        app.toggle()
        self.assertEqual(app.calls.count("stop"), 1)
        self.assertNotIn("start", app.calls)

    def test_hotkey_during_a_transcription_is_queued_not_dropped(self):
        app = self._app()
        app._is_processing = True
        app.toggle()
        self.assertTrue(app._start_requested, "o toque do atalho foi perdido")
        self.assertNotIn("start", app.calls)

    def test_target_is_captured_when_a_dictation_starts(self):
        app = self._app()
        app.toggle()
        self.assertIn("capture", app.calls)


if __name__ == "__main__":
    unittest.main(verbosity=2)
