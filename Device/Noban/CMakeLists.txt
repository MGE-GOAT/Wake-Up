cmake_minimum_required(VERSION 3.16)
project(farsi_pipeline)
set(CMAKE_CXX_STANDARD 20)

# Tensor dump for parity testing (parity_test/). When ON, the binary writes
# intermediate tensors to $DUMP_TENSORS_DIR for diff against Python ref.
# Slight overhead at every dump call site; off in production builds.
option(DUMP_TENSORS "Enable tensor dumps for parity testing" OFF)
if(DUMP_TENSORS)
    add_compile_definitions(DUMP_TENSORS=1)
endif()

# ── Detect platform ───────────────────────────────────────
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    message(STATUS "Target: ARM64 (Raspberry Pi 4 / Pi 5 — 64-bit OS)")
    # Cortex-A72 (Pi 4) tuning. NEON is mandatory on ARMv8-A so no
    # explicit -mfpu flag needed. -O3 + tree-vectorize lets GCC use NEON
    # for the YIN squared-difference loop and the mel filterbank.
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=armv8-a+crc -mtune=cortex-a72 -ftree-vectorize -ffast-math")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch32|armv7|armhf")
    message(STATUS "Target: ARMv7 32-bit (Raspberry Pi 3 — legacy Raspbian)")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=armv7-a -mfpu=neon-vfpv4 -ftree-vectorize -ffast-math")
else()
    message(STATUS "Target: x86_64 (PC / Kali)")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -ftree-vectorize -ffast-math")
endif()

# Workaround a gcc-14 internal compiler error ("Illegal instruction" in the GIMPLE
# 'nothrow' / set_nothrow_flag IPA pass) that triggers on main.cpp at -O3.
# -fno-ipa-pure-const disables only that pure/const/nothrow inference; the rest of
# -O3 stays. Drop this once the toolchain is upgraded past the bug.
add_compile_options(-fno-ipa-pure-const)

# ── Sources ───────────────────────────────────────────────
# INMP441 i2s MEMS mic delivers 24-bit samples in S32_LE frames. Without
# this define the capture path opens S16_LE which destroys the upper bits.
# Set to 0 only if migrating to a true 16-bit USB mic.
add_compile_definitions(USE_S32_CAPTURE=1)

add_executable(pipeline
    main.cpp
    pain_pipeline.cpp
    extract_background.cpp
    fall_detection.cpp
    mqtt_client.cpp
    media_crypto.cpp
    gpio_io.cpp
)

# ── libgpiod v2 — push button + RGB LED on Pi OS Trixie (sysfs GPIO removed).
# apt: libgpiod-dev. If absent (dev box), gpio_io.cpp compiles HAVE_GPIOD=0
# and button falls back to SIGUSR1, LED to print-only.
find_library(GPIOD_LIB gpiod)
find_path(GPIOD_INCLUDE gpiod.h)
if(GPIOD_LIB AND GPIOD_INCLUDE)
    message(STATUS "libgpiod: found — hardware button + LED enabled")
    target_include_directories(pipeline PRIVATE ${GPIOD_INCLUDE})
    target_link_libraries(pipeline PRIVATE ${GPIOD_LIB})
else()
    message(STATUS "libgpiod: NOT found — button=SIGUSR1, LED=print-only")
endif()

# ── libsodium — Phase 3 E2E media crypto. apt: libsodium-dev. If missing,
# media_crypto.cpp's HAVE_SODIUM is 0 and helpers return errors; main.cpp
# falls back to plaintext transport with a startup warning.
find_library(SODIUM_LIB sodium)
find_path(SODIUM_INCLUDE sodium.h)
if(SODIUM_LIB AND SODIUM_INCLUDE)
    message(STATUS "libsodium: found — E2E media crypto enabled")
    target_include_directories(pipeline PRIVATE ${SODIUM_INCLUDE})
    target_link_libraries(pipeline PRIVATE ${SODIUM_LIB})
else()
    message(STATUS "libsodium: NOT found — media will be sent in plaintext")
endif()

