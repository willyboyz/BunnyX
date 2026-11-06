# Nyushu FrameGen (Vulkan layer, x2)

Menyisipkan 1 frame interpolasi (block-matching + warp) di antara 2 frame emulator.
Layer ini menyamar sebagai `VK_LAYER_KHRONOS_validation`, jadi dimuat lewat opsi **Graphics Debugging** emulator.
BELUM DICOMPILE/DITES oleh pembuatnya. Anggap ini versi 0: kirim log kalau ada yang error.

## Build
- PC/Linux: `ANDROID_NDK=/path/ndk ./build.sh`
- Dari HP: upload folder ini ke repo GitHub, tab Actions > build-layer > Run workflow, lalu unduh artifact.
Hasil: `libVkLayer_khronos_validation.so` (arm64-v8a).

## Pasang
1. Ganti `lib/arm64-v8a/libVkLayer_khronos_validation.so` di folder hasil decompile dengan file baru.
2. Di `AndroidManifest.xml` tambah `android:debuggable="true"` pada tag `<application>` (loader Android biasanya cuma mau memuat layer dari APK yang debuggable).
3. Build ulang APK, sign, install.
4. Di emulator: Settings > Graphics > nyalakan **Enable Graphics Debugging**.

## Cek jalan atau tidak
`adb logcat -s NyushuFG` (atau lihat log lewat app logcat). Harus muncul: "layer aktif", "device dibuat", "swapchain dibuat", "swapchain ... siap".
Matikan sementara tanpa ganti file: `adb shell setprop debug.nyushu.fg 0` (nyalakan lagi: `1`).

## Batasan
- Hanya swapchain 8-bit RGBA/BGRA (UNORM/SRGB). Format lain otomatis dilewati.
- HUD/teks statis bisa ghosting. Cocok buat game 30 fps -> 60 fps. Kalau emulator sudah 60 fps, display 120 Hz baru masuk akal.
- Validation layer asli hilang (opsi debug jadi cuma stub).
- Butuh present mode FIFO supaya pacing rata; kalau MAILBOX/IMMEDIATE bisa patah-patah.
