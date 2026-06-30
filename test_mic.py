import pyaudio
p = pyaudio.PyAudio()
print(f"Total devices: {p.get_device_count()}\n")
for i in range(p.get_device_count()):
    info = p.get_device_info_by_index(i)
    if info.get("maxInputChannels", 0) > 0:
        api = p.get_host_api_info_by_index(info["hostApi"])
        print(f"[{i}] {info['name']}")
        print(f"    API: {api['name']}")
        print(f"    Channels: {info['maxInputChannels']}")
        print(f"    Default SR: {info['defaultSampleRate']}")
        print()
p.terminate()
