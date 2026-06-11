#include <Arduino.h>
#include <U8g2lib.h>
#include <arduinoFFT.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

// =======================================================
// PIN CONFIG
// =======================================================
#define OLED1_SDA 8
#define OLED1_SCL 9

#define OLED2_SDA 10
#define OLED2_SCL 11

#define BTN_GREEN_PLAY_STOP 4
#define BTN_YELLOW_NEXT     5

// =======================================================
// AUDIO CONFIG
// =======================================================
#define SERIAL_BAUDRATE 115200
#define SAMPLING_FREQUENCY 22050
#define SAMPLES 512
#define NUM_BANDS 16

#define RING_BUFFER_SIZE_1 (64 * 1024)
#define RING_BUFFER_SIZE_2 (32 * 1024)
#define RING_BUFFER_SIZE_3 (16 * 1024)

#define FFT_DEADLINE_US 30000
#define DEBOUNCE_MS 100

// Jangan print log biasa ke Serial karena Serial dipakai audio_streamer.py
#define ENABLE_SERIAL_MONITOR_LOG 0

// =======================================================
// MONITORING CONFIG
// =======================================================
// Ini khusus untuk pengambilan data laporan.
// Output akan dikirim ke Serial dalam format:
// @METRIC, @STACK, @RTOS, @MEM, @AUDIO, @BTN
#define ENABLE_METRIC_LOG 1
#define METRIC_LOG_INTERVAL_MS 5000

// =======================================================
// DEFAULT PLAYLIST FALLBACK
// Python tetap sumber utama nama file + durasi.
// Ini hanya fallback sebelum metadata dari Python masuk.
// =======================================================
#define DEFAULT_TRACK_COUNT 2

const char *DEFAULT_TRACK_NAMES[DEFAULT_TRACK_COUNT] = {
  "test.mp3",
  "test2.mp3"
};

const uint32_t DEFAULT_TRACK_DURATION_MS[DEFAULT_TRACK_COUNT] = {
  5616,
  12770
};

// Metadata dari Python:
// @@TRACK|index|total|filename|duration_ms@@\n
//
// Contoh:
// @@TRACK|1|2|test.mp3|5616@@

// =======================================================
// OLED OBJECTS
// =======================================================
U8G2_SSD1306_128X64_NONAME_F_SW_I2C oledSpectrum(
  U8G2_R0,
  OLED1_SCL,
  OLED1_SDA,
  U8X8_PIN_NONE
);

U8G2_SSD1306_128X64_NONAME_F_SW_I2C oledInfo(
  U8G2_R0,
  OLED2_SCL,
  OLED2_SDA,
  U8X8_PIN_NONE
);

// =======================================================
// RTOS OBJECTS
// =======================================================
RingbufHandle_t audioRingBuffer = NULL;
QueueHandle_t spectrumQueue = NULL;

SemaphoreHandle_t xStatsMutex = NULL;
SemaphoreHandle_t xDisplayMutex = NULL;

// Handle task untuk ISR dan monitoring stack usage
TaskHandle_t gControlTaskHandle = NULL;
TaskHandle_t gAudioTaskHandle = NULL;
TaskHandle_t gDSPTaskHandle = NULL;
TaskHandle_t gSpectrumTaskHandle = NULL;
TaskHandle_t gInfoTaskHandle = NULL;
TaskHandle_t gMonitorTaskHandle = NULL;

// =======================================================
// DATA STRUCTS
// =======================================================
typedef struct {
  uint8_t bars[NUM_BANDS];
  uint32_t frameId;
  uint32_t fftTimeUs;
  uint32_t deadlineMiss;
  float dominantFreq;
} SpectrumFrame;

typedef struct {
  bool visualEnabled;

  uint8_t trackIndex;   // 0-based
  uint8_t trackTotal;

  char trackName[40];
  char status[16];

  uint32_t trackDurationMs;
  uint32_t pcmBytesAccepted;
  uint32_t lastAudioMs;
} PlayerState;

// =======================================================
// MONITORING DATA STRUCT
// =======================================================
typedef struct {
  uint32_t lastUs;
  uint32_t maxUs;
  uint32_t minUs;
  uint64_t totalUs;
  uint32_t count;
} TaskMetric;

TaskMetric mControl  = {0, 0, 0xFFFFFFFF, 0, 0};
TaskMetric mAudio    = {0, 0, 0xFFFFFFFF, 0, 0};
TaskMetric mDSP      = {0, 0, 0xFFFFFFFF, 0, 0};
TaskMetric mSpectrum = {0, 0, 0xFFFFFFFF, 0, 0};
TaskMetric mInfo     = {0, 0, 0xFFFFFFFF, 0, 0};
TaskMetric mMonitor  = {0, 0, 0xFFFFFFFF, 0, 0};

void updateMetric(TaskMetric *m, uint32_t execUs) {
  m->lastUs = execUs;

  if (execUs > m->maxUs) {
    m->maxUs = execUs;
  }

  if (execUs < m->minUs) {
    m->minUs = execUs;
  }

  m->totalUs += execUs;
  m->count++;
}

