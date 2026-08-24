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

struct Tree {
    int id = 0;
    double x = 0, z = 0;
    double ageYears = 2.0;
    double frond = 0.2;          // 0..1 tingkat pelepah menumpuk
    FfbState ffb = FfbState::Growing;
    double ffbTimer = 15.0;
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
    double tphStock = 0;          // TBS matang normal
    double tphStockOverripe = 0;  // TBS lewat-matang -- dijual dgn diskon (lihat kTerlewatDiscount)
    double tphCap = 30;           // kapasitas GABUNGAN (tphStock + tphStockOverripe)
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
};

// ---------------------------------------------------------------------------
// Satu entri log aktivitas — permanen selama sesi (beda dgn EngineEvent yg
// cuma dipoll sekali lalu hilang), utk layar "Log Aktivitas" yg bisa dibuka
// kapan saja dan menampilkan riwayat lengkap.
// ---------------------------------------------------------------------------
struct LogEntry {
    int day = 1;
    std::string text;
};

} // namespace sawit
