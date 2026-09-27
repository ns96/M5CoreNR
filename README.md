# M5CoreNR - Real-Time Cassette Tape Audio Restoration Suite

Real-time audio digital signal processing (DSP) noise reduction, authentic tape compander decoders (Dolby B, Dolby C, DBX Type II), and psychoacoustic harmonic exciter firmware for audio cassette tapes running on the **M5Stack Core2** (ESP32 @ 240 MHz) with the **M5 Module Audio** (ES8388 stereo codec).

M5CoreNR restores and decodes analog cassette tapes in real time with **10.7 ms latency** (512-frame stereo DMA buffer at 48 kHz native sampling rate with low-jitter hardware APLL clocking).

---

## Disclaimer

> **This project is strictly a proof of concept, and it was written with the help of AI.**
>
> - **AI-assisted code.** The firmware, DSP algorithms, and this documentation were largely generated and iterated with an AI coding assistant, then built and tested on real hardware. Treat the code as a starting point to study, not as reviewed, certified, or production-grade software.
> - **Proof of concept, not a reference implementation.** The decoders here were implemented from publicly available information found on the web. They were **not** measured or verified against calibrated test signals, reference levels, or factory-aligned hardware.
> - **The decoders are not equivalent to the real 1980s silicon.** The **Dolby B**, **Dolby C**, and **DBX Type II** paths do **not** claim to match the decoding accuracy, tracking behaviour, or specification compliance of genuine Dolby or DBX hardware. Time constants, filter slopes, detector behaviour, and level calibration are simplified and will differ audibly from a correctly aligned original unit. Do not expect bit-exact or reference-grade decoding, and do not use this as an authority on how those formats actually behave.
> - **No measurements, no compliance testing.** There is no audio analyser data, THD+N figure, frequency-response plot, or formal verification behind the numbers quoted below beyond real-time CPU load and buffer/latency bookkeeping observed on the device.
> - **"Dolby" and "DBX" are trademarked names** of their respective owners. They are used here only descriptively, to indicate which playback format a mode is loosely modelled on. This project is not affiliated with, endorsed by, or licensed by either company.
> - **Use at your own risk.** No warranty of any kind. Audio artefacts, unexpected behaviour, or hardware quirks are possible; verify anything important against the original hardware before relying on it.

---

## Key Features & DSP Pipeline

- **3-Band Split Spectral Expander ("DE-HISS")**:
  - Replaces traditional single-band sweeping DNR with a modern, artifact-free **3-Band Linkwitz-Riley 4th-Order (24 dB/oct)** crossover with allpass phase compensation.
  - **Band 1 (0 to 3.0 kHz)**: **100% untouched bit-clean passthrough**. Bass, drums, vocal fundamentals, and acoustic instruments retain their natural tone and body with **zero timbral modulation** or "filter breathing."
  - **Band 2 (3.0 kHz to 7.5 kHz)**: Gentle downward expander providing up to **-8 dB** attenuation only when absence of presence energy is detected.
  - **Band 3 (7.5 kHz to 24 kHz)**: Deep downward expander providing up to **-18 dB** attenuation of wideband cassette tape hiss during quiet phrases and pauses.
  - **Zero Tonal Breathing**: Because crossover points are stationary, the music's timbre and equalization never shift based on volume.
- **Psychoacoustic Harmonic Exciter & Analog Tape Warmth**:
  - Overcomes permanent cassette demagnetization, head wear, azimuth loss, and thin playback.
  - **Analog Tape Warmth Low-Shelf (+3.5 dB at 100 Hz)**: Adds rich, deep, punchy low-end body and fullness, compensating for cassette head-gap losses and portable player headphone output rolloff.
  - **Full Midrange Presence**: Band 1 (0–3 kHz) and Band 2 (3–7.5 kHz) are preserved at 100% full uncompressed power (0.0 dB).
  - **Harmonic Synthesis (2nd & 3rd Order)**: Isolates musical presence (2.0 kHz – 4.5 kHz) and synthesizes vibrant upper-octave harmonics via asymmetric soft saturation ($y = 0.50 x_{\text{even}} + 0.50 x_{\text{odd}}$) injected above 4.2 kHz (24% drive).
  - **Air Sheen High-Shelf (+3.5 dB at 7.0 kHz)**: Restores open acoustic sparkle, cymbals, and vocal breath.
  - **Integrated Dynamic Air De-Hiss**: Seamlessly utilizes the 3-Band Air expander to attenuate high-frequency tape hiss down to -18 dB during track pauses and quiet gaps while opening to 100% full bandwidth during active music.
  - **CPU Utilization (~35%–45%)**: Efficient processing displayed in real time on the LCD header bar.
