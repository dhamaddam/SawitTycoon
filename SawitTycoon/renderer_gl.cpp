#include "renderer_gl.hpp"

// --- header GL & logging: beda per platform, API OpenGL ES 2.0-nya sendiri sama ---
#if defined(__ANDROID__)
    #include <GLES2/gl2.h>
    #include <android/log.h>
    #define SAWIT_LOG_ERROR(tag, fmt, ...) __android_log_print(ANDROID_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#elif defined(__APPLE__)
    #include <OpenGLES/ES2/gl.h>
    #include <cstdio>
    #define SAWIT_LOG_ERROR(tag, fmt, ...) fprintf(stderr, "[%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
    #error "renderer_gl.cpp hanya mendukung Android (GLES2) dan iOS (OpenGLES). Tambahkan cabang platform di sini kalau perlu target lain."
#endif

#include <cmath>
#include <cstring>
#include <vector>
#include "palm_icon_mesh.hpp"
#include "farmer_avatar_mesh.hpp"
#include "pks_building_mesh.hpp"

namespace sawit::gl {
namespace {

// ---------------------------------------------------------------------------
// Mat4 minimal (column-major, konvensi OpenGL) — sengaja tanpa dependensi
// eksternal (tidak ada akses internet di sandbox utk mengambil glm dsb).
// ---------------------------------------------------------------------------
struct Mat4 { float m[16]; };

Mat4 mat4Identity(){
    Mat4 r{}; std::memset(r.m,0,sizeof(r.m));
    r.m[0]=r.m[5]=r.m[10]=r.m[15]=1.0f;
    return r;
}
Mat4 mat4Multiply(const Mat4& a, const Mat4& b){
    Mat4 r{};
    for(int c=0;c<4;c++) for(int row=0;row<4;row++){
        float sum=0;
        for(int k=0;k<4;k++) sum += a.m[k*4+row]*b.m[c*4+k];
        r.m[c*4+row]=sum;
    }
    return r;
}
Mat4 mat4Ortho(float l,float rr,float b,float t,float n,float f){
    Mat4 r = mat4Identity();
    r.m[0]=2.0f/(rr-l); r.m[5]=2.0f/(t-b); r.m[10]=-2.0f/(f-n);
    r.m[12]=-(rr+l)/(rr-l); r.m[13]=-(t+b)/(t-b); r.m[14]=-(f+n)/(f-n);
    return r;
}
// Proyeksi PERSPEKTIF -- fondasi kamera "Gameplay Mode" (third-person,
// avatar bisa digerakkan pemain) hasil review eksternal: "Untuk gameplay
// utama, gunakan third-person perspective... Management Mode: top-down
// estate overview" (mat4Ortho di atas TETAP dipakai utuh utk Management
// Mode/Estate View yg sudah ada -- BUKAN digantikan, cuma ditambah mode
// baru). fovYRad: sudut pandang vertikal (radian, umum ~50-60 derajat utk
// third-person game). Rumus standar OpenGL column-major.
Mat4 mat4Perspective(float fovYRad, float aspect, float nearZ, float farZ){
    Mat4 r{}; std::memset(r.m,0,sizeof(r.m));
    float f = 1.0f/std::tan(fovYRad*0.5f);
    r.m[0] = f/aspect;
    r.m[5] = f;
    r.m[10] = (farZ+nearZ)/(nearZ-farZ);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f*farZ*nearZ)/(nearZ-farZ);
    return r;
}
// View matrix look-at standar OpenGL -- fondasi kamera "Gameplay Mode"
// (third-person, avatar bisa digerakkan pemain). BERBEDA dari view matrix
// Management Mode (mat4RotateX+RotateY+Translate, cuma rotasi pitch TETAP +
// yaw bebas + pan) -- kamera third-person perlu benar2 MENGHADAP ke titik
// tertentu (avatar) dari posisi bebas manapun, bukan sekadar rotasi tetap.
Mat4 mat4LookAt(float eyeX,float eyeY,float eyeZ, float targetX,float targetY,float targetZ, float upX,float upY,float upZ){
    // arah pandang (forward), ternormalisasi
    float fx=targetX-eyeX, fy=targetY-eyeY, fz=targetZ-eyeZ;
    float flen = std::sqrt(fx*fx+fy*fy+fz*fz); if (flen<1e-6f) flen=1e-6f;
    fx/=flen; fy/=flen; fz/=flen;
    // right = forward x up, ternormalisasi
    float sx=fy*upZ-fz*upY, sy=fz*upX-fx*upZ, sz=fx*upY-fy*upX;
    float slen = std::sqrt(sx*sx+sy*sy+sz*sz); if (slen<1e-6f) slen=1e-6f;
    sx/=slen; sy/=slen; sz/=slen;
    // up sebenarnya (ortogonal thd forward & right) = right x forward
    float ux=sy*fz-sz*fy, uy=sz*fx-sx*fz, uz=sx*fy-sy*fx;
    Mat4 r = mat4Identity();
    r.m[0]=sx; r.m[4]=sy; r.m[8]=sz;
    r.m[1]=ux; r.m[5]=uy; r.m[9]=uz;
    r.m[2]=-fx; r.m[6]=-fy; r.m[10]=-fz;
    r.m[12] = -(sx*eyeX+sy*eyeY+sz*eyeZ);
    r.m[13] = -(ux*eyeX+uy*eyeY+uz*eyeZ);
    r.m[14] = fx*eyeX+fy*eyeY+fz*eyeZ;
    return r;
}
Mat4 mat4RotateX(float rad){
    Mat4 r = mat4Identity();
    float c=std::cos(rad), s=std::sin(rad);
    r.m[5]=c; r.m[6]=s; r.m[9]=-s; r.m[10]=c;
    return r;
}
Mat4 mat4RotateY(float rad){
    Mat4 r = mat4Identity();
    float c=std::cos(rad), s=std::sin(rad);
    r.m[0]=c; r.m[2]=-s; r.m[8]=s; r.m[10]=c;
    return r;
}
// Fondasi kemiringan batang pohon (poin review dokumen: "Variasi ukuran,
// kemiringan, umur, dan kepadatan crown ditentukan oleh seed") -- SEBELUMNYA
// TIDAK ADA rotasi sumbu Z sama sekali, cuma RotateY (orientasi/yaw).
Mat4 mat4RotateZ(float rad){
    Mat4 r = mat4Identity();
    float c=std::cos(rad), s=std::sin(rad);
    r.m[0]=c; r.m[1]=s; r.m[4]=-s; r.m[5]=c;
    return r;
}
Mat4 mat4Translate(float x,float y,float z){
    Mat4 r = mat4Identity();
    r.m[12]=x; r.m[13]=y; r.m[14]=z;
    return r;
}
Mat4 mat4Scale(float x,float y,float z){
    Mat4 r = mat4Identity();
    r.m[0]=x; r.m[5]=y; r.m[10]=z;
    return r;
}

// ---------------------------------------------------------------------------
// Vec3 minimal + builder geometri prosedural pohon sawit — diadaptasi dari
// model referensi Three.js (profil batang, sebaran pelepah, tandan buah),
// disederhanakan (lebih sedikit sisi/segmen, tanpa tekstur) supaya tetap
// ringan dirender berkali-kali (puluhan-ratusan pohon per hektar) di OpenGL
// ES 2.0 mobile tanpa shader/tekstur kompleks.
// ---------------------------------------------------------------------------
struct V3 { float x,y,z; };
V3 v3add(V3 a, V3 b){ return {a.x+b.x, a.y+b.y, a.z+b.z}; }
V3 v3lerp(V3 a, V3 b, float t){ return { a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t }; }
V3 v3rotY(V3 p, float ang){
    float c = std::cos(ang), s = std::sin(ang);
    return { p.x*c + p.z*s, p.y, -p.x*s + p.z*c };
}
V3 v3rotX(V3 p, float ang){
    float c = std::cos(ang), s = std::sin(ang);
    return { p.x, p.y*c - p.z*s, p.y*s + p.z*c };
}
void pushTri(std::vector<float>& out, V3 a, V3 b, V3 c){
    out.insert(out.end(), {a.x,a.y,a.z, b.x,b.y,b.z, c.x,c.y,c.z});
}
void pushQuad(std::vector<float>& out, V3 a, V3 b, V3 c, V3 d){ // urutan keliling quad
    pushTri(out,a,b,c); pushTri(out,a,c,d);
}
// Kotak sejajar-sumbu sederhana (6 sisi), dipakai utk badan/celana/sepatu.
void pushBox(std::vector<float>& out, float x0,float y0,float z0, float x1,float y1,float z1){
    V3 p0{x0,y0,z0}, p1{x1,y0,z0}, p2{x1,y1,z0}, p3{x0,y1,z0};
    V3 p4{x0,y0,z1}, p5{x1,y0,z1}, p6{x1,y1,z1}, p7{x0,y1,z1};
    pushQuad(out,p0,p1,p2,p3); pushQuad(out,p5,p4,p7,p6);
    pushQuad(out,p4,p0,p3,p7); pushQuad(out,p1,p5,p6,p2);
    pushQuad(out,p3,p2,p6,p7); pushQuad(out,p4,p5,p1,p0);
}
// Kotak yg "tumbuh" dari titik pivot ke arah bawah (lokal y: -len..0), lalu
// diputar sekitar sumbu-X pivot (tiltX) sebelum dipindah ke posisi dunia --
// dipakai utk lengan yg sudutnya berubah sesuai pose (jongkok/alat/meraih).
void pushBoxPivotY(std::vector<float>& out, float w, float len, float d, float tiltX,
                    float pivotX, float pivotY, float pivotZ){
    V3 local[8] = {
        {-w/2,-len,-d/2},{w/2,-len,-d/2},{w/2,0,-d/2},{-w/2,0,-d/2},
        {-w/2,-len, d/2},{w/2,-len, d/2},{w/2,0, d/2},{-w/2,0, d/2},
    };
    V3 p[8];
    for (int i=0;i<8;i++){
        V3 r = v3rotX(local[i], tiltX);
        p[i] = { r.x+pivotX, r.y+pivotY, r.z+pivotZ };
    }
    pushQuad(out,p[0],p[1],p[2],p[3]); pushQuad(out,p[5],p[4],p[7],p[6]);
    pushQuad(out,p[4],p[0],p[3],p[7]); pushQuad(out,p[1],p[5],p[6],p[2]);
    pushQuad(out,p[3],p[2],p[6],p[7]); pushQuad(out,p[4],p[5],p[1],p[0]);
}

// Tandan buah segar: seludang (spathe) kerucut kecil + gerombolan buah bulat kecil.
// ---------------------------------------------------------------------------
// Mesh gaya ikon sekarang di-bake langsung dari ptks_palm_icon.stl, lihat
// palm_icon_mesh.hpp (di-include di atas) dan pemakaiannya di drawPalm().
// ---------------------------------------------------------------------------
GLuint g_prog = 0;
GLint g_aPos = -1, g_uMvp = -1, g_uColor = -1;
int g_width = 1, g_height = 1;
float g_panX = 0, g_panZ = 0, g_dist = 44.0f;

// LOD (Level of Detail) system -- fitur baru diminta pengguna ("kebun bisa
// memiliki ratusan/ribuan pohon... Jangan melakukan draw call individual
// utk setiap pohon jika jumlahnya besar"). Threshold jarak (0-30m/30-100m/
// 100-300m/>300m) DISKALAKAN dari meter asli ke unit dunia game -- COPY
// LANGSUNG 30/100/300 sbg unit dunia akan SALAH TOTAL: kColSpacing=5.2
// (jarak tanam) mewakili SOP asli 9m (lihat STORYLINE.md), rasio skala =
// 5.2/9 ≈ 0.578. Radius pandang kamera standar (zoom biasa) cuma ~20 unit
// -- kalau threshold HIGH=30 unit (bukan 30m), HAMPIR SEMUA pohon yg
// visible akan jatuh di tier HIGH saja, LOD jadi tak berguna sama sekali.
// Nilai final: 30m*0.578≈17.3, 100m*0.578≈57.8, 300m*0.578≈173.4.
const float kLodHighMaxDist = 17.3f;   // 0-30m asli: full detail (mesh lengkap)
const float kLodMediumMaxDist = 57.8f; // 30-100m asli: sedang (skip crown_dark+fruit)
const float kLodLowMaxDist = 173.4f;   // 100-300m asli: rendah (primitif sederhana)
                                        // >300m asli: HIDE (sebagian besar SUDAH tertangani
                                        // frustum culling isWorldPointVisible() yg ada,
                                        // tambahan eksplisit di sini utk kasus zoom-out
                                        // ekstrim di mana objek jauh masih lolos krn
                                        // isWorldPointVisible berbasis proyeksi LAYAR,
                                        // bukan jarak murni dari kamera).

float g_panY = 0.0f; // geser vertikal -- HANYA dipakai mode Inspector Pohon (scroll
                      // lihat dari akar sampai mahkota); kebun utama tak pernah
                      // menyentuh ini, jadi selalu 0 di sana (aman, tak ada regresi).
float g_animT = 0.0f; // jam animasi global, maju sedikit tiap beginFrame() -- dipakai
                       // utk animasi menekan-narik dodos/egrek (lihat drawWorker)
// Toggle "layer" beacon TBS matang (spt konsep LAYERS di dokumen desain
// Estate/Block View: "Pokok/Baris/TBS matang/Kesehatan/..."). SATU sumber
// kebenaran di sini (bukan variabel independen per-platform di JNI/
// EngineBridge yg berisiko tak sinkron) -- dipakai kedua platform via
// setShowHarvestBeacon()/getShowHarvestBeacon(). Default AKTIF.
bool g_showHarvestBeacon = true;
// Estate View -- mode kamera zoom-out lihat SEMUA block sekaligus, dengan
// representasi SEDERHANA per-block (ubin datar berwarna, BUKAN geometri
// pohon detail penuh) -- di skala ini, merender ~2600 segitiga/pohon utk
// SEMUA block sekaligus akan MEMBALIKKAN optimasi viewport culling (semua
// jadi "terlihat" bersamaan, FPS anjlok sama seperti bug asli yg sudah
// diperbaiki). layer: 0=Kesehatan, 1=Nutrisi, 2=Kematangan (TBS siap panen).
bool g_estateViewActive = false;
int g_estateViewLayer = 0;
float g_yaw = 0.7854f; // 45 derajat, sudut isometrik klasik — sekarang BISA diputar (fitur 360°)
const float kPitch = -0.9599f; // ~55 derajat -- lebih tegak ke bawah (mirip foto udara) drpd
                                // versi sebelumnya (-35deg), tapi tidak full 90 derajat spy
                                // batang & mahkota masih bisa dibedakan utk ketuk-pilih pohon.
                                // AMAN diubah ke nilai lain (asal < 0 dan > -PI/2) karena
                                // seluruh pipeline (render/hit-test/pan) ambil dari sini.
Mat4 g_view, g_proj, g_viewProj;

// State kamera "Gameplay Mode" (third-person, avatar bisa digerakkan) --
// hasil review eksternal poin #5. g_gameplayModeActive: toggle antara mode
// ini vs Management Mode (ortografis, SUDAH ADA & TETAP DIPERTAHANKAN
// UTUH). g_avatarCamEye*: posisi kamera SAAT INI (bukan posisi ideal
// seketika) -- dipakai smooth follow via exponential smoothing tiap frame,
// supaya kamera tak "nempel kaku" & terasa API/robotik saat avatar bergerak
// (persis review: "Tambahkan smooth follow").
bool g_gameplayModeActive = false;
float g_avatarCamEyeX = 0.0f, g_avatarCamEyeY = 1.6f, g_avatarCamEyeZ = -2.4f; // BUG diperbaiki: nilai lama (4.0,-6.0) berdasar skala avatar lama, diskalakan proporsional (rasio 0.4)

// Jarak 2D (X,Z) dari kamera SAAT INI ke suatu titik dunia -- fondasi LOD
// (lihat konstanta kLodHighMaxDist dkk di atas). Sumber posisi kamera
// BERBEDA tergantung mode aktif: Management Mode (ortografis, g_panX/panZ
// = titik yg dipandang kamera) vs Gameplay Mode (perspektif third-person,
// g_avatarCamEyeX/Z = posisi mata kamera sesungguhnya, BUKAN g_panX/panZ
// yg tak dipakai sama sekali di mode ini).
float cameraDistanceToPoint(float x, float z){
    float camX, camZ;
    if (g_gameplayModeActive){ camX = g_avatarCamEyeX; camZ = g_avatarCamEyeZ; }
    else { camX = g_panX; camZ = g_panZ; }
    float dx = x-camX, dz = z-camZ;
    return std::sqrt(dx*dx + dz*dz);
}
// Titik yang DILIHAT kamera (target look-at) -- diisi LANGSUNG di
// updateThirdPersonCamera() dari posisi avatar + offset tinggi (setinggi
// dada/kepala), BUKAN dihitung balik dari posisi eye di updateViewProj()
// (lebih sederhana & robust drpd inverse-kinematics posisi).
float g_avatarLookAtX = 0.0f, g_avatarLookAtY = 0.232f, g_avatarLookAtZ = 0.0f; // DISESUAIKAN
// (dari 0.6) sesuai spesifikasi EKSAK diminta pengguna: "Camera distance 6m,
// Camera height 3.2m, Pitch normal 25 derajat". Dikonversi ke unit dunia via
// rasio skala 0.578 (sama dipakai konsisten sepanjang sesi) -> distance=3.467,
// height=1.849 unit dunia, lalu targetY dihitung MATEMATIS TEPAT (bukan
// perkiraan) supaya pitch = atan((height-targetY)/distance) = PERSIS 25
// derajat (bukan cuma "dlm rentang" spt sblmnya) -- targetY=0.232 (terverifikasi
// numerik, lihat catatan lengkap di updateThirdPersonCamera()).

// State transisi kamera SMOOTH saat masuk tree inspector -- mengatasi
// laporan pengguna: "Saat inspeksi pohon kamera melakukan smooth focus/zoom
// ke pohon" -- SEBELUMNYA g_panX/panZ/dist/yaw LANGSUNG di-set instan ke
// posisi close-up tiap frame drawTreeInspectorFrame() dipanggil (lihat
// fungsi itu), tanpa transisi visual sama sekali -- kamera "melompat"
// mendadak dari posisi manapun sebelumnya (top-down Management Mode ATAU
// third-person Gameplay Mode) ke close-up pohon. Sekarang di-smooth
// exponensial (konsisten pola updateThirdPersonCamera()) dari posisi
// SEBELUM inspector dibuka menuju posisi close-up target.
float g_inspCamPanX=0, g_inspCamPanY=0, g_inspCamPanZ=0, g_inspCamDist=20.0f;
bool g_inspCamInitialized = false;
float g_avatarCamDistBehind = 3.467f; // jarak di belakang avatar -- bisa di-zoom (lihat setAvatarCamZoom).
// DISESUAIKAN (dari 3.2) sesuai spesifikasi EKSAK diminta pengguna: "Camera
// distance 6m". Dikonversi ke unit dunia via rasio 0.578 -> 3.467 unit dunia.

// Offset "lihat ke atas" -- fitur baru diminta pengguna: "tidak bisa melihat lebih
// ke atas pohon sawit, berikan lebih jauh sudut pandang hanya untuk melihat ke atas
// tidak untuk horizontal". SEBELUMNYA drag layar di Gameplay Mode HANYA memakai
// komponen horizontal (distanceX -> yaw), komponen vertikal (distanceY) SAMA SEKALI
// tak dipakai -- tak ada cara mendongak melihat kanopi pohon sawit dewasa (tinggi
// ~4.4-7 unit) dari jarak dekat third-person. Rentang [0, kMaxLookUpOffset] --
// SENGAJA cuma non-negatif (TIDAK bisa "menunduk" lebih dari baseline) sesuai
// permintaan eksplisit "hanya untuk melihat ke atas". Ditambahkan ke lookAtY dasar
// (0.232, DIPERBARUI dari 0.6 -- lihat spesifikasi kamera eksak di atas) di
// updateThirdPersonCamera() -- makin besar offset, makin tinggi target look-at,
// makin curam pandangan mendongak, TANPA mengubah yaw/rotasi horizontal sama sekali.
float g_avatarLookUpOffset = 0.0f;
const float kMaxLookUpOffset = 7.4f; // DISESUAIKAN (dari 7.0) -- base lookAtY turun
// jadi 0.232 (dari 0.6), max dinaikkan sedikit spy total maks TETAP ~7.6
// (0.232+7.4=7.632, konsisten dgn tujuan awal: cukup mendongak ke kanopi pohon tertinggi).

// Pengaturan GRAFIK & sensitivitas -- fitur baru diminta pengguna ("tambahkan
// pengaturan sensivitas dan grafik"). Variabel STATE tetap di sini (private,
// pola yg sama dgn g_avatarCamDistBehind di atas) -- fungsi PUBLIC (setter/
// getter, dipanggil dari JNI/EngineBridge) ada DI LUAR anonymous namespace
// ini (dekat isWorldPointVisible(), lihat di bawah file) -- BUG diperbaiki:
// sempat salah taruh fungsi publicnya DI SINI (internal linkage, gagal link
// dari sawit_jni.cpp/EngineBridge.mm).
int g_graphicsQuality = 1; // 0=Rendah, 1=Sedang (default), 2=Tinggi
float g_cameraSensitivity = 1.0f; // 0.5-2.0, default 1.0
// BUG diperbaiki (dilaporkan pengguna dari screen record: "tinggi pekerja
// dan pohon sawit tidak sebanding"). Verifikasi numerik (bounding box
// renderer sesungguhnya): pekerja SEBELUMNYA 4.80 unit tinggi, TERNYATA
// LEBIH TINGGI dari pohon TERMUDA sekalipun (umur 2 tahun, 4.40 unit
// setelah perbaikan batang di atas) -- padahal manusia dewasa seharusnya
// jauh lebih pendek dari kelapa sawit bahkan yg masih muda. Semua konstanta
// terkait ukuran/kamera avatar diskalakan PROPORSIONAL (rasio 0.4, sesuai
// FARMER_SCALE baru 1.6 vs lama 4.0 di drawFarmerAvatar()) supaya framing
// kamera third-person tetap koheren (avatar mengecil, kamera ikut
// "mendekat" proporsional, bukan tiba2 terasa jauh/salah framing).
const float kAvatarCamDistMin = 1.2f, kAvatarCamDistMax = 4.5f; // "zoom terbatas" sesuai review (diskalakan dari 3.0-10.0).
// Batas atas SEDIKIT diperluas (dari 4.0) mengiringi kenaikan default distBehind
// (2.4->3.2, lihat catatan lengkap di sana) -- supaya default baru tak terlalu
// dekat batas atas, tetap ada ruang zoom out yg berarti.
const float kAvatarCamHeight = 1.849f; // tinggi kamera dari tanah -- DISESUAIKAN (dari 1.9)
// sesuai spesifikasi EKSAK diminta pengguna: "Camera height 3.2m". Dikonversi ke
// unit dunia via rasio 0.578 -> 1.849 unit dunia. Kombinasi dgn distance (3.467)
// & target look-at (0.232, lihat di atas) menghasilkan pitch = PERSIS 25 derajat
// (bukan cuma "dlm rentang" spt versi sebelumnya) -- terverifikasi numerik.
const float kAvatarCamGroundClearance = 0.2f; // BUG collision dicegah: kamera tak boleh turun di bawah ini (menembus tanah) -- diskalakan dari 0.5

const char* kVS =
    "attribute vec3 aPos;\n"
    "uniform mat4 uMVP;\n"
    "void main(){ gl_Position = uMVP * vec4(aPos, 1.0); }\n";

const char* kFS =
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "void main(){ gl_FragColor = uColor; }\n";

GLuint compileShader(GLenum type, const char* src){
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if(!ok){
        char log[512]; GLsizei n=0;
        glGetShaderInfoLog(sh, sizeof(log), &n, log);
        SAWIT_LOG_ERROR("SawitGL", "Shader compile error: %s", log);
    }
    return sh;
}

void buildProgram(){
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVS);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFS);
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs);
    glAttachShader(g_prog, fs);
    glBindAttribLocation(g_prog, 0, "aPos");
    glLinkProgram(g_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    g_aPos = 0;
    g_uMvp = glGetUniformLocation(g_prog, "uMVP");
    g_uColor = glGetUniformLocation(g_prog, "uColor");
}

