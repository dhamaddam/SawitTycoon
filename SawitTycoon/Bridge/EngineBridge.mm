#import "EngineBridge.h"
#import "sawit/engine.hpp"
#import "renderer_gl.hpp"
#include <cmath>
#include <algorithm>
#include <sstream>
#include <vector>
#include <string>

using namespace sawit;

// ---------------------------------------------------------------------------
@implementation SawitTreeView
- (BOOL)isMature { return self.ageYears >= 6.0f; } // TODO: idealnya baca dari GameConfig, lihat catatan README
@end

@implementation SawitBlockSummary
- (NSString *)statusEmoji {
    if (self.deadCount > 0 || self.ganodermaCount > self.treeCount/20) return @"🔴";
    if (self.hamaCount + self.ganodermaCount > 0) return @"🟠";
    if (self.readyToHarvestCount > 0) return @"🟡";
    return @"🟢";
}
- (NSString *)soilDesc {
    if (self.soilFertility > 1.10) return @"tanah sangat subur";
    if (self.soilFertility < 0.85) return @"tanah marjinal";
    return @"tanah sedang";
}
- (NSString *)genDesc {
    if (self.geneticVigor > 1.05) return @"benih unggul";
    if (self.geneticVigor < 0.92) return @"benih standar";
    return @"benih rata-rata";
}
@end

@implementation SawitHrLevelInfo
- (BOOL)recruitable { return self.prereqMet && self.underMax; }
@end

// ---------------------------------------------------------------------------
@interface EngineBridge () {
    Engine _engine;                          // objek C++ langsung sbg ivar — sah di file .mm (Obj-C++)
    std::vector<std::string> _pendingEvents;
}
@end

@implementation EngineBridge

- (instancetype)init {
    self = [super init];
    if (self) {
        // 'this' TIDAK valid di sini — ini method Objective-C, bukan method C++,
        // meskipun file-nya .mm. Tangkap alamat ivar C++ secara langsung supaya
        // lambda C++ di setEventSink tidak bergantung pada semantik Obj-C/ARC.
        std::vector<std::string> *pending = &_pendingEvents;
        _engine.setEventSink([pending](const EngineEvent &e) {
            std::ostringstream os;
            std::string text = e.text;
            for (auto &c : text) if (c == '|' || c == '\n') c = ' ';
            os << (int)e.type << "|" << text << "|" << e.treeId;
            pending->push_back(os.str());
        });
    }
    return self;
}

- (void)tick:(double)dt { _engine.tick(dt); }

- (BOOL)actionTunas:(NSInteger)treeId { return _engine.actionTunas((int)treeId); }
- (BOOL)actionPanen:(NSInteger)treeId { return _engine.actionPanen((int)treeId); }
- (BOOL)actionAngkut:(NSInteger)treeId { return _engine.actionAngkut((int)treeId); }
- (BOOL)actionPupuk:(NSInteger)treeId { return _engine.actionPupuk((int)treeId); }
- (BOOL)actionPestisida:(NSInteger)treeId { return _engine.actionPestisida((int)treeId); }
- (BOOL)actionFungisida:(NSInteger)treeId { return _engine.actionFungisida((int)treeId); }
- (BOOL)actionTebang:(NSInteger)treeId { return _engine.actionTebangTanamUlang((int)treeId); }
- (NSInteger)actionPanenSemua { return _engine.actionPanenSemua(); }
- (NSInteger)actionAngkutSemua { return _engine.actionAngkutSemua(); }
- (NSInteger)actionPupukSemua { return _engine.actionPupukSemua(); }
- (NSInteger)actionPestisidaSemua { return _engine.actionPestisidaSemua(); }
- (NSInteger)actionFungisidaSemua { return _engine.actionFungisidaSemua(); }
- (void)kirimTruk:(NSInteger)blockId { _engine.kirimTruk((int)blockId); }

