# coronet MSVC build helper — sets up C++ dev environment without vcvars64.bat
$ErrorActionPreference = "Continue"

$VSPATH = "D:\dev\tools\VisualStudio\2022\Community"
$MSVC = "$VSPATH\VC\Tools\MSVC\14.41.34120"
$SDK = "C:\Program Files (x86)\Windows Kits\10"
$SDKVER = "10.0.26100.0"

# INCLUDE
$env:INCLUDE = "$MSVC\include;" +
               "$MSVC\atlmfc\include;" +
               "$SDK\Include\$SDKVER\ucrt;" +
               "$SDK\Include\$SDKVER\um;" +
               "$SDK\Include\$SDKVER\shared;" +
               "$SDK\Include\$SDKVER\winrt;" +
               "$SDK\Include\$SDKVER\cppwinrt"

# LIB
$env:LIB = "$MSVC\lib\x64;" +
           "$MSVC\atlmfc\lib\x64;" +
           "$SDK\Lib\$SDKVER\ucrt\x64;" +
           "$SDK\Lib\$SDKVER\um\x64"

# PATH (prepend MSVC bin)
$env:PATH = "$MSVC\bin\Hostx64\x64;$VSPATH\Common7\IDE;$env:PATH"

Write-Host "MSVC environment configured:"
Write-Host "  INCLUDE entries: $(($env:INCLUDE -split ';' | Where-Object {$_}).Count)"
Write-Host "  LIB entries:     $(($env:LIB -split ';' | Where-Object {$_}).Count)"

# Verify stdint.h is findable
foreach ($p in ($env:INCLUDE -split ';' | Where-Object {$_})) {
    if (Test-Path "$p\stdint.h") {
        Write-Host "  stdint.h found in: $p"
        break
    }
}
