# Gather MinGW runtime DLLs next to the built executable so it can run
# outside the MSYS2 shell. Walks the PE import table recursively with
# objdump and copies anything that lives under <MingwBin>.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\gather_dlls.ps1 `
#       -Exe build\NodeNetwork.exe [-MingwBin C:\msys64\mingw64\bin]
#
# CMake invokes this automatically as a POST_BUILD step on MinGW builds.

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [string]$MingwBin = "C:\msys64\mingw64\bin"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Exe)) {
    Write-Error "Exe not found: $Exe"
}
if (-not (Test-Path $MingwBin)) {
    Write-Error "MinGW bin dir not found: $MingwBin"
}

$objdump = Join-Path $MingwBin "objdump.exe"
if (-not (Test-Path $objdump)) {
    Write-Error "objdump.exe not found at $objdump. Install mingw-w64-x86_64-binutils."
}

$outDir  = Split-Path -Parent (Resolve-Path $Exe)
$seen    = @{}
$queue   = New-Object System.Collections.Queue
$queue.Enqueue((Resolve-Path $Exe).Path)
$copied  = 0

while ($queue.Count -gt 0) {
    $current = $queue.Dequeue()
    $imports = & $objdump -p $current 2>$null |
        Select-String '^\s*DLL Name:\s*(.+)$' |
        ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }

    foreach ($imp in $imports) {
        $key = $imp.ToLowerInvariant()
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true

        $src = Join-Path $MingwBin $imp
        if (Test-Path $src) {
            $dst = Join-Path $outDir $imp
            $needs = $true
            if (Test-Path $dst) {
                $a = (Get-Item $src).LastWriteTime
                $b = (Get-Item $dst).LastWriteTime
                if ($a -le $b) { $needs = $false }
            }
            if ($needs) {
                Copy-Item -Force $src $dst
                Write-Host "  copied $imp"
                $copied++
            }
            $queue.Enqueue($src)
        }
        # System DLLs (KERNEL32, USER32, ...) are not under MingwBin —
        # ignore them; Windows resolves those from System32.
    }
}

Write-Host "gather_dlls: $copied DLL(s) copied to $outDir"
