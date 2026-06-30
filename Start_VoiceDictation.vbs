






Set WshShell = CreateObject("WScript.Shell")
WshShell.CurrentDirectory = "E:\video prontos\codig cri\VoiceDictation"
WshShell.Run "C:\Python313\pythonw.exe voice_dictation.py", 0, False
