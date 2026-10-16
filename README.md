# ESP32-CAM Live Video Stream with Face Detection

This project is a two-part system that streams live video from an **ESP32-CAM** over WiFi and performs **real-time face detection** on the received video.

## Overview

The ESP32-CAM captures video from its OV2640 camera and serves it as an MJPEG stream over HTTP. That stream can be viewed directly in any browser on the same network — or picked up by a **pure C++ Android app** that decodes each frame with OpenCV and highlights detected faces on screen.

```
┌──────────────┐   WiFi (HTTP MJPEG stream)   ┌──────────────────────┐
│   ESP32-CAM  │ ───────────────────────────▶ │   Browser (any)      │
│  (OV2640     │ ───────────────────────────▶ │   Android C++ app    │
│   + HTTP 80) │                              │  (OpenCV face detect)│
└──────────────┘                              └──────────────────────┘
```

## Part 1: ESP32 Firmware (ESP-IDF)

An ESP-IDF project that runs on an Ai-Thinker style ESP32-CAM board. It:

- Connects to a 2.4 GHz WiFi network in station mode
- Runs an HTTP server on port 80
- Serves a live MJPEG stream at `/stream` (chunked, multipart/x-mixed-replace)
- Serves a minimal HTML viewer at `/` for any browser — no app or plugin needed

## Part 2: Android Native App (C++ / OpenCV)

A NativeActivity Android app written entirely in C++ — no Gradle, no Java/Kotlin:

- Opens a raw TCP socket to the ESP32 and issues a `GET /stream` HTTP request
- Parses the chunked multipart MJPEG stream and extracts individual JPEG frames
- Decodes frames with OpenCV and runs a **Haar cascade face classifier** on each one
- Draws green rectangles and labels around detected faces
- Renders frames directly to the screen via `ANativeWindow`
- OpenCV is statically linked into a single library, with 16 KB page alignment for Android 15+ devices

The app auto-reconnects if the stream drops, and handles the ESP32's limitation of one concurrent streaming client.