// Hitung posisi kamera IDEAL (di belakang+atas avatar sesuai arah hadap),
// lalu SMOOTH FOLLOW (exponential smoothing, framerate-independent) menuju
// posisi ideal itu -- BUKAN snap langsung, supaya kamera terasa "hidup"/
// natural saat avatar berbelok/bergerak (persis review: "Tambahkan smooth
// follow"). Dipanggil dari JNI/EngineBridge tiap frame SEBELUM
// updateViewProj(), dgn posisi avatar TERKINI dari engine.playerAvatar().
// effectiveDistBehind: jarak SETELAH dikoreksi collision pohon (dari
// engine.cameraSafeDistance(), dipanggil JNI/EngineBridge SEBELUM fungsi
// ini) -- BUKAN g_avatarCamDistBehind mentah, supaya collision cuma
// berlaku SEMENTARA per-frame (setting zoom pemain/g_avatarCamDistBehind
// TAK ikut berubah permanen; begitu avatar menjauh dari pohon yg
// menghalangi, kamera otomatis kembali ke jarak zoom aslinya).
void updateThirdPersonCamera(float playerX, float playerZ, float playerFacingRad, float dt, float effectiveDistBehind){
    // REDESAIN TOTAL (poin #1 laporan pengguna, dibuktikan dari video:
    // avatar terlihat dari samping lalu dari belakang penuh dlm waktu
    // SANGAT singkat setelah joystick sedikit digeser -- kamera "menempel"
    // pada facingRad avatar). SEBELUMNYA cameraYaw = playerFacingRad +
    // getCameraYawOffset() -- kamera SELALU ikut berputar setiap avatar
    // berputar (krn mekanisme "offset diserap ke facingRad" di
    // Engine::movePlayerAvatar()/JNI, lihat catatan lengkap di sana).
    // Pengguna eksplisit minta pola Roblox/third-person mobile: "Joystick
    // kiri harus 100% menjadi kontrol locomotion pekerja, bukan kontrol
    // kamera... Kamera harus mempunyai kontrol terpisah". Sekarang
    // cameraYaw MURNI dari getCameraYawOffset() (ganti makna: sekarang
    // ORIENTASI ABSOLUT kamera dari touch-drag bebas, TIDAK PERNAH lagi
    // "diserap"/direset oleh gerakan avatar) -- playerFacingRad (parameter
    // ini) TIDAK LAGI dipakai di sini sama sekali (avatar digambar
    // terpisah dgn facingRad-nya sendiri di drawFarmerAvatar(), tak
    // terpengaruh perubahan ini). effectiveDistBehind (collision thd
    // pohon) HARUS dihitung dgn sudut yg SAMA ini oleh caller (JNI/
    // EngineBridge, lihat cameraSafeDistance) spy konsisten.
    (void)playerFacingRad; // sengaja tak dipakai lagi -- lihat catatan di atas
    float cameraYaw = getCameraYawOffset();
    float idealEyeX = playerX - std::cos(cameraYaw)*effectiveDistBehind;
    float idealEyeZ = playerZ - std::sin(cameraYaw)*effectiveDistBehind;
    float idealEyeY = kAvatarCamHeight;
    // BUG collision dicegah: kamera tak pernah dibiarkan turun ke bawah
    // kAvatarCamGroundClearance (menembus tanah) -- clamp SEBELUM smoothing,
    // supaya smoothing tak "menarik" kamera balik naik dari bawah tanah yg
    // terlihat aneh. Collision thd POHON ditangani via effectiveDistBehind
    // di atas (raycast engine.cameraSafeDistance()) -- BUG diperbaiki:
    // sebelumnya HANYA ground clearance yg ditangani, kamera third-person
    // bisa menembus batang pohon tanpa halangan sama sekali.
    idealEyeY = std::max(kAvatarCamGroundClearance, idealEyeY);

    float smoothing = 1.0f - std::exp(-dt * 6.0f); // konstanta 6.0 -- responsif tapi tak kaku
    g_avatarCamEyeX += (idealEyeX - g_avatarCamEyeX) * smoothing;
    g_avatarCamEyeY += (idealEyeY - g_avatarCamEyeY) * smoothing;
    g_avatarCamEyeZ += (idealEyeZ - g_avatarCamEyeZ) * smoothing;

    // Target look-at: setinggi dada/kepala avatar -- BUG diperbaiki: nilai
    // lama (y=3.0) dihitung dari kepala WORKER_SCALE=4x di y~2.6-2.9 (skala
    // avatar LAMA). Setelah avatar diskalakan turun (rasio 0.4, lihat
    // catatan proporsi pekerja-pohon), target ikut diskalakan proporsional
    // (3.0*0.4=1.2) supaya kamera tetap fokus setinggi dada/kepala avatar
    // BARU, bukan jauh di atas kepala. BUKAN persis di kaki (y=0) -- supaya
    // pandangan kamera natural (level mata), bukan menunduk ke tanah. Ikut
    // di-smooth jg (x,z) spy tak "meloncat" ikuti avatar terlalu instan,
    // tapi lebih cepat dari smoothing eye (avatar sendiri yg jadi fokus,
    // wajar lbh responsif dari kamera).
    float smoothingTarget = 1.0f - std::exp(-dt * 10.0f);
    g_avatarLookAtX += (playerX - g_avatarLookAtX) * smoothingTarget;
    g_avatarLookAtZ += (playerZ - g_avatarLookAtZ) * smoothingTarget;
    // DISESUAIKAN sesuai spesifikasi EKSAK diminta pengguna: "Camera distance
    // 6m, Camera height 3.2m, Pitch normal 25 derajat" (bukan lagi rentang
    // perkiraan spt sblmnya, tapi angka tunggal presisi). Dikonversi ke unit
    // dunia via rasio 0.578 -> distance=3.467, height=1.849, lalu targetY
    // (0.232) dihitung MATEMATIS TEPAT supaya pitch = atan((height-targetY)/
    // distance) = PERSIS 25 derajat. Verifikasi numerik: eyeY=1.849
    // (kAvatarCamHeight), dist=3.467 (g_avatarCamDistBehind), targetY=0.232
    // -> pitch = atan((1.849-0.232)/3.467) = 25.00 derajat, PERSIS sesuai
    // spesifikasi (bukan cuma "dlm rentang").
    //
    // g_avatarLookUpOffset DITAMBAHKAN di sini (fitur baru diminta pengguna:
    // "berikan lebih jauh sudut pandang hanya untuk melihat ke atas") --
    // baseline 0.232 TETAP jadi titik awal (tak berubah saat offset=0,
    // perilaku default TERJAGA), offset MENAMBAH tinggi target saat pemain
    // drag layar ke atas (lihat adjustAvatarLookUpOffset()), TANPA menyentuh
    // yaw/rotasi horizontal sama sekali (variabel independen).
    g_avatarLookAtY = 0.232f + g_avatarLookUpOffset; // tinggi tetap (tak perlu smoothing, avatar tak berubah tinggi)
}

void updateViewProj(){
    float aspect = (g_height>0) ? (float)g_width/(float)g_height : 1.0f;
    if (g_gameplayModeActive){
        // Gameplay Mode -- PERSPEKTIF third-person. Eye & target keduanya
        // sudah di-smooth oleh updateThirdPersonCamera() (dipanggil terpisah
        // dari JNI/EngineBridge tiap frame dgn posisi avatar terkini) --
        // di sini tinggal pakai nilai state g_avatarCamEye*/g_avatarLookAt*
        // apa adanya utk bangun view matrix.
        g_view = mat4LookAt(g_avatarCamEyeX, g_avatarCamEyeY, g_avatarCamEyeZ,
                             g_avatarLookAtX, g_avatarLookAtY, g_avatarLookAtZ,
                             0,1,0);
        // FOV DISESUAIKAN (dari 55) sesuai spesifikasi EKSAK diminta pengguna: "FOV 60 derajat".
        g_proj = mat4Perspective(60.0f*3.14159265f/180.0f, aspect, 0.3f, 300.0f);
    } else {
        // Management Mode -- ORTOGRAFIS, SAMA PERSIS spt sebelumnya (TAK
        // ADA PERUBAHAN sama sekali di sini, sesuai review: "Management
        // Mode: top-down estate overview" TETAP dipertahankan utuh).
        Mat4 rot = mat4Multiply(mat4RotateX(kPitch), mat4RotateY(g_yaw));
        Mat4 trans = mat4Translate(-g_panX, -g_panY, -g_panZ);
        g_view = mat4Multiply(rot, trans);
        float halfH = g_dist * 0.45f;
        float halfW = halfH * aspect;
        g_proj = mat4Ortho(-halfW, halfW, -halfH, halfH, -100.0f, 100.0f);
    }
    g_viewProj = mat4Multiply(g_proj, g_view);
}

// yaw (opsional, default 0): rotasi TAMBAHAN sekitar sumbu Y sebelum scale --
// GPU-side (lewat matrix), BUKAN transformasi ulang vertex di CPU, jadi tetap
// murah dipanggil ratusan kali/frame. Dipakai drawPalm() memberi variasi
// orientasi PER POHON (celah yg diperbaiki: sebelumnya SEMUA pohon punya
// orientasi kanopi identik krn golden-angle di-bake dgn sudut awal sama utk
// semua instance -- dari atas terlihat jelas sbg "pattern generator" berulang,
// bukan kebun alami. Review eksternal: "hampir semua pohon tinggi sama,
// bentuk tajuk sama... kebun terlihat seperti pattern generator").
void drawTris(const float* verts, int vertCount, float r, float g, float b, float a,
              float x, float y, float z, float sx, float sy, float sz, float yaw=0.0f,
              float tiltX=0.0f, float tiltZ=0.0f){
    // tiltX/tiltZ: kemiringan batang pohon dari vertikal (poin review dokumen:
    // "Variasi ukuran, kemiringan, umur, dan kepadatan crown ditentukan oleh
    // seed" -- SEBELUMNYA tak ada kemiringan sama sekali, pohon selalu tegak
    // lurus sempurna). Default 0.0f -- SEMUA pemanggilan lama (worker, truk,
    // bangunan, dll) TAK terpengaruh sama sekali, tetap tegak lurus spt
    // sebelumnya. Diterapkan SETELAH yaw (supaya arah kemiringan konsisten
    // relatif orientasi objek yg sudah diputar), SEBELUM scale (supaya
    // kemiringan tak ikut terdistorsi non-uniform scale sumbu berbeda).
    Mat4 model = mat4Multiply(mat4Translate(x,y,z),
                    mat4Multiply(mat4RotateY(yaw),
                        mat4Multiply(mat4RotateX(tiltX),
                            mat4Multiply(mat4RotateZ(tiltZ), mat4Scale(sx,sy,sz)))));
    Mat4 mvp = mat4Multiply(g_viewProj, model);
    glUseProgram(g_prog);
    glUniformMatrix4fv(g_uMvp, 1, GL_FALSE, mvp.m);
    glUniform4f(g_uColor, r,g,b,a);
    glEnableVertexAttribArray(g_aPos);
    glVertexAttribPointer(g_aPos, 3, GL_FLOAT, GL_FALSE, 0, verts);
    glDrawArrays(GL_TRIANGLES, 0, vertCount);
    glDisableVertexAttribArray(g_aPos);
}

// Varian utk geometri yang dibangun dinamis per-frame (trunk/frond/fruit di
// atas) — posisi vertex SUDAH final (sudah diputar/dipindah di CPU saat
// dibangun), jadi di sini translate/scale model cukup identitas.
void drawDynamic(const std::vector<float>& verts, float r, float g, float b, float a){
    if(verts.empty()) return;
    drawTris(verts.data(), (int)(verts.size()/3), r,g,b,a, 0,0,0, 1,1,1);
}

} // namespace

void init(){
    glClearColor(0.62f, 0.815f, 0.851f, 1.0f); // #9fd0da, samakan dgn versi web
    // SENGAJA TIDAK memakai GL_DEPTH_TEST: dengan kamera ortografis yang di-tilt
    // (pitch isometrik), titik di puncak batang pohon (y tinggi) menghasilkan nilai
    // depth yang membuatnya "kalah" terhadap bidang tanah datar di piksel layar
    // yang sama -> pohon jadi tersembunyi di belakang tanah meski sudah digambar.
    // Solusinya pakai urutan gambar (painter's algorithm): tanah SELALU digambar
    // duluan (lihat drawGround dipanggil sebelum loop drawPalm di pemanggil), jadi
    // pohon otomatis tertimpa di atasnya tanpa perlu depth buffer sama sekali.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // utk highlight seleksi semi-transparan
    buildProgram();
    updateViewProj();
}

void resize(int width, int height){
    g_width = width; g_height = height;
    glViewport(0,0,width,height);
    updateViewProj();
}

void setCamera(float panX, float panZ, float distance, float yaw){
    g_panX = panX; g_panZ = panZ; g_dist = distance; g_yaw = yaw;
    updateViewProj();
}

// Warna langit dinamis berdasarkan waktu hari & cuaca -- fitur baru diminta
// pengguna: "mekanisme musim... harus menampilkan... malam dengan view yang
// agak gelap dan siang (adanya matahari)". SEBELUMNYA glClearColor() cuma
// dipanggil SEKALI di init() dgn 1 warna tetap (#9fd0da, biru cerah),
// terlepas waktu/cuaca -- TimeOfDay() & isRaining() SUDAH ADA PENUH di
// engine (bahkan isRaining() berbasis literatur pola hujan Sumatra), tapi
// TAK PERNAH terhubung ke visual sama sekali.
//
// Interpolasi KEYFRAME (bukan cuma 3 warna diskrit Pagi/Siang/Malam) --
// transisi halus antar waktu, SELARAS dgn kategori TimeOfDay() yg sudah
// ada (frac<0.40=Pagi, frac<0.75=Siang, sisanya=Malam) tapi dgn nuansa
// fajar/senja di antaranya (bukan lompatan tiba-tiba gelap<->terang).
// Nilai di frac=0.0 & frac=1.0 SENGAJA SAMA PERSIS -- loop mulus antar
// hari (hari berakhir gelap, hari baru mulai dari gelap yg sama, bukan
// "lompat" balik ke terang).
struct SkyKeyframe { float frac, r, g, b; };
const SkyKeyframe kSkyKeyframes[] = {
    {0.00f, 0.08f, 0.09f, 0.18f}, // tengah malam gelap
    {0.15f, 0.08f, 0.09f, 0.18f}, // dini hari, masih gelap
    {0.25f, 0.90f, 0.65f, 0.45f}, // fajar/sunrise -- oranye hangat
    {0.40f, 0.62f, 0.815f,0.851f},// pagi cerah penuh (batas kategori Pagi->Siang)
    {0.75f, 0.62f, 0.815f,0.851f},// akhir siang, msh cerah (batas kategori Siang->Malam)
    {0.85f, 0.80f, 0.55f, 0.35f}, // senja/sunset -- oranye (kategori "Malam" mulai, visual msh senja dulu)
    {0.95f, 0.20f, 0.15f, 0.28f}, // transisi cepat ke gelap
    {1.00f, 0.08f, 0.09f, 0.18f}, // kembali ke tengah malam (SAMA dgn 0.00 -- loop mulus)
};
const int kSkyKeyframeCount = 8;

void setSkyState(float dayFrac, bool isRaining){
    dayFrac = std::max(0.0f, std::min(1.0f, dayFrac));
    float r=0.62f, g=0.815f, b=0.851f;
    for (int i=0;i<kSkyKeyframeCount-1;i++){
        if (dayFrac >= kSkyKeyframes[i].frac && dayFrac <= kSkyKeyframes[i+1].frac){
            float span = kSkyKeyframes[i+1].frac - kSkyKeyframes[i].frac;
            float t = span > 1e-6f ? (dayFrac - kSkyKeyframes[i].frac) / span : 0.0f;
            r = kSkyKeyframes[i].r + (kSkyKeyframes[i+1].r - kSkyKeyframes[i].r) * t;
            g = kSkyKeyframes[i].g + (kSkyKeyframes[i+1].g - kSkyKeyframes[i].g) * t;
            b = kSkyKeyframes[i].b + (kSkyKeyframes[i+1].b - kSkyKeyframes[i].b) * t;
            break;
        }
    }
    // Hujan -- desaturasi ke abu-abu (BUKAN cuma gelap, hujan tropis siang
    // hari tetap terang tapi mendung/pucat, beda dari malam yg gelap total).
    // Blend 55% ke abu-abu netral, terlepas waktu hari.
    if (isRaining){
        float gray = (r+g+b) / 3.0f * 0.85f; // sedikit diredam jg (mendung)
        r += (gray-r) * 0.55f; g += (gray-g) * 0.55f; b += (gray-b) * 0.55f;
    }
    glClearColor(r, g, b, 1.0f);
}

// Kamera Gameplay Mode (third-person) -- dipanggil tiap frame SEBELUM
// beginFrame() dgn posisi avatar TERKINI (dari engine.playerAvatar()).
// TAK perlu panggil updateViewProj() manual di sini -- beginFrame() sudah
// memanggilnya tiap frame, cukup pastikan fungsi ini dipanggil DULU pada
// frame yg sama spy state g_avatarCamEye*/g_avatarLookAt* sudah ter-update.
// effectiveDistBehind: jarak SETELAH dikoreksi collision pohon (dari
// engine.cameraSafeDistance() -- JNI/EngineBridge memanggilnya SEBELUM
// fungsi ini, memakai getAvatarCamDistBehind() sbg jarak yg diinginkan).
void updatePlayerCamera(float playerX, float playerZ, float playerFacingRad, float effectiveDistBehind){
    updateThirdPersonCamera(playerX, playerZ, playerFacingRad, 0.033f, effectiveDistBehind); // 0.033f sama dgn increment g_animT di beginFrame()
}

void beginFrame(){
    updateViewProj();
    glClear(GL_COLOR_BUFFER_BIT);
    g_animT += 0.033f; // aproksimasi ~30fps; cukup utk animasi bob/hentak, tak perlu presisi wall-clock
}

// Efek visual hujan (garis-garis jatuh) -- fitur baru diminta pengguna:
// "harus menampilkan view hujan (efek hujan pada display permainan)".
// Partikel DIPOSISIKAN RELATIF KAMERA SAAT INI (bukan koordinat dunia
// tetap) -- supaya hujan selalu terlihat MENUTUPI SELURUH LAYAR terlepas
// kamera sedang pan/zoom ke mana (kalau posisi tetap di dunia, hujan akan
// "tertinggal" saat kamera bergerak jauh, terlihat aneh/tak merata).
// Animasi jatuh berulang (looping) berbasis g_animT -- tiap tetes py fase
// awal ACAK-TAPI-TETAP (deterministik dari indeks) spy tak semua tetes
// jatuh serentak (terlihat tak alami kalau semua sinkron).
void drawRainEffect(){
    float camX, camZ;
    if (g_gameplayModeActive){ camX = g_avatarCamEyeX; camZ = g_avatarCamEyeZ; }
    else { camX = g_panX; camZ = g_panZ; }

    const int DROP_COUNT = 80; // cukup rapat utk kesan hujan, tak berlebihan
    const float AREA_RADIUS = 30.0f; // mencakup area pandang tipikal
    const float FALL_HEIGHT = 14.0f;
    const float FALL_SPEED = 9.0f; // unit/detik, jatuh cukup cepat/jelas
    std::vector<float> drops;
    for (int i=0;i<DROP_COUNT;i++){
        unsigned seed = (unsigned)i * 2654435761u;
        seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5;
        float dx = ((seed & 0xFFFF) / 65535.0f - 0.5f) * 2.0f * AREA_RADIUS;
        seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5;
        float dz = ((seed & 0xFFFF) / 65535.0f - 0.5f) * 2.0f * AREA_RADIUS;
        seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5;
        float phase = (seed & 0xFFFF) / 65535.0f; // fase awal acak-tapi-tetap per tetes
        float y = FALL_HEIGHT * (1.0f - std::fmod(g_animT * FALL_SPEED / FALL_HEIGHT + phase, 1.0f));
        float px = camX + dx, pz = camZ + dz;
        // Garis tipis pendek (bukan titik) -- kesan "jatuh cepat", 2 segitiga tipis
        V3 top{px, y+0.4f, pz}, bot{px, y, pz}, botR{px+0.02f, y, pz+0.02f};
        pushTri(drops, top, bot, botR);
    }
    drawTris(drops.data(), (int)(drops.size()/3), 0.75f, 0.82f, 0.90f, 0.55f, 0,0,0, 1,1,1);
}

// Forward declaration -- dibutuhkan drawSun() (di bawah), implementasi
// lengkap ada LEBIH JAUH ke bawah dalam file (mengatasi laporan bug forward
// declaration serupa sebelumnya utk drawPalm()/LOD system).
void appendOctahedron(std::vector<float>& out, V3 c, float r);

// Matahari -- terlihat SAAT SIANG (fitur baru diminta pengguna: "siang
// (adanya matahari)"), TIDAK terlihat malam gelap. Diposisikan RELATIF
// KAMERA (spt drawRainEffect()) -- kamera ortografis tilt TIDAK punya
// perspektif jauh/dekat spt kamera biasa, jadi matahari yg "ditanam" di
// koordinat dunia tetap akan bergeser aneh saat kamera pan -- relatif
// kamera memberi kesan "menempel di langit" yg benar. Lintasan mengikuti
// dayFrac: RENDAH di horizon saat fajar/senja (dayFrac dekat 0.25/0.85),
// TINGGI saat tengah hari (dayFrac dekat 0.575, tengah rentang Siang) --
// meniru lintasan matahari sesungguhnya (rendah pagi/sore, tinggi siang).
void drawSun(float dayFrac){
    // Rentang terlihat: SEDIKIT lebih lebar dari kategori "Siang" murni
    // (0.40-0.75) -- mencakup jg fajar & senja (matahari SUDAH/MASIH
    // terlihat saat langit mulai/baru selesai oranye, bukan cuma saat
    // biru penuh) tapi TIDAK saat malam gelap (dayFrac<0.20 / >0.90).
    if (dayFrac < 0.20f || dayFrac > 0.90f) return;

    float camX, camZ;
    if (g_gameplayModeActive){ camX = g_avatarCamEyeX; camZ = g_avatarCamEyeZ; }
    else { camX = g_panX; camZ = g_panZ; }

    // Posisi lintasan: t=0 di awal rentang terlihat (horizon timur), t=1
    // di akhir (horizon barat) -- ketinggian mengikuti kurva sin (rendah
    // di kedua ujung/horizon, puncak di tengah/tengah hari).
    float t = (dayFrac - 0.20f) / (0.90f - 0.20f);
    float sunHeight = 6.0f + std::sin(t * 3.14159265f) * 14.0f; // 6 (horizon) .. 20 (puncak)
    float sunOffsetX = (t - 0.5f) * 40.0f; // bergerak melintasi langit timur->barat
    float sunX = camX + sunOffsetX, sunZ = camZ - 25.0f; // agak di "belakang" pandangan default

    // Opacity memudar di dekat ujung rentang (terbit/terbenam halus, bukan
    // muncul/hilang tiba-tiba) -- fade 15% pertama & terakhir dari rentang.
    float alpha = 1.0f;
    if (t < 0.15f) alpha = t/0.15f;
    else if (t > 0.85f) alpha = (1.0f-t)/0.15f;

    // Warna: kuning cerah siang, oranye lebih hangat dekat horizon (fajar/senja).
    float warmth = 1.0f - std::sin(t * 3.14159265f); // 1.0 di horizon (kedua ujung), 0.0 di puncak
    float r = 1.0f, g = 0.95f - warmth*0.25f, b = 0.55f - warmth*0.35f;

    std::vector<float> sun;
    appendOctahedron(sun, {sunX, sunHeight, sunZ}, 2.2f);
    drawTris(sun.data(), (int)(sun.size()/3), r, g, std::max(0.0f,b), alpha, 0,0,0, 1,1,1);
}

