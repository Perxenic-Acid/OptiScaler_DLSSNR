param(
    [ValidateSet('Release', 'ReleaseDebug')][string]$Configuration = 'Release',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw '未找到 Visual Studio Installer 的 vswhere.exe。' }
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsRoot) { throw '未找到 MSBuild，请安装 Visual Studio C++ 桌面开发工具。' }
$msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe'
$buildRoot = Join-Path $repoRoot 'build\ssmt'
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null

if (-not $SkipTests) {
    $tests = Join-Path $repoRoot 'build\menu-localization'
    & cmake -S (Join-Path $repoRoot 'tests\menu-localization') -B $tests -A x64
    if ($LASTEXITCODE -ne 0) { throw '界面测试配置失败。' }
    & cmake --build $tests --config Release --parallel 1
    if ($LASTEXITCODE -ne 0) { throw '界面测试构建失败。' }
    & ctest --test-dir $tests -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw '界面测试失败。' }
}

# 上游项目默认 /MP；只限制 MSBuild 项目并行数仍会同时启动大量编译器、耗尽 PCH 内存。
$options = Join-Path $buildRoot 'single-compiler.targets'
@'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup>
    <ClCompile><MultiProcessorCompilation>false</MultiProcessorCompilation></ClCompile>
  </ItemDefinitionGroup>
</Project>
'@ | Set-Content -LiteralPath $options -Encoding utf8

$log = Join-Path $buildRoot 'build.log'
Push-Location $repoRoot
try {
    & $msbuild 'OptiScaler.sln' /m:1 "/p:Configuration=$Configuration" /p:Platform=x64 `
        "/p:ForceImportBeforeCppTargets=$options" /v:minimal /fl "/flp:logfile=$log;verbosity=normal"
    if ($LASTEXITCODE -ne 0) { throw "OptiScaler 构建失败，日志：$log" }
} finally { Pop-Location }

$dll = Join-Path $repoRoot "x64\$Configuration\a\OptiScaler.dll"
if (-not (Test-Path -LiteralPath $dll)) { throw "未找到构建产物：$dll" }
$revision = (& git -C $repoRoot rev-parse HEAD).Trim()
$dirty = [bool](& git -C $repoRoot status --porcelain)
$manifest = [ordered]@{
    repository = 'https://github.com/Perxenic-Acid/OptiScaler_DLSSNR'
    revision = $revision
    dirty = $dirty
    configuration = $Configuration
    language = @('auto', 'zh-cn', 'en')
    file = 'OptiScaler.dll'
    size = (Get-Item -LiteralPath $dll).Length
    sha256 = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $buildRoot 'artifact.json') -Encoding utf8
Write-Host "构建完成：$dll"
Write-Host "来源与哈希：$(Join-Path $buildRoot 'artifact.json')"
Write-Host '仅生成构建产物，不安装到游戏目录。'
