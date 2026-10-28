# ESP32-P4 Live Dictation

Live English speech-to-text that runs entirely on an ESP32-P4. There's no cloud and no PC. You speak near the board,
and the words appear on a web page that the board serves over Ethernet.

- **Model:** [Kroko](https://huggingface.co/Banafo/Kroko-ASR) by Banafo, a streaming Zipformer2 transducer with
  65.7M parameters.
- **Engine:** [esp32-pie-asr](https://github.com/BoumedineBillal/esp32-pie-asr) by Boumedine Billal. It quantizes
  the weights to 2 and 4 bits (26.65 MB) and runs them with hand-written kernels for the P4's PIE SIMD unit.
- **This repo adds:**
  - support for the Waveshare board and for P4 chip revision v1.x;
  - loading the weights from a flash partition, so no SD card is needed;
  - Ethernet and a web page that shows the text as you speak.

## Measured on the board

On a Waveshare ESP32-P4-WIFI6-POE-ETH with chip revision v1.3 at 360 MHz and PSRAM at 200 MHz:

| | |
|---|---|
| Speed | 0.81–0.87× real time (each 1.28 s chunk of audio computes in about 1.05 s) |
| Search | beam search, 4 hypotheses |
| Boot to listening | about 8 s: the weights are read from flash in 5.5 s and initialised in 1.4 s |
| Memory left while running | 2.2 MB PSRAM, 75 KB internal RAM |
| App size | 824 KB (the weights live in their own partition) |

esp32-pie-asr reports a word error rate of 4.3% on LibriSpeech test-clean and 10.3% on test-other. Those numbers
were measured with its PC build, which matches the chip bit for bit. Accuracy with the onboard microphone hasn't been
measured.

## Hardware

- A [Waveshare ESP32-P4-WIFI6-POE-ETH](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-POE-ETH). Espressif's
  ESP32-P4-Function-EV-Board uses the same pins, but you'd need to change the chip-revision setting below to match
  its chip.
- An Ethernet cable to a network with DHCP. PoE works too.
- The onboard ES8311 codec and microphone. No extra wiring is needed.

## Build and flash

The firmware needs [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/get-started/).

```bash
git clone --recursive https://github.com/DingoOz/esp32-p4-live-dictation
cd esp32-p4-live-dictation

# the weights (CC-BY-SA 4.0, see below), checked against their published SHA-256
mkdir -p models
curl -L -o models/kroko-en-q24-v1.bin https://huggingface.co/BoumedineBillal/esp32-pie-asr-kroko-en/resolve/d97c28a8/kroko-en-q24-v1.bin
echo "52673e74be9b44948640c6652ce8fd5b2b1b1faa4a66f3f8e43105405a38ce9f  models/kroko-en-q24-v1.bin" | sha256sum -c

. ~/esp/esp-idf-v6.1/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash
python -m esptool --chip esp32p4 -p /dev/ttyACM0 -b 2000000 write-flash 0x310000 models/kroko-en-q24-v1.bin
```

You only need to flash the weights once, which takes about 3 minutes. Later `idf.py flash` runs leave them alone.

At boot, the serial log prints the page's address:

```
dictation page: http://192.168.x.x/
ASR weights from flash: 26651392 bytes in 5548 ms
ASR model ready: init 1355 ms; PSRAM free 2209348 (largest 2162688), internal free 75251
ASR listening
```

Open that address and start talking.

## How it works

- `main/main.cpp` handles the whole application:
  - **Starting up:** it brings up Ethernet (IP101 PHY, using the P4's default RMII pins) and an HTTP server.
  - **Loading the weights:** it gives the esp32-pie-asr service its settings. If the SD card's top folder has no
    `kroko-en-q24-v1.bin`, it loads the weights from the `model` flash partition instead, then starts listening.
  - **Serving the transcript:** `GET /t?g=<generation>&w=<words seen>` returns JSON with the words the page hasn't
    seen yet, the word still being decoded and the recognizer's state.
- `main/index.html` polls that endpoint every 300 ms. It shows the word still being decoded in grey and starts a new
  line after a 1.5 s pause. **Clear page** clears only the browser's view; the board keeps its transcript.
- `partitions.csv` has a 3 MB app partition and a 26 MB `model` partition at `0x310000`.
- `sdkconfig.defaults` sets:
  - `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` for chip revisions below v3 (this board's v1.3);
  - PSRAM at 200 MHz;
  - esp32-pie-asr's cache settings, a 32 KB main task and no task watchdog.

  If your chip is revision v3, drop the first setting and you can raise PSRAM to 250 MHz.
- `third_party/esp32-pie-asr` is a git submodule, used unmodified. The build uses only its engine and its microphone,
  SD-card and recognizer components; it leaves out the screen UI and demos.

## Licenses

- **The code in this repository** is under the [Apache License 2.0](LICENSE).
- **esp32-pie-asr**, a submodule, is Apache-2.0, © Boumedine Billal.
- **The weights** (`kroko-en-q24-v1.bin`) are **CC-BY-SA 4.0**. They're derived from
  [Kroko-ASR](https://huggingface.co/Banafo/Kroko-ASR) by Banafo and quantized by esp32-pie-asr. They aren't in this
  repository; the build steps download them from Hugging Face. If you redistribute them, or a firmware image that
  contains them, you must credit Banafo and share them under CC-BY-SA 4.0.