void printMetricLine(const char *name, TaskMetric *m) {
  if (m->count == 0) {
    Serial.printf("@METRIC|%s|NO_DATA\n", name);
    return;
  }

  uint32_t avgUs = (uint32_t)(m->totalUs / m->count);
  uint32_t jitterUs = 0;

  if (m->minUs != 0xFFFFFFFF && m->maxUs >= m->minUs) {
    jitterUs = m->maxUs - m->minUs;
  }

  Serial.printf(
    "@METRIC|%s|last_us=%lu|max_us=%lu|min_us=%lu|avg_us=%lu|jitter_us=%lu|count=%lu\n",
    name,
    (unsigned long)m->lastUs,
    (unsigned long)m->maxUs,
    (unsigned long)m->minUs,
    (unsigned long)avgUs,
    (unsigned long)jitterUs,
    (unsigned long)m->count
  );
}

void printStackLine(const char *name, TaskHandle_t handle, uint32_t stackSizeBytes) {
  if (handle == NULL) {
    Serial.printf("@STACK|%s|NO_HANDLE\n", name);
    return;
  }

  UBaseType_t freeStackRaw = uxTaskGetStackHighWaterMark(handle);

  uint32_t freeBytes = (uint32_t)freeStackRaw;
  uint32_t usedBytes = 0;
  uint32_t usedPercent = 0;

  if (stackSizeBytes >= freeBytes) {
    usedBytes = stackSizeBytes - freeBytes;
    usedPercent = (usedBytes * 100UL) / stackSizeBytes;
  }

  Serial.printf(
    "@STACK|%s|stack_size=%lu|free_min=%lu|used=%lu|used_percent=%lu\n",
    name,
    (unsigned long)stackSizeBytes,
    (unsigned long)freeBytes,
    (unsigned long)usedBytes,
    (unsigned long)usedPercent
  );
}

// =======================================================
// DSP BUFFERS
// =======================================================
static int16_t audio_samples[SAMPLES];
static double vReal[SAMPLES];
static double vImag[SAMPLES];

// =======================================================
// GLOBAL STATE
// =======================================================
PlayerState gPlayer;

uint32_t gBytesReceived = 0;
uint32_t gBytesAccepted = 0;
uint32_t gRingBufferOverflow = 0;

uint32_t gFFTFrames = 0;
uint32_t gSpectrumFrames = 0;
uint32_t gInfoFrames = 0;
uint32_t gDeadlineMiss = 0;
uint32_t gLastFFTTimeUs = 0;
uint32_t gRingBufferSizeUsed = 0;
float gLastDominantFreq = 0.0;

uint32_t gButtonGreenCount = 0;
uint32_t gButtonYellowCount = 0;

// =======================================================
// PYTHON HEADER PARSER
// =======================================================
const char *TRACK_PREFIX = "@@TRACK|";
const int TRACK_PREFIX_LEN = 8;

bool gInTrackHeader = false;
int gTrackPrefixMatch = 0;
char gTrackHeaderBuf[128];
int gTrackHeaderLen = 0;

// =======================================================
// FUNCTION DECLARATIONS
// =======================================================
void ControlTask(void *pvParameters);
void AudioAcquisitionTask(void *pvParameters);
void DSPTask(void *pvParameters);
void SpectrumDisplayTask(void *pvParameters);
void InfoDisplayTask(void *pvParameters);
void MonitorTask(void *pvParameters);

void initOLEDs();
void drawStartupScreens();
void drawWaitingSpectrum();
void drawPausedSpectrum();
void drawSpectrum(const SpectrumFrame &spectrum);
void drawInfoScreen();

void setVisualizerEnabled(bool enabled);
void nextTrack();
void clearAudioPipeline();

void formatTime(uint32_t ms, char *out, size_t outSize);
uint32_t bytesToMs(uint32_t bytes);
void safeCopy(char *dst, const char *src, size_t dstSize);

void sendPcmToRingBuffer(uint8_t *data, size_t len);
void processIncomingByte(uint8_t b, uint8_t *pcmOut, size_t *pcmLen, size_t pcmOutMax);
void parseTrackHeader(const char *header);

// ISR kecil
void IRAM_ATTR ButtonISR();

