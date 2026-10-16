#include <android/log.h>
#include <android/asset_manager.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android_native_app_glue.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/imgcodecs.hpp>

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "ESP32Cam", __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, "ESP32Cam", __VA_ARGS__))

// ==================== CONFIG ====================
#define ESP32_IP   "10.63.172.60"
#define ESP32_PORT 80
#define ESP32_PATH "/stream"
// ===============================================

struct AppState {
    android_app* app;
    ANativeWindow* window;
    pthread_mutex_t frameMutex;
    cv::Mat latestFrame;
    cv::CascadeClassifier faceCascade;
    bool running;
    bool cascadeLoaded;
    int networkSocket;
};

static AppState gState = {};

// ── Utility ──────────────────────────────────────────────
static bool loadCascade(AppState* state) {
    AAssetManager* mgr = state->app->activity->assetManager;
    AAsset* asset = AAssetManager_open(mgr, "haarcascade_frontalface_default.xml", AASSET_MODE_BUFFER);
    if (!asset) { LOGE("Cascade asset not found"); return false; }

    const void* data = AAsset_getBuffer(asset);
    size_t len = AAsset_getLength(asset);

    const char* dir = state->app->activity->internalDataPath;
    char path[512];
    snprintf(path, sizeof(path), "%s/cascade.xml", dir);

    FILE* f = fopen(path, "wb");
    if (!f) { AAsset_close(asset); return false; }
    fwrite(data, 1, len, f);
    fclose(f);
    AAsset_close(asset);

    state->cascadeLoaded = state->faceCascade.load(path);
    LOGI("Cascade loaded: %s", state->cascadeLoaded ? "YES" : "NO");
    return state->cascadeLoaded;
}

static int connectToESP32() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ESP32_PORT);
    inet_pton(AF_INET, ESP32_IP, &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock); return -1;
    }
    LOGI("Connected to ESP32");
    return sock;
}

static bool sendRequest(int sock) {
    char req[256];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: ESP32Cam/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        ESP32_PATH, ESP32_IP);
    return send(sock, req, strlen(req), 0) == (int)strlen(req);
}

// ── HTTP line reader ─────────────────────────────────────
static bool readLine(int sock, char* buf, int maxLen) {
    int i = 0;
    while (i < maxLen - 1) {
        char c;
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') {
            if (i > 0 && buf[i-1] == '\r') buf[i-1] = '\0';
            else buf[i] = '\0';
            return true;
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return true;
}

// ── Chunked encoding ─────────────────────────────────────
static bool readChunkSize(int sock, int& size) {
    char line[32];
    if (!readLine(sock, line, sizeof(line))) {
        LOGE("readChunkSize: failed to read line");
        return false;
    }
    // Parse hex, ignore chunk extensions after ';'
    char* end;
    size = (int)strtol(line, &end, 16);
    LOGI("Chunk size: 0x%s = %d bytes", line, size);
    return true;
}

static bool readExact(int sock, uint8_t* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = recv(sock, buf + total, len - total, 0);
        if (n <= 0) {
            LOGE("readExact failed at %d/%d (errno=%d)", total, len, errno);
            return false;
        }
        total += n;
    }
    return true;
}

// ── Multipart MJPEG parser ───────────────────────────────
enum ParseState { STATE_FIND_BOUNDARY, STATE_READ_HEADERS, STATE_READ_JPEG };

struct MJPEGParser {
    std::string boundary;
    std::vector<uint8_t> buffer;
    ParseState state = STATE_FIND_BOUNDARY;
    int jpegLen = 0;
    int jpegRead = 0;

    void setBoundary(const std::string& b) { boundary = b; }

