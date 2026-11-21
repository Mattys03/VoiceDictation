# Diagnóstico de áudio — Voice Dictation

## O que estava errado

1. O Voice Dictation estava configurado para capturar **CABLE Output (VB-Audio Virtual Cable)**.
   Esse dispositivo é um barramento virtual: ele recebe o que for enviado ao CABLE Input,
   normalmente áudio do próprio PC (vídeos, jogos, chamadas). Ele não é o microfone físico.
2. O Equalizer APO estava processando o microfone fifine **duas vezes**:
   `Microfone.txt` era incluído dentro de `CABLE_Loopback.txt` e novamente no `config.txt`.
   Isso duplicava RNNoise + LoudMax e podia causar bombeamento, cortes e voz abafada.
3. O arquivo de microfone usava `Copy: L=R R=L` e o config global também aplicava
   `Copy: L=R R=L` em `Device: all`. Em uma entrada mono/estéreo isso pode inverter ou
   anular canais.
4. O Voice Dictation mandava um `prompt` em português para o Whisper. O Whisper trata o
   prompt como se fosse uma transcrição anterior; em trechos de ruído/silêncio ele pode
   continuar o texto e inventar créditos de legenda como "Legenda Adriana Zanotto".
   Também sobrava silêncio no fim do áudio enviado para a API.

## O que foi corrigido

- Voice Dictation agora usa o microfone físico **Microfone (4- fifine Microphone)**.
- Equalizer APO agora aplica **uma única cadeia** no fifine: RNNoise -> LoudMax.
- `Copy: L=R R=L` foi removido da configuração do microfone.
- A inversão de canais foi restaurada **somente na saída de áudio dos jogos**
  (`Saida_Jogos.txt`), sem tocar no microfone nem no CABLE.
- O CABLE Output não inclui mais a cadeia do microfone.
- O prompt do Whisper foi removido.
- O áudio passa a ser cortado nas bordas silenciosas antes de ir para a API.
- O filtro de nível mínimo de fala foi elevado para não mandar chiado baixo ao Whisper.
- Backups da configuração antiga estão em `C:\Program Files\EqualizerAPO\config\*backup_20261023_*`.

## CABLE Output alto para o Discord

O CABLE Output é alimentado pelo `discord_mic_booster.py`:

`Microfone fifine -> processamento -> CABLE Input -> CABLE Output -> Discord`

O booster foi ajustado para:

- ganho de 5x;
- passa-alta em 90 Hz;
- compressor suave para manter a voz constante;
- soft-limiter com teto de -1.5 dBFS (sem hard-clip em 0.92);
- watchdog que detecta callback de áudio parado e reconecta sozinho;
- sem noise gate por padrão, para não picotar palavras.

Em um teste com fala real, o CABLE Output ficou com pico em torno de -2.1 dBFS,
sem amostras em hard-clip. O microfone físico continua separado e sem o boost
do CABLE. O backup do booster antigo está em:

`E:\video prontos\codig cri\discord_mic_booster_backup_20261023.py`

## Se ainda houver corte na chamada

Existem outras duas fontes comuns de corte:

- **Microsoft Voice Clarity**: é um APO de IA do Windows que também reduz ruído.
  Ele aparece no endpoint do fifine junto com o Equalizer APO. Dois redutores de ruído
  em série podem cortar sílabas. Para desativar:
  Configurações > Sistema > Som > Entrada > Microfone (4- fifine) > Propriedades >
  Aprimoramentos de áudio > Desativado.
- **Redução de ruído do aplicativo da chamada** (ex.: Discord/Krisp, Teams).
  Use apenas um: RNNoise no Equalizer APO ou o filtro do aplicativo.
- Em `Painel de Controle > Som > Gravação > Microfone > Avançado`, deixe o formato
  em **48000 Hz** e evite "Permitir que os aplicativos tomem controle exclusivo".
  Se o app da chamada estiver usando o dispositivo em modo exclusivo, o Windows pode
  entregar o microfone para ele e cortar outros usos.
- Evite selecionar CABLE Output no Voice Dictation. Use CABLE apenas para roteamento
  consciente entre aplicativos.
