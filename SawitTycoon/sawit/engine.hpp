#pragma once
// ============================================================================
// Sawit Tycoon — Public Engine API
// Ini adalah "permukaan" yang dipanggil oleh jembatan platform:
//   - Android: platform/android/app/src/main/cpp/sawit_jni.cpp (JNI)
//   - iOS    : platform/ios/SawitTycoon/Bridge/EngineBridge.mm (Obj-C++)
// Engine ini TIDAK tahu apa pun soal rendering/OpenGL/UI — murni simulasi &
// aturan bisnis, supaya bisa dites headless (lihat engine/tests/engine_test.cpp)
// dan supaya kode yang sama dipakai identik di Android & iOS (skema seragam).
// ============================================================================
#include "sawit/types.hpp"
#include <functional>

namespace sawit {

// Event yang dikirim engine ke platform layer (untuk toast/flytext/UI update).
// Platform layer (Kotlin/Swift) menerjemahkan ini ke widget nativenya sendiri.
enum class EventType {
    Toast,          // pesan singkat, payload = teks
    FlyMoney,       // teks melayang di posisi TPH, payload = teks (mis. "+Rp 55.000")
    TreeChanged,    // treeId berubah kondisi -> perlu re-render mesh pohon itu
    HudChanged,     // saldo/hari/TPH berubah -> perlu refresh HUD
    ScreenChanged,  // data layar (Lahan/SDM/PKS) berubah -> perlu refresh panel itu
};

struct EngineEvent {
    EventType type;
    std::string text;
    int treeId = -1;
};

using EventSink = std::function<void(const EngineEvent&)>;

class Engine {
public:
    explicit Engine(GameConfig cfg = GameConfig::makeDefault());

    // --- lifecycle ---
    void newGame();                         // reset total dengan config saat ini
    void loadConfig(const GameConfig& cfg); // ganti config saat runtime (live-ops)
    void setEventSink(EventSink sink) { events_ = std::move(sink); }

    // --- game loop ---
    // dt dalam detik. Panggil setiap frame dari platform render loop.
    void tick(double dt);

    // --- aksi pemain: kebun ---
    bool actionTunas(int treeId);
    bool actionPanen(int treeId);
    bool actionAngkut(int treeId);           // pekerja bawa TBS pohon -> TPH (2 fase ditangani internal)
    bool actionPupuk(int treeId);
    bool actionPestisida(int treeId);
    bool actionFungisida(int treeId);
    bool actionTebangTanamUlang(int treeId);

    // --- Aksi MASSAL: terapkan ke SEMUA pohon yg memenuhi syarat SEKALIGUS,
    // INSTAN -- pekerja TIDAK perlu mendatangi pohon satu-persatu (desain
    // direvisi atas permintaan eksplisit: gameplay lebih menyenangkan drpd
    // realisme kapasitas panen 100-300 tandan/hari/tim, Akvopedia "Budidaya
    // Kelapa Sawit Berkelanjutan" -- lihat catatan panjang di engine.cpp).
    // Panen & Angkut SELALU gratis (Angkut dibatasi kapasitas TPH). Pupuk/
    // Pestisida/Fungisida menghitung TOTAL biaya utk semua pohon yg memenuhi
    // syarat lebih dulu -- kalau uang cukup, SEMUA diproses sekaligus (satu
    // transaksi); kalau tidak cukup, TIDAK ADA yg diproses (bukan sebagian,
    // supaya hasilnya jelas & tidak membingungkan). Setiap pohon yg diproses
    // ditandai (Tree::lastMarkDay/lastMarkKind) utk indikator visual di layar.
    // Mengembalikan jumlah pohon yg diproses.
    int actionPanenSemua();
    int actionAngkutSemua();
    int actionPupukSemua();
    int actionPestisidaSemua();
    int actionFungisidaSemua();

    // --- aksi pemain: TPH / penjualan ---
    void kirimTruk();                        // jual/kirim seluruh stok TPH sekarang juga

    // --- aksi pemain: lahan ---
    bool beliHa(double amountHa);
    bool bukaAfdelingBaru();

    // --- aksi pemain: SDM ---
    bool rekrutLevel(const std::string& key);
    double hrEfficiency() const;
    double totalDailySalary() const;

