# ESP32 Home Controller

Firmware universal untuk ESP32 classic (DevKit, WROOM, Wemos/LOLIN32, dan clone sejenis, flash 4 MB).
Mengendalikan hingga 8 relay dan 1 fan (iTag single-button BLE), membaca 1 sensor (DHT22 atau DS18B20),
dan terhubung ke MQTT lewat WSS (Cloudflare Tunnel) atau WS/MQTT biasa.

## Build

Membutuhkan PlatformIO (Core) atau VS Code + PlatformIO extension.

```
pio run              # build
pio run -t upload    # flash via USB
pio device monitor   # serial log (115200)
```

Framework: Arduino-ESP32 3.3.x lewat pioarduino (`platformio.ini` sudah dipin ke rilis 55.03.39).
Partisi tanpa OTA: aplikasi 3 MB, LittleFS 960 KB (`partitions.csv`).
TLS: bundle root CA publik Mozilla di dalam firmware (`sdkconfig.defaults`), tanpa file CA dari user.

Upload WebUI ke LittleFS (folder `data/`) otomatis saat build filesystem:

```
pio run -t uploadfs
```

## Pertama kali

1. Flash firmware dan `uploadfs`.
2. Tanpa WiFi tersimpan, perangkat membuka AP `ESP32-Setup-XXXX`. Password AP ditampilkan di serial monitor.
3. Buka `http://192.168.4.1`, masukkan PIN `123456`, lalu isi WiFi dan MQTT di Settings.
4. Ganti PIN. WebUI menampilkan peringatan selama PIN masih default.
5. Factory reset: tahan tombol BOOT (GPIO0) selama 5 detik. PIN kembali ke 123456 dan config dihapus.

## Wiring

- Relay: output GPIO ke input relay. Default active-high (ON = pin HIGH). Bisa diubah per relay di Settings.
- Button relay: tombol momentary ke GND. Firmware memakai INPUT_PULLUP, jadi tekan = LOW.
- Setiap relay yang aktif wajib punya button GPIO.
- GPIO yang dilarang: 6–11 (flash). Input-only (34–39) tidak boleh untuk output relay atau fan.
- GPIO0 dipesan untuk tombol factory reset.
- Peringatan (tidak diblokir): GPIO 2, 5, 12, 15 (strapping pin).
- DS18B20 membutuhkan pull-up 4.7 kΩ di jalur data.

## MQTT

Broker default: `wss://mqtt.giebux.my.id:443/`. Prefix topik diatur di Settings (contoh `home1/`).

| Topik | Arah | Payload |
|---|---|---|
| `{prefix}relay1` … `relay8` | perintah | `on` / `off` (juga `1` / `0`) |
| `{prefix}relay1/state` | status (retained) | `on` / `off` |
| `{prefix}fan1` | perintah | `0`, `1`, `2`, `3` |
| `{prefix}fan1/state` | status (retained) | `0`–`3` |
| `{prefix}sensor/temp` | sensor (retained) | angka desimal, °C |
| `{prefix}sensor/humidity` | sensor (retained) | angka desimal, % (DHT22 saja) |
| `{prefix}status` | LWT (retained) | `online` / `offline` |

Di aplikasi Android (mqtt), aktifkan TLS/SSL dan WebSocket seperti sebelumnya.

## iTag (fan)

- 1 klik: fan ON/OFF.
- 2 klik saat ON: naik speed, kembali ke 1 setelah maksimum.
- 3 klik atau lebih: diabaikan.
- Scan dari Settings, pilih iTag, lalu simpan.
- Jika perilaku klik tidak sesuai, buka "Learn mode" untuk melihat byte notifikasi mentah.

## Keamanan

- PIN 6 digit. Session token 5 menit tidak aktif.
- 5 PIN salah berturut-turut = lockout 5 menit (disimpan di RAM, reset saat reboot).
- API memakai header `Authorization: Bearer`, sehingga tidak perlu token CSRF.
- Setiap perangkat sebaiknya memakai PIN unik saat dipasang di rumah lain.

## Struktur

```
src/config.*      config struct, NVS, JSON
src/gpio_map.*    validasi pin
src/relay.*       relay + debounce button
src/fan.*         fan interlock 500 ms, speed cycle, klik iTag
src/itag.*        NimBLE: scan, connect, notify, grouping klik
src/sensor.*      DHT22 / DS18B20
src/mqtt.*        esp_mqtt (WSS/WS/TCP)
src/web.*         AsyncWebServer + JSON API + auth
src/main.cpp      boot, WiFi STA/AP, NTP, factory reset, loop
data/index.html   WebUI (Inggris)
```

## Yang belum diverifikasi

Build ini ditulis tanpa kompilasi nyata di lingkungan pengembangan (registry PlatformIO dan
toolchain Espressif tidak bisa diakses). Hal berikut perlu diuji saat build pertama:

1. Nama opsi `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` di Arduino-ESP32 3.3 (pioarduino).
2. Field `esp_mqtt_client_config_t` dan `network.reconnect_timeout_ms` di IDF 5.5.
3. API NimBLE 2.5.1 (`scan->start(ms, false)`, `onScanEnd`, `subscribe`).
4. Perilaku notifikasi iTag: karakteristik FFE1, dan apakah setiap notifikasi = satu klik. Gunakan Learn mode.
5. Ukuran RAM internal saat WiFi + BLE + TLS + WebSocket aktif bersamaan.
6. Vector hasil scan BLE dan log learn mode diakses dari dua task tanpa mutex. Risiko kecil, perlu diperbaiki bila muncul crash.
