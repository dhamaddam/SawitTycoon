#pragma once
// ============================================================================
// Sawit Tycoon — Core Engine Types
// Struktur data murni C++, tanpa dependensi rendering/platform apa pun.
// Mirror 1:1 dari model data yang sudah divalidasi di prototipe web (config.json
// + index.html), supaya perilaku game konsisten saat dipindah ke native.
// ============================================================================
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace sawit {

// ---------------------------------------------------------------------------
// Konfigurasi (data-driven) — identik skema dengan config.json versi web
// ---------------------------------------------------------------------------
struct HrPrereq {
    std::string key;
    int min = 0;
    bool hasAlso = false;
    std::string alsoKey;
    int alsoMin = 0;
};

struct HrLevelDef {
    std::string key;          // "buruh", "mandor", ...
    std::string name;
    std::string icon;
    double baseCost = 0;      // tidak dipakai untuk key=="buruh" (pakai prices.pekerja)
    double costGrowth = 1.0;
    double salary = 0;
    std::string desc;
    std::string cite;
    bool hasPrereq = false;
    HrPrereq prereq;
    int maxCount = -1;        // -1 = tak terbatas
};

struct PksStationDef {
    std::string name;
    std::string icon;
    std::string desc;
};

struct GameConfig {
    // landDensity
    int    pokokPerHa       = 143;   // SOP populasi tanah mineral 136–143 pokok/Ha
    double afdelingMaxHa    = 25;

    // harvestTool
    double toolAgeThresholdYears = 6; // < ambang: dodos, >= ambang: egrek

    // prices
    double pricePupuk       = 25000;
    double pricePestisida   = 20000;
    double priceFungisida   = 60000;
    double priceTebang      = 40000;
    double pricePekerja     = 900000;
    double pricePerTandan   = 55000;

    // economy
    double startMoney            = 750000;
    double dayLengthSeconds      = 75;
    int    tphCapStart           = 30;
    double tandanPerPokokPerHari = 0.045;

    // land pricing
    double haBasePrice        = 1200000;
    double haPriceGrowth      = 1.045;
    double afdelingBaseCost   = 15000000;
    double afdelingCostGrowth = 1.4;

    // pks economy
    double oerStart   = 0.20;
    double oerMax      = 0.24;
    double oerStep      = 0.01;
    double kerRate       = 0.05;
    double avgTandanKg    = 20;
    double cpoPrice        = 9800;
    double pkPrice          = 6200;
    double pksBuildCost      = 55000000;
    double pksUpgradeBaseCost = 35000000;
    double pksUpgradeGrowth   = 1.4;
    int    pksCapacityBase     = 20;
    int    pksCapacityPerLevel = 15;

    std::vector<HrLevelDef> hrLevels;
    std::vector<PksStationDef> stations;

    static GameConfig makeDefault(); // implementasi di config.cpp, isinya identik DEFAULT_CONFIG di web
};

// ---------------------------------------------------------------------------
// Pohon sawit
// ---------------------------------------------------------------------------
enum class FfbState : uint8_t { None, Growing, Ripe, Overripe };
enum class HealthState : uint8_t { Sehat, Hama, Ganoderma, Mati };

// Fase waktu harian -- Corley & Tinker (2016) + laporan lapangan industri:
// pemanen kerja ~6:30-13:30 WIB (Pagi = jam kerja UTAMA panen), transport
// TBS lanjut sampai ~19:00 (Siang = transport/aktivitas lanjutan, TBS yg
// SUDAH dipanen tetap bisa diangkut), Malam TANPA aktivitas panen BARU sama
// sekali (tak ada pemanenan malam hari dlm praktik nyata industri sawit --
// pekerja lapangan istirahat). Pembagian 40/35/25% dari 1 hari game dipilih
// spy Pagi (jendela panen) tetap jadi porsi TERBESAR (mencerminkan durasi
// jam kerja aktif yg SEBENARNYA jauh lbh panjang drpd waktu non-kerja
// malam), bukan pembagian sepertiga rata yg tak berdasar.
enum class TimeOfDay : uint8_t { Pagi, Siang, Malam };

struct Tree {
    int id = 0;
    double x = 0, z = 0;
    double ageYears = 2.0;
    double frond = 0.2;          // 0..1 tingkat pelepah menumpuk
    FfbState ffb = FfbState::Growing;
    double ffbTimer = 15.0;
    // Durasi PENUH tahap FFB saat ini (diisi SAMA PERSIS dgn ffbTimer setiap
    // kali state berubah/direset) -- fondasi countdown/progress bar per
    // pohon, fitur baru diminta pengguna: "countdown panen sawit / progress
    // bar pada setiap pohon... tampiilkan progressnya hanya ketika pohon
    // tersebut di klik". Dipakai utk hitung persentase: progress = 1.0 -
    // (ffbTimer/ffbTimerMax) -- TANPA field ini, tak mungkin tahu berapa
    // "total" durasi tahap saat ini (ffbTimer SENDIRI cuma sisa waktu,
    // MULAI dari nilai acak berbeda tiap kali direset -- lihat simTick_(),
    // engine.cpp).
    double ffbTimerMax = 15.0;
    HealthState health = HealthState::Sehat;
    double sickTimer = 0.0;
    double nutrition = 0.7;      // 0..1
    bool hasTbsReady = false;
    // Terisi saat dipanen dlm status Overripe (bukan Ripe) -- dipakai nanti
    // saat diangkut ke TPH utk menentukan harga jual (lihat kTerlewatDiscount
    // di engine.hpp). Literatur: Corley & Tinker (2016) "The Oil Palm" §11.5.5.1
    // -- tandan yg terlewat 1 putaran panen jadi sangat lewat matang, FFA
    // (asam lemak bebas) SUDAH MULAI NAIK begitu terlambat dipanen.
    bool tbsOverripe = false;

    // Penanda aksi massal terakhir — utk tampilkan ikon kecil di atas pohon
    // ("sudah dipupuk/disemprot/dst hari ini") & sbg dasar cek "jangan proses
    // 2x" pd aksi massal instan (lihat Engine::actionXSemua()). -1 = belum
    // pernah/bukan hari ini. Kode: 0=panen,1=angkut,2=pupuk,3=pestisida,4=fungisida.
    int lastMarkDay = -1;
    int lastMarkKind = -1;

    bool isMature(const GameConfig& cfg) const { return ageYears >= cfg.toolAgeThresholdYears; }
};

// ---------------------------------------------------------------------------
// Lahan / Afdeling
// ---------------------------------------------------------------------------
struct Afdeling {
    int id = 0;
    double ha = 0;
    std::string name;
};

// ---------------------------------------------------------------------------
// BLOCK — unit operasional kebun, di BAWAH Afdeling dan di ATAS baris/pokok.
// Hierarki: Kebun > Afdeling > Block > Baris > Pokok (& Tandan).
// "Block terdiri dari banyak baris tanaman + infrastruktur lokal" -- bukan
// 1 baris = 1 block. Setiap Block mereferensikan RENTANG indeks pohon di
// trees_ (bukan menyalin data pohon), spy tak ada duplikasi/sinkronisasi.
// Corley & Tinker (2016) tidak menetapkan satu ukuran block universal --
// ukuran (ha) adalah keputusan desain game, bukan klaim langsung dari buku
// (density 143 pokok/ha yg SUDAH kita pakai memang dirujuk di buku).
// ---------------------------------------------------------------------------
struct Block {
    int id = 0;
    std::string name;      // mis. "A01"
    int afdelingId = 0;
    double ha = 0;
    int treeStartIdx = 0;   // rentang [treeStartIdx, treeEndIdx) di trees_
    int treeEndIdx = 0;
    double originX = 0;     // pusat grid pohon block ini di dunia (unit game)
    double originZ = 0;     // -- block baru (dari beliHa) ditempatkan di
                             // originX berbeda, area terpisah tanpa tumpang tindih

    // --- Pembeda antar-block: kesuburan tanah & material genetik --------
    // Corley & Tinker (2016) "The Oil Palm" 5th ed.:
    //  - §9.2.3.5 "Soil fertility": "an inherently fertile soil... has
    //    management advantages over an infertile one" -- kesuburan tanah
    //    BERVARIASI antar lokasi scr alami (bukan seragam sekebun), diukur
    //    lewat survei tanah SEBELUM pembukaan lahan (§9.2.5 Land evaluation).
    //  - Bab 6 (breeding/seed production) & Section 2.2.2.6: benih komersial
    //    D×P (Dura x Pisifera) dr SUMBER/PRODUSEN berbeda punya potensi hasil
    //    & vigor pertumbuhan berbeda -- bukan satu genetik seragam.
    // Diinisialisasi ACAK-TAPI-TETAP tiap block dibuat (newGame/beliHa),
    // MENGGERAKKAN nutrisi dasar seluruh pohon di block itu (lihat
    // generateBlockTrees_) -- jadi block "tanah subur+benih unggul" scr
    // VISUAL kanopinya lebih rimbun/hijau tua (via sistem vigor & pemucatan
    // nutrisi yg SUDAH ADA, tanpa perlu geometri/mesh baru sama sekali).
    double soilFertility = 1.0; // ~0.70 (marjinal) .. 1.30 (sangat subur)
    double geneticVigor = 1.0;  // ~0.85 (benih standar) .. 1.15 (benih unggul)

    // --- TPH SENDIRI per block (bukan 1 titik global lagi) ---------------
    // Corley & Tinker (2016): TPH/"roadside collection point" terkait JALAN
    // KOLEKSI, yg ada di TIAP block (bukan 1 utk seluruh kebun) -- celah
    // diperbaiki: sebelumnya SEMUA block berbagi 1 TPH global (kTphX/kTphZ),
    // jadi pekerja di block jauh (mis. A02 di originX=120) harus jalan
    // balik puluhan unit ke TPH dekat Block A01 -- tak realistis & lambat.
    // Posisi TPH tiap block memakai OFFSET RELATIF yg sama dr originX/Z-nya
    // sendiri (persis spt Block A01 dulu), jadi tiap block dapat TPH di
    // tepi grid-nya sendiri.
    double tphX = 0, tphZ = 0;
    double tphStock = 0;          // TBS matang normal, KHUSUS block ini
    double tphStockOverripe = 0;  // TBS lewat-matang, KHUSUS block ini
};

// Agregat status Block, dihitung LIVE dari kondisi pohon saat ini (bukan
// disimpan/di-cache) -- dipakai Estate View ("🟢 normal / 🟡 perhatian /
// 🟠 masalah / 🔴 kritis" per block) & Block View (ringkasan "744 pokok, 42
// TBS matang, dst").
struct BlockSummary {
    int id = 0;
    std::string name;
    double ha = 0;
    int treeCount = 0;
    int healthyCount = 0;
    int hamaCount = 0;
    int ganodermaCount = 0;
    int deadCount = 0;
    int readyToHarvestCount = 0;  // ffb Ripe atau Overripe
    int tbsAwaitingPickupCount = 0; // hasTbsReady==true (sudah dipanen, blm diangkut)
    // Proxy visual "kekurangan N" -- engine kita blm memodelkan N/P/K/Mg
    // terpisah (cuma 1 dimensi nutrition gabungan), jadi ambang <0.4 dipakai
    // sbg proxy "kekurangan hara" scr umum (bukan klaim spesifik N doang).
    int lowNutritionCount = 0;
    double originX = 0, originZ = 0; // pusat grid block -- dipakai UI melompat kamera ke sini
    double soilFertility = 1.0, geneticVigor = 1.0; // lihat catatan di Block, types.hpp
    double tphStock = 0, tphStockOverripe = 0; // stok TPH KHUSUS block ini
    double tphX = 0, tphZ = 0; // posisi TPH block ini (dunia game)
};

struct LandState {
    std::vector<Afdeling> afdelings;
    double sampleBlockHa = 1.0; // blok kebun inti yg dirender detail = 143 pokok = 1 Ha penuh
                                 // (SOP populasi 143 pokok/Ha, pola segitiga sama sisi 9x9x9m)
};

// ---------------------------------------------------------------------------
// SDM (jenjang jabatan)
// ---------------------------------------------------------------------------
struct HrState {
    int buruh = 0; // sinkron dengan jumlah Worker di render layer; disimpan juga di sini utk kalkulasi murni-logic
    int mandor = 0;
    int krani = 0;
    int mandorBesar = 0;
    int kraniKepala = 0;
    int asistenAfdeling = 0;
    int asistenKepala = 0;
    int manager = 0;

    int countFor(const std::string& key) const;
    void add(const std::string& key, int n = 1);
};

// Ringkasan status LIVE 1 jenjang SDM -- dipakai UI dialog rekrut (blm ada
// UI-nya sebelum ini, dilaporkan pengguna: tombol rekrut tak ada sama sekali
// meski fungsi rekrutLevel() sudah lama ada di engine).
struct HrLevelInfo {
    std::string key, name, icon, desc, cite;
    int count = 0;
    double cost = 0;
    double salary = 0;
    bool prereqMet = true;    // prasyarat jabatan (mis. 3 buruh dulu) terpenuhi?
    bool underMax = true;     // blm capai batas maksimum jabatan ini (mis. Manager cuma 1)?
    std::string prereqDesc;   // deskripsi prasyarat utk ditampilkan, kosong kalau tak ada
};

// ---------------------------------------------------------------------------
// PKS
// ---------------------------------------------------------------------------
struct PksState {
    bool built = false;
    int level = 1;
    double oer = 0.20;
    double kerRate = 0.05;
    double inputSilo = 0;
    double cpoPrice = 9800;
    double pkPrice = 6200;
    double avgTandanKg = 20;
    double buildCost = 55000000;
    double upgradeCost = 35000000;
};

// ---------------------------------------------------------------------------
// State ekonomi utama
// ---------------------------------------------------------------------------
struct EconomyState {
    double money = 750000;
    int day = 1;
    double dayTimer = 0;
    double dayLength = 75;
    // tphStock/tphStockOverripe DIPINDAH jadi per-block (Block.tphStock/
    // Block.tphStockOverripe, types.hpp) -- celah diperbaiki: TPH global
    // tunggal tak masuk akal begitu ada banyak block terpisah jauh scr
    // spasial (lihat catatan lengkap di Block). tphCap TETAP di sini, tapi
    // maknanya sekarang kapasitas SETIAP TPH block (bukan gabungan sekebun).
    double tphCap = 30;
    // Jenjang upgrade kapasitas TPH -- fitur baru diminta pengguna: "upgrade
    // kapasitas tph, saat ini tph hanya mampu menampung 30 tbs, harusnya ada
    // tambah fitur tph atau fitur memperbesar tempat penampungan tph". TAK
    // ADA referensi kuantitatif ilmiah utk "kapasitas TPH" (scr praktik
    // nyata TPH adalah area terbuka di tepi ancak/jalan, TBS ditumpuk di
    // tanah sampai truk datang -- bukan struktur berkapasitas tetap spt
    // silo) -- angka murni keputusan game balance, KONSISTEN dgn pola
    // upgrade PKS yg sudah ada (level naik, kapasitas & cost mengikuti).
    // GLOBAL (semua TPH di semua block ikut naik bersamaan) -- konsisten dgn
    // tphCap yg jg global, BUKAN per-block terpisah (perubahan arsitektur
    // lebih besar, di luar scope perbaikan minimal yg diminta).
    int tphLevel = 1;
    double pricePerTandan = 55000;
    double moraleMultiplier = 1.0;
};

// Pose visual pekerja, dipetakan dari jenis tugas yg sedang dikerjakan --
// meniru referensi ilustrasi: 0=berdiri netral, 1=jongkok memungut (spt
// mupuk/pungut di tanah), 2=pakai alat menyapu/mendorong (tunas/semprot),
// 3=meraih ke atas memanen (panen), 4=membawa keranjang (angkut->TPH).
enum class WorkerPose : uint8_t { Idle=0, Kneel=1, Tool=2, Reach=3, Carry=4 };

// ---------------------------------------------------------------------------
// Info render pekerja (posisi & status), dipakai renderer utk menggambar
// karakter pekerja di layar — sebelumnya pekerja disimulasikan tapi TIDAK
// PERNAH digambar sama sekali (celah nyata yang ditemukan pengguna).
// ---------------------------------------------------------------------------
struct WorkerRenderInfo {
    double x = 0, z = 0;
    bool busy = false;
    bool carrying = false; // sedang membawa TBS (fase angkut menuju TPH)
    WorkerPose pose = WorkerPose::Idle;
    // Utk pose Reach (panen): true=egrek (pokok tinggi, gerakan MENARIK KE
    // BAWAH dgn hentakan), false=dodos (pokok rendah, gerakan MENDORONG KE
    // ATAS) -- lihat literatur di renderer_gl.cpp buildDodosEgrekAnim().
    bool usingEgrek = false;
    // Arah hadap (radian, atan2 dr vektor GERAKAN target-posisi_sekarang) --
    // BUG diperbaiki: dulu drawWorker() TAK PUNYA parameter arah sama sekali
    // (selalu menghadap arah dunia tetap), jadi pekerja terlihat "salah
    // orientasi" dibanding baris tanam yg ditanam miring/diagonal (dilaporkan
    // pengguna: "posisi rotasi player... tidak sesuai dgn posisi tanah").
    // 0 kalau idle/diam (tak ada arah gerakan yg berarti).
    double facingRad = 0;
};

// ---------------------------------------------------------------------------
// Status truk TPH->PKS — animasi visual saat TBS diangkut keluar dari TPH
// (referensi: traktor+trailer berisi TBS, seperti gambar yg diberikan
// pengguna). Murni kosmetik: transaksi ekonomi (uang masuk) tetap terjadi
// INSTAN spt sebelumnya, animasi ini cuma lapisan visual di atasnya supaya
// pemain benar-benar "melihat" hasil kerjanya terangkut, bukan menggantung
// keputusan ekonomi pada selesainya animasi (menghindari bug/edge-case).
// ---------------------------------------------------------------------------
struct TruckState {
    bool active = false;
    double progress = 0.0; // 0..1, dari TPH menuju keluar area (arah PKS)
    int blockId = -1; // block mana yg sedang diproses -- TPH-nya beda posisi per block sekarang
};

// Avatar pemain yang bisa DIGERAKKAN LANGSUNG (Gameplay Mode, third-person)
// -- hasil review eksternal poin #5: "third-person, perspective camera...
// Tambahkan smooth follow, collision, zoom terbatas". Fitur BARU sepenuhnya
// -- game SEBELUMNYA murni tap-to-command (ketuk pohon/tombol, pekerja NPC
// bergerak sendiri), TIDAK ADA karakter yang dikontrol langsung pemain.
// facingRad: arah hadap avatar (dipakai animasi jalan drawWorker() yg SUDAH
// ADA -- avatar pemain pakai model & animasi yg SAMA dgn worker biasa,
// bukan model terpisah, demi konsistensi visual & efisiensi).
struct PlayerAvatarState {
    double x = 0.0, z = 0.0;
    double facingRad = 0.0;
    bool moving = false; // true selama input joystick aktif -- dipakai animasi jalan (poseCode)
};

// ---------------------------------------------------------------------------
// Satu entri log aktivitas — permanen selama sesi (beda dgn EngineEvent yg
// cuma dipoll sekali lalu hilang), utk layar "Log Aktivitas" yg bisa dibuka
// kapan saja dan menampilkan riwayat lengkap.
// ---------------------------------------------------------------------------
struct LogEntry {
    int day = 1;
    std::string text;
    int treeId = -1; // -1 = tak terkait pohon tertentu (mis. rekrut SDM, jual TBS ke PKS)
};

} // namespace sawit
