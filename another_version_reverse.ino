#include <Arduino.h>
#include <SPIFFS.h>

// -------- PINY --------
#define PWM_PIN 14
#define WHISTLE_PIN 27
#define AUDIO_PIN 25

// -------- AUDIO --------
#define SAMPLE_RATE 22050
#define MAX_CHANNELS 3 // 0: engine, 1: whistle, 2: coal

#define FP_SHIFT 8
#define FP_ONE (1 << FP_SHIFT)

// -------- STRUKTURY --------
struct Sample {
  uint8_t *data;
  int length;
};

struct Voice {
  Sample *sample;
  volatile uint32_t pos;
  volatile bool active;
  uint32_t pitch;
  uint16_t volume;
  bool loop; // NOWE – czy zapętlać
};

// -------- PRÓBKI --------
Sample engine, whistle, coal;
Voice voices[MAX_CHANNELS];

// -------- PWM --------
volatile uint32_t pwm_raw_speed = 0;
volatile uint32_t pwm_raw_whistle = 0;
uint32_t start_speed = 0;
uint32_t start_whistle = 0;

// -------- STAN --------
float filteredPWM = 1500;
float currentSpeed = 0;
float targetSpeed = 0;
float direction = 0;

float steamLevel = 0;

unsigned long lastCoalTime = 0;
unsigned long nextCoalDelay = 20000 + random(20000); // 20–40s

// -------- ISR PWM --------
void IRAM_ATTR handleSpeedPWM() {
  if (digitalRead(PWM_PIN)) start_speed = micros();
  else pwm_raw_speed = micros() - start_speed;
}

void IRAM_ATTR handleWhistlePWM() {
  if (digitalRead(WHISTLE_PIN)) start_whistle = micros();
  else pwm_raw_whistle = micros() - start_whistle;
}

// -------- TIMER AUDIO --------
hw_timer_t *timer = NULL;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile int lastOut = 128;

void IRAM_ATTR onTimer() {
  int32_t mix = 0;
  bool anyActive = false;

  portENTER_CRITICAL_ISR(&mux);

  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!voices[i].active || voices[i].sample == NULL) continue;

    anyActive = true;

    int idx = voices[i].pos >> FP_SHIFT;
    int len = voices[i].sample->length;

    if (idx >= len) {
      if (voices[i].loop) {
        voices[i].pos = 0;
        idx = 0;
      } else {
        voices[i].active = false;
        continue;
      }
    }

    int32_t val =
      ((int32_t)voices[i].sample->data[idx] - 128) *
      voices[i].volume / 255;

    mix += val;
    voices[i].pos += voices[i].pitch;
  }

  portEXIT_CRITICAL_ISR(&mux);

  // -------- DELIKATNA PARA --------
  static int32_t noise = 0;
  noise = (noise * 7 + (esp_random() & 0xFF) - 128) >> 3;

  int32_t steam = (noise * (5 + steamLevel * 15)) / 255;

  int32_t out;

  if (anyActive) {
    out = mix + steam + 128;
    lastOut = constrain(out, 0, 255);
  } else {
    lastOut += ((128 - lastOut) >> 3);
    out = lastOut + steam / 2;
  }

  dacWrite(AUDIO_PIN, (uint8_t)out);
}

// -------- WAV --------
Sample loadWav(const char *path) {
  File f = SPIFFS.open(path);
  Sample s = {NULL, 0};

  if (!f || f.size() < 44) return s;

  s.length = f.size() - 44;
  s.data = (uint8_t*)malloc(s.length);

  f.seek(44);
  f.read(s.data, s.length);
  f.close();

  return s;
}

// -------- PLAY --------
void playVoice(int ch, Sample &s, uint32_t pitch, uint16_t vol, bool loop=false) {
  if (ch >= MAX_CHANNELS || s.data == NULL) return;

  portENTER_CRITICAL(&mux);
  voices[ch].sample = &s;
  voices[ch].pos = 0;
  voices[ch].pitch = pitch;
  voices[ch].volume = vol;
  voices[ch].loop = loop;
  voices[ch].active = true;
  portEXIT_CRITICAL(&mux);
}

// -------- SETUP --------
void setup() {
  Serial.begin(115200);
  SPIFFS.begin(true);

  engine = loadWav("/engine.wav");   // klikający korbowód
  whistle = loadWav("/whistle.wav");
  coal = loadWav("/coal.wav");       // zasypywanie

  pinMode(PWM_PIN, INPUT);
  pinMode(WHISTLE_PIN, INPUT_PULLDOWN);

  attachInterrupt(digitalPinToInterrupt(PWM_PIN), handleSpeedPWM, CHANGE);
  attachInterrupt(digitalPinToInterrupt(WHISTLE_PIN), handleWhistlePWM, CHANGE);

  timer = timerBegin(1000000);
  timerAttachInterrupt(timer, &onTimer);
  timerAlarm(timer, 1000000 / SAMPLE_RATE, true, 0);
}

// -------- LOOP --------
void loop() {

  // --- filtr PWM ---
  filteredPWM = filteredPWM * 0.8 + pwm_raw_speed * 0.2;
  uint32_t pSpeed = filteredPWM;
  uint32_t pWhistle = pwm_raw_whistle;

  // --- kierunek ---
  if (pSpeed > 1600 && pSpeed < 2100) {
    targetSpeed = (pSpeed - 1600) / 400.0;
    direction = 1;
  }
  else if (pSpeed < 1400 && pSpeed > 900) {
    targetSpeed = (1400 - pSpeed) / 400.0;
    direction = -1;
  }
  else {
    targetSpeed = 0;
    direction = 0;
  }

  targetSpeed = constrain(targetSpeed, 0, 1);

  // --- bezwładność ---
  currentSpeed += (targetSpeed - currentSpeed) * 0.02;

  // --- poziom pary ---
  steamLevel += (currentSpeed - steamLevel) * 0.02;

  // -------- SILNIK (ciągły loop) --------
  if (!voices[0].active && engine.data != NULL) {
    playVoice(0, engine, FP_ONE, 120, true);
  }

  // pitch zależny od prędkości
  float pitchF;

  if (direction >= 0) {
    pitchF = 0.5 + currentSpeed * 2.0;
  } else {
    pitchF = 0.4 + currentSpeed * 1.5; // wolniejszy wstecz
  }

  voices[0].pitch = pitchF * FP_ONE;
  voices[0].volume = 80 + currentSpeed * 120;

  // -------- GWIZDEK --------
  if (pWhistle > 1650 && !voices[1].active) {
    playVoice(1, whistle, FP_ONE, 255, false);
  }

  // -------- WĘGIEL (LOSOWO) --------
  if (millis() - lastCoalTime > nextCoalDelay) {
    playVoice(2, coal, FP_ONE, 200, false);

    lastCoalTime = millis();
    nextCoalDelay = 20000 + random(30000); // 20–50s
  }
}