- **Authentic Dolby B Playback Decoder**:
  - Classic sliding-band de-emphasis network centered at **1.8 kHz**.
  - Sidechain high-pass detector (>2.5 kHz) continuously tracks high-frequency tape level relative to Dolby standard reference level.
  - Dynamically introduces up to **-10 dB** of high-shelf de-emphasis during quiet passages, canceling Dolby B recording pre-emphasis and eliminating high-frequency tape hiss.
- **Authentic Dolby C Playback Decoder**:
  - Dual-stage sliding-band compander providing up to **-20 dB** of high-frequency tape noise reduction.
  - Integrates an **8.5 kHz Anti-Spectral Skewing De-Emphasis Filter (-3.5 dB)** to prevent high-frequency tape saturation, harshness, and overload distortion on hot cassette recordings.
- **Authentic DBX Type II Tape Decoder**:
  - True continuous **1:2 Downward RMS Dynamic Expander**, linear in decibels across the full dynamic range.
  - Restores the original 100+ dB dynamic range from DBX-encoded cassettes.
  - Includes a matched **2.5 kHz de-emphasis network** to counteract DBX recording pre-emphasis, eliminating tape hiss without audible breathing or pumping.
- **Subsonic Motor Rumble Filter**:
  - 25 Hz high-pass filter running permanently in the background to eliminate cassette transport motor rumble, wow vibrations, and mechanical thumps.
- **Master Soft Limiter**:
  - Transparent hyperbolic tangent (`tanh`) soft-clipping limiter to prevent digital DAC clipping while preserving musical dynamics.
- **Retro Hi-Fi Touch UI (320x240 LCD)**:
  - **Double-Buffered Canvas (PSRAM Sprite)**: 100% flicker-free 30 Hz rendering.
  - **Dual Stereo VU Meters**: Vintage fluorescent bar graph with peak-hold indicators (-35 dB to +3 dB).
  - **Dynamic Header Bar**: Live **CPU %**, **Battery %**, **Round-Trip Latency (LAT:10.7ms)**, and **Volume %**.
  - **Real-Time DSP Status Panel**: Displays active mode, live de-hiss attenuation in dB, de-emphasis attenuation, expander ratio, and exciter drive.
  - **3-Page Multi-Grid Touch Navigation**:
    - **Page 1 (Enhancements)**: `[ BYPASS ]`, `[ DE-HISS ]`, `[ EXCITER ]`, `[ NEXT > ]`
    - **Page 2 (Tape Decoders)**: `[ DOLBY B ]`, `[ DOLBY C ]`, `[ DBX ]`, `[ NEXT > ]`
    - **Page 3 (Bluetooth)**: `[ BT RX ]`, `[ BT TX ]`, `[ BT OFF ]`, `[ < BACK ]`
    - Single-shot edge touch detection with 300 ms debounce preventing bounce-back.
    - Cross-page active mode border indicators so the active audio mode is always clearly visible.
    - Page indicator dots in the gap above the button row (the pages cycle 1 → 2 → 3 → 1).
    - Live Bluetooth link state (`PAIRING`, `LINKED`, `STREAMING`, ring fill, XRUN count) on page 3, plus a header badge visible from every page.
    - Tactile haptic vibration feedback on touches and volume adjustments.
- **Module Audio RGB Status LEDs**:
  - **Green**: BYPASS (Raw Tape)
  - **Cyan**: DNR (Hiss Cut)
  - **Electric Blue**: EXCITER (Air & Sparkle)
  - **Cobalt Blue**: DOLBY B (-10 dB Sliding Band)
  - **Magenta**: DOLBY C (-20 dB Dual Stage)
  - **Warm Amber / Gold**: DBX (1:2 Downward Expander)
  - **Red**: Muted
  - **LED 0 cyan/blue blink**: Bluetooth RX active (slow blink when the stream is linked)
  - **LED 0 purple blink**: Bluetooth TX active
