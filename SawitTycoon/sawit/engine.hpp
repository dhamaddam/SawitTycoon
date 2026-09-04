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
#include <cmath>

namespace sawit {

// Event yang dikirim engine ke platform layer (untuk toast/flytext/UI update).
// Platform layer (Kotlin/Swift) menerjemahkan ini ke widget nativenya sendiri.
enum class EventType {
    Toast,          // pesan singkat, MUNCUL di layar (Toast/Alert) + masuk log
    FlyMoney,       // teks melayang di posisi TPH, payload = teks (mis. "+Rp 55.000")
    TreeChanged,    // treeId berubah kondisi -> perlu re-render mesh pohon itu
    HudChanged,     // saldo/hari/TPH berubah -> perlu refresh HUD
    ScreenChanged,  // data layar (Lahan/SDM/PKS) berubah -> perlu refresh panel itu
    // Pesan yg HANYA masuk log permanen (activityLog_), TIDAK memicu Toast
    // visual di layar -- mengatasi keluhan pengguna: pesan "pohon sudah
    // dikerjakan pekerja lain" muncul berkali-kali mengganggu tiap kali
    // pemain mencoba pohon yg sama (misal ketuk tombol aksi berulang saat
    // menunggu). Info ini tetap relevan sbg riwayat (+ treeId spy bisa
    // diketuk lompat ke pohon itu di Log), tapi TAK cukup penting utk
    // mengganggu layar tiap kali terjadi.
    LogOnly,
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
    void kirimTruk(int blockId);              // jual/kirim seluruh stok TPH block ini sekarang juga

    // --- aksi pemain: lahan ---
    bool beliHa(double amountHa);
    bool bukaAfdelingBaru();

    // --- aksi pemain: SDM ---
    bool rekrutLevel(const std::string& key);
    // Ringkasan status LIVE semua jenjang SDM (biaya, jumlah, kelayakan) --
    // dipakai UI dialog rekrut.
    std::vector<HrLevelInfo> hrLevelInfos() const;
    double hrEfficiency() const;
    double totalDailySalary() const;

    // --- aksi pemain: PKS ---
    bool bangunPks();
    // Upgrade kapasitas TPH -- fitur baru diminta pengguna. Lihat catatan lengkap di engine.cpp.
    bool upgradeTph();
    double tphUpgradeCost() const;
    bool upgradePks();
    bool prosesBatchPks();                   // instan (non-blocking); platform yang atur animasi timer bila perlu

