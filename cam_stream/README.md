# ESP32-CAM Live Stream to Browser

A simple ESP-IDF project that turns an ESP32 with an OV2640 camera into a WiFi-enabled live video streamer. The video feed is accessible from any browser on the same network — no app or plugin required.

---

## What This Project Does

- Initializes an OV2640 camera on an Ai-Thinker ESP-32S style board
- Connects the ESP32 to your WiFi network (station mode)
- Runs an HTTP server on port 80
- Serves a live MJPEG stream at `/stream`
- Serves a simple HTML viewer at `/` that displays the stream

When you open the ESP32's IP address in a browser, you see the live camera feed.

---

## Hardware Requirements

- ESP32 board with an OV2640 camera module (Ai-Thinker ESP-32S layout)
- USB-to-serial adapter or onboard USB for flashing
- 2.4 GHz WiFi network (ESP32 does not support 5 GHz)

---

## Software Requirements

- **ESP-IDF v6.0.1** (other v5.x/v6.x versions may work with minor changes)
- **Python 3.13** (bundled with ESP-IDF installation)
- **Git**
- **PowerShell** (Windows)
- **esp32-camera** component (cloned into the project's `components/` folder)

---

## Project Structure

```
cam_stream/
├── CMakeLists.txt           # Project-level CMake
├── sdkconfig.defaults       # Default build config
├── partitions.csv           # Flash partition layout
├── components/
│   └── esp32-camera/        # Cloned camera driver
└── main/
    ├── CMakeLists.txt       # Main component requirements
    └── cam_stream.c         # Application source
```

---

## Setup Instructions

### 1. Set Up ESP-IDF Environment

In PowerShell, activate ESP-IDF:

```powershell
$env:IDF_PYTHON_ENV_PATH = "C:\Users\<user>\.espressif\python_env\idf6.0_py3.13_env"
$env:PATH = "$env:IDF_PYTHON_ENV_PATH\Scripts;$env:PATH"
. C:\esp\v6.0.1\esp-idf\export.ps1
```

Adjust paths to match your ESP-IDF installation. Verify with:

```powershell
idf.py --version
```

### 2. Clone the Camera Component

From the project root:

```powershell
mkdir components
cd components
git clone https://github.com/espressif/esp32-camera.git
cd ..
```

### 3. Configure WiFi Credentials

Edit `main/config.h` and set your network details:

```c
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
```

Make sure the correct camera model is selected:

```c
#define CAMERA_MODEL_AI_THINKER
// #define CAMERA_MODEL_ESP_EYE
// #define CAMERA_MODEL_ESP32S3_EYE
```

### 4. Enable PSRAM (Required for Camera Frame Buffer)

The camera needs PSRAM to buffer frames. Open menuconfig:

```powershell
idf.py menuconfig
```

Navigate to:

- `Component config` → `ESP32-specific` → `Support for external, SPI-connected RAM` → enable it
- Then inside `SPI RAM config`, set `SPI RAM access method` to `Make RAM allocatable using malloc()`

Save and exit (`Q`, then `Y`).

Alternatively, add these lines to `sdkconfig.defaults` to avoid the interactive menu:

```
CONFIG_ESP32_SPIRAM_SUPPORT=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_TYPE_AUTO=y
```

Then run `idf.py menuconfig` once so ESP-IDF generates the full `sdkconfig`.

---

## Build and Flash

### Set the target (first time only)

```powershell
idf.py set-target esp32
```

### Build

```powershell
idf.py build
```

### Find your COM port

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

### Flash (replace COM4 with your port)

```powershell
idf.py -p COM4 flash
```

### Monitor serial output

```powershell
idf.py -p COM4 monitor
```

Exit the monitor with `Ctrl+]`.

---

## Using the Stream

Once the ESP32 boots and connects to WiFi, the serial monitor prints:

```
I (xxxx) cam_stream: Camera initialized successfully
I (xxxx) cam_stream: Got IP: 192.168.1.42
I (xxxx) cam_stream: HTTP server started on port 80
```

Open a browser on any device on the same WiFi network and go to:

```
http://192.168.1.42/
```

The page shows the live camera feed. You can also hit the stream endpoint directly:

```
http://192.168.1.42/stream
```

This URL works directly in VLC, ffmpeg, or any MJPEG-compatible viewer.

---

## How It Works

- `init_camera()` configures the OV2640 over the ESP32's I2S/LEDC peripheral using the AI-Thinker pin mapping.
- `init_wifi()` sets up WiFi station mode and registers event handlers for connection and IP assignment.
- Once an IP is obtained, `start_http_server()` launches `esp_http_server` on port 80.
- `/` returns a minimal HTML page with an `<img>` tag pointing to `/stream`.
- `/stream` uses `multipart/x-mixed-replace` — the browser treats each JPEG as a new frame of an ongoing image, producing live video.
- Frames are grabbed with `esp_camera_fb_get()` and sent as HTTP chunks with a short delay between them.

---

## Tuning Tips

- **Frame rate**: Lower the `vTaskDelay` in `stream_handler` (currently 50 ms ≈ 20 FPS target).
- **Resolution**: Change `config.frame_size` (`FRAMESIZE_SVGA` = 800×600). Options: `FRAMESIZE_VGA`, `FRAMESIZE_CIF`, `FRAMESIZE_QVGA`, etc.
- **Quality**: `config.jpeg_quality` — lower number = higher quality, larger frames (range 0–63).
- **Frame buffers**: `config.fb_count = 2` allows double buffering for smoother streaming.

---

## Troubleshooting

| Symptom | Likely Cause / Fix |
| --- | --- |
| `Camera init failed with error 0xffffffff` | PSRAM not enabled — see Step 4 |
| `frame buffer malloc failed` | Same as above, or frame size too large for available PSRAM |
| `Camera capture failed` in loop | Wrong camera pin mapping — check `CAMERA_MODEL_*` define |
| No `Got IP` line | Wrong WiFi credentials, or 5 GHz network |
| Browser shows nothing | ESP32 and browser device on different networks, or firewall blocking port 80 |
| `Failed to resolve component 'wifi'` | Use `esp_wifi` instead of `wifi` in `main/CMakeLists.txt` |
| `Failed to resolve component 'esp32-camera'` | `components/esp32-camera` folder missing — re-run Step 2 |

---

## Notes

- Only **2.4 GHz WiFi** is supported by the ESP32.
- Browsers limit concurrent MJPEG streams — avoid opening the stream in many tabs at once.
- For remote access outside your LAN, put the ESP32 behind a reverse proxy or VPN; do not expose port 80 directly to the internet.
- The Ai-Thinker ESP32-CAM needs a stable 5V supply — brownouts cause camera init failures and random resets.
