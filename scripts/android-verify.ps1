param(
    [string]$JavaHome = "D:\soft\AS\jbr",
    [string]$AndroidSdk = "D:\soft\AS_sdk",
    [string]$DeviceSerial = "",
    [switch]$Install
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Gradlew = Join-Path $RepoRoot "gradlew.bat"
$ApkPath = Join-Path $RepoRoot "android\app\build\outputs\apk\debug\app-debug.apk"

if (-not (Test-Path $Gradlew)) {
    throw "gradlew.bat not found: $Gradlew"
}
if (-not (Test-Path $JavaHome)) {
    throw "JavaHome not found: $JavaHome"
}
if (-not (Test-Path $AndroidSdk)) {
    throw "AndroidSdk not found: $AndroidSdk"
}

$KotlinFiles = Get-ChildItem -Path $RepoRoot -Recurse -File -Include *.kt |
    Where-Object { $_.FullName -notmatch "\\build\\" }
if ($KotlinFiles.Count -gt 0) {
    $KotlinFiles | ForEach-Object { Write-Host $_.FullName }
    throw "Android side forbids Kotlin, but .kt files were found."
}

$env:JAVA_HOME = $JavaHome
$env:ANDROID_HOME = $AndroidSdk
$env:ANDROID_SDK_ROOT = $AndroidSdk
$env:PATH = "$JavaHome\bin;$AndroidSdk\platform-tools;$env:PATH"

Push-Location $RepoRoot
try {
    & $Gradlew ":android:app:assembleDebug" "--offline" "--no-daemon"
    if ($LASTEXITCODE -ne 0) {
        throw "Gradle build failed with exit code $LASTEXITCODE"
    }

    if (-not (Test-Path $ApkPath)) {
        throw "APK not found: $ApkPath"
    }

    $Apk = Get-Item $ApkPath
    Write-Host "APK: $($Apk.FullName)"
    Write-Host "Size: $($Apk.Length) bytes"
    Write-Host "LastWriteTime: $($Apk.LastWriteTime)"

    if ($Install) {
        $AdbArgs = @()
        if ($DeviceSerial.Trim().Length -gt 0) {
            $AdbArgs += @("-s", $DeviceSerial)
        }
        $AdbArgs += @("install", "-r", $ApkPath)
        & adb @AdbArgs
        if ($LASTEXITCODE -ne 0) {
            throw "adb install failed with exit code $LASTEXITCODE"
        }
    }
} finally {
    Pop-Location
}