    // --- getter read-only untuk UI ---
    const GameConfig& config() const { return cfg_; }
    const EconomyState& economy() const { return eco_; }
    const LandState& land() const { return land_; }
    const HrState& hr() const { return hr_; }
    const PksState& pks() const { return pksState_; }
    // Biaya upgrade PKS SAAT INI (naik tiap level, formula sama persis dgn
    // upgradePks()) -- dibutuhkan dialog PKS utk tampilkan estimasi biaya
    // sebelum eksekusi, bukan cuma angka dasar statis.
    double pksUpgradeCostNow() const {
        return std::round(pksState_.upgradeCost * std::pow(cfg_.pksUpgradeGrowth, pksState_.level-1));
    }
    // Biaya/kapasitas KOMPUTASI (gabungan state+config) -- dibutuhkan UI
    // dialog PKS utk tampilkan estimasi sebelum eksekusi (pola sama dgn
    // dialog konfirmasi aksi massal & Lahan/SDM).
    // (pksBuildCost()/pksNextUpgradeCost() dihapus -- audit menemukan
    // keduanya DUPLIKAT PERSIS dari pks().buildCost & pksUpgradeCostNow()
    // yg sudah dipakai JNI/EngineBridge, tak pernah dipanggil dari mana pun)
    int pksCapacityPerBatch() const { return cfg_.pksCapacityBase + pksState_.level*cfg_.pksCapacityPerLevel; }
    const std::vector<Tree>& trees() const { return trees_; }
    Tree* treeById(int id);
    // Cari pohon TERDEKAT dari posisi avatar (Gameplay Mode) dalam radius
    // maxDist -- fondasi interaksi "dekati pohon, muncul tombol aksi",
    // sejalan dgn drawWorker() yg pakai model/animasi sama dgn worker biasa.
    // Return -1 kalau tak ada pohon dlm jangkauan (pohon Mati DIABAIKAN --
    // tak bisa diinteraksi, sesuai perilaku tap-to-select yg sudah ada).
    int nearestTreeToPlayer(double maxDist) const;
    // Hitung jarak AMAN kamera di belakang avatar (sepanjang arah facingRad,
    // menuju posisi kamera) supaya tak menembus batang pohon -- collision
    // kamera SEBELUMNYA cuma menangani tanah (ground clearance), belum
    // pohon sama sekali. Raycast sederhana dari posisi avatar ke arah
    // kamera (BELAKANG avatar, berlawanan facingRad), berhenti di pohon
    // pertama yg terhalang dlm rentang [0, desiredDist]. Return desiredDist
    // apa adanya kalau tak ada pohon menghalangi.
    double cameraSafeDistance(double playerX, double playerZ, double facingRad, double desiredDist) const;

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
    // Posisi Z gawangan MATI (jalur antar-baris yg sengaja ditutup, tempat
    // tumpukan pelepah hasil tunas -- lihat drawFrondPile(), renderer_gl.cpp)
    // RELATIF thd originZ block (0,0) -- pemanggil (JNI/EngineBridge) tinggal
    // tambahkan b.originZ sendiri. Selang-seling: gawangan GENAP (antara
    // baris 0-1, 2-3, dst) = mati, GANJIL = hidup (dilalui pekerja/truk) --
    // SOP Palm Oil Plantation: "pancang rumpukan jadi dasar gawangan mati
    // sejak pancang tanam" -- pola TETAP, bukan berubah tiap hari.
    static std::vector<double> deadRowZOffsets(){
        std::vector<double> out;
        for (int r=0; r<kGridRows-1; r+=2){
            out.push_back(kGridOriginZ + (r+0.5)*kRowSpacing);
        }
        return out;
    }
    // Posisi Z gawangan HIDUP (kebalikan dari deadRowZOffsets() di atas --
    // indeks GANJIL, jalur BERSIH yg dilalui pekerja/truk utk panen &
    // inspeksi). Fondasi visual "jalan" yg jelas (poin dokumen review #7:
    // "Jalan panen dan jalan inspeksi harus memiliki struktur yang jelas") --
    // SEBELUMNYA tak ada penanda visual sama sekali yg membedakan jalur ini
    // dari tanah biasa (cuma "bersih" scr LOGIKA/gameplay krn tak ada mulsa
    // pelepah, tapi scr VISUAL warnanya identik dgn seluruh tanah lain).
    static std::vector<double> livingRowZOffsets(){
        std::vector<double> out;
        for (int r=1; r<kGridRows-1; r+=2){
            out.push_back(kGridOriginZ + (r+0.5)*kRowSpacing);
        }
        return out;
    }
    // Panjang penuh 1 gawangan (lebar block sepanjang baris pohon, sumbu X)
    static double gawanganFullLength(){ return (kGridCols-1)*kColSpacing + kColSpacing; }
    static double gateWorldZ() { return kGateZ; }

    // --- truk TPH->PKS: animasi visual, murni kosmetik (lihat catatan di
    //     TruckState, types.hpp) ---
    bool truckActive() const { return truck_.active; }
    // Avatar pemain (Gameplay Mode, third-person) -- lihat catatan lengkap
    // di PlayerAvatarState, types.hpp.
    const PlayerAvatarState& playerAvatar() const { return playerAvatar_; }
    // dirX/dirZ: arah gerak dari virtual joystick (TIDAK PERLU ternormalisasi
    // sebelumnya -- fungsi ini menormalisasi sendiri, supaya gerak diagonal
    // tak lebih cepat dari gerak lurus, standar praktik game). dt: delta
    // waktu (detik). Collision sliding sederhana thd pohon (lihat
    // implementasi lengkap di engine.cpp).
    // cameraYawOffset: sudut TAMBAHAN dari kamera touch-drag (fitur "lihat
    // sekeliling" independen dari arah gerak) -- ditambahkan ke facingRad
    // avatar SEBELUM transform camera-relative, spy "maju" di joystick
    // TETAP berarti "menuju arah yg terlihat di kamera SAAT INI" walau
    // pemain baru saja memutar pandangan manual via sentuh layar. Default
    // 0.0 (tak ada offset, kamera lurus di belakang avatar spt sebelumnya).
    // REDESAIN (poin #1 laporan pengguna: kamera harus kontrol terpisah dari
    // joystick, pola Roblox/third-person mobile). cameraYaw sekarang ORIENTASI
    // ABSOLUT kamera (dari touch-drag bebas), BUKAN lagi offset relatif thd
    // facingRad avatar -- lihat catatan lengkap di engine.cpp.
    void movePlayerAvatar(double dirX, double dirZ, double dt, double cameraYaw = 0.0);
    // Fase waktu harian -- computed dari fraksi dayTimer/dayLength (lihat
    // pembagian 40/35/25% & dasar literatur lengkap di comment TimeOfDay,
    // types.hpp).
    TimeOfDay timeOfDay() const {
        double frac = eco_.dayLength>0 ? eco_.dayTimer/eco_.dayLength : 0.0;
        if (frac < 0.40) return TimeOfDay::Pagi;
        if (frac < 0.75) return TimeOfDay::Siang;
        return TimeOfDay::Malam;
    }
    bool isRaining() const { return isRaining_; }
    // 0..1 -- dipakai renderer utk efek visual "silo lebih terang" saat
    // proses batch PKS baru terjadi (lihat drawPksBuilding()).
    float pksProcessPulse() const { return (float)(pksProcessPulseTimer_ / 2.0); }
    double truckProgress() const { return truck_.progress; }
    int truckBlockId() const { return truck_.blockId; }