// Latar belakang perbukitan bergelombang -- fitur baru diminta pengguna:
// "batasan layar yang berwarna biru terang saat ini harusnya
// dikomposisikan pada background lain yang cocok, semisal pegunungan,
// alam bebatuan (cari literatur yang cocok secara ilmiah)". SEBELUMNYA
// area di luar tanah kebun cuma glClearColor polos (langit tanpa apapun
// sampai horizon) -- terlihat "kosong"/kebun seolah mengambang di ruang
// hampa.
//
// RISET DULU (sesuai prinsip proyek: akurasi > estetika sembarangan):
// Pangudijatno & Purba (1987) -- "Topografi yang sesuai untuk tanaman
// kelapa sawit adalah datar sampai berombak, kemiringan 0-5 derajat".
// Kebun sawit TAK PERNAH ditanam di lereng gunung curam -- literatur
// topografi (AGROFORETECH, jurnal produktivitas sawit) menyebut variasi
// lokasi nyata: Rendahan/Lowland, Dataran/Flatland, Perbukitan/Hill Area
// (Sumatra Barat "topografi lebih berbukit", Kalimantan Tengah "sebagian
// besar dataran rendah"). Jadi latar belakang yg AKURAT adalah PERBUKITAN
// LANDAI bergelombang di kejauhan (horizon), BUKAN pegunungan terjal --
// kebun ITU SENDIRI tetap di dataran datar/landai (tak berubah, sesuai
// tanam nyata), perbukitan cuma elemen dekoratif JAUH di garis horizon.
//
// Perspektif atmosfer (Rayleigh scattering, prinsip optik universal --
// bukan spesifik botani) -- objek jauh tampak lebih biru/pucat/desaturasi
// drpd warna asli krn hamburan cahaya oleh partikel udara pada jarak
// pandang panjang. 2 lapis: bukit "dekat" (radius dalam) hijau-kecoklatan
// lebih jelas, bukit "jauh" (radius luar) biru-abu pudar/berkabut.
//
// Diposisikan RELATIF KAMERA (spt drawSun()/drawRainEffect()) -- cincin
// PENUH 360 derajat mengelilingi kamera pada radius besar (jauh melebihi
// kLodLowMaxDist=173.4 & area render pohon manapun), supaya SELALU
// terlihat mengisi horizon dari sudut manapun kamera diputar (fitur "lihat
// 360 derajat" yg sudah ada), TANPA parallax aneh saat kamera pan (radius
// jauh membuat pergeseran relatif nyaris tak terlihat, spt gunung
// sungguhan yg terlihat "diam" meski kita bergerak beberapa km).
void drawDistantHills(float dayFrac){
    float camX, camZ;
    if (g_gameplayModeActive){ camX = g_avatarCamEyeX; camZ = g_avatarCamEyeZ; }
    else { camX = g_panX; camZ = g_panZ; }

    // Warna dasar berubah sesuai waktu hari (konsisten dgn setSkyState())
    // -- lebih hangat/kecoklatan saat fajar-senja, lebih gelap saat malam,
    // biru-hijau normal saat siang.
    bool isDark = (dayFrac < 0.20f || dayFrac > 0.90f);
    float dimFactor = isDark ? 0.35f : 1.0f; // bukit jauh lebih gelap malam hari (siluet, bukan hilang total)

    const int SEGMENTS = 28;
    const float RADIUS_NEAR = 150.0f, RADIUS_FAR = 210.0f;

    // Lapis JAUH (biru-abu pudar, kabut/haze) -- digambar DULU (paling belakang)
    std::vector<float> farHills;
    for (int s=0;s<SEGMENTS;s++){
        unsigned seed = (unsigned)s * 4256249301u ^ 0x1337BEEFu;
        seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5;
        float heightVar = 8.0f + (seed & 0xFFFF)/65535.0f * 10.0f; // 8-18 unit -- bergelombang landai, BUKAN puncak tajam
        float a0 = (float)s/SEGMENTS*6.28318f, a1=(float)(s+1)/SEGMENTS*6.28318f;
        float amid = (a0+a1)*0.5f;
        V3 base0{camX+std::cos(a0)*RADIUS_FAR, 0.0f, camZ+std::sin(a0)*RADIUS_FAR};
        V3 base1{camX+std::cos(a1)*RADIUS_FAR, 0.0f, camZ+std::sin(a1)*RADIUS_FAR};
        V3 peak{camX+std::cos(amid)*RADIUS_FAR, heightVar, camZ+std::sin(amid)*RADIUS_FAR};
        pushTri(farHills, base0, base1, peak);
    }
    float farR=0.62f*dimFactor, farG=0.68f*dimFactor, farB=0.78f*dimFactor; // biru-abu berkabut
    drawTris(farHills.data(), (int)(farHills.size()/3), farR, farG, farB, 0.75f, 0,0,0, 1,1,1);

    // Lapis DEKAT (hijau-kecoklatan, vegetasi hutan tropis khas sekitar
    // kebun sawit Sumatra/Kalimantan) -- digambar SETELAH lapis jauh
    // (menimpa sebagian, memberi kesan kedalaman/lapisan).
    std::vector<float> nearHills;
    for (int s=0;s<SEGMENTS;s++){
        unsigned seed = (unsigned)s * 2971215073u ^ 0xC0FFEEu;
        seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5;
        float heightVar = 5.0f + (seed & 0xFFFF)/65535.0f * 7.0f; // 5-12 unit -- lebih rendah dr lapis jauh
        float a0 = (float)s/SEGMENTS*6.28318f, a1=(float)(s+1)/SEGMENTS*6.28318f;
        float amid = (a0+a1)*0.5f;
        V3 base0{camX+std::cos(a0)*RADIUS_NEAR, 0.0f, camZ+std::sin(a0)*RADIUS_NEAR};
        V3 base1{camX+std::cos(a1)*RADIUS_NEAR, 0.0f, camZ+std::sin(a1)*RADIUS_NEAR};
        V3 peak{camX+std::cos(amid)*RADIUS_NEAR, heightVar, camZ+std::sin(amid)*RADIUS_NEAR};
        pushTri(nearHills, base0, base1, peak);
    }
    float nearR=0.32f*dimFactor, nearG=0.42f*dimFactor, nearB=0.30f*dimFactor; // hijau-kecoklatan hutan tropis
    drawTris(nearHills.data(), (int)(nearHills.size()/3), nearR, nearG, nearB, 0.85f, 0,0,0, 1,1,1);
}

void drawGround(float originX, float originZ){
    // Dasar tanah: coklat/pasir — meniru warna permukaan tanah kebun sawit
    // (lateritic, umum di kebun Indonesia). Diperlebar dari versi sebelumnya
    // supaya menutupi grid 143 pokok (11 kolom x 13 baris, lihat engine.cpp).
    // originX/originZ: pusat grid block INI (bukan selalu 0,0 lagi -- block
    // baru dari beliHa() digeser jauh di X, perlu tanahnya sendiri jg).
    //
    // BUG "pola bergerigi di horizon" diperbaiki (dilaporkan pengguna via
    // video: "ketika kamera digerakkan gambar seperti tidak rendering
    // sempurna ketika kita punya banyak lahan 30 ha"). AKAR MASALAH
    // ditemukan lewat analisis frame video & sampling piksel warna: lebar
    // ground SEBELUMNYA cuma 90 unit (X: origin+-45), TAPI jarak antar
    // pusat block = 120 unit (newOriginX = index*120.0, engine.cpp) --
    // ada CELAH 30 unit (X: origin+45 sampai origin+75 relatif block
    // berikutnya) TANPA GROUND SAMA SEKALI di antara tiap block
    // bersebelahan. Di Management Mode (kamera ortografis top-down)
    // celah ini nyaris tak terlihat (sudut pandang dari atas), TAPI di
    // Gameplay Mode (kamera third-person sejajar tanah), celah berulang
    // dari BANYAK block (30ha = 30 block berjejer) menghasilkan garis
    // horizon yg terlihat "bertangga"/bergerigi persis spt yg dilaporkan.
    // Diperbaiki: lebar ground X DIPERLEBAR dari 45 ke 60 (setengah dari
    // 120, jarak antar block) -- tepi block SEKARANG bertemu PERSIS tanpa
    // celah/overlap (block originX=0: X=[-60,60], block originX=120:
    // X=[60,180], bertemu tepat di X=60). Z TETAP tak berubah (38) --
    // TIDAK ADA block tetangga di arah Z manapun (semua block sejajar
    // horizontal di sumbu X saja, originZ selalu 0.0).
    float quad[] = {
        originX-60,0,originZ-38,  originX+60,0,originZ-38,  originX+60,0,originZ+38,
        originX-60,0,originZ-38,  originX+60,0,originZ+38,  originX-60,0,originZ+38,
    };
    // Dihangatkan sedikit dari versi sebelumnya (#8a7355 -> #a17b4e) -- lebih
    // kaya/cerah spt tanah kering tersinari matahari tropis, TETAP tanah
    // lateritik realistis (poles kontras, BUKAN oranye kartun penuh spt
    // referensi visual game kasual -- keputusan: pertahankan akurasi sains).
    drawTris(quad, 6, 0.631f, 0.482f, 0.306f, 1.0f, 0,0,0, 1,1,1);

    // Bercak variasi tanah -- pecah kesan warna FLAT/rata satu warna, meniru
    // variasi kelembapan/kepadatan/warna tanah sungguhan (sebagian lbh gelap
    // krn lembap/bekas injakan, sebagian lbh terang krn kering, sedikit
    // kehijauan krn lumut/gulma tipis di sela). Digabung per-jenis jd 1 draw
    // call spy tetap ringan. Seed ikut originX/Z spy tiap block polanya beda.
    {
        unsigned seedV = 0xB17Cu ^ (unsigned)(originX*271.0f) ^ (unsigned)(originZ*337.0f);
        auto frandV=[&](){ seedV^=seedV<<13; seedV^=seedV>>17; seedV^=seedV<<5; return (seedV&0xFFFFFF)/float(0xFFFFFF); };
        std::vector<float> darkPatches, lightPatches, mossPatches;
        for (int i=0;i<70;i++){
            float px = originX + (frandV()-0.5f)*118.0f; // DIPERLEBAR (dari 88) konsisten lebar ground baru (60), lihat catatan lengkap di awal drawGround()
            float pz = originZ + (frandV()-0.5f)*74.0f;
            float r = 1.5f + frandV()*3.5f;
            int sides = 7;
            std::vector<float>* target;
            float pick = frandV();
            if (pick < 0.5f) target = &darkPatches;      // lbh gelap (lembap/bekas injakan) -- paling umum
            else if (pick < 0.85f) target = &lightPatches; // lbh terang (kering)
            else target = &mossPatches;                    // kehijauan tipis (lumut/gulma jarang)
            for (int s=0;s<sides;s++){
                float a0=(float)s/sides*6.28318f, a1=(float)(s+1)/sides*6.28318f;
                float rr0=r*(0.75f+frandV()*0.25f), rr1=r*(0.75f+frandV()*0.25f); // tepi tak rata sempurna
                V3 c{px,0.002f,pz}, p0{px+std::cos(a0)*rr0,0.002f,pz+std::sin(a0)*rr0}, p1{px+std::cos(a1)*rr1,0.002f,pz+std::sin(a1)*rr1};
                pushTri(*target, c, p0, p1);
            }
        }
        if (!darkPatches.empty())  drawTris(darkPatches.data(),  (int)(darkPatches.size()/3),  0.451f,0.365f,0.255f, 0.35f, 0,0,0, 1,1,1);
        if (!lightPatches.empty()) drawTris(lightPatches.data(), (int)(lightPatches.size()/3), 0.616f,0.529f,0.404f, 0.35f, 0,0,0, 1,1,1);
        if (!mossPatches.empty())  drawTris(mossPatches.data(),  (int)(mossPatches.size()/3),  0.376f,0.400f,0.220f, 0.28f, 0,0,0, 1,1,1);
    }

    // Bunga hias di TEPI/batas block -- elemen yg diadopsi dari referensi
    // visual game kasual yg diberikan pengguna, TAPI diadaptasi supaya tetap
    // berdasar realita lapangan (bukan tempelan gaya kasual tanpa alasan):
    // kebun sawit sungguhan LAZIM punya tanaman hias/pagar hidup di sepanjang
    // jalan UTAMA/batas block sbg penanda visual & penahan erosi tepi jalan
    // (bukan di gawangan antar-baris tanam, itu tetap murni mulsa spt di
    // atas). Ditempatkan di 4 SISI LUAR ground quad SAJA (bukan menyebar ke
    // seluruh kebun), warna2 cerah beragam spt kebun hias sungguhan.
    {
        unsigned seedF = 0xF107E4u ^ (unsigned)(originX*419.0f) ^ (unsigned)(originZ*613.0f);
        auto frandF=[&](){ seedF^=seedF<<13; seedF^=seedF>>17; seedF^=seedF<<5; return (seedF&0xFFFFFF)/float(0xFFFFFF); };
        std::vector<float> pink, yellow, purple, white;
        auto addFlower = [&](float fx, float fz, std::vector<float>& target){
            float r = 0.28f + frandF()*0.18f;
            int petals = 5;
            for (int s=0;s<petals;s++){
                float a0=(float)s/petals*6.28318f, a1=(float)(s+1)/petals*6.28318f;
                V3 c{fx,0.006f,fz}, p0{fx+std::cos(a0)*r,0.006f,fz+std::sin(a0)*r}, p1{fx+std::cos(a1)*r,0.006f,fz+std::sin(a1)*r};
                pushTri(target, c, p0, p1);
            }
        };
        // 4 sisi luar ground quad (X=originX+-45, Z=originZ+-38), spasi ~2.2
        // unit di sepanjang tepi, digeser sedikit ke DALAM (bukan pas di garis
        // luar) spy tak terlihat "mengambang" di luar tanah.
        float half_w = 59.0f, half_h = 37.0f, inset = 1.2f, spacing = 2.3f; // half_w DIPERLEBAR (dari 44) konsisten dgn lebar ground baru (60)
        for (float x=-half_w+inset; x<half_w-inset; x+=spacing){
            float jit = (frandF()-0.5f)*0.6f;
            int pick = (int)(frandF()*4.0f);
            std::vector<float>& t1 = pick==0?pink:(pick==1?yellow:(pick==2?purple:white));
            addFlower(originX+x+jit, originZ-half_h+inset, t1);
            pick = (int)(frandF()*4.0f);
            std::vector<float>& t2 = pick==0?pink:(pick==1?yellow:(pick==2?purple:white));
            addFlower(originX+x+jit, originZ+half_h-inset, t2);
        }
        for (float z=-half_h+inset; z<half_h-inset; z+=spacing){
            float jit = (frandF()-0.5f)*0.6f;
            int pick = (int)(frandF()*4.0f);
            std::vector<float>& t1 = pick==0?pink:(pick==1?yellow:(pick==2?purple:white));
            addFlower(originX-half_w+inset, originZ+z+jit, t1);
            pick = (int)(frandF()*4.0f);
            std::vector<float>& t2 = pick==0?pink:(pick==1?yellow:(pick==2?purple:white));
            addFlower(originX+half_w-inset, originZ+z+jit, t2);
        }
        if (!pink.empty())   drawTris(pink.data(),   (int)(pink.size()/3),   0.859f,0.263f,0.427f, 1.0f, 0,0,0, 1,1,1);
        if (!yellow.empty()) drawTris(yellow.data(), (int)(yellow.size()/3), 0.949f,0.792f,0.184f, 1.0f, 0,0,0, 1,1,1);
        if (!purple.empty()) drawTris(purple.data(), (int)(purple.size()/3), 0.541f,0.298f,0.639f, 1.0f, 0,0,0, 1,1,1);
        if (!white.empty())  drawTris(white.data(),  (int)(white.size()/3),  0.976f,0.965f,0.918f, 1.0f, 0,0,0, 1,1,1);
    }

    // Legume Cover Crop (LCC) -- tanaman kacangan penutup tanah (Mucuna
    // bracteata, Calopogonium mucunoides, Pueraria javanica, dll) yg WAJIB
    // ditanam di kebun sawit sungguhan (Pusat Penelitian Kelapa Sawit,
    // "Tanaman penutup tanah dan gulma pada kebun kelapa sawit") -- menekan
    // gulma liar, cegah erosi, perbaiki kesuburan tanah. Mengatasi keluhan
    // review eksternal: "Terrain: warna coklat relatif flat -> scene terasa
    // generik" -- SEBELUMNYA cuma variasi WARNA 2D (bercak gelap/terang di
    // atas), belum ada elemen HIJAU 3D sungguhan menutupi tanah sama sekali.
    // Daun TRIFOLIATE (3 helai, ciri khas kacangan/leguminosa) kecil &
    // hampir rata tanah (tanaman MERAMBAT, bukan tegak spt rumput), warna
    // hijau-kekuningan (sesuai deskripsi lapangan) tersebar RAPAT di
    // seluruh ground -- KECUALI di gawangan mati (sudah tertutup mulsa
    // pelepah, drawFrondPile()) & radius dekat batang pohon (pokok muda
    // butuh piringan bersih dari gulma/LCC, praktik SOP "piringan").
    {
        unsigned seedL = 0x1CC0000u ^ (unsigned)(originX*823.0f) ^ (unsigned)(originZ*911.0f);
        auto frandL=[&](){ seedL^=seedL<<13; seedL^=seedL>>17; seedL^=seedL<<5; return (seedL&0xFFFFFF)/float(0xFFFFFF); };
        std::vector<float> lcc;
        for (int i=0;i<220;i++){
            float lx = originX + (frandL()-0.5f)*118.0f; // DIPERLEBAR (dari 88), lihat catatan di awal drawGround()
            float lz = originZ + (frandL()-0.5f)*74.0f;
            // BUG diperbaiki: versi sblmnya skip berdasar jarak ke GARIS baris
            // PENUH (sepanjang SELURUH X) -- terverifikasi numerik 63% area
            // ter-skip (seharusnya piringan cuma lingkaran KECIL di sekitar
            // TITIK pohon spesifik, bukan garis penuh sepanjang baris).
            // Sekarang hitung jarak 2D ke TITIK grid pohon terdekat (bulatkan
            // ke kelipatan col/row spacing, pola segitiga: baris ganjil
            // digeser setengah kolSpacing, sesuai formula grid Engine).
            const float kColSpacingLcc = 5.2f, kRowSpacingLcc2 = 4.507f;
            float relX = lx - originX, relZ = lz - originZ;
            int nearRow = (int)std::round(relZ / kRowSpacingLcc2);
            float rowOffsetX = ((nearRow % 2) != 0) ? kColSpacingLcc*0.5f : 0.0f; // pola segitiga
            int nearCol = (int)std::round((relX - rowOffsetX) / kColSpacingLcc);
            float nearestTreeX = originX + nearCol*kColSpacingLcc + rowOffsetX;
            float nearestTreeZ = originZ + nearRow*kRowSpacingLcc2;
            float distToNearestTree = std::sqrt((lx-nearestTreeX)*(lx-nearestTreeX) + (lz-nearestTreeZ)*(lz-nearestTreeZ));
            if (distToNearestTree < 1.4f) continue; // dekat batang pohon -- piringan bersih SOP, skip
            float leafR = 0.11f + frandL()*0.07f;
            float rot = frandL()*6.28318f;
            for (int leaf=0; leaf<3; leaf++){ // trifoliate -- 3 helai per rumpun, ciri khas leguminosa
                float leafAngle = rot + leaf*2.0944f; // 120 derajat antar helai
                float lcx = lx + std::cos(leafAngle)*leafR*0.7f;
                float lcz = lz + std::sin(leafAngle)*leafR*0.7f;
                const int sides=6;
                for (int s=0;s<sides;s++){
                    float a0=(float)s/sides*6.28318f, a1=(float)(s+1)/sides*6.28318f;
                    V3 c{lcx,0.008f,lcz}, p0{lcx+std::cos(a0)*leafR,0.008f,lcz+std::sin(a0)*leafR}, p1{lcx+std::cos(a1)*leafR,0.008f,lcz+std::sin(a1)*leafR};
                    pushTri(lcc, c, p0, p1);
                }
            }
        }
        if (!lcc.empty()) drawTris(lcc.data(), (int)(lcc.size()/3), 0.322f,0.529f,0.216f, 0.85f, 0,0,0, 1,1,1); // hijau LCC
    }

    // Batu kecil tersebar (rock) -- melengkapi daftar material poin dokumen
    // review #7: "Campurkan soil, grass, mud, road, leaf litter, dan rock
    // melalui shader". SEBELUMNYA tak ada elemen batu sama sekali di tanah
    // kebun. Jauh lebih JARANG dari LCC/bunga (batu bukan elemen dominan di
    // kebun sawit terawat, cuma sesekali muncul dari tanah berbatu/lateritik
    // alami) -- bentuk poligon TAK beraturan (radius per-sisi acak, BUKAN
    // lingkaran rapi spt bunga/LCC) utk kesan padat/keras, abu-abu netral.
    {
        unsigned seedR = 0x8A0C4Bu ^ (unsigned)(originX*601.0f) ^ (unsigned)(originZ*719.0f);
        auto frandR=[&](){ seedR^=seedR<<13; seedR^=seedR>>17; seedR^=seedR<<5; return (seedR&0xFFFFFF)/float(0xFFFFFF); };
        std::vector<float> rocks;
        for (int i=0;i<18;i++){ // jauh lebih jarang drpd LCC (220) -- elemen aksen, bukan dominan
            float rx = originX + (frandR()-0.5f)*118.0f; // DIPERLEBAR (dari 88), lihat catatan di awal drawGround()
            float rz = originZ + (frandR()-0.5f)*74.0f;
            float baseR = 0.10f + frandR()*0.16f;
            int sides = 6;
            float rot = frandR()*6.28318f;
            for (int s=0;s<sides;s++){
                float a0=rot+(float)s/sides*6.28318f, a1=rot+(float)(s+1)/sides*6.28318f;
                float rr0=baseR*(0.7f+frandR()*0.5f), rr1=baseR*(0.7f+frandR()*0.5f); // sisi tak beraturan
                V3 c{rx,0.006f,rz}, p0{rx+std::cos(a0)*rr0,0.006f,rz+std::sin(a0)*rr0}, p1{rx+std::cos(a1)*rr1,0.006f,rz+std::sin(a1)*rr1};
                pushTri(rocks, c, p0, p1);
            }
        }
        if (!rocks.empty()) drawTris(rocks.data(), (int)(rocks.size()/3), 0.475f,0.463f,0.443f, 1.0f, 0,0,0, 1,1,1); // abu-abu netral
    }

    // BUG diperbaiki: sebelumnya ADA JUGA implementasi mulsa gawangan mati
    // DI SINI (quad datar warna #564b37), yg TERNYATA tumpang tindih PERSIS
    // (terverifikasi numerik: 6 dari 6 posisi identik) dgn drawFrondPile()
    // (renderer_gl.cpp, dipanggil terpisah dari JNI/EngineBridge) -- dua
    // sistem visual berbeda utk konsep yg SAMA digambar di posisi SAMA,
    // memboroskan geometri & berpotensi terlihat aneh (dua warna/bentuk
    // saling menimpa). drawFrondPile() JAUH lebih detail (batang 3D
    // sungguhan, bukan quad datar tipis) -- versi lama DIHAPUS di sini,
    // biarkan drawFrondPile() jadi SATU-SATUNYA sumber visual mulsa.
}

