#include <Arduino.h>
#include <SPIFFS.h>
#include <driver/i2s.h>

// -------- PINY --------
#define PWM_PIN 14
#define WHISTLE_PIN 27

#define I2S_BCLK 26
#define I2S_LRC  25
#define I2S_DOUT 22

// -------- AUDIO --------
#define SAMPLE_RATE 22050
#define MAX_CHANNELS 3

#define FP_SHIFT 8
#define FP_ONE (1 << FP_SHIFT)

// -------- I2S --------
#define AUDIO_BUFFER 128
int16_t audioBuffer[AUDIO_BUFFER];
int audioIndex = 0;

#define I2S_PORT I2S_NUM_0

void i2sInit() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };

  i2s_pin_config_t pins = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_PORT, &config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
}

// -------- SAMPLE --------
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
  bool loop;
};

Sample engine, whistle, coal;
Voice voices[MAX_CHANNELS];

// -------- PWM --------
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

// -------- LOAD WAV --------
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

  voices[ch].sample = &s;
  voices[ch].pos = 0;
  voices[ch].pitch = pitch;
  voices[ch].volume = vol;
  voices[ch].loop = loop;
  voices[ch].active = true;
}

// -------- AUDIO MIX --------
void audioTick() {

  int32_t mix = 0;

  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!voices[i].active || voices[i].sample == NULL) continue;

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

  mix += 128;

  int16_t out = constrain(mix, 0, 255) << 8;

  audioBuffer[audioIndex++] = out;

  if (audioIndex >= AUDIO_BUFFER) {
    size_t bytesWritten;
    i2s_write(I2S_PORT, audioBuffer, sizeof(audioBuffer),
              &bytesWritten, portMAX_DELAY);
    audioIndex = 0;
  }
}

// -------- STAN --------
float filteredPWM = 1500;
float currentSpeed = 0;
float targetSpeed = 0;
float direction = 0;

unsigned long lastCoal = 0;
unsigned long nextCoal = 20000;

// -------- SETUP --------
void setup() {
  Serial.begin(115200);
  SPIFFS.begin(true);

  i2sInit();

  engine = loadWav("/engine.wav");
  whistle = loadWav("/whistle.wav");
  coal = loadWav("/coal.wav");

  pinMode(PWM_PIN, INPUT);
  pinMode(WHISTLE_PIN, INPUT_PULLDOWN);

  attachInterrupt(PWM_PIN, handleSpeedPWM, CHANGE);
  attachInterrupt(WHISTLE_PIN, handleWhistlePWM, CHANGE);
}

// -------- LOOP --------
void loop() {

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
  currentSpeed += (targetSpeed - currentSpeed) * 0.03;

  // --- engine loop ---
  if (!voices[0].active) {
    playVoice(0, engine, FP_ONE, 120, true);
  }

  float pitch = 0.5 + currentSpeed * 2.0;
  if (direction < 0) pitch *= 0.85;

  voices[0].pitch = pitch * FP_ONE;
  voices[0].volume = 80 + currentSpeed * 120;

  // --- gwizdek ---
  if (pWhistle > 1650 && !voices[1].active) {
    playVoice(1, whistle, FP_ONE, 255, false);
  }

  // --- węgiel ---
  if (millis() - lastCoal > nextCoal) {
    playVoice(2, coal, FP_ONE, 180, false);

    lastCoal = millis();
    nextCoal = 20000 + random(30000);
  }

  // --- AUDIO STEP ---
  audioTick();
}