# ── paho-mqtt-cpp — Phase 5.1 transport. Replaces the /Live_Device HTTP
# poll with persistent broker subscriptions. apt: libpaho-mqttpp-dev (+ its
# C lib libpaho-mqtt-dev as transitive). Header is <mqtt/async_client.h>.
#
# Optional: if the libs aren't installed (e.g. on a dev box that doesn't
# care about MQTT), define HAVE_MQTT=0 and mqtt_client.cpp compiles as a
# no-op stub. Production builds (Pi after setup.sh) always have them.
find_library(PAHO_MQTT_C   paho-mqtt3a)
find_library(PAHO_MQTT_CPP paho-mqttpp3)
if(PAHO_MQTT_C AND PAHO_MQTT_CPP)
    message(STATUS "MQTT: paho found — building with broker transport")
    target_compile_definitions(pipeline PRIVATE HAVE_MQTT=1)
    target_link_libraries(pipeline PRIVATE ${PAHO_MQTT_CPP} ${PAHO_MQTT_C})
else()
    message(STATUS "MQTT: paho NOT found — mqtt_client.cpp will compile as no-op stub")
    target_compile_definitions(pipeline PRIVATE HAVE_MQTT=0)
endif()

# ── pocketfft (header-only) — bit-exact match with numpy.fft / scipy.fft
#   (which use the same pocketfft backend internally since numpy 1.17).
#   Replaces KFR for all STFT + YIN FFT operations.
target_include_directories(pipeline PRIVATE ${CMAKE_SOURCE_DIR}/deps/pocketfft)

