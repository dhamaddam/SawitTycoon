#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// Representasi ringan satu pohon utk UI/render Swift.
@interface SawitTreeView : NSObject
@property (nonatomic) NSInteger treeId;
@property (nonatomic) float x;
@property (nonatomic) float z;
@property (nonatomic) float ageYears;
@property (nonatomic) float frond;
@property (nonatomic) NSInteger ffb;     // 0=none,1=growing,2=ripe,3=overripe
@property (nonatomic) NSInteger health;  // 0=sehat,1=hama,2=ganoderma,3=mati
@property (nonatomic) BOOL hasTbsReady;
@property (nonatomic) float nutrition;
@property (nonatomic, readonly) BOOL isMature; // >= ambang umur egrek
@end

/// Ringkasan status 1 Block (fondasi hierarki Kebun>Afdeling>Block>Baris>Pokok
/// utk Estate/Block View) -- agregat dihitung LIVE dari kondisi pohon saat ini.
@interface SawitBlockSummary : NSObject
@property (nonatomic) NSInteger blockId;
@property (nonatomic, copy) NSString *name;
@property (nonatomic) float ha;
@property (nonatomic) NSInteger treeCount;
@property (nonatomic) NSInteger healthyCount;
@property (nonatomic) NSInteger hamaCount;
@property (nonatomic) NSInteger ganodermaCount;
@property (nonatomic) NSInteger deadCount;
@property (nonatomic) NSInteger readyToHarvestCount;
@property (nonatomic) NSInteger tbsAwaitingPickupCount;
@property (nonatomic) float originX;
@property (nonatomic) float originZ;
/// Status ringkas utk Estate View ("normal/perhatian/masalah/kritis") --
/// ambang sederhana, bisa disesuaikan setelah UI-nya ada.
@property (nonatomic, readonly) NSString *statusEmoji;
@end

/// Ringkasan status LIVE 1 jenjang SDM -- fondasi dialog rekrut.
@interface SawitHrLevelInfo : NSObject
@property (nonatomic, copy) NSString *key;
@property (nonatomic, copy) NSString *name;
@property (nonatomic, copy) NSString *icon;
@property (nonatomic) NSInteger count;
@property (nonatomic) double cost;
@property (nonatomic) double salary;
@property (nonatomic) BOOL prereqMet;
@property (nonatomic) BOOL underMax;
@property (nonatomic, copy) NSString *prereqDesc;
@property (nonatomic, readonly) BOOL recruitable;
@end

/// Satu-satunya pintu ke C++ (Engine + renderer GL) dari sisi Swift.
/// Class ini murni Objective-C di permukaan (aman dipanggil dari Swift biasa);
/// implementasinya (.mm) yang berisi C++ (sawit::Engine).
@interface EngineBridge : NSObject

- (instancetype)init;

- (void)tick:(double)dt;

- (BOOL)actionTunas:(NSInteger)treeId;
- (BOOL)actionPanen:(NSInteger)treeId;
- (BOOL)actionAngkut:(NSInteger)treeId;
- (BOOL)actionPupuk:(NSInteger)treeId;
- (BOOL)actionPestisida:(NSInteger)treeId;
- (BOOL)actionFungisida:(NSInteger)treeId;
- (BOOL)actionTebang:(NSInteger)treeId;
/// Aksi MASSAL: kembalikan jumlah pohon yg berhasil mulai dikerjakan, dibatasi
/// jumlah pekerja bebas (lihat catatan literatur di engine.hpp).
- (NSInteger)actionPanenSemua;
- (NSInteger)actionAngkutSemua;
- (NSInteger)actionPupukSemua;
- (NSInteger)actionPestisidaSemua;
- (NSInteger)actionFungisidaSemua;
- (void)kirimTruk;

- (BOOL)beliHa:(double)amountHa;
- (BOOL)bukaAfdelingBaru;
- (BOOL)rekrutLevel:(NSString *)key;
- (BOOL)bangunPks;
- (BOOL)upgradePks;
- (BOOL)prosesBatchPks;