// =======================================================
// ISR: BUTTON EVENT
// =======================================================
// ISR hanya memberi notifikasi ke ControlTask.
// Proses debounce, play/pause, dan next tetap dilakukan di ControlTask.
void IRAM_ATTR ButtonISR() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  if (gControlTaskHandle != NULL) {
    vTaskNotifyGiveFromISR(gControlTaskHandle, &xHigherPriorityTaskWoken);
  }

  if (xHigherPriorityTaskWoken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

// =======================================================
// SETUP
// =======================================================
void setup() {
  Serial.begin(SERIAL_BAUDRATE);
  Serial.setTimeout(5);
  delay(1500);

  memset(&gPlayer, 0, sizeof(gPlayer));

  gPlayer.visualEnabled = true;
  gPlayer.trackIndex = 0;
  gPlayer.trackTotal = DEFAULT_TRACK_COUNT;
  safeCopy(gPlayer.trackName, DEFAULT_TRACK_NAMES[0], sizeof(gPlayer.trackName));
  safeCopy(gPlayer.status, "READY", sizeof(gPlayer.status));
  gPlayer.trackDurationMs = DEFAULT_TRACK_DURATION_MS[0];
  gPlayer.pcmBytesAccepted = 0;
  gPlayer.lastAudioMs = 0;

  xStatsMutex = xSemaphoreCreateMutex();
  xDisplayMutex = xSemaphoreCreateMutex();

  initOLEDs();
  drawStartupScreens();

  if (xStatsMutex == NULL || xDisplayMutex == NULL) {
    oledSpectrum.clearBuffer();
    oledSpectrum.setFont(u8g2_font_6x10_tr);
    oledSpectrum.drawStr(0, 12, "FATAL ERROR");
    oledSpectrum.drawStr(0, 28, "Mutex fail");
    oledSpectrum.sendBuffer();

    while (true) {
      delay(1000);
    }
  }

  audioRingBuffer = xRingbufferCreate(RING_BUFFER_SIZE_1, RINGBUF_TYPE_BYTEBUF);
  gRingBufferSizeUsed = RING_BUFFER_SIZE_1;

  if (audioRingBuffer == NULL) {
    audioRingBuffer = xRingbufferCreate(RING_BUFFER_SIZE_2, RINGBUF_TYPE_BYTEBUF);
    gRingBufferSizeUsed = RING_BUFFER_SIZE_2;
  }

  if (audioRingBuffer == NULL) {
    audioRingBuffer = xRingbufferCreate(RING_BUFFER_SIZE_3, RINGBUF_TYPE_BYTEBUF);
    gRingBufferSizeUsed = RING_BUFFER_SIZE_3;
  }

  if (audioRingBuffer == NULL) {
    if (xSemaphoreTake(xDisplayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      oledSpectrum.clearBuffer();
      oledSpectrum.setFont(u8g2_font_6x10_tr);
      oledSpectrum.drawStr(0, 12, "FATAL ERROR");
      oledSpectrum.drawStr(0, 28, "RingBuffer fail");
      oledSpectrum.sendBuffer();
      xSemaphoreGive(xDisplayMutex);
    }

    while (true) {
      delay(1000);
    }
  }

  spectrumQueue = xQueueCreate(4, sizeof(SpectrumFrame));

  if (spectrumQueue == NULL) {
    if (xSemaphoreTake(xDisplayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      oledSpectrum.clearBuffer();
      oledSpectrum.setFont(u8g2_font_6x10_tr);
      oledSpectrum.drawStr(0, 12, "FATAL ERROR");
      oledSpectrum.drawStr(0, 28, "Queue fail");
      oledSpectrum.sendBuffer();
      xSemaphoreGive(xDisplayMutex);
    }

    while (true) {
      delay(1000);
    }
  }

  pinMode(BTN_GREEN_PLAY_STOP, INPUT_PULLUP);
  pinMode(BTN_YELLOW_NEXT, INPUT_PULLUP);

  xTaskCreatePinnedToCore(
    ControlTask,
    "ControlTask",
    4096,
    NULL,
    9,
    &gControlTaskHandle,
    0
  );

  xTaskCreatePinnedToCore(
    AudioAcquisitionTask,
    "AudioAcqTask",
    4096,
    NULL,
    8,
    &gAudioTaskHandle,
    0
  );

  xTaskCreatePinnedToCore(
    DSPTask,
    "DSP_FFT_Task",
    8192,
    NULL,
    6,
    &gDSPTaskHandle,
    1
  );

  xTaskCreatePinnedToCore(
    SpectrumDisplayTask,
    "SpectrumOLED",
    8192,
    NULL,
    3,
    &gSpectrumTaskHandle,
    1
  );

  xTaskCreatePinnedToCore(
    InfoDisplayTask,
    "InfoOLED",
    6144,
    NULL,
    2,
    &gInfoTaskHandle,
    1
  );

  xTaskCreatePinnedToCore(
    MonitorTask,
    "MonitorTask",
    4096,
    NULL,
    1,
    &gMonitorTaskHandle,
    1
  );

  // ISR kecil:
  // CHANGE digunakan agar ControlTask bisa segera bangun saat tombol berubah state.
  attachInterrupt(digitalPinToInterrupt(BTN_GREEN_PLAY_STOP), ButtonISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BTN_YELLOW_NEXT), ButtonISR, CHANGE);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// =======================================================
// OLED FUNCTIONS
// =======================================================
void initOLEDs() {
  oledSpectrum.setI2CAddress(0x78);
  oledInfo.setI2CAddress(0x78);

  oledSpectrum.begin();
  oledInfo.begin();
}

void drawStartupScreens() {
  if (xDisplayMutex != NULL) {
    xSemaphoreTake(xDisplayMutex, pdMS_TO_TICKS(100));
  }

  oledSpectrum.clearBuffer();
  oledSpectrum.setFont(u8g2_font_6x10_tr);
  oledSpectrum.drawStr(0, 10, "RTOS SPECTRUM");
  oledSpectrum.drawStr(0, 24, "OLED 1 ONLY");
  oledSpectrum.drawStr(0, 38, "Input: USB PCM");
  oledSpectrum.drawStr(0, 52, "Ready");
  oledSpectrum.sendBuffer();

  oledInfo.clearBuffer();
  oledInfo.setFont(u8g2_font_6x10_tr);
  oledInfo.drawStr(0, 10, "MP3 PLAYER");
  oledInfo.drawStr(0, 24, "OLED 2 INFO");
  oledInfo.drawStr(0, 38, "Green: PAUSE");
  oledInfo.drawStr(0, 52, "Yellow: NEXT");
  oledInfo.sendBuffer();

  if (xDisplayMutex != NULL) {
    xSemaphoreGive(xDisplayMutex);
  }
}

void drawWaitingSpectrum() {
  if (xSemaphoreTake(xDisplayMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    return;
  }

  oledSpectrum.clearBuffer();
  oledSpectrum.setFont(u8g2_font_6x10_tr);

  oledSpectrum.drawStr(0, 10, "RTOS SPECTRUM");
  oledSpectrum.drawHLine(0, 12, 128);
  oledSpectrum.drawStr(0, 30, "Waiting PCM...");
  oledSpectrum.drawStr(0, 44, "Run streamer.py");

  oledSpectrum.sendBuffer();
  xSemaphoreGive(xDisplayMutex);
}

void drawPausedSpectrum() {
  if (xSemaphoreTake(xDisplayMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    return;
  }

  oledSpectrum.clearBuffer();
  oledSpectrum.setFont(u8g2_font_6x10_tr);

  oledSpectrum.drawStr(0, 10, "RTOS SPECTRUM");
  oledSpectrum.drawHLine(0, 12, 128);
  oledSpectrum.drawStr(0, 30, "PAUSED");
  oledSpectrum.drawStr(0, 44, "Press GREEN");

  oledSpectrum.sendBuffer();
  xSemaphoreGive(xDisplayMutex);
}

void drawSpectrum(const SpectrumFrame &spectrum) {
  if (xSemaphoreTake(xDisplayMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;
  }

  oledSpectrum.clearBuffer();

  oledSpectrum.setFont(u8g2_font_5x7_tr);

  oledSpectrum.drawStr(0, 8, "PLAY");

  char freqText[20];
  snprintf(freqText, sizeof(freqText), "%dHz", (int)spectrum.dominantFreq);
  oledSpectrum.drawStr(35, 8, freqText);

  char missText[20];
  snprintf(missText, sizeof(missText), "M:%lu", (unsigned long)spectrum.deadlineMiss);
  oledSpectrum.drawStr(92, 8, missText);

  oledSpectrum.drawHLine(0, 10, 128);

  for (int i = 0; i < NUM_BANDS; i++) {
    int x = i * 8 + 1;
    int h = spectrum.bars[i];

    if (h > 52) h = 52;
    if (h < 0) h = 0;

    int y = 63 - h;

    if (h > 0) {
      oledSpectrum.drawBox(x, y, 6, h);
    }
  }

  oledSpectrum.sendBuffer();
  xSemaphoreGive(xDisplayMutex);
}

void drawInfoScreen() {
  PlayerState localPlayer;

  if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    localPlayer = gPlayer;
    xSemaphoreGive(xStatsMutex);
  } else {
    memset(&localPlayer, 0, sizeof(localPlayer));
    safeCopy(localPlayer.status, "ERR", sizeof(localPlayer.status));
    safeCopy(localPlayer.trackName, "unknown", sizeof(localPlayer.trackName));
    localPlayer.trackTotal = 1;
  }

  if (localPlayer.trackTotal < 1) {
    localPlayer.trackTotal = 1;
  }

  uint32_t elapsedMs = bytesToMs(localPlayer.pcmBytesAccepted);
  uint32_t durationMs = localPlayer.trackDurationMs;

  if (durationMs > 0 && elapsedMs > durationMs) {
    elapsedMs = durationMs;
  }

  char elapsedText[10];
  char durationText[10];

  formatTime(elapsedMs, elapsedText, sizeof(elapsedText));
  formatTime(durationMs, durationText, sizeof(durationText));

  char nameShown[24];
  safeCopy(nameShown, localPlayer.trackName, sizeof(nameShown));

  // Batasi nama file supaya tidak keluar layar
  if (strlen(nameShown) > 16) {
    nameShown[13] = '.';
    nameShown[14] = '.';
    nameShown[15] = '.';
    nameShown[16] = '\0';
  }

  char statusShown[18];
  safeCopy(statusShown, localPlayer.status, sizeof(statusShown));

  if (localPlayer.visualEnabled) {
    uint32_t now = millis();

    if (localPlayer.lastAudioMs > 0 && (now - localPlayer.lastAudioMs > 1500)) {
      safeCopy(statusShown, "IDLE", sizeof(statusShown));
    }
  }

  if (xSemaphoreTake(xDisplayMutex, pdMS_TO_TICKS(30)) != pdTRUE) {
    return;
  }

  oledInfo.clearBuffer();
  oledInfo.setFont(u8g2_font_5x7_tr);

  oledInfo.drawStr(0, 7, "MP3 PLAYER");
  oledInfo.drawHLine(0, 9, 128);

  char trackLine[32];
  snprintf(trackLine, sizeof(trackLine), "%u/%u %s",
           (unsigned int)(localPlayer.trackIndex + 1),
           (unsigned int)localPlayer.trackTotal,
           nameShown);
  oledInfo.drawStr(0, 21, trackLine);

  char timeLine[32];
  snprintf(timeLine, sizeof(timeLine), "%s / %s", elapsedText, durationText);
  oledInfo.drawStr(0, 35, timeLine);

  char statusLine[32];
  snprintf(statusLine, sizeof(statusLine), "Status: %s", statusShown);
  oledInfo.drawStr(0, 49, statusLine);

  char controlLine[32];
  snprintf(controlLine, sizeof(controlLine), "Y:NEXT G:%s",
           localPlayer.visualEnabled ? "PAUSE" : "PLAY");
  oledInfo.drawStr(0, 63, controlLine);

  oledInfo.sendBuffer();
  xSemaphoreGive(xDisplayMutex);

  if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    gInfoFrames++;
    xSemaphoreGive(xStatsMutex);
  }
}

// =======================================================
// TASK 1: CONTROL TASK
// =======================================================
void ControlTask(void *pvParameters) {
  int lastGreenRaw = HIGH;
  int lastYellowRaw = HIGH;

  int stableGreen = HIGH;
  int stableYellow = HIGH;

  uint32_t lastGreenChangeMs = 0;
  uint32_t lastYellowChangeMs = 0;

  while (true) {
    uint32_t startUs = micros();

    uint32_t now = millis();

    int greenRaw = digitalRead(BTN_GREEN_PLAY_STOP);
    int yellowRaw = digitalRead(BTN_YELLOW_NEXT);

    // GREEN debounce
    if (greenRaw != lastGreenRaw) {
      lastGreenChangeMs = now;
      lastGreenRaw = greenRaw;
    }

    if ((now - lastGreenChangeMs) > DEBOUNCE_MS) {
      if (greenRaw != stableGreen) {
        stableGreen = greenRaw;

        if (stableGreen == LOW) {
          bool currentlyEnabled = true;

          if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            currentlyEnabled = gPlayer.visualEnabled;
            gButtonGreenCount++;
            xSemaphoreGive(xStatsMutex);
          }

          setVisualizerEnabled(!currentlyEnabled);
        }
      }
    }

    // YELLOW debounce
    if (yellowRaw != lastYellowRaw) {
      lastYellowChangeMs = now;
      lastYellowRaw = yellowRaw;
    }

    if ((now - lastYellowChangeMs) > DEBOUNCE_MS) {
      if (yellowRaw != stableYellow) {
        stableYellow = yellowRaw;

        if (stableYellow == LOW) {
          if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            gButtonYellowCount++;
            xSemaphoreGive(xStatsMutex);
          }

          nextTrack();
        }
      }
    }

    uint32_t execUs = micros() - startUs;

    if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      updateMetric(&mControl, execUs);
      xSemaphoreGive(xStatsMutex);
    }

    // ControlTask bangun oleh ButtonISR, atau tetap bangun timeout 10 ms.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
  }
}

// =======================================================
// TASK 2: AUDIO ACQUISITION
// Baca PCM + metadata track dari audio_streamer.py
// =======================================================
void AudioAcquisitionTask(void *pvParameters) {
  uint8_t rx_temp[256];
  uint8_t pcmOut[256];

  while (true) {
    int available = Serial.available();

    if (available > 0) {
      uint32_t startUs = micros();

      int to_read = min(available, (int)sizeof(rx_temp));
      int read_bytes = Serial.readBytes(rx_temp, to_read);

      if (read_bytes > 0) {
        if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
          gBytesReceived += read_bytes;
          xSemaphoreGive(xStatsMutex);
        }

        size_t pcmLen = 0;

        for (int i = 0; i < read_bytes; i++) {
          processIncomingByte(rx_temp[i], pcmOut, &pcmLen, sizeof(pcmOut));

          if (pcmLen >= sizeof(pcmOut)) {
            sendPcmToRingBuffer(pcmOut, pcmLen);
            pcmLen = 0;
          }
        }

        if (pcmLen > 0) {
          sendPcmToRingBuffer(pcmOut, pcmLen);
          pcmLen = 0;
        }
      }

      uint32_t execUs = micros() - startUs;

      if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        updateMetric(&mAudio, execUs);
        xSemaphoreGive(xStatsMutex);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

void processIncomingByte(uint8_t b, uint8_t *pcmOut, size_t *pcmLen, size_t pcmOutMax) {
  if (gInTrackHeader) {
    if (b == '\n' || b == '\r') {
      gTrackHeaderBuf[gTrackHeaderLen] = '\0';
      parseTrackHeader(gTrackHeaderBuf);

      gInTrackHeader = false;
      gTrackHeaderLen = 0;
      gTrackPrefixMatch = 0;
      return;
    }

    if (gTrackHeaderLen < (int)sizeof(gTrackHeaderBuf) - 1) {
      gTrackHeaderBuf[gTrackHeaderLen++] = (char)b;
    }

    return;
  }

  if (b == (uint8_t)TRACK_PREFIX[gTrackPrefixMatch]) {
    gTrackPrefixMatch++;

    if (gTrackPrefixMatch >= TRACK_PREFIX_LEN) {
      gInTrackHeader = true;
      gTrackHeaderLen = 0;
      gTrackPrefixMatch = 0;
    }

    return;
  }

  if (gTrackPrefixMatch > 0) {
    for (int i = 0; i < gTrackPrefixMatch; i++) {
      if (*pcmLen < pcmOutMax) {
        pcmOut[*pcmLen] = (uint8_t)TRACK_PREFIX[i];
        (*pcmLen)++;
      }
    }

    gTrackPrefixMatch = 0;
  }

  if (*pcmLen < pcmOutMax) {
    pcmOut[*pcmLen] = b;
    (*pcmLen)++;
  }
}

void parseTrackHeader(const char *header) {
  // header setelah prefix:
  // index|total|filename|duration_ms@@

  char temp[128];
  safeCopy(temp, header, sizeof(temp));

  char *endMark = strstr(temp, "@@");
  if (endMark != NULL) {
    *endMark = '\0';
  }

  char *p1 = strtok(temp, "|");
  char *p2 = strtok(NULL, "|");
  char *p3 = strtok(NULL, "|");
  char *p4 = strtok(NULL, "|");

  if (p1 == NULL || p2 == NULL || p3 == NULL || p4 == NULL) {
    return;
  }

  int index1Based = atoi(p1);
  int total = atoi(p2);
  uint32_t duration = (uint32_t)atol(p4);

  if (index1Based < 1) index1Based = 1;
  if (total < 1) total = 1;
  if (total > 99) total = 99;

  if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    gPlayer.trackIndex = (uint8_t)(index1Based - 1);
    gPlayer.trackTotal = (uint8_t)total;
    safeCopy(gPlayer.trackName, p3, sizeof(gPlayer.trackName));
    gPlayer.trackDurationMs = duration;
    gPlayer.pcmBytesAccepted = 0;
    gPlayer.lastAudioMs = 0;
    gPlayer.visualEnabled = true;
    safeCopy(gPlayer.status, "READY", sizeof(gPlayer.status));
    xSemaphoreGive(xStatsMutex);
  }

  clearAudioPipeline();
}

void sendPcmToRingBuffer(uint8_t *data, size_t len) {
  if (len == 0) {
    return;
  }

  bool enabled = true;

  if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    enabled = gPlayer.visualEnabled;
    xSemaphoreGive(xStatsMutex);
  }

  if (!enabled) {
    return;
  }

  BaseType_t res = xRingbufferSend(
    audioRingBuffer,
    data,
    len,
    pdMS_TO_TICKS(5)
  );

  if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    if (res == pdTRUE) {
      gBytesAccepted += len;
      gPlayer.pcmBytesAccepted += len;
      gPlayer.lastAudioMs = millis();
      safeCopy(gPlayer.status, "PLAY", sizeof(gPlayer.status));
    } else {
      gRingBufferOverflow++;
    }

    xSemaphoreGive(xStatsMutex);
  }
}

