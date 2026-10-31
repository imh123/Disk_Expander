param(
    [string]$Dist = "dist\Disk_Expander-0.3.0-win64",
    [string]$Output = "dist\SHA256SUMS.txt"
)

$ErrorActionPreference = "Stop"

$distPath = (Resolve-Path -LiteralPath $Dist).Path
$lines = Get-ChildItem -LiteralPath $distPath -File -Recurse |
    Sort-Object FullName |
    ForEach-Object {
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $relative = $_.FullName.Substring($distPath.Length + 1).Replace('\', '/')
        "$hash  $relative"
    }

$lines | Set-Content -LiteralPath $Output -Encoding ascii
Write-Host "Wrote $Output"
