# Voice Dictation

<div align="center">
  <a href="https://github.com/Mattys03/VoiceDictation/releases/latest">
    <img src="https://img.shields.io/badge/📦_Download_Release-0078D4?style=for-the-badge&logo=github" alt="Download Release" />
  </a>
</div>

![Platform](https://img.shields.io/badge/Plataforma-Windows-blue)
![Python](https://img.shields.io/badge/Python-3.10%2B-green)
![License](https://img.shields.io/badge/Licen%C3%A7a-MIT-purple)

**VoiceDictation** é um utilitário desenvolvido em Python que utiliza a API de Reconhecimento de Voz do Google (Speech Recognition) para transcrever áudio em tempo real e digitar automaticamente em qualquer janela ativa do Windows.

## 🚀 Funcionalidades

- **Transcrição em Tempo Real:** Captura o áudio do microfone e injeta o texto processado na aplicação em foco.
- **Teclas de Atalho Globais (Hotkeys):** Suporte a atalhos que funcionam mesmo com a aplicação minimizada (ex: `Alt+H`), permitindo ligar e desligar o ditado instantaneamente.
- **Configuração Flexível:** Permite alterar o idioma (`pt-BR`, `en-US`), dispositivo de áudio (microfone) e o motor de IA via arquivo JSON.
- **Execução Oculta:** Desenvolvido para rodar de forma invisível em *background* sem poluir a barra de tarefas.

## 🛠️ Arquitetura e Tecnologias

- **Linguagem:** Python 3.10+
- **Bibliotecas Principais:** 
  - `SpeechRecognition` para captação do áudio.
  - `keyboard` para detecção de atalhos em nível de sistema operacional (System Hooks).
  - `pyautogui` para automação da digitação do teclado.

## 📦 Configuração e Uso

1. Faça o download da Release mais recente utilizando o botão acima.
2. Abra o arquivo `config.example.json` e renomeie para `config.json`.
3. Insira a sua própria chave de API (caso queira utilizar o Whisper via Groq) ou deixe em branco para usar o motor gratuito padrão.
4. Execute o arquivo `Start_VoiceDictation.vbs` para iniciar a ferramenta de forma silenciosa.

## 📝 Licença

Distribuído sob a Licença MIT.
