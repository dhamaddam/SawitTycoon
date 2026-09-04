#pragma once
// ============================================================================
// Renderer OpenGL ES 2.0 — kebun isometrik sederhana (ground + pohon prosedural).
// File ini dipakai BERSAMA oleh Android (via CMake, GLES2/gl2.h) dan iOS
// (ditambahkan langsung ke target Xcode, OpenGLES/ES2/gl.h) — satu kode render,
// bukan disalin dua kali, lihat renderer_gl.cpp untuk percabangan platform.
//
// CATATAN JUJUR: file ini TIDAK dikompilasi di sandbox pengembangan (hanya diuji
// sintaksnya dengan header tiruan, tidak ada NDK/Xcode asli di sini). Ditulis
// mengikuti spesifikasi resmi OpenGL ES 2.0. Kompilasi sesungguhnya baru terjadi
// saat kamu build lewat Android Studio / Xcode — kalau ada error compile, kirim
// pesan errornya, saya perbaiki.
// ============================================================================

namespace sawit::gl {

void init();                          // panggil sekali dari GLSurfaceView.Renderer.onSurfaceCreated
void resize(int width, int height);   // panggil dari onSurfaceChanged
// yaw = rotasi kamera sekitar sumbu vertikal (radian), utk lihat 360°. Nilai
// awal isometrik klasik = 0.7854 (45°), sama spt sebelum fitur rotasi ada.
void setCamera(float panX, float panZ, float distance, float yaw);
// Warna langit dinamis (siang/malam/hujan) -- fitur baru diminta pengguna.
// Lihat catatan lengkap di renderer_gl.cpp. dayFrac: 0.0-1.0 (dari
// eco_.dayTimer/dayLength engine), dipanggil SETIAP FRAME sebelum beginFrame().
void setSkyState(float dayFrac, bool isRaining);
// Kamera Gameplay Mode (third-person, avatar bisa digerakkan) -- dipanggil
// tiap frame SEBELUM beginFrame() dgn posisi avatar terkini. effectiveDistBehind
// dari engine.cameraSafeDistance() (collision terhadap pohon) -- lihat
// catatan lengkap di renderer_gl.cpp.
void updatePlayerCamera(float playerX, float playerZ, float playerFacingRad, float effectiveDistBehind);
// Baca jarak zoom SAAT INI -- dipakai sbg parameter engine.cameraSafeDistance()
// sebelum updatePlayerCamera() dipanggil.
float getAvatarCamDistBehind();
// Kamera "lihat sekeliling" via touch-drag (poin #4 laporan pengguna) --
// lihat catatan lengkap di renderer_gl.cpp.
void adjustCameraYawOffset(float deltaRad);
float getCameraYawOffset();
// Mendongak "lihat ke atas" -- fitur baru diminta pengguna. Lihat catatan lengkap di renderer_gl.cpp.
void adjustAvatarLookUpOffset(float deltaY);
float getAvatarLookUpOffset();
float getAvatarCamEyeY();
float getAvatarLookAtY();
void resetCameraYawOffset();

void beginFrame();
// Efek visual hujan -- fitur baru diminta pengguna. Lihat catatan lengkap di renderer_gl.cpp.
void drawRainEffect();
// Matahari (terlihat saat siang) -- fitur baru diminta pengguna. Lihat catatan lengkap di renderer_gl.cpp.
void drawSun(float dayFrac);
// Latar belakang perbukitan bergelombang -- fitur baru diminta pengguna. Lihat catatan lengkap di renderer_gl.cpp.
void drawDistantHills(float dayFrac);
// originX/originZ: pusat grid block yg digambar tanahnya -- panggil SEKALI
// per block (pemanggil/JNI/bridge iterasi semua block), bukan sekali global.
void drawGround(float originX, float originZ);
// health: 0=sehat,1=hama,2=ganoderma,3=mati ; ffb: 0=none,1=growing,2=ripe,3=overripe
// nutrition (0..1): mempengaruhi KERAPATAN kanopi (proxy visual defisiensi
// N/K/air) -- review eksternal "visualisasi state biologis": pohon jadi
// indikator gameplay yg bisa dibaca dari kejauhan, bukan sekadar dekorasi.
void drawPalm(float x, float z, float ageYears, float frond, int health, int ffb, bool selected, float nutrition);
// Karakter pekerja sederhana (badan+kepala+topi, gaya sama dgn pohon) — SEBELUMNYA
// TIDAK ADA sama sekali (pekerja disimulasikan di engine tapi tak pernah digambar).
// carrying=true menampilkan gumpalan TBS kecil di tangan (visual fase angkut->TPH).
// poseCode sesuai enum sawit::WorkerPose: 0=Idle,1=Kneel,2=Tool,3=Reach,4=Carry
// (lihat types.hpp) — menentukan sudut condong badan & lengan karakter.
// usingEgrek HANYA relevan saat poseCode==Reach (panen): true=egrek (pokok
// tinggi, animasi MENARIK KE BAWAH dgn hentakan), false=dodos (pokok rendah,
// animasi MENDORONG KE ATAS) -- sesuai literatur teknik panen (lihat komentar
// di implementasi drawWorker()).
void drawWorker(float x, float z, int poseCode, bool usingEgrek, float facingRad=0.0f);

// TBS hasil panen tergeletak di dasar pohon (menunggu diangkut) — literatur:
// "penumpukan brondolan sebaiknya di sebelah tandan" & buah tak boleh lama
// tergeletak (ALB naik 0.9-1.0%/24 jam kalau kena tanah/hujan, Arvis SOP
// panen). Digambar sekali per pohon yg hasTbsReady==true.
void drawTbsPile(float x, float z);

// Tumpukan TBS di TPH, JUMLAH tumpukan = jumlah tandan di stok (dibatasi
// wajar spy tak membebani draw call kalau stok sangat besar).
// Bangunan PKS -- gudang proses + tangki/silo vertikal (jumlah sesuai
// level). pulse 0..1 utk efek visual saat proses batch baru terjadi.
void drawPksBuilding(float x, float z, int level, float pulse);
void drawTphPile(float tphX, float tphZ, int stockCount);
// Tumpukan pelepah hasil tunas di gawangan mati -- lihat catatan lengkap di
// renderer_gl.cpp.
void drawFrondPile(float gawanganCenterX, float gawanganCenterZ, float gawanganLength);
// Visual jalan panen/inspeksi (gawangan hidup) -- lihat catatan lengkap di renderer_gl.cpp.
void drawRoadStrip(float gawanganCenterX, float gawanganCenterZ, float gawanganLength);

// Traktor+trailer berisi TBS, animasi keluar dari TPH menuju PKS saat truk
// dikirim (murni visual, lihat catatan TruckState di types.hpp). facingRad =
// arah hadap (radian, dari sumbu +X).
void drawTruck(float x, float z, float facingRad);

// Tanda kecil di atas pohon yg sudah dikerjakan aksi massal HARI INI --
// kind: 0=panen,1=angkut,2=pupuk,3=pestisida,4=fungisida (lihat Tree::lastMarkKind).
// Penanda TBS matang mencolok & persisten (beda dr drawActionMarker yg
// transien) -- menembus di atas kanopi, terlihat dari kejauhan/zoom-out.
// overripe: true=lewat matang (merah, mendesak), false=matang normal (oranye).
// Toggle layer beacon TBS matang -- SATU sumber kebenaran shared kedua
// platform (dipanggil dari JNI & EngineBridge).
void setShowHarvestBeacon(bool show);
bool getShowHarvestBeacon();
// Estate View -- mode kamera zoom-out lihat SEMUA block sekaligus dgn
// representasi sederhana (ubin datar berwarna), BUKAN geometri pohon detail
// (lihat catatan lengkap di renderer_gl.cpp). layer: 0=Kesehatan,
// 1=Nutrisi, 2=Kematangan.
void setEstateViewMode(bool active, int layer);
// Toggle Gameplay Mode (third-person) vs Management Mode (ortografis,
// default). "Zoom terbatas" (adjustAvatarCamZoom) -- lihat catatan lengkap
// di renderer_gl.cpp.
void setGameplayModeActive(bool active);
bool getGameplayModeActive();
void adjustAvatarCamZoom(float delta);
bool getEstateViewActive();
int getEstateViewLayer();
// Depth key painter's-algorithm yang benar utk kamera 360 derajat -- lebih
// besar = lebih dekat kamera (digambar belakangan). Dipakai platform bridge
// (JNI/EngineBridge) utk mengurutkan pohon sebelum digambar, spy urutan
// tumpang-tindih visual benar dari sudut pandang manapun (lihat catatan
// lengkap di renderer_gl.cpp).
float depthKeyForYaw(float x, float z);
void estateLayerColor(int layer, float badFraction, float* outR, float* outG, float* outB);
void drawEstateBlockTile(float originX, float originZ, float r, float g, float b);
void drawHarvestBeacon(float x, float z, float treeTopY, bool overripe);
void drawActionMarker(float x, float z, float treeTopY, int kind);

// Estimasi tinggi batang dari umur pohon -- rumus SAMA PERSIS dgn yg dipakai
// drawPalm() internal, diekspos supaya pemanggil (JNI/bridge) bisa hitung
// posisi Y yg wajar utk drawActionMarker tanpa duplikasi rumus terpisah.
float treeTrunkHeight(float ageYears);

// Rumah/kantor kebun — elemen lingkungan statis (posisi tetap, dipanggil
// sekali per frame), meniru referensi visual "digital twin kebun sawit"
// yang diberikan pengguna. Murni dekoratif, tidak terikat gameplay.
void drawFarmhouse(float x, float z);

// Figur staf/pengawas berseragam beda warna dari pemanen biasa — posisinya
// TETAP (dekat rumah kebun), warnanya mengikuti jenjang SDM TERTINGGI yg
// sudah direkrut pemain (roleLevel 0=belum ada staf non-buruh .. 7=ADM).
// Berbeda dari drawWorker() yg mewakili buruh yg BERGERAK mengerjakan
// pohon -- ini murni dekoratif, meniru referensi visual (figur bertopi
// kuning & berkemeja beda warna di antara kerumunan pemanen).
void drawStaffFigure(float x, float z, int roleLevel);
// Avatar pemain (Gameplay Mode) dari model GLB custom -- geometri statis,
// goyangan idle sederhana (bukan walk-cycle penuh). Lihat catatan lengkap
// di renderer_gl.cpp & farmer_avatar_mesh.hpp.
void drawFarmerAvatar(float x, float z, float facingRad, bool moving);

// Portal/gerbang palang merah-putih + pos jaga — praktik standar stasiun
// penerimaan kebun sawit sungguhan (cek legalitas keluar-masuk kendaraan,
// lihat kutipan literatur di engine.hpp). Posisi tetap (kGateX/kGateZ).
void drawGate(float x, float z, float facingRad);

// ============================================================================
// INSPECTOR POHON — tampilan detail 1 pohon, close-up & berputar otomatis,
// MENGGANTIKAN pendekatan WebView/tree_detail.html sepenuhnya (kita TIDAK
// lagi memakai format HTML apa pun). Memakai ULANG drawPalm()/drawTbsPile()
// yang sudah ada (sudah termasuk mahkota golden-angle 137.5°, TBS, & warna
// sesuai kesehatan/Ganoderma/hama) -- TIDAK ADA geometri baru, cuma kamera &
// komposisi scene yg beda (pohon sendirian di tengah, bukan bagian dari 143
// pohon). Frame TERPISAH dari draw utama (beginFrame/endFrame sendiri) --
// panggil INI SAJA saat mode inspector aktif, bukan bersamaan draw utama.
//   ageYears/frond/health/ffb   : state pohon yg sedang diinspeksi
//   hasTbsReady                 : tampilkan tumpukan TBS di dasar (spt di game)
//   yawSpin                     : sudut putar kamera saat ini (radian) -- naikkan
//                                 sedikit tiap frame di pemanggil (JNI/bridge)
//                                 utk efek berputar otomatis
//   panY                        : geser vertikal titik fokus kamera (unit dunia,
//                                 0=default) -- dikendalikan gesture scroll di
//                                 pemanggil, supaya pemain bisa lihat dari akar
//                                 (y=0) sampai puncak mahkota. Sebelumnya kamera
//                                 inspector terkunci total tanpa cara menggeser.
// Dipanggil SEKALI saat inspector baru dibuka -- lihat catatan lengkap di renderer_gl.cpp.
void beginTreeInspectorTransition();
void drawTreeInspectorFrame(float ageYears, float frond, int health, int ffb, bool hasTbsReady, float yawSpin, float panY, float nutrition);

void endFrame();

// Proyeksikan titik dunia (x,y,z) ke koordinat layar (0..width, 0..height).
void worldToScreenY(float x, float y, float z, float* outScreenX, float* outScreenY);
// Kompatibilitas: versi lama yang mengasumsikan y=0 (titik di permukaan tanah).
void worldToScreen(float x, float z, float* outScreenX, float* outScreenY);
// Viewport culling -- true kalau titik dunia (x,z) masih dlm jangkauan
// pandang layar saat ini (dgn margin aman utk kanopi menjulang/melebar).
// worldRadius: radius objek dlm unit dunia -- 0 (default) utk titik kecil
// spt pohon individu, >0 utk objek besar spt Block (lihat catatan lengkap
// di renderer_gl.cpp). Dipakai platform bridge (JNI/EngineBridge) SEBELUM
// memanggil drawPalm/drawGround dkk, supaya objek di luar layar dilewati.
bool isWorldPointVisible(float x, float z, float worldRadius=0.0f);
// Blok depth-test karakter+bangunan -- mengatasi laporan pengguna #2. Lihat catatan lengkap di renderer_gl.cpp.
void beginCharacterDepthBlock();
void endCharacterDepthBlock();
// Pengaturan grafik & sensitivitas kamera -- fitur baru diminta pengguna.
// Lihat catatan lengkap di renderer_gl.cpp.
void setGraphicsQuality(int level); // 0=Rendah, 1=Sedang, 2=Tinggi
int getGraphicsQuality();
void setCameraSensitivity(float mult); // rentang wajar 0.5-2.0, default 1.0
float getCameraSensitivity();
void screenToWorldOnGroundPlane(float screenX, float screenY, float* outX, float* outZ);

// Hitung pergeseran PAN (dunia) yang benar dari sepasang titik layar (awal->akhir
// satu gesture drag), memakai screenToWorldOnGroundPlane yg sama persis dgn
// hit-test/tap — otomatis ikut kemiringan & ROTASI kamera saat ini, jadi tidak
// akan lagi salah arah spt bug lama (drag ke atas malah geser ke kanan-atas,
// itu terjadi krn dulu dx/dy layar dipakai langsung tanpa memperhitungkan
// kemiringan 45° kamera). Pemanggil (Kotlin/Swift) tinggal tambahkan hasilnya
// ke panX/panZ yang tersimpan di sisi mereka.
void panWorldDelta(float startScreenX, float startScreenY, float endScreenX, float endScreenY,
                    float* outDx, float* outDz);

// Uji ketukan (screenX,screenY) terhadap SATU pohon secara menyeluruh — mencakup
// batang dari dasar sampai puncak, DAN sebaran mahkota/pelepah di sekitarnya,
// bukan cuma titik dasar di y=0 (itu bug lama: ketuk pelepah/bagian atas pohon
// tidak terdeteksi). ageYears dipakai utk menghitung tinggi batang persis sama
// seperti drawPalm(), supaya hit-test selalu sinkron dgn yang benar-benar
// tergambar di layar. Mengembalikan jarak terdekat dlm PIKSEL LAYAR ke bagian
// mana pun dari siluet pohon — pemanggil (Kotlin/Swift) tinggal ambil pohon
// dgn jarak terkecil di antara semua pohon, lalu bandingkan ke ambang batas.
float hitTestDistance(float screenX, float screenY, float treeX, float treeZ, float ageYears);

} // namespace sawit::gl