    // --- log aktivitas: riwayat PERMANEN selama sesi (beda dgn event sink yg
    //     sekali poll lalu hilang) -- utk layar "Log Aktivitas" yg pemain bisa
    //     buka kapan saja dan melihat semua yg pernah terjadi. ---
    int activityLogCount() const { return (int)activityLog_.size(); }
    // Terbaru DULUAN (index 0 = paling baru), format "Hari X: teks".
    std::string activityLogEntry(int indexFromNewest) const;
    int activityLogTreeId(int indexFromNewest) const;

    // --- utilitas khusus testing/dev (BUKAN bagian gameplay normal — jangan dipanggil dari UI) ---
    void devAddMoney(double amount) { eco_.money += amount; }
    // Dev helper -- lompat langsung ke jenjang Manager/ADM tanpa rekrut
    // berjenjang panjang dr bawah, dibutuhkan test PKS (prasyarat bangunPks).
    void devSetManager(int n) { hr_.manager = n; }
    // Dev helper -- paksa status hujan langsung, dibutuhkan test deterministik
    // pembuktian efek perlambatan truk (isRaining_ privat, sulit dipaksa scr
    // alami tanpa menunggu probabilitas acak berputar ke kondisi yg tepat).
    void devSetRaining(bool raining) { isRaining_ = raining; }
    // Dev helper -- set krani langsung, bypass prasyarat (krani butuh
    // mandor>=1 dulu scr normal). Dibutuhkan test MURNI hrEfficiency() tanpa
    // efek samping auto-assign Mandor (yg akan mengacaukan pengukuran waktu
    // kerja worker[0] krn bisa dapat tugas tambahan otomatis begitu bebas).
    void devSetKrani(int n) { hr_.krani = n; }
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
    // BUG diperbaiki: kTphZ dulu 0.0, yg TERNYATA PERSIS SAMA dgn posisi
    // baris pohon r=6 (tengah grid 13 baris) -- bukan GAWANGAN (ruang kosong
    // antar baris)! TPH seharusnya berada di jalur bebas (gawangan HIDUP,
    // bukan gawangan MATI yg sudah ditumpuki pelepah tunas -- lihat
    // deadRowZOffsets()), bukan sejajar tepat dgn satu baris pohon tertentu.
    // Dilaporkan pengguna: "jalan truk pengangkut sawit seperti tidak
    // mengikuti realita". -2.253 = gawangan hidup antara baris 5-6 (indeks
    // ganjil), TERDEKAT dgn tengah grid dari 6 pilihan gawangan hidup yg
    // ada (formula: kGridOriginZ+(5+0.5)*kRowSpacing, dihitung manual krn
    // kTphZ dideklarasikan SEBELUM konstanta grid -- lihat kGridRows dst).
    static constexpr double kTphX = 32.0, kTphZ = -2.253;
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
    PlayerAvatarState playerAvatar_; // avatar pemain (Gameplay Mode, third-person -- lihat types.hpp)
    static constexpr double kTruckDurationSec = 3.5; // lama animasi truk keluar dari TPH
    double simAccum_ = 0;      // akumulator detik utk tick simulasi 1 Hz (pertumbuhan pohon dst)
    double tphAutoTimer_ = 26; // truk auto-jemput TPH tiap N detik
    // BUG diperbaiki: autoAssign_ dulu HANYA dipicu sbg efek samping
    // completeJob_() (worker MENYELESAIKAN tugas sebelumnya) -- worker yg
    // BARU DIREKRUT & BELUM PERNAH dapat tugas apa pun (idle sejak awal,
    // tak punya "tugas sebelumnya" utk memicu completeJob_) TAK PERNAH
    // ter-assign sama sekali, macet diam di posisi kantor selamanya. Timer
    // ini men-scan SEMUA worker idle scr PERIODIK (bukan cuma reaktif),
    // menjamin worker baru JUGA dapat tugas pertamanya otomatis.
    double autoAssignCheckTimer_ = 2.0;
    // Reminder pemupukan -- Corley & Tinker (2016) menekankan nutrisi kurang
    // BERTAHUN-TAHUN berdampak signifikan ke produktivitas jangka panjang
    // (bukan cuma musim ini). Flag ini cegah SPAM notifikasi tiap hari --
    // cuma sekali per EPISODE kondisi buruk (>25% pokok kekurangan hara),
    // reset begitu membaik, supaya bisa mengingatkan LAGI kalau memburuk
    // lagi di kemudian hari (bukan sekali seumur game lalu diam selamanya).
    bool fertilizerWarningActive_ = false;
    // Cuaca hujan -- probabilistik per hari (mirip pola Hama/Ganoderma),
    // berlangsung beberapa hari sekali muncul. Efek: truk lebih lambat
    // (jalan licin) -- Corley & Tinker (2016) §11.5: "load is lowered to
    // increase stability of the vehicle as the roads can become slippery"
    // saat cuaca buruk. Lihat catatan lengkap di onNewDay_(), engine.cpp.
    bool isRaining_ = false;
    int rainDaysLeft_ = 0;
    // Timer pulsa visual PKS -- aktif SESAAT (2 detik) setelah prosesBatchPks()
    // dipanggil, dipakai renderer utk indikasi visual jelas "sesuatu sedang
    // terjadi" (silo sedikit lebih terang). Mengatasi keluhan review: "tidak
    // ada tampilan saat proses PKS berlangsung" -- lihat drawPksBuilding(),
    // renderer_gl.cpp.
    double pksProcessPulseTimer_ = 0.0;
    // (autoMode_ dihapus -- digantikan pengecekan hr_.mandor>=1 langsung)
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
    // BUG diperbaiki: dulu TAK ADA pengecekan apakah suatu pohon SUDAH sedang
    // dikerjakan worker lain SEBELUM dispatch worker baru ke pohon yg SAMA --
    // bisa terjadi mis. pemain tap tombol aksi 2x sblm worker pertama sampai,
    // atau autoAssign_ menabrak worker manual (dilaporkan pengguna: "sistem
    // tak perlu memberi tugas sama ke pekerja idle kalau sudah di-assign").
    bool isTreeAssigned_(int treeId) const;
    // Bangun grid segitiga 143 pohon (pola SAMA dgn newGame()) di originX/Z
    // manapun, MENAMBAHKAN ke trees_ (bukan reset) -- dipakai newGame() UTK
    // Block A01 (origin 0,0) dan beliHa() utk block baru (origin berbeda).
    // outSoilFertility/outGeneticVigor: faktor ACAK-TAPI-TETAP yg dihasilkan
    // utk block ini (Corley & Tinker §9.2.3.5 & bab 6 -- lihat catatan Block,
    // types.hpp), dipakai CALLER menyimpan ke Block.soilFertility/geneticVigor.
    // Return [startIdx,endIdx) rentang trees_ yg baru ditambahkan.
    void generateBlockTrees_(double originX, double originZ, int& outStartIdx, int& outEndIdx,
                              double& outSoilFertility, double& outGeneticVigor);
    void autoAssign_(int workerIdx);
    void sellOrProcessTbs_(double amount, bool silent);

    const HrLevelDef* findLevel_(const std::string& key) const;
    bool prereqOk_(const HrLevelDef& def) const;
    bool maxOk_(const HrLevelDef& def) const;
    double costFor_(const HrLevelDef& def) const;
};

} // namespace sawit
