param(
    [switch]$Silent   # mantido por compatibilidade; a tarefa sempre sobe em segundo plano
)

$ErrorActionPreference = 'Stop'

$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$exePath = Join-Path $dir 'VoiceDictation.exe'

if (-not (Test-Path $exePath)) {
    throw "Executavel nativo nao encontrado: $exePath"
}

$action = New-ScheduledTaskAction -Execute $exePath `
    -Argument '--silent' -WorkingDirectory $dir

$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME

$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -MultipleInstances IgnoreNew -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1)

Register-ScheduledTask -TaskName 'VoiceDictationLive' -Action $action `
    -Trigger $trigger -Settings $settings -Force | Out-Null

Start-ScheduledTask -TaskName 'VoiceDictationLive'

Write-Output "[OK] VoiceDictationLive registrada (nativo C++, inicia no logon, reinicia se falhar) e iniciada."
