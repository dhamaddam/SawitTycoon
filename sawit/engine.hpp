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
    int totalPokok() const;
    double haPricePerUnit() const;

    // --- utilitas khusus testing/dev (BUKAN bagian gameplay normal — jangan dipanggil dari UI) ---
    void devAddMoney(double amount) { eco_.money += amount; }
    std::string saveToJson() const;
    bool loadFromJson(const std::string& json);

private:
    GameConfig cfg_;
    EconomyState eco_;
    LandState land_;
    HrState hr_;
    PksState pksState_;
    std::vector<Tree> trees_;

    struct WorkerJob {
        bool busy = false;
        std::string kind;   // tunas | panen | pupuk | pestisida | fungisida | tebang | angkut
        int treeId = -1;
        std::string phase;  // khusus angkut: "walk" -> "toTPH"
        double remaining = 0;
    };
    std::vector<WorkerJob> workers_;
    double simAccum_ = 0;      // akumulator detik utk tick simulasi 1 Hz (pertumbuhan pohon dst)
    double tphAutoTimer_ = 26; // truk auto-jemput TPH tiap N detik
    bool autoMode_ = true;     // pekerja tambahan (selain slot ke-1) otomatis cari kerjaan
    unsigned rngState_ = 0x9E3779B9u; // xorshift sederhana, tanpa <random> agar deterministik & ringan

    double randUnit_(); // 0..1

    EventSink events_;
    void emit(EventType t, const std::string& text = "", int treeId = -1);

    void onNewDay_();
    void simTick_(double dt);
    void updateWorkers_(double dt);
    void completeJob_(WorkerJob& job);
    int findFreeWorker_() const;
    void autoAssign_(int workerIdx);
    void sellOrProcessTbs_(double amount, bool silent);

    const HrLevelDef* findLevel_(const std::string& key) const;
    bool prereqOk_(const HrLevelDef& def) const;
    bool maxOk_(const HrLevelDef& def) const;
    double costFor_(const HrLevelDef& def) const;
};

} // namespace sawit
