# ESP32 Home Controller

Firmware untuk **Wemos D1 mini ESP32** (ESP-WROOM-32, USB micro, flash 4 MB, tanpa PSRAM).
Mengendalikan hingga 8 relay dan 1 fan (iTag single-button BLE), membaca 1 sensor (DHT22 atau DS18B20),
dan terhubung ke MQTT lewat WSS (Cloudflare Tunnel).

Firmware ini dibuat khusus untuk board tersebut. Jika board diganti, daftar pin di `src/gpio_map.cpp` harus diubah lalu dibuild ulang.

## Build (GitHub Actions)

Build dijalankan di GitHub Actions (`.github/workflows/build.yml`). Hasilnya artifact `esp32-home-controller`:

- `full-flash-4MB.bin`: firmware + bootloader + partisi + LittleFS (WebUI). Flash file ini ke alamat `0x0`.
- `firmware-app-only.bin`: hanya aplikasi, untuk referensi.

Build lokal (opsional):

```
pio run                 # firmware
pio run -t buildfs      # LittleFS berisi data/
```

Framework: Arduino-ESP32 3.3.x lewat pioarduino (`platformio.ini` dipin ke rilis 55.03.312).
Partisi tanpa OTA: aplikasi 3 MB, LittleFS 832 KiB (`partitions.csv`, label `spiffs`), dan core dump 128 KiB. Pengaturan core dump, full default root CA bundle, serta MQTT WebSocket/WSS diterapkan melalui `custom_sdkconfig` khusus platform pioarduino (`sdkconfig.custom`). Build CI harus membuktikan konfigurasi tersebut berhasil diterapkan.
Tidak ada OTA. Update firmware dilakukan dengan flash ulang dari PC.

## Flash dari PC

1. Hubungkan board via USB. Board ini tidak punya tombol BOOT. Jika flash gagal ("failed to start stub flasher"), hubungkan **GPIO0 ke GND** saat flash, lalu lepas setelah selesai.
2. Flash `full-flash-4MB.bin` ke alamat `0x0` (esptool `write_flash 0x0 full-flash-4MB.bin`, atau tool flash lain).
3. Buka serial monitor 115200 untuk melihat log boot.

## Pertama kali

1. Tanpa WiFi tersimpan, perangkat membuka AP `ESP32-Setup-XXXX`. Password AP default **`11111111`**.
2. Sambungkan ke AP, buka `http://192.168.4.1`.
3. Tab **Config**, masukkan PIN default **`123456`**.
4. Isi WiFi, MQTT, relay, fan, sensor, iTag. Ganti PIN di bagian **Security**. Password AP bisa diganti di bagian WiFi.
5. Simpan. Perangkat restart dan masuk ke WiFi rumah.

## Pin board (Wemos D1 mini ESP32)

Dropdown GPIO di WebUI hanya berisi pin berikut:

**4, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33** (13 pin)

Pin yang tidak tersedia dan tidak muncul di dropdown:

| Pin | Alasan |
|---|---|
| 0 | Factory reset (tahan ke GND) |
| 1, 3 | TX/RX serial |
| 2, 5, 12, 13, 14, 15 | Strapping / JTAG, bisa mengganggu boot |
| 6–11 | Flash internal |
| 34–39 | Input-only, tanpa pull-up internal (tidak bisa jadi tombol) |

Kapasitas: satu relay memakai 2 pin (output + tombol), fan memakai 1 pin per kecepatan (2 atau 3), sensor 1 pin.
Contoh: 4 relay (8 pin) + fan 3 kecepatan (3 pin) + sensor (1 pin) = 12 dari 13 pin.

Aturan validasi (dicek di firmware saat simpan dan saat boot):

- Setiap pin hanya boleh dipakai satu fungsi. Dropdown yang sudah dipakai akan dinonaktifkan di tempat lain.
- Relay aktif wajib punya output dan tombol.
- Fan aktif wajib punya pin untuk setiap kecepatan sampai kecepatan maksimum (2 atau 3).
- Jika konfigurasi tidak valid, firmware masuk safe mode: relay tidak diaktifkan dan pesan error tampil di serial.

## Wiring

- Relay: output GPIO ke input relay. Default active-high (ON = pin HIGH). Bisa diubah per relay.
- Tombol relay: momentary ke GND. Firmware memakai INPUT_PULLUP, jadi tekan = LOW.
- DS18B20 membutuhkan pull-up 4.7 kΩ di jalur data.

## Factory reset

Di board ini tidak ada tombol reset khusus. Caranya:

1. Tunggu sampai board selesai boot (serial: `WebUI ready.`).
2. Hubungkan **GPIO0 ke GND** dan tahan **5 detik**.
3. Config dihapus, PIN kembali ke 123456, AP password kembali ke 11111111, perangkat restart.

Catatan: jika GPIO0 ke GND saat board dinyalakan atau di-reset, ESP32 masuk mode flash (download) dan firmware tidak berjalan. Hubungkan setelah boot.

