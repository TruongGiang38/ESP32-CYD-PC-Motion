# Tự động tạo file firmware gộp (bootloader + partitions + app) sau mỗi lần build.
# Kết quả: .pio/build/<env>/firmware_merged.bin  -> nạp tại địa chỉ 0x0
Import("env")  # noqa: F821  (do PlatformIO cung cấp)
import subprocess

board = env.BoardConfig()  # noqa: F821


def merge_bin(source, target, env):  # noqa: F811
    images = [env.subst(str(x)) for x in env.Flatten(env.get("FLASH_EXTRA_IMAGES", []))]
    images += [env.subst("$ESP32_APP_OFFSET"), env.subst("$BUILD_DIR/${PROGNAME}.bin")]

    merged = env.subst("$BUILD_DIR/${PROGNAME}_merged.bin")
    mcu = board.get("build.mcu", "esp32")
    flash_mode = board.get("build.flash_mode", "dio")
    f_flash = str(board.get("build.f_flash", "40000000L")).rstrip("L")
    flash_freq = "%dm" % (int(f_flash) // 1000000)
    flash_size = board.get("upload.flash_size", "4MB")

    cmd = [
        env.subst("$PYTHONEXE"), env.subst("$OBJCOPY"),
        "--chip", mcu, "merge_bin", "-o", merged,
        "--flash_mode", flash_mode, "--flash_freq", flash_freq,
        "--flash_size", flash_size,
    ] + images

    print("Tạo file gộp:", merged)
    subprocess.run(cmd, check=True)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_bin)  # noqa: F821
