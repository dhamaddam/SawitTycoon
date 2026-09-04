# 🌴 Sawit Tycoon — Alur Permainan (Storyline)

> Dokumen ini menjelaskan bagaimana permainan berjalan dari awal sampai
> berkembang jadi kebun besar — untuk pemain baru, dan untuk siapa pun yang
> lanjut mengembangkan kode ini di kemudian hari.

---

## 1. Premis

Kamu baru saja ditugaskan mengelola sepetak kebun kelapa sawit kecil —
143 pokok, seluas 1 hektar, ditanam pola segitiga sama sisi sesuai standar
industri (9m x 9m x 9m). Modal awal Rp 750.000, satu orang buruh lapangan,
dan sebidang tanah yang harus kamu ubah jadi usaha yang menguntungkan.

Tidak ada musuh, tidak ada "game over" — ini simulasi manajemen yang
tenang. Tantangannya bukan bertahan hidup, tapi **membangun sistem**: kapan
harus menambah pekerja, kapan naik jenjang jadi Mandor lalu Asisten Afdeling,
kapan cukup modal untuk PKS sendiri.

---

## 2. Hari Pertama — Belajar Ritme Kebun

Begitu masuk, kamu akan melihat kebun dari sudut isometrik, 143 pokok
berjejer rapi dengan pola berselang-seling (bukan grid kotak — itu memang
sesuai cara sawit ditanam sungguhan). Satu pekerja berdiri menunggu perintah.

**Yang bisa langsung kamu lakukan:**
- Ketuk sebuah pokok → lihat statusnya (umur, kesehatan, kematangan TBS)
- Kalau ada tandan matang (oranye/merah), tekan **Panen** — pekerja akan
  berjalan ke sana, memakai **dodos** (pokok muda, <6 tahun) atau **egrek**
  (pokok tua) sesuai tinggi pokoknya, lalu TBS-nya tergeletak di dasar pokok
- Tekan **Angkut** — pekerja memikul TBS itu ke TPH (Tempat Pengumpulan
  Hasil), TBS-nya ikut tampak menumpuk di sana
- Setiap ~26 detik (atau kapan saja lewat tombol Kirim TPH), truk datang
  mengangkut semua TBS di TPH sekaligus — uangmu bertambah instan, dan kamu
  akan melihat traktor+trailer benar-benar berangkat dari TPH

Ini siklus inti permainan: **pohon matang → panen → angkut → jual → uang
masuk**. Semua elemen lain di game ini dibangun di atas siklus ini.

---

## 3. Minggu Pertama — Mengelola Banyak Pokok Sekaligus

Begitu kamu terbiasa dengan siklus satu-per-satu, kamu akan sadar: 143 pokok
terlalu banyak untuk diketuk satu-satu setiap kali ada yang matang. Di sinilah
**tombol aksi massal** masuk:

- **Panen Semua** — instan memanen SEMUA pokok yang matang, tanpa pekerja
  perlu mendatangi satu-satu (gratis, tidak makan waktu tunggu)
- **Angkut Semua** — instan mengangkut semua TBS yang tergeletak ke TPH
  (dibatasi kapasitas TPH — kalau penuh, sisanya menunggu truk jalan dulu)
- **Pupuk / Semprot / Obati Semua** — sistem menghitung total biaya untuk
  semua pokok yang butuh dulu, baru menerapkan sekaligus kalau uangmu cukup
  (kalau tidak cukup, tidak ada yang diproses — supaya jelas, bukan
  setengah-setengah yang membingungkan)

Setiap pokok yang baru saja kamu proses akan menampilkan **ikon kecil**
di atasnya (warna beda per jenis aksi) sepanjang hari itu — jadi kamu bisa
sekilas melihat mana yang sudah dikerjakan tanpa perlu mengetuk satu-satu.

---

## 4. Bulan Pertama — Ancaman Mulai Muncul

Seiring waktu berjalan, dua hal mulai terjadi secara alami di kebunmu:

- **Hama** — sesekali (rata-rata 1 kasus baru per ~2-3 hari di seluruh
  kebun) sebuah pokok terserang hama. Tandanya jelas terlihat. Semprot
  dengan Pestisida untuk menyembuhkannya.
- **Ganoderma (busuk pangkal batang)** — jauh lebih jarang (rata-rata 1
  kasus baru per ~12 hari), tapi lebih serius. Begitu muncul gejalanya
  (bracket fungus di pangkal batang), kamu punya waktu berhari-hari (bukan
  hitungan detik) untuk mengobatinya dengan agens hayati sebelum pokok itu
  benar-benar mati. Kalau terlambat, pokok itu harus ditebang & ditanam
  ulang — mulai dari nol lagi untuk pokok itu.

