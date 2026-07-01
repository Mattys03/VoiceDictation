# Voice Dictation

<div align="center">
  <a href="https://github.com/Mattys03/VoiceDictation/releases/latest">
    <img src="https://img.shields.io/badge/📦_Download_Release-0078D4?style=for-the-badge&logo=github" alt="Download Release" />
  </a>
</div>

![Platform](https://img.shields.io/badge/Plataforma-Windows-blue)
![Python](https://img.shields.io/badge/Python-3.10%2B-green)
![License](https://img.shields.io/badge/Licen%C3%A7a-MIT-purple)

**VoiceDictation** é um utilitário avançado desenvolvido em Python que utiliza Inteligência Artificial (Google Speech Recognition / Groq Whisper API) para transcrever áudio em tempo real e digitar automaticamente em qualquer janela ativa do Windows.

---

## 📖 Como o Projeto Começou e Por Que Foi Criado?

A ideia principal do **Voice Dictation** nasceu da necessidade de ter uma ferramenta robusta, rápida e personalizável para **digitação por voz (Speech-to-Text)** no Windows, superando as limitações da ferramenta nativa do sistema (o famoso atalho `Win + H`).

**O problema da ferramenta nativa do Windows:**
* É limitada geograficamente e por idioma.
* Não permite trocar livremente os motores de reconhecimento de voz.
* Não possui um histórico acessível para reutilizar textos ditados.
* Nem sempre interage bem com ferramentas de terceiros e não permite a customização de atalhos globais.

**A Solução (O Porquê):**
O **Voice Dictation** foi criado para preencher essa lacuna, servindo como uma interface flutuante (widget) construída inteiramente em Python. A sua função principal é permitir que o usuário, com o toque de um atalho totalmente configurável (como `Alt + G`), possa falar livremente e ver seu texto sendo digitado instantaneamente em **qualquer aplicativo** que estiver em foco (Bloco de notas, navegadores, editores de código, chats, etc). 

O diferencial é a adoção de APIs de Inteligência Artificial modernas. O usuário pode optar pelo motor gratuito do Google ou usar o estado da arte com a API da **Groq (Whisper)**, garantindo velocidade impressionante e precisão perfeita, mesmo para jargões complexos ou sotaques regionais.

---

## ⚙️ Como Funciona a Arquitetura do Projeto?

O sistema foi desenvolvido utilizando Python moderno e foca em duas frentes: uma interface de usuário extremamente fluida e um backend de processamento de áudio invisível.

* **Interface Visual (UI):** Utiliza a biblioteca nativa `tkinter`, mas com um design translúcido e moderno. A janela recebe as propriedades de remoção de bordas nativas e *always on top*. Foi implementado um sistema de *drag-and-drop* para mover a janela livremente pela tela e animações matemáticas precisas, onde o microfone pulsa brilhantemente em verde (captando voz) ou amarelo/dourado (processando IA) reagindo instantaneamente aos decibéis do áudio.
* **Captura de Áudio e IA:** O cérebro do áudio captura o som via hardware (físico ou virtual, como VB-Audio) e envia para o motor selecionado, alcançando uma precisão semântica formidável.
* **Injeção de Texto Dinâmica:** Através de integrações profundas com a API do Windows (`ctypes`), o sistema "caça" automaticamente qual é a janela que está em foco na tela (`GetForegroundWindow`) e "injeta" os caracteres simulando digitação humana em nível de hardware.
* **Sistema de Atalhos (Hotkeys):** O projeto utiliza a API robusta do Windows (`RegisterHotKey`), garantindo que o atalho seja capturado diretamente no Kernel do sistema operacional, funcionando de forma 100% confiável independente de jogos em tela cheia ou programas pesados rodando por cima.
* **Módulo de Auto Pontuação (Smart NLP):** O código possui um módulo capaz de compreender intenções. Ele capitaliza inícios de frases, atende a comandos de voz literais (ao falar "vírgula" ele digita `,`) e possui regras gramaticais embutidas que injetam vírgulas automaticamente antes de palavras de conexão típicas do português (como "mas", "porém", "porque", "então").

---

## 🎯 Vantagens e Casos de Uso

### ✅ Vantagens Absolutas:
1. **Workflow Ininterrupto (Flow State):** O usuário não precisa tirar a mão do mouse ou fechar programas. O widget flutuante e os atalhos globais mantêm o foco total na tarefa.
2. **Independência de Software:** Ao contrário de extensões de navegador ou add-ons do Word, o Voice Dictation digita em **qualquer lugar**. Se houver um cursor piscando, ele vai digitar perfeitamente.
3. **Poder do Whisper (Groq):** O reconhecimento de voz é acelerado por LPU (Language Processing Units), permitindo que a IA da Groq transcreva textos quase instantaneamente, corrigindo concordâncias verbais que os motores antigos e locais falhariam miseravelmente.
4. **Histórico Salvador (Clipboard):** A ferramenta armazena as últimas falas em um menu drop-up inteligente. Com um clique, frases recorrentes são redigitadas sem a necessidade de ditar novamente.

### ⚠️ Limitações:
* **Dependência de Conexão com a Internet:** Como utiliza APIs de nuvem poderosas, requer internet estável para não apresentar latência (o "Transcrevendo...").
* **Exclusividade Windows:** O núcleo do projeto é intimamente ligado ao `ctypes.windll.user32` para manter a performance perfeita dos atalhos e foco de janelas, não sendo portado diretamente para Linux ou macOS.

---

## 📦 Configuração e Uso

1. Faça o download da Release mais recente (topo da página).
2. Abra o arquivo `config.example.json` e renomeie para `config.json`.
3. Insira a sua própria chave de API da Groq (caso queira utilizar o Whisper ultrarrápido) ou deixe em branco para usar o motor gratuito padrão do Google.
4. Execute o arquivo `Start_VoiceDictation.vbs` para iniciar a ferramenta de forma limpa, sem console poluindo a tela.
5. Pressione seu atalho (padrão inicial no painel de Configurações) e comece a falar.

## 📝 Licença
Distribuído sob a Licença MIT.
