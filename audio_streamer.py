import os
import time
import serial
import miniaudio
from serial.tools import list_ports

# =======================================================
# CONFIG
# =======================================================
BAUDRATE = 115200
SAMPLE_RATE = 22050
CHANNELS = 1
SAMPLE_WIDTH_BYTES = 2
CHUNK_SIZE = 1024

# True = kirim audio mengikuti durasi real-time
# Jangan ubah ke False untuk demo, karena bisa bikin ESP/OLED kewalahan
REALTIME_STREAM = True

# =======================================================
# SERIAL LINE READER
# Untuk membaca command/text dari ESP tanpa mengganggu streaming audio
# =======================================================
class SerialLineReader:
    def __init__(self):
        self.buffer = b""

    def read_lines(self, ser):
        lines = []

        try:
            waiting = ser.in_waiting
        except Exception:
            waiting = 0

        if waiting <= 0:
            return lines

        try:
            data = ser.read(waiting)
        except Exception:
            return lines

        if not data:
            return lines

        self.buffer += data

        while b"\n" in self.buffer:
            raw_line, self.buffer = self.buffer.split(b"\n", 1)

            try:
                line = raw_line.decode("utf-8", errors="ignore").strip()
            except Exception:
                line = ""

            if line:
                lines.append(line)

        # Jaga buffer supaya tidak membesar kalau ada data aneh tanpa newline
        if len(self.buffer) > 512:
            self.buffer = self.buffer[-128:]

        return lines


# =======================================================
# PORT SELECTION
# =======================================================
def pilih_port():
    ports = list(list_ports.comports())

    print("Port COM tersedia:")

    if len(ports) == 0:
        print("Tidak ada port COM terdeteksi.")
        return None

    for i, p in enumerate(ports):
        print(f"[{i}] {p.device} - {p.description}")

    if len(ports) == 1:
        selected = ports[0].device
        print(f"Memilih port otomatis: {selected}")
        return selected

    while True:
        try:
            idx = int(input("Pilih nomor port COM: "))
            if 0 <= idx < len(ports):
                return ports[idx].device
            print("Nomor port tidak valid.")
        except ValueError:
            print("Masukkan angka yang valid.")


# =======================================================
# PLAYLIST INPUT
# =======================================================
def minta_playlist():
    while True:
        try:
            jumlah = int(input("Masukkan jumlah file MP3: "))
            if jumlah > 0:
                break
            print("Jumlah file harus lebih dari 0.")
        except ValueError:
            print("Masukkan angka yang valid.")

    playlist = []

    for i in range(jumlah):
        while True:
            path = input(f"Masukkan path/nama file MP3 ke-{i + 1}: ").strip().strip('"')

            if os.path.isfile(path):
                playlist.append(path)
                break

            print(f"File tidak ditemukan: {path}")

    return playlist


# =======================================================
# MP3 DECODER
# =======================================================
def decode_mp3(path):
    print(f"Membaca file audio dengan miniaudio: {path}")

    decoded = miniaudio.decode_file(
        path,
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=CHANNELS,
        sample_rate=SAMPLE_RATE
    )

    pcm_bytes = bytes(decoded.samples)

    bytes_per_second = SAMPLE_RATE * CHANNELS * SAMPLE_WIDTH_BYTES
    duration_sec = len(pcm_bytes) / bytes_per_second
    duration_ms = int(duration_sec * 1000)

    print(f"Berhasil didekode!")
    print(f"Durasi     : {duration_sec:.2f} detik")
    print(f"Ukuran PCM : {len(pcm_bytes)} bytes")

    return pcm_bytes, duration_ms


# =======================================================
# METADATA TO ESP
# =======================================================
def kirim_metadata_track(ser, index, total, filename, duration_ms):
    clean_name = os.path.basename(filename)

    header = f"@@TRACK|{index}|{total}|{clean_name}|{duration_ms}@@\n"

    ser.write(header.encode("utf-8"))
    ser.flush()

    print(f"Mengirim metadata: {header.strip()}")

    time.sleep(0.2)