    // --- aksi pemain: PKS ---
    bool bangunPks();
    bool upgradePks();
    bool prosesBatchPks();                   // instan (non-blocking); platform yang atur animasi timer bila perlu

    // --- getter read-only untuk UI ---
    const GameConfig& config() const { return cfg_; }
    const EconomyState& economy() const { return eco_; }
    const LandState& land() const { return land_; }
    const HrState& hr() const { return hr_; }
    const PksState& pks() const { return pksState_; }
    const std::vector<Tree>& trees() const { return trees_; }
    Tree* treeById(int id);

    double totalHa() const;

    // --- BLOCK: unit operasional di bawah Afdeling (lihat types.hpp) ---
    // fondasi hierarki Kebun>Afdeling>Block>Baris>Pokok utk Estate/Block View.
    const std::vector<Block>& blocks() const { return blocks_; }
    // Agregat status dihitung LIVE dari kondisi pohon saat ini (bukan cache).
    std::vector<BlockSummary> blockSummaries() const;
    // Cari Block yg memuat treeId tertentu (mis. utk tahu "pohon ini di block
    // mana" saat pemain klik pohon di Estate/Block View). -1 kalau tak ketemu.
    int blockIdForTree(int treeId) const;
    int totalPokok() const;
    double haPricePerUnit() const;

    // --- pekerja: posisi & status utk digambar renderer (celah yg diperbaiki --
    //     sebelumnya pekerja disimulasikan tapi tak pernah tergambar) ---
    std::vector<WorkerRenderInfo> workersRenderInfo() const;
    static double tphWorldX() { return kTphX; }
    static double tphWorldZ() { return kTphZ; }
    static double officeWorldX() { return kOfficeX; }
    static double officeWorldZ() { return kOfficeZ; }
    static double gateWorldX() { return kGateX; }
    static double gateWorldZ() { return kGateZ; }

    // --- truk TPH->PKS: animasi visual, murni kosmetik (lihat catatan di
    //     TruckState, types.hpp) ---
    bool truckActive() const { return truck_.active; }
    double truckProgress() const { return truck_.progress; }

    // --- log aktivitas: riwayat PERMANEN selama sesi (beda dgn event sink yg
    //     sekali poll lalu hilang) -- utk layar "Log Aktivitas" yg pemain bisa
    //     buka kapan saja dan melihat semua yg pernah terjadi. ---
    int activityLogCount() const { return (int)activityLog_.size(); }
    // Terbaru DULUAN (index 0 = paling baru), format "Hari X: teks".
    std::string activityLogEntry(int indexFromNewest) const;

