#include <Arduino.h>
#include <unity.h>

// Fungsi matematika sederhana yang ingin kita uji
int tambah(int a, int b) {
    return a + b;
}

// Kasus Uji 1: Memastikan fungsi penambahan berjalan dengan benar
void test_fungsi_tambah() {
    TEST_ASSERT_EQUAL(5, tambah(2, 3));
}

// Kasus Uji 2: Memastikan pin LED terkonfigurasi dengan benar
void test_led_pin_state() {
    pinMode(2, OUTPUT);
    digitalWrite(2, HIGH);
    TEST_ASSERT_EQUAL(HIGH, digitalRead(2));
}

void setup() {
    // Beri waktu 2 detik untuk serial monitor terkoneksi
    delay(2000);

    UNITY_BEGIN(); // Memulai pengujian Unity

    RUN_TEST(test_fungsi_tambah);
    RUN_TEST(test_led_pin_state);

    UNITY_END(); // Mengakhiri pengujian Unity
}

void loop() {
    // Tidak membutuhkan perulangan untuk pengujian unit
}