    // Feed data into parser, return true if a complete JPEG frame is ready
    bool feed(const uint8_t* data, int len, std::vector<uint8_t>& outJpeg) {
        buffer.insert(buffer.end(), data, data + len);

        while (true) {
            if (state == STATE_FIND_BOUNDARY) {
                if (buffer.size() < boundary.length()) return false;

                // Find boundary in buffer
                for (size_t i = 0; i <= buffer.size() - boundary.length(); i++) {
                    if (memcmp(&buffer[i], boundary.c_str(), boundary.length()) == 0) {
                        // Skip boundary and trailing \r\n or --
                        size_t pos = i + boundary.length();
                        // Skip optional \r\n or --
                        while (pos < buffer.size() && (buffer[pos] == '\r' || buffer[pos] == '\n' || buffer[pos] == '-'))
                            pos++;
                        buffer.erase(buffer.begin(), buffer.begin() + pos);
                        state = STATE_READ_HEADERS;
                        jpegLen = 0;
                        break;
                    }
                }
                if (state != STATE_READ_HEADERS) return false;
            }

            if (state == STATE_READ_HEADERS) {
                // Look for empty line (\r\n\r\n)
                for (size_t i = 0; i + 3 < buffer.size(); i++) {
                    if (buffer[i] == '\r' && buffer[i+1] == '\n' &&
                        buffer[i+2] == '\r' && buffer[i+3] == '\n') {

                        // Parse headers
                        std::string hdr((char*)buffer.data(), i);
                        size_t cl = hdr.find("Content-Length: ");
                        if (cl != std::string::npos) {
                            jpegLen = atoi(hdr.c_str() + cl + 16);
                        } else {
                            cl = hdr.find("content-length: ");
                            if (cl != std::string::npos) jpegLen = atoi(hdr.c_str() + cl + 16);
                        }

                        buffer.erase(buffer.begin(), buffer.begin() + i + 4);
                        state = STATE_READ_JPEG;
                        jpegRead = 0;
                        LOGI("Part header parsed, JPEG length=%d", jpegLen);
                        break;
                    }
                }
                if (state != STATE_READ_JPEG) return false;
            }

            if (state == STATE_READ_JPEG) {
                int need = jpegLen - jpegRead;
                if (need <= 0) {
                    // We have a complete frame
                    outJpeg.assign(buffer.begin(), buffer.begin() + jpegLen);
                    buffer.erase(buffer.begin(), buffer.begin() + jpegLen);
                    state = STATE_FIND_BOUNDARY;
                    return true;
                }

                int avail = (int)buffer.size();
                if (avail >= need) {
                    outJpeg.assign(buffer.begin(), buffer.begin() + jpegLen);
                    buffer.erase(buffer.begin(), buffer.begin() + jpegLen);
                    state = STATE_FIND_BOUNDARY;
                    return true;
                }
                // Need more data
                jpegRead = avail;
                return false;
            }
        }
    }
};

// ── Frame processing ─────────────────────────────────────
static cv::Mat processFrame(const std::vector<uint8_t>& jpegData, AppState* state) {
    if (jpegData.size() < 10 || jpegData[0] != 0xFF || jpegData[1] != 0xD8) {
        LOGE("Invalid JPEG (size=%zu, start=%02X%02X)", jpegData.size(), jpegData[0], jpegData[1]);
        return cv::Mat();
    }

    cv::Mat img = cv::imdecode(jpegData, cv::IMREAD_COLOR);
    if (img.empty()) {
        LOGE("imdecode failed");
        return cv::Mat();
    }

    if (state->cascadeLoaded) {
        cv::Mat gray;
        cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
        cv::equalizeHist(gray, gray);

        std::vector<cv::Rect> faces;
        state->faceCascade.detectMultiScale(gray, faces, 1.1, 3, 0, cv::Size(30, 30));

        for (size_t i = 0; i < faces.size(); i++) {
            cv::rectangle(img, faces[i], cv::Scalar(0, 255, 0), 2);
            char label[32];
            snprintf(label, sizeof(label), "Face %d", (int)i + 1);
            cv::putText(img, label,
                        cv::Point(faces[i].x, faces[i].y - 5),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5,
                        cv::Scalar(0, 255, 0), 2);
        }
        if (faces.size() > 0) LOGI("Detected %d face(s)", (int)faces.size());
    }
    return img;
}

