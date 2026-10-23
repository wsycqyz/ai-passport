# tools/vocab_audio/gen_tts.ps1 - offline TTS for the Word Buddy test-bank clips (spec 8.6).
#
# Speaks the English text of every entry of main/vocab_bank.c with the Windows System.Speech
# desktop voice "Microsoft Zira Desktop" (Rate -2, Volume 100) into one 16 kHz / 16-bit /
# mono WAV file per uid. Fully offline: no network access, no cloud TTS. The script fails
# loudly (exit code 1) when the voice is missing or disabled; it never substitutes another
# voice.
#
# The WAV files go to a local cache that is NOT committed (default build/vocab_tts/, ignored
# by .gitignore). tools/vocab_audio/build_clips.py then trims, normalizes and encodes them
# into the LOCAL clip pack assets/audio/vocab_test_clips.bin and its manifest
# assets/audio/vocab_test_clips.csv, both git-ignored (amendment A6). The clips are
# TEST-ONLY: the voice is licensed with Windows, so they stay out of git and firmware
# containing them stays private and must never be published. CI and public builds embed the
# committed empty pack assets/audio/vocab_clips_empty.bin instead (no word audio).
#
# Usage (Windows PowerShell 5.1, from the repository root):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\vocab_audio\gen_tts.ps1
#   python tools\vocab_audio\build_clips.py build
#   python tools\vocab_audio\build_clips.py --check
#   idf.py reconfigure   (an existing build directory picks the pack up at configure time)
#
# Outputs in -OutDir: <uid>.wav per bank entry, tts_list.tsv (uid, text, WAV SHA-256) and
# tts_info.txt (voice, engine files, Windows build, tool versions) for the manifest.
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$VoiceName = 'Microsoft Zira Desktop'
$Rate = -2
$Volume = 100
$SampleRate = 16000
$ScriptVersion = '1.0'