Ini bukan angka sembarangan — disesuaikan dari literatur ilmiah tentang
seberapa sering & seberapa cepat penyakit ini sungguhan menyerang kebun
sawit (lihat `README.md` untuk daftar sumbernya). Tujuannya: ancaman yang
terasa nyata dan perlu direspons, tapi tidak membuatmu panik tiap beberapa
detik.

---

## 5. Naik Kelas — Jenjang SDM

Satu pekerja saja tidak akan cukup lama-lama. Di layar SDM, kamu bisa
merekrut jenjang jabatan sungguhan yang ada di perkebunan sawit nyata:

```
Buruh Lapangan → Mandor / Krani → Mandor Besar / Krani Kepala
              → Asisten Afdeling (Sinder) → Asisten Kepala (Askep)
              → Manager Kebun / Administratur (ADM)
```

Setiap jenjang butuh prasyarat jumlah bawahan (persis struktur organisasi
kebun sungguhan), dan masing-masing punya gaji harian yang harus kamu bayar
tiap hari — jadi merekrut bukan keputusan sepihak, ada trade-off nyata
antara kapasitas kerja dan biaya operasional.

---

## 6. Ekspansi Lahan

Kalau modal sudah cukup kuat, buka layar Lahan: beli tambahan hektar (harga
naik progresif seiring luas totalmu), atau buka **Afdeling baru**
(butuh Asisten Afdeling untuk mengatur operasional harian di sana). Setiap
Ha baru otomatis menambah 143 pokok sesuai kepadatan standar industri.

---

## 7. Puncak — Membangun PKS Sendiri

Ini tujuan jangka panjang di game ini. Selama belum punya PKS (Pabrik
Kelapa Sawit) sendiri, semua TBS dijual mentah ke pabrik luar dengan harga
flat per tandan. Begitu kamu berhasil merekrut **Manager/ADM** (jenjang
tertinggi), opsi membangun PKS sendiri terbuka.

PKS mengikuti 6 stasiun sungguhan (Penerimaan & Sortasi → Sterilisasi →
Penebahan → Pengempaan → Klarifikasi → Pengolahan Biji), mengubah TBS jadi
CPO + inti sawit dengan rendemen 20-24% sesuai standar industri — margin
jauh lebih tinggi daripada jual TBS mentah, tapi butuh investasi besar di
depan.

---

## 8. Filosofi Desain (untuk yang penasaran kenapa sesuatu dibuat begitu)

Beberapa keputusan desain di game ini sengaja **tidak 100% realistis**,
demi tetap menyenangkan dimainkan:

- **Pematangan TBS dibuat lebih cepat dari rotasi panen sungguhan** (7-10
  hari di dunia nyata) — supaya pemasukan tetap mengalir dan kamu tidak
  menunggu lama tanpa progres.
- **Aksi massal instan, bukan menunggu pekerja berjalan** — supaya
  mengelola 143 pokok tetap terasa ringan, bukan pekerjaan administratif
  yang melelahkan.
- **Ancaman penyakit/hama diskalakan turun** dari kejadian sungguhan supaya
  tetap jadi tantangan yang bisa direspons dengan tenang, bukan sumber
  stres yang menguras modal terus-menerus.

Prinsipnya: **akurat di mana itu menambah rasa "nyata" (SOP, struktur SDM,
proses PKS, pola tanam), tapi dipercepat/disederhanakan di mana akurasi
penuh justru bikin game tidak enak dimainkan.**

---

## 9. Ringkasan Alur (garis besar)

```
Hari 1        : belajar siklus panen -> angkut -> jual (manual, 1 pokok)
Minggu 1      : mulai pakai aksi massal, kelola banyak pokok sekaligus
Bulan 1       : hama & Ganoderma mulai muncul, belajar merespons (bukan panik)
Bulan 1-3     : rekrut jenjang SDM, mulai ekspansi lahan
Pertengahan   : buka Afdeling baru, modal makin besar
Akhir         : rekrut ADM, bangun PKS sendiri, olah TBS jadi CPO+PK sendiri
```

Tidak ada "menang" dalam arti sempit — tujuan akhirnya adalah kebun yang
berjalan sehat, tim yang lengkap, dan pabrik sendiri yang berputar. Setelah
itu, permainan jadi soal terus mengoptimalkan: efisiensi tim, luas lahan,
dan kesehatan pokok dalam jangka panjang.

dalam kelanjutannya dan realitanya kita masih dihadapkan dengan persoalan penyakit dan penelitian tentang bagaimana 
agar palm selalu sustain dengan menciptakan beberapa produk hilirasi, yang mendukung ketahanan energi, pangan dan mendukung 
permasalahan global (cari research yang berkaitan dengan hal tersebut).