# ── OpenCV — fall_detection port uses it for all frame ops ────────────────
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
target_include_directories(pipeline PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(pipeline PRIVATE ${OpenCV_LIBS})

# ── Flatbuffers (bundled — must precede system headers) ────
set(FLATBUFFERS_INCLUDE "${CMAKE_SOURCE_DIR}/deps/tensorflow-src/tflite_build/flatbuffers/include")
target_include_directories(pipeline BEFORE PRIVATE ${FLATBUFFERS_INCLUDE})

# ── libcurl — server-side STT + intent matching is reached via HTTP ───────
find_package(CURL REQUIRED)
target_include_directories(pipeline PRIVATE ${CURL_INCLUDE_DIRS})
target_link_libraries(pipeline PRIVATE ${CURL_LIBRARIES})

# ── OpenSSL — SHA-256 of the device password (Key-Hash header). Libcurl
# already loads libcrypto transitively, but link explicitly so future cmake
# changes don't break the SHA-256 call in loadDeviceIdentity().
find_package(OpenSSL REQUIRED)
target_link_libraries(pipeline PRIVATE OpenSSL::Crypto)

# ── TFLite ────────────────────────────────────────────────
set(TFLITE_BUILD "${CMAKE_SOURCE_DIR}/deps/tensorflow-src/tflite_build")
set(TFLITE_DIR "${CMAKE_SOURCE_DIR}/deps/tflite")
target_include_directories(pipeline PRIVATE ${TFLITE_DIR}/include)

# Optional static libs from r2.13 — some configs produce these, others bake
# them into libtensorflow-lite.a. Resolve at configure time so the link line
# below picks them up only when needed.
file(GLOB RUY_ARCHIVES "${TFLITE_BUILD}/_deps/ruy-build/ruy/libruy_*.a")
message(STATUS "ruy archives picked up: ${RUY_ARCHIVES}")

foreach(_pair
        "OPT_XNNPACK|${TFLITE_BUILD}/_deps/xnnpack-build/libXNNPACK.a"
        "OPT_CPUINFO_INTERNALS|${TFLITE_BUILD}/_deps/cpuinfo-build/libcpuinfo_internals.a")
    string(REPLACE "|" ";" _pp "${_pair}")
    list(GET _pp 0 _var)
    list(GET _pp 1 _path)
    if(EXISTS "${_path}")
        set(${_var} "${_path}")
        message(STATUS "TFLite opt lib present: ${_path}")
    else()
        set(${_var} "")
        message(STATUS "TFLite opt lib absent : ${_path}")
    endif()
endforeach()

# TFLite abseil headers
target_include_directories(pipeline BEFORE PRIVATE
    ${TFLITE_BUILD}/_deps/abseil-cpp-src
)

# r2.13 has circular link deps: libtensorflow-lite.a ↔ libXNNPACK.a ↔
# libpthreadpool.a. Wrapping the whole TFLite block in --start-group /
# --end-group lets the linker iterate until all symbols resolve, regardless
# of the order of individual .a files within.
target_link_libraries(pipeline PRIVATE
    -Wl,--start-group
    # TFLite main — TFLite r2.13 bakes the XNNPACK delegate sources directly
    # into libtensorflow-lite.a (no separate libxnnpack-delegate.a target).
    # Match-training requires r2.13; deployment must rebuild if version moves.
    ${TFLITE_DIR}/lib/libtensorflow-lite.a

    # ruy — glob every libruy_*.a present. On ARM the avx variants are still
    # archived (empty stubs) but the kernel_arm + pack_arm carry the real
    # symbols; on x86 it's the inverse. Globbing keeps the list correct
    # across architectures without per-arch branching.
    ${RUY_ARCHIVES}

    # xnnpack — r2.13 normally bakes XNNPACK into libtensorflow-lite.a
    # (OBJECT lib) but if the kernel-side cmake setup didn't archive the .o
    # files, you need to ar-archive them yourself (see Pi setup notes) and
    # the conditional append below will pick it up.
    ${TFLITE_BUILD}/pthreadpool/libpthreadpool.a
    ${TFLITE_BUILD}/_deps/cpuinfo-build/libcpuinfo.a
    ${OPT_XNNPACK}
    ${OPT_CPUINFO_INTERNALS}

    # fft2d
    ${TFLITE_BUILD}/_deps/fft2d-build/libfft2d_fftsg.a
    ${TFLITE_BUILD}/_deps/fft2d-build/libfft2d_fftsg2d.a
    ${TFLITE_BUILD}/_deps/fft2d-build/libfft2d_alloc.a

    # farmhash
    ${TFLITE_BUILD}/_deps/farmhash-build/libfarmhash.a

    # flatbuffers (bundled v25)
    ${TFLITE_BUILD}/_deps/flatbuffers-build/libflatbuffers.a

    # abseil
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/strings/libabsl_strings.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/strings/libabsl_strings_internal.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/strings/libabsl_str_format_internal.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/base/libabsl_base.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/base/libabsl_raw_logging_internal.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/base/libabsl_throw_delegate.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/base/libabsl_spinlock_wait.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/numeric/libabsl_int128.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/hash/libabsl_hash.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/hash/libabsl_city.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/synchronization/libabsl_synchronization.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/synchronization/libabsl_graphcycles_internal.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/time/libabsl_time.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/time/libabsl_civil_time.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/time/libabsl_time_zone.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/status/libabsl_status.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/status/libabsl_statusor.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/log/libabsl_log_internal_message.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/log/libabsl_log_internal_globals.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/log/libabsl_log_internal_format.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/log/libabsl_log_internal_check_op.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/log/libabsl_log_globals.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/log/libabsl_log_sink.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/log/libabsl_log_entry.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/debugging/libabsl_stacktrace.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/debugging/libabsl_symbolize.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/debugging/libabsl_debugging_internal.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/debugging/libabsl_demangle_internal.a
    ${TFLITE_BUILD}/_deps/abseil-cpp-build/absl/container/libabsl_raw_hash_set.a
    -Wl,--end-group
)

# ── RNNoise ───────────────────────────────────────────────
target_link_libraries(pipeline PRIVATE rnnoise)

# ── ONNX Runtime ──────────────────────────────────────────
set(ORT_DIR "${CMAKE_SOURCE_DIR}/deps/onnxruntime")
target_include_directories(pipeline PRIVATE ${ORT_DIR}/include)
target_link_libraries(pipeline PRIVATE ${ORT_DIR}/lib/libonnxruntime.so)

# ── ALSA + system libs ────────────────────────────────────
target_link_libraries(pipeline PRIVATE asound pthread dl m)

install(TARGETS pipeline DESTINATION bin)

