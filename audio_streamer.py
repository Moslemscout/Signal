import os
import sys
import time
import math

# Pastikan library yang diperlukan sudah terinstal
try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Error: Library 'pyserial' belum terinstal.")
    print("Silakan jalankan: pip install pyserial")
    sys.exit(1)

try:
    # Kami merekomendasikan 'pydub' karena sangat handal menangani decoding MP3.
    # Alternatif lain yang sangat ringan adalah 'miniaudio'.
    from pydub import AudioSegment
except ImportError:
    print("Error: Library 'pydub' belum terinstal.")
    print("Silakan jalankan: pip install pydub")
    print("Catatan: pydub memerlukan ffmpeg terinstal di sistem Anda untuk membaca file .mp3.")
    print("Jika Anda ingin alternatif tanpa ffmpeg, Anda juga bisa menginstal 'miniaudio': pip install miniaudio")
    # Cek apakah miniaudio tersedia sebagai alternatif
    try:
        import miniaudio
    except ImportError:
        sys.exit(1)

def get_ports():
    ports = list(serial.tools.list_ports.comports())
    return [p.device for p in ports]

def decode_audio_pydub(file_path, target_sr=22050):
    print(f"Membaca file audio dengan pydub: {file_path}")
    # Load audio file (.mp3, .wav, etc)
    audio = AudioSegment.from_file(file_path)
    
    # Resample ke Mono dan frekuensi target (22050 Hz)
    audio = audio.set_frame_rate(target_sr).set_channels(1).set_sample_width(2) # 2 bytes = 16-bit
    
    # Ambil data raw PCM bytes
    raw_data = audio.raw_data
    return raw_data, target_sr

def decode_audio_miniaudio(file_path, target_sr=22050):
    import miniaudio
    print(f"Membaca file audio dengan miniaudio: {file_path}")
    # Decode dan resample sekaligus
    decoded = miniaudio.decode_file_resampled(file_path, sample_rate=target_sr, nchannels=1, format=miniaudio.SampleFormat.SIGNED16)
    return decoded.samples.tobytes(), target_sr

def main():
    print("=== Python Audio Streamer untuk ESP32-S3 ===")
    
    # 1. Cari Port COM yang aktif
    ports = get_ports()
    if not ports:
        print("Error: Tidak ada port serial (COM) yang terdeteksi. Colokkan ESP32-S3 Anda!")
        sys.exit(1)
        
    print("Port COM tersedia:")
    for idx, port in enumerate(ports):
        print(f"[{idx}] {port}")
        
    # Pilih port otomatis jika hanya ada satu, atau minta input user
    if len(ports) == 1:
        port_choice = ports[0]
        print(f"Memilih port otomatis: {port_choice}")
    else:
        try:
            choice = int(input(f"Pilih nomor port COM (0-{len(ports)-1}): "))
            port_choice = ports[choice]
        except (ValueError, IndexError):
            print("Pilihan tidak valid.")
            sys.exit(1)

    # 2. Input file MP3
    file_path = input("Masukkan path file .mp3 Anda (misal: lagu.mp3): ").strip()
    if not os.path.exists(file_path):
        print(f"Error: File '{file_path}' tidak ditemukan.")
        sys.exit(1)

    # 3. Dekode MP3 ke PCM Mono 16-bit 22050Hz
    target_sr = 22050
    try:
        if 'pydub' in sys.modules:
            pcm_bytes, sr = decode_audio_pydub(file_path, target_sr)
        else:
            pcm_bytes, sr = decode_audio_miniaudio(file_path, target_sr)
    except Exception as e:
        print(f"Gagal mendekode audio: {e}")
        print("Cobalah gunakan file berformat .wav jika dekoder .mp3 Anda bermasalah.")
        sys.exit(1)

    total_bytes = len(pcm_bytes)
    duration = total_bytes / (target_sr * 2) # 2 bytes per sample
    print(f"Berhasil didekode! Durasi: {duration:.2f} detik, Ukuran PCM: {total_bytes} bytes")

    # 4. Buka koneksi serial ke ESP32-S3
    # Catatan: ESP32-S3 USB CDC mengabaikan baud rate, tapi kita set 115200 sebagai default
    try:
        ser = serial.Serial(port_choice, 115200, timeout=1)
        print(f"Koneksi serial terbuka pada {port_choice}")
    except Exception as e:
        print(f"Gagal membuka port serial: {e}")
        sys.exit(1)

    print("Memulai streaming dalam 3 detik... (Siapkan layar OLED)")
    time.sleep(3)

    # Kirim data dalam ukuran blok (chunk) 512 sampel = 1024 bytes
    chunk_samples = 512
    chunk_bytes = chunk_samples * 2 # 1024 bytes
    
    # Waktu durasi per chunk dalam detik (512 / 22050 = ~0.02322 detik / 23.22 ms)
    chunk_duration = chunk_samples / target_sr
    
    start_time = time.perf_counter()
    chunks_sent = 0
    total_chunks = math.ceil(total_bytes / chunk_bytes)

    print("Streaming audio dimulai. Tekan Ctrl+C untuk membatalkan.")
    
    try:
        while chunks_sent < total_chunks:
            # Potong data pcm
            start_idx = chunks_sent * chunk_bytes
            end_idx = min(start_idx + chunk_bytes, total_bytes)
            chunk_data = pcm_bytes[start_idx:end_idx]
            
            # Jika chunk terakhir kurang dari 1024 bytes, lakukan padding dengan nol
            if len(chunk_data) < chunk_bytes:
                chunk_data += b'\x00' * (chunk_bytes - len(chunk_data))

            # Kirim data ke ESP32
            ser.write(chunk_data)
            chunks_sent += 1

            # Sinkronisasi Waktu Real-Time (Feedback Loop)
            # Menghitung kapan chunk ini harusnya dikirim berdasarkan jumlah data yang sudah terkirim
            expected_elapsed = chunks_sent * chunk_duration
            actual_elapsed = time.perf_counter() - start_time
            delay_needed = expected_elapsed - actual_elapsed
            
            if delay_needed > 0:
                time.sleep(delay_needed)
                
            if chunks_sent % 100 == 0 or chunks_sent == total_chunks:
                progress = (chunks_sent / total_chunks) * 100
                print(f"Progress: {progress:.1f}% ({chunks_sent}/{total_chunks} chunks terkirim)", end='\r')

        print("\nStreaming selesai!")
        
    except KeyboardInterrupt:
        print("\nStreaming dibatalkan oleh pengguna.")
    finally:
        ser.close()
        print("Port Serial ditutup.")

if __name__ == "__main__":
    main()
