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
    eco_.tphStock = 0;
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
    generateBlockTrees_(0.0, 0.0, b0Start, b0End);

    // Kebun secara resmi jadi "Block A01" di bawah "Afdeling I" -- fondasi
    // hierarki Kebun>Afdeling>Block>Baris>Pokok. Block mereferensikan RENTANG
    // indeks di trees_ (bukan salinan data), spy tak ada duplikasi/drift.
    // "Block terdiri dari banyak baris tanaman + infrastruktur lokal, BUKAN
    // 1 baris = 1 block" -- keputusan desain, ukuran (ha) bukan klaim dari
    // Corley & Tinker (buku tak menetapkan 1 ukuran block universal).
    blocks_.clear();
    blocks_.push_back(Block{0, "A01", 0, land_.sampleBlockHa, b0Start, b0End, 0.0, 0.0});

    simAccum_ = 0;
    tphAutoTimer_ = 26;
    activityLog_.clear();
}

void Engine::generateBlockTrees_(double originX, double originZ, int& outStartIdx, int& outEndIdx){
    outStartIdx = (int)trees_.size();
    int id = outStartIdx;
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
            t.health = HealthState::Sehat;
            t.sickTimer = 0;
            t.nutrition = 0.6 + randUnit_()*0.3;
            t.hasTbsReady = false;
            trees_.push_back(t);
        }
    }
    outEndIdx = (int)trees_.size();
}