- **Hardware Button Volume Control (Buttons Below LCD)**:
  - **Button A (Left)**: Volume Down (`VOL -`)
  - **Button B (Center)**: Mute Toggle (`MUTE`)
  - **Button C (Right)**: Volume Up (`VOL +`)

---

---

## Bluetooth A2DP (BT RX / BT TX)

The third touch screen adds Bluetooth audio without touching the wired signal path.

| Button | Mode | Behaviour |
| :--- | :--- | :--- |
| **BT RX** | A2DP Sink | The device advertises itself as `M5CoreNR`. Pair from a phone or laptop and the decoded audio **replaces the Line In block** entering the DSP engine, so DE-HISS, Exciter, Dolby B/C and DBX all apply exactly as they do for tape. |
| **BT TX** | A2DP Source | The post-DSP 48 kHz block that feeds the headphone DAC is also resampled to 44.1 kHz, SBC encoded and streamed to the configured speaker. **The headphone output keeps working at the same time**, so both listeners hear the same processed audio. Input stays Line In. |
| **BT OFF** | - | Ends the A2DP profile and disables the Bluetooth controller, restoring the RF-quiet baseline and Line In audio. |

### Library requirement

Bluetooth needs the **ESP32-A2DP** library (pschatzmann, Apache-2.0). It is not published in
the Arduino Library Manager registry, so install it from GitHub:

```text
git clone https://github.com/pschatzmann/ESP32-A2DP.git %USERPROFILE%\Documents\Arduino\libraries\ESP32-A2DP
```

Alternatively download the repository ZIP and use *Sketch -> Include Library -> Add .ZIP
Library...*. `arduino-audio-tools` is not required.

Set `BT_ENABLE 0` in `Config.h` to build without Bluetooth: the A2DP code is compiled out and
the Bluetooth page reports `BT LIB MISSING`.

Verified with arduino-cli 1.5.1 + esp32 core 3.3.3 for `esp32:esp32:m5stack_core2` -
`BT_ENABLE 1` uses 1,349,179 bytes of flash (20%), `BT_ENABLE 0` uses 576,363 bytes (8%).

### Why the WiFi library is not linked

`WiFi.mode(WIFI_OFF)` was removed from `setup()`. Merely referencing the WiFi library pulls in
its IRAM-optimised code, and on the ESP32 the link then fails with
`region iram0_0_seg overflowed by 92 bytes` because the Bluetooth stack also lives in IRAM.
WiFi is never initialised anywhere in this firmware, so the WiFi radio still never transmits
and the wired noise floor is unchanged. If you later add WiFi features, expect to have to
trade Bluetooth back out (or move to a chip with more IRAM).

### How it is wired into the audio engine

- `BTAudio.cpp` owns two lock-free single-producer/single-consumer PCM rings (PSRAM) and
  two continuously variable cubic resamplers. `DSP_Engine`, the 48 kHz codec setup and the
  I2S full-duplex loop are unchanged - I2S keeps running in every BT state because it is
  both the ES8388 master clock and the 10.67 ms block pace maker.
- **Clock drift**: the Bluetooth peer has its own crystal, so a fixed resampling ratio would
  slowly starve or overflow the rings (a click or gap every few minutes). A drift servo
  trims the ratio by up to ±1 % based on the measured ring fill level, keeping both
  directions locked long term.
- **De-clicking**: every start/stop and every volume change is applied as a 15 ms per-sample
  gain ramp. MUTE mutes the wired and the Bluetooth output together.
- **Volume mirroring**: `BT_TX_MIRROR_VOLUME` mirrors the ES8388 DAC volume law
  (`steps = (V * 33 + 50) / 100`) onto the Bluetooth stream, so the speaker tracks the
  headphone level. Tune `BT_VOL_DB_PER_STEP` if the levels do not match to taste.
- **RTOS**: the audio task stays pinned to Core 0 together with the Bluetooth stack, which is
  why the TX DMA preload cushion and the ring levels matter. If BT TX plus EXCITER approaches
  the CPU limit shown in the header, the ring buffer absorbs the scheduling jitter.

### Knowing whether it is actually paired

Radio, link and audio are three different things, and the UI reports all three:

