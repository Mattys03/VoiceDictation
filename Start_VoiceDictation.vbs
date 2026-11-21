Set WshShell = CreateObject("WScript.Shell")
WshShell.CurrentDirectory = "C:\Users\jv05g\VoiceDictation"
WshShell.Run """C:\Users\jv05g\VoiceDictation\VoiceDictation.exe"" --silent", 0, False