Alternatif: tombol **Factory reset** di tab Config (perlu PIN).

## Dashboard dan Config

- **Dashboard** (tanpa PIN): status MQTT, iTag, suhu dan kelembapan, tombol relay besar, tombol kecepatan fan. Bisa dipakai siapa saja di rumah, misalnya saat tombol fisik rusak atau MQTT mati.
- **Config** (PIN 6 digit): semua pengaturan, termasuk mapping GPIO, WiFi, password AP, MQTT, iTag, sensor, PIN, dan factory reset.

## MQTT

Broker default: `wss://mqtt.giebux.my.id:443/`. Prefix topik diatur di Config. Isi `set/set` akan menghasilkan topik `set/set/r1`; slash pemisah ditambahkan otomatis sehingga prefix tidak perlu diakhiri slash.

| Topik | Arah | Payload |
|---|---|---|
| `{prefix}/r1` … `{prefix}/r8` | perintah relay | `on` / `off` (juga `1` / `0`) |
| `{prefix}/f1` … `{prefix}/f3` | perintah fan | `on` memilih speed tersebut; `off` menghentikan fan hanya jika speed itu sedang aktif |
| `{prefix}/r1/state` … `{prefix}/r8/state` | status relay (retained) | `on` / `off` |
| `{prefix}/f1/state` … `{prefix}/f3/state` | status speed fan (retained) | `on` / `off` |
| `{prefix}/sensor/temp` | sensor (retained) | angka desimal, °C |
| `{prefix}/sensor/humidity` | sensor (retained) | angka desimal, % (DHT22 saja) |
| `{prefix}/status` | LWT (retained) | `online` / `offline` |

Di aplikasi Android (mqtt), aktifkan TLS/SSL dan WebSocket.

## iTag (fan)

- 1 klik: fan ON/OFF.
- 2 klik saat ON: naik kecepatan, kembali ke 1 setelah maksimum.
- 3 klik atau lebih: diabaikan.
- Scan di tab Config, pilih iTag, simpan.
- Jika perilaku klik tidak sesuai, buka "Learn mode: show raw bytes" untuk melihat byte notifikasi mentah.

## Keamanan

- Endpoint dashboard (`/api/status`, `/api/relay`, `/api/fan`) tanpa PIN, sesuai kebutuhan rumah.
- Endpoint config (`/api/config`, `/api/factory-reset`, `/api/ble/*`) memakai PIN dan session token.
- PIN 6 digit. Session token berakhir setelah 5 menit tidak aktif. Token server disimpan di RAM ESP32; salinannya hanya berada di variabel JavaScript selama halaman terbuka, tidak disimpan ke `localStorage` atau `sessionStorage`.
- 5 PIN salah berturut-turut = lockout 5 menit (di RAM, reset saat reboot; reboot dapat menghapus lockout).
- Semua POST berbody JSON wajib `Content-Type: application/json`. Browser lintas situs dibatasi oleh preflight CORS yang tidak diizinkan; ini bukan autentikasi dan tidak melindungi perangkat dari klien lain pada jaringan lokal.
- Password AP default 11111111. Ganti jika perangkat dipasang di rumah yang ramai.
- Setiap perangkat sebaiknya memakai PIN unik saat dipasang di rumah lain.

## Struktur

```
src/config.*      config struct, NVS (read back after save), JSON
src/gpio_map.*    profil board (13 pin), validasi pin
src/relay.*       relay + debounce tombol
src/fan.*         fan interlock 500 ms, cycle speed, klik iTag
src/itag.*        NimBLE: scan, connect, notify, grouping klik
src/sensor.*      DHT22 / DS18B20
src/mqtt.*        esp_mqtt (WSS/WS/TCP)
src/web.*         AsyncWebServer + JSON API + auth PIN
src/main.cpp      boot, WiFi STA/AP, NTP, factory reset, loop
data/index.html   WebUI (Inggris): Dashboard + Config
```

## Yang belum diverifikasi

Build ini belum diuji di hardware. Hal berikut perlu diuji saat pertama dipakai:

1. WSS ke Cloudflare dengan full default root CA bundle Mozilla yang diaktifkan melalui `sdkconfig.custom`.
2. Field `esp_mqtt_client_config_t` dan `network.reconnect_timeout_ms` di IDF 5.5.
3. API NimBLE 2.5.1 (`scan->start(ms, false)`, `onScanEnd`, `subscribe`).
4. Perilaku notifikasi iTag: karakteristik FFE1, dan apakah setiap notifikasi = satu klik. Gunakan Learn mode.
5. Ukuran RAM internal saat WiFi + BLE + TLS + WebSocket aktif bersamaan.
6. Sinkronisasi vector hasil scan BLE dan log learn mode menggunakan FreeRTOS mutex; perilaku notifikasi iTag tetap perlu divalidasi dengan raw bytes karena belum terbukti setiap notifikasi mewakili satu klik.
