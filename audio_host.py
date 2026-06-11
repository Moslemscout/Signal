import os
import sys
import time
import math
import glob
import queue
import threading

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial belum terinstall.")
    print("Jalankan: pip install pyserial")
    sys.exit(1)

try:
    import miniaudio
except ImportError:
    miniaudio = None

try:
    import numpy as np
    import sounddevice as sd
    HAS_SOUNDDEVICE = True
except ImportError:
    np = None
    sd = None
    HAS_SOUNDDEVICE = False


# ==========================================================
# CONFIG
# ==========================================================
SAMPLE_RATE = 22050
CHANNELS = 1
BYTES_PER_SAMPLE = 2

CHUNK_SAMPLES = 512
CHUNK_BYTES = CHUNK_SAMPLES * BYTES_PER_SAMPLE

# HARUS SAMA DENGAN main.cpp:
# Serial.begin(921600);
SERIAL_BAUD = 921600

PKT_MAGIC_1 = 0xAA
PKT_MAGIC_2 = 0x55

PKT_AUDIO = 0x01
PKT_META = 0x02
PKT_STATUS = 0x03

SERIAL_WRITE_CHUNK = 128


# ==========================================================
# PORT / FILE UTILS
# ==========================================================
def choose_port():
    ports = list(serial.tools.list_ports.comports())

    if not ports:
        print("Tidak ada COM port terdeteksi.")
        print("Pastikan ESP32-S3 sudah dicolok.")
        sys.exit(1)

    print("Port COM tersedia:")
    for i, p in enumerate(ports):
        print(f"[{i}] {p.device} - {p.description}")

    if len(ports) == 1:
        print(f"Memilih otomatis: {ports[0].device}")
        return ports[0].device

    while True:
        choice = input(f"Pilih nomor COM port (0-{len(ports)-1}): ").strip()

        try:
            idx = int(choice)
            if 0 <= idx < len(ports):
                return ports[idx].device
        except ValueError:
            pass

        print("Pilihan tidak valid.")


def build_playlist():
    args = sys.argv[1:]
    files = []

    if args:
        for arg in args:
            arg = arg.strip('"')

            if os.path.isdir(arg):
                files.extend(sorted(glob.glob(os.path.join(arg, "*.mp3"))))
                files.extend(sorted(glob.glob(os.path.join(arg, "*.wav"))))
            else:
                files.append(arg)
    else:
        files.extend(sorted(glob.glob("*.mp3")))
        files.extend(sorted(glob.glob("*.wav")))

    files = [f for f in files if os.path.exists(f)]

    if not files:
        print("Tidak ada file .mp3 / .wav ditemukan.")
        print("Taruh lagu di folder project atau jalankan:")
        print("python audio_host.py test.mp3 test2.mp3")
        sys.exit(1)

    return files


def samples_to_bytes(samples):
    if isinstance(samples, bytes):
        return samples

    if isinstance(samples, bytearray):
        return bytes(samples)

    if hasattr(samples, "tobytes"):
        return samples.tobytes()

    return bytes(samples)


def decode_audio(file_path):
    if miniaudio is None:
        print("ERROR: miniaudio belum terinstall.")
        print("Jalankan: pip install miniaudio")
        sys.exit(1)

    print(f"Decode audio: {file_path}")

    decoded = miniaudio.decode_file(
        file_path,
        sample_rate=SAMPLE_RATE,
        nchannels=CHANNELS,
        output_format=miniaudio.SampleFormat.SIGNED16
    )

    pcm_bytes = samples_to_bytes(decoded.samples)

    return pcm_bytes, SAMPLE_RATE


def format_time(ms):
    total_sec = ms // 1000
    minute = total_sec // 60
    second = total_sec % 60
    return f"{minute:02d}:{second:02d}"