- (BOOL)beliHa:(double)amountHa { return _engine.beliHa(amountHa); }
- (BOOL)bukaAfdelingBaru { return _engine.bukaAfdelingBaru(); }
- (BOOL)rekrutLevel:(NSString *)key { return _engine.rekrutLevel(std::string(key.UTF8String)); }
- (BOOL)bangunPks { return _engine.bangunPks(); }
- (BOOL)upgradePks { return _engine.upgradePks(); }
- (BOOL)upgradeTph { return _engine.upgradeTph(); }
- (double)tphUpgradeCost { return _engine.tphUpgradeCost(); }
- (NSInteger)tphLevel { return (NSInteger)_engine.economy().tphLevel; }
- (BOOL)prosesBatchPks { return _engine.prosesBatchPks(); }

- (double)money { return _engine.economy().money; }
- (NSInteger)day { return _engine.economy().day; }
- (double)tphStock:(NSInteger)blockId {
    const auto& blks = _engine.blocks();
    if (blockId < 0 || blockId >= (NSInteger)blks.size()) return 0.0;
    return blks[blockId].tphStock + blks[blockId].tphStockOverripe;
}
- (double)tphCap { return _engine.economy().tphCap; }
- (double)totalHa { return _engine.totalHa(); }
- (NSInteger)totalPokok { return _engine.totalPokok(); }
- (double)haPrice { return _engine.haPricePerUnit(); }
- (double)dailySalary { return _engine.totalDailySalary(); }
- (double)hrEfficiency { return _engine.hrEfficiency(); }
- (NSInteger)hrCount:(NSString *)key { return _engine.hr().countFor(std::string(key.UTF8String)); }
- (BOOL)pksBuilt { return _engine.pks().built; }
- (float)pksWorldX { return (float)(sawit::Engine::gateWorldX() + 10.0); }
- (float)pksWorldZ { return (float)sawit::Engine::gateWorldZ(); }
- (double)pksInputSilo { return _engine.pks().inputSilo; }
- (NSInteger)pksLevel { return _engine.pks().level; }
- (NSInteger)timeOfDay { return (NSInteger)_engine.timeOfDay(); }
- (BOOL)isRaining { return _engine.isRaining(); }
- (float)pksProcessPulse { return _engine.pksProcessPulse(); }
- (double)pksOer { return _engine.pks().oer; }
- (double)pksBuildCost { return _engine.pks().buildCost; }
- (double)pksUpgradeCostNow { return _engine.pksUpgradeCostNow(); }
- (NSInteger)pksCapacityPerBatch { return _engine.pksCapacityPerBatch(); }
- (double)pksCpoPrice { return _engine.pks().cpoPrice; }
- (double)pksPkPrice { return _engine.pks().pkPrice; }
- (double)pksKerRate { return _engine.pks().kerRate; }
- (double)pksAvgTandanKg { return _engine.pks().avgTandanKg; }

- (NSArray<SawitTreeView *> *)treesForBlock:(NSInteger)blockIndex {
    NSMutableArray<SawitTreeView *> *out = [NSMutableArray array];
    const auto& blocks = _engine.blocks();
    if (blockIndex >= 0 && blockIndex < (NSInteger)blocks.size()) {
        const auto& b = blocks[blockIndex];
        const auto& allTrees = _engine.trees();
        for (int i = b.treeStartIdx; i < b.treeEndIdx && i < (int)allTrees.size(); ++i) {
            const Tree &t = allTrees[i];
            SawitTreeView *v = [SawitTreeView new];
            v.treeId = t.id;
            v.x = (float)t.x;
            v.z = (float)t.z;
            v.ageYears = (float)t.ageYears;
            v.frond = (float)t.frond;
            v.ffb = (NSInteger)t.ffb;
            v.health = (NSInteger)t.health;
            v.hasTbsReady = t.hasTbsReady;
            v.nutrition = (float)t.nutrition;
            [out addObject:v];
        }
    }
    return out;
}
- (nullable SawitTreeView *)findTreeById:(NSInteger)treeId {
    for (const Tree &t : _engine.trees()) {
        if (t.id == (int)treeId) {
            SawitTreeView *v = [SawitTreeView new];
            v.treeId = t.id;
            v.x = (float)t.x;
            v.z = (float)t.z;
            v.ageYears = (float)t.ageYears;
            v.frond = (float)t.frond;
            v.ffb = (NSInteger)t.ffb;
            v.health = (NSInteger)t.health;
            v.hasTbsReady = t.hasTbsReady;
            v.nutrition = (float)t.nutrition;
            return v;
        }
    }
    return nil;
}
- (void)treeFfbProgress:(NSInteger)treeId outFfbTimer:(float *)outFfbTimer outFfbTimerMax:(float *)outFfbTimerMax {
    *outFfbTimer = 0.0f; *outFfbTimerMax = 0.0f;
    for (const Tree &t : _engine.trees()) {
        if (t.id == (int)treeId) {
            *outFfbTimer = (float)t.ffbTimer;
            *outFfbTimerMax = (float)t.ffbTimerMax;
            break;
        }
    }
}

