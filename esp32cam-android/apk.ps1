Remove-Item -Recurse -Force build

cmake -G "Ninja" -DCMAKE_TOOLCHAIN_FILE="$env:ANDROID_NDK_HOME\build\cmake\android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release -S . -B build

# Auto-detect build-tools & platform
$buildTools = (Get-ChildItem "$env:ANDROID_SDK_ROOT\build-tools" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$platformDir = (Get-ChildItem "$env:ANDROID_SDK_ROOT\platforms" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$androidJar = "$platformDir\android.jar"
Write-Host "Build-tools: $buildTools"
Write-Host "Platform:    $platformDir"

# Paths inside FetchContent tree
$cascadeSource = "build/_deps/opencv-src/sdk/etc/haarcascades/haarcascade_frontalface_default.xml"

# Prepare assets
New-Item -ItemType Directory -Force -Path "assets" | Out-Null
New-Item -ItemType Directory -Force -Path "lib/arm64-v8a" | Out-Null

# Copy Haar cascade into assets
if (Test-Path $cascadeSource) {
    Copy-Item $cascadeSource "assets/haarcascade_frontalface_default.xml" -Force
    Write-Host "Copied Haar cascade to assets/"
} else {
    Write-Warning "Haar cascade not found at $cascadeSource"
}

# 1. Link APK
& "$buildTools\aapt2.exe" link `
  -o base.apk `
  -I $androidJar `
  -A assets `
  --min-sdk-version 24 `
  --target-sdk-version 33 `
  --version-code 1 `
  --version-name 1.0 `
  --manifest AndroidManifest.xml
if ($LASTEXITCODE -ne 0) { Write-Error "aapt2 link failed"; exit 1 }

# 2. Inject ONLY our library (OpenCV is now statically linked inside it)
Copy-Item "build/libesp32cam.so" "lib/arm64-v8a/" -Force

$jarCmd = Get-Command jar -ErrorAction SilentlyContinue
if ($jarCmd) {
    jar uf base.apk lib/arm64-v8a/libesp32cam.so
} else {
    $sevenZip = Get-Command 7z -ErrorAction SilentlyContinue
    if ($sevenZip) {
        7z a base.apk lib\arm64-v8a\* > $null
    } else {
        Write-Error "Neither 'jar' nor '7z' found in PATH."; exit 1
    }
}

# 3. Align for 16 KB page size
& "$buildTools\zipalign.exe" -f -v -P 16 4 base.apk esp32cam.apk
if ($LASTEXITCODE -ne 0) { Write-Error "zipalign failed"; exit 1 }

# 4. Sign
if (-not (Test-Path "debug.keystore")) {
    keytool -genkey -v -keystore debug.keystore -storepass android -alias androiddebugkey -keypass android -keyalg RSA -validity 10000 -dname "CN=Android Debug,O=Android,C=US"
}
& "$buildTools\apksigner.bat" sign --ks debug.keystore --ks-pass pass:android --key-pass pass:android esp32cam.apk
if ($LASTEXITCODE -ne 0) { Write-Error "apksigner failed"; exit 1 }

Write-Host "`nSUCCESS: esp32cam.apk created!" -ForegroundColor Green
Write-Host "Install: adb install -r esp32cam.apk"
Write-Host "Logcat:  adb logcat -s ESP32Cam:D *:S"

adb devices
adb install -r esp32cam.apk