// Forward declaration -- dibutuhkan LOD system di drawPalm() (di bawah),
// implementasi lengkap kedua fungsi ini ada LEBIH JAUH ke bawah dalam file.
void buildCylinder(std::vector<float>& out, float radius, float height, int sides);
void appendOctahedron(std::vector<float>& out, V3 c, float r);

void drawPalm(float x, float z, float ageYears, float frond, int health, int ffb, bool selected, float nutrition){
    float trunkH = treeTrunkHeight(ageYears); // satu sumber kebenaran, dipakai jg oleh hitTestDistance & JNI/bridge

    // LOD (Level of Detail) system -- fitur baru diminta pengguna: "kebun
    // bisa memiliki ratusan/ribuan pohon... Jangan melakukan draw call
    // individual utk setiap pohon jika jumlahnya besar". Dihitung SEDINI
    // MUNGKIN (sebelum warna/seed/geometri lain yg lebih mahal dihitung)
    // supaya tier HIDE bisa return SEGERA tanpa komputasi apapun lagi.
    // Lihat catatan lengkap threshold (kLodHighMaxDist dkk, skala meter
    // asli -> unit dunia game) di deklarasi konstanta, dekat awal file.
    float lodDist = cameraDistanceToPoint(x, z);
    if (lodDist > kLodLowMaxDist){
        // HIDE (>300m asli) -- sebagian besar KASUS SUDAH tertangani
        // frustum culling isWorldPointVisible() di caller (JNI/EngineBridge),
        // ini lapis TAMBAHAN utk kasus zoom-out ekstrim (Management Mode)
        // di mana objek jauh masih lolos krn isWorldPointVisible berbasis
        // proyeksi LAYAR, bukan jarak murni dari kamera.
        return;
    }
    enum class LodTier { High, Medium, Low };
    LodTier lod = (lodDist <= kLodHighMaxDist) ? LodTier::High
                : (lodDist <= kLodMediumMaxDist) ? LodTier::Medium
                : LodTier::Low;

    // Skala: mesh STL asli tingginya kPalmIconRefHeight (~5.02) pada proporsi
    // acuan trunkH=2.55. Diredam (S_crown) spt versi sebelumnya supaya pokok
    // tua tidak membengkak & bertabrakan dgn tetangga (jarak tanam 5.2 unit).
    const float S = trunkH / 2.55f;
    const float S_crown = 1.0f + (S-1.0f)*0.4f;
    (void)kPalmIconRefHeight;

    // --- warna batang: COKLAT (dulu ikut hijau krn klasifikasi lama cuma pakai
    //     tanda X, bukan tinggi — sudah diperbaiki di palm_icon_mesh.hpp v3) ---
    float trunkR=0.596f, trunkG=0.451f, trunkB=0.271f; // coklat, dihangatkan sedikit dr versi sblmnya
    if (health==3 /*mati*/) { trunkR=0.35f; trunkG=0.29f; trunkB=0.19f; }

    // --- warna mahkota/pelepah: dua nuansa hijau, bergeser mengikuti kesehatan ---
    // Saturasi dinaikkan & jarak terang-gelap DILEBARKAN (dulu crown_dark
    // ~70-80% dari crown_light, kini ~50-55%) -- kanopi terasa lebih
    // berdimensi/"hidup", meniru kesan visual referensi pengguna TANPA
    // mengubah bentuk/geometri sama sekali (cuma warna).
    float gR=0.086f, gG=0.694f, gB=0.263f;      // hijau terang -> grup "crown_light" (lbh vivid dr sblmnya)
    float gdR=0.043f, gdG=0.376f, gdB=0.145f;   // hijau gelap  -> grup "crown_dark" (kontras lbh tegas)
    if (health==1) { gR=0.596f; gG=0.663f; gB=0.145f; gdR=gR*0.55f; gdG=gG*0.55f; gdB=gB*0.55f; }       // hama -> kekuningan
    else if (health==2) { gR=0.388f; gG=0.463f; gB=0.204f; gdR=gR*0.55f; gdG=gG*0.55f; gdB=gB*0.55f; }  // ganoderma -> layu
    else if (health==3) { gR=0.42f; gG=0.35f; gB=0.24f; gdR=gR*0.7f; gdG=gG*0.7f; gdB=gB*0.7f; }  // mati -> coklat
    else {
        // Pohon SEHAT tapi nutrisi rendah -> warna kanopi PUCAT/kusam, bukan
        // cuma lebih kecil (vigor, lihat bawah) -- dokumen desain eksplisit:
        // "Nutrient Stress: sedikit lebih sparse / PALE". Diblend ke arah
        // hijau-kuning kusam sebanding (1-nutrition), maks 55% blend spy
        // tetap terbaca sbg "sehat" bukan sakit (itu jatah health==1/2).
        float paleness = (1.0f - nutrition) * 0.55f;
        float paleR=0.58f, paleG=0.58f, paleB=0.40f;
        gR += (paleR-gR)*paleness; gG += (paleG-gG)*paleness; gB += (paleB-gB)*paleness;
        gdR += (paleR*0.82f-gdR)*paleness; gdG += (paleG*0.82f-gdG)*paleness; gdB += (paleB*0.82f-gdB)*paleness;
    }

    // --- semburat kering/coklat sebanding akumulasi pelepah tua BLM ditunas
    //     -- Corley & Tinker (2016) Tabel 11.4: "dead leaves only, annually"
    //     (pemangkasan RUTIN pelepah mati) hasilkan panen TERBAIK (8,5 t/ha),
    //     dibanding tanpa pemangkasan sama sekali (8,0 t/ha) atau pemangkasan
    //     berlebihan sampai ke tandan (turun ke 6,8-7,2 t/ha) -- pelepah
    //     kering yg MENUMPUK (blm dipangkas) scr visual makin kusam/coklat,
    //     kembali bersih PERSIS setelah ditunas (frond direset ke 0.08 di
    //     engine.cpp). Diterapkan di LUAR blok kesehatan di atas (independen
    //     dr status sehat/sakit -- pohon sakit pun bisa menumpuk pelepah
    //     kering blm sempat dipangkas). Sebelumnya parameter frond dibuang
    //     sepenuhnya di renderer (void)frond -- tak ada perubahan visual sm
    //     sekali stlh aksi Tunas, dilaporkan pengguna.
    float deadTint = std::min(1.0f, frond) * 0.35f; // maks 35% blend, spy tetap halus/tak berlebihan
    float dryR=0.42f, dryG=0.33f, dryB=0.18f; // coklat kering pelepah tua
    gR += (dryR-gR)*deadTint; gG += (dryG-gG)*deadTint; gB += (dryB-gB)*deadTint;
    gdR += (dryR*0.85f-gdR)*deadTint; gdG += (dryG*0.85f-gdG)*deadTint; gdB += (dryB*0.85f-gdB)*deadTint;

    // --- variasi spiral pelepah kiri/kanan antar pohon (filotaksis) ---
    // Literatur: pohon sawit sungguhan menunjukkan filotaksis spiral 5/13 (deret
    // Fibonacci) yang bisa berpilin KIRI atau KANAN, dan arahnya TIDAK ditentukan
    // genetik -- populasi terbagi mendekati 50:50 antara kedua arah (Davis 1962/63
    // utk Cocos nucifera, pola serupa dilaporkan lintas famili Arecaceae termasuk
    // Elaeis guineensis: "Trunk of Elaeis guineensis showing eight and five spirals",
    // Albakri dkk. 2019 scr eksplisit menggambarkan "(a) spiral kiri (b) spiral
    // kanan" utk sawit). Mesh gaya-ikon kita adalah kipas 5 bilah sederhana (bukan
    // spiral 5/13 penuh dgn puluhan pelepah individual -- itu di luar skala
    // renderer real-time ini), tapi kita adaptasi esensi "sebagian pohon kidal,
    // sebagian tidak" lewat CERMINAN horizontal per pohon: deterministik dari
    // posisi (x,z) supaya konsisten antar frame, bukan acak tiap render.
    unsigned seed = (unsigned)(x*977.0f) ^ (unsigned)(z*1013.0f) ^ 0x51ED270Bu;
    seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5;
    float mirrorSign = ((seed & 1) == 0) ? 1.0f : -1.0f; // ~50:50, meniru rasio literatur

    // --- variasi orientasi & ukuran PER POHON -- review eksternal poin #1:
    //     "hampir semua pohon tinggi sama, bentuk tajuk sama... kebun terlihat
    //     seperti pattern generator". Akar masalahnya: golden-angle di-bake dgn
    //     sudut awal SAMA utk semua pohon (yaw=0), jadi dari atas SEMUA pohon
    //     tampak identik orientasi kanopinya -- pola berulang yg mencolok.
    //     Sekarang tiap pohon dapat rotasi awal ACAK-TAPI-TETAP (deterministik
    //     dari posisi x,z, BUKAN acak tiap frame) + jitter ukuran ringan (±8%),
    //     murah scr performa krn cuma parameter matrix GPU (lihat drawTris),
    //     BUKAN membangun ulang geometri per pohon. ---
    // --- kerapatan kanopi merespons KESEHATAN & NUTRISI SUNGGUHAN, bukan cuma
    //     warna -- review eksternal "visualisasi state biologis": "Healthy:
    //     crown density penuh (██████████), Mild stress: menyusut sedikit
    //     (████████░░), Severe: jauh lebih sparse (█████░░░░░)". Diterapkan
    //     sbg SCALE tambahan HANYA ke kanopi+buah (bukan batang -- tinggi
    //     batang mencerminkan UMUR, bukan state kesehatan sesaat), proxy
    //     visual defisiensi N/K/air TANPA perlu mesh terpisah (sesuai
    //     rekomendasi V1: "belum perlu 3 baked mesh, pakai scale variation").
    float vigor;
    // Rentang dilebarkan dari 0.70-1.00 -- versi lama nyaris tak terlihat di
    // hari-hari awal (blm ada pohon sakit) krn tenggelam di antara jitter
    // ukuran acak (sizeJitter) yg magnitudonya mirip. 0.55 dipilih spy pohon
    // nutrisi rendah SUDAH terlihat lebih kerdil sejak hari 1, bukan cuma
    // terasa saat penyakit muncul berminggu-minggu kemudian.
    if (health == 0) vigor = 0.55f + nutrition * 0.45f;      // sehat: murni nutrisi (0.55..1.00)
    else if (health == 1) vigor = 0.80f;                      // hama: kanopi termakan, menyusut
    else if (health == 2) vigor = 0.62f;                      // ganoderma: fronds layu/patah, jauh menyusut
    else vigor = 0.45f;                                        // mati: sisa kanopi kering

    seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5; // maju state, ambil angka acak lain dr seed yg sama
    float treeYaw = (seed & 0xFFFFFF) / float(0xFFFFFF) * 6.28318f;
    seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5;
    float sizeJitter = 0.92f + (seed & 0xFFFFFF) / float(0xFFFFFF) * 0.16f; // 0.92..1.08
    // Kemiringan batang SEDIKIT dari vertikal (poin review dokumen: "Trunk
    // menggunakan taper dan sedikit curvature" & "Variasi ukuran, kemiringan,
    // umur, dan kepadatan crown ditentukan oleh seed") -- SEBELUMNYA pohon
    // SELALU tegak lurus sempurna, tak ada kemiringan sama sekali. Deterministik
    // dari seed yg SAMA (posisi x,z), konsisten antar frame. Rentang KECIL
    // (maks ±4 derajat) -- SENGAJA "sedikit" sesuai dokumen, bukan miring
    // ekstrim (pohon sawit sehat tumbuh cukup lurus, variasi alami minor krn
    // angin/kemiringan tanah, BUKAN efek visual dominan).
    seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5;
    float tiltX = ((seed & 0xFFFFFF) / float(0xFFFFFF) - 0.5f) * 2.0f * 0.0698f; // ±4 derajat (0.0698 rad)
    seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5;
    float tiltZ = ((seed & 0xFFFFFF) / float(0xFFFFFF) - 0.5f) * 2.0f * 0.0698f;
    // Kepadatan crown MURNI dari seed (poin review dokumen: "Variasi ukuran,
    // kemiringan, umur, dan kepadatan crown ditentukan oleh seed") --
    // SEBELUMNYA kepadatan visual kanopi CUMA bervariasi lewat vigor
    // (health+nutrition, lihat di atas) & sizeJitter (variasi UKURAN
    // keseluruhan batang+kanopi BERSAMAAN, bukan kanopi independen). Ini
    // TIDAK memberi variasi "kepadatan" murni acak thd pohon yg SAMA SEHAT
    // sekalipun (dua pohon sehat identik nutrisi akan terlihat SAMA PERSIS
    // kepadatan kanopinya, cuma beda ukuran total dari sizeJitter). Sekarang
    // ditambah faktor TERPISAH, HANYA memengaruhi kanopi (bukan batang) --
    // pohon sehat identik nutrisi kini tetap py variasi alami kepadatan
    // kanopi kecil (±12%), independen dari ukuran total & state kesehatan.
    seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5;
    float densitySeed = 0.92f + (seed & 0xFFFFFF) / float(0xFFFFFF) * 0.16f; // 0.92..1.08 -- rentang SAMA dgn sizeJitter (konsisten), DIKURANGI dari rencana awal +-12% ke +-8% stlh verifikasi numerik: kanopi SUDAH berpotensi tumpang tindih dgn tetangga bahkan SEBELUM faktor ini (masalah pre-existing di S_crown, BUKAN regresi baru) -- minimalkan kontribusi tambahan thd masalah yg sudah ada

    // --- mesh STL asli (1688 segitiga, dibaked di palm_icon_mesh.hpp), 4
    //     kelompok warna (trunk/crown_light/crown_dark/fruit) — masing2 SATU
    //     draw call. Skala S_crown & mirrorSign (sumbu X) diterapkan lewat
    //     parameter scale drawTris (bukan dihitung ulang per-vertex spt versi
    //     prosedural, jadi lebih ringan). ---
    float Sc = S_crown * sizeJitter;
    // BUG diperbaiki (ditemukan saat investigasi laporan pengguna: "tinggi
    // pekerja dan pohon sawit tidak sebanding"). Variabel S (=trunkH/2.55,
    // skala PENUH proporsional umur pohon) DIHITUNG tapi TAK PERNAH dipakai
    // langsung -- batang malah memakai Sc (=S_crown*sizeJitter, REDAMAN yg
    // SENGAJA utk MAHKOTA supaya tak "membengkak"/bertabrakan horizontal
    // dgn tetangga, jarak tanam 5.2 unit). Redaman ini TAK relevan utk
    // BATANG (naik VERTIKAL, tak melebar horizontal, tak bertabrakan dgn
    // tetangga manapun). Terverifikasi numerik: batang pohon TUA (12 th)
    // cuma memakai 64% skala seharusnya (S_crown/S=64%) -- pohon tua TAK
    // terlihat jauh lebih tinggi dari pohon muda spt seharusnya. Sekarang
    // batang pakai Strunk (S PENUH), mahkota TETAP pakai Sc (redaman
    // dipertahankan, alasannya masih valid utk mahkota).
    float Strunk = S * sizeJitter;
    // Sedikit lebih "penuh/berantakan" saat pelepah kering menumpuk blm
    // ditunas (maks +6%) -- bukti visual TAMBAHAN selain warna, langsung
    // kembali ke skala normal begitu ditunas (frond direset ke 0.08).
    float clutter = 1.0f + std::min(1.0f, frond) * 0.06f;
    float ScCanopy = Sc * vigor * clutter * densitySeed; // kanopi+buah ikut menyusut/membesar sesuai state & variasi alami acak, batang TIDAK

    if (lod == LodTier::Low){
        // LOW LOD (100-300m asli) -- GANTI mesh detail (4704 triangle total)
        // dgn PRIMITIF SEDERHANA: silinder 6-sisi (~24 triangle, buildCylinder())
        // + octahedron (8 triangle, appendOctahedron()) -- PENGHEMATAN >99%
        // triangle per pohon pd tier ini. Warna kesehatan/nutrisi (gR/gG/gB,
        // trunkR/G/B) TETAP diterapkan -- pohon sakit/sehat masih bisa
        // dibedakan dari kejauhan, walau tanpa detail pelepah individual.
        // Geometri di-CACHE static (dihitung SEKALI, dipakai ulang semua
        // pohon LOW LOD via parameter scale drawTris(), bukan realokasi
        // vector tiap panggilan) -- overhead per-pohon jadi HANYA 2 draw
        // call murah, tak ada alokasi memori tambahan sama sekali.
        static std::vector<float> lodTrunk = [](){
            std::vector<float> v; buildCylinder(v, 0.5f, 2.55f, 6); return v;
        }();
        static std::vector<float> lodCrown = [](){
            std::vector<float> v; appendOctahedron(v, {0,2.55f,0}, 1.4f); return v;
        }();
        drawTris(lodTrunk.data(), (int)(lodTrunk.size()/3), trunkR,trunkG,trunkB, 1.0f, x,0,z, Strunk*mirrorSign,Strunk,Strunk, treeYaw, tiltX, tiltZ);
        drawTris(lodCrown.data(), (int)(lodCrown.size()/3), gR,gG,gB, 1.0f, x,0,z, ScCanopy*mirrorSign,ScCanopy,ScCanopy, treeYaw, tiltX, tiltZ);
        // fruit SENGAJA di-skip total pd LOW LOD -- tak relevan/tak terlihat
        // dari jarak sejauh ini, murni penghematan tanpa kehilangan info
        // yg pemain butuhkan (indikator TBS matang sudah ada via
        // drawHarvestBeacon() terpisah, yg TAK terpengaruh LOD ini).
    } else {
        drawTris(kPalmIconTrunk, kPalmIconTrunk_COUNT, trunkR,trunkG,trunkB, 1.0f, x,0,z, Strunk*mirrorSign,Strunk,Strunk, treeYaw, tiltX, tiltZ);
        drawTris(kPalmIconCrownLight, kPalmIconCrownLight_COUNT, gR,gG,gB, 1.0f, x,0,z, ScCanopy*mirrorSign,ScCanopy,ScCanopy, treeYaw, tiltX, tiltZ);
        if (lod == LodTier::High){
            // crown_dark (kontras bayangan kanopi) HANYA pd HIGH LOD --
            // MEDIUM LOD skip elemen ini (poin dokumen LOD: "MEDIUM LOD"
            // tak perlu detail penuh) sbg penghematan ~25% triangle tanpa
            // kehilangan siluet dasar kanopi (crownLight tetap digambar).
            drawTris(kPalmIconCrownDark, kPalmIconCrownDark_COUNT, gdR,gdG,gdB, 1.0f, x,0,z, ScCanopy*mirrorSign,ScCanopy,ScCanopy, treeYaw, tiltX, tiltZ);
        }
    }

    // --- buah: sekarang tampil SEJAK status "Growing" (bukan cuma Ripe/Overripe)
    //     dgn progres warna hijau->oranye->merah -- review eksternal poin #4:
    //     "TBS perlu dibuat jauh lebih terlihat... buat visual state yg sangat
    //     jelas: belum matang/mulai matang/matang/lewat matang". Ini jg
    //     memberi pemain sinyal dini "tandan sedang terbentuk", bukan cuma
    //     muncul tiba-tiba pas matang.
    // LOD: fruit HANYA pd HIGH LOD -- MEDIUM/LOW skip (poin dokumen: MEDIUM
    // "kurang detail area jauh", buah individual tak relevan/tak terlihat
    // jelas dari jarak sedang-jauh; indikator kematangan tetap ada via
    // drawHarvestBeacon() terpisah yg TAK terpengaruh LOD ini). ---
    if (ffb>=1 && lod == LodTier::High){
        float fruitR, fruitG, fruitB;
        if (ffb==1){ fruitR=0.263f; fruitG=0.616f; fruitB=0.184f; }      // Growing: hijau, saturasi dinaikkan
        else if (ffb==2){ fruitR=0.980f; fruitG=0.514f; fruitB=0.110f; } // Ripe: oranye lebih cerah/vivid
        else { fruitR=0.678f; fruitG=0.129f; fruitB=0.086f; }            // Overripe: merah lebih tegas
        drawTris(kPalmIconFruit, kPalmIconFruit_COUNT, fruitR,fruitG,fruitB, 1.0f, x,0,z, ScCanopy*mirrorSign,ScCanopy,ScCanopy, treeYaw, tiltX, tiltZ);
    }

    // --- highlight seleksi: cincin tipis kuning transparan di dasar pohon ---
    if (selected){
        static const float ring[] = {
            -1.2f,0.02f,-1.2f, 1.2f,0.02f,-1.2f, 1.2f,0.02f,1.2f,
            -1.2f,0.02f,-1.2f, 1.2f,0.02f,1.2f, -1.2f,0.02f,1.2f,
        };
        drawTris(ring, 6, 0.878f, 0.655f, 0.122f, 0.35f, x, 0, z, 1,1,1);
    }
}

// Kotak sederhana, origin di TENGAH alas (y=0), memanjang ke +Y sejauh `len`
// (boleh negatif utk menjuntai ke bawah dari titik pivot, mis. lengan).
void buildBox(std::vector<float>& out, float w, float d, float len){
    float hw=w*0.5f, hd=d*0.5f;
    V3 p000{-hw,0,-hd}, p100{hw,0,-hd}, p110{hw,0,hd}, p010{-hw,0,hd};
    V3 p001{-hw,len,-hd}, p101{hw,len,-hd}, p111{hw,len,hd}, p011{-hw,len,hd};
    pushQuad(out,p000,p100,p110,p010);
    pushQuad(out,p011,p111,p101,p001);
    pushQuad(out,p000,p001,p101,p100);
    pushQuad(out,p100,p101,p111,p110);
    pushQuad(out,p110,p111,p011,p010);
    pushQuad(out,p010,p011,p001,p000);
}
// Silinder BERDIRI (sumbu Y, dasar di y=0 sampai y=height) -- dibutuhkan
// tangki/silo vertikal PKS, elemen paling IKONIK dari kejauhan menurut riset
// literatur ("oil storage tank", tangki metalik silinder menjulang tinggi,
// jauh lebih mudah dikenali drpd bangunan gudang datar biasa).
void buildCylinder(std::vector<float>& out, float radius, float height, int sides=12){
    std::vector<V3> bottom, top;
    for (int s=0;s<sides;s++){
        float a = (float)s/sides*6.28318f;
        bottom.push_back({std::cos(a)*radius, 0.0f, std::sin(a)*radius});
        top.push_back({std::cos(a)*radius, height, std::sin(a)*radius});
    }
    V3 centerBottom{0,0,0}, centerTop{0,height,0};
    for (int s=0;s<sides;s++){
        int s2 = (s+1)%sides;
        // dinding samping (2 segitiga per segmen)
        pushQuad(out, bottom[s], bottom[s2], top[s2], top[s]);
        // tutup bawah & atas
        pushTri(out, centerBottom, bottom[s2], bottom[s]);
        pushTri(out, centerTop, top[s], top[s2]);
    }
}
void appendAt(std::vector<float>& out, const std::vector<float>& local, V3 off){
    for(size_t i=0;i<local.size();i+=3){
        out.push_back(local[i]+off.x); out.push_back(local[i+1]+off.y); out.push_back(local[i+2]+off.z);
    }
}
void appendRotXAt(std::vector<float>& out, const std::vector<float>& local, float ang, V3 off){
    for(size_t i=0;i<local.size();i+=3){
        V3 p = v3rotX({local[i],local[i+1],local[i+2]}, ang);
        out.push_back(p.x+off.x); out.push_back(p.y+off.y); out.push_back(p.z+off.z);
    }
}
void appendOctahedron(std::vector<float>& out, V3 c, float r){
    V3 top{c.x,c.y+r,c.z}, bot{c.x,c.y-r,c.z};
    V3 px{c.x+r,c.y,c.z}, nx{c.x-r,c.y,c.z}, pz{c.x,c.y,c.z+r}, nz{c.x,c.y,c.z-r};
    pushTri(out,top,px,pz); pushTri(out,top,pz,nx); pushTri(out,top,nx,nz); pushTri(out,top,nz,px);
    pushTri(out,bot,pz,px); pushTri(out,bot,nx,pz); pushTri(out,bot,nz,nx); pushTri(out,bot,px,nz);
}