- (NSArray<SawitBlockSummary *> *)blockSummaries {
    NSMutableArray<SawitBlockSummary *> *out = [NSMutableArray array];
    for (const BlockSummary &b : _engine.blockSummaries()) {
        SawitBlockSummary *v = [SawitBlockSummary new];
        v.blockId = b.id;
        v.name = [NSString stringWithUTF8String:b.name.c_str()];
        v.ha = (float)b.ha;
        v.treeCount = b.treeCount;
        v.healthyCount = b.healthyCount;
        v.hamaCount = b.hamaCount;
        v.ganodermaCount = b.ganodermaCount;
        v.deadCount = b.deadCount;
        v.readyToHarvestCount = b.readyToHarvestCount;
        v.tbsAwaitingPickupCount = b.tbsAwaitingPickupCount;
        v.originX = (float)b.originX;
        v.originZ = (float)b.originZ;
        v.soilFertility = (float)b.soilFertility;
        v.geneticVigor = (float)b.geneticVigor;
        v.lowNutritionCount = b.lowNutritionCount;
        v.tphStock = b.tphStock;
        v.tphStockOverripe = b.tphStockOverripe;
        [out addObject:v];
    }
    return out;
}
- (NSInteger)blockIdForTree:(NSInteger)treeId { return _engine.blockIdForTree((int)treeId); }
- (void)devRandomizeConditions { _engine.devRandomizeConditions(); }
- (void)setShowHarvestBeacon:(BOOL)show { sawit::gl::setShowHarvestBeacon(show); }
- (BOOL)showHarvestBeacon { return sawit::gl::getShowHarvestBeacon(); }
- (double)pricePupuk { return _engine.config().pricePupuk; }
- (double)pricePestisida { return _engine.config().pricePestisida; }
- (double)priceFungisida { return _engine.config().priceFungisida; }
- (NSArray<SawitHrLevelInfo *> *)hrLevelInfos {
    NSMutableArray<SawitHrLevelInfo *> *out = [NSMutableArray array];
    for (const HrLevelInfo &i : _engine.hrLevelInfos()) {
        SawitHrLevelInfo *v = [SawitHrLevelInfo new];
        v.key = [NSString stringWithUTF8String:i.key.c_str()];
        v.name = [NSString stringWithUTF8String:i.name.c_str()];
        v.icon = [NSString stringWithUTF8String:i.icon.c_str()];
        v.count = i.count;
        v.cost = i.cost;
        v.salary = i.salary;
        v.prereqMet = i.prereqMet;
        v.underMax = i.underMax;
        v.prereqDesc = [NSString stringWithUTF8String:i.prereqDesc.c_str()];
        v.desc = [NSString stringWithUTF8String:i.desc.c_str()];
        [out addObject:v];
    }
    return out;
}

- (NSArray<NSString *> *)pollEventsRaw {
    NSMutableArray<NSString *> *out = [NSMutableArray arrayWithCapacity:_pendingEvents.size()];
    for (auto &s : _pendingEvents) [out addObject:[NSString stringWithUTF8String:s.c_str()]];
    _pendingEvents.clear();
    return out;
}
- (NSInteger)activityLogCount { return _engine.activityLogCount(); }
- (NSString *)activityLogEntry:(NSInteger)indexFromNewest {
    return [NSString stringWithUTF8String:_engine.activityLogEntry((int)indexFromNewest).c_str()];
}
- (NSInteger)activityLogTreeId:(NSInteger)indexFromNewest {
    return _engine.activityLogTreeId((int)indexFromNewest);
}

- (NSString *)saveJson { return [NSString stringWithUTF8String:_engine.saveToJson().c_str()]; }
- (BOOL)loadJson:(NSString *)json { return _engine.loadFromJson(std::string(json.UTF8String)); }