// =======================================================
// TASK 3: DSP / FFT
// =======================================================
void DSPTask(void *pvParameters) {
  const size_t bytes_to_read = SAMPLES * sizeof(int16_t);

  arduinoFFT FFT = arduinoFFT(vReal, vImag, SAMPLES, SAMPLING_FREQUENCY);

  uint32_t frameCounter = 0;

  while (true) {
    size_t bytes_received = 0;
    uint8_t *p_dest = (uint8_t *)audio_samples;

    while (bytes_received < bytes_to_read) {
      size_t item_size = 0;

      void *item = xRingbufferReceiveUpTo(
        audioRingBuffer,
        &item_size,
        pdMS_TO_TICKS(50),
        bytes_to_read - bytes_received
      );

      if (item != NULL) {
        memcpy(p_dest + bytes_received, item, item_size);
        bytes_received += item_size;
        vRingbufferReturnItem(audioRingBuffer, item);
      } else {
        vTaskDelay(pdMS_TO_TICKS(2));
      }
    }

    uint32_t startUs = micros();

    double mean = 0.0;

    for (int i = 0; i < SAMPLES; i++) {
      mean += (double)audio_samples[i];
    }

    mean /= SAMPLES;

    for (int i = 0; i < SAMPLES; i++) {
      vReal[i] = (double)audio_samples[i] - mean;
      vImag[i] = 0.0;
    }

    FFT.Windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    FFT.Compute(FFT_FORWARD);
    FFT.ComplexToMagnitude();

    SpectrumFrame out;
    memset(&out, 0, sizeof(out));

    out.frameId = frameCounter++;

    double maxMag = 0.0;
    int maxIndex = 1;

    for (int i = 1; i < SAMPLES / 2; i++) {
      if (vReal[i] > maxMag) {
        maxMag = vReal[i];
        maxIndex = i;
      }
    }

    out.dominantFreq = ((float)maxIndex * (float)SAMPLING_FREQUENCY) / (float)SAMPLES;

    int bins_per_band = (SAMPLES / 2) / NUM_BANDS;
    double bandValues[NUM_BANDS];
    double bandMax = 0.0;

    for (int band = 0; band < NUM_BANDS; band++) {
      double sum = 0.0;

      for (int b = 0; b < bins_per_band; b++) {
        int bin_idx = (band * bins_per_band) + b + 1;

        if (bin_idx < SAMPLES / 2) {
          sum += vReal[bin_idx];
        }
      }

      double avg = sum / bins_per_band;
      bandValues[band] = avg;

      if (avg > bandMax) {
        bandMax = avg;
      }
    }

    if (bandMax < 1.0) {
      bandMax = 1.0;
    }

    for (int band = 0; band < NUM_BANDS; band++) {
      double normalized = bandValues[band] / bandMax;

      int hLinear = (int)(normalized * 48.0);
      int hLog = (int)(8.0 * log10(bandValues[band] + 1.0));

      int h = hLinear;

      if (hLog > h) {
        h = hLog;
      }

      if (h > 52) h = 52;
      if (h < 0) h = 0;

      out.bars[band] = (uint8_t)h;
    }

    uint32_t fftTime = micros() - startUs;
    out.fftTimeUs = fftTime;

    if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      updateMetric(&mDSP, fftTime);

      gFFTFrames++;
      gLastFFTTimeUs = fftTime;
      gLastDominantFreq = out.dominantFreq;

      if (fftTime > FFT_DEADLINE_US) {
        gDeadlineMiss++;
      }

      out.deadlineMiss = gDeadlineMiss;
      xSemaphoreGive(xStatsMutex);
    }

    if (xQueueSend(spectrumQueue, &out, 0) != pdTRUE) {
      SpectrumFrame dummy;
      xQueueReceive(spectrumQueue, &dummy, 0);
      xQueueSend(spectrumQueue, &out, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// =======================================================
// TASK 4: OLED 1 SPECTRUM ONLY
// =======================================================
void SpectrumDisplayTask(void *pvParameters) {
  SpectrumFrame spectrum;
  uint32_t lastFrameMs = 0;
  uint32_t lastDrawMs = 0;

  drawWaitingSpectrum();

  while (true) {
    bool enabled = true;

    if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      enabled = gPlayer.visualEnabled;
      xSemaphoreGive(xStatsMutex);
    }

    if (!enabled) {
      drawPausedSpectrum();
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }

    if (xQueueReceive(spectrumQueue, &spectrum, pdMS_TO_TICKS(50)) == pdTRUE) {
      uint32_t now = millis();

      // Batasi OLED1 sekitar 25 FPS supaya OLED2 tidak patah-patah
      if (now - lastDrawMs >= 40) {
        uint32_t startUs = micros();

        drawSpectrum(spectrum);

        uint32_t execUs = micros() - startUs;

        lastDrawMs = now;

        if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
          updateMetric(&mSpectrum, execUs);
          gSpectrumFrames++;
          xSemaphoreGive(xStatsMutex);
        }
      }

      lastFrameMs = now;
    } else {
      if (millis() - lastFrameMs > 1000) {
        drawWaitingSpectrum();
        lastFrameMs = millis();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// =======================================================
// TASK 5: OLED 2 INFO ONLY
// =======================================================
void InfoDisplayTask(void *pvParameters) {
  while (true) {
    uint32_t startUs = micros();

    drawInfoScreen();

    uint32_t execUs = micros() - startUs;

    if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      updateMetric(&mInfo, execUs);
      xSemaphoreGive(xStatsMutex);
    }

    // OLED2 cukup update 2x per detik
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// =======================================================
// TASK 6: MONITOR
// =======================================================
void MonitorTask(void *pvParameters) {
  while (true) {
    uint32_t startUs = micros();

#if ENABLE_SERIAL_MONITOR_LOG
    Serial.println("Monitor disabled by default for audio streaming.");
#endif

#if ENABLE_METRIC_LOG
    if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      Serial.println("@METRIC_BEGIN");

      printMetricLine("ControlTask", &mControl);
      printMetricLine("AudioTask", &mAudio);
      printMetricLine("DSPTask", &mDSP);
      printMetricLine("SpectrumTask", &mSpectrum);
      printMetricLine("InfoTask", &mInfo);
      printMetricLine("MonitorTask", &mMonitor);

      printStackLine("ControlTask", gControlTaskHandle, 4096);
      printStackLine("AudioTask", gAudioTaskHandle, 4096);
      printStackLine("DSPTask", gDSPTaskHandle, 8192);
      printStackLine("SpectrumTask", gSpectrumTaskHandle, 8192);
      printStackLine("InfoTask", gInfoTaskHandle, 6144);
      printStackLine("MonitorTask", gMonitorTaskHandle, 4096);

      Serial.printf(
        "@RTOS|fft_last_us=%lu|fft_max_us=%lu|fft_deadline_us=%lu|deadline_miss=%lu|dominant_hz=%d\n",
        (unsigned long)gLastFFTTimeUs,
        (unsigned long)mDSP.maxUs,
        (unsigned long)FFT_DEADLINE_US,
        (unsigned long)gDeadlineMiss,
        (int)gLastDominantFreq
      );

      Serial.printf(
        "@MEM|free_heap=%lu|min_free_heap=%lu|ring_buffer_used=%lu\n",
        (unsigned long)ESP.getFreeHeap(),
        (unsigned long)ESP.getMinFreeHeap(),
        (unsigned long)gRingBufferSizeUsed
      );

      Serial.printf(
        "@AUDIO|rx_bytes=%lu|accepted_bytes=%lu|ring_overflow=%lu|fft_frames=%lu|spectrum_frames=%lu|info_frames=%lu\n",
        (unsigned long)gBytesReceived,
        (unsigned long)gBytesAccepted,
        (unsigned long)gRingBufferOverflow,
        (unsigned long)gFFTFrames,
        (unsigned long)gSpectrumFrames,
        (unsigned long)gInfoFrames
      );

      Serial.printf(
        "@BTN|green_count=%lu|yellow_count=%lu|debounce_ms=%u|polling_ms=10|wakeup=ISR_notification\n",
        (unsigned long)gButtonGreenCount,
        (unsigned long)gButtonYellowCount,
        (unsigned int)DEBOUNCE_MS
      );

      Serial.println("@METRIC_END");

      xSemaphoreGive(xStatsMutex);
    }
#endif

    uint32_t execUs = micros() - startUs;

    if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      updateMetric(&mMonitor, execUs);
      xSemaphoreGive(xStatsMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(METRIC_LOG_INTERVAL_MS));
  }
}

// =======================================================
// UTILS
// =======================================================
void setVisualizerEnabled(bool enabled) {
  if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    gPlayer.visualEnabled = enabled;

    if (enabled) {
      safeCopy(gPlayer.status, "READY", sizeof(gPlayer.status));
      gPlayer.lastAudioMs = 0;
    } else {
      safeCopy(gPlayer.status, "PAUSED", sizeof(gPlayer.status));
      gPlayer.lastAudioMs = 0;
    }

    xSemaphoreGive(xStatsMutex);
  }

  clearAudioPipeline();
}

void nextTrack() {
  if (xSemaphoreTake(xStatsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    uint8_t total = gPlayer.trackTotal;

    if (total < 1) {
      total = DEFAULT_TRACK_COUNT;
    }

    uint8_t next = (gPlayer.trackIndex + 1) % total;

    gPlayer.trackIndex = next;
    gPlayer.pcmBytesAccepted = 0;
    gPlayer.lastAudioMs = 0;
    gPlayer.visualEnabled = true;

    if (next < DEFAULT_TRACK_COUNT) {
      safeCopy(gPlayer.trackName, DEFAULT_TRACK_NAMES[next], sizeof(gPlayer.trackName));
      gPlayer.trackDurationMs = DEFAULT_TRACK_DURATION_MS[next];
    }

    safeCopy(gPlayer.status, "NEXT...", sizeof(gPlayer.status));

    xSemaphoreGive(xStatsMutex);
  }

  clearAudioPipeline();

  // Dibaca oleh audio_streamer.py.
  Serial.println("NEXT");
}

void clearAudioPipeline() {
  if (audioRingBuffer != NULL) {
    while (true) {
      size_t item_size = 0;

      void *item = xRingbufferReceiveUpTo(
        audioRingBuffer,
        &item_size,
        0,
        1024
      );

      if (item == NULL) {
        break;
      }

      vRingbufferReturnItem(audioRingBuffer, item);
    }
  }

  if (spectrumQueue != NULL) {
    xQueueReset(spectrumQueue);
  }
}

void formatTime(uint32_t ms, char *out, size_t outSize) {
  uint32_t totalSec = ms / 1000;
  uint32_t min = totalSec / 60;
  uint32_t sec = totalSec % 60;

  snprintf(out, outSize, "%02lu:%02lu", (unsigned long)min, (unsigned long)sec);
}

uint32_t bytesToMs(uint32_t bytes) {
  return (uint32_t)(((uint64_t)bytes * 1000ULL) /
                    ((uint64_t)SAMPLING_FREQUENCY * 2ULL));
}

void safeCopy(char *dst, const char *src, size_t dstSize) {
  if (dstSize == 0) {
    return;
  }

  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}