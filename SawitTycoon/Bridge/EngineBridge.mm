#import "EngineBridge.h"
#import "sawit/engine.hpp"
#import "renderer_gl.hpp"
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
- (void)kirimTruk { _engine.kirimTruk(); }

- (BOOL)beliHa:(double)amountHa { return _engine.beliHa(amountHa); }
- (BOOL)bukaAfdelingBaru { return _engine.bukaAfdelingBaru(); }
- (BOOL)rekrutLevel:(NSString *)key { return _engine.rekrutLevel(std::string(key.UTF8String)); }
- (BOOL)bangunPks { return _engine.bangunPks(); }
- (BOOL)upgradePks { return _engine.upgradePks(); }
- (BOOL)prosesBatchPks { return _engine.prosesBatchPks(); }

- (double)money { return _engine.economy().money; }
- (NSInteger)day { return _engine.economy().day; }
- (double)tphStock { return _engine.economy().tphStock; }
- (double)tphCap { return _engine.economy().tphCap; }
- (double)totalHa { return _engine.totalHa(); }
- (NSInteger)totalPokok { return _engine.totalPokok(); }
- (double)haPrice { return _engine.haPricePerUnit(); }
- (double)dailySalary { return _engine.totalDailySalary(); }
- (double)hrEfficiency { return _engine.hrEfficiency(); }
- (NSInteger)hrCount:(NSString *)key { return _engine.hr().countFor(std::string(key.UTF8String)); }
- (BOOL)pksBuilt { return _engine.pks().built; }
- (double)pksInputSilo { return _engine.pks().inputSilo; }
- (NSInteger)pksLevel { return _engine.pks().level; }
- (double)pksOer { return _engine.pks().oer; }

- (NSArray<SawitTreeView *> *)trees {
    NSMutableArray<SawitTreeView *> *out = [NSMutableArray array];
    for (const Tree &t : _engine.trees()) {
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
    return out;
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
        [out addObject:v];
    }
    return out;
}
- (NSInteger)blockIdForTree:(NSInteger)treeId { return _engine.blockIdForTree((int)treeId); }
- (void)devRandomizeConditions { _engine.devRandomizeConditions(); }
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

- (NSString *)saveJson { return [NSString stringWithUTF8String:_engine.saveToJson().c_str()]; }
- (BOOL)loadJson:(NSString *)json { return _engine.loadFromJson(std::string(json.UTF8String)); }

// --- render ---
- (void)glInit { sawit::gl::init(); }
- (void)glResizeWidth:(int)width height:(int)height { sawit::gl::resize(width, height); }
- (void)glSetCameraPanX:(float)panX panZ:(float)panZ dist:(float)dist yaw:(float)yaw { sawit::gl::setCamera(panX, panZ, dist, yaw); }
- (void)glDrawTreeInspectorAge:(float)ageYears frond:(float)frond health:(NSInteger)health ffb:(NSInteger)ffb hasTbsReady:(BOOL)hasTbsReady yawSpin:(float)yawSpin panY:(float)panY nutrition:(float)nutrition {
    sawit::gl::drawTreeInspectorFrame(ageYears, frond, (int)health, (int)ffb, hasTbsReady, yawSpin, panY, nutrition);
}
- (void)glDrawFrameSelectedTreeId:(NSInteger)selectedTreeId {
    sawit::gl::beginFrame();
    // Tanah digambar PER BLOCK (bukan sekali global spt dulu) -- block baru
    // dari beliHa() punya originX/Z sendiri, butuh tanahnya sendiri jg.
    for (const Block &b : _engine.blocks()) {
        sawit::gl::drawGround((float)b.originX, (float)b.originZ);
    }
    int today = _engine.economy().day;
    for (const Tree &t : _engine.trees()) {
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
    }
    // Pekerja digambar SETELAH pohon (celah yg diperbaiki -- sebelumnya tak pernah
    // digambar sama sekali meski sudah disimulasikan penuh di engine).
    for (const WorkerRenderInfo &w : _engine.workersRenderInfo()) {
        sawit::gl::drawWorker((float)w.x, (float)w.z, (int)w.pose, w.usingEgrek);
    }
    // Tumpukan TBS di TPH, jumlah tumpukan = jumlah tandan di stok (celah baru
    // diperbaiki -- sebelumnya TPH tak menunjukkan apa pun scr visual).
    int tphStock = (int)_engine.economy().tphStock;
    sawit::gl::drawTphPile((float)Engine::tphWorldX(), (float)Engine::tphWorldZ(), tphStock);
    // Truk keluar dari TPH menuju PKS (murni visual, lihat TruckState) --
    // celah baru diperbaiki, sebelumnya tak ada animasi sama sekali.
    if (_engine.truckActive()) {
        double p = _engine.truckProgress();
        double startX = Engine::tphWorldX(), startZ = Engine::tphWorldZ();
        double exitX = startX + 18.0, exitZ = startZ;
        float tx = (float)(startX + (exitX-startX)*p);
        float tz = (float)(startZ + (exitZ-startZ)*p);
        sawit::gl::drawTruck(tx, tz, 0.0f);
    }
    // Rumah kebun & figur staf (elemen lingkungan dari analisis referensi
    // visual "digital twin kebun sawit" yg diberikan pengguna) -- posisi
    // tetap di dekat TPH, warna seragam staf mengikuti jenjang SDM tertinggi.
    sawit::gl::drawFarmhouse(38.0f, 15.0f);
    {
        const HrState &hr = _engine.hr();
        int roleLevel = 0;
        if (hr.manager>0) roleLevel = 3;
        else if (hr.asistenKepala>0 || hr.asistenAfdeling>0) roleLevel = 2;
        else if (hr.mandor>0 || hr.krani>0 || hr.mandorBesar>0 || hr.kraniKepala>0) roleLevel = 1;
        sawit::gl::drawStaffFigure(35.0f, 12.0f, roleLevel);
    }
    // Portal/gerbang kebun -- elemen fisik yg blm ada, ditambahkan setelah
    // analisis referensi (praktik nyata stasiun penerimaan kebun sawit).
    sawit::gl::drawGate((float)Engine::gateWorldX(), (float)Engine::gateWorldZ(), 0.0f);
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

@end
