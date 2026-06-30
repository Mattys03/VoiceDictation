#!/bin/bash
# ─────────────────────────────────────────────────────────────────────
# Voice Dictation Tool – Instalador (Linux)
# Instala dependências, cria autostart e atalho no menu
# ─────────────────────────────────────────────────────────────────────

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_PATH="$SCRIPT_DIR/voice_dictation.py"
AUTOSTART_DIR="$HOME/.config/autostart"
APPS_DIR="$HOME/.local/share/applications"

echo "╔══════════════════════════════════════════════╗"
echo "║   Voice Dictation – Instalador (Linux)       ║"
echo "╚══════════════════════════════════════════════╝"
echo ""

# ── Check Python ──
if ! command -v python3 &> /dev/null; then
    echo "[✗] Python3 não encontrado!"
    echo "    Instale com: sudo apt install python3 python3-pip"
    exit 1
fi
echo "[✓] Python3 encontrado: $(python3 --version)"

# ── Install system dependencies ──
echo ""
echo "[..] Verificando dependências do sistema..."
MISSING_PKGS=""

# Check for PyAudio dependencies
if ! dpkg -l | grep -q "portaudio19-dev" 2>/dev/null; then
    MISSING_PKGS="$MISSING_PKGS portaudio19-dev"
fi
if ! dpkg -l | grep -q "python3-tk" 2>/dev/null; then
    MISSING_PKGS="$MISSING_PKGS python3-tk"
fi
if ! command -v xclip &> /dev/null && ! command -v xsel &> /dev/null; then
    MISSING_PKGS="$MISSING_PKGS xclip"
fi

if [ -n "$MISSING_PKGS" ]; then
    echo "[..] Instalando pacotes do sistema:$MISSING_PKGS"
    sudo apt install -y $MISSING_PKGS
else
    echo "[✓] Dependências do sistema OK"
fi

# ── Install Python dependencies ──
echo ""
echo "[..] Instalando dependências Python..."
pip3 install -r "$SCRIPT_DIR/requirements.txt" --break-system-packages 2>/dev/null || \
pip3 install -r "$SCRIPT_DIR/requirements.txt"
echo "[✓] Dependências Python instaladas!"

# ── Make script executable ──
chmod +x "$SCRIPT_PATH"

# ── Create autostart entry ──
echo ""
mkdir -p "$AUTOSTART_DIR"
cat > "$AUTOSTART_DIR/voice-dictation.desktop" << EOF
[Desktop Entry]
Type=Application
Name=Voice Dictation
Comment=Ditado por voz com Alt+H (estilo Windows Win+H)
Exec=bash -c 'sleep 3 && python3 "$SCRIPT_PATH"'
Icon=audio-input-microphone
Terminal=false
Categories=Utility;Accessibility;
StartupNotify=false
X-GNOME-Autostart-enabled=true
X-GNOME-Autostart-Delay=5
EOF
echo "[✓] Autostart configurado: $AUTOSTART_DIR/voice-dictation.desktop"

# ── Create application menu entry ──
mkdir -p "$APPS_DIR"
cat > "$APPS_DIR/voice-dictation.desktop" << EOF
[Desktop Entry]
Type=Application
Name=Voice Dictation
Comment=Ditado por voz com Alt+H (estilo Windows Win+H)
Exec=python3 "$SCRIPT_PATH"
Icon=audio-input-microphone
Terminal=false
Categories=Utility;Accessibility;
StartupNotify=false
EOF
echo "[✓] Atalho do menu criado: $APPS_DIR/voice-dictation.desktop"

echo ""
echo "═══════════════════════════════════════════════"
echo "  Instalação concluída!"
echo ""
echo "  • Alt+H → Ativar/desativar ditado por voz"
echo "  • O programa inicia automaticamente no login"
echo "  • Requer conexão com a internet"
echo ""
echo "  Para iniciar agora:"
echo "    python3 $SCRIPT_PATH &"
echo "═══════════════════════════════════════════════"
