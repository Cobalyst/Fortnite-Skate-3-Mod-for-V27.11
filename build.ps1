# Builds the mod into bin\: SkateMod.dll, PadReader.exe and skate_engine.dll.
#   powershell -ExecutionPolicy Bypass -File build.ps1
# Needs: Visual Studio 2022 or newer with "Desktop development with C++", Rust (rustup, stable),
# and a Dumper-7 SDK of Fortnite 27.11 in SDK\ (see README.md, "Building from source").
# Release flags: no PDB / debug info, NDEBUG, build paths remapped, so nothing of the build PC ends up in the files.
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Bin = Join-Path $Root 'bin'
$Obj = Join-Path $Root 'obj'

$SdkDir = Get-ChildItem (Join-Path $Root 'SDK') -Directory -ErrorAction SilentlyContinue |
          Where-Object { Test-Path (Join-Path $_.FullName 'CppSDK\SDK\Basic.cpp') } |
          Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $SdkDir) { Write-Host "No Fortnite 27.11 SDK found in $Root\SDK (see README.md)" -ForegroundColor Red; exit 1 }
$Cpp = Join-Path $SdkDir.FullName 'CppSDK'
$MinHook = Join-Path $Root 'third_party\minhook'

$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $VsWhere)) { Write-Host 'Visual Studio not found (vswhere.exe missing)' -ForegroundColor Red; exit 1 }
$VsPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $VsPath) { Write-Host 'Visual Studio with the C++ tools not found' -ForegroundColor Red; exit 1 }
$VsDevCmd = Join-Path $VsPath 'Common7\Tools\VsDevCmd.bat'

New-Item -ItemType Directory -Force "$Obj\sdk", "$Obj\mod", "$Obj\pad", $Bin | Out-Null
$Mp = if ($env:SKATEMOD_JOBS) { "/MP$($env:SKATEMOD_JOBS)" } else { '/MP' }
$Flags = "/nologo /std:c++latest /O2 /MT /EHsc $Mp /bigobj /W1 /DNOMINMAX /DNDEBUG /Brepro /I`"$Cpp`" /I`"$MinHook\include`""
$Commands = @()
$SdkSources = 'Basic.cpp', 'CoreUObject_functions.cpp', 'Engine_functions.cpp', 'ProceduralMeshComponent_functions.cpp', 'EnhancedInput_functions.cpp'
$Missing = $SdkSources | Where-Object { -not (Test-Path "$Obj\sdk\$([IO.Path]::GetFileNameWithoutExtension($_)).obj") }
if ($Missing) {
    $List = ($Missing | ForEach-Object { "`"$Cpp\SDK\$_`"" }) -join ' '
    $Commands += "cl $Flags /c $List /Fo`"$Obj\sdk\\`""
}
$ModSources = (Get-ChildItem "$Root\SkateMod\src\*.cpp" | ForEach-Object { "`"$($_.FullName)`"" }) -join ' '
$MinHookSources = "`"$MinHook\src\buffer.c`" `"$MinHook\src\hook.c`" `"$MinHook\src\trampoline.c`" `"$MinHook\src\hde\hde64.c`""
$Commands += "cl $($Flags -replace '/std:c\+\+latest','') /c $MinHookSources /Fo`"$Obj\mod\\`""
$Commands += "cl $Flags /c $ModSources /Fo`"$Obj\mod\\`""
$Commands += "link /nologo /DLL /RELEASE /Brepro /OUT:`"$Bin\SkateMod.dll`" `"$Obj\mod\*.obj`" `"$Obj\sdk\*.obj`" user32.lib winmm.lib"
# The controller reader (PadReader.exe): every real controller read happens in it, outside the game.
$Commands += "cl /nologo /std:c++latest /O2 /MT /EHsc /W1 /DNOMINMAX /DNDEBUG /Brepro `"$Root\SkateMod\padreader\PadReader.cpp`" /Fo`"$Obj\pad\\`" /Fe`"$Bin\PadReader.exe`" user32.lib shell32.lib winmm.lib /link /SUBSYSTEM:WINDOWS /RELEASE /Brepro"

$ErrorActionPreference = 'Continue'  # compiler warnings go to stderr; only exit codes matter
foreach ($c in $Commands) {
    cmd /c "`"$VsDevCmd`" -arch=x64 -host_arch=x64 -no_logo && $c"
    if ($LASTEXITCODE -ne 0) { Write-Host 'BUILD FAILED' -ForegroundColor Red; exit 1 }
}

# skate_engine.dll (Rust): no debug info, build paths remapped
$env:CARGO_TARGET_DIR = Join-Path $Root 'target'
$env:CARGO_PROFILE_RELEASE_DEBUG = 'false'
$env:CARGO_PROFILE_RELEASE_STRIP = 'true'
$CargoHome = if ($env:CARGO_HOME) { $env:CARGO_HOME } else { Join-Path $env:USERPROFILE '.cargo' }
$RustupHome = if ($env:RUSTUP_HOME) { $env:RUSTUP_HOME } else { Join-Path $env:USERPROFILE '.rustup' }
# (CARGO_ENCODED_RUSTFLAGS: one flag per 0x1F-separated item, so spaces in folder names are fine)
$Sep = [string][char]0x1f
$env:CARGO_ENCODED_RUSTFLAGS = @("--remap-path-prefix=$CargoHome\registry\src=crates", "--remap-path-prefix=$CargoHome=cargo",
    "--remap-path-prefix=$RustupHome=rust", "--remap-path-prefix=$Root=src", '-C', 'link-arg=/Brepro') -join $Sep
Remove-Item Env:RUSTFLAGS -ErrorAction SilentlyContinue
Push-Location (Join-Path $Root 'SkateEngine')
cargo build --release
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { Write-Host 'ENGINE BUILD FAILED' -ForegroundColor Red; exit 1 }
Copy-Item (Join-Path $Root 'target\release\skate_engine.dll') $Bin -Force
Write-Host "Built $Bin (SkateMod.dll, PadReader.exe, skate_engine.dll)" -ForegroundColor Green
