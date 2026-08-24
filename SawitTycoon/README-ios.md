# Setup Xcode — Sawit Tycoon (iOS)

Saya tidak membuat file `.xcodeproj` di sini karena itu bundle biner/plist
kompleks yang gampang korup kalau ditulis manual tanpa Xcode asli. Sebagai
gantinya, semua **kode sumbernya sudah lengkap** — kamu tinggal membuat
proyek Xcode kosong lalu menambahkan file-file ini. ~10 menit.

## 1. Buat proyek baru

Xcode → File → New → Project → **iOS → App**
- Product Name: `SawitTycoon`
- Interface: **Storyboard** boleh dipilih default (tidak dipakai, akan kita lepas)
- Language: **Swift**
- Uncheck "Use Core Data", "Include Tests" (opsional)

## 2. Hapus file bawaan yang tidak dipakai

Xcode akan membuat `ContentView.swift`/`Main.storyboard`/`SceneDelegate.swift`/
`AppDelegate.swift` bawaan. **Hapus semuanya** (Move to Trash) — kita pakai
punya kami yang sudah menghubungkan ke C++ engine.

Di **Info** tab target-mu, hapus baris **"Main storyboard file base name"**
kalau ada (kita tidak pakai storyboard sama sekali).

## 3. Tambahkan semua file dari folder `SawitTycoon/` di paket ini

Drag folder `platform/ios/SawitTycoon/` (termasuk subfolder `Bridge/`) ke
Project Navigator Xcode. Saat dialog muncul:
- ✅ "Copy items if needed"
- ✅ "Create groups"
- ✅ Add to target: SawitTycoon

Ini akan menambahkan:
- `AppDelegate.swift`, `SceneDelegate.swift`, `GameViewController.swift`
- `Info.plist` (replace punya bawaan Xcode — lihat langkah 5)
- `Bridge/EngineBridge.h`, `Bridge/EngineBridge.mm`
- `SawitTycoon-Bridging-Header.h`

## 4. Tambahkan engine C++ & renderer

Drag juga dua folder ini dari root paket (**di luar** folder `SawitTycoon/`):
- `engine/include/`, `engine/src/` (file `.hpp`/`.cpp`)
- `render/renderer_gl.hpp`, `render/renderer_gl.cpp`

Set "Add to target: SawitTycoon" untuk semuanya.

## 5. Build Settings yang WAJIB diatur

Pilih target **SawitTycoon** → tab **Build Settings** (mode "All"):

| Setting | Nilai |
|---|---|
| Objective-C Bridging Header | `SawitTycoon/SawitTycoon-Bridging-Header.h` |
| Header Search Paths | tambahkan path ke `engine/include` dan `render/` (relatif ke lokasi proyek) |
| C++ Language Dialect | GNU++17 / C++17 |
| C++ Standard Library | libc++ |

Tab **General** → **Info**:
- Custom iOS Target Properties: pastikan `Info.plist` yang dipakai adalah
  punya kami (cek path di "Info.plist File" pada Build Settings kalau Xcode
  masih menunjuk ke yang lama).

Tab **General** → hapus referensi "Main Interface" / storyboard kalau masih
ada (kosongkan field-nya) — kita pakai `SceneDelegate` programatik.

## 6. Framework yang perlu ditautkan

Tab **General** → **Frameworks, Libraries, and Embedded Content** → tambahkan:
- `OpenGLES.framework`
- `GLKit.framework`

(Keduanya deprecated sejak iOS 12 tapi masih berfungsi penuh — lihat catatan
"Kenapa OpenGL ES, bukan Metal?" di README utama untuk rencana migrasi.)

## 7. Build & Run

Pilih simulator/device lalu ⌘R. Kalau ada error compile, kirim pesan error
lengkapnya (dari Issue Navigator Xcode) — saya bantu telusuri.

## Ekspektasi realistis

File `.mm`/`.swift` di paket ini **belum pernah dikompilasi dengan Xcode
asli** (sandbox saya tidak punya Xcode). Saya sudah tinjau manual baris per
baris dan cocokkan dengan API resmi Apple, tapi wajar kalau ada 1-2 typo
kecil yang baru ketahuan saat compile sungguhan — itu bagian normal dari
proses, bukan tanda kodenya asal-asalan.
