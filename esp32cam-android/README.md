# ESP32-CAM Live Stream + Android Face Detection

A pure **C++ Android NativeActivity app** that connects to an ESP32-CAM over WiFi, receives its live MJPEG camera stream, and performs real-time **face detection** using OpenCV — with **no Gradle, no IDE, no Java/Kotlin**. The entire build is done from the command line with CMake + Ninja, and the APK is packaged and signed manually with Android SDK build-tools.

## How It Works

```
┌──────────────┐   WiFi (HTTP MJPEG stream, chunked encoding)   ┌───────────────────────┐
│   ESP32-CAM  │ ──────────────────────────────────────────────▶ │   Android Phone       │
│  (camera +   │        GET /stream                             │  ┌─────────────────┐  │
│  HTTP server │ ──────────────────────────────────────────────▶ │  │ Native C++ app  │  │
│  port 80)    │        multipart/x-mixed-replace               │  │ · TCP socket    │  │
└──────────────┘                                               │  │ · Chunked HTTP  │  │
                                                               │  │ · MJPEG parser  │  │
                                                               │  │ · OpenCV Haar   │  │
                                                               │  │   face detect   │  │
                                                               │  │ · ANativeWindow │  │
                                                               │  │   rendering     │  │
                                                               │  └─────────────────┘  │
                                                               └───────────────────────┘
```

- The Android app opens a raw TCP socket to the ESP32 and issues `GET /stream HTTP/1.1`
- The ESP32 (ESP-IDF `esp_http_server` with `httpd_resp_send_chunk`) replies with a **chunked, multipart MJPEG** stream
- The app parses chunk sizes and multipart boundaries, extracts JPEG frames, and decodes them with OpenCV
- Each frame is converted to grayscale, run through a **Haar cascade face classifier**, and faces are drawn with green rectangles and labels
- Frames are blitted directly to the screen via `ANativeWindow` — no Java UI layer at all
- OpenCV is **statically linked** into a single `libesp32cam.so`, built with 16 KB page alignment for Android 15+ devices

## Project Structure

```
esp32cam-android/
├── AndroidManifest.xml      # Pure NativeActivity, hasCode="false", INTERNET permission
├── CMakeLists.txt           # Builds libesp32cam.so; FetchContent fetches OpenCV Android SDK
├── apk.ps1                  # Packages, aligns (16 KB), signs the APK manually
├── src/
│   └── main.cpp             # Entire app: socket, HTTP/MJPEG parser, OpenCV, rendering
├── assets/                  # (auto-generated) haarcascade_frontalface_default.xml
└── build/                   # (auto-generated) CMake output + FetchContent OpenCV SDK
```

## Prerequisites

| Requirement | Notes |
|---|---|
| Android SDK | `ANDROID_SDK_ROOT` env var set; platform `android-33`+ recommended |
| Android NDK | r25+, tested with r30 — `ANDROID_NDK_HOME` env var set |
| CMake + Ninja | On PATH |
| JDK | `jar`, `keytool` on PATH (or 7-Zip as `7z` fallback) |
| adb | On PATH, phone with USB debugging enabled |
| OpenCV Android SDK zip | Downloaded manually, extracted by CMake `FetchContent` |
| ESP32-CAM | Already flashed and streaming (see below) |

PowerShell environment variables (Windows):

```powershell
$env:ANDROID_SDK_ROOT = "C:\path\to\android-sdk"
$env:ANDROID_NDK_HOME = "C:\path\to\android-ndk"   # e.g. ...\ndk\30.0.15729638
```

## ESP32 Firmware Requirements

The ESP32 side uses ESP-IDF with a camera + HTTP server. The stream handler must:

- Serve an MJPEG multipart stream on `/stream`, port 80
- Use chunked transfer encoding via `httpd_resp_send_chunk` (standard in the ESP-IDF camera example)
- Send `Content-Type: multipart/x-mixed-replace; boundary=<boundary>`

> **Note:** The stock ESP32-CAM web server typically handles **only one streaming client at a time**. Close any browser tabs viewing the stream before launching the Android app, or increase `config.max_open_sockets` in `httpd_config_t` on the ESP32 side.

The app prints the ESP32 IP to serial on boot — note it down, you'll need it below.

