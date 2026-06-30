# Voice Dictation

<div align="center">
  <a href="https://github.com/Mattys03/VoiceDictation/releases/latest">
    <img src="https://img.shields.io/badge/📦_Download_Release-0078D4?style=for-the-badge&logo=github" alt="Download Release" />
  </a>
</div>

![Platform](https://img.shields.io/badge/Platform-Windows-blue)
![Python](https://img.shields.io/badge/Python-3.10%2B-green)
![License](https://img.shields.io/badge/License-MIT-purple)

A lightweight, highly responsive Voice Dictation tool for Windows that mimics the native `Win+H` experience but offers greater reliability, customizability, and offline-ready speech recognition capabilities via Google Speech Recognition API (with fallbacks).

## 🚀 Features

- **Global Hotkey:** Activate dictation from anywhere using a customizable hotkey (default: `Alt+H`).
- **Real-Time Transcription:** Extremely fast transcription using optimized audio chunking.
- **Auto-Punctuation Engine:** Intelligently adds commas, periods, and capitalization based on context and pauses.
- **Voice Commands:** Supports explicit punctuation commands (e.g., saying "vírgula" inserts `,`, "nova linha" inserts `\n`).
- **Floating UI:** Minimalist, unobtrusive floating widget that stays on top and indicates listening status with dynamic animations.
- **Smart Window Tracking:** Automatically pastes the transcribed text into the active window/application.
- **Duplicate Prevention:** Prevents multiple instances from running simultaneously.

## 🛠️ Requirements

- Windows 10/11
- Python 3.10+
- A working microphone

## 📦 Installation

1. Clone the repository:
   ```cmd
   git clone https://github.com/yourusername/VoiceDictation.git
   cd VoiceDictation
   ```

2. Run the installation script to setup the virtual environment and install dependencies:
   ```cmd
   install.bat
   ```

## ⚙️ Configuration

You can configure the hotkey and select the preferred microphone device. The settings are saved in `config.json`.
Click the **Gear (⚙)** icon on the floating widget to open the Settings Dialog.

## 🏃‍♂️ Usage

1. Run the application:
   ```cmd
   Start_VoiceDictation.vbs
   ```
   *(The VBS script ensures the application runs silently in the background without opening a console window).*

2. Press `Alt+H` (or your configured hotkey) to start dictation.
3. Speak clearly into the microphone. The widget will pulse yellow while processing.
4. When you stop speaking, the text will be automatically typed into your active window.

## 🏗️ Architecture

- `voice_dictation.py`: Main entry point containing the Tkinter floating UI, hotkey bindings, audio chunking, and the auto-punctuation engine.
- `settings_manager.py`: Handles loading and saving JSON configuration safely.
- `settings_dialog.py`: UI for selecting the microphone and modifying the global hotkey.

## 📝 License

Distributed under the MIT License. See `LICENSE` for more information.