void Engine::emit(EventType t, const std::string& text, int treeId){
    if (events_) events_(EngineEvent{t, text, treeId});
    // Catat ke log PERMANEN hanya utk event yg bermakna sbg "aktivitas" (Toast &
    // FlyMoney) -- TreeChanged/HudChanged/ScreenChanged cuma sinyal teknis "perlu
    // refresh UI", bukan sesuatu yg relevan ditampilkan sbg riwayat aktivitas.
    if (!text.empty() && (t == EventType::Toast || t == EventType::FlyMoney)) {
        activityLog_.push_back(LogEntry{eco_.day, text});
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

std::vector<WorkerRenderInfo> Engine::workersRenderInfo() const {
    std::vector<WorkerRenderInfo> out;
    out.reserve(workers_.size());
    for (auto& w : workers_) {
        WorkerRenderInfo info;
        info.x = w.x; info.z = w.z; info.busy = w.busy; info.carrying = w.carrying;
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
        truck_.progress += dt / kTruckDurationSec;
        if (truck_.progress >= 1.0){ truck_.active = false; truck_.progress = 0.0; }
    }

    eco_.dayTimer += dt;
    if (eco_.dayTimer >= eco_.dayLength){
        eco_.dayTimer = 0;
        eco_.day++;
        onNewDay_();
        emit(EventType::HudChanged);
    }

    simAccum_ += dt;
    while (simAccum_ >= 1.0){
        simAccum_ -= 1.0;
        simTick_(1.0);
    }

    tphAutoTimer_ -= dt;
    if (tphAutoTimer_ <= 0){
        tphAutoTimer_ = 26;
        if (eco_.tphStock > 0 || eco_.tphStockOverripe > 0) kirimTruk();
    }
}

void Engine::simTick_(double dt){
    (void)dt; // dipanggil selalu dengan 1.0 dari tick(), lihat while(simAccum_>=1.0) di atas
    for (auto& t : trees_){
        if (t.health == HealthState::Mati) continue;

        // pematangan TBS
        if (t.ffb == FfbState::None){
            t.ffbTimer -= 1.0;
            if (t.ffbTimer <= 0){ t.ffb = FfbState::Growing; t.ffbTimer = 10 + randUnit_()*8*(1.3-t.nutrition); emit(EventType::TreeChanged,"",t.id); }
        } else if (t.ffb == FfbState::Growing){
            t.ffbTimer -= 1.0;
            if (t.ffbTimer <= 0){ t.ffb = FfbState::Ripe; t.ffbTimer = 12 + randUnit_()*10; emit(EventType::TreeChanged,"",t.id); }
        } else if (t.ffb == FfbState::Ripe){
            t.ffbTimer -= 1.0;
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
}

// ---------------------------------------------------------------------------
// WORKER JOB ENGINE
// ---------------------------------------------------------------------------
int Engine::findFreeWorker_() const {
    for (size_t i=0;i<workers_.size();++i) if (!workers_[i].busy) return (int)i;
    return -1;
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
    w.totalDuration = w.walkDuration + taskDuration(kind); // waktu KERJA di tempat, DITAMBAH (bukan dibagi dari) waktu jalan
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
            job.targetX = kTphX; job.targetZ = kTphZ;
            {
                computeCorridorPath_(job.startX, job.startZ, job.targetX, job.targetZ, job.treeId,
                                      &job.wp1X, &job.wp1Z, &job.wp2X, &job.wp2Z);
                double seg1 = std::sqrt((job.wp1X-job.startX)*(job.wp1X-job.startX) + (job.wp1Z-job.startZ)*(job.wp1Z-job.startZ));
                double seg2 = std::sqrt((job.wp2X-job.wp1X)*(job.wp2X-job.wp1X) + (job.wp2Z-job.wp1Z)*(job.wp2Z-job.wp1Z));
                double seg3 = std::sqrt((job.targetX-job.wp2X)*(job.targetX-job.wp2X) + (job.targetZ-job.wp2Z)*(job.targetZ-job.wp2Z));
                double dist = seg1+seg2+seg3; // jarak TOTAL jalur koridor ke TPH (tepi kebun)
                job.walkDuration = std::max(0.4, dist / kWorkerWalkSpeed);
            }
            job.totalDuration = job.walkDuration; // fase angkut->TPH murni perjalanan, tak ada "kerja diam"
            job.remaining = job.totalDuration;
            job.carrying = true; // mulai bawa TBS scr visual
            return; // masih busy, lanjut fase berikutnya
        } else {
            job.carrying = false;
            bool overripe = t && t->tbsOverripe;
            double combinedStock = eco_.tphStock + eco_.tphStockOverripe;
            if (combinedStock < eco_.tphCap){
                if (overripe) eco_.tphStockOverripe += 1;
                else eco_.tphStock += 1;
            }
            emit(EventType::Toast, overripe ? "+1 tandan LEWAT MATANG di TPH (harga lbh rendah)" : "+1 tandan di TPH");
            emit(EventType::HudChanged);
            job.busy=false; job.kind.clear(); job.treeId=-1; job.phase.clear();
            return;
        }
    }

    if (t){
        if (kind=="tunas"){ t->frond = 0.08; emit(EventType::Toast,"Pelepah ditunas",t->id); }
        else if (kind=="panen"){ t->tbsOverripe = (t->ffb==FfbState::Overripe); t->ffb=FfbState::None; t->ffbTimer=14+randUnit_()*14; t->hasTbsReady=true; emit(EventType::Toast,"TBS dipanen!",t->id); }
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
            t->nutrition = 0.6+randUnit_()*0.3;
            emit(EventType::Toast,"Ditanam ulang",t->id);
        }
        emit(EventType::TreeChanged,"",t->id);
    }
    emit(EventType::HudChanged);
    job.busy=false; job.kind.clear(); job.treeId=-1; job.phase.clear();

    // pekerja tambahan (indeks > 0) otomatis cari kerjaan berikutnya
    if (autoMode_){
        for (size_t i=1;i<workers_.size();++i) if (!workers_[i].busy){ autoAssign_((int)i); }
    }
}

void Engine::autoAssign_(int workerIdx){
    if (workerIdx<0 || workerIdx>=(int)workers_.size() || workers_[workerIdx].busy) return;
    // prioritas: angkut TBS siap -> panen matang -> obati Ganoderma -> semprot hama -> tunas menumpuk
    for (auto& t : trees_) if (t.hasTbsReady){ actionAngkut(t.id); return; }
    for (auto& t : trees_) if ((t.ffb==FfbState::Ripe||t.ffb==FfbState::Overripe) && t.health!=HealthState::Mati){ actionPanen(t.id); return; }
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
    int wi = findFreeWorker_();
    if (wi<0){ emit(EventType::Toast,"Semua pekerja sedang bertugas"); return false; }
    startJob_(wi, "tunas", treeId, t->x, t->z);
    return true;
}
bool Engine::actionPanen(int treeId){
    Tree* t = treeById(treeId);
    if (!t || !((t->ffb==FfbState::Ripe||t->ffb==FfbState::Overripe) && t->health!=HealthState::Mati)) return false;
    int wi = findFreeWorker_();
    if (wi<0){ emit(EventType::Toast,"Semua pekerja sedang bertugas"); return false; }
    startJob_(wi, "panen", treeId, t->x, t->z);
    return true;
}
bool Engine::actionAngkut(int treeId){
    Tree* t = treeById(treeId);
    if (!t || !t->hasTbsReady) return false;
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
    for (auto& t : trees_){
        if (!((t.ffb==FfbState::Ripe||t.ffb==FfbState::Overripe) && t.health!=HealthState::Mati)) continue;
        t.tbsOverripe = (t.ffb==FfbState::Overripe); // celah diperbaiki: dulu tak ditandai sama sekali
                                                       // di jalur aksi massal (beda dr jalur single-tree)
        t.ffb = FfbState::None; t.ffbTimer = 14 + randUnit_()*14; t.hasTbsReady = true;
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
    for (auto& t : trees_){
        if ((eco_.tphStock+eco_.tphStockOverripe) >= eco_.tphCap) break; // TPH penuh, sisanya nunggu truk
        if (!t.hasTbsReady) continue;
        t.hasTbsReady = false;
        if (t.tbsOverripe) eco_.tphStockOverripe += 1; else eco_.tphStock += 1;
        t.lastMarkDay = eco_.day; t.lastMarkKind = 1;
        emit(EventType::TreeChanged,"",t.id);
        n++;
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

void Engine::kirimTruk(){
    double normal = eco_.tphStock;
    double overripe = eco_.tphStockOverripe;
    double amount = normal + overripe;
    eco_.tphStock = 0;
    eco_.tphStockOverripe = 0;
    if (amount > 0){
        if (pksState_.built){
            pksState_.inputSilo += amount;
            emit(EventType::Toast, std::to_string((long long)amount)+" tandan masuk ke silo PKS");
        } else {
            // TBS lewat-matang dijual dgn DISKON -- Corley & Tinker (2016) "The
            // Oil Palm" 5th ed. §11.5.5.1: tandan yg terlewat panen FFA-nya
            // sudah mulai naik, mutu turun (lihat kOverripeDiscount, engine.hpp).
            double income = normal*eco_.pricePerTandan + overripe*eco_.pricePerTandan*kOverripeDiscount;
            eco_.money += income;
            emit(EventType::FlyMoney, "+Rp "+std::to_string((long long)income));
        }
        emit(EventType::HudChanged);
        // Aktifkan animasi truk (murni visual, transaksi uang di atas sudah SELESAI
        // instan -- lihat catatan di TruckState, types.hpp).
        truck_.active = true; truck_.progress = 0.0;
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
        generateBlockTrees_(newOriginX, 0.0, ts, te);
        int newId = (int)blocks_.size();
        std::string blockName = "A" + std::string(blocks_.size()+1 < 10 ? "0" : "") + std::to_string((int)blocks_.size()+1);
        blocks_.push_back(Block{newId, blockName, last.id, 1.0, ts, te, newOriginX, 0.0});
        land_.sampleBlockHa += 1.0; // kecualikan dr formula hasil abstrak -- block ini skrg produksi via simulasi pohon sungguhan, bukan formula lagi
        emit(EventType::Toast, blockName+" dibuka dengan 143 pokok baru!");
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
    else { hr_.add(key,1); }
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
    os << "\"tphStock\":" << eco_.tphStock << ",";
    os << "\"tphStockOverripe\":" << eco_.tphStockOverripe << ",";
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
    os << "\"pks\":{\"built\":" << (pksState_.built?"true":"false") << ",\"level\":" << pksState_.level
       << ",\"oer\":" << pksState_.oer << ",\"inputSilo\":" << pksState_.inputSilo << "}";
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
    if (auto v = findNum("money")) eco_.money = *v;
    if (auto v = findNum("day")) eco_.day = (int)*v;
    if (auto v = findNum("tphStock")) eco_.tphStock = *v;
    if (auto v = findNum("tphStockOverripe")) eco_.tphStockOverripe = *v;
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
    return true;
}

} // namespace sawit
