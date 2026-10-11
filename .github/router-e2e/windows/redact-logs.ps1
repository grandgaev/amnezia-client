param(
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [Parameter(Mandatory = $true)]
    [string]$Destination,
    [string]$ServerAddress = ''
)

$content = if (Test-Path $Source) { Get-Content -Raw $Source } else { '' }
if ($ServerAddress) {
    $content = $content -replace [regex]::Escape($ServerAddress), '[routing-lab-server]'
}
$content = $content -replace '(?<![A-Za-z0-9+/])[A-Za-z0-9+/]{43}=(?![A-Za-z0-9+/])', '[base64-key]'
$content = $content -replace '(?i)(?<![0-9a-f])[0-9a-f]{64}(?![0-9a-f])', '[hex-key]'
[IO.File]::WriteAllText($Destination, $content, [Text.UTF8Encoding]::new($false))