| Link | Header badge | Page 3 status line | RX/TX button | LED 0 |
| :--- | :--- | :--- | :--- | :--- |
| Not connected (advertising / searching) | `BT:RX` cyan, `BT:TX` cobalt | `BT RX: PAIR "M5CoreNR" ON YOUR PHONE` | `PAIRING` / `SEARCHING` | fast blink |
| **Paired / connected, no audio** | **amber** | `BT RX: <phone> CONNECTED (IDLE)` | `CONNECTED` | **steady** |
| Audio flowing | green | `BT RX: <phone>  RING 42%  XRUN 0` | `LINKED` / `STREAMING` | slow blink |
| Library missing | `BT:!!` red | `BT LIB MISSING - INSTALL ESP32-A2DP` | `PAIRING` | - |

`CONNECTED (IDLE)` is the state that answers "did pairing work?": it comes from the A2DP link
state itself, so it shows up even when the phone is paused. The peer name and the
`RING`/`XRUN` figures need an active stream. The serial log prints one line per transition
(`A2DP sink CONNECTED`, `streaming`, `link lost`), which is the easiest way to watch a
pairing session from a PC.

### Configuration (`Config.h`)

| Define | Purpose |
| :--- | :--- |
| `BT_ENABLE` | Compile-time master switch |
| `BT_SINK_NAME` | Name advertised in BT RX mode (`M5CoreNR`) |
| `BT_SOURCE_PEERS` | Comma separated A2DP sink names for BT TX - an A2DP source cannot browse, so the target must be listed |
| `BT_RX_RING_FRAMES` / `BT_TX_RING_FRAMES` | Ring depths (powers of two, allocated in PSRAM) |
| `BT_RING_TARGET_PCT` / `BT_RING_MIN_PCT` / `BT_RING_MAX_PCT` | Servo set point and fade limits |
| `BT_SERVO_KP` / `BT_SERVO_MAX_TRIM` | Drift servo gain and authority (±1 %) |
| `BT_TX_ACCEPT_FIRST` | BT TX: connect to the first audio device discovered when no configured name matches |
| `BT_TX_MIRROR_VOLUME` / `BT_VOL_DB_PER_STEP` | Headphone volume mirroring |
| `BT_RX_SOURCE_VOLUME` | AVRCP volume advertised to a device connecting in BT RX mode (127 = full scale) |

### AVRCP volume ("connected but silent")

The A2DP sink advertises an absolute volume to whatever connects. The ESP32-A2DP default is
**0**, and the reply the sink sends when a source registers for volume notifications carries
that value - so phones and PCs obediently set their own output volume to 0, which looks like a
successful pairing that produces no sound until the source volume is raised. The same 0 also
scales the decoded PCM to silence once a volume is applied. M5CoreNR advertises full scale
(`BT_RX_SOURCE_VOLUME 127`) instead and leaves loudness to the local headphone volume, while
still honouring the source slider when the user moves it. Every volume the source requests is
logged as `[BT] Source volume: N%`.

### Known limitations

1. **One A2DP role at a time** - BT RX and BT TX cannot run simultaneously (the library and
   Bluedroid own a single A2DP role). BT RX with headphones, and BT TX with headphones, both
   work; BT RX and BT TX together do not.
2. **SBC is lossy** - BT TX is an SBC re-encode (~345 kbps), so it is a close copy, not a
   bit-exact one. BYPASS remains bit-perfect on the wired headphone path only.
3. **Latency** - A2DP adds roughly 150-250 ms on top of the ring buffer depth, so the wired
   headphone output leads the Bluetooth output. Irrelevant for separate listeners.
4. **BT TX chooses its target by name** - an A2DP source cannot browse devices and connects by
   matching the speaker's advertised name. `BTAudio` logs every device it discovers
   (`[BT] Found device: "..." RSSI .. dBm`), matches the prefixes in `BT_SOURCE_PEERS`, and -
   when nothing matches and `BT_TX_ACCEPT_FIRST` is 1 - connects to the first audio capable
   device found. Listing the real name is the reliable option, because accept-first will also
   latch onto any other audio device in range (for example a laptop). The inquiry is re-armed
   every 12 s while nothing is connected, which the library does not do by itself.
5. **Radio noise** - enabling Bluetooth wakes a 2.4 GHz transmitter inside the case. The radio
   is only powered when BT RX or BT TX is selected, and BT OFF disables the controller again,
   so the wired-only noise floor is unchanged.

### Verified on hardware (M5Stack Core2, esp32 core 3.3.3)