def open_serial_port(port):
    print(f"Membuka serial {port} @ {SERIAL_BAUD} baud...")

    ser = serial.Serial(
        port=port,
        baudrate=SERIAL_BAUD,
        timeout=0,
        write_timeout=2,
        rtscts=False,
        dsrdtr=False,
        xonxoff=False
    )

    # Penting untuk ESP32-S3 USB CDC:
    # DTR=True supaya output Serial.print dari ESP kebaca Python.
    # RTS=False supaya tidak masuk reset/boot mode aneh.
    try:
        ser.setDTR(True)
        ser.setRTS(False)
    except Exception:
        pass

    return ser


# ==========================================================
# AUDIO HOST
# ==========================================================
class AudioHost:
    def __init__(self, ser, playlist, keyboard_queue):
        self.ser = ser
        self.playlist = playlist
        self.keyboard_queue = keyboard_queue

        self.index = 0
        self.rx_text = ""
        self.quit_requested = False
        self.is_playing = False
        self.serial_lock = threading.Lock()
        self.serial_error_count = 0

    # ------------------------------------------------------
    # Packet Host -> ESP
    # ------------------------------------------------------
    def safe_write(self, data, label="data"):
        try:
            with self.serial_lock:
                offset = 0
                total = len(data)

                while offset < total:
                    chunk = data[offset:offset + SERIAL_WRITE_CHUNK]
                    self.ser.write(chunk)
                    offset += len(chunk)

                    time.sleep(0.0005)

            return True

        except serial.SerialTimeoutException:
            self.serial_error_count += 1
            print(f"[WARNING] Serial write timeout saat kirim {label}.")
            print("ESP tidak menerima data dengan normal. Jangan tekan RST saat Python jalan.")
            return False

        except Exception as e:
            self.serial_error_count += 1
            print(f"[WARNING] Serial write gagal saat kirim {label}: {e}")
            return False

    def send_packet(self, pkt_type, payload):
        if isinstance(payload, str):
            payload = payload.encode("utf-8")

        length = len(payload)

        if length > 65535:
            print("Payload terlalu besar, packet dibatalkan.")
            return False

        header = bytes([
            PKT_MAGIC_1,
            PKT_MAGIC_2,
            pkt_type,
            length & 0xFF,
            (length >> 8) & 0xFF
        ])

        return self.safe_write(header + payload, label=f"packet type {pkt_type}")

    def send_meta(self, file_path, duration_ms, sample_rate):
        filename = os.path.basename(file_path)
        meta = f"{filename}|{duration_ms}|{sample_rate}"

        print(f"[HOST -> ESP] META: {meta}")
        return self.send_packet(PKT_META, meta)

    def send_status(self, status):
        print(f"[HOST -> ESP] STATUS: {status}")
        return self.send_packet(PKT_STATUS, status)

    # ------------------------------------------------------
    # Command ESP -> Host
    # ------------------------------------------------------
    def poll_serial_command(self):
        try:
            n = self.ser.in_waiting
        except Exception:
            return None

        if n <= 0:
            return None

        try:
            data = self.ser.read(n)
        except Exception:
            return None

        if not data:
            return None

        text = data.decode("utf-8", errors="ignore")
        self.rx_text += text

        command = None

        while "\n" in self.rx_text:
            line, self.rx_text = self.rx_text.split("\n", 1)
            line = line.strip()

            if line.startswith("@ESP:"):
                cmd = line.replace("@ESP:", "", 1).strip().upper()
                print(f"[ESP -> HOST] {cmd}")

                if cmd in ("READY", "PLAY", "STOP", "NEXT"):
                    command = cmd

        return command

    def poll_keyboard_command(self):
        try:
            return self.keyboard_queue.get_nowait()
        except queue.Empty:
            return None

    def poll_command(self):
        cmd = self.poll_serial_command()
        if cmd is not None:
            return cmd

        cmd = self.poll_keyboard_command()
        if cmd is not None:
            return cmd

        return None

    # ------------------------------------------------------
    # Track Control
    # ------------------------------------------------------
    def select_next_track(self):
        self.index = (self.index + 1) % len(self.playlist)

        file_path = self.playlist[self.index]
        print(f"Selected next track: [{self.index + 1}/{len(self.playlist)}] {file_path}")

        try:
            pcm_bytes, sr = decode_audio(file_path)
            duration_ms = int((len(pcm_bytes) * 1000) / (sr * BYTES_PER_SAMPLE))
            self.send_meta(file_path, duration_ms, sr)
            self.send_status("IDLE")
        except Exception as e:
            print(f"Gagal membaca metadata lagu: {e}")

    def play_current_track(self):
        file_path = self.playlist[self.index]

        print()
        print("==================================================")
        print(f"PLAY [{self.index + 1}/{len(self.playlist)}]: {file_path}")
        print("==================================================")

        pcm_bytes, sr = decode_audio(file_path)

        total_bytes = len(pcm_bytes)
        duration_ms = int((total_bytes * 1000) / (sr * BYTES_PER_SAMPLE))
        total_chunks = math.ceil(total_bytes / CHUNK_BYTES)

        print(f"Durasi   : {duration_ms / 1000:.2f} detik")
        print(f"PCM size : {total_bytes} bytes")
        print(f"Format   : {sr} Hz, mono, 16-bit")
        print(f"Chunks   : {total_chunks}")

        self.send_meta(file_path, duration_ms, sr)
        time.sleep(0.05)
        self.send_status("PLAY")

        self.is_playing = True

        sound_started = False

        if HAS_SOUNDDEVICE:
            try:
                samples = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32) / 32768.0
                sd.stop()
                sd.play(samples, sr, blocking=False)
                sound_started = True
                print("Speaker laptop: PLAY")
            except Exception as e:
                print(f"Speaker laptop gagal play: {e}")
                print("Stream ke ESP tetap dicoba.")
        else:
            print("sounddevice/numpy tidak tersedia. Speaker laptop tidak bunyi.")
            print("Install: pip install sounddevice numpy")

        result = "END"
        start_time = time.perf_counter()

        try:
            for chunk_index in range(total_chunks):
                cmd = self.poll_command()

                if cmd == "STOP":
                    result = "STOP"
                    break

                if cmd == "NEXT":
                    result = "NEXT"
                    break

                if cmd == "QUIT":
                    result = "QUIT"
                    break

                start_idx = chunk_index * CHUNK_BYTES
                end_idx = min(start_idx + CHUNK_BYTES, total_bytes)

                chunk = pcm_bytes[start_idx:end_idx]

                if len(chunk) < CHUNK_BYTES:
                    chunk += b"\x00" * (CHUNK_BYTES - len(chunk))

                ok = self.send_packet(PKT_AUDIO, chunk)

                if not ok and self.serial_error_count >= 5:
                    print()
                    print("[ERROR] Serial gagal berkali-kali.")
                    print("Streaming ke ESP dihentikan.")
                    print("Cek: baudrate main.cpp harus 921600, jangan buka Serial Monitor, jangan tekan RST.")
                    result = "SERIAL_ERROR"
                    break

                progress_bytes = min(end_idx, total_bytes)
                elapsed_ms = int((progress_bytes * 1000) / (sr * BYTES_PER_SAMPLE))

                if chunk_index % 20 == 0:
                    percent = 100.0 * (chunk_index + 1) / total_chunks
                    print(
                        f"Streaming {chunk_index + 1}/{total_chunks} "
                        f"({percent:.1f}%) "
                        f"{format_time(elapsed_ms)} / {format_time(duration_ms)}"
                    )

                next_time = start_time + ((chunk_index + 1) * CHUNK_SAMPLES / sr)

                while time.perf_counter() < next_time:
                    cmd = self.poll_command()

                    if cmd == "STOP":
                        result = "STOP"
                        break

                    if cmd == "NEXT":
                        result = "NEXT"
                        break

                    if cmd == "QUIT":
                        result = "QUIT"
                        break

                    time.sleep(0.002)

                if result in ("STOP", "NEXT", "QUIT", "SERIAL_ERROR"):
                    break

        except KeyboardInterrupt:
            result = "QUIT"

        self.is_playing = False

        if sound_started:
            try:
                sd.stop()
            except Exception:
                pass

        if result == "STOP":
            print("STOP diterima.")
            self.send_status("STOP")
            return "STOP"

        if result == "NEXT":
            print("NEXT diterima.")
            self.send_status("NEXT")
            self.index = (self.index + 1) % len(self.playlist)
            return "NEXT"

        if result == "QUIT":
            print("QUIT.")
            self.send_status("STOP")
            self.quit_requested = True
            return "QUIT"

        if result == "SERIAL_ERROR":
            self.send_status("STOP")
            return "SERIAL_ERROR"

        print("Lagu selesai.")
        self.send_status("END")

        self.index = (self.index + 1) % len(self.playlist)
        return "END"

    # ------------------------------------------------------
    # Main Loop
    # ------------------------------------------------------
    def run(self):
        print()
        print("Host siap.")
        print("Kontrol ESP:")
        print("  Tombol HIJAU  = PLAY kalau idle, STOP kalau play")
        print("  Tombol KUNING = NEXT lagu")
        print()
        print("Kontrol keyboard terminal fallback:")
        print("  p = play")
        print("  s = stop")
        print("  n = next")
        print("  q = quit")
        print()
        print("Untuk test button asli, jangan ketik p. Langsung pencet tombol hijau di ESP.")
        print()

        self.send_status("IDLE")

        while not self.quit_requested:
            cmd = self.poll_command()

            if cmd == "READY":
                print("ESP ready.")

            elif cmd == "PLAY":
                if not self.is_playing:
                    self.serial_error_count = 0
                    self.play_current_track()

            elif cmd == "STOP":
                self.send_status("STOP")

            elif cmd == "NEXT":
                if self.is_playing:
                    pass
                else:
                    self.select_next_track()

            elif cmd == "QUIT":
                self.send_status("STOP")
                self.quit_requested = True
                break

            time.sleep(0.02)


