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
@property (nonatomic) float soilFertility;
@property (nonatomic) float geneticVigor;
@property (nonatomic) NSInteger lowNutritionCount;
/// BUG diperbaiki -- field ini sudah lama ada di C++ BlockSummary (sejak TPH
/// per-block) tapi tak pernah diekspos ke sini, persis pola bug yg sama
/// ditemukan & diperbaiki di sisi Kotlin (BlockSummaryView).
@property (nonatomic) double tphStock;
@property (nonatomic) double tphStockOverripe;
/// Deskripsi kualitatif -- Corley & Tinker (2016) §9.2.3.5 (soil fertility) & bab 6 (D x P seed source).
@property (nonatomic, readonly) NSString *soilDesc;
@property (nonatomic, readonly) NSString *genDesc;
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
/// BUG diperbaiki -- deskripsi tugas literatur-grounded sudah lama ada di
/// HrLevelInfo C++, tak pernah diekspos ke sini, persis pola bug yg sama
/// ditemukan & diperbaiki di sisi Kotlin (HrLevelInfoView).
@property (nonatomic, copy) NSString *desc;
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
- (void)kirimTruk:(NSInteger)blockId;

- (BOOL)beliHa:(double)amountHa;
- (BOOL)bukaAfdelingBaru;
- (BOOL)rekrutLevel:(NSString *)key;
- (BOOL)bangunPks;
- (BOOL)upgradePks;
// Upgrade kapasitas TPH -- fitur baru diminta pengguna. Lihat catatan lengkap di engine.cpp.
- (BOOL)upgradeTph;
- (double)tphUpgradeCost;
- (NSInteger)tphLevel;
- (BOOL)prosesBatchPks;

- (double)money;
- (NSInteger)day;
- (double)tphStock:(NSInteger)blockId;
- (double)tphCap;
- (double)totalHa;
- (NSInteger)totalPokok;
- (double)haPrice;
- (double)dailySalary;
- (double)hrEfficiency;
- (NSInteger)hrCount:(NSString *)key;
- (BOOL)pksBuilt;
// Posisi dunia PKS -- fondasi tombol "Lihat PKS" (navigasi kamera). Identik Android, lihat catatan lengkap di sawit_jni.cpp.
- (float)pksWorldX;
- (float)pksWorldZ;
- (double)pksInputSilo;
- (NSInteger)pksLevel;
- (NSInteger)timeOfDay; // 0=Pagi,1=Siang,2=Malam
- (BOOL)isRaining;
- (float)pksProcessPulse;
- (double)pksOer;
- (double)pksBuildCost;
- (double)pksUpgradeCostNow;
// Audit menemukan: fungsi2 ini BELUM PERNAH ADA di iOS sama sekali --
// kapasitas batch (bug #10 "TPH-PKS mismatch" blm pernah diport dr Android),
// & komponen harga/rendemen (estimasi pendapatan tak bisa ditampilkan).
- (NSInteger)pksCapacityPerBatch;
- (double)pksCpoPrice;
- (double)pksPkPrice;
- (double)pksKerRate;
- (double)pksAvgTandanKg;

// Countdown/progress bar panen per pohon -- fitur baru diminta pengguna:
// "countdown panen sawit / progress bar pada setiap pohon... tampiilkan
// progressnya hanya ketika pohon tersebut di klik". Identik Android
// (nativeGetTreeFfbProgress) -- fungsi TERPISAH (bukan field baru di
// SawitTreeView, yg akan dipanggil massal tiap frame utk SEMUA pohon).
// Dipanggil HANYA saat dialog/radial menu pohon dibuka. outFfbTimer=sisa
// detik, outFfbTimerMax=total detik tahap ini.
- (void)treeFfbProgress:(NSInteger)treeId outFfbTimer:(float *)outFfbTimer outFfbTimerMax:(float *)outFfbTimerMax;
/// Fondasi hierarki Kebun>Afdeling>Block>Baris>Pokok -- API sudah siap, UI
/// Estate/Block View menyusul di sesi berikutnya.
- (NSArray<SawitBlockSummary *> *)blockSummaries;
- (NSInteger)blockIdForTree:(NSInteger)treeId;
// BUG performa BESAR diperbaiki (dilaporkan pengguna: "sistem hampir tidak
// bisa digunakan jika memliki kebun di atas 30 ha") -- identik Android,
// lihat catatan lengkap di sawit_jni.cpp. Sekarang per-block (parameter
// blockIndex baru), kompleksitas O(143) TETAP KONSTAN terlepas berapa ha
// dimiliki, bukan O(total_pohon_dimiliki) spt sebelumnya.
- (NSArray<SawitTreeView *> *)treesForBlock:(NSInteger)blockIndex NS_SWIFT_NAME(trees(forBlock:));
// Cari 1 pohon spesifik LINTAS SEMUA block -- utk selectAndJumpToTree()
// (dipicu dari Log overlay, bisa mencatat kejadian pohon di block manapun).
- (nullable SawitTreeView *)findTreeById:(NSInteger)treeId NS_SWIFT_NAME(findTreeById(_:));
/// Alat uji visual -- acak kondisi kebun tanpa perlu tunggu hari.
- (void)devRandomizeConditions;
/// Toggle layer beacon TBS matang -- variabel SHARED renderer_gl.cpp, sama
/// persis dgn sisi Android (bukan variabel independen yg bisa tak sinkron).
- (void)setShowHarvestBeacon:(BOOL)show;
- (BOOL)showHarvestBeacon;
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
/// treeId terkait 1 entri log (-1 = tak terkait pohon) -- fondasi navigasi
/// "ketuk log utk lompat ke pohon".
- (NSInteger)activityLogTreeId:(NSInteger)indexFromNewest;