// Karakter pekerja berartikulasi — SEBELUMNYA cuma silinder+bola polos (pekerja
// disimulasikan penuh di engine tapi nyaris tak berbentuk manusia). Sekarang:
// kaki+sepatu, overalls biru (bib), kemeja merah, kepala+rambut, sepasang
// lengan yg posenya berubah otomatis sesuai `poseCode` dari engine (WorkerPose:
// 0=Idle,1=Kneel,2=Tool,3=Reach,4=Carry) — meniru referensi foto pekerja sawit
// (menjangkau buah, membungkuk pakai alat, jongkok memungut, membawa keranjang).
// Animasi panen mengikuti teknik SUNGGUHAN dari literatur (Gokomodo "Macam-
// macam Alat Panen Kelapa Sawit"; Perbandingan Egrek dan Dodos, Sarana Prima
// Lestari 2025): "Dodos digunakan dengan cara MENDORONG KE ATAS untuk menebas
// tandan, sedangkan egrek digunakan dengan cara MENARIK KE BAWAH." Utk pokok
// <3m (dodos, gagang pendek-sedang) gerakan mendorong lengan ke atas berulang;
// utk pokok tinggi (egrek, galah panjang) gerakan menghentak turun (Panduan
// Panen Sawit Pemula: "tarik gagang egrek dgn gerakan menyentak/hentakan
// kuat"). Animasi berosilasi pakai jam global g_animT, BUKAN wall-clock
// presisi -- cukup utk kesan gerakan berulang, bukan simulasi fisik penuh.
void drawWorker(float x, float z, int poseCode, bool usingEgrek, float facingRad){
    // Skala diperbesar dari ukuran "manusia asli" (tinggi dasar ~1.2 unit) --
    // pada skala asli pekerja nyaris tak kelihatan dibanding pohon (pokok
    // termuda saja sudah 2.6 unit, yg tua bisa 6+ unit, jarak antar pokok 5.2
    // unit). Sebelumnya 2.5x, dinaikkan lagi jadi 4.0x krn masih dilaporkan
    // kurang kelihatan. Ini BUKAN akurasi proporsi manusia:pohon yg presis
    // (itu memang tidak akan pernah keduanya "cukup terlihat" sekaligus di
    // skala dunia nyata, sawit dewasa tingginya belasan meter vs manusia
    // ~1.7m) -- ini kompromi gameplay spy pekerja tetap jelas terlihat & bisa
    // dikenali aktivitasnya dari kejauhan/saat kamera di-zoom out.
    const float WORKER_SCALE = 4.0f;

    float torsoLean=0.0f, armAngle=0.15f, hipDrop=0.0f;
    bool hasTool=false, hasBasket=false;
    switch(poseCode){
        case 1: torsoLean=1.05f; armAngle=0.95f; hipDrop=0.16f; break; // Kneel: jongkok memungut
        case 2: torsoLean=0.80f; armAngle=0.50f; hasTool=true;  break; // Tool: membungkuk pakai alat
        case 3: { // Reach: menjangkau ke atas -- animasi dodos/egrek
            torsoLean=-0.15f;
            if (usingEgrek){
                // egrek: tarik ke BAWAH dgn hentakan -- ayunan lambat naik, lalu hentak cepat turun
                float t = std::fmod(g_animT, 1.1f)/1.1f;
                float snap = (t<0.75f) ? (t/0.75f) : (1.0f - (t-0.75f)/0.25f); // naik pelan, turun cepat (hentakan)
                armAngle = 2.35f + snap*0.55f;
            } else {
                // dodos: dorong ke ATAS -- sentakan cepat naik, turun pelan (bersiap lagi)
                float t = std::fmod(g_animT, 0.85f)/0.85f;
                float snap = (t<0.30f) ? (t/0.30f) : (1.0f - (t-0.30f)/0.70f);
                armAngle = 2.55f + snap*0.35f;
            }
            break;
        }
        case 4: torsoLean=0.08f; armAngle=1.60f; hasBasket=true; break;// Carry: membawa keranjang
        case 5: break; // Idle DIAM TOTAL (khusus avatar pemain, Gameplay Mode, saat tak ada input
                        // joystick) -- BEDA dari default(poseCode=0): case ini SENGAJA dikecualikan
                        // dari animasi jalan (lihat bool walking di bawah), spy avatar tak terlihat
                        // "jalan di tempat" terus-menerus walau sedang diam tanpa input.
        default: break; // Idle: berdiri/jalan biasa (worker NPC -- SELALU dlm perjalanan saat poseCode=0)
    }
    const float hipY = 0.55f - hipDrop;
    std::vector<float> skin, hair, shirt, overall, boot, tool, basket;

    // --- animasi jalan: Idle ("jalan biasa" antar pohon) & Carry (jalan bawa
    //     TBS ke TPH) SAMA-SAMA representasi pekerja sedang BERJALAN dlm
    //     gameplay -- sebelumnya kaki statis total, makanya terlihat spt
    //     "robot kaku" (dilaporkan pengguna) bukan orang berjalan. Kneel/Tool/
    //     Reach TETAP diam (sedang bekerja di tempat, bukan berjalan).
    //     poseCode==5 (Idle Diam avatar pemain) SENGAJA dikecualikan --
    //     lihat catatan case 5 di atas.
    bool walking = (poseCode == 0 || poseCode == 4);
    float legSwing = 0.0f, armSwingL = 0.0f, armSwingR = 0.0f, bodyBounce = 0.0f;
    if (walking){
        float phase = std::fmod(g_animT * 2.6f, 6.28318f); // kecepatan langkah
        float swing = std::sin(phase) * 0.45f;
        legSwing = swing;
        // Bounce tubuh ATAS (torso/kepala/lengan, BUKAN kaki -- kaki sudah
        // dianimasikan sendiri via legSwing/pivot pinggul) -- BUG VISUAL
        // diperbaiki: sebelumnya tubuh atas diam TOTAL saat berjalan,
        // terlihat kaku dibanding referensi video (karakter berjalan dgn
        // tubuh naik-turun jelas tiap langkah). Frekuensi 2x dari swing kaki
        // krn scr biomekanik pinggul naik SETIAP KALI salah satu kaki
        // menumpu (2x per siklus penuh langkah kiri-kanan), BUKAN cuma gaya
        // kartun sembarang -- fase absolut (bukan sin biasa) spy puncak
        // bounce selalu di TENGAH tiap langkah (saat kaki menumpu penuh),
        // bukan di ujung ayunan.
        bodyBounce = std::abs(std::sin(phase)) * 0.035f;
        if (poseCode == 0){ // Idle: lengan mengayun BERLAWANAN arah kaki (gaya jalan alami manusia)
            armSwingL = -swing * 0.6f;
            armSwingR =  swing * 0.6f;
        }
        // Carry: lengan TETAP memegang keranjang, cuma kaki yg mengayun
    }
    // Condong badan sedikit ke arah gerakan saat berjalan (dulu torsoLean
    // TETAP 0 utk pose Idle -- terasa "berdiri tegak kaku" walau sedang
    // melangkah, BUKAN gestur jalan alami spt referensi video).
    if (walking && torsoLean == 0.0f) torsoLean = 0.045f;

    // hipYUpper KHUSUS tubuh bagian atas (torso/kepala/lengan) -- kaki TETAP
    // pakai hipY asli (tanpa bounce) supaya konsisten dgn animasi langkahnya
    // sendiri (legSwing/pivot pinggul), sesuai anatomi: kaki yg menumpu tanah
    // TAK bergerak vertikal, yg naik-turun adalah PINGGUL/tubuh atas relatif
    // kaki penumpu.
    const float hipYUpper = hipY + bodyBounce;

    // --- kaki & sepatu: pivot di PINGGUL (bukan lantai) supaya bisa
    //     mengayun spt langkah sungguhan -- kedua kaki berlawanan fase.
    for (float side : {-1.0f, 1.0f}){
        float legPhase = (side<0.0f) ? legSwing : -legSwing;
        std::vector<float> bootBox; buildBox(bootBox, 0.09f, 0.14f, 0.10f);
        for (size_t i=1;i<bootBox.size();i+=3) bootBox[i] -= hipY; // geser origin ke pinggul
        appendRotXAt(boot, bootBox, legPhase, {side*0.09f, hipY, 0});
        std::vector<float> trouserBox; buildBox(trouserBox, 0.08f, 0.10f, std::max(0.05f, hipY-0.10f));
        for (size_t i=1;i<trouserBox.size();i+=3) trouserBox[i] += 0.10f - hipY; // geser origin ke pinggul jg
        appendRotXAt(overall, trouserBox, legPhase, {side*0.09f, hipY, 0});
    }

    const float skinR=0.867f, skinG=0.678f, skinB=0.478f;
    const float hairR=0.353f, hairG=0.220f, hairB=0.129f;
    const float shirtR=0.827f, shirtG=0.196f, shirtB=0.145f;   // merah, saturasi dinaikkan -- kontras lbh tegas thd hijau kebun
    const float overallR=0.161f, overallG=0.318f, overallB=0.573f; // biru, saturasi dinaikkan
    const float bootR=0.420f, bootG=0.290f, bootB=0.169f;

    // --- torso: overalls bawah + kemeja atas, ikut condong torsoLean di pivot hip ---
    std::vector<float> torsoLowerLocal; buildBox(torsoLowerLocal, 0.30f, 0.16f, 0.20f);
    appendRotXAt(overall, torsoLowerLocal, torsoLean, {0, hipYUpper, 0});

    std::vector<float> torsoUpperLocal; buildBox(torsoUpperLocal, 0.28f, 0.15f, 0.25f);
    for (size_t i=1;i<torsoUpperLocal.size();i+=3) torsoUpperLocal[i]+=0.20f; // nempel di atas torsoLower
    appendRotXAt(shirt, torsoUpperLocal, torsoLean, {0, hipYUpper, 0});

    // --- kepala & rambut ---
    std::vector<float> headLocal; buildBox(headLocal, 0.19f,0.18f,0.20f);
    for (size_t i=1;i<headLocal.size();i+=3) headLocal[i]+=0.45f; // di atas torsoUpper (0.20+0.25)
    appendRotXAt(skin, headLocal, torsoLean, {0, hipYUpper, 0});
    std::vector<float> hairLocal; buildBox(hairLocal, 0.20f,0.19f,0.08f);
    for (size_t i=0;i<hairLocal.size();i+=3){ hairLocal[i+1]+=0.57f; hairLocal[i+2]-=0.02f; }
    appendRotXAt(hair, hairLocal, torsoLean, {0, hipYUpper, 0});

    // --- lengan: rotasi armAngle DULU di bahu (lokal), baru ikut torsoLean badan ---
    V3 handPos[2];
    int hi=0;
    for (float side : {-1.0f, 1.0f}){
        float armSwing = (side<0.0f) ? armSwingL : armSwingR;
        std::vector<float> armLocal; buildBox(armLocal, 0.075f,0.075f,-0.30f); // menjuntai ke bawah dari bahu
        V3 shoulderRelHip{side*0.17f, 0.42f, 0};
        std::vector<float> armBent;
        for (size_t i=0;i<armLocal.size();i+=3){
            V3 p = v3rotX({armLocal[i],armLocal[i+1],armLocal[i+2]}, armAngle+armSwing);
            armBent.push_back(p.x+shoulderRelHip.x); armBent.push_back(p.y+shoulderRelHip.y); armBent.push_back(p.z+shoulderRelHip.z);
        }
        appendRotXAt(shirt, armBent, torsoLean, {0, hipYUpper, 0});
        // posisi tangan (ujung lengan) -- dipakai utk taruh alat/keranjang
        V3 handLocal = v3rotX({0,-0.30f,0}, armAngle+armSwing);
        V3 handRelHip{handLocal.x+shoulderRelHip.x, handLocal.y+shoulderRelHip.y, handLocal.z+shoulderRelHip.z};
        V3 handLean = v3rotX(handRelHip, torsoLean);
        handPos[hi++] = {handLean.x, handLean.y+hipYUpper, handLean.z};
    }

    // --- alat: dodos/parang (Tool, dari tangan ke TANAH) atau dodos/egrek
    //     panen (Reach, dari tangan ke ATAS arah mahkota) ---
    if (hasTool){
        V3 h = handPos[1];
        std::vector<float> pole; buildBox(pole, 0.03f, 0.03f, -(h.y));
        appendAt(tool, pole, h);
    }
    if (poseCode==3){ // Reach: galah dodos/egrek menjulur ke atas dari tangan
        V3 h = handPos[1];
        float poleLen = usingEgrek ? 1.7f : 0.9f; // egrek: galah panjang; dodos: gagang pendek-sedang
        std::vector<float> pole; buildBox(pole, 0.025f, 0.025f, poleLen);
        appendAt(tool, pole, h);
    }
    // --- keranjang TBS, di antara kedua tangan (fase angkut membawa) ---
    if (hasBasket){
        V3 mid{(handPos[0].x+handPos[1].x)*0.5f, (handPos[0].y+handPos[1].y)*0.5f, (handPos[0].z+handPos[1].z)*0.5f + 0.12f};
        std::vector<float> basketBox; buildBox(basketBox, 0.22f,0.20f,0.14f);
        appendAt(basket, basketBox, {mid.x-0.11f, mid.y-0.05f, mid.z-0.10f});
        appendOctahedron(basket, {mid.x, mid.y+0.11f, mid.z}, 0.09f);
        appendOctahedron(basket, {mid.x-0.08f, mid.y+0.08f, mid.z+0.03f}, 0.07f);
        appendOctahedron(basket, {mid.x+0.08f, mid.y+0.08f, mid.z-0.02f}, 0.07f);
    }

    if (!skin.empty())    drawTris(skin.data(),    (int)(skin.size()/3),    skinR,skinG,skinB, 1.0f, x,0,z, WORKER_SCALE,WORKER_SCALE,WORKER_SCALE, facingRad);
    if (!hair.empty())    drawTris(hair.data(),    (int)(hair.size()/3),    hairR,hairG,hairB, 1.0f, x,0,z, WORKER_SCALE,WORKER_SCALE,WORKER_SCALE, facingRad);
    if (!shirt.empty())   drawTris(shirt.data(),   (int)(shirt.size()/3),   shirtR,shirtG,shirtB, 1.0f, x,0,z, WORKER_SCALE,WORKER_SCALE,WORKER_SCALE, facingRad);
    if (!overall.empty()) drawTris(overall.data(), (int)(overall.size()/3), overallR,overallG,overallB, 1.0f, x,0,z, WORKER_SCALE,WORKER_SCALE,WORKER_SCALE, facingRad);
    if (!boot.empty())    drawTris(boot.data(),    (int)(boot.size()/3),    bootR,bootG,bootB, 1.0f, x,0,z, WORKER_SCALE,WORKER_SCALE,WORKER_SCALE, facingRad);
    if (!tool.empty())    drawTris(tool.data(),    (int)(tool.size()/3),    0.45f,0.42f,0.38f, 1.0f, x,0,z, WORKER_SCALE,WORKER_SCALE,WORKER_SCALE, facingRad);
    if (!basket.empty()){
        // kotak keranjang (coklat) & buah (oranye) beda warna -> pisah 2 draw call
        // sederhana: seluruhnya kita gambar sbg satu grup coklat, cukup utk gaya low-poly.
        drawTris(basket.data(), (int)(basket.size()/3), 0.788f,0.541f,0.204f, 1.0f, x,0,z, WORKER_SCALE,WORKER_SCALE,WORKER_SCALE, facingRad);
    }
}

// TBS hasil panen tergeletak di dasar pohon -- lihat literatur di header:
// tumpukan buah kecil (3-4 gumpalan oranye) langsung di tanah dekat batang.
void drawTbsPile(float x, float z){
    std::vector<float> pile;
    unsigned seed = (unsigned)(x*991.0f) ^ (unsigned)(z*757.0f) ^ 0xABCDEFu;
    auto frand=[&](){ seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return (seed&0xFFFFFF)/float(0xFFFFFF); };
    for (int i=0;i<4;i++){
        float a = frand()*6.28318f, rad = 0.18f+frand()*0.15f;
        V3 c{std::cos(a)*rad, 0.10f+frand()*0.04f, std::sin(a)*rad};
        appendOctahedron(pile, c, 0.11f+frand()*0.03f);
    }
    drawTris(pile.data(), (int)(pile.size()/3), 0.827f,0.435f,0.157f, 1.0f, x,0,z, 1,1,1);
}

// Tumpukan TBS di TPH -- SATU tumpukan (3-4 gumpalan) per tandan di stok,
// disebar melingkar spy tak numpuk persis di titik yg sama. Dibatasi 12
// tumpukan spy draw call tetap wajar kalau stok sangat besar.
// Bangunan PKS -- mengatasi keluhan review eksternal: "tidak ada tampilan
// sama sekali bangunan PKS di scene 3D" (padahal logic-nya lengkap &
// berfungsi sejak lama). Desain berdasar riset literatur industri sawit:
// gudang proses utama (rangka baja industrial) + tangki/silo vertikal
// METALIK (elemen paling IKONIK dari kejauhan -- "oil storage tank",
// "sterilization tank", jauh lebih mudah dikenali sbg "pabrik" drpd
// bangunan datar biasa). Jumlah silo bertambah seiring LEVEL PKS naik --
// progresi visual yg mencerminkan kapasitas proses yg BENAR-BENAR bertambah
// per level (pksCapacityPerBatch, lihat engine.hpp), bukan cuma angka.
// pulse: 0..1, dipakai efek visual SAAT proses batch BARU SAJA terjadi
// (lihat catatan lengkap di JNI/EngineBridge draw loop).
void drawPksBuilding(float x, float z, int level, float pulse){
    // Diganti model GLB custom (Blender, 'PKS.glb') sesuai permintaan
    // pengguna: "Buatkan bangunan pks mengikuti design 3d yang sudah
    // dibangun sebelumnya" -- MENGGANTIKAN geometri prosedural (kotak+atap
    // pelana+silinder) sebelumnya. Lihat catatan lengkap ekstraksi (object
    // tak relevan yg diabaikan, dll) di pks_building_mesh.hpp.
    //
    // "level" SEBELUMNYA menambah JUMLAH silo (1-4) scr prosedural --
    // TIDAK bisa direplikasi persis dgn mesh BAKED tunggal ini (geometri
    // tetap, bukan modular). Diganti: SCALE KESELURUHAN membesar sedikit
    // seiring level (max +15% di level 4) -- kesan visual "upgrade" tetap
    // ada tanpa perlu directX bangunan scr struktural.
    float levelScale = 1.0f + (float)(std::min(4, std::max(1, level)) - 1) * 0.05f;

    // Pulsa warna saat proses batch baru terjadi -- silo sedikit lebih
    // TERANG (SAMA spt versi prosedural sebelumnya, dipertahankan krn
    // kategori 'silo' dari mesh baru scr semantik SAMA -- tangki metalik).
    float siloBright = 1.0f + pulse * 0.5f;

    drawTris(kPksMesh_factory, kPksMesh_factory_COUNT,
             kPksColor_factory[0], kPksColor_factory[1], kPksColor_factory[2],
             1.0f, x, 0, z, levelScale, levelScale, levelScale);
    drawTris(kPksMesh_silo, kPksMesh_silo_COUNT,
             std::min(1.0f, kPksColor_silo[0]*siloBright), std::min(1.0f, kPksColor_silo[1]*siloBright), std::min(1.0f, kPksColor_silo[2]*siloBright),
             1.0f, x, 0, z, levelScale, levelScale, levelScale);
}

void drawTphPile(float tphX, float tphZ, int stockCount){
    int n = stockCount; if (n>12) n=12; if (n<0) n=0;
    for (int p=0; p<n; p++){
        float a = (float)p/std::max(1,n) * 6.28318f;
        float rad = 0.9f + (p%3)*0.6f;
        float px = tphX + std::cos(a)*rad, pz = tphZ + std::sin(a)*rad;
        std::vector<float> pile;
        unsigned seed = (unsigned)(p*137u) ^ 0x51F00Du;
        auto frand=[&](){ seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return (seed&0xFFFFFF)/float(0xFFFFFF); };
        for (int i=0;i<5;i++){
            float aa = frand()*6.28318f, rr = 0.12f+frand()*0.16f;
            V3 c{std::cos(aa)*rr, 0.12f+frand()*0.10f, std::sin(aa)*rr};
            appendOctahedron(pile, c, 0.12f+frand()*0.04f);
        }
        drawTris(pile.data(), (int)(pile.size()/3), 0.827f,0.435f,0.157f, 1.0f, px,0,pz, 1,1,1);
    }
}

// Tumpukan pelepah hasil tunas di GAWANGAN MATI (jalur antar-baris yg
// SENGAJA ditutup/tak dilalui) -- mengatasi keluhan review: "pelepah hasil
// tunas ditumpuk di gawangan mati (jalur selang-seling) tidak ada tanda
// tumpukan scr visual sama sekali". Literatur: SOP Palm Oil Plantation
// ("Lokasi pancang rumpukan nantinya dijadikan dasar gawangan mati pada
// saat pancang tanam") & studi erosi gawangan hidup vs mati (jurnal
// tropicalsoil UNILA/Brawijaya -- "pelepah sawit yang dipangkas disusun di
// jalur gawangan mati", berfungsi jg sbg pengendali erosi & pengembalian
// bahan organik tanah, BUKAN cuma dibuang sembarangan).
//
// Geometri: beberapa "batang" pelepah (kotak panjang tipis, BUKAN
// octahedron bulat spt TBS/TPH pile) ditumpuk SILANG-MENYILANG sepanjang
// gawangan -- kesan organik alami, bukan rapi sejajar kaku. Membentang di
// sumbu X (arah baris pohon dlm grid kita) sepanjang gawanganLength.
void drawFrondPile(float gawanganCenterX, float gawanganCenterZ, float gawanganLength){
    std::vector<float> pile;
    unsigned seed = (unsigned)(gawanganCenterZ*1000.0f) ^ 0xF20D1Eu;
    auto frand=[&](){ seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return (seed&0xFFFFFF)/float(0xFFFFFF); };
    int nBatang = std::max(3, (int)(gawanganLength / 2.2f)); // kepadatan proporsional panjang gawangan
    for (int i=0;i<nBatang;i++){
        float segLen = 1.6f + frand()*0.8f;
        float px = gawanganCenterX - gawanganLength*0.5f + (gawanganLength/nBatang)*(i+0.5f) + (frand()-0.5f)*0.6f;
        float pz = gawanganCenterZ + (frand()-0.5f)*0.5f;
        float py = 0.04f + (i%3)*0.05f; // tumpukan berlapis (variasi tinggi kecil)
        float jitterYaw = (frand()-0.5f)*0.5f; // sedikit miring, bukan sejajar kaku sumbu X murni
        std::vector<float> batang; buildBox(batang, 0.10f, 0.14f, segLen);
        for (size_t k=1;k<batang.size();k+=3) batang[k] -= 0.07f; // pusatkan tinggi ke origin lokal
        for (size_t k=0;k<batang.size();k+=3){
            V3 p = v3rotY({batang[k],batang[k+1],batang[k+2]}, jitterYaw);
            pile.push_back(p.x+px); pile.push_back(p.y+py); pile.push_back(p.z+pz);
        }
    }
    // Warna coklat-hijau (pelepah baru dipangkas, belum sepenuhnya kering)
    // -- beda dgn coklat gelap TPH pile (TBS matang) yg lebih oranye-coklat.
    drawTris(pile.data(), (int)(pile.size()/3), 0.451f,0.408f,0.196f, 1.0f, 0,0,0, 1,1,1);
}

