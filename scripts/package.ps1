# 打包闪寻：生成 setup.exe、portable.zip 与 SHA256SUMS（静态 CRT，单 exe，无需运行库）
param(
    [string]$BuildDirectory = 'build',
    [string]$Iscc = '',
    [string]$OutputDirectory = 'dist'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$version = ((Select-String -Path CMakeLists.txt -Pattern 'project\(FlashFind VERSION ([0-9.]+)').Matches[0].Groups[1].Value)
$arch = 'x64'
$name = "FlashFind-$version-windows-$arch"
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$out = (Resolve-Path $OutputDirectory).Path
$zip = Join-Path $out "$name-portable.zip"
$setup = Join-Path $out "$name-setup.exe"
foreach ($f in @($zip, $setup)) { if (Test-Path $f) { Remove-Item $f } }

$exe = Join-Path $BuildDirectory 'FlashFind.exe'
if (!(Test-Path $exe)) { throw "Missing $exe; build Release first" }

# 安装文件暂存
$stage = Join-Path $out ("staging-" + [Guid]::NewGuid().ToString('N'))
$payload = Join-Path $stage $name
New-Item -ItemType Directory -Force $payload | Out-Null
Copy-Item $exe $payload
Copy-Item LICENSE, README.md, THIRD_PARTY_NOTICES.md $payload
$rev = (git rev-parse HEAD).Trim()
"Version: $version`r`nArchitecture: $arch`r`nSource revision: $rev" | Set-Content (Join-Path $payload 'BUILD.txt') -Encoding UTF8

# 检查二进制为 x64，且不依赖 VC++ 运行库 DLL（静态 CRT）
foreach ($bin in (Get-ChildItem $payload -Recurse -File -Filter *.exe)) {
    $bytes = [IO.File]::ReadAllBytes($bin.FullName)
    $pe = [BitConverter]::ToInt32($bytes, 0x3c)
    if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) { throw "Not x64: $($bin.FullName)" }
    $ascii = [Text.Encoding]::ASCII.GetString($bytes)
    foreach ($dll in 'VCRUNTIME140.dll', 'MSVCP140.dll') { if ($ascii.IndexOf($dll, [StringComparison]::OrdinalIgnoreCase) -ge 0) { throw "Unexpected runtime dependency $dll" } }
}

Compress-Archive -Path $payload -DestinationPath $zip -CompressionLevel Optimal

if (!$Iscc) {
    $cands = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")
    if ($env:ISCC) { $cands = @($env:ISCC) + $cands }
    $Iscc = $cands | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}
if (!$Iscc) { throw 'Inno Setup 6 (ISCC.exe) not found; pass -Iscc or set $env:ISCC' }
& $Iscc "/DAppSourceDir=$payload" "/DAppVersion=$version" "/DAppArchitecture=$arch" "/DOutputDirectory=$out" installer/FlashFind.iss | Out-Host
if ($LASTEXITCODE -ne 0) { throw "ISCC failed: $LASTEXITCODE" }
Remove-Item -Recurse -Force $stage

$sums = foreach ($f in @($setup, $zip)) { "{0}  {1}" -f (Get-FileHash $f -Algorithm SHA256).Hash.ToLower(), (Split-Path $f -Leaf) }
$sums | Set-Content (Join-Path $out "SHA256SUMS-$version-$arch.txt") -Encoding ASCII
$sums
