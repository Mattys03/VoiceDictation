import json
import os
import unittest

from settings_manager import is_loopback_device


class ConfigStructureTest(unittest.TestCase):
    def test_config_structure(self):
        """The example config must stay valid and expose the core options."""
        config_path = os.path.join(
            os.path.dirname(__file__), "..", "config.example.json"
        )
        self.assertTrue(os.path.exists(config_path))

        with open(config_path, "r", encoding="utf-8") as f:
            config = json.load(f)

        self.assertIn("language", config)
        self.assertIn("engine", config)
        self.assertEqual(config["language"], "auto")
        self.assertEqual(config["engine"], "Google (Gratuito)")
        self.assertEqual(config.get("groq_api_key", ""), "")
        self.assertEqual(config.get("theme"), "dark")
        self.assertIs(config.get("auto_start"), False)

    def test_loopback_devices_are_detected(self):
        self.assertTrue(is_loopback_device("CABLE Output (VB-Audio Virtual Cable)"))
        self.assertTrue(is_loopback_device("Mixagem estéreo (Realtek)"))
        self.assertFalse(is_loopback_device("Microfone (4- fifine Microphone)"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
