param(
    [Parameter(Mandatory = $true)]
    [string] $InputPath,

    [string] $RtmpUrl = "rtmp://127.0.0.1/live/stream"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $InputPath)) {
    throw "Input file not found: $InputPath"
}

$ffmpeg = Get-Command ffmpeg -ErrorAction Stop

& $ffmpeg.Source `
    -re `
    -stream_loop -1 `
    -i $InputPath `
    -c:v libx264 `
    -preset veryfast `
    -tune zerolatency `
    -profile:v baseline `
    -pix_fmt yuv420p `
    -g 60 `
    -bf 0 `
    -c:a aac `
    -ar 48000 `
    -ac 2 `
    -f flv `
    $RtmpUrl
