#include "sawit/engine.hpp"
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace sawit {

static const char* kRoman[] = {"I","II","III","IV","V","VI","VII","VIII","IX","X"};
static std::string romanFor(int n){
    if (n>=1 && n<=10) return kRoman[n-1];
    return std::to_string(n);
}

static double taskDuration(const std::string& kind){
    if (kind=="tunas") return 2.2;
    if (kind=="panen") return 2.6;
    if (kind=="pupuk") return 1.8;
    if (kind=="pestisida") return 2.0;
    if (kind=="fungisida") return 3.2;
    if (kind=="tebang") return 2.6;
    if (kind=="angkut") return 2.6; // dipakai utk kedua fase (walk & toTPH), meniru parity versi web
    return 2.0;
}

double Engine::randUnit_(){
    // xorshift32 — cukup untuk variasi gameplay, deterministik jika di-seed sama
    rngState_ ^= rngState_ << 13;
    rngState_ ^= rngState_ >> 17;
    rngState_ ^= rngState_ << 5;
    return (rngState_ & 0xFFFFFF) / double(0xFFFFFF);
}

Engine::Engine(GameConfig cfg) : cfg_(std::move(cfg)) {
    newGame();
}

void Engine::loadConfig(const GameConfig& cfg){
    cfg_ = cfg; // live-ops: harga/SDM/PKS baru berlaku untuk aksi berikutnya, progres pemain tidak direset
}

void Engine::newGame(){
    eco_ = EconomyState{};
    eco_.money = cfg_.startMoney;
    eco_.day = 1;
    eco_.dayTimer = 0;
    eco_.dayLength = cfg_.dayLengthSeconds;
    eco_.tphCap = cfg_.tphCapStart;
    eco_.pricePerTandan = cfg_.pricePerTandan;
    eco_.moraleMultiplier = 1.0;

    land_ = LandState{};
    land_.sampleBlockHa = 143.0 / std::max(1, cfg_.pokokPerHa); // blok inti = 143 pokok (lihat layout di bawah)
    land_.afdelings = { Afdeling{0, land_.sampleBlockHa, "Afdeling I"} };

    hr_ = HrState{};
    workers_.clear();
    workers_.push_back(WorkerJob{}); // 1 buruh awal, meniru buildWorkers() di versi web
    workers_.back().x = kOfficeX; workers_.back().z = kOfficeZ; // mulai dari kantor (basis kerja, konsisten dgn pekerja rekrutan baru)
    hr_.buruh = 1;

    // Avatar pemain (Gameplay Mode) mulai dari KANTOR -- BUG signifikan
    // diperbaiki (dilaporkan pengguna: "joystick digerakkan tapi avatar
    // tidak mau bergerak"). Spawn default (0,0) TERNYATA sangat dekat
    // (0.168 unit) dgn sebuah pohon (posisi pohon py sedikit jitter acak
    // dari grid sempurna) -- terverifikasi numerik: dgn langkah SEKECIL
    // frame rate nyata game (~0.016-0.033 detik/frame, BUKAN 0.3 spt test
    // awal yg kebetulan "melompati" zona bahaya dlm 1 langkah), avatar
    // SAMA SEKALI tak bisa bergerak krn tiap langkah kecil tetap berakhir
    // dlm radius collision pohon yg sangat dekat itu. Konsisten dgn pola
    // pekerja BARU yg sudah ada (baris di atas: mulai dari kantor, basis
    // kerja) -- avatar jg masuk akal scr naratif mulai dari kantor, BUKAN
    // muncul tiba-tiba di tengah barisan pohon.
    playerAvatar_ = PlayerAvatarState{};
    // Offset kecil dari kOfficeX/Z -- BUG diperbaiki: pekerja PERTAMA
    // (baris di atas) SUDAH spawn TEPAT di (kOfficeX,kOfficeZ), kalau
    // avatar spawn di titik SAMA PERSIS, collision thd worker (baru
    // ditambahkan utk perbaikan bug "2 avatar tumpang tindih") justru
    // menjebak avatar lagi di lokasi baru ini -- ironis, memperbaiki 1 bug
    // spawn malah kena bug spawn lain. +2 unit di X cukup jauh dari radius
    // collision worker (0.8) tanpa keluar area dekat kantor scr naratif.
    playerAvatar_.x = kOfficeX + 2.0; playerAvatar_.z = kOfficeZ;

    pksState_ = PksState{};
    pksState_.buildCost = cfg_.pksBuildCost;
    pksState_.upgradeCost = cfg_.pksUpgradeBaseCost;
    pksState_.oer = cfg_.oerStart;
    pksState_.kerRate = cfg_.kerRate;
    pksState_.avgTandanKg = cfg_.avgTandanKg;
    pksState_.cpoPrice = cfg_.cpoPrice;
    pksState_.pkPrice = cfg_.pkPrice;

    trees_.clear();
    // 143 pokok, pola SEGITIGA SAMA SISI (mata lima) -- BUKAN grid kotak lurus,
    // di origin (0,0) -- lihat generateBlockTrees_() utk detail pola & sitasi.
    int b0Start, b0End;
    double b0SoilFertility, b0GeneticVigor;
    generateBlockTrees_(0.0, 0.0, b0Start, b0End, b0SoilFertility, b0GeneticVigor);

    // Kebun secara resmi jadi "Block A01" di bawah "Afdeling I" -- fondasi
    // hierarki Kebun>Afdeling>Block>Baris>Pokok. Block mereferensikan RENTANG
    // indeks di trees_ (bukan salinan data), spy tak ada duplikasi/drift.
    // "Block terdiri dari banyak baris tanaman + infrastruktur lokal, BUKAN
    // 1 baris = 1 block" -- keputusan desain, ukuran (ha) bukan klaim dari
    // Corley & Tinker (buku tak menetapkan 1 ukuran block universal).
    blocks_.clear();
    blocks_.push_back(Block{0, "A01", 0, land_.sampleBlockHa, b0Start, b0End, 0.0, 0.0, b0SoilFertility, b0GeneticVigor});
    blocks_.back().tphX = kTphX; blocks_.back().tphZ = kTphZ; // TPH block A01 -- posisi SAMA persis spt versi global lama

    simAccum_ = 0;
    tphAutoTimer_ = 26;
    activityLog_.clear();
}

void Engine::generateBlockTrees_(double originX, double originZ, int& outStartIdx, int& outEndIdx,
                                   double& outSoilFertility, double& outGeneticVigor){
    outStartIdx = (int)trees_.size();
    int id = outStartIdx;
    // Faktor pembeda antar-block, ACAK-TAPI-TETAP per block (lihat catatan
    // lengkap di types.hpp, Block): kesuburan tanah (Corley & Tinker §9.2.3.5,
    // survei tanah SEBELUM pembukaan lahan menemukan variasi alami) & vigor
    // genetik/asal benih D×P (bab 6 -- produsen benih beda punya potensi
    // hasil & vigor pertumbuhan beda, BUKAN satu genetik seragam sekebun).
    outSoilFertility = 0.70 + randUnit_()*0.60; // ~0.70 (marjinal) .. 1.30 (sangat subur)
    outGeneticVigor  = 0.85 + randUnit_()*0.30; // ~0.85 (benih standar) .. 1.15 (benih unggul)
    // Formula LAMA (0.45x) pusatnya terlalu dekat ambang "kekurangan hara"
    // (0.4) -- ketahuan via test: block RATA-RATA (faktor=1.0x1.0) sampai
    // ~50% pohonnya default defisien sejak hari 1, tak realistis utk kondisi
    // awal normal. Sekarang dipusatkan ke ~0.70 (setara baseline SEBELUM ada
    // diferensiasi block) sambil tetap melebar sesuai kesuburan+genetik --
    // block TERBAIK jelas lebih subur, block TERBURUK jelas lbh kekurangan,
    // tapi block RATA-RATA tetap sehat scr default (bukan separuh defisien).
    double nutritionBase = std::max(0.15, std::min(0.92, 0.72 * outSoilFertility * outGeneticVigor - 0.02));

    // 143 pokok, pola SEGITIGA SAMA SISI (mata lima) -- BUKAN grid kotak lurus.
    // Sesuai SOP jarak tanam 9x9x9m utk populasi 143 pokok/Ha: jarak antar
    // tanaman 9m, jarak antar BARIS 7,8m (rasio 7,8/9=0,867 = sin(60 derajat),
    // krn segitiga sama sisi). Baris genap digeser setengah spacing horizontal
    // -> pola berselang-seling (mata lima/hexagonal), bukan baris lurus sejajar.
    // originX/originZ menggeser SELURUH grid (bukan tiap pohon independen) --
    // dipakai beliHa() menempatkan block baru di area terpisah dr block lain.
    for (int r=0; r<kGridRows; ++r){
        double rowOffset = (r % 2 == 1) ? kColSpacing*0.5 : 0.0; // selang-seling -> pola segitiga
        for (int c=0; c<kGridCols; ++c){
            Tree t;
            t.id = id++;
            t.x = originX + kGridOriginX + c*kColSpacing + rowOffset + (randUnit_()-0.5)*0.4;
            t.z = originZ + kGridOriginZ + r*kRowSpacing + (randUnit_()-0.5)*0.4;
            t.ageYears = 2.0 + randUnit_()*10.0;
            t.frond = 0.15 + randUnit_()*0.3;
            t.ffb = FfbState::Growing;
            t.ffbTimer = 10.0 + randUnit_()*20.0;
            t.ffbTimerMax = t.ffbTimer; // fondasi countdown/progress bar -- lihat catatan lengkap di types.hpp
            t.health = HealthState::Sehat;
            t.sickTimer = 0;
            // Nutrisi dasar digerakkan oleh kesuburan tanah x vigor genetik
            // block ini (bukan lagi rentang seragam 0.6-0.9 sama persis di
            // SEMUA block) -- jitter per-pohon ringan (+/-0.15) di atasnya
            // spy tetap ada variasi individual dlm satu block.
            t.nutrition = std::max(0.0, std::min(1.0, nutritionBase + (randUnit_()-0.5)*0.30));
            t.hasTbsReady = false;
            trees_.push_back(t);
        }
    }
    outEndIdx = (int)trees_.size();
}

void Engine::emit(EventType t, const std::string& text, int treeId){
    if (events_) events_(EngineEvent{t, text, treeId});
    // Catat ke log PERMANEN utk event yg bermakna sbg "aktivitas" (Toast,
    // FlyMoney, & LogOnly) -- TreeChanged/HudChanged/ScreenChanged cuma
    // sinyal teknis "perlu refresh UI", bukan sesuatu yg relevan ditampilkan
    // sbg riwayat aktivitas. LogOnly ditambahkan (lihat definisi EventType)
    // -- masuk log SAMA spt Toast, tapi platform layer TAK menampilkannya
    // sbg Toast visual (lihat nativePollEvents/startUiPollingLoop).
    if (!text.empty() && (t == EventType::Toast || t == EventType::FlyMoney || t == EventType::LogOnly)) {
        activityLog_.push_back(LogEntry{eco_.day, text, treeId});
        if ((int)activityLog_.size() > kActivityLogMax) {
            activityLog_.erase(activityLog_.begin()); // buang yg tertua
        }
    }
}

std::string Engine::activityLogEntry(int indexFromNewest) const {
    int n = (int)activityLog_.size();
    if (indexFromNewest < 0 || indexFromNewest >= n) return "";
    const LogEntry& e = activityLog_[n - 1 - indexFromNewest]; // index 0 = paling baru
    return "Hari " + std::to_string(e.day) + ": " + e.text;
}
// treeId terkait 1 entri log (-1 kalau tak terkait pohon tertentu) -- fondasi
// fitur "ketuk log utk lompat ke pohon" (celah diperbaiki: data ini sudah
// lama ada di emit() tp dibuang, tak pernah tersimpan sampai sekarang).
int Engine::activityLogTreeId(int indexFromNewest) const {
    int n = (int)activityLog_.size();
    if (indexFromNewest < 0 || indexFromNewest >= n) return -1;
    return activityLog_[n - 1 - indexFromNewest].treeId;
}

std::vector<WorkerRenderInfo> Engine::workersRenderInfo() const {
    std::vector<WorkerRenderInfo> out;
    out.reserve(workers_.size());
    for (auto& w : workers_) {
        WorkerRenderInfo info;
        info.x = w.x; info.z = w.z; info.busy = w.busy; info.carrying = w.carrying;
        // Arah hadap dari vektor start->target job (KONSTAN sepanjang 1 fase,
        // mewakili arah gerakan keseluruhan tanpa perlu state tambahan per-
        // frame) -- BUG diperbaiki: dulu tak dihitung sama sekali (konstan 0
        // di JNI/EngineBridge), pekerja & truk selalu menghadap arah dunia
        // tetap, tak peduli arah gerakan sebenarnya/orientasi baris tanam.
        // Guard div-by-zero/NaN kalau start==target persis (jarak sangat kecil).
        if (w.busy){
            double dx = w.targetX - w.startX, dz = w.targetZ - w.startZ;
            if (dx*dx + dz*dz > 1e-6) info.facingRad = std::atan2(dz, dx);
        }
        // Pose dipetakan dari jenis tugas -- meniru 4 pose referensi ilustrasi:
        // jongkok memungut (pupuk), pakai alat (tunas/pestisida/fungisida/tebang),
        // meraih ke atas (panen), membawa keranjang (angkut fase bawa ke TPH).
        if (w.carrying) info.pose = WorkerPose::Carry;
        else if (!w.busy) info.pose = WorkerPose::Idle;
        else if (w.kind=="panen") {
            info.pose = WorkerPose::Reach;
            const Tree* t = const_cast<Engine*>(this)->treeById(w.treeId);
            info.usingEgrek = t ? t->isMature(cfg_) : false; // egrek utk pokok tinggi, dodos utk pokok muda
        }
        else if (w.kind=="pupuk") info.pose = WorkerPose::Kneel;
        else if (w.kind=="tunas" || w.kind=="pestisida" || w.kind=="fungisida" || w.kind=="tebang") info.pose = WorkerPose::Tool;
        else info.pose = WorkerPose::Idle; // angkut fase "walk" (blm bawa) -> jalan biasa
        out.push_back(info);
    }
    return out;
}

Tree* Engine::treeById(int id){
    for (auto& t : trees_) if (t.id==id) return &t;
    return nullptr;
}

int Engine::nearestTreeToPlayer(double maxDist) const {
    int bestId = -1;
    double bestDistSq = maxDist*maxDist;
    for (const auto& t : trees_){
        if (t.health == HealthState::Mati) continue; // pohon tumbang/ditebang -- tak bisa diinteraksi
        double dx = t.x - playerAvatar_.x, dz = t.z - playerAvatar_.z;
        double distSq = dx*dx + dz*dz;
        if (distSq < bestDistSq){ bestDistSq = distSq; bestId = t.id; }
    }
    return bestId;
}

double Engine::cameraSafeDistance(double playerX, double playerZ, double facingRad, double desiredDist) const {
    // Arah dari avatar MENUJU kamera = BELAKANG avatar (berlawanan facingRad)
    // -- PERSIS formula idealEyeX/Z di updateThirdPersonCamera() (renderer_gl.cpp),
    // harus konsisten supaya raycast ini benar2 mewakili garis pandang kamera.
    double dirX = -std::cos(facingRad), dirZ = -std::sin(facingRad);
    const double kTreeCollisionRadius = 1.3; // sama dgn radius collision avatar (movePlayerAvatar)
    double safeDist = desiredDist;
    for (const auto& t : trees_){
        if (t.health == HealthState::Mati) continue; // pohon tumbang -- bukan penghalang
        double toTreeX = t.x - playerX, toTreeZ = t.z - playerZ;
        double proj = toTreeX*dirX + toTreeZ*dirZ; // jarak sepanjang garis pandang
        if (proj < 0.0 || proj > desiredDist) continue; // pohon TAK di antara avatar & kamera
        double perpX = toTreeX - proj*dirX, perpZ = toTreeZ - proj*dirZ;
        double perpDistSq = perpX*perpX + perpZ*perpZ;
        if (perpDistSq < kTreeCollisionRadius*kTreeCollisionRadius){
            // Pohon menghalangi -- kamera berhenti TEPAT di tepi radius
            // pohon (bukan di pusatnya), spy kamera tak "menembus" batang.
            double clampedDist = proj - std::sqrt(std::max(0.0, kTreeCollisionRadius*kTreeCollisionRadius - perpDistSq));
            clampedDist = std::max(0.8, clampedDist); // jarak minimum wajar -- kamera jgn sampai NEMPEL avatar
            if (clampedDist < safeDist) safeDist = clampedDist;
        }
    }
    return safeDist;
}

double Engine::totalHa() const {
    double s=0; for (auto& a : land_.afdelings) s += a.ha; return s;
}
int Engine::totalPokok() const { return (int)std::llround(totalHa()*cfg_.pokokPerHa); }
double Engine::haPricePerUnit() const { return std::round(cfg_.haBasePrice * std::pow(cfg_.haPriceGrowth, totalHa())); }

std::vector<BlockSummary> Engine::blockSummaries() const {
    std::vector<BlockSummary> out;
    out.reserve(blocks_.size());
    for (const auto& b : blocks_) {
        BlockSummary s;
        s.id = b.id; s.name = b.name; s.ha = b.ha;
        s.originX = b.originX; s.originZ = b.originZ;
        s.soilFertility = b.soilFertility; s.geneticVigor = b.geneticVigor;
        s.tphStock = b.tphStock; s.tphStockOverripe = b.tphStockOverripe;
        s.tphX = b.tphX; s.tphZ = b.tphZ;
        for (int i = b.treeStartIdx; i < b.treeEndIdx && i < (int)trees_.size(); ++i) {
            const Tree& t = trees_[i];
            s.treeCount++;
            switch (t.health) {
                case HealthState::Sehat: s.healthyCount++; break;
                case HealthState::Hama: s.hamaCount++; break;
                case HealthState::Ganoderma: s.ganodermaCount++; break;
                case HealthState::Mati: s.deadCount++; break;
            }
            if ((t.ffb==FfbState::Ripe || t.ffb==FfbState::Overripe) && t.health!=HealthState::Mati) s.readyToHarvestCount++;
            if (t.hasTbsReady) s.tbsAwaitingPickupCount++;
            if (t.nutrition < 0.4) s.lowNutritionCount++;
        }
        out.push_back(s);
    }
    return out;
}

int Engine::blockIdForTree(int treeId) const {
    for (const auto& b : blocks_) {
        if (treeId >= b.treeStartIdx && treeId < b.treeEndIdx) return b.id;
    }
    return -1;
}

void Engine::devRandomizeConditions(){
    for (auto& t : trees_){
        double r = randUnit_();
        if (r < 0.08) t.health = HealthState::Ganoderma;
        else if (r < 0.20) t.health = HealthState::Hama;
        else if (r < 0.23) t.health = HealthState::Mati;
        else t.health = HealthState::Sehat;
        t.nutrition = randUnit_(); // 0..1 penuh (bukan cuma 0.6-0.9 spt newGame()) spy ekstrem jg kelihatan
        if (t.health != HealthState::Mati){
            double rf = randUnit_();
            if (rf < 0.25) t.ffb = FfbState::None;
            else if (rf < 0.55) t.ffb = FfbState::Growing;
            else if (rf < 0.85) t.ffb = FfbState::Ripe;
            else t.ffb = FfbState::Overripe;
        } else {
            t.ffb = FfbState::None;
        }
        emit(EventType::TreeChanged, "", t.id);
    }
    emit(EventType::HudChanged);
    emit(EventType::Toast, "Kondisi kebun diacak (mode uji visual)");
}

// ---------------------------------------------------------------------------
// GAME LOOP
// ---------------------------------------------------------------------------
void Engine::tick(double dt){
    updateWorkers_(dt);

    if (truck_.active){
        // BUG signifikan diperbaiki (dilaporkan pengguna: "jalan truk
        // pengangkut sawit seperti tidak mengikuti realita"): durasi
        // animasi dulu KONSTAN (kTruckDurationSec) terlepas dari SEBERAPA
        // JAUH block dari gerbang/PKS -- terverifikasi numerik: kecepatan
        // visual truk melonjak dari 9.4 unit/detik (block pertama) sampai
        // 93.4 unit/detik (block ke-4, HAMPIR 10x LEBIH CEPAT), krn block
        // lebih jauh (dari beliHa()) py jarak tempuh jauh lbh besar tapi
        // durasi animasi TETAP SAMA. Sekarang durasi diskalakan PROPORSIONAL
        // thd jarak sesungguhnya -- block pertama (baseline referenceDist=33
        // unit, jarak asli kTphX->kGateX+10) TETAP dpt durasi kTruckDurationSec
        // persis spt sebelumnya (tak mengubah pengalaman yg sudah ada), block
        // lain otomatis dpt durasi proporsional lbh lama, menjaga KECEPATAN
        // VISUAL konsisten di semua block.
        double jarakTempuh = 33.0; // fallback -- baseline block pertama
        if (truck_.blockId >= 0 && truck_.blockId < (int)blocks_.size()){
            double startX = blocks_[truck_.blockId].tphX;
            double exitX = kGateX + 10.0;
            jarakTempuh = std::abs(exitX - startX);
        }
        const double kReferenceDistance = 33.0; // jarak block pertama (kTphX=32 -> kGateX+10=65)
        double distanceFactor = jarakTempuh / kReferenceDistance;
        // Hujan bikin jalan licin -- truk lebih lambat (durasi animasi 1.6x
        // lebih lama saat hujan). Corley & Tinker (2016) §11.5: muatan
        // dikurangi & kecepatan diturunkan demi stabilitas kendaraan saat
        // cuaca buruk.
        double effectiveDuration = kTruckDurationSec * distanceFactor * (isRaining_ ? 1.6 : 1.0);
        truck_.progress += dt / effectiveDuration;
        if (truck_.progress >= 1.0){ truck_.active = false; truck_.progress = 0.0; }
    }
    if (pksProcessPulseTimer_ > 0.0) pksProcessPulseTimer_ = std::max(0.0, pksProcessPulseTimer_ - dt);

    eco_.dayTimer += dt;
    if (eco_.dayTimer >= eco_.dayLength){
        eco_.dayTimer = 0;
        eco_.day++;
        onNewDay_();
        emit(EventType::HudChanged);
    }

    // Pengecekan auto-assign PERIODIK (bukan cuma reaktif dr completeJob_) --
    // lihat catatan lengkap di autoAssignCheckTimer_, engine.hpp. Interval
    // 2 detik cukup responsif utk pemain (tak terasa "macet lama"), tapi tak
    // membebani CPU scan tiap frame.
    autoAssignCheckTimer_ -= dt;
    if (autoAssignCheckTimer_ <= 0){
        autoAssignCheckTimer_ = 2.0;
        if (hr_.mandor >= 1){
            for (size_t i=1;i<workers_.size();++i) if (!workers_[i].busy){ autoAssign_((int)i); }
        }
    }

    simAccum_ += dt;
    while (simAccum_ >= 1.0){
        simAccum_ -= 1.0;
        simTick_(1.0);
    }

    tphAutoTimer_ -= dt;
    if (tphAutoTimer_ <= 0){
        tphAutoTimer_ = 26;
        // Iterasi SEMUA block -- tiap block dgn stok tertunda dikirim truknya
        // sendiri (celah diperbaiki: dulu cuma cek 1 stok global). Kalau lbh
        // dari 1 block ada stok bersamaan, truck_ (state animasi TUNGGAL)
        // akan menampilkan yg TERAKHIR diproses -- transaksi uang tiap block
        // tetap benar & lengkap, cuma animasi visualnya tak bisa tampilkan
        // semua truk sekaligus (penyederhanaan yg disengaja).
        for (size_t i=0; i<blocks_.size(); ++i){
            if (blocks_[i].tphStock > 0 || blocks_[i].tphStockOverripe > 0) kirimTruk((int)i);
        }
    }
}

void Engine::simTick_(double dt){
    (void)dt; // dipanggil selalu dengan 1.0 dari tick(), lihat while(simAccum_>=1.0) di atas
    for (auto& t : trees_){
        if (t.health == HealthState::Mati) continue;

        // Faktor perlambatan pematangan TBS akibat kondisi kesehatan --
        // literatur: infeksi hama/penyakit mengurangi hasil TBS ~50-80%
        // (Corley & Tinker 2015, via Woittiez et al. 2017; jg dikonfirmasi
        // multi-sumber independen: Ganoderma "reduce yields by as much as
        // 80 percent" sblm pohon mati -- Asian Agri). BUKAN berhenti total
        // (pohon sakit MASIH bisa berbuah, cuma jauh lebih lambat) -- sesuai
        // laporan pengguna: opsi "dikurangi kecepatan berbuahnya" dipilih
        // drpd "tidak berbuah sama sekali" krn lebih akurat scr ilmiah.
        // Ganoderma (basal stem rot, jauh lebih parah/mematikan) dapat
        // penalti lebih besar drpd Hama biasa (rentang atas vs tengah
        // rentang literatur 50-80%).
        double ffbRateFactor = 1.0;
        if (t.health == HealthState::Hama) ffbRateFactor = 0.5;        // -50%
        else if (t.health == HealthState::Ganoderma) ffbRateFactor = 0.2; // -80%

        // pematangan TBS
        if (t.ffb == FfbState::None){
            t.ffbTimer -= 1.0*ffbRateFactor;
            if (t.ffbTimer <= 0){ t.ffb = FfbState::Growing; t.ffbTimer = 10 + randUnit_()*8*(1.3-t.nutrition); t.ffbTimerMax = t.ffbTimer; emit(EventType::TreeChanged,"",t.id); }
        } else if (t.ffb == FfbState::Growing){
            t.ffbTimer -= 1.0*ffbRateFactor;
            if (t.ffbTimer <= 0){ t.ffb = FfbState::Ripe; t.ffbTimer = 12 + randUnit_()*10; t.ffbTimerMax = t.ffbTimer; emit(EventType::TreeChanged,"",t.id); }
        } else if (t.ffb == FfbState::Ripe){
            t.ffbTimer -= 1.0*ffbRateFactor;
            if (t.ffbTimer <= 0){ t.ffb = FfbState::Overripe; emit(EventType::TreeChanged,"",t.id); }
        }

        // pelepah makin menumpuk
        t.frond = std::min(1.0, t.frond + 0.006);
        // kesuburan menurun perlahan
        t.nutrition = std::max(0.0, t.nutrition - 0.002);

        // hama & Ganoderma — DISKALAKAN ULANG dari literatur (lihat kutipan di
        // bawah), BUKAN angka sembarangan. Sebelum perbaikan ini, konstanta
        // wabah TIDAK ikut diturunkan saat kebun diperbesar dari 20 -> 143
        // pokok, sehingga wabah baru muncul ~6.6x/hari (hama) & ~2.2x/hari
        // (Ganoderma) di SELURUH kebun -- jauh lebih intens dari kenyataan &
        // salah satu penyebab pemain "kehabisan uang" krn terus-terusan beli
        // pestisida/fungisida.
        //
        // Target baru (di 143 pokok): ~1 kasus hama baru/2.7 hari, ~1 kasus
        // Ganoderma baru/12 hari -- cocok dgn sifat "spotted/mengelompok,
        // bukan serentak" pd kejadian BPB di lapangan (Balai Besar Perbenihan
        // & Pelindungan Tanaman Perkebunan Medan, "Kejadian penyakit BPB
        // terdapat pada areal-areal tertentu dan cenderung mengelompok").
        //
        // CATATAN: pematangan TBS di atas SENGAJA TIDAK ikut diperlambat
        // meski literatur rotasi panen sesungguhnya 7-10 hari (Arvis, Gokomodo,
        // dll) -- itu satu-satunya sumber pemasukan pemain, jadi mempercepat
        // wabah TAPI memperlambat panen sekaligus akan membuat game makin
        // tidak bisa dimainkan, bukan makin natural. Ini keputusan desain
        // sadar: akurasi wabah + generositas panen, demi "tetap menghibur".
        if (t.health == HealthState::Sehat){
            double pestChance = 0.00005 * (1.4 - t.nutrition);
            double ganoChance = 0.000013 * (1.3 - t.nutrition);
            double r = randUnit_();
            if (r < pestChance){ t.health = HealthState::Hama; emit(EventType::TreeChanged,"",t.id); emit(EventType::Toast, "Hama menyerang pokok #"+std::to_string(t.id)); }
            else if (r < pestChance+ganoChance){ t.health = HealthState::Ganoderma; t.sickTimer=0; emit(EventType::TreeChanged,"",t.id); emit(EventType::Toast, "Gejala Ganoderma di pokok #"+std::to_string(t.id)+"!"); }
        } else if (t.health == HealthState::Ganoderma){
            t.sickTimer += 1.0;
            // Literatur (Balai Medan): gejala BPB baru TERLIHAT 6-12 BULAN
            // setelah infeksi, & siklus penyakit penuh 1-3 TAHUN sampai pokok
            // benar2 mati. Sebelumnya ambang di sini cuma 55 detik (<1 hari
            // game!) -- pemain nyaris tak sempat bereaksi. Sekarang ~18 hari
            // game (1350 detik) -- masih jauh dipercepat dari kenyataan
            // (kompresi waktu memang perlu utk game), tapi memberi pemain
            // waktu nyata utk menyadari & mengobati sebelum pokok mati.
            if (t.sickTimer > 1350){ t.health = HealthState::Mati; emit(EventType::TreeChanged,"",t.id); emit(EventType::Toast, "Pokok #"+std::to_string(t.id)+" mati akibat busuk pangkal batang"); }
        }
    }
    emit(EventType::HudChanged);
}

void Engine::onNewDay_(){
    double salary = totalDailySalary();
    double paid = std::min(salary, eco_.money);
    eco_.money -= paid;
    eco_.moraleMultiplier = (paid >= salary) ? 1.0 : 0.55;
    if (eco_.moraleMultiplier < 1.0) emit(EventType::Toast, "Gaji tidak terbayar penuh -- efisiensi afdeling menurun sementara");

    double abstractHa = std::max(0.0, totalHa() - land_.sampleBlockHa);
    if (abstractHa > 0){
        double pokok = abstractHa * cfg_.pokokPerHa;
        double tandan = std::round(pokok * cfg_.tandanPerPokokPerHari * hrEfficiency() * eco_.moraleMultiplier);
        sellOrProcessTbs_(tandan, true);
    }
    emit(EventType::ScreenChanged);

    // Reminder pemupukan -- laporan pengguna: "pemupukan harus diingatkan,
    // karena ini akan jadi produktivitas beberapa tahun ke depan". Dasar
    // literatur: Corley & Tinker (2016) "The Oil Palm" 5th ed. menekankan
    // defisiensi nutrisi (terutama N, K, Mg) yg dibiarkan BERTAHUN-TAHUN
    // berdampak KUMULATIF & SIGNIFIKAN ke produktivitas jangka panjang --
    // BUKAN sekadar penurunan musim ini saja, karena palem butuh cadangan
    // nutrisi utk pembentukan tandan yg baru terlihat 18-24 bulan kemudian
    // (lag time diferensiasi bunga). Ambang 25% pokok kekurangan hara (dari
    // proxy nutrition<0.4 yg sudah dipakai lowNutritionCount) dipilih spy
    // reminder muncul SEBELUM kondisi jadi kritis di seluruh kebun, tapi
    // tak terlalu sensitif shg mengganggu di kondisi wajar (bbrp pokok
    // kekurangan hara itu NORMAL, bukan tanda darurat).
    {
        int totalTrees = 0, lowNutritionTrees = 0;
        for (const auto& t : trees_){
            if (t.health == HealthState::Mati) continue; // pokok mati tak relevan dipupuk
            totalTrees++;
            if (t.nutrition < 0.4) lowNutritionTrees++;
        }
        double fraction = totalTrees>0 ? (double)lowNutritionTrees/totalTrees : 0.0;
        if (fraction > 0.25 && !fertilizerWarningActive_){
            fertilizerWarningActive_ = true;
            emit(EventType::Toast, "⚠️ Pengingat: "+std::to_string(lowNutritionTrees)+" dari "+std::to_string(totalTrees)+
                " pokok kekurangan hara. Segera pupuk -- nutrisi kurang berdampak ke produktivitas beberapa TAHUN ke depan, bukan cuma musim ini.");
        } else if (fraction <= 0.15 && fertilizerWarningActive_){
            fertilizerWarningActive_ = false; // membaik -- boleh diingatkan lagi kalau memburuk lagi nanti
        }
    }

    // Cuaca hujan -- probabilistik per hari (mirip pola desain Hama/
    // Ganoderma). Sumatra rata2 10-18 hari hujan/bulan dari 30 hari, puncak
    // November 18 hari (worlddata.info, "Climate: Sumatra in Indonesia") --
    // probabilitas ~35% per hari dipilih mendekati RATA-RATA WAJAR (bukan
    // puncak musim penghujan). Durasi 1-2 hari per episode (bukan hujan
    // konstan sepanjang game, ATAU cuma 1 tick lalu hilang tak berkesan).
    if (rainDaysLeft_ > 0){
        rainDaysLeft_--;
        if (rainDaysLeft_ == 0){ isRaining_ = false; emit(EventType::Toast, "☀️ Cuaca cerah kembali"); }
    } else if (randUnit_() < 0.35){
        isRaining_ = true;
        rainDaysLeft_ = 1 + (int)(randUnit_()*2); // 1-2 hari
        emit(EventType::Toast, "🌧️ Hujan turun -- jalan licin, truk lebih lambat mengangkut TBS ke PKS");
    }
}

// ---------------------------------------------------------------------------
// WORKER JOB ENGINE
// ---------------------------------------------------------------------------
int Engine::findFreeWorker_() const {
    for (size_t i=0;i<workers_.size();++i) if (!workers_[i].busy) return (int)i;
    return -1;
}

bool Engine::isTreeAssigned_(int treeId) const {
    for (const auto& w : workers_) if (w.busy && w.treeId == treeId) return true;
    return false;
}

void Engine::updateWorkers_(double dt){
    for (auto& w : workers_){
        if (!w.busy) continue;
        w.remaining -= dt;
        // Posisi berjalan menuju target: berjalan selama w.walkDuration detik
        // PERTAMA (dihitung dari jarak sungguhan / kecepatan, lihat startJob_
        // & kWorkerWalkSpeed) — bukan pecahan tetap dari total durasi lagi
        // (itu penyebab lama pekerja terasa nyaris tak bergerak: jendela jalan
        // dulu bisa <1 detik apapun jaraknya). Setelah walkDuration terlewati,
        // pekerja "bekerja di tempat" sampai job selesai.
        double elapsed = w.totalDuration - w.remaining;
        double walkT = (w.walkDuration > 0) ? std::min(1.0, std::max(0.0, elapsed / w.walkDuration)) : 1.0;

        // Jalan MENYUSURI KORIDOR (3 segmen: start->wp1->wp2->target), BUKAN
        // garis lurus diagonal yg bisa "menembus" kanopi pohon lain (lihat
        // computeCorridorPath_ & catatan literatur di WorkerJob, engine.hpp).
        double d1x=w.wp1X-w.startX, d1z=w.wp1Z-w.startZ, seg1 = std::sqrt(d1x*d1x+d1z*d1z);
        double d2x=w.wp2X-w.wp1X,   d2z=w.wp2Z-w.wp1Z,   seg2 = std::sqrt(d2x*d2x+d2z*d2z);
        double d3x=w.targetX-w.wp2X, d3z=w.targetZ-w.wp2Z, seg3 = std::sqrt(d3x*d3x+d3z*d3z);
        double totalSeg = seg1+seg2+seg3;
        double travelled = walkT * totalSeg;
        if (totalSeg < 1e-6){
            w.x = w.targetX; w.z = w.targetZ;
        } else if (travelled <= seg1){
            double t = (seg1>1e-6) ? travelled/seg1 : 1.0;
            w.x = w.startX + d1x*t; w.z = w.startZ + d1z*t;
        } else if (travelled <= seg1+seg2){
            double t = (seg2>1e-6) ? (travelled-seg1)/seg2 : 1.0;
            w.x = w.wp1X + d2x*t; w.z = w.wp1Z + d2z*t;
        } else {
            double t = (seg3>1e-6) ? (travelled-seg1-seg2)/seg3 : 1.0;
            w.x = w.wp2X + d3x*t; w.z = w.wp2Z + d3z*t;
        }
        if (w.remaining <= 0) completeJob_(w);
    }
}

// Hitung 2 waypoint jalur koridor -- literatur: Corley & Tinker (2016)
// "harvesting paths" antar baris tanam (jg dipakai analisis kompaksi tanah
// gambut, §9); SOP "pasar pikul" (jalur panen antar baris). Pekerja jalan
// SEJAJAR SUMBU (menyusuri gawangan/koridor antar baris) alih-alih memotong
// diagonal yg bisa terlihat "menembus" kanopi pohon-pohon lain di antaranya.
void Engine::computeCorridorPath_(double startX, double startZ, double targetX, double targetZ, int targetTreeId,
                                    double* wp1X, double* wp1Z, double* wp2X, double* wp2Z) const {
    // Cari originZ BLOCK yg memuat targetTreeId (bisa beda dari kGridOriginZ
    // kalau target ada di block baru hasil beliHa()). Fallback ke
    // kGridOriginZ (Block A01) kalau target bukan pohon (mis. TPH/kantor).
    double originZ = kGridOriginZ;
    for (const auto& b : blocks_){
        if (targetTreeId >= b.treeStartIdx && targetTreeId < b.treeEndIdx){ originZ = b.originZ + kGridOriginZ; break; }
    }
    // baris terdekat dgn TARGET (formula SAMA dgn generateBlockTrees_ -- originZ/kRowSpacing)
    int row = (int)std::round((targetZ - originZ) / kRowSpacing);
    row = std::max(0, std::min(kGridRows-1, row));
    // pilih koridor (celah SEBELUM atau SESUDAH baris tsb) yg lebih dekat ke
    // posisi AWAL pekerja -- spy segmen pertama (start->wp1) sesingkat mungkin.
    double corridorBefore = originZ + (row-0.5)*kRowSpacing;
    double corridorAfter  = originZ + (row+0.5)*kRowSpacing;
    double corridorZ = (std::abs(startZ-corridorBefore) < std::abs(startZ-corridorAfter))
                        ? corridorBefore : corridorAfter;
    *wp1X = startX;  *wp1Z = corridorZ;  // segmen 1: lurus ke garis koridor (Z berubah, X tetap)
    *wp2X = targetX; *wp2Z = corridorZ;  // segmen 2: menyusuri koridor (X berubah, Z tetap)
    // segmen 3 (wp2->target): langkah pendek masuk ke posisi pohon persisnya
}

void Engine::startJob_(int workerIdx, const std::string& kind, int treeId, double targetX, double targetZ){
    WorkerJob& w = workers_[workerIdx];
    w.busy = true; w.kind = kind; w.treeId = treeId; w.phase.clear();
    w.startX = w.x; w.startZ = w.z; // mulai dari posisi TERAKHIR (bukan reset ke 0,0)
    w.targetX = targetX; w.targetZ = targetZ;
    computeCorridorPath_(w.startX, w.startZ, targetX, targetZ, treeId, &w.wp1X, &w.wp1Z, &w.wp2X, &w.wp2Z);
    double seg1 = std::sqrt((w.wp1X-w.startX)*(w.wp1X-w.startX) + (w.wp1Z-w.startZ)*(w.wp1Z-w.startZ));
    double seg2 = std::sqrt((w.wp2X-w.wp1X)*(w.wp2X-w.wp1X) + (w.wp2Z-w.wp1Z)*(w.wp2Z-w.wp1Z));
    double seg3 = std::sqrt((targetX-w.wp2X)*(targetX-w.wp2X) + (targetZ-w.wp2Z)*(targetZ-w.wp2Z));
    double dist = seg1+seg2+seg3; // jarak TOTAL jalur koridor (bukan garis lurus lagi)
    w.walkDuration = std::max(0.4, dist / kWorkerWalkSpeed); // minimum 0.4s spy tak "snap" instan kalau kebetulan dekat
    // BUG FUNGSIONAL diperbaiki: hrEfficiency() dulu HANYA dipakai di formula
    // abstractHa yg SELALU 0 dlm kondisi normal (semua Ha sudah jadi block
    // nyata sejak beliHa() diperbaiki) -- SDM berjenjang (Krani/Krani Kepala/
    // Asisten Afdeling/Asisten Kepala/Manager, YG SEMUANYA berkontribusi ke
    // hrEfficiency) TAK PERNAH benar-benar berdampak ke gameplay nyata,
    // meski pemain menghabiskan jutaan rupiah + gaji harian besar utk
    // merekrutnya (dilaporkan pengguna: "fungsi SDM blm diketahui jelas").
    // Sekarang efisiensi MEMPERCEPAT waktu kerja di TEMPAT saja (taskDuration,
    // bukan walkDuration -- jarak fisik tak berubah, tak masuk akal SDM
    // mempercepat langkah kaki pekerja). Selaras literatur manajemen SDM
    // sawit: supervisi/administrasi yg baik (Mandor mengawasi, Krani
    // mencatat/mengorganisir) mengurangi waktu terbuang di lapangan.
    w.totalDuration = w.walkDuration + taskDuration(kind) / hrEfficiency();
    w.remaining = w.totalDuration;
    w.carrying = false;
}

void Engine::completeJob_(WorkerJob& job){
    Tree* t = treeById(job.treeId);
    const std::string kind = job.kind;

    if (kind=="angkut"){
        if (job.phase=="walk"){
            if (t) t->hasTbsReady = false;
            job.phase = "toTPH";
            job.startX = job.x; job.startZ = job.z; // dari posisi pohon (sudah sampai)
            // TPH block SENDIRI (bukan konstanta global lagi) -- celah
            // diperbaiki: dulu SEMUA pekerja jalan ke 1 TPH dekat Block A01,
            // tak masuk akal utk block yg jauh scr spasial (mis. A02+).
            int bId = blockIdForTree(job.treeId);
            double tphX = kTphX, tphZ = kTphZ; // fallback kalau treeId blm/tak ketemu block manapun
            if (bId >= 0 && bId < (int)blocks_.size()){ tphX = blocks_[bId].tphX; tphZ = blocks_[bId].tphZ; }
            job.targetX = tphX; job.targetZ = tphZ;
            {
                computeCorridorPath_(job.startX, job.startZ, job.targetX, job.targetZ, job.treeId,
                                      &job.wp1X, &job.wp1Z, &job.wp2X, &job.wp2Z);
                double seg1 = std::sqrt((job.wp1X-job.startX)*(job.wp1X-job.startX) + (job.wp1Z-job.startZ)*(job.wp1Z-job.startZ));
                double seg2 = std::sqrt((job.wp2X-job.wp1X)*(job.wp2X-job.wp1X) + (job.wp2Z-job.wp1Z)*(job.wp2Z-job.wp1Z));
                double seg3 = std::sqrt((job.targetX-job.wp2X)*(job.targetX-job.wp2X) + (job.targetZ-job.wp2Z)*(job.targetZ-job.wp2Z));
                double dist = seg1+seg2+seg3; // jarak TOTAL jalur koridor ke TPH block ini
                job.walkDuration = std::max(0.4, dist / kWorkerWalkSpeed);
            }
            job.totalDuration = job.walkDuration; // fase angkut->TPH murni perjalanan, tak ada "kerja diam"
            job.remaining = job.totalDuration;
            job.carrying = true; // mulai bawa TBS scr visual
            return; // masih busy, lanjut fase berikutnya
        } else {
            job.carrying = false;
            bool overripe = t && t->tbsOverripe;
            int bId = blockIdForTree(job.treeId);
            if (bId >= 0 && bId < (int)blocks_.size()){
                Block& b = blocks_[bId];
                double combinedStock = b.tphStock + b.tphStockOverripe;
                if (combinedStock < eco_.tphCap){ // tphCap = kapasitas SETIAP TPH (bukan gabungan sekebun lagi)
                    if (overripe) b.tphStockOverripe += 1; else b.tphStock += 1;
                }
            }
            emit(EventType::Toast, overripe ? "+1 tandan LEWAT MATANG di TPH (harga lbh rendah)" : "+1 tandan di TPH");
            emit(EventType::HudChanged);
            job.busy=false; job.kind.clear(); job.treeId=-1; job.phase.clear();
            return;
        }
    }

    if (t){
        if (kind=="tunas"){ t->frond = 0.08; emit(EventType::Toast,"Pelepah ditunas",t->id); }
        else if (kind=="panen"){ t->tbsOverripe = (t->ffb==FfbState::Overripe); t->ffb=FfbState::None; t->ffbTimer=14+randUnit_()*14; t->ffbTimerMax=t->ffbTimer; t->hasTbsReady=true; emit(EventType::Toast,"TBS dipanen!",t->id); }
        else if (kind=="pupuk"){ t->nutrition = std::min(1.0, t->nutrition+0.4); emit(EventType::Toast,"Dipupuk",t->id); }
        else if (kind=="pestisida"){ t->health = HealthState::Sehat; emit(EventType::Toast,"Hama tuntas",t->id); }
        else if (kind=="fungisida"){
            bool success = randUnit_() < (0.7 + t->nutrition*0.2);
            if (success){ t->health=HealthState::Sehat; t->sickTimer=0; emit(EventType::Toast,"Ganoderma sembuh",t->id); }
            else { emit(EventType::Toast,"Belum sembuh, coba lagi",t->id); }
        } else if (kind=="tebang"){
            double x=t->x, z=t->z; int tid=t->id;
            *t = Tree{};
            t->id=tid; t->x=x; t->z=z;
            t->ageYears = 2.0 + randUnit_()*10.0;
            t->frond = 0.15+randUnit_()*0.3;
            t->ffbTimer = 10+randUnit_()*20;
            t->ffbTimerMax = t->ffbTimer; // fondasi countdown/progress bar -- Tree{} reset ke default, perlu disinkronkan lagi
            t->nutrition = 0.6+randUnit_()*0.3;
            emit(EventType::Toast,"Ditanam ulang",t->id);
        }
        emit(EventType::TreeChanged,"",t->id);
    }
    emit(EventType::HudChanged);
    job.busy=false; job.kind.clear(); job.treeId=-1; job.phase.clear();

    // Auto-assign pekerja tambahan (indeks > 0) HANYA jika MANDOR sudah
    // direkrut -- BUG diperbaiki: dulu pakai autoMode_ (SELALU true secara
    // default), TAK PERNAH benar2 terikat status rekrut Mandor sama sekali
    // (dilaporkan pengguna). Job desk Mandor SESUNGGUHNYA (job listing
    // industri sawit Malaysia, 2026): "assign daily task for harvesting &
    // maintenance mandore... monitor & supervise harvesting interval" --
    // PERSIS mekanisme auto-assign ini (pilih prioritas: angkut siap ->
    // panen matang -> obati Ganoderma -> semprot hama -> tunas menumpuk).
    // Tanpa Mandor, pemain HARUS dispatch tiap pekerja SENDIRI -- worker
    // tambahan yg direkrut sblm ada Mandor tetap idle nunggu perintah manual,
    // realistis: tanpa supervisor lapangan, tak ada yg mengoordinasikan
    // penugasan harian scr otomatis.
    if (hr_.mandor >= 1){
        for (size_t i=1;i<workers_.size();++i) if (!workers_[i].busy){ autoAssign_((int)i); }
    }
}

void Engine::autoAssign_(int workerIdx){
    if (workerIdx<0 || workerIdx>=(int)workers_.size() || workers_[workerIdx].busy) return;
    // prioritas: angkut TBS siap -> panen matang -> obati Ganoderma -> semprot hama -> tunas menumpuk
    for (auto& t : trees_) if (t.hasTbsReady){ actionAngkut(t.id); return; }
    // Malam hari -- LEWATI prioritas panen sepenuhnya (bukan cuma andalkan
    // actionPanen return false) -- tanpa ini, Mandor akan mencoba panen tiap
    // 2 detik SEMALAMAN, gagal terus, menyebabkan toast "Malam hari..."
    // berulang-ulang yg mengganggu. Angkut/obati/semprot/tunas TETAP boleh
    // malam (bukan aktivitas "memanen" yg dibatasi literatur).
    if (timeOfDay() != TimeOfDay::Malam){
        for (auto& t : trees_) if ((t.ffb==FfbState::Ripe||t.ffb==FfbState::Overripe) && t.health!=HealthState::Mati){ actionPanen(t.id); return; }
    }
    for (auto& t : trees_) if (t.health==HealthState::Ganoderma && eco_.money>=cfg_.priceFungisida){ actionFungisida(t.id); return; }
    for (auto& t : trees_) if (t.health==HealthState::Hama && eco_.money>=cfg_.pricePestisida){ actionPestisida(t.id); return; }
    for (auto& t : trees_) if (t.frond>0.7 && t.health!=HealthState::Mati){ actionTunas(t.id); return; }
}

// ---------------------------------------------------------------------------
// AKSI KEBUN
// ---------------------------------------------------------------------------
bool Engine::actionTunas(int treeId){
    Tree* t = treeById(treeId);
    if (!t || !(t->frond>0.35 && t->health!=HealthState::Mati)) return false;
    if (isTreeAssigned_(treeId)){ emit(EventType::LogOnly, "Pohon #" + std::to_string(treeId) + " sudah sedang dikerjakan pekerja lain", treeId); return false; }
    int wi = findFreeWorker_();
    if (wi<0){ emit(EventType::Toast,"Semua pekerja sedang bertugas"); return false; }
    startJob_(wi, "tunas", treeId, t->x, t->z);
    return true;
}
bool Engine::actionPanen(int treeId){
    Tree* t = treeById(treeId);
    if (!t || !((t->ffb==FfbState::Ripe||t->ffb==FfbState::Overripe) && t->health!=HealthState::Mati)) return false;
    // Malam hari -- TANPA aktivitas panen baru sama sekali, sesuai praktik
    // nyata industri sawit (pemanen kerja ~6:30-13:30 WIB, tak ada
    // pemanenan malam hari -- lihat catatan lengkap di TimeOfDay, types.hpp).
    if (timeOfDay() == TimeOfDay::Malam){ emit(EventType::Toast,"🌙 Malam hari -- panen cuma dilakukan siang hari (jam kerja 6:30-13:30 WIB, sesuai praktik nyata)"); return false; }
    if (isTreeAssigned_(treeId)){ emit(EventType::LogOnly, "Pohon #" + std::to_string(treeId) + " sudah sedang dikerjakan pekerja lain", treeId); return false; }
    int wi = findFreeWorker_();
    if (wi<0){ emit(EventType::Toast,"Semua pekerja sedang bertugas"); return false; }
    startJob_(wi, "panen", treeId, t->x, t->z);
    return true;
}
bool Engine::actionAngkut(int treeId){
    Tree* t = treeById(treeId);
    if (!t || !t->hasTbsReady) return false;
    if (isTreeAssigned_(treeId)){ emit(EventType::LogOnly, "Pohon #" + std::to_string(treeId) + " sudah sedang dikerjakan pekerja lain", treeId); return false; }
    int wi = findFreeWorker_();
    if (wi<0){ emit(EventType::Toast,"Semua pekerja sedang bertugas"); return false; }
    startJob_(wi, "angkut", treeId, t->x, t->z);
    workers_[wi].phase = "walk";
    return true;
}
bool Engine::actionPupuk(int treeId){
    Tree* t = treeById(treeId);
    if (!t || t->health==HealthState::Mati) return false;
    if (eco_.money < cfg_.pricePupuk){ emit(EventType::Toast,"Uang tidak cukup"); return false; }
    if (isTreeAssigned_(treeId)){ emit(EventType::LogOnly, "Pohon #" + std::to_string(treeId) + " sudah sedang dikerjakan pekerja lain", treeId); return false; }
    int wi = findFreeWorker_();
    if (wi<0){ emit(EventType::Toast,"Semua pekerja sedang bertugas"); return false; }
    eco_.money -= cfg_.pricePupuk;
    startJob_(wi, "pupuk", treeId, t->x, t->z);
    emit(EventType::HudChanged);
    return true;
}
bool Engine::actionPestisida(int treeId){
    Tree* t = treeById(treeId);
    if (!t || t->health!=HealthState::Hama) return false;
    if (eco_.money < cfg_.pricePestisida){ emit(EventType::Toast,"Uang tidak cukup"); return false; }
    if (isTreeAssigned_(treeId)){ emit(EventType::LogOnly, "Pohon #" + std::to_string(treeId) + " sudah sedang dikerjakan pekerja lain", treeId); return false; }
    int wi = findFreeWorker_();
    if (wi<0){ emit(EventType::Toast,"Semua pekerja sedang bertugas"); return false; }
    eco_.money -= cfg_.pricePestisida;
    startJob_(wi, "pestisida", treeId, t->x, t->z);
    emit(EventType::HudChanged);
    return true;
}
bool Engine::actionFungisida(int treeId){
    Tree* t = treeById(treeId);
    if (!t || t->health!=HealthState::Ganoderma) return false;
    if (eco_.money < cfg_.priceFungisida){ emit(EventType::Toast,"Uang tidak cukup"); return false; }
    if (isTreeAssigned_(treeId)){ emit(EventType::LogOnly, "Pohon #" + std::to_string(treeId) + " sudah sedang dikerjakan pekerja lain", treeId); return false; }
    int wi = findFreeWorker_();
    if (wi<0){ emit(EventType::Toast,"Semua pekerja sedang bertugas"); return false; }
    eco_.money -= cfg_.priceFungisida;
    startJob_(wi, "fungisida", treeId, t->x, t->z);
    emit(EventType::HudChanged);
    return true;
}
bool Engine::actionTebangTanamUlang(int treeId){
    Tree* t = treeById(treeId);
    if (!t || t->health!=HealthState::Mati) return false;
    if (eco_.money < cfg_.priceTebang){ emit(EventType::Toast,"Uang tidak cukup"); return false; }
    if (isTreeAssigned_(treeId)){ emit(EventType::LogOnly, "Pohon #" + std::to_string(treeId) + " sudah sedang dikerjakan pekerja lain", treeId); return false; }
    int wi = findFreeWorker_();
    if (wi<0){ emit(EventType::Toast,"Semua pekerja sedang bertugas"); return false; }
    eco_.money -= cfg_.priceTebang;
    startJob_(wi, "tebang", treeId, t->x, t->z);
    emit(EventType::HudChanged);
    return true;
}

// --- Aksi MASSAL: panggil aksi tunggal berulang selama masih ada pekerja
// bebas & pohon yg memenuhi syarat -- lihat catatan literatur di engine.hpp.
// --- Aksi MASSAL versi INSTAN (revisi dari versi sebelumnya yg dibatasi
// jumlah pekerja bebas) — pekerja TIDAK perlu mendatangi pohon satu-persatu,
// sistem langsung menerapkan aksi ke SEMUA pohon yg memenuhi syarat dlm satu
// waktu. Setiap pohon yg diproses ditandai (lastMarkDay/lastMarkKind) supaya
// terlihat jelas di layar mana yg sudah dikerjakan hari ini.
//
// Panen & Angkut: SELALU gratis (sama spt versi single-tree), jadi langsung
// diterapkan ke semua yg memenuhi syarat tanpa cek uang. Angkut dibatasi
// kapasitas TPH (tphCap) -- kalau sudah penuh sebelum semua terangkut,
// berhenti di situ (sisanya tetap "siap angkut", tunggu truk jalan/TPH
// dikosongkan baru bisa lanjut).
//
// Pupuk/Pestisida/Fungisida: PAKAI UANG (beli pupuk/bahan kimia utk semua
// pohon), jadi dicek TOTAL BIAYA dulu utk semua pohon yg memenuhi syarat --
// kalau uang cukup, semua diproses sekaligus (satu transaksi, bukan
// sebagian-sebagian yg membingungkan); kalau tidak cukup, TIDAK ADA yg
// diproses & pemain diberi tahu persis berapa total yg dibutuhkan.
int Engine::actionPanenSemua(){
    int n=0;
    // Malam hari -- TANPA aktivitas panen baru sama sekali (konsistensi dgn
    // actionPanen single-tree, lihat catatan lengkap di sana).
    if (timeOfDay() == TimeOfDay::Malam){
        emit(EventType::Toast,"🌙 Malam hari -- panen cuma dilakukan siang hari (jam kerja 6:30-13:30 WIB, sesuai praktik nyata)");
        return 0;
    }
    for (auto& t : trees_){
        if (!((t.ffb==FfbState::Ripe||t.ffb==FfbState::Overripe) && t.health!=HealthState::Mati)) continue;
        t.tbsOverripe = (t.ffb==FfbState::Overripe); // celah diperbaiki: dulu tak ditandai sama sekali
                                                       // di jalur aksi massal (beda dr jalur single-tree)
        t.ffb = FfbState::None; t.ffbTimer = 14 + randUnit_()*14; t.ffbTimerMax = t.ffbTimer; t.hasTbsReady = true;
        t.lastMarkDay = eco_.day; t.lastMarkKind = 0;
        emit(EventType::TreeChanged,"",t.id);
        n++;
    }
    if (n>0) emit(EventType::Toast, std::to_string(n)+" pokok dipanen sekaligus");
    emit(EventType::HudChanged);
    return n;
}
int Engine::actionAngkutSemua(){
    int n=0;
    // Iterasi PER BLOCK -- tiap block dicek kapasitas TPH-nya SENDIRI (bukan
    // cap gabungan sekebun lagi). Block penuh -> lanjut ke block berikutnya
    // (bukan berhenti total spt versi lama, krn skrg TPH-nya independen).
    for (auto& b : blocks_){
        for (int i=b.treeStartIdx; i<b.treeEndIdx && i<(int)trees_.size(); ++i){
            if ((b.tphStock+b.tphStockOverripe) >= eco_.tphCap) break; // TPH block ini penuh
            Tree& t = trees_[i];
            if (!t.hasTbsReady) continue;
            t.hasTbsReady = false;
            if (t.tbsOverripe) b.tphStockOverripe += 1; else b.tphStock += 1;
            t.lastMarkDay = eco_.day; t.lastMarkKind = 1;
            emit(EventType::TreeChanged,"",t.id);
            n++;
        }
    }
    if (n>0) emit(EventType::Toast, std::to_string(n)+" tandan diangkut sekaligus ke TPH");
    emit(EventType::HudChanged);
    return n;
}
int Engine::actionPupukSemua(){
    int eligible=0;
    for (auto& t : trees_) if (t.health!=HealthState::Mati) eligible++;
    double total = eligible * cfg_.pricePupuk;
    if (eligible==0) return 0;
    if (eco_.money < total){ emit(EventType::Toast, "Uang tidak cukup (butuh Rp "+std::to_string((long long)total)+" utk "+std::to_string(eligible)+" pokok)"); return 0; }
    eco_.money -= total;
    for (auto& t : trees_){
        if (t.health==HealthState::Mati) continue;
        t.nutrition = std::min(1.0, t.nutrition+0.4);
        t.lastMarkDay = eco_.day; t.lastMarkKind = 2;
        emit(EventType::TreeChanged,"",t.id);
    }
    emit(EventType::Toast, eligible>0 ? (std::to_string(eligible)+" pokok dipupuk sekaligus") : "");
    emit(EventType::HudChanged);
    return eligible;
}
int Engine::actionPestisidaSemua(){
    int eligible=0;
    for (auto& t : trees_) if (t.health==HealthState::Hama) eligible++;
    if (eligible==0) return 0;
    double total = eligible * cfg_.pricePestisida;
    if (eco_.money < total){ emit(EventType::Toast, "Uang tidak cukup (butuh Rp "+std::to_string((long long)total)+" utk "+std::to_string(eligible)+" pokok)"); return 0; }
    eco_.money -= total;
    for (auto& t : trees_){
        if (t.health!=HealthState::Hama) continue;
        t.health = HealthState::Sehat;
        t.lastMarkDay = eco_.day; t.lastMarkKind = 3;
        emit(EventType::TreeChanged,"",t.id);
    }
    emit(EventType::Toast, std::to_string(eligible)+" pokok disemprot sekaligus");
    emit(EventType::HudChanged);
    return eligible;
}
int Engine::actionFungisidaSemua(){
    int eligible=0;
    for (auto& t : trees_) if (t.health==HealthState::Ganoderma) eligible++;
    if (eligible==0) return 0;
    double total = eligible * cfg_.priceFungisida;
    if (eco_.money < total){ emit(EventType::Toast, "Uang tidak cukup (butuh Rp "+std::to_string((long long)total)+" utk "+std::to_string(eligible)+" pokok)"); return 0; }
    eco_.money -= total;
    int cured=0;
    for (auto& t : trees_){
        if (t.health!=HealthState::Ganoderma) continue;
        bool success = randUnit_() < (0.7 + t.nutrition*0.2); // sama persis probabilitas versi single-tree
        if (success){ t.health=HealthState::Sehat; t.sickTimer=0; cured++; }
        t.lastMarkDay = eco_.day; t.lastMarkKind = 4;
        emit(EventType::TreeChanged,"",t.id);
    }
    emit(EventType::Toast, std::to_string(eligible)+" pokok diobati sekaligus ("+std::to_string(cured)+" sembuh)");
    emit(EventType::HudChanged);
    return eligible;
}

void Engine::sellOrProcessTbs_(double amount, bool silent){
    if (amount<=0) return;
    if (pksState_.built){
        pksState_.inputSilo += amount;
        if (!silent) emit(EventType::Toast, std::to_string((long long)amount)+" tandan masuk ke silo PKS");
    } else {
        double income = amount * eco_.pricePerTandan;
        eco_.money += income;
        if (!silent) emit(EventType::FlyMoney, "+Rp "+std::to_string((long long)income));
    }
    emit(EventType::HudChanged);
}

void Engine::movePlayerAvatar(double dirX, double dirZ, double dt, double cameraYaw){
    double len = std::sqrt(dirX*dirX + dirZ*dirZ);
    if (len < 1e-6){
        playerAvatar_.moving = false;
        // Fitur baru diminta pengguna (item #13 laporan): "Badan pekerja
        // mengikuti arah pergerakan. Saat pemain tidak bergerak, pekerja
        // dapat diarahkan mengikuti orientasi kamera secara smooth."
        // SEBELUMNYA fungsi ini return LANGSUNG saat idle -- facingRad
        // diam BEKU di arah gerak terakhir, tak peduli kamera diputar
        // (touch-drag) ke mana pun setelahnya. MELENGKAPI (bukan
        // bertentangan dgn) redesain kamera sebelumnya (poin #1, "kamera
        // kontrol terpisah dari joystick/gerakan avatar") -- arah
        // pengaruh di sini KEBALIKANNYA: avatar mengikuti kamera SAAT
        // IDLE, BUKAN kamera mengikuti avatar (yg SUDAH diperbaiki, tetap
        // tak berubah -- kamera tak pernah "ditulis balik" oleh fungsi
        // ini, cuma DIBACA sbg referensi target rotasi).
        //
        // Interpolasi exponential SMOOTH (bukan instant snap) -- konsisten
        // pola smoothing lain di codebase ini (mis. updateThirdPersonCamera(),
        // renderer_gl.cpp). Selisih sudut dinormalisasi ke [-PI,PI] DULU
        // spy interpolasi ambil JALUR TERPENDEK (bukan berputar jauh krn
        // wrap-around 0<->2*PI, mis. dari 350derajat ke 10derajat harus
        // lewat +20derajat, BUKAN -340derajat).
        const double kPi = 3.14159265358979;
        double diff = cameraYaw - playerAvatar_.facingRad;
        while (diff > kPi) diff -= 2.0*kPi;
        while (diff < -kPi) diff += 2.0*kPi;
        double smoothing = 1.0 - std::exp(-dt * 4.0); // sedang -- terlihat halus, tak instan/kaku
        playerAvatar_.facingRad += diff * smoothing;
        return;
    }
    dirX /= len; dirZ /= len; // normalisasi -- gerak diagonal TAK lebih cepat dari gerak lurus

    // REDESAIN TOTAL (dilaporkan pengguna: "Joystick kiri harus 100% menjadi
    // kontrol locomotion pekerja, bukan kontrol kamera... Kamera harus
    // mempunyai kontrol terpisah seperti pola Roblox / third-person mobile
    // game"). SEBELUMNYA parameter terakhir ini adalah "cameraYawOffset"
    // (RELATIF thd facingRad avatar FRAME SEBELUMNYA: camYaw = facingRad +
    // offset), dan offset itu "diserap PERMANEN" ke facingRad avatar
    // setiap kali avatar bergerak (lihat riwayat call site JNI/EngineBridge)
    // -- efek sampingnya: renderer menghitung ULANG cameraYaw = facingRad
    // (yg sudah menyerap arah baru) + offset (sudah 0 krn direset), hasil
    // akhirnya KAMERA IKUT BERPUTAR PERSIS SETIAP KALI AVATAR BERPUTAR
    // (dibuktikan dari video: badan avatar terlihat dari samping lalu
    // dari belakang penuh dlm waktu SANGAT singkat setelah joystick
    // sedikit digeser, kamera "menempel" pada facingRad avatar).
    //
    // Sekarang: parameter ini adalah `cameraYaw` ABSOLUT (orientasi kamera
    // SAAT INI, dikontrol PENUH oleh touch-drag bebas di layar -- lihat
    // renderer_gl.cpp, TIDAK PERNAH diturunkan dari facingRad avatar).
    // dirZ (maju/mundur joystick) = maju relatif ARAH PANDANG KAMERA (utk
    // rasa kontrol third-person yg wajar -- "atas joystick" selalu berarti
    // "menjauh dari kamera ke arah pandang", standar Roblox/PUBG Mobile),
    // dirX (kiri/kanan) = strafe relatif arah yg sama. facingRad avatar
    // TETAP diperbarui mengikuti arah GERAK (bukan arah kamera persis) --
    // avatar tetap menghadap ke mana ia berjalan (joystick diagonal ->
    // badan menghadap diagonal), TAPI kamera SENDIRI tidak pernah ikut
    // berputar akibat ini (lihat renderer_gl.cpp: cameraYaw sekarang
    // murni dari touch-drag, tak lagi ditambah facingRad).
    double worldDirX = dirZ*std::cos(cameraYaw) - dirX*std::sin(cameraYaw);
    double worldDirZ = dirZ*std::sin(cameraYaw) + dirX*std::cos(cameraYaw);

    playerAvatar_.moving = true;
    // Rotasi SMOOTH mengikuti arah gerak -- fitur baru diminta pengguna:
    // "Jangan membuat worker langsung berputar 180 derajat mengikuti
    // kamera. Gunakan rotasi smooth: targetYaw = atan2(direction.x,
    // direction.z); workerYaw = smoothAngle(workerYaw, targetYaw,
    // rotationSpeed*deltaTime)". SEBELUMNYA facingRad = atan2(...) LANGSUNG
    // (instant snap) -- avatar "meloncat" seketika menghadap arah baru
    // tanpa transisi visual sama sekali, terutama terlihat kasar saat
    // joystick dibelokkan tajam (mis. dari maju ke mundur, selisih 180
    // derajat -- badan avatar "membalik" instan tanpa animasi berputar).
    // Interpolasi exponential SMOOTH -- pola & alasan lengkap IDENTIK dgn
    // idle-rotate-toward-camera di atas (selisih sudut dinormalisasi ke
    // [-PI,PI] dulu spy ambil jalur TERPENDEK, bukan berputar jauh krn
    // wrap-around). Konstanta 8.0 (LEBIH CEPAT dari idle's 4.0) --
    // rotasi mengikuti gerakan aktif perlu terasa RESPONSIF (avatar tak
    // boleh terasa "lambat mematuhi" joystick), tapi TETAP ada transisi
    // visual yg terlihat (bukan instan) -- konvergen penuh dlm ~0.3-0.5 detik.
    double targetFacing = std::atan2(worldDirZ, worldDirX);
    const double kPiRot = 3.14159265358979;
    double facingDiff = targetFacing - playerAvatar_.facingRad;
    while (facingDiff > kPiRot) facingDiff -= 2.0*kPiRot;
    while (facingDiff < -kPiRot) facingDiff += 2.0*kPiRot;
    double facingSmoothing = 1.0 - std::exp(-dt * 8.0);
    playerAvatar_.facingRad += facingDiff * facingSmoothing;

    double speed = kWorkerWalkSpeed; // SAMA dgn kecepatan pekerja NPC -- animasi drawWorker() konsisten
    double newX = playerAvatar_.x + worldDirX*speed*dt;
    double newZ = playerAvatar_.z + worldDirZ*speed*dt;

    // Collision SLIDING sederhana thd pohon -- BUKAN berhenti total saat
    // gerakan diagonal menyenggol pohon (itu terasa "nyangkut", UX buruk),
    // tapi coba gerak per-SUMBU independen: kalau gerak penuh (X&Z sekaligus)
    // akan bertabrakan, coba X saja / Z saja -- avatar "meluncur" di sisi
    // pohon, standar praktik game 3D sederhana (bukan fisika penuh).
    // BUG UX diperbaiki (dilaporkan pengguna: "joystick digerakkan ke kanan/
    // kiri tapi avatar tidak mau bergerak"). BUKAN bug input/joystick sama
    // sekali -- terverifikasi numerik: collision SEBENARNYA bekerja BENAR
    // scr teknis, tapi radius 1.3 (sebelumnya) terlalu besar relatif jarak
    // antar pohon dlm 1 baris (kColSpacing=5.2), menyisakan celah cuma ~2.6
    // unit -- avatar terjebak di ruang sempit antar pohon berdekatan.
    // Radius batang pohon SEBENARNYA (literatur, diameter 45-65cm) cuma
    // ~0.3 unit -- 1.3 sudah 4x lebih besar dari radius batang asli sejak
    // awal. Turunkan ke 0.6 (2x radius batang, buffer wajar) -> celah antar
    // pohon jadi ~4.0 unit, cukup lega utk avatar (lebar tubuh ~0.3 unit).
    const double kCollisionRadius = 0.6;
    // Radius collision thd PEKERJA NPC -- BUG UX diperbaiki (dilaporkan
    // pengguna via screenshot: "ada 2 avatar di iOS"). BUKAN bug rendering
    // (drawWorker()/drawFarmerAvatar() masing2 dipanggil TEPAT sekali,
    // terverifikasi dari source) -- tapi KETIADAAN collision avoidance
    // antara avatar pemain & worker NPC, keduanya bisa kebetulan berada
    // di posisi SAMA, terlihat spt "dua figur tumpang tindih". Radius lebih
    // kecil drpd pohon (0.8 vs 1.3) -- tubuh manusia lbh ramping dari batang.
    const double kWorkerCollisionRadius = 0.8;
    auto collides = [&](double px, double pz)->bool{
        for (const auto& t : trees_){
            if (t.health == HealthState::Mati) continue; // pohon tumbang/ditebang -- tak jadi penghalang
            double dx = px-t.x, dz = pz-t.z;
            if (dx*dx+dz*dz < kCollisionRadius*kCollisionRadius) return true;
        }
        for (const auto& w : workers_){
            // TANPA filter busy -- workersRenderInfo() mengembalikan SEMUA
            // worker (busy MAUPUN idle) tanpa filter, jadi drawWorker() di
            // JNI/EngineBridge memang menggambar SEMUANYA di layar. Collision
            // harus konsisten dgn apa yg BENAR-BENAR terlihat, bukan cuma yg
            // sedang bekerja.
            double dx = px-w.x, dz = pz-w.z;
            if (dx*dx+dz*dz < kWorkerCollisionRadius*kWorkerCollisionRadius) return true;
        }
        return false;
    };

    if (!collides(newX, newZ)){
        playerAvatar_.x = newX; playerAvatar_.z = newZ;
    } else if (!collides(newX, playerAvatar_.z)){
        playerAvatar_.x = newX; // meluncur di sumbu X saja
    } else if (!collides(playerAvatar_.x, newZ)){
        playerAvatar_.z = newZ; // meluncur di sumbu Z saja
    }
    // (kalau ketiganya bertabrakan -- avatar diam total, sudut sempit di antara 2 pohon)

    // Batas area gerak -- cukup luas (mencakup beberapa block hasil beliHa()
    // ke arah +X) tanpa collision presisi per-block, MVP awal fitur baru ini.
    playerAvatar_.x = std::max(-60.0, std::min(500.0, playerAvatar_.x));
    playerAvatar_.z = std::max(-60.0, std::min(60.0, playerAvatar_.z));
}

void Engine::kirimTruk(int blockId){
    if (blockId < 0 || blockId >= (int)blocks_.size()) return;
    Block& b = blocks_[blockId];
    double normal = b.tphStock;
    double overripe = b.tphStockOverripe;
    double amount = normal + overripe;
    b.tphStock = 0;
    b.tphStockOverripe = 0;
    if (amount > 0){
        if (pksState_.built){
            pksState_.inputSilo += amount;
            emit(EventType::Toast, b.name+": "+std::to_string((long long)amount)+" tandan masuk ke silo PKS");
        } else {
            // TBS lewat-matang dijual dgn DISKON -- Corley & Tinker (2016) "The
            // Oil Palm" 5th ed. §11.5.5.1: tandan yg terlewat panen FFA-nya
            // sudah mulai naik, mutu turun (lihat kOverripeDiscount, engine.hpp).
            double income = normal*eco_.pricePerTandan + overripe*eco_.pricePerTandan*kOverripeDiscount;
            eco_.money += income;
            emit(EventType::FlyMoney, "+Rp "+std::to_string((long long)income));
        }
        emit(EventType::HudChanged);
        // Aktifkan animasi truk dari TPH BLOCK INI (posisi beda per block
        // sekarang) -- transaksi uang di atas sudah SELESAI instan, animasi
        // murni visual (lihat catatan di TruckState, types.hpp).
        truck_.active = true; truck_.progress = 0.0; truck_.blockId = blockId;
    }
}

// ---------------------------------------------------------------------------
// LAHAN
// ---------------------------------------------------------------------------
bool Engine::beliHa(double amountHa){
    if (land_.afdelings.empty()) return false;
    Afdeling& last = land_.afdelings.back();
    if (last.ha + amountHa > cfg_.afdelingMaxHa){ emit(EventType::Toast,"Afdeling ini penuh, buka afdeling baru dulu"); return false; }
    double price = std::round(haPricePerUnit() * amountHa);
    if (eco_.money < price){ emit(EventType::Toast,"Uang tidak cukup"); return false; }
    eco_.money -= price;
    last.ha += amountHa;

    // Setiap 1.0 Ha PENUH yg dibeli -> 1 block baru dgn 143 pohon SUNGGUHAN
    // (dulu cuma angka abstrak, TAK PERNAH ada pohonnya) -- ditempatkan di
    // area TERPISAH (originX digeser 120 unit/block, grid asli lebar ~57
    // unit dgn jitter jadi tak mungkin tumpang tindih). Sisa pecahan (kalau
    // amountHa bukan kelipatan 1.0 penuh) tetap dihitung scr abstrak spt
    // sebelumnya (lihat onNewDay_) -- blm cukup utk 1 grid 143 pohon penuh.
    int wholeHaBought = (int)std::floor(amountHa);
    for (int i=0; i<wholeHaBought; ++i){
        double newOriginX = (double)blocks_.size() * 120.0;
        int ts, te;
        double soilF, genV;
        generateBlockTrees_(newOriginX, 0.0, ts, te, soilF, genV);
        int newId = (int)blocks_.size();
        std::string blockName = "A" + std::string(blocks_.size()+1 < 10 ? "0" : "") + std::to_string((int)blocks_.size()+1);
        blocks_.push_back(Block{newId, blockName, last.id, 1.0, ts, te, newOriginX, 0.0, soilF, genV});
        blocks_.back().tphX = newOriginX + kTphX; blocks_.back().tphZ = 0.0 + kTphZ; // TPH sendiri di tepi grid block ini
        land_.sampleBlockHa += 1.0; // kecualikan dr formula hasil abstrak -- block ini skrg produksi via simulasi pohon sungguhan, bukan formula lagi
        // Deskripsi kualitatif kesuburan+genetik block baru -- pemain langsung
        // tahu apa yg didapat, bukan cuma angka tersembunyi (Corley & Tinker
        // §9.2.3.5 soil fertility & bab 6 D×P seed source -- lihat catatan
        // types.hpp, Block).
        std::string soilDesc = soilF > 1.10 ? "tanah sangat subur" : (soilF < 0.85 ? "tanah marjinal" : "tanah sedang");
        std::string genDesc = genV > 1.05 ? "benih unggul" : (genV < 0.92 ? "benih standar" : "benih rata-rata");
        emit(EventType::Toast, blockName+" dibuka dengan 143 pokok baru! ("+soilDesc+", "+genDesc+")");
    }

    emit(EventType::HudChanged); emit(EventType::ScreenChanged);
    emit(EventType::Toast, "+"+std::to_string((long long)amountHa)+" Ha ditambahkan ke "+last.name);
    return true;
}
bool Engine::bukaAfdelingBaru(){
    int need = (int)land_.afdelings.size();
    if (hr_.asistenAfdeling < need){ emit(EventType::Toast,"Rekrut Asisten Afdeling dulu"); return false; }
    double cost = std::round(cfg_.afdelingBaseCost * std::pow(cfg_.afdelingCostGrowth, land_.afdelings.size()));
    if (eco_.money < cost){ emit(EventType::Toast,"Uang tidak cukup"); return false; }
    eco_.money -= cost;
    Afdeling a; a.id=(int)land_.afdelings.size(); a.ha=0; a.name="Afdeling "+romanFor((int)land_.afdelings.size()+1);
    land_.afdelings.push_back(a);
    emit(EventType::HudChanged); emit(EventType::ScreenChanged);
    emit(EventType::Toast, a.name+" dibuka!");
    return true;
}

// ---------------------------------------------------------------------------
// SDM
// ---------------------------------------------------------------------------
const HrLevelDef* Engine::findLevel_(const std::string& key) const {
    for (auto& d : cfg_.hrLevels) if (d.key==key) return &d;
    return nullptr;
}
bool Engine::prereqOk_(const HrLevelDef& def) const {
    if (!def.hasPrereq) return true;
    if (hr_.countFor(def.prereq.key) < def.prereq.min) return false;
    if (def.prereq.hasAlso && hr_.countFor(def.prereq.alsoKey) < def.prereq.alsoMin) return false;
    return true;
}
bool Engine::maxOk_(const HrLevelDef& def) const {
    if (def.maxCount<0) return true;
    return hr_.countFor(def.key) < def.maxCount;
}
double Engine::costFor_(const HrLevelDef& def) const {
    if (def.key=="buruh") return cfg_.pricePekerja + std::max(0,(int)workers_.size()-1)*300000.0;
    return std::round(def.baseCost * std::pow(def.costGrowth, hr_.countFor(def.key)));
}
double Engine::hrEfficiency() const {
    double m = 1.0;
    m += hr_.mandor*0.03 + hr_.krani*0.02 + hr_.mandorBesar*0.05 + hr_.kraniKepala*0.04;
    m += hr_.asistenAfdeling*0.10 + hr_.asistenKepala*0.15 + hr_.manager*0.25;
    return m;
}
double Engine::totalDailySalary() const {
    const HrLevelDef* buruhDef = findLevel_("buruh");
    double sum = workers_.size() * (buruhDef ? buruhDef->salary : 110000.0);
    for (auto& def : cfg_.hrLevels){
        if (def.key=="buruh") continue;
        sum += hr_.countFor(def.key) * def.salary;
    }
    return sum;
}
bool Engine::rekrutLevel(const std::string& key){
    const HrLevelDef* def = findLevel_(key);
    if (!def){ return false; }
    if (!prereqOk_(*def)){ emit(EventType::Toast,"Prasyarat jabatan belum terpenuhi"); return false; }
    if (!maxOk_(*def)){ emit(EventType::Toast,"Sudah mencapai batas maksimum jabatan ini"); return false; }
    double cost = costFor_(*def);
    if (eco_.money < cost){ emit(EventType::Toast,"Uang tidak cukup"); return false; }
    eco_.money -= cost;
    if (key=="buruh"){
        WorkerJob w{};
        // Celah diperbaiki: sebelumnya pekerja baru spawn di (0,0) default --
        // yaitu TEPAT DI TENGAH barisan tanam (bukan di dekat kantor/basis spt
        // kru sungguhan berkumpul). Sekarang mulai dari area kantor.
        w.x = kOfficeX; w.z = kOfficeZ;
        workers_.push_back(w);
        hr_.buruh = (int)workers_.size();
    }
    else {
        bool isFirstMandor = (key=="mandor" && hr_.mandor==0);
        hr_.add(key,1);
        // Mandor PERTAMA direkrut -- jelaskan perubahan perilaku ke pemain
        // (job desk Mandor sesungguhnya: assign tugas harian & supervisi
        // interval panen -- lihat catatan lengkap di completeJob_).
        if (isFirstMandor){
            emit(EventType::Toast, "🎉 Mandor direkrut! Pekerja tambahan (di luar yg pertama) sekarang OTOMATIS mencari kerjaan sendiri (angkut->panen->obati->semprot->tunas, sesuai prioritas) -- tak perlu lagi kamu tugaskan satu-satu.");
        }
    }
    emit(EventType::HudChanged); emit(EventType::ScreenChanged);
    emit(EventType::Toast, def->name+" baru direkrut");
    return true;
}

std::vector<HrLevelInfo> Engine::hrLevelInfos() const {
    std::vector<HrLevelInfo> out;
    out.reserve(cfg_.hrLevels.size());
    for (const auto& def : cfg_.hrLevels){
        HrLevelInfo info;
        info.key = def.key; info.name = def.name; info.icon = def.icon;
        info.desc = def.desc; info.cite = def.cite;
        info.count = hr_.countFor(def.key);
        info.cost = costFor_(def);
        info.salary = def.salary;
        info.prereqMet = prereqOk_(def);
        info.underMax = maxOk_(def);
        if (def.hasPrereq){
            info.prereqDesc = "Butuh " + std::to_string(def.prereq.min) + "x " + def.prereq.key;
            if (def.prereq.hasAlso) info.prereqDesc += " & " + std::to_string(def.prereq.alsoMin) + "x " + def.prereq.alsoKey;
        }
        out.push_back(info);
    }
    return out;
}

// ---------------------------------------------------------------------------
// PKS
// ---------------------------------------------------------------------------
bool Engine::bangunPks(){
    if (hr_.manager<1){ emit(EventType::Toast,"Perlu Manager/ADM untuk membangun PKS sendiri"); return false; }
    if (eco_.money < pksState_.buildCost){ emit(EventType::Toast,"Uang tidak cukup"); return false; }
    eco_.money -= pksState_.buildCost;
    pksState_.built = true;
    emit(EventType::HudChanged); emit(EventType::ScreenChanged); emit(EventType::Toast,"PKS berhasil dibangun!");
    return true;
}
// Upgrade kapasitas TPH -- fitur baru diminta pengguna: "upgrade kapasitas
// tph, saat ini tph hanya mampu menampung 30 tbs, harusnya ada tambah
// fitur tph atau fitur memperbesar tempat penampungan tph". TAK ADA
// referensi kuantitatif ilmiah utk "kapasitas TPH" (sudah dicek -- scr
// praktik nyata TPH area terbuka, TBS ditumpuk di tanah, bukan struktur
// berkapasitas tetap spt silo) -- angka murni game balance, KONSISTEN
// dgn pola upgradePks() yg sudah ada (level naik, cost naik eksponensial).
// +15 kapasitas/level (30->45->60->75->...), cost dasar Rp 300rb tumbuh
// 60%/level -- upgrade pertama terjangkau relatif modal awal (Rp 750rb,
// lihat STORYLINE.md) tapi tak trivial, upgrade lanjutan makin mahal
// (dorong pemain pertimbangkan trade-off vs ekspansi lahan/SDM lain).
double Engine::tphUpgradeCost() const {
    return std::round(300000.0 * std::pow(1.6, eco_.tphLevel-1));
}
bool Engine::upgradeTph(){
    double cost = tphUpgradeCost();
    if (eco_.money < cost){ emit(EventType::Toast,"Uang tidak cukup"); return false; }
    eco_.money -= cost;
    eco_.tphLevel++;
    eco_.tphCap = cfg_.tphCapStart + (eco_.tphLevel-1)*15.0;
    emit(EventType::HudChanged); emit(EventType::ScreenChanged);
    std::ostringstream os;
    os << "Kapasitas TPH meningkat jadi " << (int)eco_.tphCap << " tandan (level " << eco_.tphLevel << ")";
    emit(EventType::Toast, os.str());
    return true;
}

bool Engine::upgradePks(){
    double cost = std::round(pksState_.upgradeCost * std::pow(cfg_.pksUpgradeGrowth, pksState_.level-1));
    if (eco_.money < cost){ emit(EventType::Toast,"Uang tidak cukup"); return false; }
    eco_.money -= cost;
    pksState_.level++;
    pksState_.oer = std::min(cfg_.oerMax, pksState_.oer + cfg_.oerStep);
    emit(EventType::HudChanged); emit(EventType::ScreenChanged); emit(EventType::Toast,"Kapasitas & rendemen PKS meningkat");
    return true;
}
bool Engine::prosesBatchPks(){
    int cap = cfg_.pksCapacityBase + pksState_.level*cfg_.pksCapacityPerLevel;
    double amount = std::min(pksState_.inputSilo, (double)cap);
    if (amount<=0){ emit(EventType::Toast,"Silo PKS kosong"); return false; }
    pksState_.inputSilo -= amount;
    double kgTbs = amount * pksState_.avgTandanKg;
    double cpoKg = kgTbs * pksState_.oer;
    double pkKg = kgTbs * pksState_.kerRate;
    double income = cpoKg*pksState_.cpoPrice + pkKg*pksState_.pkPrice;
    eco_.money += income;
    emit(EventType::HudChanged); emit(EventType::ScreenChanged);
    std::ostringstream os;
    os << "CPO " << (long long)cpoKg << " kg + PK " << (long long)pkKg << " kg terjual, +Rp " << (long long)income;
    emit(EventType::Toast, os.str());
    pksProcessPulseTimer_ = 2.0; // aktifkan pulsa visual 2 detik -- lihat drawPksBuilding()
    return true;
}

// ---------------------------------------------------------------------------
// SAVE / LOAD (JSON minimal, hanya field progres — bukan config)
// ---------------------------------------------------------------------------
std::string Engine::saveToJson() const {
    std::ostringstream os;
    os << std::fixed << std::setprecision(4);
    os << "{";
    os << "\"money\":" << eco_.money << ",";
    os << "\"day\":" << eco_.day << ",";
    // tphStock/tphStockOverripe DIHAPUS dr sini -- sudah pindah jadi per-block
    // (Block.tphStock/tphStockOverripe). CATATAN JUJUR: trees_ & blocks_
    // (termasuk stok TPH tiap block) MEMANG BELUM PERNAH ikut tersimpan sama
    // sekali di save/load ini (gap PRE-EXISTING, bukan diperkenalkan sesi
    // ini) -- artinya block hasil beliHa() akan HILANG kalau save lalu load
    // ulang. Di luar cakupan perbaikan TPH-per-block kali ini; perlu sesi
    // terpisah utk serialisasi trees_/blocks_ penuh.
    os << "\"tphCap\":" << eco_.tphCap << ",";
    os << "\"pricePerTandan\":" << eco_.pricePerTandan << ",";
    os << "\"afdelings\":[";
    for (size_t i=0;i<land_.afdelings.size();++i){
        auto& a = land_.afdelings[i];
        if (i) os << ",";
        os << "{\"ha\":" << a.ha << "}";
    }
    os << "],";
    os << "\"hr\":{";
    os << "\"buruh\":" << hr_.buruh << ",\"mandor\":" << hr_.mandor << ",\"krani\":" << hr_.krani
       << ",\"mandorBesar\":" << hr_.mandorBesar << ",\"kraniKepala\":" << hr_.kraniKepala
       << ",\"asistenAfdeling\":" << hr_.asistenAfdeling << ",\"asistenKepala\":" << hr_.asistenKepala
       << ",\"manager\":" << hr_.manager;
    os << "},";
    // BUG BESAR diperbaiki: "built" dulu ditulis sbg literal true/false (kata),
    // tapi findNum() di loadFromJson cuma sanggup parse ANGKA murni (std::stod)
    // -- setiap kali dimuat, parsing "built":true GAGAL diam-diam (exception
    // ditangkap catch(...), balik nullopt), jadi pksState_.built TAK PERNAH
    // ter-restore, SELALU reset ke false meski PKS sudah dibangun & level 4+
    // sebelumnya (persis laporan pengguna: "PKS tak ter-unlock stlh tutup app").
    // Sekarang ditulis sbg angka 0/1, konsisten dgn parser yg sudah ada.
    os << "\"pks\":{\"built\":" << (pksState_.built?1:0) << ",\"level\":" << pksState_.level
       << ",\"oer\":" << pksState_.oer << ",\"inputSilo\":" << pksState_.inputSilo << "},";

    // --- trees_ & blocks_ -- BUG BESAR diperbaiki: sebelumnya data ini SAMA
    // SEKALI tak pernah ikut save/load (dari awal proyek, bukan diperkenalkan
    // sesi ini), artinya pemain yg beli beberapa Block, main berjam-jam,
    // lalu keluar-masuk app -- KEHILANGAN SELURUH PROGRESS (block yg dibeli,
    // kondisi tiap pohon, stok TPH). Pakai format CSV-dalam-string (pola sama
    // dgn JNI/Kotlin CSV export) krn parser JSON kita sengaja minimal (tanpa
    // dependensi eksternal), tak sanggup uraikan array objek bersarang.
    // lastMarkDay/lastMarkKind (ikon "baru dikerjakan hari ini") SENGAJA tak
    // disimpan -- murni visual sesaat, wajar reset stlh reload.
    os << "\"treesCsv\":\"";
    for (const auto& t : trees_){
        os << t.id << "," << t.x << "," << t.z << "," << t.ageYears << "," << t.frond << ","
           << (int)t.ffb << "," << t.ffbTimer << "," << (int)t.health << "," << t.sickTimer << ","
           << t.nutrition << "," << (t.hasTbsReady?1:0) << "," << (t.tbsOverripe?1:0) << ","
           << t.ffbTimerMax << ";";
    }
    os << "\",";
    os << "\"blocksCsv\":\"";
    for (const auto& b : blocks_){
        os << b.id << "," << b.name << "," << b.afdelingId << "," << b.ha << ","
           << b.treeStartIdx << "," << b.treeEndIdx << "," << b.originX << "," << b.originZ << ","
           << b.soilFertility << "," << b.geneticVigor << "," << b.tphX << "," << b.tphZ << ","
           << b.tphStock << "," << b.tphStockOverripe << ";";
    }
    os << "\"";

    os << "}";
    return os.str();
}

// Parser JSON sangat minimal khusus format saveToJson() di atas (bukan parser umum).
// Untuk config.json yang lebih kompleks, lihat catatan di README (disarankan pakai
// nlohmann/json di proyek sesungguhnya; di sini sengaja tanpa dependensi eksternal
// karena sandbox tidak punya akses internet untuk mengunduh library).
bool Engine::loadFromJson(const std::string& json){
    auto findNum = [&](const std::string& key)->std::optional<double>{
        auto pos = json.find("\""+key+"\":");
        if (pos==std::string::npos) return std::nullopt;
        pos += key.size()+3;
        size_t end = json.find_first_of(",}", pos);
        try { return std::stod(json.substr(pos, end-pos)); } catch(...) { return std::nullopt; }
    };
    // findString: ambil NILAI STRING (di antara tanda kutip) suatu key --
    // dibutuhkan utk treesCsv/blocksCsv (findNum cuma sanggup ambil angka).
    auto findString = [&](const std::string& key)->std::optional<std::string>{
        auto pos = json.find("\""+key+"\":\"");
        if (pos==std::string::npos) return std::nullopt;
        pos += key.size()+4;
        size_t end = json.find("\"", pos);
        if (end==std::string::npos) return std::nullopt;
        return json.substr(pos, end-pos);
    };
    auto splitStr = [](const std::string& s, char delim)->std::vector<std::string>{
        std::vector<std::string> out;
        size_t start = 0;
        for (size_t i=0;i<=s.size();++i){
            if (i==s.size() || s[i]==delim){
                if (i>start) out.push_back(s.substr(start, i-start));
                start = i+1;
            }
        }
        return out;
    };

    if (auto v = findNum("money")) eco_.money = *v;
    if (auto v = findNum("day")) eco_.day = (int)*v;
    if (auto v = findNum("tphCap")) eco_.tphCap = *v;
    if (auto v = findNum("pricePerTandan")) eco_.pricePerTandan = *v;
    // HR & afdeling & pks: pola sama, disederhanakan (lihat engine_test.cpp untuk contoh pemakaian penuh)
    if (auto v = findNum("buruh")) { hr_.buruh = (int)*v; workers_.resize(std::max(1,(int)*v)); }
    if (auto v = findNum("mandor")) hr_.mandor = (int)*v;
    if (auto v = findNum("krani")) hr_.krani = (int)*v;
    if (auto v = findNum("mandorBesar")) hr_.mandorBesar = (int)*v;
    if (auto v = findNum("kraniKepala")) hr_.kraniKepala = (int)*v;
    if (auto v = findNum("asistenAfdeling")) hr_.asistenAfdeling = (int)*v;
    if (auto v = findNum("asistenKepala")) hr_.asistenKepala = (int)*v;
    if (auto v = findNum("manager")) hr_.manager = (int)*v;
    if (auto v = findNum("built")) pksState_.built = (*v!=0);
    if (auto v = findNum("level")) pksState_.level = (int)*v;
    if (auto v = findNum("oer")) pksState_.oer = *v;
    if (auto v = findNum("inputSilo")) pksState_.inputSilo = *v;

    // --- trees_ & blocks_ -- BUG BESAR diperbaiki (lihat catatan di
    // saveToJson): data ini REPLACE TOTAL isi trees_/blocks_ yg dibuat
    // newGame() (bukan overlay/tambah), krn ini rekonstruksi ULANG seluruh
    // struktur kebun sesuai save, bukan sekadar update angka ekonomi.
    if (auto treesCsv = findString("treesCsv")){
        std::vector<Tree> loaded;
        for (auto& row : splitStr(*treesCsv, ';')){
            auto p = splitStr(row, ',');
            if (p.size() < 12) continue;
            Tree t;
            try {
                t.id = std::stoi(p[0]); t.x = std::stod(p[1]); t.z = std::stod(p[2]);
                t.ageYears = std::stod(p[3]); t.frond = std::stod(p[4]);
                t.ffb = (FfbState)std::stoi(p[5]); t.ffbTimer = std::stod(p[6]);
                t.health = (HealthState)std::stoi(p[7]); t.sickTimer = std::stod(p[8]);
                t.nutrition = std::stod(p[9]); t.hasTbsReady = (p[10]=="1"); t.tbsOverripe = (p[11]=="1");
                // Backward compatibility -- save LAMA (format 12 kolom) tak
                // punya ffbTimerMax (field baru, fondasi countdown/progress
                // bar). Fallback: ffbTimerMax=ffbTimer saat dimuat (progress
                // 0% sementara sampai transisi state berikutnya terjadi --
                // JAUH lebih aman drpd crash/persentase salah tampil). Save
                // BARU (13 kolom, p[12] ada) baca nilai sesungguhnya.
                t.ffbTimerMax = (p.size() >= 13) ? std::stod(p[12]) : t.ffbTimer;
            } catch(...) { continue; }
            loaded.push_back(t);
        }
        if (!loaded.empty()) trees_ = std::move(loaded);
    }
    if (auto blocksCsv = findString("blocksCsv")){
        std::vector<Block> loaded;
        for (auto& row : splitStr(*blocksCsv, ';')){
            auto p = splitStr(row, ',');
            if (p.size() < 14) continue;
            Block b;
            try {
                b.id = std::stoi(p[0]); b.name = p[1]; b.afdelingId = std::stoi(p[2]); b.ha = std::stod(p[3]);
                b.treeStartIdx = std::stoi(p[4]); b.treeEndIdx = std::stoi(p[5]);
                b.originX = std::stod(p[6]); b.originZ = std::stod(p[7]);
                b.soilFertility = std::stod(p[8]); b.geneticVigor = std::stod(p[9]);
                b.tphX = std::stod(p[10]); b.tphZ = std::stod(p[11]);
                b.tphStock = std::stod(p[12]); b.tphStockOverripe = std::stod(p[13]);
            } catch(...) { continue; }
            loaded.push_back(b);
        }
        if (!loaded.empty()) blocks_ = std::move(loaded);
    }
    return true;
}

} // namespace sawit