// --- render ---
- (void)glInit { sawit::gl::init(); }
- (void)glResizeWidth:(int)width height:(int)height { sawit::gl::resize(width, height); }
- (void)glSetCameraPanX:(float)panX panZ:(float)panZ dist:(float)dist yaw:(float)yaw { sawit::gl::setCamera(panX, panZ, dist, yaw); }
// Avatar pemain (Gameplay Mode, third-person) -- hasil review eksternal
// poin #5. REDESAIN TOTAL (dilaporkan pengguna: "Joystick kiri harus 100%
// menjadi kontrol locomotion pekerja, bukan kontrol kamera... pola Roblox").
// Identik Android, lihat catatan lengkap di sawit_jni.cpp/engine.cpp.
// getCameraYawOffset() sekarang ORIENTASI ABSOLUT kamera (murni touch-drag),
// TIDAK PERNAH lagi direset/diserap oleh gerakan avatar.
- (void)movePlayerAvatarDirX:(float)dirX dirZ:(float)dirZ dt:(float)dt {
    float cameraYaw = sawit::gl::getCameraYawOffset();
    _engine.movePlayerAvatar((double)dirX, (double)dirZ, (double)dt, (double)cameraYaw);
}
- (void)setGameplayModeActive:(BOOL)active { sawit::gl::setGameplayModeActive(active); }
- (BOOL)getGameplayModeActive { return sawit::gl::getGameplayModeActive() ? YES : NO; }
- (void)adjustAvatarZoom:(float)delta { sawit::gl::adjustAvatarCamZoom(delta); }
// Kamera "lihat sekeliling" via touch-drag (poin #4 laporan).
- (void)adjustCameraYawOffset:(float)deltaRad { sawit::gl::adjustCameraYawOffset(deltaRad); }
- (void)adjustAvatarLookUpOffset:(float)deltaY { sawit::gl::adjustAvatarLookUpOffset(deltaY); }
- (void)setGraphicsQuality:(NSInteger)level { sawit::gl::setGraphicsQuality((int)level); }
- (NSInteger)getGraphicsQuality { return (NSInteger)sawit::gl::getGraphicsQuality(); }
- (void)setCameraSensitivity:(float)mult { sawit::gl::setCameraSensitivity(mult); }
- (float)getCameraSensitivity { return sawit::gl::getCameraSensitivity(); }
// Cari pohon TERDEKAT dari avatar dlm radius -- fondasi interaksi Gameplay
// Mode. Return -1 kalau tak ada. Identik Android.
- (NSInteger)nearestTreeToPlayerMaxDist:(float)maxDist {
    return (NSInteger)_engine.nearestTreeToPlayer((double)maxDist);
}
- (void)beginTreeInspectorTransition {
    sawit::gl::beginTreeInspectorTransition();
}
- (void)glDrawTreeInspectorAge:(float)ageYears frond:(float)frond health:(NSInteger)health ffb:(NSInteger)ffb hasTbsReady:(BOOL)hasTbsReady yawSpin:(float)yawSpin panY:(float)panY nutrition:(float)nutrition {
    sawit::gl::drawTreeInspectorFrame(ageYears, frond, (int)health, (int)ffb, hasTbsReady, yawSpin, panY, nutrition);
}
- (void)glDrawFrameSelectedTreeId:(NSInteger)selectedTreeId {
    // Gameplay Mode -- kamera HARUS di-update DULU (posisi avatar terkini)
    // SEBELUM beginFrame(), identik Android (lihat catatan lengkap di
    // sawit_jni.cpp).
    if (sawit::gl::getGameplayModeActive()) {
        const PlayerAvatarState &pa = _engine.playerAvatar();
        // Collision kamera thd pohon -- identik Android, lihat catatan
        // lengkap di sawit_jni.cpp. Sudut kamera VISUAL = ORIENTASI
        // ABSOLUT dari touch-drag bebas (poin #1: kamera terpisah dari
        // joystick/avatar) -- TIDAK LAGI ditambah pa.facingRad.
        float cameraYaw = sawit::gl::getCameraYawOffset();
        float desiredDist = sawit::gl::getAvatarCamDistBehind();
        float safeDist = (float)_engine.cameraSafeDistance(pa.x, pa.z, (double)cameraYaw, (double)desiredDist);
        sawit::gl::updatePlayerCamera((float)pa.x, (float)pa.z, (float)pa.facingRad, safeDist);
    }
    // Mekanisme musim/waktu -- identik Android, lihat catatan lengkap di
    // sawit_jni.cpp/renderer_gl.cpp.
    const EconomyState &eco = _engine.economy();
    float dayFrac = eco.dayLength > 0 ? (float)(eco.dayTimer / eco.dayLength) : 0.0f;
    bool raining = _engine.isRaining();
    sawit::gl::setSkyState(dayFrac, raining);
    sawit::gl::beginFrame();
    sawit::gl::drawSun(dayFrac);
    sawit::gl::drawDistantHills(dayFrac);
    // Estate View -- mode zoom-out lihat SEMUA block sekaligus. Representasi
    // SEDERHANA (ubin datar berwarna), BUKAN geometri pohon detail -- lihat
    // catatan lengkap di renderer_gl.cpp/sawit_jni.cpp (identik Android).
    if (sawit::gl::getEstateViewActive()) {
        int layer = sawit::gl::getEstateViewLayer();
        for (const BlockSummary &b : _engine.blockSummaries()) {
            float badFraction;
            if (layer == 0) {
                badFraction = b.treeCount>0 ? (float)(b.hamaCount+b.ganodermaCount+b.deadCount)/b.treeCount : 0.0f;
            } else if (layer == 1) {
                badFraction = b.treeCount>0 ? (float)b.lowNutritionCount/b.treeCount : 0.0f;
            } else {
                badFraction = b.treeCount>0 ? (float)b.readyToHarvestCount/b.treeCount : 0.0f;
            }
            float r,g,bl;
            sawit::gl::estateLayerColor(layer, badFraction, &r,&g,&bl);
            sawit::gl::drawEstateBlockTile((float)b.originX, (float)b.originZ, r,g,bl);
        }
        return;
    }
    // Tanah digambar PER BLOCK (bukan sekali global spt dulu) -- block baru
    // dari beliHa() punya originX/Z sendiri, butuh tanahnya sendiri jg.
    // Viewport culling -- BUG PERFORMA diperbaiki: dulu SEMUA block digambar
    // tiap frame tanpa peduli apakah terlihat di layar (dilaporkan pengguna:
    // >3-4 block, FPS anjlok ke ~8). Radius DINAIKKAN ke 75 (dari 50)
    // mengiringi perlebaran ground (90->120 unit, mengatasi bug "pola
    // bergerigi di horizon" -- dilaporkan pengguna via video, lihat catatan
    // lengkap di drawGround()) -- diagonal ground baru (~71 unit) LEBIH
    // BESAR dari radius lama (50), yg akan memotong ground prematur.
    for (const Block &b : _engine.blocks()) {
        if (!sawit::gl::isWorldPointVisible((float)b.originX, (float)b.originZ, 75.0f)) continue;
        sawit::gl::drawGround((float)b.originX, (float)b.originZ);
        // Tumpukan pelepah hasil tunas di gawangan MATI -- identik Android,
        // lihat catatan lengkap di deadRowZOffsets()/drawFrondPile().
        double gawanganLen = Engine::gawanganFullLength();
        for (double zOff : Engine::deadRowZOffsets()) {
            sawit::gl::drawFrondPile((float)b.originX, (float)(b.originZ+zOff), (float)gawanganLen);
        }
        // Visual jalan panen/inspeksi -- identik Android, lihat catatan
        // lengkap di livingRowZOffsets()/drawRoadStrip().
        for (double zOff : Engine::livingRowZOffsets()) {
            sawit::gl::drawRoadStrip((float)b.originX, (float)(b.originZ+zOff), (float)gawanganLen);
        }
    }
    int today = _engine.economy().day;
    // Kumpulkan dulu pohon yg LOLOS viewport culling, urutkan berdasar
    // KEDALAMAN thd kamera (BUKAN urutan array/ID/tanam) -- identik dgn
    // perbaikan Android, lihat catatan lengkap di sawit_jni.cpp & depthKeyForYaw().
    //
    // OPTIMASI 2-LEVEL ditambahkan -- identik Android (permintaan pengguna:
    // "frustum/visibility-based rendering"), lihat catatan lengkap di
    // sawit_jni.cpp. Filter BLOCK visible dulu (treeStartIdx/treeEndIdx),
    // cek pohon individual HANYA utk block yg lolos. BUG ditemukan &
    // diperbaiki SENDIRI (self-audit): radius filter block ini HARUS
    // SAMA persis dgn radius ground di atas (SEKARANG 75.0, dinaikkan
    // bersamaan mengiringi perlebaran ground) -- kalau beda, terverifikasi
    // numerik ada zona di mana ground di-skip tapi pohon tetap digambar
    // (pohon "melayang" tanpa tanah di bawahnya).
    std::vector<const Tree*> visibleTrees;
    const auto& allTrees = _engine.trees();
    for (const auto& b : _engine.blocks()) {
        if (!sawit::gl::isWorldPointVisible((float)b.originX, (float)b.originZ, 75.0f)) continue;
        for (int i = b.treeStartIdx; i < b.treeEndIdx && i < (int)allTrees.size(); ++i) {
            const Tree& t = allTrees[i];
            if (sawit::gl::isWorldPointVisible((float)t.x, (float)t.z)) visibleTrees.push_back(&t);
        }
    }
    std::sort(visibleTrees.begin(), visibleTrees.end(), [](const Tree* a, const Tree* b) {
        return sawit::gl::depthKeyForYaw((float)a->x, (float)a->z) < sawit::gl::depthKeyForYaw((float)b->x, (float)b->z);
    });
    // BUG "occlusion tak lengkap" diperbaiki (laporan pengguna item #14) --
    // identik Android, lihat catatan lengkap di sawit_jni.cpp. Blok
    // depth-test SEKARANG dimulai DI SINI (sebelum loop pohon), bukan lagi
    // setelahnya -- mencakup pohon+TBS pile+marker+beacon+worker+avatar+
    // PKS+TPH pile+truk+rumah+staff+gerbang SEKALIGUS dlm SATU depth
    // buffer bersama. Tanah (drawGround(), SEBELUM baris ini) TETAP di
    // LUAR blok ini, tak terpengaruh.
    sawit::gl::beginCharacterDepthBlock();
    for (const Tree* tp : visibleTrees) {
        const Tree &t = *tp;
        sawit::gl::drawPalm((float)t.x, (float)t.z, (float)t.ageYears, (float)t.frond,
                             (int)t.health, (int)t.ffb, (NSInteger)t.id == selectedTreeId, (float)t.nutrition);
        // TBS hasil panen tergeletak di dasar pohon, menunggu diangkut (celah
        // baru diperbaiki -- sebelumnya buah yg sudah dipanen "hilang" begitu
        // saja secara visual sampai worker mengangkutnya).
        if (t.hasTbsReady) sawit::gl::drawTbsPile((float)t.x, (float)t.z);
        // Tanda aksi massal HARI INI (celah baru diperbaiki -- sebelumnya tak
        // ada indikasi visual pohon mana yg sudah dipupuk/disemprot/dst).
        if (t.lastMarkDay == today) {
            float topY = sawit::gl::treeTrunkHeight((float)t.ageYears);
            sawit::gl::drawActionMarker((float)t.x, (float)t.z, topY, t.lastMarkKind);
        }
        // Beacon TBS matang -- mengatasi keluhan review eksternal: "TBS sulit
        // dibaca dari kejauhan krn pelepah menutupi". Toggleable ("layer" spt
        // di dokumen desain Estate/Block View) -- pemain bisa matikan kalau
        // dirasa mengganggu, default AKTIF.
        if (sawit::gl::getShowHarvestBeacon() && (int)t.health != 3 && ((int)t.ffb == 2 || (int)t.ffb == 3)) {
            float topY = sawit::gl::treeTrunkHeight((float)t.ageYears);
            sawit::gl::drawHarvestBeacon((float)t.x, (float)t.z, topY, (int)t.ffb == 3);
        }
    }
    // Pekerja digambar SETELAH pohon (celah yg diperbaiki -- sebelumnya tak pernah
    // digambar sama sekali meski sudah disimulasikan penuh di engine).
    // SEMUA pekerja NPC SEKARANG pakai model GLB baru (drawFarmerAvatar) --
    // identik Android, lihat catatan lengkap (peringatan performa,
    // penyederhanaan pose) di sawit_jni.cpp.
    // BUG performa BESAR diperbaiki -- identik Android, lihat catatan
    // lengkap di sawit_jni.cpp.
    // BUG "rumah menembus pekerja" diperbaiki (laporan pengguna #2) --
    // identik Android, lihat catatan lengkap di renderer_gl.cpp/sawit_jni.cpp.
    // beginCharacterDepthBlock() TAK dipanggil lagi di sini -- SUDAH dimulai
    // LEBIH AWAL (sebelum loop pohon di atas) supaya pohon jg ikut blok
    // depth-test yg sama (mengatasi celah occlusion item #14).
    for (const WorkerRenderInfo &w : _engine.workersRenderInfo()) {
        if (!sawit::gl::isWorldPointVisible((float)w.x, (float)w.z, 2.0f)) continue;
        bool workerMoving = (w.pose != WorkerPose::Idle);
        sawit::gl::drawFarmerAvatar((float)w.x, (float)w.z, (float)w.facingRad, workerMoving);
    }
    // Avatar PEMAIN (Gameplay Mode, third-person) -- SAMA model GLB dgn
    // pekerja NPC di atas (konsisten). Identik Android.
    if (sawit::gl::getGameplayModeActive()) {
        const PlayerAvatarState &pa = _engine.playerAvatar();
        sawit::gl::drawFarmerAvatar((float)pa.x, (float)pa.z, (float)pa.facingRad, pa.moving);
    }
    // Bangunan PKS -- mengatasi keluhan review: "tidak ada tampilan sama
    // sekali bangunan PKS di scene 3D". Posisi dekat gerbang (kGateX+10) --
    // searah dgn animasi truk keluar dari TPH yg sudah ada. Hanya digambar
    // kalau PKS sudah dibangun -- viewport culling JUGA diterapkan.
    if (_engine.pks().built) {
        float pksX = (float)Engine::gateWorldX() + 10.0f;
        float pksZ = (float)Engine::gateWorldZ();
        if (sawit::gl::isWorldPointVisible(pksX, pksZ, 12.0f)) {
            sawit::gl::drawPksBuilding(pksX, pksZ, _engine.pks().level, _engine.pksProcessPulse());
        }
    }
    // Tumpukan TBS di TPH -- SEKARANG per-block (celah diperbaiki: dulu 1
    // titik global tunggal, sekarang tiap block dapat tumpukannya sendiri
    // di posisi TPH-nya sendiri). Viewport culling ditambahkan (celah audit,
    // identik Android) -- radius 3.0 mencakup sebaran tumpukan.
    for (const Block &b : _engine.blocks()) {
        int stock = (int)(b.tphStock + b.tphStockOverripe);
        if (stock > 0 && sawit::gl::isWorldPointVisible((float)b.tphX, (float)b.tphZ, 3.0f)) sawit::gl::drawTphPile((float)b.tphX, (float)b.tphZ, stock);
    }
    // Truk keluar dari TPH block yg SEDANG diproses (posisi beda per block
    // sekarang, lihat truckBlockId()) menuju PKS (murni visual).
    if (_engine.truckActive()) {
        double p = _engine.truckProgress();
        int tbId = _engine.truckBlockId();
        double startX = Engine::tphWorldX(), startZ = Engine::tphWorldZ(); // fallback
        const auto& blks = _engine.blocks();
        if (tbId >= 0 && tbId < (int)blks.size()) { startX = blks[tbId].tphX; startZ = blks[tbId].tphZ; }
        // BUG diperbaiki (BELUM tersinkron dari Android sebelumnya):
        // exitX = startX+18.0 (RELATIF thd TPH block MASING-MASING) --
        // utk block selain pertama, ini membuat truk BERGERAK MENJAUH dari
        // gerbang/PKS (terverifikasi numerik: block ke-4 [originX=360]
        // selisih 355 unit dari gerbang, SEMAKIN JAUH seiring block
        // bertambah). Truk SEKARANG menuju POSISI PKS SESUNGGUHNYA
        // (gateWorldX()+10, PERSIS sama dgn drawPksBuilding() & Android) --
        // konsisten utk SEMUA block.
        double exitX = Engine::gateWorldX() + 10.0, exitZ = Engine::gateWorldZ();
        float tx = (float)(startX + (exitX-startX)*p);
        float tz = (float)(startZ + (exitZ-startZ)*p);
        // Arah hadap dihitung EKSPLISIT (bukan hardcode 0.0f) -- konsisten
        // dgn perbaikan arah pekerja & sisi Android (lihat catatan di
        // sawit_jni.cpp: WorkerRenderInfo.facingRad).
        float truckFacing = std::atan2((float)(exitZ-startZ), (float)(exitX-startX));
        // Viewport culling ditambahkan (celah audit, identik Android) --
        // radius 6.0 mencakup panjang truk+trailer.
        if (sawit::gl::isWorldPointVisible(tx, tz, 6.0f)) sawit::gl::drawTruck(tx, tz, truckFacing);
    }
    // Rumah kebun & figur staf (elemen lingkungan dari analisis referensi
    // visual "digital twin kebun sawit" yg diberikan pengguna) -- posisi
    // tetap di dekat TPH, warna seragam staf mengikuti jenjang SDM tertinggi.
    // Viewport culling ditambahkan utk KEDUANYA (celah audit, identik Android).
    if (sawit::gl::isWorldPointVisible(38.0f, 15.0f, 6.0f)) sawit::gl::drawFarmhouse(38.0f, 15.0f);
    {
        const HrState &hr = _engine.hr();
        int roleLevel = 0;
        if (hr.manager>0) roleLevel = 3;
        else if (hr.asistenKepala>0 || hr.asistenAfdeling>0) roleLevel = 2;
        else if (hr.mandor>0 || hr.krani>0 || hr.mandorBesar>0 || hr.kraniKepala>0) roleLevel = 1;
        if (sawit::gl::isWorldPointVisible(35.0f, 12.0f, 2.5f)) sawit::gl::drawStaffFigure(35.0f, 12.0f, roleLevel);
    }
    // Portal/gerbang kebun -- elemen fisik yg blm ada, ditambahkan setelah
    // analisis referensi (praktik nyata stasiun penerimaan kebun sawit).
    // Viewport culling ditambahkan (celah audit, identik Android).
    if (sawit::gl::isWorldPointVisible((float)Engine::gateWorldX(), (float)Engine::gateWorldZ(), 4.0f)) {
        sawit::gl::drawGate((float)Engine::gateWorldX(), (float)Engine::gateWorldZ(), 0.0f);
    }
    // Akhiri blok depth-test -- identik Android, lihat catatan lengkap di sawit_jni.cpp.
    sawit::gl::endCharacterDepthBlock();
    // Hujan digambar TERAKHIR -- identik Android, lihat catatan lengkap di sawit_jni.cpp.
    if (raining) sawit::gl::drawRainEffect();
    sawit::gl::endFrame();
}
- (void)screenToWorldX:(float)sx y:(float)sy outX:(float *)outX outZ:(float *)outZ {
    sawit::gl::screenToWorldOnGroundPlane(sx, sy, outX, outZ);
}
- (void)panWorldDeltaStartX:(float)startX startY:(float)startY endX:(float)endX endY:(float)endY outDx:(float *)outDx outDz:(float *)outDz {
    sawit::gl::panWorldDelta(startX, startY, endX, endY, outDx, outDz);
}
- (float)hitTestDistanceScreenX:(float)screenX screenY:(float)screenY treeX:(float)treeX treeZ:(float)treeZ ageYears:(float)ageYears {
    return sawit::gl::hitTestDistance(screenX, screenY, treeX, treeZ, ageYears);
}
- (void)worldToScreenX:(float)x z:(float)z outX:(float *)outX outY:(float *)outY {
    sawit::gl::worldToScreen(x, z, outX, outY);
}
- (void)setEstateViewModeActive:(BOOL)active layer:(NSInteger)layer {
    sawit::gl::setEstateViewMode(active, (int)layer);
}
- (BOOL)getEstateViewActive { return sawit::gl::getEstateViewActive(); }

@end