## Setup & Build

### 1. Clone and enter the project

```powershell
cd esp32cam-android
```

### 2. Download the OpenCV Android SDK (one time, ~290 MB)

```powershell
Invoke-WebRequest -Uri "https://github.com/opencv/opencv/releases/download/4.10.0/opencv-4.10.0-android-sdk.zip" -OutFile "opencv-4.10.0-android-sdk.zip"
```

The zip must sit in the project root — CMake `FetchContent` extracts it into `build/_deps/`.

### 3. Configure your ESP32's IP address

Edit `src/main.cpp` near the top:

```cpp
#define ESP32_IP   "192.168.0.132"   // ← your ESP32's IP (from serial monitor)
#define ESP32_PORT 80
```

### 4. Build the native library

```powershell
cmake -G "Ninja" `
  -DCMAKE_TOOLCHAIN_FILE="$env:ANDROID_NDK_HOME\build\cmake\android.toolchain.cmake" `
  -DANDROID_ABI=arm64-v8a `
  -DANDROID_PLATFORM=android-24 `
  -DCMAKE_BUILD_TYPE=Release `
  -S . -B build

cmake --build build
```

This produces `build/libesp32cam.so` with OpenCV statically linked inside it.

### 5. Package, align, and sign the APK

```powershell
.\apk.ps1
```

The script auto-detects your latest build-tools and platform, injects the native library and the Haar cascade asset, aligns for 16 KB page sizes, and signs with a debug keystore. Output: `esp32cam.apk`.

### 6. Install and run

```powershell
adb install -r esp32cam.apk
adb shell am start -n com.example.esp32cam/android.app.NativeActivity
adb logcat -s ESP32Cam:D *:S
```

## Usage

1. Power on the ESP32-CAM and make sure it connects to WiFi
2. Make sure your **phone is on the same WiFi network** as the ESP32
3. Make sure no other device (e.g., a browser) is currently viewing the stream
4. Launch the app — the video appears immediately, with green boxes around detected faces
5. Watch progress in logcat:

```
ESP32Cam: === ESP32Cam Face Detection Starting ===
ESP32Cam: Cascade loaded: YES
ESP32Cam: Connected to ESP32
ESP32Cam: Boundary: --frame
ESP32Cam: Stream started
ESP32Cam: Detected 1 face(s)
```

The stream auto-reconnects every 2 seconds if the connection drops.

## Troubleshooting

| Symptom | Cause / Fix |
|---|---|
| Black screen, "connection reset" (errno 104) | Another client (browser) is holding the ESP32 stream — close it, or raise `max_open_sockets` on the ESP32 |
| Connection timeout | Phone and ESP32 not on the same WiFi, or wrong `ESP32_IP` in `main.cpp` |
| `ALooper_pollAll unavailable` (NDK r30+) | Already handled — code uses `ALooper_pollOnce` |
| `Invalid character escape` in CMake (Windows) | Already handled — CMakeLists converts backslashes to forward slashes |
| 16 KB page-size warning / app won't install (Android 15+) | Already handled — statically linked OpenCV + `-Wl,-z,max-page-size=16384` + `zipalign -P 16` |
| No faces detected | Lighting/angle; try lowering `minNeighbors` or adjusting `minSize` in `detectMultiScale` |

## Tech Notes

- **Pure native**: `android:hasCode="false"` + `NativeActivity`; entry point is `android_main()`
- **OpenCV**: static libs (`core`, `imgproc`, `objdetect`, `imgcodecs`) from `sdk/native/staticlibs` linked into one `.so`; the Haar cascade XML ships as an APK asset and is copied to internal storage at first run (`cv::CascadeClassifier::load` needs a file path)
- **16 KB alignment**: required by Android 15+; enforced at link time and via `zipalign -P 16`
- **Only `arm64-v8a`** is built by default; add other ABIs by re-running CMake with `-DANDROID_ABI=armeabi-v7a` etc. and adjusting `apk.ps1`

## Possible Extensions

- On-screen FPS counter
- Video recording to phone storage
- DNN-based face detector (more accurate than Haar)
- Two-way control channel (send commands from Android → ESP32)
- Multi-camera support

## License

MIT (or adjust to your preference)
