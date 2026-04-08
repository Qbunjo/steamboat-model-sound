# 🚢 ESP32 Steam Engine Sound Generator (RC Controlled)

This project implements a **realistic steam engine sound generator** for RC models (e.g. steamboats), using an **ESP32**, external amplifier (e.g. LM386), and **WAV audio samples** stored in SPIFFS.

It reproduces the characteristic **double piston stroke ("chuff-chuff")**, synchronized with throttle input, and allows simultaneous playback of additional sounds such as a **steam whistle**.

---

## 🔊 Features

* 🎮 **RC PWM control** (throttle input)
* 🚂 Realistic **double steam piston cycle**
* 🎧 **Audio mixing engine** (multiple sounds at once)
* 🎺 **Whistle sound** on secondary RC channel
* ⚙️ **Smooth acceleration (inertia simulation)**
* 🎲 Slight timing randomness for natural behavior
* 💾 WAV playback from **SPIFFS**
* ⚡ Low-latency audio using ESP32 internal DAC + hardware timer

---

## 🧠 How It Works

The system uses a **custom audio mixer** running in a timer interrupt:

* WAV samples are loaded into RAM
* Each sound is treated as a separate **voice**
* Samples are mixed in real time and sent to the ESP32 internal DAC (GPIO25)

The throttle signal (PWM input) controls:

* Playback speed (cycle interval)
* Timing of piston strokes

---

## 🔧 Hardware Requirements

* ESP32 (any variant with DAC, e.g. GPIO25)
* Audio amplifier (e.g. LM386 or PAM8403)
* Speaker (4–8Ω)
* RC receiver (PWM output)

### Wiring (basic)

```
ESP32 GPIO25 → 10µF capacitor → LM386 input
ESP32 GND    → LM386 GND
LM386 OUT    → Speaker
ESP32 GPIO14 - PWM from RC (engine speed)
ESP32 GPIO35 - PWM from RC (whistle sound)
```

Optional:

* Capacitor (10µF) between pins 1–8 of LM386 for higher gain
* RC filter for smoother DAC output

---

## 📁 Audio Files

Store WAV files in SPIFFS:

```
/chuff1.wav     # strong piston stroke
/chuff2.wav     # weaker return stroke
/chuff3.wav     # just to differ from second
/whistle.wav    # steam whistle
```

### Requirements:

* 8-bit PCM
* mono
* 22050 Hz sample rate
* small file size (RAM limitations)

---

## 🎮 Controls

| Function | Input Signal       | Description           |
| -------- | ------------------ | --------------------- |
| Throttle | PWM (1000–2000 µs) | Controls engine speed |
| Whistle  | PWM > 1700 µs      | Plays whistle sound   |

---

## ⚙️ Configuration

Key parameters in code:

```cpp
#define SAMPLE_RATE 22050
#define PWM_MIN 1000
#define PWM_MAX 2000
```

You can adjust:

* engine response (inertia)
* chuff timing
* randomness
* audio levels

---

## ⚠️ Limitations

* Only **8-bit audio** (due to DAC)
* Limited RAM for WAV samples
* No hardware filtering → may require external RC filter, but hey, it's steam machine, it oughts to hiss ;)
* PWM reading uses `pulseIn()` (blocking)

---

## 🚀 Possible Improvements

* 🎧 I2S DAC for higher audio quality (16-bit)
* 🎚️ Dynamic pitch shifting based on speed
* 🔀 More simultaneous audio channels
* 🎲 Multiple randomized chuff samples
* ⚡ Non-blocking PWM input (interrupt-based)

---

## 📜 License

MIT License

---

## 🤝 Contributions

Feel free to open issues or submit pull requests!

---