    // --- utilitas khusus testing/dev (BUKAN bagian gameplay normal — jangan dipanggil dari UI) ---
    void devAddMoney(double amount) { eco_.money += amount; }
    // Mengacak kesehatan/nutrisi/status TBS seluruh kebun -- MURNI alat uji
    // visual (spy variasi kanopi/warna dari vigor & pemucatan nutrisi bisa
    // langsung terlihat, bukan nunggu berhari-hari game-time spt kondisi
    // alami). Tidak dipakai jalur gameplay normal.
    void devRandomizeConditions();
    std::string saveToJson() const;
    bool loadFromJson(const std::string& json);

private:
    // Posisi dunia TPH — DIPINDAH ke TEPI kebun (bukan di tengah barisan
    // tanam spt sebelumnya) sesuai literatur: TPH sungguhan berada di depan
    // jalur pokok, di pinggir JALAN KOLEKSI (bukan terkubur di antara pokok).
    // "Biasanya dalam 3 pasar pikul terdapat 1 TPH yang letaknya di depan
    // jalur pokok yang berada di pinggir jalan koleksi" (Fitriherdiyanti,
    // budidaya kelapa sawit; ICDX "Istilah-istilah Penting dalam Kebun
    // Sawit": "Jalan koleksi: jalan yang menghubungkan TPH dengan jalan
    // utama"). Grid 143 pokok (11x13, lihat engine.cpp) membentang kira2
    // X:[-26,29], Z:[-27,27] -- TPH diletakkan di X=32 (persis di luar tepi
    // kebun, spt posisinya di sepanjang jalan koleksi sungguhan).
    static constexpr double kTphX = 32.0, kTphZ = 0.0;
    // Posisi kantor/mess & portal gerbang kebun -- elemen fisik nyata yg
    // sebelumnya tak ada sama sekali di scene (celah yg diperbaiki, terinspirasi
    // analisis referensi visual "smart farming"): pos satpam & portal memang
    // praktik standar di stasiun penerimaan kebun sawit ("Pos satpam ...
    // mengecek legalitas keluar dan masuk barang ... surat jalan dan surat
    // pengantar buah" -- tugas Kerani Timbang Sawit). Diletakkan di area yg
    // sama dgn TPH/jalan koleksi (tepi kebun), krn di situlah kru & kendaraan
    // sungguhan berkumpul, BUKAN di tengah barisan tanam.
    // Posisi kantor/mess -- DISELARASKAN dgn posisi drawFarmhouse() di
    // renderer_gl.cpp (38,15), supaya pekerja spawn PERSIS di depan bangunan
    // yg sungguhan terlihat, bukan di titik kosong terpisah. Portal gerbang
    // (kGateX/Z) elemen BARU: praktik standar stasiun penerimaan kebun sawit
    // sungguhan ("Pos satpam ... mengecek legalitas keluar dan masuk barang
    // ... surat jalan dan surat pengantar buah" -- tugas Kerani Timbang Sawit),
    // diletakkan di ujung jalan keluar dari TPH menuju arah PKS.
    static constexpr double kOfficeX = 38.0, kOfficeZ = 15.0;
    static constexpr double kGateX = 55.0, kGateZ = 0.0;
    // Diskon harga jual TBS lewat-matang -- berdasar Corley & Tinker (2016)
    // "The Oil Palm" 5th ed. §11.5.5.1: tandan yg terlewat 1 putaran panen
    // FFA (asam lemak bebas)-nya SUDAH MULAI NAIK, menurunkan mutu/harga jual
    // sungguhan. 15% dipilih sbg angka ilustratif konservatif (buku tak
    // mencantumkan persentase diskon harga pasti, hanya kualitatif "sudah
    // mulai naik") -- cukup signifikan utk memberi insentif nyata memanen
    // tepat waktu, tanpa membuat telat panen jadi hukuman ekstrem.
    static constexpr double kOverripeDiscount = 0.85; // dijual 85% dari harga normal

    // Konstanta grid pohon (SATU sumber kebenaran, dipakai newGame() DAN sistem
    // jalur koridor -- lihat computeCorridorPath_). Pola segitiga sama sisi
    // sesuai SOP 143 pokok/Ha (9x9x9m): jarak antar tanaman 9m, jarak antar
    // BARIS 7,8m (rasio 7,8/9=0,867).
    static constexpr int kGridCols = 11, kGridRows = 13; // 11x13 = 143 persis
    static constexpr double kColSpacing = 5.2;             // skala permainan
    static constexpr double kRowSpacing = kColSpacing * 0.8667; // rasio SOP 7.8/9
    static constexpr double kGridOriginX = -((kGridCols-1)*kColSpacing)/2.0;
    static constexpr double kGridOriginZ = -((kGridRows-1)*kRowSpacing)/2.0;
    static constexpr double kWorkerWalkSpeed = 5.0; // unit dunia/detik -- cukup lambat spy pergerakan
        // jelas terlihat (celah yg diperbaiki: dulu fase jalan cuma pecahan tetap dari
        // durasi tugas, kadang <1 detik apapun jaraknya), tapi cukup cepat spy aksi
        // angkut manual ke TPH (kini di tepi kebun, bisa >50 unit jauhnya) tak bikin
        // pemain menunggu puluhan detik utk SATU pohon -- utk banyak pohon sekaligus,
        // pakai tombol "Angkut Semua" yg instan (lihat actionAngkutSemua()).

    GameConfig cfg_;
    EconomyState eco_;
    LandState land_;
    HrState hr_;
    PksState pksState_;
    std::vector<Tree> trees_;
    std::vector<Block> blocks_; // fondasi hierarki Afdeling>Block>Baris>Pokok

