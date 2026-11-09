<#
.SYNOPSIS
    The release bundle for Google Play (.aab), built here and signed with an upload key.

.DESCRIPTION
    As release.yml makes its APK: the same options, the licenses among the package's assets,
    then the bundle, signed by jarsigner. Needs a configure preset of one's own for Qt for
    Android (CMakeUserPresets.json; README, "Android"). The bundle lands in the build folder as
    ratcast-<version>.aab.

    The passwords are asked for, or taken from RATCAST_KEYSTORE_PASSWORD and
    RATCAST_KEY_PASSWORD; they reach the tools through the environment, never the command line.

.EXAMPLE
    .\app\android\release-aab.ps1 -Keystore C:\keys\upload.jks

.EXAMPLE
    .\app\android\release-aab.ps1 -Keystore C:\keys\upload.jks -Version v1.0.15
#>
[CmdletBinding()]
param(
    # The keystore holding the upload key.
    [Parameter(Mandatory = $true)][string]$Keystore,
    # The key's alias; none: the keystore's only key.
    [string]$Alias,
    # The version, like v1.2.3, Google Play's version code made of it (app/CMakeLists.txt);
    # none: the latest tag's. Each upload needs a code of its own.
    [string]$Version,
    [string]$Preset = "android-app-local",
    [string]$BuildDir = "build/android-app-release",
    [string]$CMake = "C:/Program Files/CMake/bin/cmake.exe"
)

$ErrorActionPreference = "Stop"

function Invoke-Tool {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $Program) failed ($LASTEXITCODE)" }
}

function Get-ToolOutput {
    param([string]$Program, [string[]]$Arguments)
    # What a tool writes to stderr (keytool's warnings) must not stop the script.
    $ErrorActionPreference = "Continue"
    & $Program @Arguments 2>&1 | ForEach-Object { "$_" }
}

function Find-JdkTool {
    param([string]$Name)
    if ($env:JAVA_HOME) {
        $inHome = Join-Path $env:JAVA_HOME "bin/$Name.exe"
        if (Test-Path $inHome) { return $inHome }
    }
    $found = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $found) { throw "$Name not found: set JAVA_HOME or put the JDK on PATH" }
    return $found.Source
}

function Read-Secret {
    param([string]$Prompt)
    $secure = Read-Host -Prompt $Prompt -AsSecureString
    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer) }
}

function Get-CacheValue {
    param([string]$Name)
    $line = Select-String -Path (Join-Path $BuildDir "CMakeCache.txt") -Pattern "^$Name`:[A-Z]+=(.*)$"
    if (-not $line) { throw "$Name is not in the build's cache" }
    return $line.Matches[0].Groups[1].Value
}

$root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
Set-Location $root
if (-not (Test-Path $Keystore)) { throw "No keystore at $Keystore" }
$Keystore = (Resolve-Path $Keystore).Path
$keytool = Find-JdkTool "keytool"
$jarsigner = Find-JdkTool "jarsigner"

