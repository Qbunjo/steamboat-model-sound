#include <Arduino.h>
#include <SPIFFS.h>

// --- PIN CONFIG ---
#define PWM_PIN 14
#define WHISTLE_PIN 35
#define AUDIO_PIN 25  // DAC1

// ---  AUDIO PARAMETERS ---
#define SAMPLE_RATE 22050
#define FADE_SAMPLES 400 
#define MAX_CHANNELS 4  // 3 chuffs + whistle

struct Sample {
  uint8_t *data;
  int length;
};

struct Voice {
  Sample *sample;
  volatile int pos;
  volatile bool active;
};

// --- SAMPLES ---
Sample chuff1, chuff2, chuff3, whistle;
Voice voices[MAX_CHANNELS];

// --- VAR PWM ---
volatile uint32_t pwm_raw_speed = 0;
volatile uint32_t pwm_raw_whistle = 0;
uint32_t start_speed = 0;
uint32_t start_whistle = 0;

// --- ISR PWM ---
void IRAM_ATTR handleSpeedPWM() {
  if (digitalRead(PWM_PIN) == HIGH) start_speed = micros();
  else pwm_raw_speed = micros() - start_speed;
}

void IRAM_ATTR handleWhistlePWM() {
  if (digitalRead(WHISTLE_PIN) == HIGH) start_whistle = micros();
  else pwm_raw_whistle = micros() - start_whistle;
}

// --- AUDIO TIMER ---
hw_timer_t * timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR onTimer() {
  int32_t mix = 0;

  portENTER_CRITICAL_ISR(&timerMux);
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (voices[i].active && voices[i].sample != NULL) {
      int pos = voices[i].pos;
      int len = voices[i].sample->length;
      int32_t val = (int32_t)voices[i].sample->data[pos] - 128;

      int remaining = len - pos;
      if (remaining <= FADE_SAMPLES) val = (val * remaining) / FADE_SAMPLES;

      mix += val;
      voices[i].pos++;

      if (voices[i].pos >= len) voices[i].active = false;
    }
  }
  portEXIT_CRITICAL_ISR(&timerMux);

  // --- MIXING ---
  int32_t finalOut = mix + 128;
  finalOut = constrain(finalOut, 0, 255);
  dacWrite(AUDIO_PIN, finalOut);
}

// --- FUNCTIONS ---
Sample loadWav(const char *path) {
  File f = SPIFFS.open(path);
  Sample s = {NULL, 0};
  if (!f) {
    Serial.printf("[ERROR] File not found: %s\n", path);
    return s;
  }
  uint32_t fileSize = f.size();
  if (fileSize < 44) {
    Serial.printf("[ERROR] File %s is too short (WAV header)\n", path);
    f.close();
    return s;
  }

  s.length = fileSize - 44;
  s.data = (uint8_t*)malloc(s.length);
  if (s.data) {
    f.seek(44);
    f.read(s.data, s.length);
    Serial.printf("[OK] Loaded %s, size: %d bytes\n", path, s.length);
  } else {
    Serial.printf("[ERROR] Not enough RAM for %s!\n", path);
  }
  f.close();
  return s;
}

void playVoice(int channel, Sample &s) {
  if (s.data == NULL) return;
  portENTER_CRITICAL(&timerMux);
  voices[channel].sample = &s;
  voices[channel].pos = 0;
  voices[channel].active = true;
  portEXIT_CRITICAL(&timerMux);
}

// --- CONFIGURATION ---
void setup() {
  Serial.begin(115200);
  delay(1000); 
  Serial.println("\n--- AUDIO STEAMBOAT START ESP32 ---");
  
  if (!SPIFFS.begin(true)) {
    Serial.println("[CRITICAL] Error SPIFFS!");
    while(1);
  }

  //files loading
  chuff1 = loadWav("/chuff1.wav");
  chuff2 = loadWav("/chuff2.wav");
  chuff3 = loadWav("/chuff3.wav");
  whistle = loadWav("/whistle.wav");

  // PWM
  pinMode(PWM_PIN, INPUT);
  pinMode(WHISTLE_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(PWM_PIN), handleSpeedPWM, CHANGE);
  attachInterrupt(digitalPinToInterrupt(WHISTLE_PIN), handleWhistlePWM, CHANGE);

  // --- TIMER ---
  timer = timerBegin(SAMPLE_RATE);  // only sample freq
  timerAttachInterrupt(timer, &onTimer); 
  timerStart(timer);

  Serial.println("[OK] Timer started.");
}

// --- VAR LOOP ---
unsigned long lastChuff = 0;
int nextChuff = 0;
float currentSpeed = 0;
float targetSpeed = 0;

// --- LOOP ---
void loop() {
  uint32_t pSpeed = pwm_raw_speed;
  uint32_t pWhistle = pwm_raw_whistle;

  // --- SPEED COUNT ---
  if (pSpeed > 1100 && pSpeed < 2100) targetSpeed = (float)(pSpeed - 1100) / 900.0;
  else targetSpeed = 0;
  targetSpeed = constrain(targetSpeed, 0, 1);
  currentSpeed += (targetSpeed - currentSpeed) * 0.05;

  // ---DYNAMIC CHUFF INTERVAL ---
  if (currentSpeed > 0.02) {
    float speedFactor = pow(currentSpeed, 0.5); 
    int interval = 2000 - (1800 * speedFactor); // 2000..200ms

    if (millis() - lastChuff > interval) {
      Sample *s;
      switch (nextChuff) {
        case 0: s = &chuff1; break;
        case 1: s = &chuff2; break;
        case 2: s = &chuff1; break; // repeat first chuff
        case 3: s = &chuff3; break;
      }

      playVoice(nextChuff < 3 ? nextChuff : 0, *s); // different channels for mixing
      nextChuff = (nextChuff + 1) % 4;
      lastChuff = millis();
    }
  }

  // --- GWIZDEK ---
  if (pWhistle > 1650 && !voices[3].active) playVoice(3, whistle);

  // --- DEBUG ---
  static unsigned long t = 0;
  if (millis() - t > 500) {
    Serial.printf("PWM:%d Speed:%.2f Heap:%d\n", pSpeed, currentSpeed, ESP.getFreeHeap());
    t = millis();
  }
}