    struct WorkerJob {
        bool busy = false;
        std::string kind;   // tunas | panen | pupuk | pestisida | fungisida | tebang | angkut
        int treeId = -1;
        std::string phase;  // khusus angkut: "walk" -> "toTPH"
        double remaining = 0;
        // --- posisi utk render (baru) ---
        double x = 0, z = 0;                 // posisi saat ini di dunia
        double startX = 0, startZ = 0;       // posisi saat job ini dimulai
        double targetX = 0, targetZ = 0;     // tujuan (pohon yg dikerjakan / TPH)
        // Jalur JALAN via koridor antar baris (gawangan) -- BUKAN garis lurus
        // diagonal yg bisa "menembus" kanopi pohon lain (literatur: Corley &
        // Tinker 2016 "harvesting paths" antar baris tanam; SOP "pasar pikul").
        // wp1=segaris start tp di garis koridor terdekat, wp2=segaris target
        // di koridor yg sama -- jalur jadi start->wp1->wp2->target (3 segmen
        // menyusuri gawangan), bukan 1 garis lurus start->target.
        double wp1X = 0, wp1Z = 0, wp2X = 0, wp2Z = 0;
        double walkDuration = 0;             // lama fase JALAN (berbasis jarak&kecepatan, lihat startJob_)
        double totalDuration = 1;            // walkDuration + waktu kerja di tempat
        bool carrying = false;               // sedang bawa TBS (visual, fase angkut->toTPH)
    };
    std::vector<WorkerJob> workers_;
    TruckState truck_;
    static constexpr double kTruckDurationSec = 3.5; // lama animasi truk keluar dari TPH
    double simAccum_ = 0;      // akumulator detik utk tick simulasi 1 Hz (pertumbuhan pohon dst)
    double tphAutoTimer_ = 26; // truk auto-jemput TPH tiap N detik
    bool autoMode_ = true;     // pekerja tambahan (selain slot ke-1) otomatis cari kerjaan
    unsigned rngState_ = 0x9E3779B9u; // xorshift sederhana, tanpa <random> agar deterministik & ringan

    double randUnit_(); // 0..1

    EventSink events_;
    void emit(EventType t, const std::string& text = "", int treeId = -1);
    std::vector<LogEntry> activityLog_; // terbaru di BELAKANG vector, lihat activityLogEntry()
    static constexpr int kActivityLogMax = 500; // batasi memori, buang yg tertua kalau kepenuhan

    void onNewDay_();
    void simTick_(double dt);
    void updateWorkers_(double dt);
    // Hitung 2 waypoint jalur koridor (menyusuri gawangan antar baris, bukan
    // garis lurus diagonal) dari start ke target -- lihat catatan WorkerJob.
    // targetTreeId dipakai mencari originZ BLOCK yg benar (bukan cuma
    // kGridOriginZ global) -- penting skrg krn block baru (dari beliHa) bisa
    // punya originZ berbeda dari Block A01. -1 (target bukan pohon, mis.
    // TPH/kantor) -> pakai kGridOriginZ (block pertama) sbg fallback wajar.
    void computeCorridorPath_(double startX, double startZ, double targetX, double targetZ, int targetTreeId,
                               double* wp1X, double* wp1Z, double* wp2X, double* wp2Z) const;
    void completeJob_(WorkerJob& job);
    void startJob_(int workerIdx, const std::string& kind, int treeId, double targetX, double targetZ);
    int findFreeWorker_() const;
    // Bangun grid segitiga 143 pohon (pola SAMA dgn newGame()) di originX/Z
    // manapun, MENAMBAHKAN ke trees_ (bukan reset) -- dipakai newGame() UTK
    // Block A01 (origin 0,0) dan beliHa() utk block baru (origin berbeda).
    // Return [startIdx,endIdx) rentang trees_ yg baru ditambahkan.
    void generateBlockTrees_(double originX, double originZ, int& outStartIdx, int& outEndIdx);
    void autoAssign_(int workerIdx);
    void sellOrProcessTbs_(double amount, bool silent);

    const HrLevelDef* findLevel_(const std::string& key) const;
    bool prereqOk_(const HrLevelDef& def) const;
    bool maxOk_(const HrLevelDef& def) const;
    double costFor_(const HrLevelDef& def) const;
};

} // namespace sawit