- (double)money;
- (NSInteger)day;
- (double)tphStock;
- (double)tphCap;
- (double)totalHa;
- (NSInteger)totalPokok;
- (double)haPrice;
- (double)dailySalary;
- (double)hrEfficiency;
- (NSInteger)hrCount:(NSString *)key;
- (BOOL)pksBuilt;
- (double)pksInputSilo;
- (NSInteger)pksLevel;
- (double)pksOer;

- (NSArray<SawitTreeView *> *)trees;
/// Fondasi hierarki Kebun>Afdeling>Block>Baris>Pokok -- API sudah siap, UI
/// Estate/Block View menyusul di sesi berikutnya.
- (NSArray<SawitBlockSummary *> *)blockSummaries;
- (NSInteger)blockIdForTree:(NSInteger)treeId;
/// Alat uji visual -- acak kondisi kebun tanpa perlu tunggu hari.
- (void)devRandomizeConditions;
/// Harga satuan aksi -- dipakai dialog konfirmasi estimasi biaya sebelum
/// aksi massal dieksekusi.
- (double)pricePupuk;
- (double)pricePestisida;
- (double)priceFungisida;
/// Fondasi dialog rekrut SDM -- rekrutLevel() sudah lama ada di engine tapi
/// tak pernah tersambung ke UI (dilaporkan pengguna: error "harus rekrut
/// asisten dulu" tp tombol rekrut tak ada sama sekali).
- (NSArray<SawitHrLevelInfo *> *)hrLevelInfos;
/// Setiap elemen berformat "type|text|treeId" (type: 0=Toast,1=FlyMoney,2=TreeChanged,3=HudChanged,4=ScreenChanged).
- (NSArray<NSString *> *)pollEventsRaw;
/// Log aktivitas PERMANEN (beda dgn pollEventsRaw yg sekali poll lalu hilang) --
/// utk layar "Log Aktivitas" yg bisa dibuka kapan saja. index 0 = terbaru.
- (NSInteger)activityLogCount;
- (NSString *)activityLogEntry:(NSInteger)indexFromNewest;

- (NSString *)saveJson;
- (BOOL)loadJson:(NSString *)json;

// --- render (panggil dari GLKView/GLKViewController, thread GL) ---
- (void)glInit;
- (void)glResizeWidth:(int)width height:(int)height;
- (void)glSetCameraPanX:(float)panX panZ:(float)panZ dist:(float)dist yaw:(float)yaw;
/// Inspector Pohon: render close-up 1 pohon berputar otomatis -- MENGGANTIKAN
/// pendekatan WKWebView/tree_detail.html sepenuhnya (tdk pakai HTML lagi).
/// Frame TERPISAH (beginFrame/endFrame sendiri) drpd glDrawFrameSelectedTreeId.
- (void)glDrawTreeInspectorAge:(float)ageYears frond:(float)frond health:(NSInteger)health ffb:(NSInteger)ffb hasTbsReady:(BOOL)hasTbsReady yawSpin:(float)yawSpin panY:(float)panY nutrition:(float)nutrition;
- (void)glDrawFrameSelectedTreeId:(NSInteger)selectedTreeId;
- (void)screenToWorldX:(float)sx y:(float)sy outX:(float *)outX outZ:(float *)outZ;
/// Pergeseran pan (dunia) yg benar dari titik layar awal->akhir 1 gesture drag —
/// otomatis ikut kemiringan & rotasi kamera saat ini (lihat renderer_gl.hpp).
- (void)panWorldDeltaStartX:(float)startX startY:(float)startY endX:(float)endX endY:(float)endY outDx:(float *)outDx outDz:(float *)outDz;
/// Hit-test satu pohon thd titik ketuk layar, mencakup seluruh tinggi & sebaran
/// mahkota (bukan cuma titik dasar) — mengembalikan jarak dlm PIKSEL LAYAR.
- (float)hitTestDistanceScreenX:(float)screenX screenY:(float)screenY treeX:(float)treeX treeZ:(float)treeZ ageYears:(float)ageYears;

@end

NS_ASSUME_NONNULL_END