# =======================================================
# READ COMMAND / METRIC FROM ESP
# =======================================================
def handle_esp_lines(line_reader, ser):
    lines = line_reader.read_lines(ser)

    command = None

    for line in lines:
        # Command dari ESP untuk next track
        if line == "NEXT":
            print("NEXT diterima dari ESP.")
            command = "NEXT"

        # Output monitoring dari ESP
        elif line.startswith("@"):
            print(line)

        # Abaikan line lain supaya streaming tidak berantakan
        else:
            pass

    return command


# =======================================================
# STREAM PCM
# =======================================================
def stream_pcm(ser, line_reader, pcm_bytes):
    total_len = len(pcm_bytes)
    sent = 0
    chunk_count = 0

    bytes_per_second = SAMPLE_RATE * CHANNELS * SAMPLE_WIDTH_BYTES
    delay_per_chunk = CHUNK_SIZE / bytes_per_second

    print("Streaming audio dimulai. Tekan Ctrl+C untuk membatalkan.")

    stream_start = time.time()

    while sent < total_len:
        cmd = handle_esp_lines(line_reader, ser)

        if cmd == "NEXT":
            print("Skip track karena tombol NEXT.")
            return "NEXT"

        end = min(sent + CHUNK_SIZE, total_len)
        chunk = pcm_bytes[sent:end]

        try:
            ser.write(chunk)
            ser.flush()
        except serial.SerialTimeoutException:
            print("Serial write timeout. Streaming dihentikan.")
            return "TIMEOUT"

        sent = end
        chunk_count += 1

        if chunk_count % 10 == 0 or sent >= total_len:
            progress = (sent / total_len) * 100
            print(f"\rProgress: {progress:.1f}% ({chunk_count} chunks terkirim)", end="")

        # Baca lagi setelah kirim chunk, supaya metric dari ESP tetap muncul
        cmd = handle_esp_lines(line_reader, ser)

        if cmd == "NEXT":
            print("\nSkip track karena tombol NEXT.")
            return "NEXT"

        if REALTIME_STREAM:
            time.sleep(delay_per_chunk)

    print()

    stream_elapsed = time.time() - stream_start
    print(f"Streaming selesai. Waktu streaming: {stream_elapsed:.2f} detik")

    return "DONE"


# =======================================================
# MAIN
# =======================================================
def main():
    print("=== Python Audio Streamer untuk ESP32-S3 ===")
    print("Mode: MP3 -> PCM 16-bit mono -> Serial COM")
    print("Monitoring ESP: @METRIC / @RTOS / @MEM / @AUDIO / @BTN")
    print()

    port = pilih_port()

    if port is None:
        return

    playlist = minta_playlist()

    print()

    try:
        ser = serial.Serial(
            port=port,
            baudrate=BAUDRATE,
            timeout=0,
            write_timeout=5
        )
    except Exception as e:
        print(f"Gagal membuka port serial {port}: {e}")
        return

    line_reader = SerialLineReader()

    print(f"Koneksi serial terbuka pada {port}")
    print("Memulai streaming dalam 3 detik... Siapkan OLED.")
    print("Jangan buka PlatformIO Serial Monitor bersamaan.")
    print()

    time.sleep(3)

    try:
        ser.reset_input_buffer()
        ser.reset_output_buffer()
    except Exception:
        pass

    try:
        total_tracks = len(playlist)
        current_index = 0

        while current_index < total_tracks:
            path = playlist[current_index]

            print()
            print("===================================================")
            print(f"Track {current_index + 1}/{total_tracks}: {os.path.basename(path)}")
            print("===================================================")

            pcm_bytes, duration_ms = decode_mp3(path)

            kirim_metadata_track(
                ser,
                current_index + 1,
                total_tracks,
                path,
                duration_ms
            )

            result = stream_pcm(ser, line_reader, pcm_bytes)

            if result == "NEXT":
                current_index += 1

                if current_index >= total_tracks:
                    current_index = 0

                continue

            elif result == "DONE":
                current_index += 1
                continue

            elif result == "TIMEOUT":
                break

        print()
        print("Playlist selesai.")

        # Baca sisa metric terakhir dari ESP
        time.sleep(0.5)
        handle_esp_lines(line_reader, ser)

    except KeyboardInterrupt:
        print()
        print("Streaming dibatalkan oleh user.")

    finally:
        try:
            ser.close()
        except Exception:
            pass

        print("Port Serial ditutup.")


if __name__ == "__main__":
    main()