// ── Network thread ───────────────────────────────────────
static void* networkThread(void* arg) {
    AppState* state = (AppState*)arg;

    while (state->running) {
        int sock = connectToESP32();
        if (sock < 0) { sleep(2); continue; }
        state->networkSocket = sock;

        if (!sendRequest(sock)) { close(sock); sleep(2); continue; }

        // ── Read HTTP response headers ─────────────────────
        char line[1024];
        bool headersDone = false;
        bool chunked = false;
        std::string boundary;

        LOGI("--- HTTP Headers ---");
        for (int i = 0; i < 50 && !headersDone; i++) {
            if (!readLine(sock, line, sizeof(line))) {
                LOGE("Header read failed");
                close(sock); sleep(2); continue;
            }
            LOGI("HDR: %s", line);

            if (strlen(line) == 0) {
                headersDone = true;
            } else {
                if (strstr(line, "Transfer-Encoding: chunked") || strstr(line, "transfer-encoding: chunked"))
                    chunked = true;

                const char* b = strstr(line, "boundary=");
                if (b) {
                    b += 9;
                    if (*b == '"') b++;
                    boundary = std::string("--") + b;
                    size_t end = boundary.find('"');
                    if (end != std::string::npos) boundary = boundary.substr(0, end);
                    end = boundary.find_first_of(" \t;");
                    if (end != std::string::npos) boundary = boundary.substr(0, end);
                    LOGI("Boundary: [%s]", boundary.c_str());
                }
            }
        }

        if (!headersDone) {
            LOGE("Headers never completed");
            close(sock); sleep(2); continue;
        }

        if (boundary.empty()) {
            // Default for ESP-IDF camera example
            boundary = "--frame";
            LOGI("Using default boundary: %s", boundary.c_str());
        }

        LOGI("Stream started (chunked=%s)", chunked ? "YES" : "NO");

        // ── Read body ──────────────────────────────────────
        MJPEGParser parser;
        parser.setBoundary(boundary);

        int frameCount = 0;
        while (state->running) {
            std::vector<uint8_t> chunkData;

            if (chunked) {
                int chunkSize = 0;
                if (!readChunkSize(sock, chunkSize)) {
                    LOGE("Chunk size read failed");
                    break;
                }
                if (chunkSize == 0) {
                    LOGI("Last chunk received");
                    break; // end of stream
                }
                if (chunkSize > 5 * 1024 * 1024) {
                    LOGE("Chunk too large: %d", chunkSize);
                    break;
                }

                chunkData.resize(chunkSize);
                if (!readExact(sock, chunkData.data(), chunkSize)) break;

                // Read trailing \r\n
                uint8_t crlf[2];
                if (!readExact(sock, crlf, 2)) break;
            } else {
                // Non-chunked: read raw data in blocks
                uint8_t buf[8192];
                int n = recv(sock, buf, sizeof(buf), 0);
                if (n <= 0) break;
                chunkData.assign(buf, buf + n);
            }

            // Feed to MJPEG parser
            std::vector<uint8_t> jpeg;
            while (parser.feed(chunkData.data(), (int)chunkData.size(), jpeg)) {
                frameCount++;
                LOGI("Frame #%d: %zu bytes", frameCount, jpeg.size());

                cv::Mat frame = processFrame(jpeg, state);
                if (!frame.empty()) {
                    pthread_mutex_lock(&state->frameMutex);
                    state->latestFrame = frame.clone();
                    pthread_mutex_unlock(&state->frameMutex);
                }
                chunkData.clear(); // parser consumed all
            }
        }

        LOGI("Stream ended after %d frames", frameCount);
        close(sock);
        state->networkSocket = -1;
        sleep(2);
    }
    return NULL;
}

// ── Rendering ────────────────────────────────────────────
static void renderFrame(AppState* state) {
    if (!state->window) return;

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(state->window, &buffer, NULL) < 0) return;

    pthread_mutex_lock(&state->frameMutex);
    cv::Mat frame = state->latestFrame.clone();
    pthread_mutex_unlock(&state->frameMutex);

    if (!frame.empty()) {
        cv::Mat scaled, rgba;
        cv::resize(frame, scaled, cv::Size(buffer.width, buffer.height));
        cv::cvtColor(scaled, rgba, cv::COLOR_BGR2RGBA);

        uint8_t* dst = (uint8_t*)buffer.bits;
        int dstStride = buffer.stride * 4;
        for (int y = 0; y < buffer.height; y++) {
            memcpy(dst + y * dstStride, rgba.ptr(y), buffer.width * 4);
        }
    } else {
        uint8_t* dst = (uint8_t*)buffer.bits;
        for (int y = 0; y < buffer.height; y++) {
            memset(dst + y * buffer.stride * 4, 0, buffer.width * 4);
        }
    }
    ANativeWindow_unlockAndPost(state->window);
}

// ── App glue ─────────────────────────────────────────────
static void handle_cmd(struct android_app* app, int32_t cmd) {
    AppState* state = (AppState*)app->userData;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            state->window = app->window;
            ANativeWindow_setBuffersGeometry(state->window, 0, 0, WINDOW_FORMAT_RGBA_8888);
            LOGI("Window ready: %dx%d",
                 ANativeWindow_getWidth(state->window),
                 ANativeWindow_getHeight(state->window));
            break;
        case APP_CMD_TERM_WINDOW:
            state->window = NULL;
            break;
    }
}

static int32_t handle_input(struct android_app* app, AInputEvent* event) {
    return 0;
}

void android_main(struct android_app* state) {
    LOGI("=== ESP32Cam Face Detection Starting ===");

    memset(&gState, 0, sizeof(gState));
    gState.app = state;
    gState.running = true;
    gState.networkSocket = -1;
    pthread_mutex_init(&gState.frameMutex, NULL);

    state->userData = &gState;
    state->onAppCmd = handle_cmd;
    state->onInputEvent = handle_input;

    loadCascade(&gState);

    pthread_t netThread;
    pthread_create(&netThread, NULL, networkThread, &gState);

    while (1) {
        int ident;
        int events;
        struct android_poll_source* source;

        while ((ident = ALooper_pollOnce(16, NULL, &events, (void**)&source)) >= 0) {
            if (source != NULL) source->process(state, source);
            if (state->destroyRequested != 0) {
                LOGI("Shutting down...");
                gState.running = false;
                if (gState.networkSocket >= 0) {
                    close(gState.networkSocket);
                    gState.networkSocket = -1;
                }
                pthread_join(netThread, NULL);
                pthread_mutex_destroy(&gState.frameMutex);
                LOGI("Shutdown complete");
                return;
            }
        }
        renderFrame(&gState);
    }
}
