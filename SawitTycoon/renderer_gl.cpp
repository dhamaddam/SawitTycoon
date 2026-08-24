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
float g_panY = 0.0f; // geser vertikal -- HANYA dipakai mode Inspector Pohon (scroll
                      // lihat dari akar sampai mahkota); kebun utama tak pernah
                      // menyentuh ini, jadi selalu 0 di sana (aman, tak ada regresi).
float g_animT = 0.0f; // jam animasi global, maju sedikit tiap beginFrame() -- dipakai
                       // utk animasi menekan-narik dodos/egrek (lihat drawWorker)
float g_yaw = 0.7854f; // 45 derajat, sudut isometrik klasik — sekarang BISA diputar (fitur 360°)
const float kPitch = -0.9599f; // ~55 derajat -- lebih tegak ke bawah (mirip foto udara) drpd
                                // versi sebelumnya (-35deg), tapi tidak full 90 derajat spy
                                // batang & mahkota masih bisa dibedakan utk ketuk-pilih pohon.
                                // AMAN diubah ke nilai lain (asal < 0 dan > -PI/2) karena
                                // seluruh pipeline (render/hit-test/pan) ambil dari sini.
Mat4 g_view, g_proj, g_viewProj;

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

void updateViewProj(){
    // Sudut isometrik klasik: pitch ~ -35.264deg (arctan(1/sqrt(2))) TETAP,
    // yaw sekarang bisa diputar bebas (g_yaw) utk fitur lihat 360°.
    Mat4 rot = mat4Multiply(mat4RotateX(kPitch), mat4RotateY(g_yaw));
    Mat4 trans = mat4Translate(-g_panX, -g_panY, -g_panZ);
    g_view = mat4Multiply(rot, trans);

    float aspect = (g_height>0) ? (float)g_width/(float)g_height : 1.0f;
    float halfH = g_dist * 0.45f;
    float halfW = halfH * aspect;
    g_proj = mat4Ortho(-halfW, halfW, -halfH, halfH, -100.0f, 100.0f);
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
              float x, float y, float z, float sx, float sy, float sz, float yaw=0.0f){
    Mat4 model = mat4Multiply(mat4Translate(x,y,z), mat4Multiply(mat4RotateY(yaw), mat4Scale(sx,sy,sz)));
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

void beginFrame(){
    updateViewProj();
    glClear(GL_COLOR_BUFFER_BIT);
    g_animT += 0.033f; // aproksimasi ~30fps; cukup utk animasi bob/hentak, tak perlu presisi wall-clock
}

void drawGround(){
    // Dasar tanah: coklat/pasir — meniru warna permukaan tanah kebun sawit
    // (lateritic, umum di kebun Indonesia). Diperlebar dari versi sebelumnya
    // supaya menutupi grid 143 pokok (11 kolom x 13 baris, lihat engine.cpp).
    static const float quad[] = {
        -45,0,-38,  45,0,-38,  45,0,38,
        -45,0,-38,  45,0,38,  -45,0,38,
    };
    drawTris(quad, 6, 0.694f, 0.616f, 0.475f, 1.0f, 0,0,0, 1,1,1); // #b19d79 tanah/pasir

    // Tumpukan mulsa pelepah bekas tunas ("gawangan mati") di jalur ANTARA
    // baris tanam — literatur SOP: pelepah hasil tunas ditumpuk di gawangan
    // MATI (jalur selang-seling, BUKAN gawangan hidup/jalur akses panen) sbg
    // mulsa & pengendali erosi. Makanya di sini cuma SETENGAH koridor yg diisi
    // mulsa (row%2==0), sisanya jalur bersih (gawangan hidup) -- sesuai SOP,
    // sekaligus otomatis mengurangi jumlah geometri. Tiap koridor digabung jadi
    // SATU draw call (bukan 1 per patch) spy tetap ringan meski kebun 143 pokok.
    // Bentuknya SENGAJA organik (patch pendek posisi&rotasi acak-tapi-tetap),
    // BUKAN garis lurus penuh spt versi lama, krn di kebun sungguhan tumpukan
    // pelepah tidak pernah rapi lurus sempurna.
    const float ROW_SPACING = 4.507f;   // = colSpacing(5.2) * 0.8667 (rasio SOP 7.8/9)
    const float ROW_ORIGIN_Z = -6.0f*ROW_SPACING; // pusatkan spt originZ 13 baris di engine
    const float GROUND_HALF_W = 44.0f;

    unsigned seed = 0xC0FFEEu;
    auto frand=[&](){ seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return (seed&0xFFFFFF)/float(0xFFFFFF); };

    for (int row=-1; row<=13; row++){
        if (((row+20) % 2) != 0) continue; // gawangan mati selang-seling saja (SOP)
        float zc = ROW_ORIGIN_Z + (row+0.5f)*ROW_SPACING;
        if (zc < -37.0f || zc > 37.0f) continue;
        std::vector<float> corridor; // semua patch koridor ini digabung -> 1 draw call
        float xCursor = -GROUND_HALF_W;
        while (xCursor < GROUND_HALF_W){
            float len = 3.0f + frand()*4.5f;
            float width = 0.9f + frand()*0.9f;
            float rot = (frand()-0.5f)*0.35f;      // sedikit miring, kesan organik
            float zJitter = (frand()-0.5f)*0.6f;
            float xCenter = xCursor + len*0.5f;
            V3 c{xCenter, 0, zc+zJitter};
            V3 half{len*0.5f, 0, width*0.5f};
            V3 p0 = v3rotY({-half.x,0,-half.z}, rot);
            V3 p1 = v3rotY({ half.x,0,-half.z}, rot);
            V3 p2 = v3rotY({ half.x,0, half.z}, rot);
            V3 p3 = v3rotY({-half.x,0, half.z}, rot);
            pushQuad(corridor,
                V3{p0.x+c.x,0.004f,p0.z+c.z}, V3{p1.x+c.x,0.004f,p1.z+c.z},
                V3{p2.x+c.x,0.004f,p2.z+c.z}, V3{p3.x+c.x,0.004f,p3.z+c.z});
            xCursor += len + 0.8f + frand()*1.5f; // jarak antar patch (celah, bukan sambung terus)
        }
        if (!corridor.empty())
            drawTris(corridor.data(), (int)(corridor.size()/3), 0.336f,0.294f,0.216f, 1.0f, 0,0,0, 1,1,1); // #564b37
    }
}

void drawPalm(float x, float z, float ageYears, float frond, int health, int ffb, bool selected){
    (void)frond; // mesh STL yg dibaked adalah bentuk TETAP -- tidak bisa ditekuk per parameter
                 // spt versi prosedural sebelumnya (lihat catatan "Yang belum ada" di README)
    float trunkH = treeTrunkHeight(ageYears); // satu sumber kebenaran, dipakai jg oleh hitTestDistance & JNI/bridge

    // Skala: mesh STL asli tingginya kPalmIconRefHeight (~5.02) pada proporsi
    // acuan trunkH=2.55. Diredam (S_crown) spt versi sebelumnya supaya pokok
    // tua tidak membengkak & bertabrakan dgn tetangga (jarak tanam 5.2 unit).
    const float S = trunkH / 2.55f;
    const float S_crown = 1.0f + (S-1.0f)*0.4f;
    (void)kPalmIconRefHeight;

    // --- warna batang: COKLAT (dulu ikut hijau krn klasifikasi lama cuma pakai
    //     tanda X, bukan tinggi — sudah diperbaiki di palm_icon_mesh.hpp v3) ---
    float trunkR=0.545f, trunkG=0.416f, trunkB=0.267f; // coklat
    if (health==3 /*mati*/) { trunkR=0.35f; trunkG=0.29f; trunkB=0.19f; }

    // --- warna mahkota/pelepah: dua nuansa hijau, bergeser mengikuti kesehatan ---
    float gR=0.110f, gG=0.604f, gB=0.294f;      // hijau terang -> grup "crown_light"
    float gdR=0.075f, gdG=0.478f, gdB=0.227f;   // hijau gelap  -> grup "crown_dark"
    if (health==1) { gR=0.541f; gG=0.604f; gB=0.184f; gdR=gR*0.8f; gdG=gG*0.8f; gdB=gB*0.8f; }       // hama -> kekuningan
    else if (health==2) { gR=0.353f; gG=0.420f; gB=0.227f; gdR=gR*0.8f; gdG=gG*0.8f; gdB=gB*0.8f; }  // ganoderma -> layu
    else if (health==3) { gR=0.42f; gG=0.35f; gB=0.24f; gdR=gR*0.85f; gdG=gG*0.85f; gdB=gB*0.85f; }  // mati -> coklat

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
    seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5; // maju state, ambil angka acak lain dr seed yg sama
    float treeYaw = (seed & 0xFFFFFF) / float(0xFFFFFF) * 6.28318f;
    seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5;
    float sizeJitter = 0.92f + (seed & 0xFFFFFF) / float(0xFFFFFF) * 0.16f; // 0.92..1.08

    // --- mesh STL asli (1688 segitiga, dibaked di palm_icon_mesh.hpp), 4
    //     kelompok warna (trunk/crown_light/crown_dark/fruit) — masing2 SATU
    //     draw call. Skala S_crown & mirrorSign (sumbu X) diterapkan lewat
    //     parameter scale drawTris (bukan dihitung ulang per-vertex spt versi
    //     prosedural, jadi lebih ringan). ---
    float Sc = S_crown * sizeJitter;
    drawTris(kPalmIconTrunk, kPalmIconTrunk_COUNT, trunkR,trunkG,trunkB, 1.0f, x,0,z, Sc*mirrorSign,Sc,Sc, treeYaw);
    drawTris(kPalmIconCrownLight, kPalmIconCrownLight_COUNT, gR,gG,gB, 1.0f, x,0,z, Sc*mirrorSign,Sc,Sc, treeYaw);
    drawTris(kPalmIconCrownDark,  kPalmIconCrownDark_COUNT,  gdR,gdG,gdB, 1.0f, x,0,z, Sc*mirrorSign,Sc,Sc, treeYaw);

    // --- buah: sekarang tampil SEJAK status "Growing" (bukan cuma Ripe/Overripe)
    //     dgn progres warna hijau->oranye->merah -- review eksternal poin #4:
    //     "TBS perlu dibuat jauh lebih terlihat... buat visual state yg sangat
    //     jelas: belum matang/mulai matang/matang/lewat matang". Ini jg
    //     memberi pemain sinyal dini "tandan sedang terbentuk", bukan cuma
    //     muncul tiba-tiba pas matang. ---
    if (ffb>=1){
        float fruitR, fruitG, fruitB;
        if (ffb==1){ fruitR=0.290f; fruitG=0.541f; fruitB=0.220f; }      // Growing: hijau (belum matang)
        else if (ffb==2){ fruitR=0.941f; fruitG=0.490f; fruitB=0.180f; } // Ripe: oranye cerah
        else { fruitR=0.612f; fruitG=0.161f; fruitB=0.114f; }            // Overripe: merah tua
        drawTris(kPalmIconFruit, kPalmIconFruit_COUNT, fruitR,fruitG,fruitB, 1.0f, x,0,z, Sc*mirrorSign,Sc,Sc, treeYaw);
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
void drawWorker(float x, float z, int poseCode, bool usingEgrek){
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
        default: break; // Idle: berdiri/jalan biasa
    }
    const float hipY = 0.55f - hipDrop;
    std::vector<float> skin, hair, shirt, overall, boot, tool, basket;

    // --- animasi jalan: Idle ("jalan biasa" antar pohon) & Carry (jalan bawa
    //     TBS ke TPH) SAMA-SAMA representasi pekerja sedang BERJALAN dlm
    //     gameplay -- sebelumnya kaki statis total, makanya terlihat spt
    //     "robot kaku" (dilaporkan pengguna) bukan orang berjalan. Kneel/Tool/
    //     Reach TETAP diam (sedang bekerja di tempat, bukan berjalan).
    bool walking = (poseCode == 0 || poseCode == 4);
    float legSwing = 0.0f, armSwingL = 0.0f, armSwingR = 0.0f;
    if (walking){
        float phase = std::fmod(g_animT * 2.6f, 6.28318f); // kecepatan langkah
        float swing = std::sin(phase) * 0.45f;
        legSwing = swing;
        if (poseCode == 0){ // Idle: lengan mengayun BERLAWANAN arah kaki (gaya jalan alami manusia)
            armSwingL = -swing * 0.6f;
            armSwingR =  swing * 0.6f;
        }
        // Carry: lengan TETAP memegang keranjang, cuma kaki yg mengayun
    }

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
    const float shirtR=0.706f, shirtG=0.216f, shirtB=0.176f;   // merah (kemeja)
    const float overallR=0.180f, overallG=0.290f, overallB=0.478f; // biru (overalls/bib)
    const float bootR=0.420f, bootG=0.290f, bootB=0.169f;

    // --- torso: overalls bawah + kemeja atas, ikut condong torsoLean di pivot hip ---
    std::vector<float> torsoLowerLocal; buildBox(torsoLowerLocal, 0.30f, 0.16f, 0.20f);
    appendRotXAt(overall, torsoLowerLocal, torsoLean, {0, hipY, 0});

    std::vector<float> torsoUpperLocal; buildBox(torsoUpperLocal, 0.28f, 0.15f, 0.25f);
    for (size_t i=1;i<torsoUpperLocal.size();i+=3) torsoUpperLocal[i]+=0.20f; // nempel di atas torsoLower
    appendRotXAt(shirt, torsoUpperLocal, torsoLean, {0, hipY, 0});

    // --- kepala & rambut ---
    std::vector<float> headLocal; buildBox(headLocal, 0.19f,0.18f,0.20f);
    for (size_t i=1;i<headLocal.size();i+=3) headLocal[i]+=0.45f; // di atas torsoUpper (0.20+0.25)
    appendRotXAt(skin, headLocal, torsoLean, {0, hipY, 0});
    std::vector<float> hairLocal; buildBox(hairLocal, 0.20f,0.19f,0.08f);
    for (size_t i=0;i<hairLocal.size();i+=3){ hairLocal[i+1]+=0.57f; hairLocal[i+2]-=0.02f; }
    appendRotXAt(hair, hairLocal, torsoLean, {0, hipY, 0});

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
        appendRotXAt(shirt, armBent, torsoLean, {0, hipY, 0});
        // posisi tangan (ujung lengan) -- dipakai utk taruh alat/keranjang
        V3 handLocal = v3rotX({0,-0.30f,0}, armAngle+armSwing);
        V3 handRelHip{handLocal.x+shoulderRelHip.x, handLocal.y+shoulderRelHip.y, handLocal.z+shoulderRelHip.z};
        V3 handLean = v3rotX(handRelHip, torsoLean);
        handPos[hi++] = {handLean.x, handLean.y+hipY, handLean.z};
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

    if (!skin.empty())    drawTris(skin.data(),    (int)(skin.size()/3),    skinR,skinG,skinB, 1.0f, x,0,z, WORKER_SCALE,WORKER_SCALE,WORKER_SCALE);
    if (!hair.empty())    drawTris(hair.data(),    (int)(hair.size()/3),    hairR,hairG,hairB, 1.0f, x,0,z, WORKER_SCALE,WORKER_SCALE,WORKER_SCALE);
    if (!shirt.empty())   drawTris(shirt.data(),   (int)(shirt.size()/3),   shirtR,shirtG,shirtB, 1.0f, x,0,z, WORKER_SCALE,WORKER_SCALE,WORKER_SCALE);
    if (!overall.empty()) drawTris(overall.data(), (int)(overall.size()/3), overallR,overallG,overallB, 1.0f, x,0,z, WORKER_SCALE,WORKER_SCALE,WORKER_SCALE);
    if (!boot.empty())    drawTris(boot.data(),    (int)(boot.size()/3),    bootR,bootG,bootB, 1.0f, x,0,z, WORKER_SCALE,WORKER_SCALE,WORKER_SCALE);
    if (!tool.empty())    drawTris(tool.data(),    (int)(tool.size()/3),    0.45f,0.42f,0.38f, 1.0f, x,0,z, WORKER_SCALE,WORKER_SCALE,WORKER_SCALE);
    if (!basket.empty()){
        // kotak keranjang (coklat) & buah (oranye) beda warna -> pisah 2 draw call
        // sederhana: seluruhnya kita gambar sbg satu grup coklat, cukup utk gaya low-poly.
        drawTris(basket.data(), (int)(basket.size()/3), 0.788f,0.541f,0.204f, 1.0f, x,0,z, WORKER_SCALE,WORKER_SCALE,WORKER_SCALE);
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
    std::vector<float> body, cab, wheels, trailer, fruit;

    auto rotPlace = [&](std::vector<float>& out, const std::vector<float>& local, V3 off){
        for (size_t i=0;i<local.size();i+=3){
            V3 p = v3rotY({local[i],local[i+1],local[i+2]}, facingRad);
            out.push_back(p.x+off.x); out.push_back(p.y+off.y); out.push_back(p.z+off.z);
        }
    };
    auto wheelDisc = [&](std::vector<float>& out, V3 centerLocal){
        const int SIDES=8; float r=0.16f;
        std::vector<float> local;
        for (int s=0;s<SIDES;s++){
            float a0=(float)s/SIDES*6.28318f, a1=(float)(s+1)/SIDES*6.28318f;
            V3 c{centerLocal.x, centerLocal.y, centerLocal.z};
            V3 p0{c.x, c.y+std::sin(a0)*r, c.z+std::cos(a0)*r};
            V3 p1{c.x, c.y+std::sin(a1)*r, c.z+std::cos(a1)*r};
            pushTri(local, c, p0, p1);
        }
        rotPlace(out, local, {0,0,0});
    };

    // --- traktor (bodi hijau tua + kabin) ---
    std::vector<float> bodyBox; buildBox(bodyBox, 0.55f, 0.34f, 0.42f);
    { std::vector<float> tmp; appendAt(tmp, bodyBox, {0,0.20f,-0.10f}); rotPlace(body, tmp, {0,0,0}); }
    std::vector<float> cabBox; buildBox(cabBox, 0.38f, 0.30f, 0.40f);
    { std::vector<float> tmp; appendAt(tmp, cabBox, {0,0.42f,0.05f}); rotPlace(cab, tmp, {0,0,0}); }

    // roda traktor (belakang besar, depan lebih kecil)
    wheelDisc(wheels, {-0.30f,0.20f,-0.20f}); wheelDisc(wheels, {0.30f,0.20f,-0.20f});
    wheelDisc(wheels, {-0.30f,0.14f, 0.16f}); wheelDisc(wheels, {0.30f,0.14f, 0.16f});

    // --- trailer (hijau muda) + roda + muatan TBS oranye ---
    std::vector<float> trailerBox; buildBox(trailerBox, 0.62f, 0.40f, 0.30f);
    { std::vector<float> tmp; appendAt(tmp, trailerBox, {0,0.16f,0.55f}); rotPlace(trailer, tmp, {0,0,0}); }
    wheelDisc(wheels, {-0.34f,0.16f,0.60f}); wheelDisc(wheels, {0.34f,0.16f,0.60f});

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
    drawTris(wheels.data(), (int)(wheels.size()/3), 0.098f,0.098f,0.098f, 1.0f, x,0,z, TRUCK_SCALE,TRUCK_SCALE,TRUCK_SCALE); // roda hitam
    drawTris(trailer.data(), (int)(trailer.size()/3), 0.290f,0.494f,0.361f, 1.0f, x,0,z, TRUCK_SCALE,TRUCK_SCALE,TRUCK_SCALE); // trailer hijau muda
    drawTris(fruit.data(), (int)(fruit.size()/3), 0.827f,0.435f,0.157f, 1.0f, x,0,z, TRUCK_SCALE,TRUCK_SCALE,TRUCK_SCALE);    // TBS oranye
}

// Tanda kecil melayang di atas pohon yg sudah dikerjakan aksi massal HARI
// INI — segitiga datar warna beda per jenis aksi, gampang dibedakan sekilas.
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
    appendAt(shirt, torsoUpperLocal, {0,hipY+0.20f,0});
    for (float side : {-1.0f, 1.0f}){
        std::vector<float> armBox; buildBox(armBox, 0.075f,0.075f,-0.30f);
        appendAt(shirt, armBox, {side*0.17f, hipY+0.42f, 0});
    }
    std::vector<float> headLocal; buildBox(headLocal, 0.19f,0.18f,0.20f);
    appendAt(skin, headLocal, {0,hipY+0.45f,0});
    std::vector<float> hairLocal; buildBox(hairLocal, 0.20f,0.19f,0.08f);
    { std::vector<float> tmp; appendAt(tmp, hairLocal, {0,hipY+0.57f,-0.02f}); hair.insert(hair.end(),tmp.begin(),tmp.end()); }
    if (hasHat){
        std::vector<float> hatBox; buildBox(hatBox, 0.22f,0.21f,0.07f);
        appendAt(hat, hatBox, {0,hipY+0.63f,0});
    }

    drawTris(boot.data(), (int)(boot.size()/3), bootR,bootG,bootB, 1.0f, x,0,z, STAFF_SCALE,STAFF_SCALE,STAFF_SCALE);
    drawTris(trouser.data(), (int)(trouser.size()/3), trouserR,trouserG,trouserB, 1.0f, x,0,z, STAFF_SCALE,STAFF_SCALE,STAFF_SCALE);
    drawTris(shirt.data(), (int)(shirt.size()/3), shirtR,shirtG,shirtB, 1.0f, x,0,z, STAFF_SCALE,STAFF_SCALE,STAFF_SCALE);
    drawTris(skin.data(), (int)(skin.size()/3), skinR,skinG,skinB, 1.0f, x,0,z, STAFF_SCALE,STAFF_SCALE,STAFF_SCALE);
    drawTris(hair.data(), (int)(hair.size()/3), hairR,hairG,hairB, 1.0f, x,0,z, STAFF_SCALE,STAFF_SCALE,STAFF_SCALE);
    if (hasHat) drawTris(hat.data(), (int)(hat.size()/3), hatR,hatG,hatB, 1.0f, x,0,z, STAFF_SCALE,STAFF_SCALE,STAFF_SCALE);
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
    float ry=1.6f; V3 rTip{0,0,0};
    { std::vector<float> tmp;
      V3 c0{2.6f-0.8f,ry,-0.8f-0.8f}, c1{2.6f+0.8f,ry,-0.8f-0.8f}, c2{2.6f+0.8f,ry,-0.8f+0.8f}, c3{2.6f-0.8f,ry,-0.8f+0.8f};
      V3 tip{2.6f,ry+0.5f,-0.8f};
      pushTri(tmp,c0,c1,tip); pushTri(tmp,c1,c2,tip); pushTri(tmp,c2,c3,tip); pushTri(tmp,c3,c0,tip);
      rotPlace(roof, tmp, {0,0,0}); (void)rTip;
    }

    drawTris(posts.data(), (int)(posts.size()/3), 0.75f,0.75f,0.73f, 1.0f, x,0,z, 1,1,1);       // tiang abu-abu
    drawTris(barrierRed.data(), (int)(barrierRed.size()/3), 0.78f,0.13f,0.11f, 1.0f, x,0,z, 1,1,1);   // palang merah
    drawTris(barrierWhite.data(), (int)(barrierWhite.size()/3), 0.95f,0.95f,0.93f, 1.0f, x,0,z, 1,1,1); // palang putih
    drawTris(booth.data(), (int)(booth.size()/3), 0.90f,0.86f,0.75f, 1.0f, x,0,z, 1,1,1);       // pos jaga krem
    drawTris(roof.data(), (int)(roof.size()/3), 0.55f,0.27f,0.18f, 1.0f, x,0,z, 1,1,1);         // atap coklat
}

void drawTreeInspectorFrame(float ageYears, float frond, int health, int ffb, bool hasTbsReady, float yawSpin, float panY){
    // Simpan kamera game utama SEBELUM diubah -- dipulihkan di akhir supaya
    // drawFrame() normal berikutnya tidak "meloncat" krn state kamera global
    // (g_panX/g_panZ/g_panY/g_dist/g_yaw) ini dipakai bersama oleh scene utama.
    float savedPanX=g_panX, savedPanY=g_panY, savedPanZ=g_panZ, savedDist=g_dist, savedYaw=g_yaw;

    // Kamera close-up: pohon SELALU digambar di origin (0,0) dlm mode ini
    // (bukan posisi dunia sungguhannya) -- lebih simpel & konsisten dipotret
    // dari sudut manapun. Jarak kamera menyesuaikan tinggi pohon spy pohon
    // muda & tua sama-sama pas dlm bingkai. panY = geser vertikal titik fokus
    // kamera (0 = pusat default, dinaikkan pemanggil via gesture scroll spy
    // pemain bisa lihat dari akar/pangkal batang [y=0] sampai ke puncak
    // mahkota -- sebelumnya kamera terkunci total tanpa cara menggeser).
    float trunkH = treeTrunkHeight(ageYears);
    float dist = std::max(10.0f, trunkH*2.3f);
    g_panX = 0; g_panY = panY; g_panZ = 0; g_dist = dist; g_yaw = yawSpin;
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

    drawPalm(0.0f, 0.0f, ageYears, frond, health, ffb, false);
    if (hasTbsReady) drawTbsPile(0.0f, 0.0f);

    // Pulihkan kamera game utama.
    g_panX=savedPanX; g_panY=savedPanY; g_panZ=savedPanZ; g_dist=savedDist; g_yaw=savedYaw;
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