// Visual "jalan" (road) yg JELAS di gawangan HIDUP -- mengatasi celah poin
// dokumen review #7 "Terrain dan Struktur Barisan Kebun": "Campurkan soil,
// grass, mud, road, leaf litter, dan rock melalui shader" & "Jalan panen dan
// jalan inspeksi harus memiliki struktur yang jelas". SEBELUMNYA gawangan
// hidup (jalur BERSIH yg dilalui pekerja/truk utk panen&inspeksi) TAK punya
// penanda visual sama sekali -- scr LOGIKA/gameplay memang "bersih" (tak ada
// mulsa pelepah spt gawangan mati), tapi scr VISUAL warnanya identik dgn
// tanah biasa di sekitarnya, tak ada kesan "jalan" sungguhan. Warna abu-abu
// kecoklatan (tanah dipadatkan oleh lalu-lalang berulang, BUKAN coklat murni
// spt tanah gembur di sekitarnya) + sedikit variasi tekstur (bercak kecil,
// meniru jejak roda/kaki tak seragam sempurna).
void drawRoadStrip(float gawanganCenterX, float gawanganCenterZ, float gawanganLength){
    const float ROAD_WIDTH = 2.2f; // cukup utk 1 pekerja+troli lewat, tak terlalu lebar dominasi visual
    float half = gawanganLength * 0.5f;
    float quad[] = {
        gawanganCenterX-half,0.001f,gawanganCenterZ-ROAD_WIDTH*0.5f,  gawanganCenterX+half,0.001f,gawanganCenterZ-ROAD_WIDTH*0.5f,  gawanganCenterX+half,0.001f,gawanganCenterZ+ROAD_WIDTH*0.5f,
        gawanganCenterX-half,0.001f,gawanganCenterZ-ROAD_WIDTH*0.5f,  gawanganCenterX+half,0.001f,gawanganCenterZ+ROAD_WIDTH*0.5f,  gawanganCenterX-half,0.001f,gawanganCenterZ+ROAD_WIDTH*0.5f,
    };
    // Abu-abu kecoklatan -- tanah dipadatkan, KONTRAS jelas dgn dasar tanah
    // gembur (#a17b4e di drawGround()) tanpa perlu shader multi-material
    // (keterbatasan renderer ini: warna solid per drawTris(), tak ada
    // texture blending sungguhan) -- pendekatan flat-color BERBEDA cukup
    // memberi struktur visual "jalan" yg diminta dokumen.
    drawTris(quad, 6, 0.518f, 0.475f, 0.400f, 1.0f, 0,0,0, 1,1,1);

    // Variasi tekstur kecil (jejak roda/kaki tak seragam) -- deterministik
    // dari posisi Z gawangan, konsisten antar frame.
    unsigned seed = (unsigned)(gawanganCenterZ*1000.0f) ^ 0xA0AD1Eu;
    auto frand=[&](){ seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return (seed&0xFFFFFF)/float(0xFFFFFF); };
    std::vector<float> darkTracks;
    int nTracks = std::max(4, (int)(gawanganLength / 3.0f));
    for (int i=0;i<nTracks;i++){
        float px = gawanganCenterX - half + (gawanganLength/nTracks)*(i+0.5f) + (frand()-0.5f)*1.5f;
        float pz = gawanganCenterZ + (frand()-0.5f)*(ROAD_WIDTH*0.7f);
        float r = 0.4f + frand()*0.5f;
        int sides = 6;
        for (int s=0;s<sides;s++){
            float a0=(float)s/sides*6.28318f, a1=(float)(s+1)/sides*6.28318f;
            V3 c{px,0.0015f,pz}, p0{px+std::cos(a0)*r,0.0015f,pz+std::sin(a0)*r}, p1{px+std::cos(a1)*r,0.0015f,pz+std::sin(a1)*r};
            pushTri(darkTracks, c, p0, p1);
        }
    }
    if (!darkTracks.empty()) drawTris(darkTracks.data(), (int)(darkTracks.size()/3), 0.416f,0.376f,0.310f, 0.5f, 0,0,0, 1,1,1);
}

// Traktor+trailer sederhana, meniru siluet referensi (traktor hijau tua,
// trailer hijau muda berisi TBS oranye) — dibangun dari kotak & roda pipih,
// konsisten dgn gaya low-poly renderer ini (bukan model mesh detail).
void drawTruck(float x, float z, float facingRad){
    // Skala truk — SEBELUMNYA TIDAK ADA sama sekali (digambar di ukuran lokal
    // apa adanya, cuma ~0.82 unit tinggi -- jauh lebih kecil dari pekerja yg
    // sudah diskalakan ke 4.8 unit (WORKER_SCALE=4.0), sehingga truk nyaris
    // tak kelihatan/proporsinya aneh). 7.5x membuat traktor+trailer sepanjang
    // ~8.6 unit & tinggi kabin ~6.2 unit -- lebih besar dari pekerja (wajar,
    // ini kendaraan) tapi masih di bawah tinggi pokok dewasa+mahkota (~9 unit).
    const float TRUCK_SCALE = 7.5f;
    std::vector<float> body, cab, wheels, trailer, fruit, frame, exhaust;

    auto rotPlace = [&](std::vector<float>& out, const std::vector<float>& local, V3 off){
        for (size_t i=0;i<local.size();i+=3){
            V3 p = v3rotY({local[i],local[i+1],local[i+2]}, facingRad);
            out.push_back(p.x+off.x); out.push_back(p.y+off.y); out.push_back(p.z+off.z);
        }
    };
    // Roda SILINDER 3D (BUKAN disc datar 8-sisi spt sebelumnya -- dari sudut
    // manapun selain tepat samping, disc datar terlihat sbg garis tipis
    // tak nyata, salah satu penyebab review "material perlu diperbaiki").
    // Sumbu silinder LANGSUNG di X (lebar roda ke samping kendaraan, spt
    // roda sungguhan), bukan bangun sumbu-Y lalu rotasi terpisah.
    auto wheelCyl = [&](std::vector<float>& out, V3 centerLocal, float radius, float thickness){
        const int SIDES=10;
        std::vector<V3> ringNear, ringFar;
        for (int s=0;s<SIDES;s++){
            float a = (float)s/SIDES*6.28318f;
            float cy = std::cos(a)*radius, cz = std::sin(a)*radius;
            ringNear.push_back({centerLocal.x - thickness*0.5f, centerLocal.y+cy, centerLocal.z+cz});
            ringFar.push_back({centerLocal.x + thickness*0.5f, centerLocal.y+cy, centerLocal.z+cz});
        }
        V3 hubNear{centerLocal.x-thickness*0.5f, centerLocal.y, centerLocal.z};
        V3 hubFar{centerLocal.x+thickness*0.5f, centerLocal.y, centerLocal.z};
        std::vector<float> local;
        for (int s=0;s<SIDES;s++){
            int s2=(s+1)%SIDES;
            pushQuad(local, ringNear[s], ringNear[s2], ringFar[s2], ringFar[s]); // ban (tapak keliling)
            pushTri(local, hubNear, ringNear[s2], ringNear[s]);   // sisi dekat
            pushTri(local, hubFar, ringFar[s], ringFar[s2]);      // sisi jauh
        }
        rotPlace(out, local, {0,0,0});
    };

    // --- traktor: kap mesin depan + kabin TERBUKA (rangka/roll-bar, BUKAN
    // kotak tertutup penuh) -- sesuai referensi foto traktor sawit
    // sungguhan (kabin terbuka beratap, tiang di 4 sudut, tanpa dinding). ---
    std::vector<float> bodyBox; buildBox(bodyBox, 0.55f, 0.34f, 0.42f);
    { std::vector<float> tmp; appendAt(tmp, bodyBox, {0,0.20f,-0.10f}); rotPlace(body, tmp, {0,0,0}); }

    // Rangka kabin: 4 tiang tipis (roll-bar) + atap datar tipis di puncak --
    // BUKAN kabBox kotak solid sblmnya (terlihat spt kotak kaca tertutup,
    // bukan kabin traktor terbuka khas kebun).
    auto pillar = [&](float px, float pz){
        std::vector<float> col; buildBox(col, 0.03f, 0.34f, 0.03f);
        std::vector<float> tmp; appendAt(tmp, col, {px, 0.42f, pz});
        rotPlace(frame, tmp, {0,0,0});
    };
    pillar(-0.16f, -0.12f); pillar(0.16f, -0.12f); pillar(-0.16f, 0.20f); pillar(0.16f, 0.20f);
    std::vector<float> roofBox; buildBox(roofBox, 0.40f, 0.03f, 0.36f);
    { std::vector<float> tmp; appendAt(tmp, roofBox, {0,0.76f,0.04f}); rotPlace(cab, tmp, {0,0,0}); }
    // Kap mesin depan (moncong traktor, blm ada sblmnya -- sblmnya cuma
    // body+kabin tanpa "hidung" depan spt traktor sungguhan)
    std::vector<float> hoodBox; buildBox(hoodBox, 0.34f, 0.20f, 0.24f);
    { std::vector<float> tmp; appendAt(tmp, hoodBox, {0,0.22f,-0.42f}); rotPlace(body, tmp, {0,0,0}); }
    // Knalpot vertikal di depan kabin -- ciri khas traktor pertanian, blm
    // ada sblmnya sama sekali.
    { std::vector<float> tmp; buildCylinder(tmp, 0.025f, 0.30f, 8);
      std::vector<float> tmp2; appendAt(tmp2, tmp, {0.18f, 0.42f, -0.28f}); rotPlace(exhaust, tmp2, {0,0,0}); }

    // roda traktor (belakang besar, depan lebih kecil) -- SEKARANG silinder 3D
    wheelCyl(wheels, {-0.30f,0.20f,-0.20f}, 0.20f, 0.09f); wheelCyl(wheels, {0.30f,0.20f,-0.20f}, 0.20f, 0.09f);
    wheelCyl(wheels, {-0.30f,0.14f, 0.16f}, 0.14f, 0.08f); wheelCyl(wheels, {0.30f,0.14f, 0.16f}, 0.14f, 0.08f);

    // --- Hitch/drawbar: batang penghubung traktor-trailer -- SEBELUMNYA
    // TIDAK ADA sama sekali, traktor & trailer terlihat 2 objek terpisah
    // yg kebetulan berdekatan, bukan satu kesatuan kendaraan tersambung. ---
    std::vector<float> hitchBox; buildBox(hitchBox, 0.06f, 0.05f, 0.14f);
    { std::vector<float> tmp; appendAt(tmp, hitchBox, {0,0.16f,0.32f}); rotPlace(frame, tmp, {0,0,0}); }

    // --- trailer: PAGAR/RANGKA terbuka (batang vertikal keliling), BUKAN
    // kotak tertutup polos spt sebelumnya -- sesuai referensi foto trailer
    // TBS sungguhan (bak terbuka berpagar, supaya TBS terlihat & mudah
    // dibongkar-muat, bukan kontainer tertutup). ---
    std::vector<float> floorBox; buildBox(floorBox, 0.62f, 0.06f, 0.30f);
    { std::vector<float> tmp; appendAt(tmp, floorBox, {0,0.16f,0.55f}); rotPlace(trailer, tmp, {0,0,0}); }
    auto railPost = [&](float px, float pz){
        std::vector<float> col; buildBox(col, 0.025f, 0.20f, 0.025f);
        std::vector<float> tmp; appendAt(tmp, col, {px, 0.29f, pz});
        rotPlace(frame, tmp, {0,0,0});
    };
    for (float pz : {0.42f, 0.55f, 0.68f}){ railPost(-0.30f, pz); railPost(0.30f, pz); }
    std::vector<float> railTopBox; buildBox(railTopBox, 0.62f, 0.02f, 0.02f);
    { std::vector<float> tmp; appendAt(tmp, railTopBox, {0,0.39f,0.42f}); rotPlace(frame, tmp, {0,0,0}); }
    { std::vector<float> tmp; appendAt(tmp, railTopBox, {0,0.39f,0.68f}); rotPlace(frame, tmp, {0,0,0}); }
    wheelCyl(wheels, {-0.34f,0.16f,0.60f}, 0.16f, 0.08f); wheelCyl(wheels, {0.34f,0.16f,0.60f}, 0.16f, 0.08f);

    unsigned seed=0xF00Du;
    auto frand=[&](){ seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return (seed&0xFFFFFF)/float(0xFFFFFF); };
    for (int i=0;i<7;i++){
        float lx=(frand()-0.5f)*0.5f, lz=0.42f+frand()*0.28f, ly=0.42f+frand()*0.12f;
        std::vector<float> tmp; appendOctahedron(tmp, {lx,ly,lz}, 0.10f+frand()*0.03f);
        std::vector<float> rot; rotPlace(rot, tmp, {0,0,0});
        fruit.insert(fruit.end(), rot.begin(), rot.end());
    }

    drawTris(body.data(), (int)(body.size()/3), 0.106f,0.302f,0.169f, 1.0f, x,0,z, TRUCK_SCALE,TRUCK_SCALE,TRUCK_SCALE);   // traktor hijau tua
    drawTris(cab.data(), (int)(cab.size()/3), 0.157f,0.361f,0.216f, 1.0f, x,0,z, TRUCK_SCALE,TRUCK_SCALE,TRUCK_SCALE);
    drawTris(frame.data(), (int)(frame.size()/3), 0.086f,0.086f,0.086f, 1.0f, x,0,z, TRUCK_SCALE,TRUCK_SCALE,TRUCK_SCALE); // rangka/roll-bar/pagar abu gelap
    drawTris(exhaust.data(), (int)(exhaust.size()/3), 0.15f,0.15f,0.15f, 1.0f, x,0,z, TRUCK_SCALE,TRUCK_SCALE,TRUCK_SCALE); // knalpot
    drawTris(wheels.data(), (int)(wheels.size()/3), 0.098f,0.098f,0.098f, 1.0f, x,0,z, TRUCK_SCALE,TRUCK_SCALE,TRUCK_SCALE); // roda hitam
    drawTris(trailer.data(), (int)(trailer.size()/3), 0.290f,0.494f,0.361f, 1.0f, x,0,z, TRUCK_SCALE,TRUCK_SCALE,TRUCK_SCALE); // trailer hijau muda
    drawTris(fruit.data(), (int)(fruit.size()/3), 0.827f,0.435f,0.157f, 1.0f, x,0,z, TRUCK_SCALE,TRUCK_SCALE,TRUCK_SCALE);    // TBS oranye
}

// Tanda kecil melayang di atas pohon yg sudah dikerjakan aksi massal HARI
// INI — segitiga datar warna beda per jenis aksi, gampang dibedakan sekilas.
// Penanda TBS matang yang MENCOLOK & PERSISTEN (beda dari drawActionMarker yg
// kecil & transien "baru dikerjakan hari ini") -- mengatasi keluhan review
// eksternal: "TBS sulit dibaca dari kejauhan krn jumlah pelepah sangat
// banyak, sulit membaca 'oh pohon ini punya TBS matang'". Ditempatkan JAUH
// di atas kanopi (bukan sekadar treeTopY+0.6 spt actionMarker) supaya
// menembus tajuk & terlihat bahkan saat zoom-out/Estate View. Berdenyut
// halus (pulse via g_animT) spy makin menarik perhatian mata. Warna beda
// eksplisit matang(oranye) vs lewat-matang(merah, lebih mendesak).
// Setter/getter publik toggle beacon -- dipanggil dari JNI (Android) &
// EngineBridge (iOS), keduanya membaca/tulis SATU variabel shared di atas.
void setShowHarvestBeacon(bool show){ g_showHarvestBeacon = show; }
bool getShowHarvestBeacon(){ return g_showHarvestBeacon; }
void setEstateViewMode(bool active, int layer){ g_estateViewActive = active; g_estateViewLayer = layer; }
bool getEstateViewActive(){ return g_estateViewActive; }
int getEstateViewLayer(){ return g_estateViewLayer; }

// Toggle Gameplay Mode (third-person, avatar bisa digerakkan) vs
// Management Mode (ortografis, default & TETAP dipertahankan utuh) --
// hasil review eksternal poin #5.
void setGameplayModeActive(bool active){ g_gameplayModeActive = active; }
bool getGameplayModeActive(){ return g_gameplayModeActive; }
// "Zoom terbatas" sesuai review -- delta positif = menjauh (zoom out),
// negatif = mendekat (zoom in). Clamp ke [kAvatarCamDistMin,kAvatarCamDistMax]
// -- avatar tak bisa di-zoom sampai kamera menembus badannya sendiri (min)
// atau terlalu jauh sampai terasa spt Management Mode lagi (max).
void adjustAvatarCamZoom(float delta){
    g_avatarCamDistBehind = std::max(kAvatarCamDistMin, std::min(kAvatarCamDistMax, g_avatarCamDistBehind+delta));
}
// Baca jarak zoom SAAT INI -- dipakai JNI/EngineBridge sbg parameter
// engine.cameraSafeDistance() SEBELUM updatePlayerCamera() dipanggil, supaya
// collision kamera thd pohon (baru ditambahkan, sblmnya cuma ground
// clearance) tak permanen menimpa setting zoom pemain -- cuma dipakai
// SEMENTARA di frame itu (kamera otomatis kembali ke jarak zoom asli begitu
// avatar menjauh dari pohon yg menghalangi).
float getAvatarCamDistBehind(){ return g_avatarCamDistBehind; }

// Kamera "lihat sekeliling" via touch-drag (poin #4 laporan pengguna:
// "sudut pandang orang ketika harusnya bisa berubah ketika layar disentuh
// bukannya hanya dari joystick dalam mode berjalan") -- SEBELUMNYA kamera
// third-person SELALU terkunci lurus di belakang avatar berdasar facingRad
// (arah gerak terakhir), tak ada cara memutar pandang independen tanpa
// bergerak. g_cameraYawOffset ditambahkan ke facingRad avatar HANYA utk
// menentukan arah kamera & referensi gerak camera-relative (lihat
// Engine::movePlayerAvatar) -- BUKAN mengubah facingRad avatar itu sendiri
// scr permanen.
float g_cameraYawOffset = 0.0f;
// Putar pandang -- dipanggil dari touch-drag gesture (delta pergeseran jari
// horizontal, dikonversi radian oleh platform layer sebelum panggil ini).
void adjustCameraYawOffset(float deltaRad){
    // Sensitivitas kamera diterapkan DI SINI (bukan di platform layer
    // Kotlin/Swift) -- satu titik perubahan, otomatis berlaku Android & iOS
    // sekaligus. Fitur baru diminta pengguna ("tambahkan pengaturan
    // sensivitas dan grafik").
    g_cameraYawOffset += deltaRad * g_cameraSensitivity;
}
// Baca offset saat ini -- dipakai JNI/EngineBridge sbg parameter
// engine.movePlayerAvatar() SEBELUM avatar bergerak.
float getCameraYawOffset(){ return g_cameraYawOffset; }
// Mendongak "lihat ke atas" -- fitur baru diminta pengguna: "tidak bisa
// melihat lebih ke atas pohon sawit, berikan lebih jauh sudut pandang
// hanya untuk melihat ke atas tidak untuk horizontal". Dipanggil dari
// touch-drag VERTIKAL (komponen distanceY) di Gameplay Mode -- TERPISAH
// SEPENUHNYA dari adjustCameraYawOffset() (komponen distanceX/horizontal)
// di atas, sesuai permintaan eksplisit "tidak untuk horizontal" (tak
// mengubah rotasi kamera horizontal sama sekali). Clamp [0,
// kMaxLookUpOffset] -- SENGAJA cuma non-negatif (delta negatif tak bisa
// menurunkan di bawah 0) krn permintaan HANYA utk melihat ke atas, bukan
// menambah kemampuan menunduk lebih dari baseline.
void adjustAvatarLookUpOffset(float deltaY){
    g_avatarLookUpOffset = std::max(0.0f, std::min(kMaxLookUpOffset, g_avatarLookUpOffset + deltaY * g_cameraSensitivity));
}
float getAvatarLookUpOffset(){ return g_avatarLookUpOffset; }
// Getter verifikasi/debugging -- posisi mata & target look-at kamera third-person saat ini.
float getAvatarCamEyeY(){ return g_avatarCamEyeY; }
float getAvatarLookAtY(){ return g_avatarLookAtY; }
// PERINGATAN -- JANGAN PANGGIL fungsi ini dari mana pun (SENGAJA dibiarkan
// TIDAK DIPAKAI/dead code, diverifikasi tak ada call site sama sekali di
// seluruh codebase saat ini). Komentar LAMA di sini (sebelum redesain besar
// kamera third-person, poin #1 laporan pengguna) menyebut fungsi ini
// "dipanggil JNI/EngineBridge saat avatar bergerak" -- itu SUDAH TAK
// BERLAKU LAGI (bagian dari mekanisme "offset diserap ke facingRad" yg
// SUDAH DIHAPUS krn justru itulah akar bug "kamera ikut berputar
// otomatis"). Fitur baru diminta pengguna secara EKSPLISIT: "Kamera
// sebaiknya tidak selalu tepat di belakang worker... Jangan otomatis: User
// melepas layar -> Camera langsung kembali -> tepat belakang worker. Itu
// biasanya terasa mengganggu." -- memanggil fungsi ini di mana pun (mis.
// saat touch-up/ACTION_UP, atau saat avatar mulai bergerak) akan MERUSAK
// persis perilaku yg diminta ini. Dipertahankan HANYA sbg API historis
// (kalau suatu saat benar2 dibutuhkan reset manual eksplisit oleh pemain,
// mis. tombol "kembalikan kamera" -- TAPI BUKAN otomatis).
void resetCameraYawOffset(){ g_cameraYawOffset = 0.0f; }

