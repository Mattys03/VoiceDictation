import pyaudio
import wave
import time
import sys
import os

DURATION = 5
SAMPLE_RATE = 16000
CHUNK = 1024
FORMAT = pyaudio.paInt16
CHANNELS = 1

def list_microphones():
    p = pyaudio.PyAudio()
    mics = []
    print("\n🎤 MICROFONES DISPONÍVEIS:\n")
    for i in range(p.get_device_count()):
        info = p.get_device_info_by_index(i)
        if info['maxInputChannels'] > 0:
            name = info['name']
            mics.append((i, name))
            print(f"  [{i}] {name}")
    p.terminate()
    return mics

def test_mic(device_index, device_name):
    print(f"\n[TESTANDO] Índice {device_index}: {device_name}")
    print(f"  Falarei por {DURATION} segundos...")
    
    p = pyaudio.PyAudio()
    stream = p.open(format=FORMAT,
                    channels=CHANNELS,
                    rate=SAMPLE_RATE,
                    input=True,
                    input_device_index=device_index,
                    frames_per_buffer=CHUNK)
    
    frames = []
    for _ in range(0, int(SAMPLE_RATE / CHUNK * DURATION)):
        data = stream.read(CHUNK, exception_on_overflow=False)
        frames.append(data)
    
    stream.stop_stream()
    stream.close()
    p.terminate()
    
    # Salva WAV
    safe_name = "".join(c for c in device_name if c.isalnum() or c in (' ', '-', '_')).replace(' ', '_')
    filename = f"teste_mic_{device_index}_{safe_name}.wav"
    wf = wave.open(filename, 'wb')
    wf.setnchannels(CHANNELS)
    wf.setsampwidth(p.get_sample_size(FORMAT))
    wf.setframerate(SAMPLE_RATE)
    wf.writeframes(b''.join(frames))
    wf.close()
    
    print(f"  ✅ Gravado e salvo como: {filename}")
    return filename

def main():
    mics = list_microphones()
    if not mics:
        print("Nenhum microfone encontrado!")
        sys.exit(1)
    
    print("\nDigite o ÍNDICE do microfone que deseja testar (ou 'all' para testar todos):")
    choice = input("> ").strip()
    
    log_lines = []
    log_lines.append("=== LOG DE TESTE DE MICROFONE ===\n")
    log_lines.append(f"Data/Hora: {time.ctime()}\n")
    
    if choice.lower() == 'all':
        for idx, name in mics:
            log_lines.append(f"\n--- Testando índice {idx}: {name} ---")
            fname = test_mic(idx, name)
            log_lines.append(f"Arquivo gerado: {fname}")
    else:
        try:
            idx = int(choice)
            found = None
            for i, name in mics:
                if i == idx:
                    found = name
                    break
            if found:
                log_lines.append(f"\n--- Testando índice {idx}: {found} ---")
                fname = test_mic(idx, found)
                log_lines.append(f"Arquivo gerado: {fname}")
            else:
                print(f"Índice {idx} não encontrado na lista.")
                sys.exit(1)
        except ValueError:
            print("Entrada inválida. Use um número ou 'all'.")
            sys.exit(1)
    
    # Salva log
    log_file = "teste_mic_log.txt"
    with open(log_file, 'w', encoding='utf-8') as f:
        f.write("\n".join(log_lines))
    print(f"\n📄 Log salvo em: {log_file}")
    print("Escute os arquivos .wav para identificar qual microfone capturou sua voz corretamente.")
    print("Depois, configure esse índice no programa principal (via engrenagem ⚙ ou editando config.json).")

if __name__ == "__main__":
    main()