- (NSString *)saveJson;
- (BOOL)loadJson:(NSString *)json;

// --- render (panggil dari GLKView/GLKViewController, thread GL) ---
- (void)glInit;
- (void)glResizeWidth:(int)width height:(int)height;
- (void)glSetCameraPanX:(float)panX panZ:(float)panZ dist:(float)dist yaw:(float)yaw;
// Avatar pemain (Gameplay Mode, third-person) -- hasil review eksternal
// poin #5. Lihat catatan lengkap di EngineBridge.mm.
- (void)movePlayerAvatarDirX:(float)dirX dirZ:(float)dirZ dt:(float)dt;
- (void)setGameplayModeActive:(BOOL)active;
- (BOOL)getGameplayModeActive;
- (void)adjustAvatarZoom:(float)delta;
// Kamera "lihat sekeliling" via touch-drag (poin #4 laporan).
- (void)adjustCameraYawOffset:(float)deltaRad;
// Mendongak "lihat ke atas" -- fitur baru diminta pengguna: "tidak bisa
// melihat lebih ke atas pohon sawit, berikan lebih jauh sudut pandang
// hanya untuk melihat ke atas tidak untuk horizontal". Lihat catatan
// lengkap di renderer_gl.cpp.
- (void)adjustAvatarLookUpOffset:(float)deltaY;
// Pengaturan grafik & sensitivitas kamera -- fitur baru diminta pengguna
// ("tambahkan pengaturan sensivitas dan grafik"). Identik Android, lihat
// catatan lengkap di renderer_gl.cpp/sawit_jni.cpp.
- (void)setGraphicsQuality:(NSInteger)level; // 0=Rendah, 1=Sedang, 2=Tinggi
- (NSInteger)getGraphicsQuality;
- (void)setCameraSensitivity:(float)mult; // 0.5-2.0, default 1.0
- (float)getCameraSensitivity;
// Cari pohon TERDEKAT dari avatar dlm radius -- fondasi interaksi Gameplay
// Mode. Return -1 kalau tak ada.
- (NSInteger)nearestTreeToPlayerMaxDist:(float)maxDist;
/// Inspector Pohon: render close-up 1 pohon berputar otomatis -- MENGGANTIKAN
/// pendekatan WKWebView/tree_detail.html sepenuhnya (tdk pakai HTML lagi).
/// Frame TERPISAH (beginFrame/endFrame sendiri) drpd glDrawFrameSelectedTreeId.
// Mulai transisi kamera smooth saat inspector BARU dibuka -- lihat catatan lengkap di renderer_gl.cpp/sawit_jni.cpp.
- (void)beginTreeInspectorTransition;
- (void)glDrawTreeInspectorAge:(float)ageYears frond:(float)frond health:(NSInteger)health ffb:(NSInteger)ffb hasTbsReady:(BOOL)hasTbsReady yawSpin:(float)yawSpin panY:(float)panY nutrition:(float)nutrition;
- (void)glDrawFrameSelectedTreeId:(NSInteger)selectedTreeId;
- (void)screenToWorldX:(float)sx y:(float)sy outX:(float *)outX outZ:(float *)outZ;
/// Pergeseran pan (dunia) yg benar dari titik layar awal->akhir 1 gesture drag —
/// otomatis ikut kemiringan & rotasi kamera saat ini (lihat renderer_gl.hpp).
- (void)panWorldDeltaStartX:(float)startX startY:(float)startY endX:(float)endX endY:(float)endY outDx:(float *)outDx outDz:(float *)outDz;
/// Hit-test satu pohon thd titik ketuk layar, mencakup seluruh tinggi & sebaran
/// mahkota (bukan cuma titik dasar) — mengembalikan jarak dlm PIKSEL LAYAR.
- (float)hitTestDistanceScreenX:(float)screenX screenY:(float)screenY treeX:(float)treeX treeZ:(float)treeZ ageYears:(float)ageYears;
// Estate View -- mode zoom-out lihat semua block, dan worldToScreen dibutuhkan
// hit-test ubin block (bukan pohon, lihat catatan lengkap di renderer_gl.cpp).
- (void)worldToScreenX:(float)x z:(float)z outX:(float *)outX outY:(float *)outY;
- (void)setEstateViewModeActive:(BOOL)active layer:(NSInteger)layer;
- (BOOL)getEstateViewActive;

@end

NS_ASSUME_NONNULL_END