# ==========================================================
# KEYBOARD FALLBACK
# ==========================================================
def keyboard_worker(cmd_queue):
    while True:
        try:
            text = input().strip().lower()
        except EOFError:
            break

        if text in ("p", "play"):
            cmd_queue.put("PLAY")

        elif text in ("s", "stop"):
            cmd_queue.put("STOP")

        elif text in ("n", "next"):
            cmd_queue.put("NEXT")

        elif text in ("q", "quit", "exit"):
            cmd_queue.put("QUIT")
            break

        elif text:
            print("Command tidak dikenal. Pakai: p / s / n / q")


# ==========================================================
# MAIN
# ==========================================================
def main():
    print("=== ESP32-S3 RTOS MP3 Host Player ===")

    playlist = build_playlist()

    print()
    print("Playlist:")
    for i, f in enumerate(playlist):
        print(f"[{i + 1}] {f}")

    print()

    port = choose_port()

    try:
        ser = open_serial_port(port)
    except Exception as e:
        print(f"Gagal membuka serial port: {e}")
        print("Pastikan PlatformIO Serial Monitor sudah ditutup.")
        sys.exit(1)

    print("Serial terbuka.")
    print("Menunggu ESP stabil 7 detik...")
    time.sleep(7.0)

    try:
        ser.reset_input_buffer()
    except Exception:
        pass

    cmd_queue = queue.Queue()
    t = threading.Thread(target=keyboard_worker, args=(cmd_queue,), daemon=True)
    t.start()

    host = AudioHost(ser, playlist, cmd_queue)

    try:
        host.run()
    except KeyboardInterrupt:
        print("Dihentikan user.")
        try:
            host.send_status("STOP")
        except Exception:
            pass

    finally:
        try:
            if HAS_SOUNDDEVICE:
                sd.stop()
        except Exception:
            pass

        try:
            ser.close()
        except Exception:
            pass

        print("Serial ditutup.")


if __name__ == "__main__":
    main()