# Asked first: the build takes a while, and a wrong password should not wait for it.
$hadStorePassword = [bool]$env:RATCAST_KEYSTORE_PASSWORD
$hadKeyPassword = [bool]$env:RATCAST_KEY_PASSWORD
try {
    if (-not $hadStorePassword) { $env:RATCAST_KEYSTORE_PASSWORD = Read-Secret "Keystore password" }
    if (-not $hadKeyPassword) {
        $keyPassword = ""
        if (-not $hadStorePassword) { $keyPassword = Read-Secret "Key password (Enter: the same)" }
        if ($keyPassword) { $env:RATCAST_KEY_PASSWORD = $keyPassword }
        else { $env:RATCAST_KEY_PASSWORD = $env:RATCAST_KEYSTORE_PASSWORD }
    }

    $listing = Get-ToolOutput $keytool @(
        "-J-Duser.language=en", "-list", "-keystore", $Keystore,
        "-storepass:env", "RATCAST_KEYSTORE_PASSWORD"
    )
    if ($LASTEXITCODE -ne 0) { throw "The keystore did not open: $($listing -join ' ')" }
    if (-not $Alias) {
        $keys = @($listing | Where-Object { $_ -match "PrivateKeyEntry" } |
            ForEach-Object { ($_ -split ",")[0].Trim() })
        if ($keys.Count -ne 1) { throw "Say which key with -Alias; the keystore has: $($keys -join ', ')" }
        $Alias = $keys[0]
    }
    Write-Host "Signing with key '$Alias'"

    $configure = @(
        "--preset", $Preset, "-B", $BuildDir,
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        "-DRATCAST_BUILD_TESTS=OFF", "-DRATCAST_BUILD_TOOLS=OFF",
        "-DRATCAST_WITH_FFMPEG=ON", "-DRATCAST_WITH_OPUS=ON", "-DRATCAST_WITH_WEBRTC_APM=ON"
    )
    # A version given once stays in the cache: without one, the tag's again.
    if ($Version) { $configure += "-DRATCAST_VERSION=$Version" } else { $configure += "-URATCAST_VERSION" }
    Invoke-Tool $CMake $configure

    # The licenses among the package's assets, as in release.yml.
    $licenses = Join-Path $BuildDir "app/android/assets/licenses"
    New-Item -ItemType Directory -Force $licenses | Out-Null
    Copy-Item LICENSE (Join-Path $licenses "ratcast.txt")
    $installed = Join-Path (Get-CacheValue "VCPKG_INSTALLED_DIR") (Get-CacheValue "VCPKG_TARGET_TRIPLET")
    $copyrights = @(Get-ChildItem (Join-Path $installed "share/*/copyright"))
    if ($copyrights.Count -eq 0) { throw "No licenses under $installed" }
    foreach ($copyright in $copyrights) {
        Copy-Item $copyright.FullName (Join-Path $licenses "$($copyright.Directory.Name).txt")
    }

    $outputs = Join-Path $BuildDir "app/android-build/build/outputs/bundle"
    if (Test-Path $outputs) { Remove-Item -Recurse -Force $outputs }
    if (-not $env:GRADLE_OPTS) { $env:GRADLE_OPTS = "-Dorg.gradle.daemon=false" }
    Invoke-Tool $CMake @("--build", $BuildDir, "--target", "aab")
    # The debug bundle is made beside it, in a folder of its own.
    $unsigned = @(Get-ChildItem (Join-Path $outputs "release") -Filter *.aab)
    if ($unsigned.Count -ne 1) { throw "Expected one release bundle under $outputs, found $($unsigned.Count)" }

    $settings = Get-Content (Join-Path $BuildDir "app/android-ratcast_app-deployment-settings.json") -Raw |
        ConvertFrom-Json
    $name = $settings."android-version-name"
    $signed = Join-Path (Resolve-Path $BuildDir).Path "ratcast-$name.aab"
    if (Test-Path $signed) { Remove-Item -Force $signed }
    Invoke-Tool $jarsigner @(
        "-keystore", $Keystore,
        "-storepass:env", "RATCAST_KEYSTORE_PASSWORD",
        "-keypass:env", "RATCAST_KEY_PASSWORD",
        "-signedjar", $signed,
        $unsigned[0].FullName, $Alias
    )

    $verified = Get-ToolOutput $jarsigner @("-J-Duser.language=en", "-verify", $signed)
    if (-not ($verified -match "jar verified")) { throw "The signature did not verify: $($verified -join ' ')" }

    Write-Host ""
    Write-Host "Signed: $signed"
    Write-Host "Version $name, code $($settings.'android-version-code')"
}
finally {
    if (-not $hadStorePassword) { Remove-Item Env:RATCAST_KEYSTORE_PASSWORD -ErrorAction SilentlyContinue }
    if (-not $hadKeyPassword) { Remove-Item Env:RATCAST_KEY_PASSWORD -ErrorAction SilentlyContinue }
}
