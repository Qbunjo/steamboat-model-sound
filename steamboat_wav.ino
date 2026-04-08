#include <Arduino.h>
#include <SPIFFS.h>

// --- PINY ---
#define PWM_PIN 14
#define WHISTLE_PIN 35
#define AUDIO_PIN 25

// --- AUDIO ---
#define SAMPLE_RATE 22050
#define FADE_SAMPLES 400
#define MAX_CHANNELS 4 // 0: chuff, 1: chuff, 2: chuff, 3: whistle

#define FP_SHIFT 8
#define FP_ONE (1 << FP_SHIFT)

struct Sample {
  uint8_t *data;
  int length;
};

struct Voice {
  Sample *sample;
  volatile uint32_t pos;
  volatile bool active;
  uint32_t pitch;
  uint16_t volume; // 0-255
};

Sample chuff1, chuff2, chuff3, whistle;
Voice voices[MAX_CHANNELS];

// --- PWM ---
volatile uint32_t pwm_raw_speed = 0;
volatile uint32_t pwm_raw_whistle = 0;
uint32_t start_speed = 0;
uint32_t start_whistle = 0;

void IRAM_ATTR handleSpeedPWM() {
  if (digitalRead(PWM_PIN)) start_speed = micros();
  else pwm_raw_speed = micros() - start_speed;
}

void IRAM_ATTR handleWhistlePWM() {
  if (digitalRead(WHISTLE_PIN)) start_whistle = micros();
  else pwm_raw_whistle = micros() - start_whistle;
}

// --- TIMER ---
hw_timer_t *timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
volatile int lastOut = 128; // ostatnia wartość DAC

void IRAM_ATTR onTimer() {
  int32_t mix = 0;
  bool anyActive = false;

  portENTER_CRITICAL_ISR(&timerMux);
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!voices[i].active || voices[i].sample == NULL) continue;

    anyActive = true;
    int idx = voices[i].pos >> FP_SHIFT;
    int len = voices[i].sample->length;

    if (idx >= len) {
      voices[i].active = false;
      continue;
    }

    int32_t val = ((int32_t)voices[i].sample->data[idx] - 128) * voices[i].volume / 255;

    int remaining = len - idx;
    if (remaining <= FADE_SAMPLES) val = val * remaining / FADE_SAMPLES;

    mix += val;
    voices[i].pos += voices[i].pitch;
  }
  portEXIT_CRITICAL_ISR(&timerMux);

  int32_t out;
  if (anyActive) {
    out = ((mix * 2) / 2) + 128;
    out = constrain(out, 0, 255);
    lastOut = out;
  } else {
    lastOut += ((128 - lastOut) >> 3); // prosty slew do 128
    out = lastOut;
  }

  dacWrite(AUDIO_PIN, (uint8_t)out);
}

// --- WAV LOAD ---
Sample loadWav(const char *path) {
  File f = SPIFFS.open(path);
  Sample s = {NULL, 0};
  if (!f) {
    Serial.printf("[BŁĄD] Nie znaleziono: %s\n", path);
    return s;
  }

  if (f.size() < 44) {
    Serial.printf("[BŁĄD] Plik %s za krótki\n", path);
    f.close();
    return s;
  }

  s.length = f.size() - 44;
  s.data = (uint8_t*)malloc(s.length);
  if (!s.data) {
    Serial.println("[BŁĄD] Brak pamięci RAM!");
    f.close();
    return s;
  }

  f.seek(44);
  f.read(s.data, s.length);
  f.close();

  Serial.printf("[OK] Załadowano %s (%d bajtów)\n", path, s.length);
  return s;
}

// --- PLAY ---
void playVoice(int ch, Sample &s, uint32_t pitch = FP_ONE, uint16_t volume = 255) {
  if (ch >= MAX_CHANNELS || s.data == NULL) return;
  portENTER_CRITICAL(&timerMux);
  voices[ch].sample = &s;
  voices[ch].pos = 0;
  voices[ch].pitch = pitch;
  voices[ch].volume = volume;
  voices[ch].active = true;
  portEXIT_CRITICAL(&timerMux);
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- PAROWIEC AUDIO START ---");

  if (!SPIFFS.begin(true)) {
    Serial.println("[KRYTYCZNY] Błąd SPIFFS!");
    while (1);
  }

  chuff1 = loadWav("/chuff1.wav");
  chuff2 = loadWav("/chuff2.wav");
  chuff3 = loadWav("/chuff3.wav");
  whistle = loadWav("/whistle.wav");

  pinMode(PWM_PIN, INPUT);
  pinMode(WHISTLE_PIN, INPUT);

  attachInterrupt(digitalPinToInterrupt(PWM_PIN), handleSpeedPWM, CHANGE);
  attachInterrupt(digitalPinToInterrupt(WHISTLE_PIN), handleWhistlePWM, CHANGE);

  timer = timerBegin(1000000);
  timerAttachInterrupt(timer, &onTimer);
  timerAlarm(timer, 1000000 / SAMPLE_RATE, true, 0);

  Serial.println("[OK] Timer uruchomiony.");
}

// --- LOOP ---
unsigned long lastChuff = 0;
int nextChuff = 0;
float currentSpeed = 0;
float targetSpeed = 0;

void loop() {
  uint32_t pSpeed = pwm_raw_speed;
  uint32_t pWhistle = pwm_raw_whistle;

  // --- SPEED ---
  if (pSpeed > 1100 && pSpeed < 2100) targetSpeed = (float)(pSpeed - 1100) / 900.0;
  else targetSpeed = 0;
  targetSpeed = constrain(targetSpeed, 0, 1);
  currentSpeed += (targetSpeed - currentSpeed) * 0.05;

  // --- CHUFF ---
  if (currentSpeed > 0.02) {
    int minInterval = 150;   // minimalny odstęp ms
int maxInterval = 2000;  // maksymalny odstęp ms
float speedFactor = pow(currentSpeed, 0.6);  // dopasowanie krzywej
int interval = maxInterval - (maxInterval - minInterval) * speedFactor;
    if (millis() - lastChuff > interval) {
      Sample *s;
      switch (nextChuff) {
        case 0: s = &chuff1; break; // pierwszy chuff
        case 1: s = &chuff2; break; // drugi chuff
        case 2: s = &chuff1; break; // pierwszy chuff powtarzany jako trzeci
        case 3: s = &chuff3; break; // trzeci chuff
      }

      float minPitch = 0.3;       // wolno -> 2x dłuższy chuff
      float maxPitch = 3.5;       // szybko -> normalna długość
      float pitchF = minPitch + (maxPitch - minPitch) * currentSpeed;
      float variation = (random(-10, 11) / 100.0); // drobna losowa zmiana
      uint32_t pitch = (uint32_t)((pitchF + variation) * FP_ONE);

      uint16_t vol = 128 + currentSpeed * 127; // głośność zależna od prędkości
      playVoice(0, *s, pitch, vol);

      nextChuff = (nextChuff + 1) % 4; // cykl 4-chuffowy
      lastChuff = millis();
    }
  }

  // --- WHISTLE ---
  if (pWhistle > 1650 && !voices[3].active) playVoice(3, whistle, FP_ONE, 255);

  // --- DEBUG ---
  static unsigned long t = 0;
  if (millis() - t > 500) {
    Serial.printf("PWM:%d Speed:%.2f Heap:%d\n", pSpeed, currentSpeed, ESP.getFreeHeap());
    t = millis();
  }
}