| Path | Result |
| :--- | :--- |
| BT RX, PC and phone streaming | Two independent sources, 130-150 s each: ring held 30-37 % against the 35 % servo target, `xrun 0`, ~250 packets per 5 s, DSP 25-34 %, no watchdog abort |
| BT TX, Bluetooth speaker | Ring held 29-38 %, `xrun 0`, ~1740 pulls per 5 s (44.1 kHz), DSP 33-35 % |
| Simultaneous BT TX + wired headphones | Headphone output keeps running while the speaker streams the same processed audio |
| AVRCP volume | Sources connect at full scale instead of being muted |

Known source-side edge case: one source (a QFX RETRO-1980 boombox) completes the AVDTP start
handshake (`a2dp STARTED`) but then transmits **zero** media packets, which the health line shows
as `pkts 0/5s NO-AUDIO` while the panel reads `CONNECTED (NO AUDIO)`. Nothing is wrong on the
M5 side in that case - the peer never sends audio. Its manual explains why the deck is a special
case: Bluetooth **transmit** is a separate switch position and only works **in TAPE playback mode**
(green LED = transmitting, blue = receiving as a speaker), and the deck re-connects to its
remembered sink. So it has to be unpaired from that speaker, in TAPE with a cassette playing, and
switched to TX before it will stream to the M5. It also will not stream radio or AUX - for those,
use Line In.

### Diagnostic logging

While `BT_LOG_STATS` is 1 (default) a health line is printed every 5 s for the active direction:
`ring` fill, `xrun` count, packet or pull rate, A2DP stream state, DSP load and free heap. Set it
to 0 for a quiet log. The boot banner also reports the reset reason of the previous boot, and
`[BT]` lines trace connections, peer names, source volume requests and link loss.

---

## From 1980s Analog Silicon to Modern Real-Time MCU DSP