// Depth key utk sorting painter's-algorithm yang BENAR pada kamera 360
// derajat -- BUG diperbaiki: loop pohon sebelumnya digambar dlm urutan
// ARRAY (ID/urutan tanam), BUKAN urutan kedalaman thd arah pandang kamera
// saat ini. Terverifikasi numerik: pada separuh rentang yaw (180-315
// derajat, dari fitur "lihat 360 derajat" yg sudah ada), pohon yg
// SEHARUSNYA di depan (lebih dekat kamera) malah tertutup pohon di
// belakangnya krn digambar duluan sesuai urutan array, BUKAN urutan depth
// yg benar. Proyeksi (x,z) ke "sumbu pandang" kamera berdasar g_yaw --
// NILAI LEBIH KECIL = LEBIH JAUH dari kamera (digambar DULU), NILAI LEBIH
// BESAR = LEBIH DEKAT (digambar BELAKANGAN, menimpa yg jauh, sesuai
// painter's algorithm yg sudah dipakai ground-vs-pohon).
float depthKeyForYaw(float x, float z){
    return x*std::sin(g_yaw) + z*std::cos(g_yaw);
}

// Warna ubin Estate View berdasar fraksi kondisi block -- fungsi MURNI
// (tanpa side-effect), gampang diuji langsung. Gradien hijau (baik) ->
// kuning -> oranye -> merah (perlu perhatian), konsisten dgn pola warna
// kesehatan yg SUDAH ADA di kanopi pohon individu (drawPalm) & statusEmoji
// (BlockSummaryView, Kotlin/Swift) -- BUKAN skema warna baru yg tak
// konsisten dgn elemen visual lain yg sudah mapan.
void estateLayerColor(int layer, float badFraction, float* outR, float* outG, float* outB){
    // badFraction: 0=kondisi sempurna (hijau), 1=kondisi terburuk (merah) --
    // makna "buruk" BERBEDA per layer (lihat pemanggil JNI/EngineBridge utk
    // detail perhitungan per-layer: Kesehatan/Nutrisi/Kematangan).
    float r,g,b;
    if (badFraction < 0.33f){
        float t = badFraction/0.33f;
        r = 0.145f + t*(0.949f-0.145f); g = 0.616f + t*(0.792f-0.616f); b = 0.204f + t*(0.184f-0.204f); // hijau->kuning
    } else if (badFraction < 0.66f){
        float t = (badFraction-0.33f)/0.33f;
        r = 0.949f + t*(0.980f-0.949f); g = 0.792f + t*(0.514f-0.792f); b = 0.184f + t*(0.110f-0.184f); // kuning->oranye
    } else {
        float t = std::min(1.0f,(badFraction-0.66f)/0.34f);
        r = 0.980f + t*(0.886f-0.980f); g = 0.514f + t*(0.176f-0.514f); b = 0.110f + t*(0.129f-0.110f); // oranye->merah
    }
    (void)layer; // gradien SAMA utk semua layer, cuma makna badFraction beda per-pemanggil
    *outR=r; *outG=g; *outB=b;
}

// Ubin datar 1 block -- lebar & dalam MENGIKUTI ground quad asli (drawGround)
// spy posisi & ukuran KONSISTEN scr visual saat transisi masuk/keluar Estate
// View, tapi geometri SANGAT SEDERHANA (1 quad, 2 segitiga) -- bukan ratusan
// pohon detail.
void drawEstateBlockTile(float originX, float originZ, float r, float g, float b){
    std::vector<float> quad;
    float hw=44.0f, hd=37.0f; // sama persis dgn half_w/half_h drawGround()
    V3 p0{-hw,0.05f,-hd}, p1{hw,0.05f,-hd}, p2{hw,0.05f,hd}, p3{-hw,0.05f,hd};
    pushQuad(quad, p0,p1,p2,p3);
    drawTris(quad.data(), (int)(quad.size()/3), r,g,b, 0.92f, originX,0,originZ, 1,1,1);
}

void drawHarvestBeacon(float x, float z, float treeTopY, bool overripe){
    float r,g,b;
    if (overripe) { r=0.886f; g=0.176f; b=0.129f; } // merah -- lebih mendesak (FFA naik, Corley §11.5.5.1)
    else { r=0.980f; g=0.573f; b=0.129f; }          // oranye -- siap panen normal

    float pulse = 0.85f + 0.15f * std::sin(g_animT * 3.0f); // denyut halus 0.85..1.0
    float beaconY = treeTopY * 1.8f + 1.2f; // jauh di atas kanopi, bukan cuma treeTopY+0.6 spt actionMarker
    float size = 0.34f * pulse;

    std::vector<float> tri;
    // Bentuk berlian (diamond) memanjang vertikal -- lebih mencolok & mudah
    // dikenali dari kejauhan drpd segitiga kecil actionMarker.
    V3 top{0, beaconY+size*1.4f, 0}, bot{0, beaconY-size*0.5f, 0};
    V3 left{-size*0.55f, beaconY, 0}, right{size*0.55f, beaconY, 0};
    V3 leftD{0, beaconY, -size*0.55f}, rightD{0, beaconY, size*0.55f}; // sisi kedua (silang) spy tampak dari sudut manapun
    pushTri(tri, bot, right, top); pushTri(tri, bot, top, right);
    pushTri(tri, bot, top, left); pushTri(tri, bot, left, top);
    pushTri(tri, bot, rightD, top); pushTri(tri, bot, top, rightD);
    pushTri(tri, bot, top, leftD); pushTri(tri, bot, leftD, top);
    drawTris(tri.data(), (int)(tri.size()/3), r,g,b, 0.92f, x,0,z, 1,1,1);
}

void drawActionMarker(float x, float z, float treeTopY, int kind){
    float r,g,b;
    switch(kind){
        case 0: r=0.827f; g=0.435f; b=0.157f; break; // panen: oranye (TBS)
        case 1: r=0.290f; g=0.494f; b=0.804f; break; // angkut: biru (TPH)
        case 2: r=0.541f; g=0.290f; b=0.804f; break; // pupuk: ungu
        case 3: r=0.929f; g=0.831f; b=0.220f; break; // pestisida: kuning
        case 4: r=0.196f; g=0.741f; b=0.322f; break; // fungisida: hijau terang (sembuh)
        default: r=0.9f; g=0.9f; b=0.9f; break;
    }
    float y = treeTopY + 0.6f;
    std::vector<float> tri;
    V3 top{0,y+0.22f,0}, bl{-0.19f,y,0}, br{0.19f,y,0};
    pushTri(tri, bl, br, top);
    pushTri(tri, br, bl, top); // dobel sisi (renderer tak cull backface, tp jaga2 supaya selalu tampak dari kedua arah kamera)
    drawTris(tri.data(), (int)(tri.size()/3), r,g,b, 1.0f, x,0,z, 1,1,1);
}

float treeTrunkHeight(float ageYears){
    const float TOOL_AGE_THRESHOLD = 6.0f;
    bool mature = ageYears >= TOOL_AGE_THRESHOLD;
    float trunkH = mature ? (5.4f + (ageYears-TOOL_AGE_THRESHOLD)*0.15f) : (1.6f + ageYears*0.5f);
    if (trunkH < 0.6f) trunkH = 0.6f;
    return trunkH;
}

// Rumah/kantor kebun sederhana: badan krem + atap segitiga coklat + pintu +
// jendela kecil. Meniru referensi visual yg diberikan pengguna (elemen yg
// SEBELUMNYA TIDAK ADA sama sekali di scene 3D kita).
void drawFarmhouse(float x, float z){
    std::vector<float> wall, roof, door, window;

    std::vector<float> wallBox; buildBox(wallBox, 5.0f, 3.6f, 2.6f);
    appendAt(wall, wallBox, {0,0,0});

    // atap: prisma segitiga (2 sisi miring + 2 sisi ujung berbentuk segitiga)
    float roofW=5.6f, roofD=4.0f, roofH=1.6f, roofY=2.6f;
    V3 flTop{-roofW*0.5f,roofY,-roofD*0.5f}, frTop{roofW*0.5f,roofY,-roofD*0.5f};
    V3 blTop{-roofW*0.5f,roofY, roofD*0.5f}, brTop{roofW*0.5f,roofY, roofD*0.5f};
    V3 ridgeFront{0,roofY+roofH,-roofD*0.5f+0.3f}, ridgeBack{0,roofY+roofH,roofD*0.5f-0.3f};
    pushTri(roof, flTop, frTop, ridgeFront); // sisi depan
    pushTri(roof, blTop, ridgeBack, brTop);  // sisi belakang
    pushTri(roof, flTop, ridgeFront, ridgeBack); pushTri(roof, flTop, ridgeBack, blTop); // sisi kiri
    pushTri(roof, frTop, brTop, ridgeBack); pushTri(roof, frTop, ridgeBack, ridgeFront); // sisi kanan

    std::vector<float> doorBox; buildBox(doorBox, 0.9f, 0.08f, 1.7f);
    appendAt(door, doorBox, {0,0,-1.81f});

    for (float side : {-1.4f, 1.4f}){
        std::vector<float> winBox; buildBox(winBox, 0.7f, 0.08f, 0.7f);
        appendAt(window, winBox, {side,1.4f,-1.81f});
    }

    drawTris(wall.data(), (int)(wall.size()/3), 0.898f,0.855f,0.749f, 1.0f, x,0,z, 1,1,1);   // krem
    drawTris(roof.data(), (int)(roof.size()/3), 0.545f,0.271f,0.176f, 1.0f, x,0,z, 1,1,1);   // coklat atap
    drawTris(door.data(), (int)(door.size()/3), 0.365f,0.243f,0.145f, 1.0f, x,0,z, 1,1,1);   // coklat tua pintu
    drawTris(window.data(), (int)(window.size()/3), 0.616f,0.788f,0.851f, 1.0f, x,0,z, 1,1,1); // biru muda kaca
}

// Figur staf/pengawas statis (BUKAN buruh yg bergerak/drawWorker) — seragam
// beda warna per tingkat jenjang SDM tertinggi yg direkrut pemain, meniru
// referensi visual (figur bertopi kuning & berkemeja beda warna di antara
// kerumunan pemanen). roleLevel: 1=pengawas lapangan (Mandor/Krani/dst,
// topi kuning+rompi oranye), 2=staf kebun (Asisten Afdeling/Kepala, topi
// putih+kemeja biru muda), 3=pimpinan (Manager/ADM, tanpa topi+kemeja ungu).
void drawStaffFigure(float x, float z, int roleLevel){
    if (roleLevel<1) return;
    const float STAFF_SCALE = 4.0f; // sama dgn WORKER_SCALE, konsisten ukuran

    float shirtR,shirtG,shirtB, hatR,hatG,hatB; bool hasHat=true;
    if (roleLevel==1){ shirtR=0.918f; shirtG=0.494f; shirtB=0.157f; hatR=0.949f; hatG=0.831f; hatB=0.220f; }      // rompi oranye, topi kuning
    else if (roleLevel==2){ shirtR=0.427f; shirtG=0.639f; shirtB=0.804f; hatR=0.95f; hatG=0.95f; hatB=0.95f; }   // kemeja biru muda, topi putih
    else { shirtR=0.463f; shirtG=0.267f; shirtB=0.588f; hatR=0; hatG=0; hatB=0; hasHat=false; }                  // kemeja ungu (pimpinan), tanpa topi

    const float skinR=0.867f, skinG=0.678f, skinB=0.478f;
    const float hairR=0.353f, hairG=0.220f, hairB=0.129f;
    const float trouserR=0.180f, trouserG=0.290f, trouserB=0.478f;
    const float bootR=0.420f, bootG=0.290f, bootB=0.169f;
    const float hipY = 0.55f;

    // Animasi IDLE halus -- BUG diperbaiki: sebelumnya seluruh figur staff
    // TOTAL statis (semua vertex dihitung tanpa g_animT sama sekali),
    // terlihat spt "patung mati" -- sangat kontras dgn drawWorker() yg
    // sudah punya walk cycle+bounce penuh. Staff BERDIRI DIAM di posisi
    // tetap (35,12, dekat kantor -- BUKAN berjalan spt worker), jadi tak
    // perlu walk cycle, TAPI tetap butuh tanda "hidup": napas halus (bob
    // naik-turun kecil di torso+kepala) + goyangan bahu/lengan sangat kecil
    // (spt orang berdiri santai, bukan kaku sempurna). Amplitudo SENGAJA
    // kecil (0.012 unit) -- staff sedang "mengawasi", bukan beraktivitas
    // aktif spt worker.
    float breathe = std::sin(g_animT * 1.8f) * 0.012f;
    float swayArm = std::sin(g_animT * 1.3f + 1.0f) * 0.04f;

    std::vector<float> skin, hair, shirt, trouser, boot, hat;

    for (float side : {-1.0f, 1.0f}){
        std::vector<float> bootBox; buildBox(bootBox, 0.09f,0.14f,0.10f);
        appendAt(boot, bootBox, {side*0.09f,0,0});
        std::vector<float> trouserBox; buildBox(trouserBox, 0.08f,0.10f,hipY-0.10f);
        appendAt(trouser, trouserBox, {side*0.09f,0.10f,0});
    }
    std::vector<float> torsoLowerLocal; buildBox(torsoLowerLocal, 0.30f,0.16f,0.20f);
    appendAt(trouser, torsoLowerLocal, {0,hipY,0});
    std::vector<float> torsoUpperLocal; buildBox(torsoUpperLocal, 0.28f,0.15f,0.25f);
    appendAt(shirt, torsoUpperLocal, {0,hipY+0.20f+breathe,0});
    for (float side : {-1.0f, 1.0f}){
        std::vector<float> armBox; buildBox(armBox, 0.075f,0.075f,-0.30f);
        V3 shoulderRelHip{side*0.17f, hipY+0.42f+breathe, 0};
        std::vector<float> armBent;
        for (size_t i=0;i<armBox.size();i+=3){
            V3 p = v3rotX({armBox[i],armBox[i+1],armBox[i+2]}, side*swayArm);
            armBent.push_back(p.x+shoulderRelHip.x); armBent.push_back(p.y+shoulderRelHip.y); armBent.push_back(p.z+shoulderRelHip.z);
        }
        shirt.insert(shirt.end(), armBent.begin(), armBent.end());
    }
    std::vector<float> headLocal; buildBox(headLocal, 0.19f,0.18f,0.20f);
    appendAt(skin, headLocal, {0,hipY+0.45f+breathe,0});
    std::vector<float> hairLocal; buildBox(hairLocal, 0.20f,0.19f,0.08f);
    { std::vector<float> tmp; appendAt(tmp, hairLocal, {0,hipY+0.57f+breathe,-0.02f}); hair.insert(hair.end(),tmp.begin(),tmp.end()); }
    if (hasHat){
        std::vector<float> hatBox; buildBox(hatBox, 0.22f,0.21f,0.07f);
        appendAt(hat, hatBox, {0,hipY+0.63f+breathe,0});
    }

    drawTris(boot.data(), (int)(boot.size()/3), bootR,bootG,bootB, 1.0f, x,0,z, STAFF_SCALE,STAFF_SCALE,STAFF_SCALE);
    drawTris(trouser.data(), (int)(trouser.size()/3), trouserR,trouserG,trouserB, 1.0f, x,0,z, STAFF_SCALE,STAFF_SCALE,STAFF_SCALE);
    drawTris(shirt.data(), (int)(shirt.size()/3), shirtR,shirtG,shirtB, 1.0f, x,0,z, STAFF_SCALE,STAFF_SCALE,STAFF_SCALE);
    drawTris(skin.data(), (int)(skin.size()/3), skinR,skinG,skinB, 1.0f, x,0,z, STAFF_SCALE,STAFF_SCALE,STAFF_SCALE);
    drawTris(hair.data(), (int)(hair.size()/3), hairR,hairG,hairB, 1.0f, x,0,z, STAFF_SCALE,STAFF_SCALE,STAFF_SCALE);
    if (hasHat) drawTris(hat.data(), (int)(hat.size()/3), hatR,hatG,hatB, 1.0f, x,0,z, STAFF_SCALE,STAFF_SCALE,STAFF_SCALE);
}

// Avatar pemain (Gameplay Mode) dari model GLB custom (Blender, lihat catatan
// lengkap di farmer_avatar_mesh.hpp) -- GEOMETRI STATIS (bukan dibangun ulang
// tiap frame spt drawWorker()/drawStaffFigure(), krn sudah di-bake sbg array
// tetap saat ekstraksi). Goyangan IDLE sederhana (napas halus, SAMA konsep
// dgn drawStaffFigure()) -- BUKAN walk-cycle penuh: model asal punya SEMUA
// bagian tubuh menyatu jadi SATU mesh utuh (bukan lengan/kaki terpisah),
// tak bisa diputar independen per-pivot spt sistem animasi drawWorker().
// facingRad memutar SELURUH model via parameter yaw drawTris() (rotasi di
// GPU, bukan manual per-vertex) -- jauh lebih murah krn geometri statis.
void drawFarmerAvatar(float x, float z, float facingRad, bool moving){
    // Shadow elips sederhana di TANAH (Y hampir 0, sedikit di atas supaya
    // tak z-fighting dgn drawGround()) -- mengatasi laporan pengguna #4:
    // "kelihatan pekerja tidak menginjakkan ground... diputar 2 jari juga
    // dengan sudut pandang berbeda, juga masih tidak menginjakkan tanah".
    // AKAR MASALAH bukan bug posisi (avatar SUDAH digambar tepat di Y=0,
    // terverifikasi numerik) -- tapi TAK ADA shadow/bayangan SAMA SEKALI
    // di renderer ini utk objek apapun. Diverifikasi numerik: pada proyeksi
    // kamera tilt (~55 derajat, khas Management Mode), titik setinggi
    // kepala avatar (Y=1.92) bergeser 70+ piksel dari titik kaki (Y=0) di
    // posisi dunia SAMA -- tanpa penanda visual jelas di tanah, mata tak
    // py referensi titik kontak sebenarnya, siluet avatar terkesan
    // "mengambang" di suatu tempat, BUKAN "berdiri" di titik tertentu.
    // Shadow elips gelap semi-transparan memberi titik jangkar visual yg
    // jelas -- solusi umum di game isometrik/orthografis utk masalah
    // persepsi kedalaman spt ini. TETAP terlihat scr konsisten dari SEMUA
    // sudut yaw (rotasi 2 jari, poin laporan) krn elips digambar FLAT di
    // Y tanah, posisinya independen dari sudut pandang kamera manapun.
    {
        const int SIDES = 16;
        const float RX = 0.32f, RZ = 0.20f; // elips (bukan lingkaran) -- kesan alami perspektif tanah
        std::vector<float> shadow;
        V3 c{x, 0.003f, z};
        for (int s=0;s<SIDES;s++){
            float a0=(float)s/SIDES*6.28318f, a1=(float)(s+1)/SIDES*6.28318f;
            V3 p0{x+std::cos(a0)*RX, 0.003f, z+std::sin(a0)*RZ};
            V3 p1{x+std::cos(a1)*RX, 0.003f, z+std::sin(a1)*RZ};
            pushTri(shadow, c, p0, p1);
        }
        drawTris(shadow.data(), (int)(shadow.size()/3), 0.0f, 0.0f, 0.0f, 0.35f, 0,0,0, 1,1,1);
    }

    // Goyangan napas halus + sedikit ayun saat berjalan (amplitudo KECIL --
    // model detail begini terlihat aneh kalau di-bounce besar spt drawWorker()
    // yg memang didesain utk box sederhana).
    float breathe = std::sin(g_animT * 1.8f) * 0.010f;
    float walkBob = moving ? std::abs(std::sin(g_animT * 5.2f)) * 0.022f : 0.0f;
    float yOffset = breathe + walkBob;

    // BUG proporsi diperbaiki (dilaporkan pengguna dari screen record:
    // "tinggi pekerja dan pohon sawit tidak sebanding"). SEBELUMNYA
    // FARMER_SCALE=4.0 -> tinggi pekerja 4.80 unit, TERBUKTI numerik LEBIH
    // TINGGI dari pohon TERMUDA sekalipun (2 tahun, 4.40 unit). Sekarang
    // 1.6 -> tinggi pekerja 1.92 unit, MEMBERI rasio pohon:pekerja 2.3x
    // (termuda) sampai 3.7x (tertua) -- pohon SELALU terlihat jelas lebih
    // tinggi, sesuai realita botani (manusia dewasa jauh lebih pendek dari
    // kelapa sawit bahkan yg masih muda). kAvatarCamHeight/DistMin/Max/
    // GroundClearance ikut diskalakan proporsional (rasio sama, 0.4) di
    // atas -- framing kamera third-person tetap koheren.
    const float FARMER_SCALE = 1.6f;

    // BUG "celurit tembus badan" diperbaiki (dilaporkan pengguna). Akar
    // masalah SAMA dgn bug "skin menembus celana" sebelumnya: TIDAK ADA
    // GL_DEPTH_TEST global di renderer ini (SENGAJA, lihat catatan init() --
    // kamera ortografis tilt bikin depth GLOBAL rusak utk pohon/tanah), jadi
    // urutan drawTris() manual yg menentukan menang-kalah scr visual, BUKAN
    // posisi Z sebenarnya. "tool" (celurit) SEBELUMNYA digambar PALING
    // TERAKHIR supaya menimpa bagian lain -- ini BEKERJA saat celurit
    // seharusnya di DEPAN badan, TAPI SALAH TOTAL saat celurit seharusnya
    // tersembunyi DI BELAKANG badan (mis. dipegang di sisi yg membelakangi
    // kamera) -- celurit tetap "menang"/terlihat menembus, krn urutan
    // gambar manual tak bisa membedakan kasus per-frame spt ini.
    //
    // GL_DEPTH_TEST TIDAK LAGI diaktifkan/dibersihkan DI SINI (BEDA dari
    // versi sebelumnya) -- BUG ditemukan sendiri saat investigasi laporan
    // pengguna #2 ("rumah tampak menembus tubuh pekerja"): drawFarmhouse()
    // digambar SETELAH loop worker TANPA depth test sama sekali, jadi
    // rumah SELALU menang/menimpa worker terlepas posisi sebenarnya --
    // clear+enable/disable LOKAL per-avatar (versi lama) tak bisa
    // mengatasi ini krn depth buffer di-reset habis setiap avatar,
    // "melupakan" apapun yg digambar sebelumnya termasuk rumah. Sekarang
    // GL_DEPTH_TEST diaktifkan di level LEBIH TINGGI (JNI/EngineBridge,
    // membungkus SELURUH blok: rumah+staff+SEMUA worker+avatar sekaligus,
    // lihat nativeGlDrawFrame) -- depth buffer dipakai BERSAMA oleh semua
    // objek dlm blok itu, celurit TETAP benar (dites sendiri, geometrinya
    // kompak+dekat kamera third-person) DAN rumah-vs-pekerja jg benar
    // (keduanya kini saling depth-test, bukan salah satu "buta" thd yg lain).

    drawTris(kFarmerAvatar_skin, kFarmerAvatar_skin_COUNT, kFarmerAvatarColor_skin[0],kFarmerAvatarColor_skin[1],kFarmerAvatarColor_skin[2], 1.0f, x,yOffset,z, FARMER_SCALE,FARMER_SCALE,FARMER_SCALE, facingRad);
    drawTris(kFarmerAvatar_boot, kFarmerAvatar_boot_COUNT, kFarmerAvatarColor_boot[0],kFarmerAvatarColor_boot[1],kFarmerAvatarColor_boot[2], 1.0f, x,yOffset,z, FARMER_SCALE,FARMER_SCALE,FARMER_SCALE, facingRad);
    drawTris(kFarmerAvatar_overall, kFarmerAvatar_overall_COUNT, kFarmerAvatarColor_overall[0],kFarmerAvatarColor_overall[1],kFarmerAvatarColor_overall[2], 1.0f, x,yOffset,z, FARMER_SCALE,FARMER_SCALE,FARMER_SCALE, facingRad);
    drawTris(kFarmerAvatar_shirt, kFarmerAvatar_shirt_COUNT, kFarmerAvatarColor_shirt[0],kFarmerAvatarColor_shirt[1],kFarmerAvatarColor_shirt[2], 1.0f, x,yOffset,z, FARMER_SCALE,FARMER_SCALE,FARMER_SCALE, facingRad);
    drawTris(kFarmerAvatar_hair, kFarmerAvatar_hair_COUNT, kFarmerAvatarColor_hair[0],kFarmerAvatarColor_hair[1],kFarmerAvatarColor_hair[2], 1.0f, x,yOffset,z, FARMER_SCALE,FARMER_SCALE,FARMER_SCALE, facingRad);
    drawTris(kFarmerAvatar_hat, kFarmerAvatar_hat_COUNT, kFarmerAvatarColor_hat[0],kFarmerAvatarColor_hat[1],kFarmerAvatarColor_hat[2], 1.0f, x,yOffset,z, FARMER_SCALE,FARMER_SCALE,FARMER_SCALE, facingRad);
    drawTris(kFarmerAvatar_tool, kFarmerAvatar_tool_COUNT, kFarmerAvatarColor_tool[0],kFarmerAvatarColor_tool[1],kFarmerAvatarColor_tool[2], 1.0f, x,yOffset,z, FARMER_SCALE,FARMER_SCALE,FARMER_SCALE, facingRad);
}