function Fail([string]$Message) {
    [Console]::Error.WriteLine("gen_tts.ps1: ERROR: $Message")
    exit 1
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

if ($OutDir -eq '') { $OutDir = Join-Path $RepoRoot 'build\vocab_tts' }
$bankPath = Join-Path $RepoRoot 'main\vocab_bank.c'
if (-not (Test-Path -LiteralPath $bankPath)) { Fail "bank source not found: $bankPath" }

# Bank entries: CLOZE_ENTRY(uid, category, "text", ...) / PLAIN_ENTRY(uid, category, "text", ...).
# build_clips.py parses the same file independently and rejects a cache whose uid/text list
# differs from the bank.
$source = [System.IO.File]::ReadAllText($bankPath, [System.Text.Encoding]::UTF8)
$pattern = '(?:CLOZE_ENTRY|PLAIN_ENTRY)\(\s*(\d+)\s*,\s*\d+\s*,\s*"((?:[^"\\\r\n]|\\.)*)"'
$entries = @()
foreach ($m in [regex]::Matches($source, $pattern)) {
    $text = $m.Groups[2].Value
    if ($text.Contains('\')) { Fail "escape sequences are not supported in bank text: $text" }
    if ($text -notmatch '^[\x20-\x7E]+$') { Fail "bank text must be printable ASCII: $text" }
    $entries += [pscustomobject]@{ Uid = [int]$m.Groups[1].Value; Text = $text }
}
if ($entries.Count -eq 0) { Fail "no CLOZE_ENTRY/PLAIN_ENTRY found in $bankPath" }
$dupes = $entries | Group-Object Uid | Where-Object { $_.Count -gt 1 }
if ($dupes) { Fail ("duplicate uid(s) in the bank: " + (($dupes | ForEach-Object { $_.Name }) -join ', ')) }

Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
try {
    $voice = $synth.GetInstalledVoices() | Where-Object { $_.VoiceInfo.Name -eq $VoiceName }
    if (-not $voice) {
        $names = ($synth.GetInstalledVoices() | ForEach-Object { $_.VoiceInfo.Name }) -join ', '
        Fail "voice '$VoiceName' is not installed (installed: $names). No substitute voice is used."
    }
    if (-not $voice.Enabled) { Fail "voice '$VoiceName' is installed but disabled." }
    $synth.SelectVoice($VoiceName)
    if ($synth.Voice.Name -ne $VoiceName) { Fail "could not select voice '$VoiceName'." }
    $synth.Rate = $Rate
    $synth.Volume = $Volume
    $info = $synth.Voice

    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    Get-ChildItem -LiteralPath $OutDir -Filter '*.wav' | Remove-Item -Force

    $format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
        $SampleRate, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
        [System.Speech.AudioFormat.AudioChannel]::Mono)
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("uid`ttext`twav_sha256")
    foreach ($e in ($entries | Sort-Object Uid)) {
        $wav = Join-Path $OutDir ("{0}.wav" -f $e.Uid)
        $synth.SetOutputToWaveFile($wav, $format)
        $synth.Speak($e.Text)
        $synth.SetOutputToNull()
        if (-not (Test-Path -LiteralPath $wav) -or (Get-Item -LiteralPath $wav).Length -le 44) {
            Fail "no audio written for uid $($e.Uid) ('$($e.Text)')"
        }
        $lines.Add(("{0}`t{1}`t{2}" -f $e.Uid, $e.Text, (Get-Sha256 $wav)))
    }
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText((Join-Path $OutDir 'tts_list.tsv'), (($lines -join "`n") + "`n"), $utf8)

    # Provenance for the manifest (spec 8.6: voice, rate, Windows build).
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $token = Get-ItemProperty ('HKLM:\SOFTWARE\Microsoft\Speech\Voices\Tokens\' + $info.Id) -ErrorAction SilentlyContinue
    $engineFiles = @()
    if ($token -and $token.PSObject.Properties['VoicePath']) {
        $engineFiles += Get-ChildItem -Path ($token.VoicePath + '.*') -File -ErrorAction SilentlyContinue
    }
    if ($token -and $token.PSObject.Properties['LangDataPath']) {
        $engineFiles += Get-Item -LiteralPath $token.LangDataPath -ErrorAction SilentlyContinue
    }
    $speechAsm = [System.Speech.Synthesis.SpeechSynthesizer].Assembly.GetName()
    $infoLines = New-Object System.Collections.Generic.List[string]
    $infoLines.Add("script_version=$ScriptVersion")
    $infoLines.Add("voice=$($info.Name)")
    $infoLines.Add("voice_id=$($info.Id)")
    $infoLines.Add("voice_culture=$($info.Culture.Name)")
    $infoLines.Add("voice_version=$($info.AdditionalInfo['Version'])")
    $infoLines.Add("rate=$Rate")
    $infoLines.Add("volume=$Volume")
    $infoLines.Add("format=$SampleRate Hz, 16-bit, mono PCM")
    $infoLines.Add(("windows_build={0}.{1}.{2}.{3}" -f [Environment]::OSVersion.Version.Major,
        [Environment]::OSVersion.Version.Minor, $cv.CurrentBuild, $cv.UBR))
    $infoLines.Add("windows_display_version=$($cv.DisplayVersion)")
    $infoLines.Add("powershell=$($PSVersionTable.PSVersion) ($($PSVersionTable.PSEdition))")
    $infoLines.Add("system_speech=$($speechAsm.Version)")
    foreach ($f in $engineFiles) {
        $infoLines.Add(("engine_file={0} {1} {2}" -f $f.Name, $f.Length, (Get-Sha256 $f.FullName)))
    }
    [System.IO.File]::WriteAllText((Join-Path $OutDir 'tts_info.txt'), (($infoLines -join "`n") + "`n"), $utf8)
    Write-Output ("gen_tts.ps1: {0} clips written to {1} with '{2}' (rate {3}, volume {4})" -f
        $entries.Count, $OutDir, $info.Name, $Rate, $Volume)
}
finally {
    $synth.Dispose()
}