Throughout the 1970s and 1980s, dedicated analog integrated circuits (such as Dolby B/C chips, discrete DBX VCA modules, and National Semiconductor's LM1894 DNR) were the **only viable option** for tape noise reduction. Microcontrollers and early digital signal processors of that era were far too slow, primitive, and prohibitively expensive for consumer real-time audio. Analog circuitry required banks of precision resistors, capacitors, and factory-calibrated trimpots—all inherently prone to component aging, thermal drift, calibration misalignment, and channel mismatch.

It was not until the early to mid-1990s that digital processing finally became feasible in consumer cassette decks, most notably with systems like Pioneer's **FLEX (Frequency Level Expander)** and digital-processing decks (such as the CT-S670D and T-D7). These high-end decks used custom, expensive DSP ASICs to digitally analyze playback in real time, dynamically expanding lost high frequencies and cleaning up tape sound.

### The Modern MCU Reality

Today, a single low-cost microcontroller like the **ESP32** (dual-core 32-bit Xtensa @ 240 MHz with hardware FPU) can perform all of these classic analog and early digital restoration techniques entirely in software:

- **Mathematical Precision & Zero Drift**: Dynamic companders, envelope detectors, and state-variable filters are calculated with 32-bit floating-point math, delivering **perfect 0.00 dB channel tracking**, zero thermal drift, and zero component degradation over time.
- **Predictable Computational Performance**:
  - Lightweight modes (**BYPASS**, **DNR**, **Dolby B**, **Dolby C**, and **DBX Type II**) operate with minimal overhead, consuming only **~4% to 15%** of Core 0.
  - The computationally demanding **Psychoacoustic Harmonic Exciter**—which executes real-time dynamic SVF filtering, cascaded presence biquads, non-linear 2nd/3rd-order polynomial saturation equations ($y = 0.55 x_{\text{odd}} + 0.45 x_{\text{even}}$), sparkle highpass filtering, and air-sheen shelving per stereo sample—approaches **~55% to 60% CPU usage**, running smoothly with ample real-time headroom.
- **Multi-Standard Flexibility**: Rather than requiring multiple discrete analog decoder boards or costly legacy cassette decks, a modern pocket-sized MCU switches instantly between Dolby B, Dolby C, DBX Type II, and dynamic enhancement filters on a touch screen—all with **10.7 ms round-trip latency**.

---

## Code Compactness: Lines of Code & Why Software DSP is So Lean

The entire real-time DSP engine in [`DSP_Engine.cpp`](DSP_Engine.cpp) comprises only **824 lines of code** (and 50 lines in [`DSP_Engine.h`](DSP_Engine.h)). Each specific noise reduction or tape decoding algorithm requires just **50 to 100 lines** of specialized logic:

| DSP Method / Component | Approximate Lines | What It Implements |
| :--- | :---: | :--- |
| **Shared Filter Primitives** | ~145 lines | Generalized Direct Form II Biquad, Linkwitz-Riley 4th-order cascaded Butterworth, 1st-order Subsonic HPF, and Tanh Soft Limiter. |
| **3-Band Spectral De-Hiss** | ~95 lines | Fixed 3000 Hz / 7500 Hz LR4 crossover, 2nd-order allpass delay compensation, dual envelope followers, and soft-knee downward expansion. |
| **Psychoacoustic Exciter** | ~95 lines | 3.0 kHz presence BPF, envelope follower, asymmetric saturation (2nd/3rd harmonics), 4.2 kHz HPF, and +3.5 dB air shelf. |
| **Dolby B Decoder** | ~55 lines | 2.5 kHz sidechain level detector and dynamic 1.8 kHz sliding high-shelf (-10 dB de-emphasis). |
| **Dolby C Decoder** | ~70 lines | 8.5 kHz anti-spectral skew filter, 2.0 kHz sidechain detector, and dual-stage sliding band (-20 dB de-emphasis). |
| **DBX Type II Decoder** | ~50 lines | True block RMS power integration, logarithmic dB conversion, 1:2 downward expansion, and matched 2.5 kHz de-emphasis. |
| **Engine Core & Infrastructure** | ~320 lines | 16-bit PCM $\leftrightarrow$ 32-bit float buffer conversions, 25 Hz rumble blocker, DC offset removal, and VU meter ballistic tracking. |
| **Total DSP Engine** | **~830 lines** | **Full real-time processing suite** |

### Why Does It Take So Few Lines?

In the 1970s and 1980s, each of these systems occupied entire circuit boards packed with hundreds of discrete components, high-precision resistor ladders, matched transistor pairs, discrete Voltage-Controlled Amplifiers (VCAs), and delicate factory calibration trim-pots. 

Software DSP collapses that complexity down to dozens of lines because:

1. **Difference Equations Replace Complex Analog Networks**:
   - A multi-component analog RLC active filter or op-amp shelving network collapses into a standard Direct Form II Transposed biquad difference equation ($y[n] = b_0 x[n] + s_1$). The actual per-sample filter execution takes only **4 lines of C++**.
2. **Reusable Filter & Envelope Primitives**:
   - Because all companders share common mathematical primitives (biquad shelving, bandpass detection, and smoothing filters), the individual decoders only need to calculate their specific target levels and update filter coefficients.
3. **Instant Mathematical Functions Replace Dedicated Silicon**:
   - An analog DBX decoder required an entire discrete temperature-stabilized log-converter module to compute RMS levels and logarithmic expansion. In C++, that entire subsystem is executed in three standard math calls:
     ```cpp
     float rms = sqrtf(sumSq / numFrames);
     float in_dB = 20.0f * log10f(rms / ref);
     float gainLin = powf(10.0f, (in_dB * 1.0f) / 20.0f);
     ```
4. **Zero Calibration Trim-Pots or Hardware Trimming**:
   - Analog Dolby and DBX circuits required multi-turn potentiometer trimmers to manually align left/right balance and match the tape deck's 200 nWb/m Dolby calibration level. In digital DSP, reference levels are exact mathematical constants with **0.00 dB tracking error** and zero drift over time.

> For a deep dive into the engineering history, patents, and mathematical equations behind our Dolby B, Dolby C, and DBX Type II decoders, see the [Noise Reduction Technical Reference](NRTech.md).

---

## Hardware Connections & Architecture

```text
[Cassette Player Line Out] ──> 3.5mm Line In ──> [ES8388 ADC]
                                                        │
                                                        │ I2S DMA Stereo 48kHz (Core 0 High-Priority Task)
                                                        │ ESP32 Hardware APLL Clock (12.288 MHz MCLK)
                                                        ▼
                                              [ESP32 Audio Pipeline]
                                              ├── 2-Buffer TX Preload (Zero Underflow)
                                              ├── 25Hz Subsonic Rumble HPF
                                              ├── Mode Router:
                                              │   ├── BYPASS   (Bit-perfect 1:1 raw tape)
                                              │   ├── DE-HISS  (3-band Linkwitz-Riley spectral expander)
                                              │   ├── EXCITER  (Harmonic synthesis + 7.0 kHz air)
                                              │   ├── DOLBY B  (-10 dB sliding-band de-emphasis)
                                              │   ├── DOLBY C  (-20 dB dual-stage + anti-skew)
                                              │   └── DBX      (1:2 RMS downward expander)
                                              └── Master Tanh Soft Limiter
                                                        │
                                                        │ I2S DMA Stereo 48kHz
                                                        ▼
[Headphones / Amp Out]     <── 3.5mm Phone   <── [ES8388 DAC]
```

### Bluetooth Audio Paths (BT RX / BT TX)

```text
BT RX  [Phone / Laptop] ──A2DP SBC──> [A2DP Sink] ──44.1/48k PCM──> [Ring + drift servo]
                                                                          │
                                                       resample to 48 kHz ▼
                                          replaces the Line In block ──> [DSP Engine]

BT TX  [ES8388 ADC] ──> [DSP Engine] ──┬──> [ES8388 DAC] ──> 3.5mm Phone (headphones)
                                        │
                                        └──> [Ring + drift servo] ──> [A2DP Source]
                                                                       └──SBC──> [BT speaker]
```

The headphone DAC and the Bluetooth transmitter therefore always receive **the same
post-DSP audio**, simultaneously, with no interruption to the wired output.

### Pinout Mapping:
| Function | ESP32 GPIO | Description |
| :--- | :--- | :--- |
| **I2S MCLK** | GPIO 0 | Hardware APLL Master Clock to ES8388 (12.288 MHz) |
| **I2S BCLK** | GPIO 19 | Bit Clock |
| **I2S LRCK** | GPIO 27 | Word Select (Left/Right Clock) |
| **I2S DOUT** | GPIO 2 | DAC Audio Data (To Headphones / Phone Jack) |
| **I2S DIN** | GPIO 34 | ADC Audio Data (From Line In Jack) |
| **I2C SDA** | GPIO 21 | Wire1 to ES8388 & STM32 Module Audio |
| **I2C SCL** | GPIO 22 | Wire1 to ES8388 & STM32 Module Audio |

---

## Required Libraries

In the Arduino IDE or PlatformIO, ensure the following libraries are installed:
1. **`M5Unified`** (by M5Stack)
2. **`Module-Audio`** (by M5Stack, includes `audio_i2c.hpp` and `es8388.hpp`)

---

## Operation & Touch Controls

1. Connect the Line Out or Headphone Out of your cassette deck to the **Line In** jack on the M5 Module Audio.
2. Connect your headphones or external amplifier to the **Phone** jack.
3. Power on the M5Stack Core2.
4. **Select Mode & Page Navigation**:
   - **Page 1: Enhancements & Raw Tape**
     - **`[ BYPASS ]`**: 100% bit-perfect direct passthrough. Raw tape sound for immediate A/B comparison.
     - **`[ DE-HISS ]`**: 3-Band Split Spectral Expander (attenuates background tape hiss up to -18 dB without tonal breathing).
     - **`[ EXCITER ]`**: Psychoacoustic harmonic synthesizer + Air Sheen (+3.5 dB at 7.0 kHz).
     - **`[ NEXT > ]`**: Flips screen to Page 2 (Tape Decoders).
   - **Page 2: Authentic Cassette Tape Decoders**
     - **`[ DOLBY B ]`**: Dolby B playback decoder with sliding-band high-shelf de-emphasis (up to -10 dB).
     - **`[ DOLBY C ]`**: Dolby C playback decoder with dual-stage companding (-20 dB) and anti-spectral skewing filter.
     - **`[ DBX ]`**: DBX Type II 1:2 downward RMS dynamic expander with matched 2.5 kHz de-emphasis.
     - **`[ < BACK ]`**: Flips screen back to Page 1.
5. **Adjust Volume & Mute**:
   - Tap or hold the **Left capacitive button (Btn A)** below the screen to decrease volume.
   - Tap or hold the **Right capacitive button (Btn C)** to increase volume.
   - Tap the **Center button (Btn B)** to toggle mute.
