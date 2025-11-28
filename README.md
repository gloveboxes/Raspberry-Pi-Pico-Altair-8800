# Raspberry Pi Pico Altair 8800

## Clone With Submodules

This project bundles Pimoroni's Pico helper libraries as a git submodule.
Clone (or update) with:

```shell
git clone --recurse-submodules https://github.com/gloveboxes/Raspberyy-Pi-Pico-2-W-Altair-8800.git
# or, if already cloned
git submodule update --init --recursive
```

## Serial Terminal

### macos

1. screen /dev/tty.usbmodem101 115200
   
   - screen is built into macOS
   - Exit: Press <kbd>ctrl+c</kbd> then <kbd>K</kbd>

2. picocom /dev/tty.usbmodem101 -b 115200

    - Install

        ```shell
        brew install picocom
        ```

    - Exit: Press <kbd>ctrl+a</kbd>, then <kbd>ctrl+x</kbd>


## Wi-Fi Console

1. Provide credentials via the new CMake cache variables (preferred):
    ```shell
    cmake -B build -DWIFI_SSID="MyNetwork" -DWIFI_PASSWORD="secretpass"
    ```
    The values are injected as preprocessor macros. (You can still fall back to defining `PICO_DEFAULT_WIFI_SSID`/`PICO_DEFAULT_WIFI_PASSWORD` elsewhere if you prefer.)
2. Build and flash as usual. On boot the Pico W connects to Wi-Fi and starts a WebSocket console on port `8082`.
3. Point a browser at `http://<pico-ip>:8082/` to load the bundled console UI, or use any WebSocket-capable client (e.g., `wscat`) to connect to `ws://<pico-ip>:8082/` and interact with the Altair terminal alongside USB serial.
4. USB serial stays active; terminal I/O is mirrored between USB and the WebSocket session.

### CMake Configuration Options

| Option | Default | Purpose |
| --- | --- | --- |
| `-DWIFI_SSID=""` | empty | Wi-Fi SSID for the Pico W (passed to firmware at build time). |
| `-DWIFI_PASSWORD=""` | empty | Wi-Fi password accompanying the SSID. |
| `-DENABLE_INKY_DISPLAY=ON` | ON | Pulls in the Pimoroni Inky Pack driver and shows the welcome/IP screen. Set to `OFF` to save flash/RAM when the display isn't connected. |
| `-DCMAKE_BUILD_TYPE=Release` | Debug | Usual CMake switch for optimized builds (recommended). |

## Regenerate Disk Image Header

1. Copy the .dsk file to the disks folder
2. Run the following command

    ```shell
    python3 dsk_to_header.py --input cpm63k.dsk --output Altair8800/cpm63k_disk.h --symbol cpm63k_dsk
    ```

3. Copy the .h file to the Altair8800 folder
4. Rebuild and deploy


## Rebuild for Performance

cmake -B build -DCMAKE_BUILD_TYPE=Release regenerated the build directory with CMAKE_BUILD_TYPE explicitly set to Release (confirmed by the “Build type is Release” line). That enables the Pico SDK’s release optimization flags (-O3, no extra debug helpers).
cmake --build build then rebuilt everything with those settings. The log shows only Release-config targets being built and the final altair.elf linked successfully with no errors—just the usual picotool fetch/install noise and a warning about duplicate errors/liberrors.a, which the SDK always emits.


```shell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Building App

There are two build tasks

1. Build Altair (Release): Default build task, does clean then build.
2. Build Altair (Debug)L Create a debug build.
