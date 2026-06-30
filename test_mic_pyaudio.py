import pyaudio
import wave
import time

DURATION = 5  # segundos
SAMPLE_RATE = 16000
CHUNK = 1024
FORMAT = pyaudio.paInt16
CHANNELS = 1

print("Teste de microfone - falarei por 5 segundos")

p = pyaudio.PyAudio()

# Lista dispositivos de entrada
print("\nDispositivos de entrada disponíveis:")
for i in range(p.get_device_count()):
    info = p.get_device_info_by_index(i)
    if info['maxInputChannels'] > 0:
        print(f"  [{i}] {info['name']}")

# Abre o dispositivo padrão (ou mude o índice se quiser)
stream = p.open(format=FORMAT,
                channels=CHANNELS,
                rate=SAMPLE_RATE,
                input=True,
                frames_per_buffer=CHUNK)

print("\nGravando... Fale agora!")
frames = []
for _ in range(0, int(SAMPLE_RATE / CHUNK * DURATION)):
    data = stream.read(CHUNK, exception_on_overflow=False)
    frames.append(data)

stream.stop_stream()
stream.close()
p.terminate()

# Salva o áudio
wf = wave.open("teste_gravado.wav", 'wb')
wf.setnchannels(CHANNELS)
wf.setsampwidth(p.get_sample_size(FORMAT))
wf.setframerate(SAMPLE_RATE)
wf.writeframes(b''.join(frames))
wf.close()

print(f"\nGravado! Salvo como 'teste_gravado.wav'. Escute o arquivo.")