#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <arduinoFFT.h>

// Definisikan Pin I2C untuk OLED 0.96"
// Silakan sesuaikan pin ini dengan koneksi fisik pada board ESP32-S3 Anda
#define I2C_SDA 8
#define I2C_SCL 9

// Parameter Audio
#define SAMPLING_FREQUENCY 22050 // Frekuensi sampling audio (Hz)
#define SAMPLES 512              // Harus berpangkat 2 (e.g., 128, 256, 512, 1024)
#define NUM_BANDS 16             // Jumlah pita frekuensi visualisasi spektrum

// Ukuran Ring Buffer untuk menyangga data audio masuk (64 KB)
#define RING_BUFFER_SIZE (64 * 1024)

// Include FreeRTOS Headers
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

// Handle untuk Ring Buffer
RingbufHandle_t audioRingBuffer = NULL;

// Mutex untuk melindungi data spektrum yang dibagikan antar-task
SemaphoreHandle_t xSpectrumMutex = NULL;
uint8_t shared_spectrum_bars[NUM_BANDS] = {0};

// Deklarasi Task
void AudioAcquisitionTask(void *pvParameters);
void DSPTask(void *pvParameters);
void OLEDDisplayTask(void *pvParameters);

void setup() {
  // Inisialisasi USB Serial dengan kecepatan tinggi (USB CDC akan berjalan pada kecepatan penuh USB)
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n=== Memulai Sistem Signal Processing & Acquisition ===");

  // 1. Inisialisasi PSRAM untuk Alokasi Memori yang Efisien
  if (psramInit()) {
    Serial.printf("PSRAM terdeteksi! Total: %d bytes, Sisa: %d bytes\n", ESP.getPsramSize(), ESP.getFreePsram());
  } else {
    Serial.println("PSRAM TIDAK TERDETEKSI! Sistem akan menggunakan Internal SRAM.");
  }

  // 2. Alokasi Ring Buffer FreeRTOS di PSRAM (jika ada) untuk menghindari race condition data audio
  if (ESP.getPsramSize() > 0) {
    uint8_t* puiRingbufferStorage = (uint8_t*)heap_caps_malloc(RING_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    StaticRingbuffer_t* pxStaticRingbuffer = (StaticRingbuffer_t*)heap_caps_malloc(sizeof(StaticRingbuffer_t), MALLOC_CAP_SPIRAM);
    
    if (puiRingbufferStorage != NULL && pxStaticRingbuffer != NULL) {
      audioRingBuffer = xRingbufferCreateStatic(RING_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF, puiRingbufferStorage, pxStaticRingbuffer);
      if (audioRingBuffer != NULL) {
        Serial.println("Ring Buffer berhasil dibuat di PSRAM.");
      }
    }
  }

  if (audioRingBuffer == NULL) {
    Serial.println("Gagal mengalokasi di PSRAM atau PSRAM tidak aktif. Mencoba alokasi di SRAM internal...");
    audioRingBuffer = xRingbufferCreate(RING_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
  }


  if (audioRingBuffer == NULL) {
    Serial.println("FATAL: Gagal membuat Ring Buffer! Sistem berhenti.");
    while (1) { delay(1000); }
  } else {
    Serial.println("Ring Buffer berhasil dibuat.");
  }

  // 3. Buat Mutex untuk sinkronisasi data spektrum antara DSP Task dan Display Task
  xSpectrumMutex = xSemaphoreCreateMutex();
  if (xSpectrumMutex == NULL) {
    Serial.println("FATAL: Gagal membuat Mutex! Sistem berhenti.");
    while (1) { delay(1000); }
  }

  // 4. Inisialisasi Jalur Komunikasi I2C dengan Kecepatan Tinggi (400 kHz) untuk OLED
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  Serial.println("I2C berhasil diinisialisasi pada kecepatan 400kHz.");

  // 5. Buat Task FreeRTOS dan Pin ke Core tertentu untuk pemrosesan paralel yang mulus
  // Task 1: Akuisisi Data Audio (Core 0, Prioritas Tinggi)
  xTaskCreatePinnedToCore(
    AudioAcquisitionTask,
    "AudioAcqTask",
    3 * 1024,         // Stack size (3 KB)
    NULL,
    10,               // Prioritas tinggi agar tidak ada data serial yang hilang
    NULL,
    0                 // Dipin ke Core 0
  );

  // Task 2: Digital Signal Processing / FFT (Core 1, Prioritas Sedang)
  xTaskCreatePinnedToCore(
    DSPTask,
    "DSP_FFT_Task",
    8 * 1024,         // Stack size (8 KB - aman untuk double precision math)
    NULL,
    5,                // Prioritas sedang
    NULL,
    1                 // Dipin ke Core 1 (Core pengolah data)
  );

  // Task 3: Visualisasi OLED (Core 1, Prioritas Rendah)
  xTaskCreatePinnedToCore(
    OLEDDisplayTask,
    "DisplayTask",
    4 * 1024,         // Stack size (4 KB)
    NULL,
    2,                // Prioritas rendah
    NULL,
    1                 // Dipin ke Core 1 bersama dengan DSP
  );

  Serial.println("Semua task FreeRTOS telah dijadwalkan.");
}

void loop() {
  // Loop utama dibiarkan kosong karena FreeRTOS menangani semua operasi lewat Tasks
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// ==========================================
// TASK 1: Akuisisi Data Audio dari USB Serial
// ==========================================
void AudioAcquisitionTask(void *pvParameters) {
  uint8_t rx_temp[512]; // Buffer lokal sementara untuk membaca serial
  
  while (1) {
    int available = Serial.available();
    if (available > 0) {
      int to_read = min(available, (int)sizeof(rx_temp));
      int read_bytes = Serial.readBytes(rx_temp, to_read);
      
      if (read_bytes > 0) {
        // Kirim data audio PCM masuk ke Ring Buffer
        // Menggunakan timeout singkat 10ms agar tidak memblokir akuisisi
        BaseType_t res = xRingbufferSend(audioRingBuffer, rx_temp, read_bytes, pdMS_TO_TICKS(10));
        if (res != pdTRUE) {
          // Ring buffer penuh, terjadi overflow data (laptop mengirim data terlalu cepat)
          // Ini adalah penanda penting bagi perancangan memory & bandwidth
          // Diabaikan sementara untuk menjaga stabilitas sistem
        }
      }
    } else {
      // Jika tidak ada data, lepas kendali CPU selama 1ms untuk mencegah starvation
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

// ==========================================
// TASK 2: Pengolahan Sinyal Digital (FFT)
// ==========================================
void DSPTask(void *pvParameters) {
  const size_t bytes_to_read = SAMPLES * sizeof(int16_t); // 512 sampel * 2 byte = 1024 byte
  
  // Alokasikan buffer pemrosesan secara dinamis di PSRAM agar SRAM tetap bersih
  int16_t* audio_samples = (int16_t*)heap_caps_malloc(bytes_to_read, MALLOC_CAP_SPIRAM);
  double* vReal = (double*)heap_caps_malloc(SAMPLES * sizeof(double), MALLOC_CAP_SPIRAM);
  double* vImag = (double*)heap_caps_malloc(SAMPLES * sizeof(double), MALLOC_CAP_SPIRAM);

  if (audio_samples == NULL || vReal == NULL || vImag == NULL) {
    Serial.println("DSP TASK ERROR: Gagal mengalokasi buffer pemrosesan di PSRAM!");
    vTaskDelete(NULL);
  }

  // Inisialisasi library FFT
  arduinoFFT FFT = arduinoFFT(vReal, vImag, SAMPLES, SAMPLING_FREQUENCY);

  while (1) {
    size_t bytes_received = 0;
    uint8_t* p_dest = (uint8_t*)audio_samples;

    // Baca data biner mentah hingga terkumpul tepat 1024 byte (512 sampel)
    while (bytes_received < bytes_to_read) {
      size_t item_size = 0;
      // Ambil data dari Ring Buffer secara non-blocking dengan timeout 50ms
      void* item = xRingbufferReceiveUpTo(audioRingBuffer, &item_size, pdMS_TO_TICKS(50), bytes_to_read - bytes_received);
      
      if (item != NULL) {
        memcpy(p_dest + bytes_received, item, item_size);
        bytes_received += item_size;
        vRingbufferReturnItem(audioRingBuffer, item); // Wajib kembalikan item buffer
      } else {
        // Jika data belum lengkap, tunggu sejenak sebelum mencoba lagi
        vTaskDelay(pdMS_TO_TICKS(2));
      }
    }

    // Konversi data PCM 16-bit ke representasi double untuk FFT
    for (int i = 0; i < SAMPLES; i++) {
      vReal[i] = (double)audio_samples[i];
      vImag[i] = 0.0;
    }

    // Lakukan windowing (Hamming Window) untuk mengurangi kebocoran spektral (spectral leakage)
    FFT.Windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);

    // Hitung FFT
    FFT.Compute(FFT_FORWARD);

    // Ambil nilai Magnitudo Kompleks (mengubah nilai riil & imajiner menjadi nilai mutlak amplitudo)
    FFT.ComplexToMagnitude();

    // Petakan bin FFT ke 16 pita frekuensi (frequency bands)
    // Frekuensi nyquist = SAMPLING_FREQUENCY / 2 = 11025 Hz
    // Resolusi frekuensi per bin = 22050 / 512 = ~43 Hz per bin
    // Kita abaikan komponen DC (bin 0) dan kelompokkan bin sisanya secara logaritmik atau linier
    uint8_t local_bars[NUM_BANDS] = {0};
    int bins_per_band = (SAMPLES / 2) / NUM_BANDS; // 256 bin / 16 band = 16 bin per band

    for (int band = 0; band < NUM_BANDS; band++) {
      double sum = 0;
      for (int b = 0; b < bins_per_band; b++) {
        int bin_idx = (band * bins_per_band) + b + 1; // Mulai dari bin 1
        if (bin_idx < SAMPLES / 2) {
          sum += vReal[bin_idx];
        }
      }
      double avg = sum / bins_per_band;

      // Skala logaritmik agar visualisasi amplitudo audio terlihat natural di mata
      // Tinggi layar OLED 64px, kita gunakan max tinggi spektrum 52px (sisa untuk text header)
      int bar_height = (int)(12.0 * log10(avg + 1.0));
      if (bar_height > 64) bar_height = 64;
      if (bar_height < 0) bar_height = 0;

      local_bars[band] = bar_height;
    }

    // Update data spektrum bersama yang aman dari Race Condition menggunakan Mutex
    if (xSpectrumMutex != NULL) {
      if (xSemaphoreTake(xSpectrumMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        memcpy(shared_spectrum_bars, local_bars, NUM_BANDS);
        xSemaphoreGive(xSpectrumMutex);
      }
    }

    // Istirahatkan task sejenak untuk membiarkan FreeRTOS menjalankan tugas lain
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ==========================================
// TASK 3: Visualisasi Spektrum pada Layar OLED
// ==========================================
void OLEDDisplayTask(void *pvParameters) {
  // Inisialisasi U8g2 untuk SSD1306 128x64 dengan Hardware I2C
  // Menggunakan Full Buffer (_F_) untuk grafis yang mulus tanpa flicker
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ I2C_SCL, /* data=*/ I2C_SDA);
  
  if (!u8g2.begin()) {
    Serial.println("OLED TASK ERROR: Inisialisasi layar OLED gagal!");
    vTaskDelete(NULL);
  }

  uint8_t display_bars[NUM_BANDS] = {0};
  uint8_t peak_bars[NUM_BANDS] = {0}; // Menyimpan puncak spektrum (fall decay peak effect)

  while (1) {
    uint8_t raw_bars[NUM_BANDS] = {0};

    // Ambil data spektrum terbaru dari DSP task dengan aman menggunakan Mutex
    if (xSpectrumMutex != NULL) {
      if (xSemaphoreTake(xSpectrumMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        memcpy(raw_bars, shared_spectrum_bars, NUM_BANDS);
        xSemaphoreGive(xSpectrumMutex);
      }
    }

    // Algoritma Decay (Peredaman) untuk animasi spektrum yang halus (tidak melompat kasar)
    for (int i = 0; i < NUM_BANDS; i++) {
      // Peredaman batang utama
      if (raw_bars[i] > display_bars[i]) {
        display_bars[i] = raw_bars[i]; // Naik secara instan
      } else {
        if (display_bars[i] > 1) {
          display_bars[i] -= 2; // Turun secara perlahan (decay)
        } else {
          display_bars[i] = 0;
        }
      }

      // Peredaman titik puncak (peak dot)
      if (display_bars[i] >= peak_bars[i]) {
        peak_bars[i] = display_bars[i];
      } else {
        if (peak_bars[i] > 0) {
          peak_bars[i] -= 1; // Titik puncak turun lebih lambat dari batang utama
        }
      }
    }

    // Render Gambar ke Frame Buffer OLED
    u8g2.clearBuffer();

    // 1. Gambar Header Informasi
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(0, 8, "ESP32-S3 DSP SPECTRUM");
    u8g2.drawHLine(0, 10, 128); // Garis pemisah horizontal

    // 2. Gambar Batang Spektrum (16 band)
    // Lebar layar = 128 piksel. 
    // 128px / 16 band = 8px per band.
    // Kita buat lebar batang = 6px, dengan celah (gap) = 2px.
    // Tinggi area spektrum = 64px - 12px (header) = 52px (skala tinggi 0 sampai 52)
    for (int i = 0; i < NUM_BANDS; i++) {
      int x = i * 8 + 1;
      int h = (display_bars[i] * 52) / 64; // Konversi skala 0-64 ke 0-52px
      int y = 64 - h;

      // Gambar batang aktif (isi penuh)
      u8g2.drawBox(x, y, 6, h);

      // Gambar titik puncak (peak dot) di atas batang aktif
      int peak_h = (peak_bars[i] * 52) / 64;
      int peak_y = 64 - peak_h;
      if (peak_y < 63 && peak_y >= 12) {
        u8g2.drawHLine(x, peak_y, 6);
      }
    }

    // Kirim buffer ke layar fisik OLED
    u8g2.sendBuffer();

    // Batasi FPS render layar di kisaran ~30 FPS agar tidak membuang resource CPU sia-sia
    vTaskDelay(pdMS_TO_TICKS(33));
  }
}
