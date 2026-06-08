#include <Arduino.h>

// Definisikan pin LED bawaan (biasanya GPIO 2 pada board ESP32 DevKit)
#define LED_PIN 2

void setup() {
  // Inisialisasi Serial Monitor dengan baud rate 115200
  Serial.begin(115200);
  delay(1000);
  Serial.println("ESP32 berhasil terhubung dan menyala!");

  // Atur pin LED sebagai OUTPUT
  pinMode(LED_PIN, OUTPUT);
}

void loop() {
  // Nyalakan LED
  digitalWrite(LED_PIN, HIGH);
  Serial.println("LED Menyala (HIGH)");
  delay(1000);

  // Matikan LED
  digitalWrite(LED_PIN, LOW);
  Serial.println("LED Mati (LOW)");
  delay(1000);
}