// Portal/gerbang kebun: dua tiang, palang merah-putih (bisa terangkat/tidak,
// di sini digambar dlm posisi TERTUTUP/mendatar spt penjagaan aktif), + pos
// jaga kecil di sampingnya. Praktik standar stasiun penerimaan kebun sawit
// sungguhan -- lihat kutipan literatur di engine.hpp (kGateX/kGateZ).
void drawGate(float x, float z, float facingRad){
    std::vector<float> posts, barrierRed, barrierWhite, booth, roof;
    auto rotPlace = [&](std::vector<float>& out, const std::vector<float>& local, V3 off){
        for (size_t i=0;i<local.size();i+=3){
            V3 p = v3rotY({local[i],local[i+1],local[i+2]}, facingRad);
            out.push_back(p.x+off.x); out.push_back(p.y+off.y); out.push_back(p.z+off.z);
        }
    };

    // dua tiang portal, mengapit jalan (sumbu Z)
    for (float side : {-1.6f, 1.6f}){
        std::vector<float> postBox; buildBox(postBox, 0.14f, 0.14f, 1.9f);
        std::vector<float> tmp; appendAt(tmp, postBox, {side,0,0}); rotPlace(posts, tmp, {0,0,0});
    }
    // palang mendatar, selang-seling merah-putih (spt palang perlintasan/portal sungguhan)
    const int SEGMENTS = 6;
    float barLen = 3.2f, segLen = barLen/SEGMENTS;
    for (int s=0;s<SEGMENTS;s++){
        std::vector<float> segBox; buildBox(segBox, segLen*0.94f, 0.10f, 0.10f);
        std::vector<float> tmp; appendAt(tmp, segBox, {-barLen*0.5f+segLen*(s+0.5f), 1.75f, 0});
        auto& target = (s%2==0) ? barrierRed : barrierWhite;
        std::vector<float> rotated; rotPlace(rotated, tmp, {0,0,0});
        target.insert(target.end(), rotated.begin(), rotated.end());
    }
    // pos jaga kecil di samping
    std::vector<float> boothBox; buildBox(boothBox, 1.3f, 1.3f, 1.6f);
    { std::vector<float> tmp; appendAt(tmp, boothBox, {2.6f,0,-0.8f}); rotPlace(booth, tmp, {0,0,0}); }
    float ry=1.6f;
    { std::vector<float> tmp;
      V3 c0{2.6f-0.8f,ry,-0.8f-0.8f}, c1{2.6f+0.8f,ry,-0.8f-0.8f}, c2{2.6f+0.8f,ry,-0.8f+0.8f}, c3{2.6f-0.8f,ry,-0.8f+0.8f};
      V3 tip{2.6f,ry+0.5f,-0.8f};
      pushTri(tmp,c0,c1,tip); pushTri(tmp,c1,c2,tip); pushTri(tmp,c2,c3,tip); pushTri(tmp,c3,c0,tip);
      rotPlace(roof, tmp, {0,0,0});
    }

    drawTris(posts.data(), (int)(posts.size()/3), 0.75f,0.75f,0.73f, 1.0f, x,0,z, 1,1,1);       // tiang abu-abu
    drawTris(barrierRed.data(), (int)(barrierRed.size()/3), 0.78f,0.13f,0.11f, 1.0f, x,0,z, 1,1,1);   // palang merah
    drawTris(barrierWhite.data(), (int)(barrierWhite.size()/3), 0.95f,0.95f,0.93f, 1.0f, x,0,z, 1,1,1); // palang putih
    drawTris(booth.data(), (int)(booth.size()/3), 0.90f,0.86f,0.75f, 1.0f, x,0,z, 1,1,1);       // pos jaga krem
    drawTris(roof.data(), (int)(roof.size()/3), 0.55f,0.27f,0.18f, 1.0f, x,0,z, 1,1,1);         // atap coklat
}

// Dipanggil SEKALI dari platform layer (Kotlin/Swift) saat inspector BARU
// dibuka (openTreeInspector()), SEBELUM drawTreeInspectorFrame() pertama
// kali dipanggil frame berikutnya -- menyalin posisi kamera scene utama
// SAAT ITU sbg titik awal animasi smooth menuju close-up (bukan mulai dari
// 0/default yg bisa terasa aneh).
void beginTreeInspectorTransition(){
    g_inspCamPanX = g_panX; g_inspCamPanY = g_panY; g_inspCamPanZ = g_panZ;
    g_inspCamDist = g_dist;
    g_inspCamInitialized = true;
}

void drawTreeInspectorFrame(float ageYears, float frond, int health, int ffb, bool hasTbsReady, float yawSpin, float panY, float nutrition){
    // Simpan kamera game utama SEBELUM diubah -- dipulihkan di akhir supaya
    // drawFrame() normal berikutnya tidak "meloncat" krn state kamera global
    // (g_panX/g_panZ/g_panY/g_dist/g_yaw) ini dipakai bersama oleh scene utama.
    float savedPanX=g_panX, savedPanY=g_panY, savedPanZ=g_panZ, savedDist=g_dist, savedYaw=g_yaw;
    // BUG diperbaiki (dilaporkan pengguna: "Menu detail pohon tidak kelihatan
    // tampilan"). Akar masalah: updateViewProj() SELALU memakai proyeksi
    // PERSPEKTIF third-person (mengikuti posisi avatar, lewat g_avatarCamEye*/
    // g_avatarLookAt*) saat g_gameplayModeActive true -- MENGABAIKAN TOTAL
    // g_panX/g_panY/g_panZ/g_dist/g_yaw yg diatur DI BAWAH utk kamera
    // close-up inspector. Kalau pemain buka "Detail 3D" SAAT sedang dlm
    // mode berjalan (Gameplay Mode aktif), kamera TETAP terkunci ke posisi
    // avatar (jauh dari origin tempat pohon digambar di sini) -- pohon tak
    // pernah terlihat sama sekali. Paksa false SEMENTARA (dipulihkan di
    // akhir, sama pola dgn simpan&pulihkan kamera di atas) spy
    // updateViewProj() memakai cabang ORTOGRAFIS yg memang memakai
    // g_panX/g_dist/g_yaw ini.
    bool savedGameplayMode = g_gameplayModeActive;
    g_gameplayModeActive = false;

    // Kamera close-up: pohon SELALU digambar di origin (0,0) dlm mode ini
    // (bukan posisi dunia sungguhannya) -- lebih simpel & konsisten dipotret
    // dari sudut manapun. Jarak kamera menyesuaikan tinggi pohon spy pohon
    // muda & tua sama-sama pas dlm bingkai. panY = geser vertikal titik fokus
    // kamera (0 = pusat default, dinaikkan pemanggil via gesture scroll spy
    // pemain bisa lihat dari akar/pangkal batang [y=0] sampai ke puncak
    // mahkota -- sebelumnya kamera terkunci total tanpa cara menggeser).
    float trunkH = treeTrunkHeight(ageYears);
    float targetDist = std::max(10.0f, trunkH*2.3f);
    // BUG UX diperbaiki (dilaporkan pengguna: "Saat inspeksi pohon kamera
    // melakukan smooth focus/zoom ke pohon") -- SEBELUMNYA g_panX/panY/panZ/
    // dist di-set LANGSUNG ke target (0,panY,0,targetDist) tanpa transisi,
    // kamera "melompat" mendadak. Sekarang di-smooth exponensial (konsisten
    // pola updateThirdPersonCamera(), konstanta 8.0 dipilih cukup cepat
    // spy tak terasa lambat/mengganggu, tapi tetap halus terlihat).
    // g_yaw (rotasi) SENGAJA TAK di-smooth di sini -- sudah smooth alami
    // dari auto-spin/rotasi manual yg sudah ada sebelumnya (yawSpin sendiri
    // berputar bertahap tiap frame), smoothing tambahan di sini malah bikin
    // kamera "tertinggal" terus mengejar target yg terus bergerak.
    if (!g_inspCamInitialized){
        // Jaga-jaga kalau beginTreeInspectorTransition() belum sempat
        // dipanggil (mis. pemanggilan langsung tanpa lewat platform layer) --
        // langsung ke target, tak ada posisi awal utk di-smooth.
        g_inspCamPanX=0; g_inspCamPanY=panY; g_inspCamPanZ=0; g_inspCamDist=targetDist;
        g_inspCamInitialized = true;
    }
    float dt = 0.033f; // frame time tetap, konsisten dgn g_animT increment di bawah
    float smoothing = 1.0f - std::exp(-dt * 8.0f);
    g_inspCamPanX += (0.0f - g_inspCamPanX) * smoothing;
    g_inspCamPanY += (panY - g_inspCamPanY) * smoothing;
    g_inspCamPanZ += (0.0f - g_inspCamPanZ) * smoothing;
    g_inspCamDist += (targetDist - g_inspCamDist) * smoothing;
    g_panX = g_inspCamPanX; g_panY = g_inspCamPanY; g_panZ = g_inspCamPanZ; g_dist = g_inspCamDist; g_yaw = yawSpin;
    updateViewProj();

    glClear(GL_COLOR_BUFFER_BIT);
    g_animT += 0.033f; // dodos/egrek dkk pakai ini jg, tp inspector tak gambar pekerja -- aman

    // Pedestal tanah kecil (bukan hamparan 90x76 unit spt kebun utama --
    // akan terlihat aneh dizoom sedekat ini) -- warna sama dgn tanah kebun.
    const int SIDES = 20;
    std::vector<float> pedestal;
    float r = std::max(3.0f, trunkH*0.9f);
    for (int s=0;s<SIDES;s++){
        float a0=(float)s/SIDES*6.28318f, a1=(float)(s+1)/SIDES*6.28318f;
        V3 c{0,0,0}, p0{std::cos(a0)*r,0,std::sin(a0)*r}, p1{std::cos(a1)*r,0,std::sin(a1)*r};
        pushTri(pedestal, c, p0, p1);
    }
    drawTris(pedestal.data(), (int)(pedestal.size()/3), 0.694f,0.616f,0.475f, 1.0f, 0,0,0, 1,1,1);

    drawPalm(0.0f, 0.0f, ageYears, frond, health, ffb, false, nutrition);
    if (hasTbsReady) drawTbsPile(0.0f, 0.0f);

    // Pulihkan kamera game utama + status Gameplay Mode.
    g_panX=savedPanX; g_panY=savedPanY; g_panZ=savedPanZ; g_dist=savedDist; g_yaw=savedYaw;
    g_gameplayModeActive = savedGameplayMode;
    updateViewProj();
}

void endFrame(){
    // no-op utk GLES2 (buffer swap ditangani GLSurfaceView di Kotlin)
}

void worldToScreenY(float x, float y, float z, float* outScreenX, float* outScreenY){
    float clip[4];
    float wx=x, wy=y, wz=z, ww=1;
    for(int row=0; row<4; ++row){
        clip[row] = g_viewProj.m[0*4+row]*wx + g_viewProj.m[1*4+row]*wy
                  + g_viewProj.m[2*4+row]*wz + g_viewProj.m[3*4+row]*ww;
    }
    float ndcX = clip[0]/clip[3], ndcY = clip[1]/clip[3];
    *outScreenX = (ndcX*0.5f+0.5f) * g_width;
    *outScreenY = (1.0f-(ndcY*0.5f+0.5f)) * g_height;
}

void worldToScreen(float x, float z, float* outScreenX, float* outScreenY){
    worldToScreenY(x, 0.0f, z, outScreenX, outScreenY);
}

// Cek apakah titik dunia (x,z) MASIH BISA TERLIHAT di layar saat ini --
// dasar OPTIMASI VIEWPORT CULLING: dgn banyak block (each 143 pohon,
// ~2600+ segitiga/pohon), me-render SEMUA block sekaligus tanpa peduli
// apakah sedang terlihat di layar bikin FPS ANJLOK drastis begitu jumlah
// block bertambah (dilaporkan pengguna: >3-4 block, FPS turun ke ~8). Cukup
// render pohon yang BENAR-BENAR ada dlm jangkauan pandang kamera saat ini.
//
// worldRadius: radius OBJEK dlm UNIT DUNIA (bukan piksel) -- BUG dicegah:
// mengecek visibilitas HANYA dari 1 titik origin TIDAK CUKUP utk objek
// BESAR spt satu Block (~90 unit lebar) -- kalau kamera melihat bagian
// TENGAH/PINGGIR block (bukan persis titik origin-nya), objek besar BISA
// SALAH di-cull padahal sebagian masih terlihat (tanah hilang tiba-tiba
// padahal pohon2 di block itu masih tampak, krn pohon dicek per-individu
// dgn presisi tinggi sementara tanah cuma dicek dari 1 titik origin yg jauh
// dr pohon2 yg sebenarnya masih terlihat). 0 = titik kecil spt pohon
// individu (margin skrn saja sudah cukup); >0 = objek besar spt Block.
// worldRadius dikonversi ke margin piksel PROPORSIONAL thd zoom kamera
// (g_dist) -- supaya tetap benar di tingkat zoom berapa pun, bukan margin
// piksel tetap yg cuma benar di 1 tingkat zoom tertentu.
// Pengaturan GRAFIK & sensitivitas kamera -- fitur baru diminta pengguna
// ("tambahkan pengaturan sensivitas dan grafik"). Fungsi PUBLIC di sini
// (bukan di dalam anonymous namespace, lihat variabel g_graphicsQuality/
// g_cameraSensitivity di dekat g_avatarCamDistBehind) -- dipanggil dari
// JNI/EngineBridge sbg pengaturan pengguna.
float graphicsQualityMultiplier(){
    // Mengalikan MARGIN culling isWorldPointVisible() (di bawah) -- MAKIN
    // KECIL, MAKIN AGRESIF culling (objek mulai disembunyikan lebih dekat
    // ke tepi layar, mengurangi jumlah triangle digambar tiap frame).
    // Tombol utama mengatasi FPS rendah di lahan luas (100ha+) pada HP
    // kelas bawah -- jarak render VISUAL brubah, gameplay/collision TETAP
    // penuh terlepas setting ini.
    switch(g_graphicsQuality){
        case 0: return 0.55f;  // Rendah (paling agresif/FPS terbaik)
        case 2: return 1.4f;   // Tinggi (margin lebih longgar, pop-in lebih halus)
        default: return 1.0f;  // Sedang (default lama)
    }
}
void setGraphicsQuality(int level){ g_graphicsQuality = std::max(0, std::min(2, level)); }
int getGraphicsQuality(){ return g_graphicsQuality; }
// Sensitivitas kamera (touch-drag "lihat sekeliling") -- MULTIPLIER thd
// sensitivitas dasar yg sudah ada di platform layer (Kotlin/Swift).
void setCameraSensitivity(float mult){ g_cameraSensitivity = std::max(0.3f, std::min(3.0f, mult)); }
float getCameraSensitivity(){ return g_cameraSensitivity; }

// Blok depth-test utk KARAKTER+BANGUNAN (worker/avatar/rumah/staff) --
// mengatasi laporan pengguna #2: "rumah tampak menembus tubuh pekerja".
// AKAR MASALAH ditemukan: drawFarmhouse() digambar SETELAH loop worker
// TANPA depth test sama sekali (renderer ini SENGAJA tanpa GL_DEPTH_TEST
// global, lihat catatan init() -- kamera ortografis tilt bikin depth
// GLOBAL rusak utk pohon/tanah), jadi rumah SELALU menang/menimpa worker
// terlepas posisi Z sebenarnya. Dipanggil dari JNI/EngineBridge SEBELUM
// loop worker+rumah+staff dimulai (begin) & SETELAH semuanya selesai
// (end) -- SEMUA objek dlm blok ini (termasuk drawFarmerAvatar()'s
// bagian tubuh sendiri, celurit dkk) saling depth-test dgn benar krn
// berbagi SATU depth buffer yg TAK di-clear di tengah blok. TIDAK
// mempengaruhi rendering SEBELUM/SESUDAH blok ini (pohon, tanah, dll
// yg sengaja tanpa depth test krn alasan proyeksi tilt yg berbeda).
void beginCharacterDepthBlock(){
    glEnable(GL_DEPTH_TEST);
    glClear(GL_DEPTH_BUFFER_BIT);
}
void endCharacterDepthBlock(){
    glDisable(GL_DEPTH_TEST);
}

bool isWorldPointVisible(float x, float z, float worldRadius){
    float sx, sy;
    worldToScreen(x, z, &sx, &sy);
    // Skala piksel-per-unit-dunia mengikuti proyeksi ortografis yg dipakai
    // (lihat resize(): halfH = g_dist*0.45f dipetakan ke g_height piksel).
    float halfH_world = g_dist * 0.45f;
    float pxPerWorldUnit = (halfH_world > 0.0001f) ? (g_height / (2.0f*halfH_world)) : 1.0f;
    float extraPx = worldRadius * pxPerWorldUnit;
    // Margin dikalikan graphicsQualityMultiplier() -- pengaturan grafik baru
    // (Rendah/Sedang/Tinggi) diminta pengguna, jadi tombol utama mengatasi
    // FPS rendah di lahan luas: kualitas Rendah = margin lebih SEMPIT =
    // objek disembunyikan lebih dekat ke tepi layar = lebih sedikit
    // triangle digambar tiap frame.
    float qMult = graphicsQualityMultiplier();
    float marginTop = (g_height * 0.35f + extraPx) * qMult;    // kanopi menjulang tinggi dari titik dasar
    float marginSide = (g_width * 0.15f + extraPx) * qMult;    // lebar kanopi wajar
    float marginBottom = (g_height * 0.15f + extraPx) * qMult;
    return sx > -marginSide && sx < g_width+marginSide &&
           sy > -marginTop && sy < g_height+marginBottom;
}

float hitTestDistance(float screenX, float screenY, float treeX, float treeZ, float ageYears){
    // Tinggi batang HARUS persis sama dgn formula di drawPalm() supaya hit-test
    // selalu sinkron dgn yang benar-benar tergambar di layar -- makanya pakai
    // treeTrunkHeight() bersama, bukan rumus terpisah lagi.
    float trunkH = treeTrunkHeight(ageYears);

    float best = 1e30f;
    auto testPoint = [&](float wx, float wy, float wz){
        float sx, sy;
        worldToScreenY(wx, wy, wz, &sx, &sy);
        float dx = sx-screenX, dy = sy-screenY;
        float d = std::sqrt(dx*dx + dy*dy);
        if (d < best) best = d;
    };

    // Sepanjang batang, dari dasar sampai puncak — ini yg dulu HILANG (hit-test
    // lama cuma cek dasar/y=0), makanya ketuk pelepah di bagian atas tidak kena.
    for(int i=0; i<=4; i++){
        float t = (float)i/4;
        testPoint(treeX, trunkH*t, treeZ);
    }
    // Sebaran mahkota/pelepah: beberapa titik di sekitar radius tajuk pada dua
    // ketinggian atas, supaya ketukan di ujung pelepah yg merunduk keluar juga kena.
    const float crownR = 2.6f;
    const float heights[2] = { trunkH*0.55f, trunkH*0.90f };
    for(float hy : heights){
        for(int k=0; k<6; k++){
            float ang = (float)k/6*6.28318f;
            testPoint(treeX+std::cos(ang)*crownR, hy, treeZ+std::sin(ang)*crownR);
        }
    }
    return best;
}

void screenToWorldOnGroundPlane(float screenX, float screenY, float* outX, float* outZ){
    // Proyeksi ortografis -> tidak perlu unproject penuh; cukup balik transformasi 2D
    // karena bidang y=0 tegak lurus terhadap sumbu proyeksi ortho isometrik kita.
    float ndcX = (screenX / g_width) * 2.0f - 1.0f;
    float ndcY = 1.0f - (screenY / g_height) * 2.0f;
    float aspect = (g_height>0) ? (float)g_width/(float)g_height : 1.0f;
    float halfH = g_dist * 0.45f;
    float halfW = halfH * aspect;
    float A = ndcX * halfW; // = view-space X setelah RotateY(yaw) (RotateX tidak mengubah X)
    float B = ndcY * halfH; // = view-space Y setelah RotateX(pitch), dgn Y-dunia=0 di bidang tanah

    // Derivasi lengkap (lihat updateViewProj: view = RotateX(pitch) * RotateY(yaw) * Translate(-pan)):
    // Untuk titik dunia (X1,0,Z1) relatif kamera (X1=x-panX, Z1=z-panZ):
    //   A = cos(yaw)*X1 + sin(yaw)*Z1
    //   B = sin(pitch) * ( sin(yaw)*X1 - cos(yaw)*Z1 )      <-- PENTING: sin(pitch), bukan cos(pitch)
    // Selesaikan sistem 2x2 (determinan = -1):
    //   X1 =  A*cos(yaw) + C*sin(yaw)
    //   Z1 =  A*sin(yaw) - C*cos(yaw)     <-- PENTING: bukan negatifnya
    // dengan C = B / sin(pitch).
    const float C = B / std::sin(kPitch);
    const float cosYaw = std::cos(g_yaw), sinYaw = std::sin(g_yaw);
    float X1 = A*cosYaw + C*sinYaw;
    float Z1 = A*sinYaw - C*cosYaw;
    *outX = X1 + g_panX;
    *outZ = Z1 + g_panZ;
}

void panWorldDelta(float startScreenX, float startScreenY, float endScreenX, float endScreenY,
                    float* outDx, float* outDz){
    // Pakai fungsi screenToWorldOnGroundPlane yg SAMA persis dgn hit-test (sudah
    // tervalidasi lewat round-trip test) — otomatis benar mengikuti kemiringan
    // DAN rotasi kamera saat ini, berapa pun g_yaw sekarang. g_panX/g_panZ
    // (state saat ini) SALING MENGHILANGKAN di pengurangan, jadi hasil delta ini
    // tidak bergantung pada nilai pan sekarang — aman dipanggil berulang tiap
    // event gesture drag.
    float x1, z1, x2, z2;
    screenToWorldOnGroundPlane(startScreenX, startScreenY, &x1, &z1);
    screenToWorldOnGroundPlane(endScreenX, endScreenY, &x2, &z2);
    // Konvensi "cengkeram & seret": titik dunia yg tadinya di bawah jari (start)
    // harus muncul di bawah jari yg baru (end) -> pan bertambah sejumlah (start-end).
    *outDx = x1 - x2;
    *outDz = z1 - z2;
}

} // namespace sawit::gl
