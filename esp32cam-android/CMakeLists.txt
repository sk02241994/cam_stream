cmake_minimum_required(VERSION 3.14)
project(esp32cam)

set(CMAKE_CXX_STANDARD 14)

set(NDK_HOME $ENV{ANDROID_NDK_HOME})
string(REPLACE "\\" "/" NDK_HOME "${NDK_HOME}")

set(NDK_NATIVE_APP_GLUE "${NDK_HOME}/sources/android/native_app_glue")

add_library(native_app_glue STATIC "${NDK_NATIVE_APP_GLUE}/android_native_app_glue.c")

include(FetchContent)
FetchContent_Declare(
    opencv
    URL "file://${CMAKE_CURRENT_SOURCE_DIR}/opencv-4.10.0-android-sdk.zip"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(opencv)

set(OPENCV_SDK_DIR     ${opencv_SOURCE_DIR})
set(OPENCV_INC         ${OPENCV_SDK_DIR}/sdk/native/jni/include)
set(OPENCV_STATIC_DIR  ${OPENCV_SDK_DIR}/sdk/native/staticlibs/${ANDROID_ABI})
set(OPENCV_3RD_DIR     ${OPENCV_SDK_DIR}/sdk/native/3rdparty/libs/${ANDROID_ABI})

function(add_opencv_static name)
    set(lib_path ${OPENCV_STATIC_DIR}/libopencv_${name}.a)
    if(EXISTS ${lib_path})
        add_library(opencv_${name} STATIC IMPORTED)
        set_target_properties(opencv_${name} PROPERTIES IMPORTED_LOCATION ${lib_path})
    else()
        message(FATAL_ERROR "Missing OpenCV static lib: ${lib_path}")
    endif()
endfunction()

add_opencv_static(core)
add_opencv_static(imgproc)
add_opencv_static(objdetect)
add_opencv_static(imgcodecs)

file(GLOB OPENCV_3RD_LIBS "${OPENCV_3RD_DIR}/*.a")

# ── Our app (MUST be defined before target_link_options) ──
add_library(esp32cam SHARED src/main.cpp)

target_include_directories(esp32cam PRIVATE
    "${NDK_NATIVE_APP_GLUE}"
    "${OPENCV_INC}"
)

target_link_options(esp32cam PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,-u,ANativeActivity_onCreate"
)

target_link_libraries(esp32cam
    android
    native_app_glue
    EGL
    GLESv1_CM
    log
    opencv_objdetect
    opencv_imgproc
    opencv_imgcodecs
    opencv_core
    ${OPENCV_3RD_LIBS}
    z
    dl
    m
)