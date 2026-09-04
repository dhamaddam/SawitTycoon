import UIKit
// 'internal' eksplisit di sini (bukan cuma "import GLKit" biasa) supaya konsisten
// dgn modul GLKit yang diimpor sbg 'internal' di tempat lain pada proyek yang sama
// -- Swift 6 menolak campuran implicit/explicit access-level utk modul yang sama
// dlm satu target ("Ambiguous implicit access level for import of 'GLKit'").
internal import GLKit

/// Satu-satunya view controller game. UI HUD/panel aksi pakai UIKit biasa
/// (bukan SwiftUI dulu, sengaja eksplisit supaya gampang ditelusuri kalau ada
/// error build). Simulasi & render 3D sepenuhnya di C++ lewat EngineBridge.
///
/// GLKView dibuat programatik di `loadView()` — TIDAK perlu setup Storyboard/XIB.
final class GameViewController: GLKViewController {

    private var engine: EngineBridge!
    private var soundManager: SoundManager!
    // File save di direktori Documents app (privat, tak perlu izin apa pun).
    private let saveFileURL: URL = {
        let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return dir.appendingPathComponent("savegame.json")
    }()
    private var eaglContext: EAGLContext!
    private var lastUpdateTime: CFTimeInterval = 0

    private var panX: Float = 0
    // Avatar pemain (Gameplay Mode, third-person) -- hasil review eksternal
    // poin #5. Diubah dari joystick UI, dibaca tiap frame di update() utk
    // gerakkan avatar. TAK perlu ternormalisasi (engine.movePlayerAvatar
    // menormalisasi sendiri).
    var joystickDirX: Float = 0
    var joystickDirZ: Float = 0
    // Tombol interaksi (muncul saat avatar dekat pohon) + treeId hasil
    // polling nearestTreeToPlayer -- fondasi interaksi Gameplay Mode.
    private var interactBtn: UIButton!
    private var nearbyTreeId: Int = -1
    private var panZ: Float = 0
    // Target transisi kamera SINEMATIK (non-nil = sedang animasi) -- dipakai
    // saat lompat antar-Block. Sebelumnya instan/snap langsung ke posisi baru;
    // spesifikasi desain eksplisit minta "kamera bergerak scr cinematic,
    // durasi 0.6-1.0 detik" -- bukan teleport sekejap.
    private var panXTarget: Float?
    private var panZTarget: Float?
    private var camDist: Float = 44
    // Estate View -- layer aktif (0=Kesehatan,1=Nutrisi,2=Kematangan) & posisi
    // kamera tersimpan (utk kembali saat dimatikan manual, bukan drill-down).
    private var estateViewActive: Bool = false
    private var estateViewLayer: Int = 0
    private var savedCamX: Float = 0
    private var savedCamZ: Float = 0
    private var savedCamDist: Float = 44
    // Rotasi kamera sekitar sumbu vertikal (radian) — fitur lihat 360°.
    // Default 45° = sudut isometrik klasik, sama spt sebelum fitur ini ada.
    private var camYaw: Float = 0.7854

    private var selectedTreeId: Int = -1
    private var trees: [SawitTreeView] = []
    // Kumpulan tombol kolom kiri (dev/lahan/SDM/PKS/kirim/beacon/suara/estate/
    // aksi-massal) -- utk toggle visibility via tombol "☰ Menu" tunggal.
    // Mengatasi keluhan review eksternal: "UI kiri: terlalu banyak tombol
    // SELALU terlihat -> dunia game tertutup oleh menu" (prioritas Tinggi).
    private var leftColumnBtns: [UIButton] = []
    private var menuToggleBtn: UIButton!
    // Snapshot properti pohon terakhir dirender di panel -- SawitTreeView (kelas
    // Obj-C) pakai identity equality (==) bawaan NSObject, jadi objek baru dari
    // engine.trees() tiap poll TIDAK PERNAH == objek lama meski isinya identik.
    // Makanya dibandingkan manual lewat tuple properti (Int/Float/Bool semua
    // Equatable). Sebelumnya panel dibangun ulang TANPA SYARAT tiap 250ms --
    // itu salah satu penyebab tombol aksi (termasuk "Detail 3D") kadang terasa
    // tak responsif kalau ketukan bertepatan dgn momen rebuild.
    private var lastRenderedTreeSnapshot: (id: Int, health: Int, ffb: Int, frond: Float, hasTbsReady: Bool)?
    private var hudTimer: Timer?

    // Overlay UIKit
    private let hudMoney = UILabel()
    private let hudDay = UILabel()
    private let hudTph = UILabel()
    private let hudBlock = UILabel()
    private let panelTitle = UILabel()
    private let actionStack = UIStackView()
    // Kontainer luar (panelTitle + baris tombol aksi) -- disimpan spy bar aksi
    // massal bisa disambung via AutoLayout ke topAnchor-nya (lihat setupBulkActionUi),
    // otomatis mengikuti tinggi AKTUAL panel (bukan konstanta tetap yg dulu
    // menyebabkan tertimpa saat panelTitle jadi multi-baris).
    private var bottomPanel: UIStackView!
    // Floating overlay radial menu -- MENGGANTIKAN bottomPanel/actionStack utk
    // aksi per-pohon (celah diperbaiki: dulu menempel bar bawah spt panel
    // datar lama, sekarang benar2 mengambang di titik pohon diketuk).
    private var radialMenuOverlay: UIView!
    // Countdown/progress bar panen TBS -- fitur baru diminta pengguna:
    // "countdown panen sawit / progress bar pada setiap pohon... tampiilkan
    // progressnya hanya ketika pohon tersebut di klik". Identik Android
    // (progressLabel di MainActivity.kt) -- statusLabel yg SUDAH ADA
    // (menampilkan "Pokok #ID (Egrek/Dodos)\nSehat|Tumbuh dst") dijadikan
    // property class supaya bisa diperbarui SECARA TERPISAH (lihat
    // updateFfbCountdown()) tanpa membangun ulang seluruh radial menu tiap
    // polling 250ms.
    private var statusLabel: UILabel?
    private var radialLeadingConstraint: NSLayoutConstraint!
    private var radialTopConstraint: NSLayoutConstraint!
    private var radialScreenPoint: CGPoint = .zero
    // Nama lengkap tiap tombol radial (utk label sementara saat hover/tekan-
    // lama) -- key WeakMap-style pakai NSMapTable spy tak nahan referensi
    // tombol yg sudah dihapus (removeFromSuperview) tetap di memori.
    private var radialBtnLabels = NSMapTable<UIButton, NSString>(keyOptions: .weakMemory, valueOptions: .strongMemory)
    private var radialHoverLabel: UILabel?
    // Proteksi spam tombol -- laporan pengguna: "ada indikasi terjadi spam
    // button". Sama pola dgn Android (setOnClickListenerDebounced), tapi
    // disesuaikan struktur iOS: selector radial TERPISAH per aksi (bukan
    // closure inline), jadi dibungkus lewat helper debounceGuard() dipanggil
    // di AWAL tiap handler. Maks 1 eksekusi per 400ms per-tombol (bukan
    // global keseluruhan app, spy tombol BEDA tetap responsif independen).
    private var lastTapTimes = NSMapTable<UIButton, NSDate>(keyOptions: .weakMemory, valueOptions: .strongMemory)
    private func debounceGuard(_ sender: UIButton) -> Bool {
        let now = Date()
        if let last = lastTapTimes.object(forKey: sender) as Date?, now.timeIntervalSince(last) < 0.4 {
            return false
        }
        lastTapTimes.setObject(now as NSDate, forKey: sender)
        return true
    }

    // --- Log Aktivitas ---
    private let logButton = UIButton(type: .system)
    private let logOverlay = UIView()
    private let logScrollView = UIScrollView()
    private let logStackView = UIStackView() // ganti dr UITextView tunggal -- tiap entri kini UILabel sendiri, bisa diketuk

    // --- Detail 3D Pohon ---
    private var lastRenderedTree: SawitTreeView?
    // Inspector Pohon: close-up 1 pohon berputar otomatis, MENGGANTIKAN
    // pendekatan WKWebView/HTML sepenuhnya. Non-nil -> mode inspector aktif.
    private var inspectorTree: SawitTreeView?
    private var inspectorYaw: Float = 0
    // Rotasi manual (kiri/kanan) -- diubah tombol ◀/▶. Begitu pemain sentuh
    // kontrol ini, auto-spin BERHENTI (biar tak "berebut" dgn niat pemain
    // mengarahkan sendiri) -- sebelumnya CUMA auto-spin, tak ada cara melihat
    // dari sudut TERTENTU secara sengaja (dilaporkan pengguna: "hanya mampu
    // naik-turun, tidak bisa X/Y/Z sesuai sudut pandang harapan").
    private var inspectorAutoSpin = true
    // Geser vertikal kamera inspector -- diubah gesture pan saat inspector
    // aktif. 0 = default; dinaikkan/diturunkan pemain spy bisa lihat dari
    // akar (y=0) sampai puncak mahkota. Direset ke 0 tiap buka inspector baru.
    private var inspectorPanY: Float = 0
    private var inspectorCloseBtn: UIButton?
    // Block mana yg sedang ditampilkan di label HUD (0=Block A01 default) --
    // berubah saat pemain lompat via tapBlockSelector().
    private var currentBlockIndex: Int = 0
    private var inspectorNavUp: UIButton?
    private var inspectorNavDown: UIButton?
    private var inspectorNavLeft: UIButton?
    private var inspectorNavRight: UIButton?
    private var navHoldTimer: Timer?
    private var navHoldDir: Float = 0
    private var logVisible = false

    override func loadView() {
        // GLKView dibuat langsung di kode (bukan lewat Storyboard) supaya proyek ini
        // bisa dibangun tanpa konfigurasi XIB/Storyboard manual yang rawan typo.
        self.view = GLKView(frame: UIScreen.main.bounds)
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        engine = EngineBridge()
        loadAndApplySettings() // muat pengaturan grafik & sensitivitas tersimpan (UserDefaults) -- identik Android
        soundManager = SoundManager()
        soundManager.startBgm()

        // Muat save lama kalau ada -- BUG BESAR diperbaiki: saveJson/loadJson
        // SUDAH ADA lama di EngineBridge, tapi tak pernah dipanggil dari mana
        // pun (persis pola bug lain sesi ini) -- progress pemain SELALU
        // hilang begitu app ditutup, meski format JSON trees_/blocks_ sudah
        // diperbaiki di sisi engine, TANPA disambungkan ke siklus hidup app
        // ini tetap tak berdampak nyata sama sekali.
        loadGameFromFile()
        // Simpan otomatis begitu app masuk background/nonaktif -- lebih andal
        // drpd cuma viewWillDisappear (iOS bisa suspend app kapan saja).
        NotificationCenter.default.addObserver(self, selector: #selector(saveGameToFile),
            name: UIApplication.willResignActiveNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(saveGameToFile),
            name: UIApplication.didEnterBackgroundNotification, object: nil)
        // BGM ikut pause/resume mengikuti siklus hidup app (jangan terus
        // berbunyi di background, & lanjut lagi begitu app aktif kembali).
        NotificationCenter.default.addObserver(self, selector: #selector(pauseBgmForBackground),
            name: UIApplication.willResignActiveNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(resumeBgmForForeground),
            name: UIApplication.didBecomeActiveNotification, object: nil)

        guard let eaglCtx = EAGLContext(api: .openGLES2) else {
            fatalError("OpenGL ES 2.0 tidak didukung di perangkat ini.")
        }
        eaglContext = eaglCtx

        guard let glkView = self.view as? GLKView else {
            fatalError("Tidak seharusnya terjadi — loadView() di atas selalu membuat GLKView.")
        }
        glkView.context = eaglContext
        glkView.delegate = self
        glkView.drawableColorFormat = .RGBA8888
        glkView.drawableDepthFormat = .format16
        EAGLContext.setCurrent(eaglContext)
        engine.glInit()

        setupGestures()
        setupOverlayUI()
        setupActivityLogUi()
        startPollingLoop()
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        let scale = Float(view.contentScaleFactor)
        engine.glResizeWidth(Int32(Float(view.bounds.width) * scale),
                              height: Int32(Float(view.bounds.height) * scale))
    }

    // MARK: - Render loop
    // PENTING: `update()` di sini SENGAJA TIDAK memakai `override` — menurut
    // dokumentasi resmi Apple, GLKViewController tidak mendeklarasikan `update`
    // sebagai method class biasa; ia mengecek keberadaannya secara runtime
    // (mirip respondsToSelector: di Objective-C), jadi harus method biasa TANPA
    // `override`, kalau tidak Swift menolak dengan "does not override any
    // method from its superclass".
    //
    // Rujukan resmi Apple (GLKViewController "Subclassing Notes"):
    // "As an alternative to implementing a glkViewControllerUpdate(_:) method in
    //  a delegate, your subclass can provide an update method instead. The
    //  method must have the following signature: - (void)update;"
    // PENTING: @objc eksplisit di sini WAJIB. GLKViewController mengecek
    // keberadaan method ini lewat respondsToSelector: (mekanisme Objective-C
    // runtime, bukan vtable Swift biasa) — tanpa @objc, method Swift biasa
    // tidak otomatis "terlihat" oleh pengecekan itu, dan update() (termasuk
    // engine.tick() serta sinkronisasi kamera pan/zoom di dalamnya) TIDAK
    // PERNAH terpanggil sama sekali meski tidak ada error compile apa pun.
    // BUG UX diperbaiki (fitur baru diminta pengguna: "Kunci pan secara
    // total") -- identik Android, lihat catatan lengkap di GameRenderer.kt.
    // SATU SUMBER KEBENARAN eksplisit -- TAK PERNAH berubah krn gesture pan
    // manual, HANYA lewat animateCameraTo() (navigasi terprogram).
    private var activeBlockOriginX: Float = 0

    private func animateCameraTo(_ targetX: Float, _ targetZ: Float) {
        panXTarget = targetX
        panZTarget = targetZ
        activeBlockOriginX = (round(targetX / 120)) * 120
    }
    // Dipanggil saat pemain mulai geser kamera manual (drag) -- supaya animasi
    // otomatis (dr lompat-block) tak "berebut" arah dgn gesture pemain kalau
    // kebetulan disentuh di tengah transisi.
    private func cancelCameraTransition() {
        panXTarget = nil
        panZTarget = nil
    }
    // Interpolasi eksponensial framerate-independent (bukan lerp linear
    // per-frame yg kecepatannya berubah kalau FPS berubah), berhenti otomatis
    // & snap ke nilai persis begitu cukup dekat. speed=6.0 -> konvergen ~99%
    // dlm ~0.7 detik, pas di rentang spesifikasi desain (0.6-1.0 detik).
    // speed=8.0 & threshold=0.3 (bukan 6.0/0.05) -- diverifikasi numerik: versi
    // lebih lambat butuh 1.23 detik utk jarak besar (120 unit antar-block,
    // melebihi target spesifikasi 0.6-1.0 detik) krn ambang absolut terlalu
    // ketat utk jarak jauh. Kombinasi ini konvergen TEPAT 0.70 detik.
    private func stepCameraTransition(_ dt: Double) {
        guard let tx = panXTarget, let tz = panZTarget else { return }
        let factor = Float(min(max(dt * 8.0, 0.0), 1.0))
        panX += (tx - panX) * factor
        panZ += (tz - panZ) * factor
        if abs(tx - panX) < 0.3 && abs(tz - panZ) < 0.3 {
            panX = tx; panZ = tz
            panXTarget = nil; panZTarget = nil
        }
    }

    @objc func update() {
        let now = CACurrentMediaTime()
        var dt = lastUpdateTime == 0 ? 0 : (now - lastUpdateTime)
        lastUpdateTime = now
        if dt > 0.1 { dt = 0.1 } // clamp, hindari lonjakan simulasi kalau app sempat freeze
        engine.tick(dt) // simulasi tetap jalan; cuma TAMPILAN yg beda di mode inspector
        if inspectorTree == nil {
            // Avatar pemain (Gameplay Mode) -- gerakkan SEBELUM camera step,
            // dt AKURAT (bukan fixed) krn gerakan avatar perlu konsisten
            // terlepas dari framerate. HANYA saat Gameplay Mode aktif.
            if engine.getGameplayModeActive() {
                engine.movePlayerAvatarDirX(joystickDirX, dirZ: joystickDirZ, dt: Float(dt))
            }
            stepCameraTransition(dt) // konvergen halus menuju target lompat-block (kalau sedang animasi)
            engine.glSetCameraPanX(panX, panZ: panZ, dist: camDist, yaw: camYaw)
        } else {
            if inspectorAutoSpin { inspectorYaw += 0.008 } // putar otomatis pelan (sampai pemain ambil alih manual)
        }
    }

    // CATATAN: berbeda dgn update() di atas, method ini justru BUTUH `override`
    // di sebagian versi SDK Xcode (GLKViewController sudah punya implementasi
    // default utk method protokol GLKViewDelegate ini). Kalau muncul error
    // "Overriding declaration requires an 'override' keyword", itu tandanya
    // SDK-mu termasuk versi itu -- sudah ditambahkan di sini.
    override func glkView(_ view: GLKView, drawIn rect: CGRect) {
        if let t = inspectorTree {
            engine.glDrawTreeInspectorAge(t.ageYears, frond: t.frond, health: t.health, ffb: t.ffb,
                                           hasTbsReady: t.hasTbsReady, yawSpin: Float(inspectorYaw), panY: inspectorPanY, nutrition: t.nutrition)
        } else {
            engine.glDrawFrameSelectedTreeId(selectedTreeId)
        }
    }

    // MARK: - Gestures: tap pilih pohon, drag pan, pinch zoom, putar 2 jari = rotasi 360°

    private func setupGestures() {
        view.addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(onTap(_:))))
        view.addGestureRecognizer(UIPanGestureRecognizer(target: self, action: #selector(onPan(_:))))

        let pinch = UIPinchGestureRecognizer(target: self, action: #selector(onPinch(_:)))
        let rotate = UIRotationGestureRecognizer(target: self, action: #selector(onRotate(_:)))
        pinch.delegate = self
        rotate.delegate = self
        view.addGestureRecognizer(pinch)
        view.addGestureRecognizer(rotate)
    }

    @objc private func onTap(_ g: UITapGestureRecognizer) {
        if inspectorTree != nil { return } // inspector aktif -> abaikan tap kebun
        let p = g.location(in: view)
        let scale = Float(view.contentScaleFactor)
        let screenX = Float(p.x) * scale, screenY = Float(p.y) * scale

        // Estate View aktif -- hit-test terhadap UBIN BLOCK (bukan pohon,
        // yg tak digambar di mode ini). Kena -> matikan Estate View &
        // animasikan kamera MASUK ke block itu (drill-down).
        if engine.getEstateViewActive() {
            let blocks = engine.blockSummaries()
            var bestBlock: SawitBlockSummary?
            var bestDist: Float = .greatestFiniteMagnitude
            var ox: Float = 0, oy: Float = 0
            for b in blocks {
                engine.world(toScreenX: b.originX, z: b.originZ, outX: &ox, outY: &oy)
                let d = hypotf(ox-screenX, oy-screenY)
                if d < bestDist { bestDist = d; bestBlock = b }
            }
            if let bb = bestBlock {
                estateViewActive = false
                camDist = savedCamDist
                animateCameraTo(bb.originX, bb.originZ)
                engine.setEstateViewModeActive(false, layer: estateViewLayer)
                showToast("Masuk ke Block \(bb.name)")
            }
            return
        }
        // Hit-test langsung di RUANG LAYAR — pendekatan lama (screenToWorldX pada
        // asumsi y=0) gagal kalau yg diketuk adalah pelepah/bagian atas pohon,
        // karena posisinya di layar lebih tinggi dari dasar pohon dan salah
        // dipetakan ke lokasi tanah yg jauh dari pohon aslinya.
        var best: SawitTreeView?
        var bestDist: Float = .greatestFiniteMagnitude
        for t in trees {
            let d = engine.hitTestDistanceScreenX(screenX, screenY: screenY, treeX: t.x, treeZ: t.z, ageYears: t.ageYears)
            if d < bestDist { bestDist = d; best = t }
        }
        if let b = best, bestDist < 80 { // ambang dlm PIKSEL layar, bukan unit dunia lagi
            selectedTreeId = b.treeId
            lastRenderedTreeSnapshot = (id: b.treeId, health: b.health, ffb: b.ffb, frond: b.frond, hasTbsReady: b.hasTbsReady)
            // Posisi ketuk SEBENARNYA (POINT-space UIKit utk constraint, beda
            // dr screenX/screenY yg sudah di-scale ke PIXEL utk hit-test C++)
            // -- dipakai menempatkan radial menu floating persis di titik
            // dipilih (celah diperbaiki: dulu menempel bar bawah layar spt
            // panel datar lama, BUKAN mengambang dekat pohon spt spesifikasi).
            radialScreenPoint = p
            renderActionPanel(for: b)
        } else {
            selectedTreeId = -1
            lastRenderedTreeSnapshot = nil
            radialMenuOverlay.isHidden = true
            panelTitle.text = "Ketuk sebuah pokok"
        }
    }

    @objc private func onPan(_ g: UIPanGestureRecognizer) {
        let t = g.translation(in: view)
        // Kamera "lihat sekeliling" via touch-drag (poin #4 laporan: sudut
        // pandang harusnya bisa berubah dari sentuh layar, bukan cuma
        // joystick, dalam mode berjalan). SEBELUMNYA onPan TAK dikondisikan
        // thd Gameplay Mode sama sekali -- sentuhan saat Gameplay Mode aktif
        // tetap mengubah panX/panZ (variabel Management Mode yg TAK
        // berpengaruh visual apa pun krn kamera third-person diatur
        // updateThirdPersonCamera(), bukan panX/panZ) -- gestur "geser layar"
        // terasa TAK melakukan apa-apa. Identik Android (onScroll,
        // MainActivity.kt), TAPI tanda dibalik: translation(in:) iOS
        // = posisiBaru-posisiLama (POSITIF saat geser kanan), KEBALIKAN
        // distanceX Android (posisiLama-posisiBaru, NEGATIF saat geser
        // kanan) -- jadi TANPA tanda negatif di sini spy arah visual SAMA
        // (geser kanan = putar pandang ke kanan).
        if engine.getGameplayModeActive() && g.numberOfTouches <= 1 {
            // BUG dicegah: onPan didaftarkan di `view` UTAMA (seluruh layar,
            // lihat setupOverlayUI), sementara onJoystickPan (joystick)
            // punya UIPanGestureRecognizer TERPISAH khusus di joyBase.
            // shouldRecognizeSimultaneouslyWith (extension di akhir file)
            // mengizinkan SEMUA pasangan gesture dikenali BERSAMAAN --
            // tanpa pengecekan ini, menggeser joystick akan IKUT memutar
            // kamera scr tak sengaja (kedua gesture aktif bersamaan).
            let touchPoint = g.location(in: view)
            let joyFrame = joyBase != nil ? joyBase.convert(joyBase.bounds, to: view) : .zero
            let interactFrame = (interactBtn != nil && !interactBtn.isHidden) ? interactBtn.convert(interactBtn.bounds, to: view) : .zero
            if !joyFrame.contains(touchPoint) && !interactFrame.contains(touchPoint) {
                let deltaRad = Float(t.x) * (2.0 * Float.pi / 1000.0)
                engine.adjustCameraYawOffset(deltaRad)
                // Mendongak "lihat ke atas" -- fitur baru diminta pengguna:
                // "tidak bisa melihat lebih ke atas pohon sawit, berikan
                // lebih jauh sudut pandang hanya untuk melihat ke atas tidak
                // untuk horizontal". Komponen VERTIKAL (t.y) dari gestur pan
                // yg SAMA -- TERPISAH SEPENUHNYA dari deltaRad (yaw) di atas.
                // PERHATIAN konvensi UIKit: translation(in:).y POSITIF berarti
                // jari bergerak ke BAWAH layar (BERBEDA dari Android
                // GestureDetector.distanceY yg positif saat jari ke ATAS) --
                // dinegasikan (-t.y) supaya jari geser ke ATAS (t.y negatif)
                // menghasilkan delta POSITIF (mendongak), konsisten scr hasil
                // akhir dgn Android meski konvensi arah platform berbeda.
                let deltaLookUp = Float(-t.y) * (7.0 / 1000.0)
                engine.adjustAvatarLookUpOffset(deltaLookUp)
            }
            g.setTranslation(.zero, in: view)
            return
        }
        if inspectorTree != nil {
            if g.numberOfTouches > 1 { return }
            // Scroll KE BAWAH (jari turun di layar, t.y positif di UIKit) -> panY
            // berkurang -> kamera geser turun, memperlihatkan pangkal batang/akar
            // (y=0). Dibatasi rentang wajar (-1..9) spy tak bisa digeser jauh ke
            // luar area pohon yg berguna. Sama persis logikanya dgn Android.
            // Tanda DIBALIK dari versi sebelumnya -- dilaporkan pengguna arahnya
            // kebalikan (drag ke atas malah pohon bergerak ke bawah, & sebaliknya).
            inspectorPanY = min(9, max(-1, inspectorPanY + Float(t.y) * 0.012))
            g.setTranslation(.zero, in: view)
            return
        }
        if g.numberOfTouches > 1 { return } // 2+ jari: biarkan ditangani pinch/rotate, bukan pan
        let scale = Float(view.contentScaleFactor)
        let p = g.location(in: view)
        // Titik AKHIR (posisi jari sekarang) dan titik AWAL (posisi sebelum translasi
        // ini) dlm ruang layar -- panWorldDelta menghitung pergeseran dunia yg benar
        // dari sepasang titik layar, otomatis ikut kemiringan & rotasi kamera saat
        // ini (BUKAN dx/dy layar dikali konstanta spt sebelumnya, yg salah arah
        // begitu kamera miring 45° apalagi setelah bisa diputar bebas).
        let endX = Float(p.x) * scale, endY = Float(p.y) * scale
        let startX = (Float(p.x) - Float(t.x)) * scale, startY = (Float(p.y) - Float(t.y)) * scale
        var dx: Float = 0, dz: Float = 0
        engine.panWorldDeltaStartX(startX, startY: startY, endX: endX, endY: endY, outDx: &dx, outDz: &dz)
        cancelCameraTransition() // pemain ambil alih manual -- jangan berebut arah dgn animasi lompat-block
        // BUG UX diperbaiki (fitur baru diminta pengguna: "Kunci pan secara
        // total... pastikan hanya melihat tampilan satu lahan secara full
        // screen") -- identik Android, lihat catatan lengkap di
        // MainActivity.kt/GameRenderer.kt. panX di-clamp ke
        // activeBlockOriginX TETAP (bukan dihitung ulang dinamis) -- pan
        // akan MENTOK di batas block, tak pernah menyeberang. PENGECUALIAN:
        // Estate View (lihat semua block sekaligus) dilewati sepenuhnya.
        if estateViewActive {
            panX += dx
            panZ += dz
        } else {
            panX = min(activeBlockOriginX + 60, max(activeBlockOriginX - 60, panX + dx))
            panZ = min(38, max(-38, panZ + dz))
        }
        g.setTranslation(.zero, in: view)
    }

    @objc private func onPinch(_ g: UIPinchGestureRecognizer) {
        if inspectorTree != nil { return } // inspector aktif -> abaikan zoom kebun
        // BUG UX diperbaiki (fitur baru diminta pengguna: "view full screen
        // pada tampilan awal dan pastikan hanya melihat tampilan satu lahan
        // secara full screen, untuk melihat lahan lain cukup dengan
        // menggunakan menu sebelah kiri saja"). Identik Android -- max
        // SEBELUMNYA 70 belum mempertimbangkan jarak antar block (120 unit,
        // lihat newOriginX di engine.cpp) -- diturunkan ke 55 (konservatif,
        // aman di semua orientasi yaw krn kamera ortografis tilt + rotasi
        // 360 derajat yg sudah ada bikin arah "lebar layar" bisa berubah
        // relatif sumbu dunia). Batas diperluas saat Estate View aktif
        // (lihat tapEstateView) -- konsistensi: mencegah pinch manual scr
        // tak sengaja mempersempit zoom yg sudah diatur utk lihat semua block.
        let maxDist: Float = estateViewActive ? 280 : 55
        camDist = min(maxDist, max(22, camDist / Float(g.scale)))
        g.scale = 1.0
    }

    @objc private func onRotate(_ g: UIRotationGestureRecognizer) {
        if inspectorTree != nil { return } // inspector aktif -> abaikan rotasi kebun
        camYaw += Float(g.rotation)
        g.rotation = 0
    }

    // MARK: - Overlay UIKit (HUD + panel aksi)

    private func setupOverlayUI() {
        for lbl in [hudMoney, hudDay, hudTph] {
            lbl.textColor = .white
            lbl.font = .boldSystemFont(ofSize: 13)
            lbl.textAlignment = .center
        }
        hudMoney.textAlignment = .left
        hudTph.textAlignment = .right
        // Upgrade kapasitas TPH -- fitur baru diminta pengguna. HUD TPH yg
        // SUDAH ADA dijadikan pemicu dialog upgrade saat disentuh -- UILabel
        // butuh UITapGestureRecognizer eksplisit (beda dari UIButton yg bisa
        // langsung addTarget).
        hudTph.isUserInteractionEnabled = true
        hudTph.addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(tapTphUpgrade)))

        let hudStack = UIStackView(arrangedSubviews: [hudMoney, hudDay, hudTph])
        hudStack.axis = .horizontal
        hudStack.distribution = .fillEqually
        hudStack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(hudStack)
        NSLayoutConstraint.activate([
            hudStack.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 8),
            hudStack.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 12),
            hudStack.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -12),
        ])

        // Label ringkasan Block -- bukti visual PERTAMA dari hierarki Block yg
        // dibangun sesi sebelumnya (murni API/backend, blm ada tampilan sama
        // sekali sampai sekarang). Belum Estate/Block View penuh (itu nanti,
        // butuh redesain kamera+UI besar) -- ini cuma strip info kecil dulu,
        // supaya kerja kemarin tak terasa "menghilang" tanpa bukti.
        hudBlock.textColor = UIColor(white: 0.95, alpha: 1)
        hudBlock.font = .systemFont(ofSize: 11)
        hudBlock.backgroundColor = UIColor(white: 0, alpha: 0.6)
        hudBlock.translatesAutoresizingMaskIntoConstraints = false
        hudBlock.isUserInteractionEnabled = true
        hudBlock.addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(tapBlockSelector)))
        view.addSubview(hudBlock)
        NSLayoutConstraint.activate([
            hudBlock.topAnchor.constraint(equalTo: hudStack.bottomAnchor, constant: 6),
            hudBlock.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 12),
        ])

        // Tombol kecil -- alat uji visual, spy variasi kanopi (vigor & warna
        // nutrisi) yg baru dibangun bisa langsung dicek tanpa nunggu hari
        // game berlalu (hama/ganoderma alami baru muncul stlh berhari-hari).
        let devBtn = UIButton(type: .system)
        devBtn.setTitle("🔧 Uji Visual", for: .normal)
        devBtn.setTitleColor(.white, for: .normal)
        devBtn.titleLabel?.font = .systemFont(ofSize: 11)
        devBtn.backgroundColor = UIColor(white: 0, alpha: 0.5)
        devBtn.layer.cornerRadius = 6
        devBtn.contentEdgeInsets = UIEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
        devBtn.translatesAutoresizingMaskIntoConstraints = false
        devBtn.addTarget(self, action: #selector(tapDevRandomize), for: .touchUpInside)
        view.addSubview(devBtn)
        leftColumnBtns.append(devBtn)
        NSLayoutConstraint.activate([
            // BUG UX diperbaiki (dilaporkan pengguna: overlap serupa versi
            // Android): sebelumnya constant=6, SAMA PERSIS dgn menuToggleBtn
            // (jg constraint ke hudBlock.bottomAnchor+6) -- keduanya tumpang
            // tindih saat menu dibuka (devBtn muncul di posisi menuToggleBtn).
            // constant=52 memberi CELAH utk menuToggleBtn di antara hudBlock
            // & awal kolom kiri (menuToggleBtn ~40dp tinggi + jarak wajar).
            devBtn.topAnchor.constraint(equalTo: hudBlock.bottomAnchor, constant: 52),
            devBtn.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 12),
        ])

        // Tombol Lahan -- BUG LAMA diperbaiki: beliHa/bukaAfdelingBaru sudah
        // ada di EngineBridge SEJAK SEBELUM sesi ini, tapi tak pernah
        // tersambung ke UI mana pun (dilaporkan pengguna: "tidak melihat
        // tombol beli ha/blok"). Sekarang setiap 1 Ha yg dibeli membuat Block
        // BARU dgn 143 pohon sungguhan (lihat engine.cpp beliHa()).
        let landBtn = UIButton(type: .system)
        landBtn.setTitle("🌍 Lahan", for: .normal)
        landBtn.setTitleColor(.white, for: .normal)
        landBtn.titleLabel?.font = .systemFont(ofSize: 11)
        landBtn.backgroundColor = UIColor(white: 0, alpha: 0.5)
        landBtn.layer.cornerRadius = 6
        landBtn.contentEdgeInsets = UIEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
        landBtn.translatesAutoresizingMaskIntoConstraints = false
        landBtn.addTarget(self, action: #selector(tapLand), for: .touchUpInside)
        view.addSubview(landBtn)
        leftColumnBtns.append(landBtn)
        NSLayoutConstraint.activate([
            landBtn.topAnchor.constraint(equalTo: devBtn.bottomAnchor, constant: 6),
            landBtn.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 12),
        ])

        // Tombol SDM -- BUG LAMA diperbaiki: rekrutLevel() sudah lama ada di
        // EngineBridge, tapi tak pernah tersambung ke UI mana pun (dilaporkan
        // pengguna: error "harus rekrut asisten dulu" tp tombol rekrut tak ada).
        let hrBtn = UIButton(type: .system)
        hrBtn.setTitle("👤 SDM", for: .normal)
        hrBtn.setTitleColor(.white, for: .normal)
        hrBtn.titleLabel?.font = .systemFont(ofSize: 11)
        hrBtn.backgroundColor = UIColor(white: 0, alpha: 0.5)
        hrBtn.layer.cornerRadius = 6
        hrBtn.contentEdgeInsets = UIEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
        hrBtn.translatesAutoresizingMaskIntoConstraints = false
        hrBtn.addTarget(self, action: #selector(tapHr), for: .touchUpInside)
        view.addSubview(hrBtn)
        leftColumnBtns.append(hrBtn)
        NSLayoutConstraint.activate([
            hrBtn.topAnchor.constraint(equalTo: landBtn.bottomAnchor, constant: 6),
            hrBtn.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 12),
        ])

        // Tombol PKS -- BUG BESAR ditemukan lewat AUDIT SISTEMATIS (bandingkan
        // semua method EngineBridge yg dideklarasikan vs yg benar2 dipanggil
        // UI): bangunPks/upgradePks/prosesBatchPks SUDAH ADA lengkap &
        // berfungsi, tapi TAK PERNAH punya UI sama sekali -- padahal
        // STORYLINE.md menyebut ini eksplisit sbg "Puncak — Membangun PKS
        // Sendiri", tujuan akhir permainan.
        let pksBtn = UIButton(type: .system)
        pksBtn.setTitle("🏭 PKS", for: .normal)
        pksBtn.setTitleColor(.white, for: .normal)
        pksBtn.titleLabel?.font = .systemFont(ofSize: 11)
        pksBtn.backgroundColor = UIColor(white: 0, alpha: 0.5)
        pksBtn.layer.cornerRadius = 6
        pksBtn.contentEdgeInsets = UIEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
        pksBtn.translatesAutoresizingMaskIntoConstraints = false
        pksBtn.addTarget(self, action: #selector(tapPks), for: .touchUpInside)
        view.addSubview(pksBtn)
        leftColumnBtns.append(pksBtn)
        NSLayoutConstraint.activate([
            pksBtn.topAnchor.constraint(equalTo: hrBtn.bottomAnchor, constant: 6),
            pksBtn.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 12),
        ])

        // Tombol pengaturan grafik & sensitivitas -- fitur baru diminta
        // pengguna ("tambahkan pengaturan sensivitas dan grafik"). Identik
        // Android, lihat catatan lengkap di showSettingsDialog di bawah.
        let settingsBtn = UIButton(type: .system)
        settingsBtn.setTitle("⚙️ Pengaturan", for: .normal)
        settingsBtn.setTitleColor(.white, for: .normal)
        settingsBtn.titleLabel?.font = .systemFont(ofSize: 11)
        settingsBtn.backgroundColor = UIColor(white: 0, alpha: 0.5)
        settingsBtn.layer.cornerRadius = 6
        settingsBtn.contentEdgeInsets = UIEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
        settingsBtn.translatesAutoresizingMaskIntoConstraints = false
        settingsBtn.addTarget(self, action: #selector(tapSettings), for: .touchUpInside)
        view.addSubview(settingsBtn)
        leftColumnBtns.append(settingsBtn)
        NSLayoutConstraint.activate([
            settingsBtn.topAnchor.constraint(equalTo: pksBtn.bottomAnchor, constant: 6),
            settingsBtn.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 12),
        ])

        // Tombol kirim truk manual -- sisa terakhir dari audit sistematis
        // (kirimTruk sudah ada di EngineBridge, auto-timer sudah menangani
        // pengiriman otomatis tiap 26 detik, tapi belum ada cara pemain
        // memaksa kirim SEKARANG juga kalau mau, mis. sebelum ganti block).
        let truckBtn = UIButton(type: .system)
        truckBtn.setTitle("🚚 Kirim", for: .normal)
        truckBtn.setTitleColor(.white, for: .normal)
        truckBtn.titleLabel?.font = .systemFont(ofSize: 11)
        truckBtn.backgroundColor = UIColor(white: 0, alpha: 0.5)
        truckBtn.layer.cornerRadius = 6
        truckBtn.contentEdgeInsets = UIEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
        truckBtn.translatesAutoresizingMaskIntoConstraints = false
        truckBtn.addTarget(self, action: #selector(tapKirimTruk), for: .touchUpInside)
        view.addSubview(truckBtn)
        leftColumnBtns.append(truckBtn)
        NSLayoutConstraint.activate([
            truckBtn.topAnchor.constraint(equalTo: settingsBtn.bottomAnchor, constant: 6),
            truckBtn.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 12),
        ])

        // Tombol toggle layer beacon TBS matang -- mengatasi keluhan review
        // eksternal: "TBS sulit dibaca dari kejauhan krn pelepah menutupi".
        // Beacon (penanda mencolok, tembus di atas kanopi) sekarang bisa
        // dimatikan kalau dirasa mengganggu -- konsep "layer" spt di dokumen
        // desain Estate/Block View.
        let beaconBtn = UIButton(type: .system)
        beaconBtn.setTitle("🔴 Beacon: ON", for: .normal)
        beaconBtn.setTitleColor(.white, for: .normal)
        beaconBtn.titleLabel?.font = .systemFont(ofSize: 11)
        beaconBtn.backgroundColor = UIColor(white: 0, alpha: 0.5)
        beaconBtn.layer.cornerRadius = 6
        beaconBtn.contentEdgeInsets = UIEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
        beaconBtn.translatesAutoresizingMaskIntoConstraints = false
        beaconBtn.addTarget(self, action: #selector(tapToggleBeacon(_:)), for: .touchUpInside)
        view.addSubview(beaconBtn)
        leftColumnBtns.append(beaconBtn)
        NSLayoutConstraint.activate([
            beaconBtn.topAnchor.constraint(equalTo: truckBtn.bottomAnchor, constant: 6),
            beaconBtn.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 12),
        ])

        // Tombol toggle Musik & SFX -- suara SENGAJA disintesis sendiri (bukan
        // diunduh, lihat catatan lengkap di SoundManager.swift), jadi 100%
        // bebas lisensi & ukuran kecil. Satu tombol utk toggle KEDUANYA
        // sekaligus (musik latar + efek suara) -- sama persis pola Android.
        let musicBtn = UIButton(type: .system)
        musicBtn.setTitle("🎵 Suara: ON", for: .normal)
        musicBtn.setTitleColor(.white, for: .normal)
        musicBtn.titleLabel?.font = .systemFont(ofSize: 11)
        musicBtn.backgroundColor = UIColor(white: 0, alpha: 0.5)
        musicBtn.layer.cornerRadius = 6
        musicBtn.contentEdgeInsets = UIEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
        musicBtn.translatesAutoresizingMaskIntoConstraints = false
        musicBtn.addTarget(self, action: #selector(tapToggleMusic(_:)), for: .touchUpInside)
        view.addSubview(musicBtn)
        leftColumnBtns.append(musicBtn)
        NSLayoutConstraint.activate([
            musicBtn.topAnchor.constraint(equalTo: beaconBtn.bottomAnchor, constant: 6),
            musicBtn.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 12),
        ])
        // Tombol Estate View -- mode kamera zoom-out lihat SEMUA block
        // sekaligus dgn ubin warna per-layer. Tap = toggle on/off, tekan-lama
        // = ganti layer (siklus Kesehatan->Nutrisi->Kematangan).
        let estateBtn = UIButton(type: .system)
        estateBtn.setTitle("🗺️ Estate: Kesehatan", for: .normal)
        estateBtn.setTitleColor(.white, for: .normal)
        estateBtn.titleLabel?.font = .systemFont(ofSize: 11)
        estateBtn.backgroundColor = UIColor(white: 0, alpha: 0.5)
        estateBtn.layer.cornerRadius = 6
        estateBtn.contentEdgeInsets = UIEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
        estateBtn.translatesAutoresizingMaskIntoConstraints = false
        estateBtn.addTarget(self, action: #selector(tapEstateView(_:)), for: .touchUpInside)
        let estateLongPress = UILongPressGestureRecognizer(target: self, action: #selector(longPressEstateView(_:)))
        estateBtn.addGestureRecognizer(estateLongPress)
        view.addSubview(estateBtn)
        leftColumnBtns.append(estateBtn)
        NSLayoutConstraint.activate([
            estateBtn.topAnchor.constraint(equalTo: musicBtn.bottomAnchor, constant: 6),
            estateBtn.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 12),
        ])
        // Tombol "☰ Menu" -- SATU-SATUNYA yg SELALU visible di posisi paling
        // atas (menggantikan devBtn scr visual), buka/tutup SEMUA 9 tombol
        // kolom kiri sekaligus. Default TERSEMBUNYI (collapsed) -- lihat
        // catatan lengkap di leftColumnBtns (deklarasi property).
        let menuToggleBtn = UIButton(type: .system)
        menuToggleBtn.setTitle("☰ Menu", for: .normal)
        menuToggleBtn.setTitleColor(.white, for: .normal)
        menuToggleBtn.titleLabel?.font = .systemFont(ofSize: 11)
        menuToggleBtn.backgroundColor = UIColor(white: 0, alpha: 0.5)
        menuToggleBtn.layer.cornerRadius = 6
        menuToggleBtn.contentEdgeInsets = UIEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
        menuToggleBtn.translatesAutoresizingMaskIntoConstraints = false
        menuToggleBtn.addTarget(self, action: #selector(tapMenuToggle), for: .touchUpInside)
        view.addSubview(menuToggleBtn)
        NSLayoutConstraint.activate([
            menuToggleBtn.topAnchor.constraint(equalTo: hudBlock.bottomAnchor, constant: 6),
            menuToggleBtn.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 12),
        ])
        self.menuToggleBtn = menuToggleBtn

        setupBulkActionUi(anchorView: estateBtn)
        setupGameplayModeUi()
        for b in leftColumnBtns { b.isHidden = true } // sembunyikan SEMUA (termasuk bulkBtn dari fungsi terpisah di atas)

        panelTitle.textColor = UIColor(white: 0.95, alpha: 1)
        panelTitle.font = .boldSystemFont(ofSize: 14)
        panelTitle.text = "Ketuk sebuah pokok"

        actionStack.axis = .horizontal
        actionStack.spacing = 8
        actionStack.isHidden = true

        let scroll = UIScrollView()
        scroll.showsHorizontalScrollIndicator = false
        scroll.addSubview(actionStack)
        actionStack.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            actionStack.leadingAnchor.constraint(equalTo: scroll.leadingAnchor),
            actionStack.trailingAnchor.constraint(equalTo: scroll.trailingAnchor),
            actionStack.topAnchor.constraint(equalTo: scroll.topAnchor),
            actionStack.bottomAnchor.constraint(equalTo: scroll.bottomAnchor),
            actionStack.heightAnchor.constraint(equalTo: scroll.heightAnchor),
        ])

        let panel = UIStackView(arrangedSubviews: [panelTitle, scroll])
        panel.axis = .vertical
        panel.spacing = 8
        panel.isLayoutMarginsRelativeArrangement = true
        panel.layoutMargins = UIEdgeInsets(top: 10, left: 12, bottom: 10, right: 12)
        panel.backgroundColor = UIColor(red: 0.145, green: 0.212, blue: 0.122, alpha: 1.0)
        panel.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(panel)
        NSLayoutConstraint.activate([
            panel.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            panel.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            panel.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor),
            // Diperbesar dari 44pt -- radial menu (silang atas/bawah/kiri/kanan)
            // butuh ruang vertikal lbh besar drpd baris tombol datar lama.
            // bottomPanel di atas OTOMATIS menyesuaikan (constraint-driven).
            scroll.heightAnchor.constraint(equalToConstant: 190),
        ])
        bottomPanel = panel
        // bottomPanel (panelTitle+actionStack) TAK DIPAKAI LAGI utk aksi
        // per-pohon -- digantikan radialMenuOverlay yg floating. Disembunyikan
        // PERMANEN (bukan dihapus) -- constraint tetap AKTIF meski isHidden,
        // jadi bulkActionBar (yg posisinya terikat ke bottomPanel.topAnchor)
        // tak terpengaruh sama sekali.
        bottomPanel.isHidden = true

        radialMenuOverlay = UIView()
        // TANPA background solid -- pohon di balik overlay HARUS tetap
        // terlihat (celah diperbaiki: versi sebelumnya backgroundColor solid
        // alpha 0.87 menutupi seluruh area, termasuk pohonnya sendiri). Cuma
        // tombol individual & kartu status yg punya background kecil sendiri
        // (konsisten dgn Android yg sudah tanpa background di container luar).
        radialMenuOverlay.isHidden = true
        radialMenuOverlay.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(radialMenuOverlay)
        radialLeadingConstraint = radialMenuOverlay.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 0)
        radialTopConstraint = radialMenuOverlay.topAnchor.constraint(equalTo: view.topAnchor, constant: 0)
        NSLayoutConstraint.activate([
            radialLeadingConstraint, radialTopConstraint,
            radialMenuOverlay.widthAnchor.constraint(equalToConstant: 300),
            radialMenuOverlay.heightAnchor.constraint(equalToConstant: 400), // BUG UX diperbaiki (dilaporkan: "Detail 3D tumpang tindih dgn Rawat") -- 320 tak cukup utk statusLabel+detailBtn+semua tombol radial yg sudah diperbesar (target sentuh 52pt)
        ])
    }

    // MARK: - Log Aktivitas
    // Tombol mengambang + overlay layar penuh, menampilkan SEMUA aktivitas yg
    // pernah terjadi (panen, tunas, pupuk, rekrut, jual TBS, dst) — sebelumnya
    // toast cuma tampil sekilas lalu hilang tanpa jejak. Sumber datanya dari log
    // PERMANEN di engine (activityLogCount/activityLogEntry), bukan cuma event
    // sekali-poll (pollEventsRaw).

    private func setupActivityLogUi() {
        logButton.setTitle("📋 Log", for: .normal)
        logButton.setTitleColor(.white, for: .normal)
        logButton.backgroundColor = UIColor(white: 0, alpha: 0.35)
        logButton.layer.cornerRadius = 8
        logButton.contentEdgeInsets = UIEdgeInsets(top: 6, left: 12, bottom: 6, right: 12)
        logButton.translatesAutoresizingMaskIntoConstraints = false
        logButton.addTarget(self, action: #selector(toggleLogOverlay), for: .touchUpInside)
        view.addSubview(logButton)
        NSLayoutConstraint.activate([
            logButton.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 8),
            logButton.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -12),
        ])

        logOverlay.backgroundColor = UIColor(red: 0.071, green: 0.133, blue: 0.059, alpha: 0.93)
        logOverlay.isHidden = true
        logOverlay.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(logOverlay)
        NSLayoutConstraint.activate([
            logOverlay.topAnchor.constraint(equalTo: view.topAnchor),
            logOverlay.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            logOverlay.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            logOverlay.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])

        // Header tetap (judul + hint) + daftar entri yg bisa DIKETUK per-baris --
        // celah diperbaiki: sebelumnya 1 UITextView berisi SEMUA log jd satu
        // blok teks, tak mungkin dibuat entri per-baris bisa diketuk, dan
        // gesture "ketuk di mana saja utk tutup" jg dihapus krn kini bentrok
        // dgn tap individual tiap entri. Entri terkait pohon (treeId>=0)
        // diketuk -> tutup log, lompat kamera & pilih pohon itu.
        let headerLabel = UILabel()
        headerLabel.text = "📋 LOG AKTIVITAS"
        headerLabel.textColor = UIColor(white: 0.95, alpha: 1)
        headerLabel.font = .systemFont(ofSize: 15, weight: .bold)
        let hintLabel = UILabel()
        hintLabel.text = "Ketuk entri utk lompat ke pokok terkait -- ketuk ✕ utk tutup"
        hintLabel.textColor = UIColor(white: 0.7, alpha: 1)
        hintLabel.font = .systemFont(ofSize: 11)
        hintLabel.numberOfLines = 0

        logStackView.axis = .vertical
        logStackView.spacing = 4
        logStackView.alignment = .fill

        let outerStack = UIStackView(arrangedSubviews: [headerLabel, hintLabel, logStackView])
        outerStack.axis = .vertical
        outerStack.spacing = 8
        outerStack.translatesAutoresizingMaskIntoConstraints = false

        logScrollView.translatesAutoresizingMaskIntoConstraints = false
        logScrollView.addSubview(outerStack)
        logOverlay.addSubview(logScrollView)
        NSLayoutConstraint.activate([
            logScrollView.topAnchor.constraint(equalTo: logOverlay.safeAreaLayoutGuide.topAnchor, constant: 60),
            logScrollView.leadingAnchor.constraint(equalTo: logOverlay.leadingAnchor),
            logScrollView.trailingAnchor.constraint(equalTo: logOverlay.trailingAnchor),
            logScrollView.bottomAnchor.constraint(equalTo: logOverlay.bottomAnchor, constant: -24),
            outerStack.topAnchor.constraint(equalTo: logScrollView.topAnchor),
            outerStack.leadingAnchor.constraint(equalTo: logScrollView.leadingAnchor, constant: 16),
            outerStack.trailingAnchor.constraint(equalTo: logScrollView.trailingAnchor, constant: -16),
            outerStack.bottomAnchor.constraint(equalTo: logScrollView.bottomAnchor),
        ])

        // Tombol tutup eksplisit -- daftar yg scrollable bisa "mencuri" sentuhan
        // dari gesture ketuk-di-mana-saja, jadi ini jalan tutup yg pasti bekerja.
        let closeBtn = UIButton(type: .system)
        closeBtn.setTitle("✕ Tutup", for: .normal)
        closeBtn.setTitleColor(.white, for: .normal)
        closeBtn.backgroundColor = UIColor(white: 1, alpha: 0.15)
        closeBtn.layer.cornerRadius = 8
        closeBtn.contentEdgeInsets = UIEdgeInsets(top: 8, left: 14, bottom: 8, right: 14)
        closeBtn.translatesAutoresizingMaskIntoConstraints = false
        closeBtn.addTarget(self, action: #selector(toggleLogOverlay), for: .touchUpInside)
        logOverlay.addSubview(closeBtn)
        NSLayoutConstraint.activate([
            closeBtn.topAnchor.constraint(equalTo: logOverlay.safeAreaLayoutGuide.topAnchor, constant: 8),
            closeBtn.trailingAnchor.constraint(equalTo: logOverlay.trailingAnchor, constant: -12),
        ])
    }

    // MARK: - Aksi Massal
    // Baris tombol "Panen Semua/Angkut Semua/Pupuk Semua" dkk, diletakkan di
    // atas panel aksi pohon. INSTAN: pekerja TIDAK perlu mendatangi pohon
    // satu-persatu (revisi dari desain awal yg dibatasi jumlah pekerja
    // bebas). Panen/Angkut selalu gratis; Pupuk/Pestisida/Fungisida dicek
    // TOTAL biaya dulu utk semua pohon yg memenuhi syarat (semua-atau-
    // tidak-sama-sekali) -- lihat Engine::actionXSemua() di engine.cpp.
    private func setupBulkActionUi(anchorView: UIView) {
        // Aksi massal SEKARANG lewat 1 tombol toggle di kolom kiri (konsisten
        // dgn Android & pola PKS/SDM/dst) yg membuka actionSheet pilihan --
        // BUKAN bar horizontal permanen menempel di bawah layar (celah
        // diperbaiki: dulu memakan strip penuh, mengurangi ruang tampilan
        // game -- dilaporkan pengguna "supaya tampilan game bisa full
        // gambar"). tapPanenSemua() dkk (dgn confirmBulkAction di dalamnya)
        // TAK DIUBAH SAMA SEKALI -- cuma cara memicunya yg beda.
        let bulkBtn = UIButton(type: .system)
        bulkBtn.setTitle("⚡ Aksi Massal", for: .normal)
        bulkBtn.setTitleColor(.white, for: .normal)
        bulkBtn.titleLabel?.font = .systemFont(ofSize: 11)
        bulkBtn.backgroundColor = UIColor(white: 0, alpha: 0.5)
        bulkBtn.layer.cornerRadius = 6
        bulkBtn.contentEdgeInsets = UIEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
        bulkBtn.translatesAutoresizingMaskIntoConstraints = false
        bulkBtn.addTarget(self, action: #selector(tapBulkActionMenu), for: .touchUpInside)
        view.addSubview(bulkBtn)
        leftColumnBtns.append(bulkBtn)
        NSLayoutConstraint.activate([
            bulkBtn.topAnchor.constraint(equalTo: anchorView.bottomAnchor, constant: 6),
            bulkBtn.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 12),
        ])
    }

    // MARK: - Gameplay Mode (third-person, avatar bisa digerakkan)
    // Hasil review eksternal poin #5: "Untuk gameplay utama, gunakan
    // third-person perspective... Management Mode: top-down estate
    // overview". Tombol toggle + virtual joystick (base+knob programatik,
    // identik konsep dgn Android -- lihat catatan lengkap di MainActivity.kt).
    private var joyBase: UIView!
    private var joyKnob: UIView!
    private let joyBaseRadius: CGFloat = 70
    private let joyKnobRadius: CGFloat = 32
    private var modeToggleBtn: UIButton!
    private var fadeOverlay: UIView!

    private func setupGameplayModeUi() {
        // Overlay HITAM utk fade transition saat toggle mode -- identik
        // Android, lihat catatan lengkap di MainActivity.kt. Ditambahkan di
        // atas SEMUA subview lain (bringSubviewToFront) spy benar2 menutupi
        // layar penuh saat animasi.
        fadeOverlay = UIView()
        fadeOverlay.backgroundColor = .black
        fadeOverlay.alpha = 0
        fadeOverlay.isUserInteractionEnabled = false
        fadeOverlay.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(fadeOverlay)
        NSLayoutConstraint.activate([
            fadeOverlay.topAnchor.constraint(equalTo: view.topAnchor),
            fadeOverlay.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            fadeOverlay.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            fadeOverlay.trailingAnchor.constraint(equalTo: view.trailingAnchor),
        ])
        view.bringSubviewToFront(fadeOverlay)

        // Tombol toggle mode -- sudut kanan-bawah (area kosong, tak bentrok
        // dgn kolom tombol kiri/HUD atas/Estate button).
        modeToggleBtn = UIButton(type: .system)
        modeToggleBtn.setTitle("🗺️ Mode: Manajemen", for: .normal)
        modeToggleBtn.setTitleColor(.white, for: .normal)
        modeToggleBtn.titleLabel?.font = .systemFont(ofSize: 11)
        modeToggleBtn.backgroundColor = UIColor(white: 0, alpha: 0.5)
        modeToggleBtn.layer.cornerRadius = 6
        modeToggleBtn.contentEdgeInsets = UIEdgeInsets(top: 6, left: 10, bottom: 6, right: 10)
        modeToggleBtn.translatesAutoresizingMaskIntoConstraints = false
        modeToggleBtn.addTarget(self, action: #selector(tapModeToggle), for: .touchUpInside)
        view.addSubview(modeToggleBtn)
        NSLayoutConstraint.activate([
            modeToggleBtn.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor, constant: -12),
            modeToggleBtn.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -12),
        ])

        // Joystick DASAR (lingkaran besar, semi-transparan) -- sudut
        // kiri-bawah, konvensi umum mobile game virtual joystick.
        joyBase = UIView()
        joyBase.backgroundColor = UIColor(white: 1, alpha: 0.2)
        joyBase.layer.cornerRadius = joyBaseRadius
        joyBase.layer.borderWidth = 2
        joyBase.layer.borderColor = UIColor(white: 1, alpha: 0.5).cgColor
        joyBase.isHidden = true // sembunyi default -- Management Mode aktif di awal
        joyBase.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(joyBase)
        NSLayoutConstraint.activate([
            joyBase.widthAnchor.constraint(equalToConstant: joyBaseRadius*2),
            joyBase.heightAnchor.constraint(equalToConstant: joyBaseRadius*2),
            joyBase.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor, constant: -24),
            joyBase.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 24),
        ])

        // Joystick KNOB (lingkaran kecil, digeser dlm batas joyBase via transform)
        joyKnob = UIView()
        joyKnob.backgroundColor = UIColor(white: 1, alpha: 0.65)
        joyKnob.layer.cornerRadius = joyKnobRadius
        joyKnob.isHidden = true
        joyKnob.isUserInteractionEnabled = false // sentuhan diproses via gesture di joyBase, bukan knob sendiri
        joyKnob.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(joyKnob)
        NSLayoutConstraint.activate([
            joyKnob.widthAnchor.constraint(equalToConstant: joyKnobRadius*2),
            joyKnob.heightAnchor.constraint(equalToConstant: joyKnobRadius*2),
            joyKnob.centerXAnchor.constraint(equalTo: joyBase.centerXAnchor),
            joyKnob.centerYAnchor.constraint(equalTo: joyBase.centerYAnchor),
        ])

        // Gesture TERPISAH khusus joyBase (BUKAN onPan() global yg sudah
        // ada utk kamera pan Management Mode) -- supaya sentuhan di area
        // joystick tak bentrok/salah ditangkap gesture kamera.
        let joyPan = UIPanGestureRecognizer(target: self, action: #selector(onJoystickPan(_:)))
        joyBase.addGestureRecognizer(joyPan)

        setupInteractButton()
    }

    // DOWN/MOVE (.changed & .began): hitung offset dari pusat joyBase,
    // clamp ke radius maks (base-knob, spy knob tak keluar lingkaran
    // dasar), update posisi visual (transform) & arah avatar (dinormalisasi
    // -1..1 kasar -- engine yg menormalisasi ulang scr vektor).
    // UP/CANCEL (.ended/.cancelled): knob kembali ke tengah, arah avatar
    // (0,0) -- berhenti. Konvensi: dy layar positif = ke BAWAH -> gerak
    // MUNDUR avatar, jadi dirZ = -dy (atas joystick = maju), sama persis
    // konvensi Android.
    @objc private func onJoystickPan(_ g: UIPanGestureRecognizer) {
        switch g.state {
        case .began, .changed:
            let t = g.translation(in: joyBase)
            var dx = Float(t.x), dy = Float(t.y)
            let dist = sqrtf(dx*dx + dy*dy)
            let maxDist = Float(joyBaseRadius - joyKnobRadius)
            if dist > maxDist && dist > 0 { dx = dx/dist*maxDist; dy = dy/dist*maxDist }
            joyKnob.transform = CGAffineTransform(translationX: CGFloat(dx), y: CGFloat(dy))
            joystickDirX = dx/maxDist
            joystickDirZ = -dy/maxDist
        case .ended, .cancelled, .failed:
            joyKnob.transform = .identity
            joystickDirX = 0; joystickDirZ = 0
        default: break
        }
    }

    @objc private func tapMenuToggle() {
        let expanding = leftColumnBtns.first?.isHidden ?? true
        for b in leftColumnBtns { b.isHidden = !expanding }
        menuToggleBtn.setTitle(expanding ? "✕ Tutup" : "☰ Menu", for: .normal)
    }

    @objc private func tapModeToggle() {
        modeToggleBtn.isEnabled = false // cegah dobel-ketuk selama transisi berlangsung
        UIView.animate(withDuration: 0.15, animations: { self.fadeOverlay.alpha = 1 }, completion: { _ in
            // Perubahan mode SEBENARNYA terjadi DI SINI -- saat layar hitam
            // PENUH (alpha=1), jadi potongan kamera yg mendadak (ortografis
            // <-> perspektif) tak pernah terlihat pemain. Identik Android.
            let newActive = !self.engine.getGameplayModeActive()
            self.engine.setGameplayModeActive(newActive)
            self.modeToggleBtn.setTitle(newActive ? "🎮 Mode: Berjalan" : "🗺️ Mode: Manajemen", for: .normal)
            self.joyBase.isHidden = !newActive
            self.joyKnob.isHidden = !newActive
            self.interactBtn.isHidden = true // reset -- timer akan tampilkan lagi kalau memang dekat pohon
            if !newActive { self.joystickDirX = 0; self.joystickDirZ = 0 } // berhenti total saat kembali ke Management Mode
            UIView.animate(withDuration: 0.15, animations: { self.fadeOverlay.alpha = 0 }, completion: { _ in
                self.modeToggleBtn.isEnabled = true
            })
        })
    }

    // Tombol interaksi -- muncul HANYA saat avatar dekat pohon (Gameplay
    // Mode). Memakai ULANG renderActionPanel() yg SUDAH ADA (radial menu
    // sama persis dgn tap-to-select), cuma dipicu proximity avatar. Posisi
    // tengah-bawah, di ATAS joystick (tak bentrok).
    private func setupInteractButton() {
        interactBtn = UIButton(type: .system)
        interactBtn.setTitle("🌴 Interaksi", for: .normal)
        interactBtn.setTitleColor(.white, for: .normal)
        interactBtn.titleLabel?.font = .systemFont(ofSize: 13)
        interactBtn.backgroundColor = UIColor(white: 0, alpha: 0.6)
        interactBtn.layer.cornerRadius = 8
        interactBtn.contentEdgeInsets = UIEdgeInsets(top: 8, left: 14, bottom: 8, right: 14)
        interactBtn.isHidden = true
        interactBtn.translatesAutoresizingMaskIntoConstraints = false
        interactBtn.addTarget(self, action: #selector(tapInteract), for: .touchUpInside)
        view.addSubview(interactBtn)
        NSLayoutConstraint.activate([
            interactBtn.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor, constant: -100),
            interactBtn.centerXAnchor.constraint(equalTo: view.centerXAnchor),
        ])

        // Polling ringan (250ms, Timer terpisah dari update() per-frame --
        // nearestTreeToPlayer TAK PERLU dicek 60x/detik) -- cek pohon
        // terdekat avatar HANYA saat Gameplay Mode aktif. Radius 3.5 unit,
        // identik Android.
        Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self = self else { return }
            if self.engine.getGameplayModeActive() {
                self.nearbyTreeId = Int(self.engine.nearestTree(toPlayerMaxDist: 3.5))
                self.interactBtn.isHidden = self.nearbyTreeId < 0
            } else if !self.interactBtn.isHidden {
                self.interactBtn.isHidden = true
            }
        }
    }

    @objc private func tapInteract() {
        // BUG diperbaiki: SawitTreeView TIDAK PUNYA property 'id' (cuma
        // 'treeId') -- ketahuan dari error Swift yg membingungkan "Cannot
        // call value of non-function type" (closure yg gagal type-check krn
        // '.id' tak ada, membuat compiler fallback ke overload '.first'
        // PROPERTY bukan method 'first(where:)', menghasilkan error
        // berantai yg tak langsung menunjuk akar masalah sesungguhnya).
        // Konsisten dgn pola lain di file ini (baris 1137, 1835: $0.treeId).
        guard let tree = trees.first(where: { $0.treeId == nearbyTreeId }) else { return }
        // Highlight visual konsisten dgn tap-to-select (TANPA animasi kamera
        // -- kamera sudah third-person ikuti avatar).
        selectedTreeId = tree.treeId
        renderActionPanel(for: tree)
    }

    @objc private func tapBulkActionMenu() {
        let alert = GameAlertController(title: "Aksi Massal (Block saat ini)", message: nil, preferredStyle: .actionSheet)
        alert.addAction(GameAlertAction(title: "🪓 Panen Semua", style: .default) { [weak self] _ in self?.tapPanenSemua() })
        alert.addAction(GameAlertAction(title: "🚚 Angkut Semua", style: .default) { [weak self] _ in self?.tapAngkutSemua() })
        alert.addAction(GameAlertAction(title: "🧪 Pupuk Semua", style: .default) { [weak self] _ in self?.tapPupukSemua() })
        alert.addAction(GameAlertAction(title: "🐛 Semprot Semua", style: .default) { [weak self] _ in self?.tapPestisidaSemua() })
        alert.addAction(GameAlertAction(title: "🍄 Obati Semua", style: .default) { [weak self] _ in self?.tapFungisidaSemua() })
        alert.addAction(GameAlertAction(title: "Batal", style: .cancel))
        present(alert, animated: true)
    }

    // Target-action terpisah per jenis (bukan closure capturing self) — sama
    // spt pola tombol aksi pohon di atas, lebih aman dari retain-cycle.
    // Semua sekarang lewat confirmBulkAction -- dialog konfirmasi (jumlah pokok
    // + estimasi biaya) SEBELUM eksekusi. Review eksternal poin 10: "jangan
    // langsung 700 pokok secara magic, munculkan estimasi dulu".
    //
    // BUG UX diperbaiki (dilaporkan pengguna: "fitur panen semua lahan. saat
    // ini fitur hanya memanen lahan yang tampak, tidak memproses semua lahan
    // yang dimiliki"). Identik Android -- lihat catatan lengkap di
    // MainActivity.kt. AKAR MASALAH: preview count SEBELUMNYA cuma memakai
    // blockSummaries().first (BLOCK PERTAMA SAJA), TAPI action sesungguhnya
    // (actionPanenSemua() dkk, engine.cpp) SUDAH BENAR memproses SEMUA block
    // sekaligus (for(auto& t : trees_), tanpa filter block). Sekarang: agregat
    // SEMUA block via .reduce(0){...}.
    @objc private func tapPanenSemua() {
        let n = engine.blockSummaries().reduce(0) { $0 + Int($1.readyToHarvestCount) }
        confirmBulkAction("Panen Semua", previewCount: n, unitPrice: nil) { [weak self] in
            Int(self?.engine.actionPanenSemua() ?? 0)
        }
    }
    @objc private func tapAngkutSemua() {
        // BUG diperbaiki -- sama persis dgn perbaikan Android: dulu dialog
        // konfirmasi menjanjikan N pokok diproses (dihitung dr hasTbsReady
        // saja), tapi HASIL AKTUAL bisa 0 kalau TPH block sudah PENUH (aksi
        // massal berhenti begitu kapasitas TPH tercapai) -- tanpa penjelasan
        // sama sekali kenapa, pemain cuma lihat toast "0 pohon selesai" yg
        // membingungkan (dilaporkan pengguna). Diperbarui utk agregat semua
        // block (konsisten dgn perbaikan preview count) -- cek SEMUA block
        // penuh (bukan cuma satu), krn actionAngkutSemua() lanjut ke block
        // LAIN yg blm penuh scr independen.
        let blocks = engine.blockSummaries()
        let tphCap = engine.tphCap()
        let allFull = !blocks.isEmpty && blocks.allSatisfy { ($0.tphStock + $0.tphStockOverripe) >= tphCap }
        if allFull {
            showToast("TPH SEMUA block sudah PENUH -- kirim truk (🚚 Kirim) dulu sebelum mengangkut lebih banyak")
            return
        }
        let n = blocks.reduce(0) { $0 + Int($1.tbsAwaitingPickupCount) }
        confirmBulkAction("Angkut Semua", previewCount: n, unitPrice: nil) { [weak self] in
            Int(self?.engine.actionAngkutSemua() ?? 0)
        }
    }
    @objc private func tapPupukSemua() {
        let n = engine.blockSummaries().reduce(0) { $0 + Int($1.treeCount - $1.deadCount) }
        confirmBulkAction("Pupuk Semua", previewCount: n, unitPrice: engine.pricePupuk()) { [weak self] in
            Int(self?.engine.actionPupukSemua() ?? 0)
        }
    }
    @objc private func tapPestisidaSemua() {
        let n = engine.blockSummaries().reduce(0) { $0 + Int($1.hamaCount) }
        confirmBulkAction("Semprot Semua", previewCount: n, unitPrice: engine.pricePestisida()) { [weak self] in
            Int(self?.engine.actionPestisidaSemua() ?? 0)
        }
    }
    @objc private func tapFungisidaSemua() {
        let n = engine.blockSummaries().reduce(0) { $0 + Int($1.ganodermaCount) }
        confirmBulkAction("Obati Semua", previewCount: n, unitPrice: engine.priceFungisida()) { [weak self] in
            Int(self?.engine.actionFungisidaSemua() ?? 0)
        }
    }

    private func confirmBulkAction(_ label: String, previewCount: Int, unitPrice: Double?, action: @escaping () -> Int) {
        guard previewCount > 0 else {
            showToast("\(label): tidak ada pokok yg memenuhi syarat saat ini")
            return
        }
        // BUG diperbaiki -- pesan dialog SEBELUMNYA menyebut "Block <nama>"
        // (implikasi cuma 1 block), padahal action sesungguhnya memproses
        // SEMUA block. Diganti jumlah block eksplisit spy jujur mencerminkan
        // cakupan sebenarnya.
        let blockCount = engine.blockSummaries().count
        let costLine: String
        if let price = unitPrice {
            costLine = "\nEstimasi biaya: Rp \(Int(Double(previewCount) * price))"
        } else {
            costLine = "\nGratis (tanpa biaya)"
        }
        let alert = GameAlertController(
            title: label,
            message: "Seluruh lahan (\(blockCount) block): \(previewCount) pokok akan diproses.\(costLine)\n\nLanjutkan?",
            preferredStyle: .alert)
        alert.addAction(GameAlertAction(title: "Batal", style: .cancel))
        alert.addAction(GameAlertAction(title: "Terapkan", style: .default) { [weak self] _ in
            guard let self = self else { return }
            let done = action()
            self.bulkActionFeedback(label, done)
        })
        present(alert, animated: true)
    }

    private func bulkActionFeedback(_ label: String, _ count: Int) {
        showToast("\(label): \(count) pohon selesai sekaligus")
        refreshTreesAndHud()
    }

    @objc private func toggleLogOverlay() {
        logVisible.toggle()
        logOverlay.isHidden = !logVisible
        if logVisible { refreshActivityLog() }
    }

    private func refreshActivityLog() {
        let count = engine.activityLogCount()
        logStackView.arrangedSubviews.forEach { $0.removeFromSuperview() }
        if count == 0 {
            let lbl = UILabel()
            lbl.text = "Belum ada aktivitas."
            lbl.textColor = UIColor(white: 0.95, alpha: 1)
            lbl.font = .systemFont(ofSize: 13)
            logStackView.addArrangedSubview(lbl)
            return
        }
        for i in 0..<count {
            let text = engine.activityLogEntry(i)
            let treeId = Int(engine.activityLogTreeId(i))
            let lbl = UILabel()
            lbl.font = .systemFont(ofSize: 13)
            lbl.numberOfLines = 0
            if treeId >= 0 {
                lbl.text = "• \(text)  ›"
                lbl.textColor = UIColor(red: 0.62, green: 0.85, blue: 0.63, alpha: 1) // hijau muda -- isyarat "bisa diketuk"
                lbl.isUserInteractionEnabled = true
                lbl.tag = treeId
                lbl.addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(tapLogEntry(_:))))
            } else {
                lbl.text = "• \(text)"
                lbl.textColor = UIColor(white: 0.95, alpha: 1)
            }
            logStackView.addArrangedSubview(lbl)
        }
    }

    @objc private func tapLogEntry(_ g: UITapGestureRecognizer) {
        guard let lbl = g.view as? UILabel else { return }
        let treeId = lbl.tag
        toggleLogOverlay() // tutup dulu
        selectAndJumpToTree(treeId)
    }

    // Lompat kamera + pilih pohon dari entri log yg diketuk -- kalau pohon di
    // block LAIN dari yg sedang dilihat, kamera jg ikut pindah block.
    private func selectAndJumpToTree(_ treeId: Int) {
        // BUG diperbaiki (konsekuensi perbaikan performa 30ha di atas): `trees`
        // SEKARANG cuma berisi pohon dari currentBlockIndex (bukan semua
        // block lagi) -- fitur INI butuh mencari LINTAS SEMUA block (log
        // bisa mencatat kejadian pohon di block manapun), jadi pakai
        // findTreeById() TERPISAH dari `trees`, bukan trees.first{} yg
        // sekarang tak lagi valid utk pohon dari block lain.
        guard let tree = engine.findTreeById(treeId) else {
            showToast("Pokok #\(treeId) tidak ditemukan (mungkin sudah ditebang)")
            return
        }
        let blockIdx = Int(engine.blockId(forTree: treeId))
        if blockIdx >= 0 { currentBlockIndex = blockIdx }
        animateCameraTo(tree.x, tree.z)
        selectedTreeId = treeId
        lastRenderedTreeSnapshot = (id: tree.treeId, health: tree.health, ffb: tree.ffb, frond: tree.frond, hasTbsReady: tree.hasTbsReady)
        // Radial menu di TENGAH LAYAR (bukan posisi tap terakhir yg tak
        // relevan lagi) -- kamera baru MULAI animasi sinematik (0.6-1.0
        // detik), posisi layar pohon setelah kamera sampai belum pasti.
        radialScreenPoint = CGPoint(x: view.bounds.midX, y: view.bounds.midY)
        renderActionPanel(for: tree)
        refreshTreesAndHud()
    }

    // State radial menu -- level 1 (aksi utama) atau 2 (di dalam "Rawat").
    // Direset ke 1 HANYA saat pilih pohon BARU (bukan tiap refresh state
    // pohon yg SAMA), spy menu tak "lompat balik" ke level 1 tanpa diminta
    // saat pemain sedang di level 2 & kondisi pohon kebetulan berubah.
    private var radialLevel = 1
    private var radialTreeId = -1

    // Perbarui teks countdown/progress bar TBS SAJA (bagian akhir teks
    // statusLabel) -- identik Android (updateFfbCountdown di MainActivity.kt).
    // Dipanggil dari buildRadialMenu() (isi awal) & refreshTreesAndHud()
    // (polling 250ms, spy countdown TERLIHAT BERJALAN turun tiap detik,
    // bukan macet di angka sama selama radial menu terbuka -- SawitTreeView
    // TAK berubah selama countdown berlangsung, cuma ffbTimer internal yg
    // berubah, jadi perlu di-refresh terpisah dari perbandingan snapshot
    // yg sudah ada).
    private func updateFfbCountdown(treeId: Int) {
        guard let label = statusLabel, let t = trees.first(where: { $0.treeId == treeId }) else { return }
        var ffbTimer: Float = 0, ffbTimerMax: Float = 0
        engine.treeFfbProgress(treeId, outFfbTimer: &ffbTimer, outFfbTimerMax: &ffbTimerMax)
        let countdownText: String
        switch t.ffb {
        case 0: // None -- menunggu mulai tumbuh setelah panen sebelumnya
            let pct = ffbTimerMax > 0 ? min(100, max(0, Int((1 - ffbTimer/ffbTimerMax) * 100))) : 0
            countdownText = "\n⏳ Menunggu tumbuh: \(pct)% (\(Int(ffbTimer))dtk lagi)"
        case 1: // Growing -- sedang menuju matang
            let pct = ffbTimerMax > 0 ? min(100, max(0, Int((1 - ffbTimer/ffbTimerMax) * 100))) : 0
            countdownText = "\n🌱 Sedang tumbuh: \(pct)% (\(Int(ffbTimer))dtk lagi)"
        case 2: countdownText = "\n🌾 Siap panen! Lewat matang dlm \(Int(ffbTimer))dtk" // Ripe
        default: countdownText = "" // Overripe -- tak ada progress lanjutan, sudah tersirat dari ffbText "Lewat matang!"
        }
        let healthText: String
        switch t.health {
        case 0: healthText = "✅ Sehat"
        case 1: healthText = "🐛 Hama"
        case 2: healthText = "🍄 Ganoderma!"
        case 3: healthText = "☠️ Mati"
        default: healthText = "?"
        }
        var ffbText = ""
        if t.health != 3 {
            switch t.ffb {
            case 2: ffbText = " | 🔴 Siap panen"
            case 3: ffbText = " | ⚠️ Lewat matang!"
            case 1: ffbText = " | 🌾 Tumbuh"
            default: break
            }
        }
        label.text = "Pokok #\(t.treeId) (\(t.isMature ? "Egrek" : "Dodos"))\n\(healthText)\(ffbText)\(countdownText)"
    }

    private func renderActionPanel(for t: SawitTreeView) {
        if radialTreeId != t.treeId { radialLevel = 1; radialTreeId = t.treeId }
        lastRenderedTree = t // dipakai tapDetail3D -- butuh SELURUH state pohon, bukan cuma treeId
        buildRadialMenu(for: t)
    }

    // Radial menu 2-tingkat, FLOATING di posisi ketuk (celah diperbaiki: versi
    // sebelumnya menempel di bar bawah layar spt panel datar lama -- BUKAN
    // mengambang dekat pohon spt diminta pengguna) -- referensi spesifikasi:
    // Level 1 (Rawat/Pupuk/Panen/Angkut) -> pilih "Rawat" -> Level 2 (Tunas/
    // Pestisida/Ganoderma/Tebang). Dipetakan PERSIS ke 8 aksi yg SUDAH ADA.
    private func buildRadialMenu(for t: SawitTreeView) {
        radialMenuOverlay.isHidden = false
        radialMenuOverlay.subviews.forEach { $0.removeFromSuperview() }

        // Posisikan PUSAT overlay persis di titik ketuk (clamp spy tak
        // terpotong tepi layar).
        // Diperbesar (dulu 220x250) -- mengakomodasi tombol yg sekarang lebih
        // besar (~52pt target sentuh, dari ~24pt sebelumnya). HARUS sinkron
        // dgn constraint widthAnchor/heightAnchor radialMenuOverlay di setup awal.
        let overlayW: CGFloat = 300, overlayH: CGFloat = 400
        let screenW = view.bounds.width, screenH = view.bounds.height
        var left = radialScreenPoint.x - overlayW/2
        var top = radialScreenPoint.y - overlayH/2
        left = min(max(left, 8), max(screenW - overlayW - 8, 8))
        top = min(max(top, 60), max(screenH - overlayH - 60, 60))
        radialLeadingConstraint.constant = left
        radialTopConstraint.constant = top

        // Kartu status kondisi pohon -- floating BERSAMA radial menu (celah
        // diperbaiki: dulu panelTitle di posisi TETAP, sekarang ikut
        // "mengambang" ke mana pun radial menu muncul).
        let healthText: String
        switch t.health {
        case 0: healthText = "✅ Sehat"
        case 1: healthText = "🐛 Hama"
        case 2: healthText = "🍄 Ganoderma!"
        case 3: healthText = "☠️ Mati"
        default: healthText = "?"
        }
        var ffbText = ""
        if t.health != 3 {
            switch t.ffb {
            case 2: ffbText = " | 🔴 Siap panen"
            case 3: ffbText = " | ⚠️ Lewat matang!"
            case 1: ffbText = " | 🌾 Tumbuh"
            default: break
            }
        }
        let newStatusLabel = UILabel()
        newStatusLabel.text = "Pokok #\(t.treeId) (\(t.isMature ? "Egrek" : "Dodos"))\n\(healthText)\(ffbText)"
        newStatusLabel.textColor = UIColor(white: 0.95, alpha: 1)
        newStatusLabel.font = .systemFont(ofSize: 11)
        newStatusLabel.numberOfLines = 0
        newStatusLabel.textAlignment = .center
        newStatusLabel.translatesAutoresizingMaskIntoConstraints = false
        radialMenuOverlay.addSubview(newStatusLabel)
        statusLabel = newStatusLabel
        // Countdown/progress bar TBS -- tambahkan ke teks yg SUDAH dibuat
        // di atas (bukan label terpisah, lebih ringkas). Dipanggil di sini
        // (isi awal segera) & lagi tiap polling 250ms di refreshTreesAndHud()
        // (lihat updateFfbCountdown()).
        updateFfbCountdown(treeId: t.treeId)

        let closeBtn = UIButton(type: .system)
        closeBtn.setTitle("✕", for: .normal)
        closeBtn.setTitleColor(.white, for: .normal)
        closeBtn.translatesAutoresizingMaskIntoConstraints = false
        closeBtn.addTarget(self, action: #selector(tapCloseRadial), for: .touchUpInside)
        radialMenuOverlay.addSubview(closeBtn)

        let radialArea = UIView()
        radialArea.translatesAutoresizingMaskIntoConstraints = false
        radialMenuOverlay.addSubview(radialArea)

        let hub = UILabel()
        hub.text = "🌴"
        hub.font = .systemFont(ofSize: 20)
        hub.translatesAutoresizingMaskIntoConstraints = false
        radialArea.addSubview(hub)

        let detailBtn = UIButton(type: .system)
        detailBtn.setTitle("🔍 Detail 3D", for: .normal)
        detailBtn.setTitleColor(.white, for: .normal)
        detailBtn.titleLabel?.font = .systemFont(ofSize: 10)
        // BUG konsistensi diperbaiki: sebelumnya TIDAK ADA background sama
        // sekali (beda dari devBtn/landBtn/dst yg sudah punya latar gelap
        // semi-transparan) -- ketahuan saat audit review eksternal
        // "Interaksi: tombol besar seperti Detail 3D -> terasa spt aplikasi".
        detailBtn.backgroundColor = UIColor(white: 0, alpha: 0.5)
        detailBtn.layer.cornerRadius = 6
        detailBtn.contentEdgeInsets = UIEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
        detailBtn.translatesAutoresizingMaskIntoConstraints = false
        detailBtn.addTarget(self, action: #selector(tapDetail3D(_:)), for: .touchUpInside)
        detailBtn.tag = t.treeId
        radialMenuOverlay.addSubview(detailBtn)

        NSLayoutConstraint.activate([
            newStatusLabel.topAnchor.constraint(equalTo: radialMenuOverlay.topAnchor, constant: 6),
            newStatusLabel.centerXAnchor.constraint(equalTo: radialMenuOverlay.centerXAnchor),
            newStatusLabel.widthAnchor.constraint(lessThanOrEqualTo: radialMenuOverlay.widthAnchor, constant: -20),
            closeBtn.topAnchor.constraint(equalTo: radialMenuOverlay.topAnchor, constant: 4),
            closeBtn.trailingAnchor.constraint(equalTo: radialMenuOverlay.trailingAnchor, constant: -8),
            detailBtn.topAnchor.constraint(equalTo: newStatusLabel.bottomAnchor, constant: 6),
            detailBtn.centerXAnchor.constraint(equalTo: radialMenuOverlay.centerXAnchor),
            radialArea.topAnchor.constraint(equalTo: detailBtn.bottomAnchor, constant: 4),
            radialArea.leadingAnchor.constraint(equalTo: radialMenuOverlay.leadingAnchor, constant: 10),
            radialArea.trailingAnchor.constraint(equalTo: radialMenuOverlay.trailingAnchor, constant: -10),
            radialArea.bottomAnchor.constraint(equalTo: radialMenuOverlay.bottomAnchor, constant: -8),
            hub.centerXAnchor.constraint(equalTo: radialArea.centerXAnchor),
            hub.centerYAnchor.constraint(equalTo: radialArea.centerYAnchor),
        ])

        // Icon-only -- teks label ("Pupuk","Panen" dst) TAK DITAMPILKAN di
        // tombol, cuma emoji. Nama lengkap muncul via label sementara saat
        // HOVER (trackpad/mouse -- mendukung skenario spt screenshot yg
        // terlihat dijalankan sbg Mac Catalyst/Simulator dgn pointer) ATAU
        // TEKAN-LAMA (fallback utk sentuhan murni di iPhone/iPad tanpa mouse).
        func makeBtn(_ icon: String, _ label: String, enabled: Bool = true) -> UIButton {
            let b = UIButton(type: .system)
            b.setTitle(icon, for: .normal)
            b.isEnabled = enabled
            b.backgroundColor = UIColor(white: 1, alpha: enabled ? 0.16 : 0.05)
            b.setTitleColor(.white, for: .normal)
            // BUG diperbaiki: font 16pt + padding 4-6pt jauh di bawah standar
            // Apple HIG (minimum 44pt target sentuh) -- persis keluhan
            // pengguna "jari kesulitan hit tombol" (sama persis akar masalah
            // dgn versi Android). Sekarang 24pt + padding 14pt -> target
            // sentuh total ~52pt (melebihi standar minimum).
            b.titleLabel?.font = .systemFont(ofSize: 24)
            b.layer.cornerRadius = 8
            b.contentEdgeInsets = UIEdgeInsets(top: 14, left: 14, bottom: 14, right: 14)
            b.tag = t.treeId
            b.translatesAutoresizingMaskIntoConstraints = false
            radialArea.addSubview(b)
            let hover = UIHoverGestureRecognizer(target: self, action: #selector(onRadialBtnHover(_:)))
            b.addGestureRecognizer(hover)
            let longPress = UILongPressGestureRecognizer(target: self, action: #selector(onRadialBtnLongPress(_:)))
            longPress.minimumPressDuration = 0.4
            b.addGestureRecognizer(longPress)
            radialBtnLabels.setObject(label as NSString, forKey: b)
            return b
        }

        if radialLevel == 1 {
            let rawat = makeBtn("🌿", "Rawat")
            rawat.addTarget(self, action: #selector(tapRawatNav), for: .touchUpInside)
            let pupuk = makeBtn("🧪", "Pupuk", enabled: t.health != 3)
            pupuk.addTarget(self, action: #selector(tapPupuk(_:)), for: .touchUpInside)
            let panen = makeBtn("🔪", "Panen", enabled: (t.ffb == 2 || t.ffb == 3) && t.health != 3)
            panen.addTarget(self, action: #selector(tapPanen(_:)), for: .touchUpInside)
            let angkut = makeBtn("🚚", "Angkut", enabled: t.hasTbsReady)
            angkut.addTarget(self, action: #selector(tapAngkut(_:)), for: .touchUpInside)
            NSLayoutConstraint.activate([
                rawat.topAnchor.constraint(equalTo: radialArea.topAnchor),
                rawat.centerXAnchor.constraint(equalTo: radialArea.centerXAnchor),
                pupuk.centerYAnchor.constraint(equalTo: radialArea.centerYAnchor),
                pupuk.leadingAnchor.constraint(equalTo: radialArea.leadingAnchor),
                panen.centerYAnchor.constraint(equalTo: radialArea.centerYAnchor),
                panen.trailingAnchor.constraint(equalTo: radialArea.trailingAnchor),
                angkut.bottomAnchor.constraint(equalTo: radialArea.bottomAnchor),
                angkut.centerXAnchor.constraint(equalTo: radialArea.centerXAnchor),
            ])
        } else {
            let back = makeBtn("←", "Kembali")
            back.addTarget(self, action: #selector(tapBackNav), for: .touchUpInside)
            let tunas = makeBtn("✂️", "Tunas", enabled: t.frond > 0.35 && t.health != 3)
            tunas.addTarget(self, action: #selector(tapTunas(_:)), for: .touchUpInside)
            let pestisida = makeBtn("🐛", "Pestisida", enabled: t.health == 1)
            pestisida.addTarget(self, action: #selector(tapPestisida(_:)), for: .touchUpInside)
            let ganoderma = makeBtn("🍄", "Ganoderma", enabled: t.health == 2)
            ganoderma.addTarget(self, action: #selector(tapFungisida(_:)), for: .touchUpInside)
            let tebang = makeBtn("🪓", "Tebang", enabled: t.health == 3)
            tebang.addTarget(self, action: #selector(tapTebang(_:)), for: .touchUpInside)
            NSLayoutConstraint.activate([
                back.topAnchor.constraint(equalTo: radialArea.topAnchor),
                back.centerXAnchor.constraint(equalTo: radialArea.centerXAnchor),
                tunas.centerYAnchor.constraint(equalTo: radialArea.centerYAnchor),
                tunas.leadingAnchor.constraint(equalTo: radialArea.leadingAnchor),
                pestisida.centerYAnchor.constraint(equalTo: radialArea.centerYAnchor),
                pestisida.trailingAnchor.constraint(equalTo: radialArea.trailingAnchor),
                ganoderma.bottomAnchor.constraint(equalTo: radialArea.bottomAnchor),
                ganoderma.leadingAnchor.constraint(equalTo: radialArea.leadingAnchor),
                tebang.bottomAnchor.constraint(equalTo: radialArea.bottomAnchor),
                tebang.trailingAnchor.constraint(equalTo: radialArea.trailingAnchor),
            ])
        }
    }

    @objc private func onRadialBtnHover(_ g: UIHoverGestureRecognizer) {
        guard let btn = g.view as? UIButton, let label = radialBtnLabels.object(forKey: btn) as String? else { return }
        switch g.state {
        case .began, .changed:
            showRadialHoverLabel(label, near: btn)
        default:
            hideRadialHoverLabel()
        }
    }
    @objc private func onRadialBtnLongPress(_ g: UILongPressGestureRecognizer) {
        guard let btn = g.view as? UIButton, let label = radialBtnLabels.object(forKey: btn) as String? else { return }
        if g.state == .began {
            showRadialHoverLabel(label, near: btn)
        } else if g.state == .ended || g.state == .cancelled {
            hideRadialHoverLabel()
        }
    }
    private func showRadialHoverLabel(_ text: String, near btn: UIButton) {
        radialHoverLabel?.removeFromSuperview()
        let lbl = UILabel()
        lbl.text = text
        lbl.textColor = .white
        lbl.font = .systemFont(ofSize: 10)
        lbl.backgroundColor = UIColor(white: 0, alpha: 0.75)
        lbl.layer.cornerRadius = 4
        lbl.layer.masksToBounds = true
        lbl.textAlignment = .center
        lbl.translatesAutoresizingMaskIntoConstraints = false
        radialMenuOverlay.addSubview(lbl)
        NSLayoutConstraint.activate([
            lbl.bottomAnchor.constraint(equalTo: btn.topAnchor, constant: -2),
            lbl.centerXAnchor.constraint(equalTo: btn.centerXAnchor),
            lbl.widthAnchor.constraint(greaterThanOrEqualToConstant: 44),
            lbl.heightAnchor.constraint(equalToConstant: 18),
        ])
        radialHoverLabel = lbl
    }
    private func hideRadialHoverLabel() {
        radialHoverLabel?.removeFromSuperview()
        radialHoverLabel = nil
    }

    @objc private func tapCloseRadial() {
        selectedTreeId = -1
        lastRenderedTreeSnapshot = nil
        radialMenuOverlay.isHidden = true
    }
    @objc private func tapRawatNav() {
        soundManager.playClick()
        radialLevel = 2
        if let t = lastRenderedTree { buildRadialMenu(for: t) }
    }
    @objc private func tapBackNav() {
        soundManager.playClick()
        radialLevel = 1
        if let t = lastRenderedTree { buildRadialMenu(for: t) }
    }

    // Target-action terpisah per jenis (bukan closure capturing self) — lebih aman
    // dari retain-cycle & lebih mudah dibaca dibanding closure bersarang di Swift.
    @objc private func tapTunas(_ sender: UIButton) { guard debounceGuard(sender) else { return }; soundManager.playClick(); if engine.actionTunas(sender.tag) { refreshTreesAndHud() } }
    @objc private func tapPanen(_ sender: UIButton) { guard debounceGuard(sender) else { return }; soundManager.playClick(); if engine.actionPanen(sender.tag) { refreshTreesAndHud() } }
    @objc private func tapAngkut(_ sender: UIButton) { guard debounceGuard(sender) else { return }; soundManager.playClick(); if engine.actionAngkut(sender.tag) { refreshTreesAndHud() } }
    @objc private func tapPupuk(_ sender: UIButton) { guard debounceGuard(sender) else { return }; soundManager.playClick(); if engine.actionPupuk(sender.tag) { refreshTreesAndHud() } }
    @objc private func tapPestisida(_ sender: UIButton) { guard debounceGuard(sender) else { return }; soundManager.playClick(); if engine.actionPestisida(sender.tag) { refreshTreesAndHud() } }
    @objc private func tapFungisida(_ sender: UIButton) { guard debounceGuard(sender) else { return }; soundManager.playClick(); if engine.actionFungisida(sender.tag) { refreshTreesAndHud() } }
    @objc private func tapTebang(_ sender: UIButton) { guard debounceGuard(sender) else { return }; soundManager.playClick(); if engine.actionTebang(sender.tag) { refreshTreesAndHud() } }

    // -------------------------------------------------------------------
    // INSPECTOR POHON — close-up 1 pohon berputar otomatis, dirender NATIVE
    // OpenGL (glDrawTreeInspectorAge -> drawPalm/drawTbsPile yg SAMA dgn
    // scene utama, sudah termasuk mahkota golden-angle & warna sesuai
    // kesehatan). INI MENGGANTIKAN pendekatan WKWebView/tree_detail.html
    // sepenuhnya -- tidak ada lagi format HTML yg dipakai di app ini.
    // -------------------------------------------------------------------
    @objc private func tapDetail3D(_ sender: UIButton) {
        guard let t = lastRenderedTree, t.treeId == sender.tag else { return }
        inspectorTree = t
        inspectorYaw = 0
        inspectorAutoSpin = true // reset -- auto-spin nyala lagi sampai pemain sentuh kontrol manual
        inspectorPanY = 0 // reset tiap buka baru, mulai dari framing default
        // Mulai transisi kamera SMOOTH (poin laporan pengguna: "Saat inspeksi
        // pohon kamera melakukan smooth focus/zoom ke pohon") -- identik
        // Android, lihat catatan lengkap di renderer_gl.cpp.
        engine.beginTreeInspectorTransition()
        if inspectorCloseBtn == nil {
            let closeBtn = UIButton(type: .system)
            closeBtn.setTitle("✕ Tutup", for: .normal)
            closeBtn.setTitleColor(.white, for: .normal)
            closeBtn.backgroundColor = UIColor(white: 0, alpha: 0.45)
            closeBtn.layer.cornerRadius = 8
            closeBtn.contentEdgeInsets = UIEdgeInsets(top: 8, left: 14, bottom: 8, right: 14)
            closeBtn.translatesAutoresizingMaskIntoConstraints = false
            closeBtn.addTarget(self, action: #selector(closeTreeInspector), for: .touchUpInside)
            view.addSubview(closeBtn)
            NSLayoutConstraint.activate([
                closeBtn.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 8),
                closeBtn.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -12),
            ])
            inspectorCloseBtn = closeBtn

            // Tombol navigasi naik/turun -- gesture pan tak selalu bisa diuji di
            // desktop/simulator (mouse), jadi ini jalur PASTI bekerja di platform
            // apapun. Ditahan (bukan sekali tap) spy geser halus, spt Android.
            let navUp = UIButton(type: .system)
            navUp.setTitle("▲", for: .normal)
            navUp.setTitleColor(.white, for: .normal)
            navUp.backgroundColor = UIColor(white: 0, alpha: 0.45)
            navUp.layer.cornerRadius = 8
            navUp.contentEdgeInsets = UIEdgeInsets(top: 10, left: 16, bottom: 10, right: 16)
            navUp.translatesAutoresizingMaskIntoConstraints = false
            navUp.addTarget(self, action: #selector(navUpDown(_:)), for: .touchDown)
            navUp.addTarget(self, action: #selector(navRelease), for: [.touchUpInside, .touchUpOutside, .touchCancel])
            view.addSubview(navUp)

            let navDown = UIButton(type: .system)
            navDown.setTitle("▼", for: .normal)
            navDown.setTitleColor(.white, for: .normal)
            navDown.backgroundColor = UIColor(white: 0, alpha: 0.45)
            navDown.layer.cornerRadius = 8
            navDown.contentEdgeInsets = UIEdgeInsets(top: 10, left: 16, bottom: 10, right: 16)
            navDown.translatesAutoresizingMaskIntoConstraints = false
            navDown.addTarget(self, action: #selector(navDownDown(_:)), for: .touchDown)
            navDown.addTarget(self, action: #selector(navRelease), for: [.touchUpInside, .touchUpOutside, .touchCancel])
            view.addSubview(navDown)

            NSLayoutConstraint.activate([
                navUp.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -16),
                navUp.bottomAnchor.constraint(equalTo: view.centerYAnchor, constant: -8),
                navDown.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -16),
                navDown.topAnchor.constraint(equalTo: view.centerYAnchor, constant: 8),
            ])
            inspectorNavUp = navUp
            inspectorNavDown = navDown

            let navLeft = UIButton(type: .system)
            navLeft.setTitle("◀", for: .normal)
            navLeft.setTitleColor(.white, for: .normal)
            navLeft.backgroundColor = UIColor(white: 0, alpha: 0.45)
            navLeft.layer.cornerRadius = 8
            navLeft.contentEdgeInsets = UIEdgeInsets(top: 10, left: 16, bottom: 10, right: 16)
            navLeft.translatesAutoresizingMaskIntoConstraints = false
            navLeft.addTarget(self, action: #selector(navLeftDown(_:)), for: .touchDown)
            navLeft.addTarget(self, action: #selector(navRelease), for: [.touchUpInside, .touchUpOutside, .touchCancel])
            view.addSubview(navLeft)

            let navRight = UIButton(type: .system)
            navRight.setTitle("▶", for: .normal)
            navRight.setTitleColor(.white, for: .normal)
            navRight.backgroundColor = UIColor(white: 0, alpha: 0.45)
            navRight.layer.cornerRadius = 8
            navRight.contentEdgeInsets = UIEdgeInsets(top: 10, left: 16, bottom: 10, right: 16)
            navRight.translatesAutoresizingMaskIntoConstraints = false
            navRight.addTarget(self, action: #selector(navRightDown(_:)), for: .touchDown)
            navRight.addTarget(self, action: #selector(navRelease), for: [.touchUpInside, .touchUpOutside, .touchCancel])
            view.addSubview(navRight)

            NSLayoutConstraint.activate([
                navLeft.centerXAnchor.constraint(equalTo: view.centerXAnchor, constant: -50),
                navLeft.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor, constant: -140),
                navRight.centerXAnchor.constraint(equalTo: view.centerXAnchor, constant: 50),
                navRight.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor, constant: -140),
            ])
            inspectorNavLeft = navLeft
            inspectorNavRight = navRight
        }
        inspectorCloseBtn?.isHidden = false
        inspectorNavUp?.isHidden = false
        inspectorNavDown?.isHidden = false
        inspectorNavLeft?.isHidden = false
        inspectorNavRight?.isHidden = false
        radialMenuOverlay.isHidden = true
    }

    @objc private func navUpDown(_ sender: UIButton) { startNavHold(axis: 1, dir: 1) }
    @objc private func navDownDown(_ sender: UIButton) { startNavHold(axis: 1, dir: -1) }
    @objc private func navLeftDown(_ sender: UIButton) { inspectorAutoSpin = false; startNavHold(axis: 2, dir: -1) }
    @objc private func navRightDown(_ sender: UIButton) { inspectorAutoSpin = false; startNavHold(axis: 2, dir: 1) }
    // axis: 1=vertikal(panY), 2=rotasi(yaw manual)
    private func startNavHold(axis: Int, dir: Float) {
        navHoldDir = dir
        navHoldTimer?.invalidate()
        navHoldTimer = Timer.scheduledTimer(withTimeInterval: 0.016, repeats: true) { [weak self] _ in
            guard let self = self else { return }
            if axis == 1 {
                self.inspectorPanY = min(9, max(-1, self.inspectorPanY + self.navHoldDir * 0.10))
            } else {
                self.inspectorYaw += self.navHoldDir * 0.06
            }
        }
    }
    @objc private func navRelease() {
        navHoldDir = 0
        navHoldTimer?.invalidate()
        navHoldTimer = nil
    }

    @objc private func tapBlockSelector() {
        let blocks = engine.blockSummaries()
        // BUG diperbaiki: nama tipe C++ MURNI 'BlockSummary' (types.hpp)
        // TIDAK bisa dipakai sbg anotasi tipe eksplisit di Swift murni --
        // ketahuan dari error Xcode "Cannot find type 'BlockSummary' in
        // scope". Nama Swift-bridged yg BENAR adalah 'SawitBlockSummary'
        // (dgn prefix, spt 'SawitTreeView') -- dikonfirmasi dari penggunaan
        // lain di file ini (var bestBlock: SawitBlockSummary?, baris ~314).
        func detailFor(_ b: SawitBlockSummary) -> String {
            var s = "\(b.statusEmoji) Block \(b.name) — \(b.treeCount) pokok\n\(b.soilDesc), \(b.genDesc)\n✅ \(b.healthyCount) sehat"
            if b.lowNutritionCount > 0 { s += " · 🟡 \(b.lowNutritionCount) kekurangan hara" }
            if b.hamaCount > 0 { s += " · 🐛 \(b.hamaCount) hama" }
            if b.ganodermaCount > 0 { s += " · 🍄 \(b.ganodermaCount) ganoderma" }
            if b.readyToHarvestCount > 0 { s += "\n🌾 \(b.readyToHarvestCount) siap panen" }
            return s
        }
        if blocks.count <= 1 {
            // BUG UX diperbaiki: sebelumnya cuma toast "baru ada 1 Block"
            // (tak berguna) -- sekarang tampilkan detail LENGKAP status
            // block itu, identik Android.
            if let b = blocks.first {
                let alert = GameAlertController(title: "Detail Block", message: detailFor(b), preferredStyle: .alert)
                alert.addAction(GameAlertAction(title: "Tutup", style: .cancel))
                present(alert, animated: true)
            }
            return
        }
        let alert = GameAlertController(title: "Lompat ke Block", message: nil, preferredStyle: .actionSheet)
        for (idx, b) in blocks.enumerated() {
            alert.addAction(GameAlertAction(title: detailFor(b), style: .default) { [weak self] _ in
                guard let self = self else { return }
                self.currentBlockIndex = idx
                // Lompat kamera ke pusat block yg dipilih -- jarak/rotasi
                // dipertahankan spy tetap terasa sbg "geser pandangan".
                self.animateCameraTo(b.originX, b.originZ)
                self.refreshTreesAndHud()
                self.showToast("Kamera lompat ke Block \(b.name)")
            })
        }
        alert.addAction(GameAlertAction(title: "Tutup", style: .cancel))
        if let popover = alert.popoverPresentationController {
            popover.sourceView = view
            popover.sourceRect = CGRect(x: view.bounds.midX, y: view.bounds.midY, width: 0, height: 0)
        }
        present(alert, animated: true)
    }

    @objc private func tapToggleMusic(_ sender: UIButton) {
        let bgmOn = soundManager.toggleBgm()
        let sfxOn = soundManager.toggleSfx()
        sender.setTitle((bgmOn || sfxOn) ? "🎵 Suara: ON" : "🔇 Suara: OFF", for: .normal)
        if sfxOn { soundManager.playClick() } // konfirmasi terdengar begitu dinyalakan lagi
    }

    @objc private func tapEstateView(_ sender: UIButton) {
        soundManager.playClick()
        estateViewActive.toggle()
        if estateViewActive {
            savedCamX = panX; savedCamZ = panZ; savedCamDist = camDist
            camDist = 200 // zoom jauh -- cukup lihat semua block yg wajar (batas 280 saat aktif, lihat onPinch)
            animateCameraTo(0, 0) // pusatkan pandangan (blocks tersebar dari X=0 ke arah +X)
        } else {
            camDist = savedCamDist
            animateCameraTo(savedCamX, savedCamZ)
        }
        engine.setEstateViewModeActive(estateViewActive, layer: estateViewLayer)
    }

    @objc private func longPressEstateView(_ g: UILongPressGestureRecognizer) {
        guard g.state == .began, let btn = g.view as? UIButton else { return }
        estateViewLayer = (estateViewLayer + 1) % 3
        let layerName: String
        switch estateViewLayer { case 0: layerName = "Kesehatan"; case 1: layerName = "Nutrisi"; default: layerName = "Kematangan" }
        btn.setTitle("🗺️ Estate: \(layerName)", for: .normal)
        if engine.getEstateViewActive() { engine.setEstateViewModeActive(true, layer: estateViewLayer) }
        showToast("Layer: \(layerName)")
    }

    @objc private func tapToggleBeacon(_ sender: UIButton) {
        let newState = !engine.showHarvestBeacon()
        engine.setShowHarvestBeacon(newState)
        sender.setTitle(newState ? "🔴 Beacon: ON" : "⚪ Beacon: OFF", for: .normal)
    }

    @objc private func tapKirimTruk() {
        let stockBefore = engine.tphStock(currentBlockIndex)
        if stockBefore <= 0 {
            showToast("TPH block ini masih kosong")
        } else {
            engine.kirimTruk(currentBlockIndex)
            showToast("Truk dikirim dari block yg sedang dilihat!")
            refreshTreesAndHud()
        }
    }

    // Dialog pengaturan grafik & sensitivitas kamera -- fitur baru diminta
    // pengguna ("tambahkan pengaturan sensivitas dan grafik"). Identik
    // Android, lihat catatan lengkap di showSettingsDialog(), MainActivity.kt.
    // Persistensi via UserDefaults -- dimuat ulang di loadAndApplySettings()
    // (dipanggil viewDidLoad, lihat di sana).
    @objc private func tapSettings() {
        let container = UIStackView()
        container.axis = .vertical
        container.spacing = 8
        container.translatesAutoresizingMaskIntoConstraints = false

        let graphicsLabel = UILabel()
        graphicsLabel.text = "🖥️ Kualitas Grafis"
        graphicsLabel.textColor = .white
        graphicsLabel.font = .systemFont(ofSize: 13)
        container.addArrangedSubview(graphicsLabel)

        let graphicsHint = UILabel()
        graphicsHint.text = "Rendah = FPS lebih baik di lahan luas (100ha+)."
        graphicsHint.textColor = UIColor(white: 0.75, alpha: 1)
        graphicsHint.font = .systemFont(ofSize: 10)
        graphicsHint.numberOfLines = 0
        container.addArrangedSubview(graphicsHint)

        let segmented = UISegmentedControl(items: ["Rendah", "Sedang", "Tinggi"])
        segmented.selectedSegmentIndex = engine.getGraphicsQuality()
        segmented.addAction(UIAction { [weak self] _ in
            guard let self = self else { return }
            self.engine.setGraphicsQuality(segmented.selectedSegmentIndex)
            UserDefaults.standard.set(segmented.selectedSegmentIndex, forKey: "graphics_quality")
        }, for: .valueChanged)
        container.addArrangedSubview(segmented)

        let sensLabel = UILabel()
        let currentSens = engine.getCameraSensitivity()
        sensLabel.text = "🎯 Sensitivitas Kamera: \(String(format: "%.2f", currentSens))x"
        sensLabel.textColor = .white
        sensLabel.font = .systemFont(ofSize: 13)
        container.addArrangedSubview(sensLabel)

        let sensSlider = UISlider()
        sensSlider.minimumValue = 0.5
        sensSlider.maximumValue = 2.0
        sensSlider.value = currentSens
        sensSlider.addAction(UIAction { [weak self] _ in
            guard let self = self else { return }
            let mult = sensSlider.value
            sensLabel.text = "🎯 Sensitivitas Kamera: \(String(format: "%.2f", mult))x"
            self.engine.setCameraSensitivity(mult)
            UserDefaults.standard.set(mult, forKey: "camera_sensitivity")
        }, for: .valueChanged)
        container.addArrangedSubview(sensSlider)

        let alert = GameAlertController(title: "⚙️ Pengaturan", message: nil, preferredStyle: .alert)
        alert.customContentView = container
        alert.addAction(GameAlertAction(title: "Tutup", style: .cancel))
        present(alert, animated: true)
    }

    // Muat pengaturan tersimpan (UserDefaults) & terapkan ke renderer --
    // dipanggil SEKALI di viewDidLoad(), SETELAH engine siap.
    private func loadAndApplySettings() {
        let savedQuality = UserDefaults.standard.object(forKey: "graphics_quality") as? Int ?? 1 // default Sedang
        let savedSensitivity = UserDefaults.standard.object(forKey: "camera_sensitivity") as? Float ?? 1.0 // default 1.0x
        engine.setGraphicsQuality(savedQuality)
        engine.setCameraSensitivity(savedSensitivity)
    }

    // Dialog upgrade kapasitas TPH -- fitur baru diminta pengguna. Identik
    // Android, lihat catatan lengkap (formula/alasan tak ada referensi
    // ilmiah kuantitatif utk kapasitas TPH) di engine.cpp.
    @objc private func tapTphUpgrade() {
        let level = engine.tphLevel()
        let cap = engine.tphCap()
        let cost = engine.tphUpgradeCost()
        let alert = GameAlertController(
            title: "📦 TPH (Tempat Pengumpulan Hasil) -- Level \(level)",
            message: "Kapasitas saat ini: \(Int(cap)) tandan/TPH\nSetelah upgrade: \(Int(cap+15)) tandan/TPH\n\nBerlaku utk SEMUA TPH di seluruh lahan sekaligus.\n\nBiaya: Rp \(Int(cost))",
            preferredStyle: .alert)
        alert.addAction(GameAlertAction(title: "Upgrade", style: .default) { [weak self] _ in
            guard let self = self else { return }
            if !self.engine.upgradeTph() { self.showToast("Uang tidak cukup") }
            self.refreshTreesAndHud()
        })
        alert.addAction(GameAlertAction(title: "Tutup", style: .cancel))
        present(alert, animated: true)
    }

    @objc private func tapPks() {
        if !engine.pksBuilt() {
            let buildCost = engine.pksBuildCost()
            let alert = GameAlertController(
                title: "🏭 Bangun PKS Sendiri",
                message: "Selama belum punya PKS, semua TBS dijual mentah ke pabrik luar dgn harga flat.\n\n" +
                    "Dengan PKS sendiri: TBS diolah jadi CPO + inti sawit, margin jauh lebih tinggi.\n\n" +
                    "Syarat: Manager/ADM (jenjang SDM tertinggi)\nBiaya: Rp \(Int(buildCost))",
                preferredStyle: .alert)
            alert.addAction(GameAlertAction(title: "Bangun", style: .default) { [weak self] _ in
                guard let self = self else { return }
                let ok = self.engine.bangunPks()
                self.showToast(ok ? "PKS berhasil dibangun!" : "Gagal -- cek syarat Manager/ADM atau uang")
                self.refreshTreesAndHud()
            })
            alert.addAction(GameAlertAction(title: "Tutup", style: .cancel))
            present(alert, animated: true)
            return
        }
        let level = engine.pksLevel()
        let oer = engine.pksOer()
        let silo = engine.pksInputSilo()
        let upgradeCost = engine.pksUpgradeCostNow()
        // Audit menemukan: kapasitas batch (bug #10 "TPH-PKS mismatch") &
        // estimasi pendapatan BELUM PERNAH ditampilkan di iOS sama sekali
        // (sudah diperbaiki di Android sesi sebelumnya, tertinggal di sini).
        let batchCap = engine.pksCapacityPerBatch()
        let cpoPrice = engine.pksCpoPrice()
        let pkPrice = engine.pksPkPrice()
        let kerRate = engine.pksKerRate()
        let avgTandanKg = engine.pksAvgTandanKg()
        let amountEst = min(silo, Double(batchCap))
        let kgTbsEst = amountEst * avgTandanKg
        let estIncome = (kgTbsEst*oer*cpoPrice) + (kgTbsEst*kerRate*pkPrice)
        let overCapNote = silo > Double(batchCap) ? " (silo melebihi kapasitas -- perlu proses berkali-kali, TBS TETAP AMAN menunggu)" : ""
        let alert = GameAlertController(
            title: "🏭 PKS -- Level \(level)",
            message: "Rendemen (OER): \(String(format: "%.1f", oer*100))%\n" +
                "Silo TBS menunggu diolah: \(Int(silo)) tandan\n" +
                "Kapasitas proses per-batch: \(batchCap) tandan\(overCapNote)\n" +
                "Harga jual: CPO Rp \(Int(cpoPrice))/kg, Inti Rp \(Int(pkPrice))/kg\n" +
                "Estimasi pendapatan batch berikutnya: Rp \(Int(estIncome))\n\n" +
                "Upgrade berikutnya: Rp \(Int(upgradeCost)) (naikkan kapasitas & rendemen)",
            preferredStyle: .alert)
        alert.addAction(GameAlertAction(title: "Proses Batch", style: .default) { [weak self] _ in
            guard let self = self else { return }
            let ok = self.engine.prosesBatchPks()
            if !ok {
                self.showToast("Silo kosong -- kirim TBS ke TPH dulu")
            } else {
                // Pesan sisa silo -- konsistensi dgn Android (dulu diam saja
                // stlh berhasil, tak menjelaskan kalau masih ada backlog).
                let sisaSilo = self.engine.pksInputSilo()
                if sisaSilo > 0 {
                    self.showToast("Diproses \(batchCap) tandan. Sisa \(Int(sisaSilo)) tandan masih menunggu -- tekan Proses Batch lagi")
                }
            }
            self.refreshTreesAndHud()
        })
        alert.addAction(GameAlertAction(title: "Upgrade", style: .default) { [weak self] _ in
            guard let self = self else { return }
            if !self.engine.upgradePks() { self.showToast("Uang tidak cukup") }
            self.refreshTreesAndHud()
        })
        // Navigasi kamera SINEMATIK ke lokasi bangunan PKS -- diminta pengguna
        // ("buatkan ui untuk kesana"). Ditambahkan sbg action BARU (bukan
        // mengganti "Tutup" spt Android -- GameAlertController di sini tak
        // terbatas 3 slot spt UIAlertController/AlertDialog native).
        alert.addAction(GameAlertAction(title: "📍 Lihat PKS", style: .default) { [weak self] _ in
            guard let self = self else { return }
            // Keluar Gameplay Mode DULU kalau aktif -- kamera Management Mode
            // (animateCameraTo(), dipakai di sini) DI-OVERRIDE total oleh
            // sistem kamera third-person selama Gameplay Mode aktif, navigasi
            // TAK akan terlihat efeknya kalau tak dikeluarkan dulu (identik
            // logic tapModeToggle() -- reset joystick/interactBtn spy UI
            // tetap konsisten).
            if self.engine.getGameplayModeActive() {
                self.engine.setGameplayModeActive(false)
                self.modeToggleBtn.setTitle("🗺️ Mode: Manajemen", for: .normal)
                self.joyBase.isHidden = true
                self.joyKnob.isHidden = true
                self.interactBtn.isHidden = true
                self.joystickDirX = 0; self.joystickDirZ = 0
            }
            self.estateViewActive = false // keluar Estate View jika aktif
            self.animateCameraTo(self.engine.pksWorldX(), self.engine.pksWorldZ())
            self.showToast("Kamera menuju PKS")
        })
        alert.addAction(GameAlertAction(title: "Tutup", style: .cancel))
        present(alert, animated: true)
    }

    @objc private func tapHr() {
        showHrTreeDialog()
    }

    // BUG UX diperbaiki (fitur baru diminta pengguna): "Buatkan tampilan
    // SDM menjadi bentuk treeview (bagan) menunjukkan jenjang" --
    // SEBELUMNYA cuma action sheet list datar, TAK menunjukkan HIERARKI
    // sama sekali. Diganti HrTreeView (custom UIView, Core Graphics) di
    // dalam GameAlertController.customContentView, dibungkus UIScrollView
    // (bagan 7 baris bisa lebih tinggi dari layar) -- identik Android,
    // lihat catatan lengkap struktur DAG di HrTreeView.swift. Fungsi
    // TERPISAH (bukan inline di tapHr()) supaya bisa dipanggil ULANG
    // setelah rekrut (refresh bagan dgn count/status baru).
    private func showHrTreeDialog() {
        let infos = engine.hrLevelInfos()
        let dailySalary = engine.dailySalary()
        let efficiency = engine.hrEfficiency()
        let summary = "Total gaji Rp \(Int(dailySalary))/hari\nEfisiensi \(Int(efficiency*100))% -- mempercepat waktu kerja lapangan (panen/pupuk/semprot/dst)"

        let treeView = HrTreeView()
        treeView.translatesAutoresizingMaskIntoConstraints = false
        treeView.infos = infos
        treeView.onNodeTap = { [weak self] i in
            guard let self = self else { return }
            if !i.recruitable {
                self.showToast(!i.underMax ? "\(i.name): sudah maksimum" : "\(i.name) butuh: \(i.prereqDesc)")
                return
            }
            // BUG diperbaiki (dilaporkan pengguna, runtime error UIKit):
            // "Attempt to present GameAlertController... which is already
            // presenting GameAlertController". AKAR MASALAH: confirmAlert
            // SEBELUMNYA di-present LANGSUNG dari `self` (GameViewController)
            // SEMENTARA `self` MASIH sedang mempresent dialog SDM (`alert`,
            // di bawah) -- UIKit TAK MENGIZINKAN 1 view controller
            // mempresent 2 alert sekaligus dari root yg sama. Diperbaiki:
            // dismiss dialog SDM DULU (dgn completion handler, memastikan
            // dismissal SELESAI sebelum present berikutnya dimulai -- bukan
            // cuma memanggil present() segera setelah dismiss() tanpa
            // menunggu), BARU tampilkan konfirmasi. Dialog SDM dibuka ULANG
            // setelah rekrut ATAU batal (bukan cuma setelah rekrut spt
            // sebelumnya) -- konsisten, pemain tak "kehilangan konteks"
            // kembali ke layar kosong tanpa dialog apa pun.
            self.dismiss(animated: true) {
                let confirmAlert = GameAlertController(title: "\(i.icon) \(i.name) (\(i.count)x)",
                    message: "\(i.desc)\n\nRp \(Int(i.cost)) (gaji Rp \(Int(i.salary))/hari)", preferredStyle: .alert)
                confirmAlert.addAction(GameAlertAction(title: "Rekrut", style: .default) { _ in
                    let ok = self.engine.rekrutLevel(i.key)
                    self.showToast(ok ? "\(i.name) berhasil direkrut!" : "Rekrut \(i.name) gagal -- uang tidak cukup")
                    self.refreshTreesAndHud()
                    self.showHrTreeDialog() // buka ulang -- bagan diperbarui dgn count/status baru
                })
                confirmAlert.addAction(GameAlertAction(title: "Batal", style: .cancel) { _ in
                    self.showHrTreeDialog() // buka ulang jg meski dibatalkan -- konsisten, konteks tak hilang
                })
                self.present(confirmAlert, animated: true)
            }
        }
        let scroll = UIScrollView()
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.addSubview(treeView)
        // BUG diperbaiki (dilaporkan pengguna: "mungkin saja punya masalah
        // yang sama" -- konfirmasi, YA, DAN lebih rumit). AKAR MASALAH:
        // scroll (pembungkus HrTreeView, jadi customContentView dialog SDM)
        // SEBELUMNYA punya height constraint FIXED (equalToConstant, priority
        // REQUIRED default) -- kombinasi dgn perbaikan bug block selector
        // sebelumnya (panel dibatasi max 85% tinggi layar) bisa menciptakan
        // KONFLIK CONSTRAINT NYATA: kalau title+message+scroll(fixed 420pt)+
        // tombol Tutup MELEBIHI 85% tinggi layar (mis. bagan SDM py 7 baris
        // jenjang di layar landscape kecil), Auto Layout TAK BISA memenuhi
        // semua constraint sekaligus (outerStack butuh EQUAL ruang ke
        // panel.top/bottom, TAPI scroll yg FIXED 420pt tak mau mengalah) --
        // hasilnya PERILAKU TAK TERDUGA (constraint "dilanggar" scr diam2
        // oleh UIKit, bisa berupa overlap/terpotong, PERSIS spt screenshot:
        // node terakhir + tombol Tutup nyaris tak terjangkau). SEBELUM
        // perbaikan panel 85% pun, masalah SUDAH ADA scr berbeda bentuk:
        // panel tak dibatasi sama sekali, jadi kalau bagan lebih tinggi dari
        // layar, bagian bawah panel (termasuk tombol Tutup) fisik berada DI
        // LUAR layar, tak terjangkau sentuhan sama sekali.
        //
        // Diperbaiki: height constraint scroll SEKARANG priority .defaultHigh
        // (BUKAN .required) -- Auto Layout BOLEH "melanggar"/mengecilkan
        // constraint ini scr GRACEFUL (tanpa conflict warning) saat ruang
        // tak cukup, MENGALAH ke batasan panel (85% tinggi layar, priority
        // .required/default) yg py priority lebih tinggi. compression
        // resistance jg diturunkan eksplisit -- konsisten pola actionsScroll
        // di GameAlertController (lihat catatan lengkap di sana).
        let scrollHeightConstraint = scroll.heightAnchor.constraint(equalToConstant: min(treeView.intrinsicContentSize.height, 420))
        scrollHeightConstraint.priority = .defaultHigh
        scroll.setContentCompressionResistancePriority(.defaultLow, for: .vertical)
        NSLayoutConstraint.activate([
            treeView.leadingAnchor.constraint(equalTo: scroll.leadingAnchor),
            treeView.trailingAnchor.constraint(equalTo: scroll.trailingAnchor),
            treeView.topAnchor.constraint(equalTo: scroll.topAnchor),
            treeView.bottomAnchor.constraint(equalTo: scroll.bottomAnchor),
            treeView.widthAnchor.constraint(equalTo: scroll.widthAnchor),
            treeView.heightAnchor.constraint(equalToConstant: treeView.intrinsicContentSize.height),
            scrollHeightConstraint,
        ])

        let alert = GameAlertController(title: "👤 SDM", message: summary, preferredStyle: .alert)
        alert.customContentView = scroll
        alert.addAction(GameAlertAction(title: "Tutup", style: .cancel))
        present(alert, animated: true)
    }

    @objc private func saveGameToFile() {
        let json = engine.saveJson()
        try? json.write(to: saveFileURL, atomically: true, encoding: .utf8)
    }
    @objc private func pauseBgmForBackground() { soundManager.onPause() }
    @objc private func resumeBgmForForeground() { soundManager.onResume() }
    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        saveGameToFile() // lapis pengaman tambahan -- redundan dgn observer NotificationCenter di atas
    }
    private func loadGameFromFile() {
        guard let json = try? String(contentsOf: saveFileURL, encoding: .utf8) else { return }
        _ = engine.loadJson(json) // gagal (save rusak/tak terbaca) -- lanjut dgn newGame() default, aman
    }
    deinit {
        NotificationCenter.default.removeObserver(self)
        hudTimer?.invalidate()
    }

    @objc private func tapLand() {
        let totalHa = engine.totalHa()
        let totalPokok = engine.totalPokok()
        let pricePerHa = engine.haPrice()
        let blockCount = engine.blockSummaries().count
        let msg = "Total lahan: \(totalHa) Ha (\(totalPokok) pokok)\nJumlah Block: \(blockCount)\n" +
            "Harga beli 1 Ha berikutnya: Rp \(Int(pricePerHa))\n\n" +
            "Membeli 1 Ha akan membuka Block baru berisi 143 pokok sungguhan " +
            "(bukan cuma angka), ditempatkan di area terpisah dari block yg sudah ada."
        let alert = GameAlertController(title: "🌍 Lahan", message: msg, preferredStyle: .alert)
        alert.addAction(GameAlertAction(title: "Beli 1 Ha", style: .default) { [weak self] _ in
            guard let self = self else { return }
            if !self.engine.beliHa(1.0) {
                self.showToast("Gagal -- cek uang atau kapasitas afdeling")
            }
            self.refreshTreesAndHud()
        })
        alert.addAction(GameAlertAction(title: "Buka Afdeling Baru", style: .default) { [weak self] _ in
            guard let self = self else { return }
            if !self.engine.bukaAfdelingBaru() {
                self.showToast("Gagal -- cek syarat (Asisten Afdeling) atau uang")
            }
            self.refreshTreesAndHud()
        })
        alert.addAction(GameAlertAction(title: "Tutup", style: .cancel))
        present(alert, animated: true)
    }

    @objc private func tapDevRandomize() {
        engine.devRandomizeConditions()
        refreshTreesAndHud()
    }

    @objc private func closeTreeInspector() {
        inspectorTree = nil
        inspectorCloseBtn?.isHidden = true
        inspectorNavUp?.isHidden = true
        inspectorNavDown?.isHidden = true
        inspectorNavLeft?.isHidden = true
        inspectorNavRight?.isHidden = true
        navRelease()
        // Kembalikan radial menu (BUKAN actionStack lama yg sudah tak dipakai)
        // -- HANYA kalau pohon masih terpilih (celah lama: dulu tak pernah
        // dikembalikan sama sekali, bikin tombol aksi terlihat "kurang
        // responsif" krn sebenarnya invisible, bukan macet).
        if selectedTreeId >= 0, let t = lastRenderedTree { buildRadialMenu(for: t) }
    }

    // MARK: - Polling HUD & event (toast)

    private func startPollingLoop() {
        hudTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            self?.refreshTreesAndHud()
            self?.drainEvents()
            if self?.logVisible == true { self?.refreshActivityLog() }
        }
    }

    private func refreshTreesAndHud() {
        // BUG performa BESAR diperbaiki (dilaporkan pengguna: "sistem hampir
        // tidak bisa digunakan jika memliki kebun di atas 30 ha") -- identik
        // Android, lihat catatan lengkap di sawit_jni.cpp. Sebelumnya
        // engine.trees() membangun NSArray<SawitTreeView*> utk SEMUA pohon
        // dari SEMUA block (alloc+init Objective-C object per pohon) --
        // dgn 30ha (~4290 pohon) tiap 250ms (Timer.scheduledTimer, MAIN
        // RUNLOOP -- thread yg sama memproses touch event), overhead ini
        // memblokir main thread cukup lama utk bikin tap tombol (termasuk
        // dialog SDM) terasa tak responsif. Sekarang per-block, O(143)
        // TETAP KONSTAN terlepas berapa ha dimiliki.
        //
        // Celah kecil diperbaiki (ditemukan saat audit ini, bukan regresi
        // baru): validasi "currentBlockIndex di luar range" (mis. block
        // dihapus) SEBELUMNYA terjadi SETELAH trees=... dipanggil (lihat
        // beberapa baris di bawah) -- dgn treesForBlock() yg BARU (peka
        // thd index tak valid, beda dari trees() lama yg selalu ambil
        // SEMUA block terlepas index), urutan lama bisa bikin `trees`
        // kosong/salah selama 1 siklus polling. Validasi dipindah ke SINI,
        // SEBELUM treesForBlock() dipanggil.
        let allBlocksForValidation = engine.blockSummaries()
        if currentBlockIndex >= allBlocksForValidation.count { currentBlockIndex = 0 }
        trees = engine.trees(forBlock: currentBlockIndex)
        hudMoney.text = "💰 Rp \(Int(engine.money()))"
        let timeIcon: String
        switch engine.timeOfDay() { case 0: timeIcon = "🌅"; case 1: timeIcon = "☀️"; default: timeIcon = "🌙" }
        let rainIcon = engine.isRaining() ? " 🌧️" : ""
        hudDay.text = "📅 Hari \(engine.day()) \(timeIcon)\(rainIcon)"
        hudTph.text = "📦 TPH \(Int(engine.tphStock(currentBlockIndex)))/\(Int(engine.tphCap()))"
        let allBlocks = allBlocksForValidation // sudah dihitung & divalidasi di atas, hindari panggilan blockSummaries() ganda
        if !allBlocks.isEmpty {
            let b = allBlocks[currentBlockIndex]
            // BUG UX diperbaiki (review eksternal, prioritas Tinggi): "Top
            // status bar: terlalu banyak angka dalam satu baris -> sulit
            // dipindai cepat". Identik Android -- lihat catatan lengkap di
            // MainActivity.kt. Detail lengkap dipindah ke showBlockSelector().
            let anyIssue = b.lowNutritionCount > 0 || b.hamaCount > 0 || b.ganodermaCount > 0
            var line = "\(b.statusEmoji) \(b.name) · \(b.treeCount) pokok"
            if anyIssue { line += " ⚠️" }
            if b.readyToHarvestCount > 0 { line += " 🌾\(b.readyToHarvestCount)" }
            line += "  ▾"
            hudBlock.text = line
        }
        if selectedTreeId >= 0, let t = trees.first(where: { $0.treeId == selectedTreeId }) {
            let snap = (id: t.treeId, health: t.health, ffb: t.ffb, frond: t.frond, hasTbsReady: t.hasTbsReady)
            if lastRenderedTreeSnapshot == nil || snap != lastRenderedTreeSnapshot! {
                renderActionPanel(for: t)
                lastRenderedTreeSnapshot = snap
            }
            // Countdown/progress bar TBS -- fitur baru diminta pengguna.
            // DIPANGGIL TIAP POLLING (bukan cuma saat snapshot berubah) --
            // identik Android, lihat catatan lengkap di updateFfbCountdown().
            updateFfbCountdown(treeId: t.treeId)
        }
    }

    private func drainEvents() {
        for line in engine.pollEventsRaw() {
            let parts = line.components(separatedBy: "|")
            guard parts.count >= 2, let type = Int(parts[0]) else { continue }
            // EventType: 0=Toast,1=FlyMoney,2=TreeChanged,3=HudChanged,
            // 4=ScreenChanged,5=LogOnly. LogOnly SENGAJA tak masuk kondisi di
            // bawah (bukan 0/1) -- identik Android, lihat catatan lengkap di
            // MainActivity.kt/Engine::emit.
            if type == 0 || type == 1 {
                showToast(parts[1])
                // FlyMoney (uang masuk) SELALU cash.wav -- Toast biasa pilih SFX
                // kontekstual dari isi teks (lihat SoundManager.playForToast).
                if type == 1 { soundManager.playCash() } else { soundManager.playForToast(parts[1]) }
            }
        }
    }

    // Toast minimal native (UILabel yang fade-out sendiri) — silakan ganti gaya sesuai selera.
    // Antrian toast SEDERHANA -- BUG diperbaiki: dulu setiap showToast()
    // langsung membuat label BARU di posisi SAMA PERSIS tanpa antrian sama
    // sekali. Kalau 2+ toast muncul hampir bersamaan (mis. toast "Rekrut
    // gagal: prasyarat blm terpenuhi" BERSAMAAN dgn toast auto-assign Mandor
    // yg gagal pupuk krn uang kurang UNTUK PUPUK, tak berkaitan dgn rekrut
    // SDM sama sekali), KEDUANYA saling tumpang tindih visual di posisi yg
    // SAMA -- pesan yg SEBENARNYA relevan jadi tak terbaca, pemain cuma
    // lihat toast LAIN yg kebetulan muncul bersamaan (dilaporkan pengguna:
    // "log uang tidak mencukupi" muncul saat rekrut Asisten Afdeling,
    // padahal prasyaratnya BUKAN uang -- butuh Mandor Besar & Krani Kepala
    // dulu). Android's native Toast SUDAH punya antrian bawaan dr sistem OS
    // (inilah kenapa bug ini spesifik iOS) -- di sini diimplementasikan
    // manual: toast BARU masuk antrian, ditampilkan SATU PER SATU berurutan.
    private var toastQueue: [String] = []
    private var toastShowing = false

    private func showToast(_ text: String) {
        toastQueue.append(text)
        processToastQueue()
    }

    private func processToastQueue() {
        guard !toastShowing, !toastQueue.isEmpty else { return }
        toastShowing = true
        let text = toastQueue.removeFirst()
        displayToastLabel(text) { [weak self] in
            self?.toastShowing = false
            self?.processToastQueue() // lanjut ke antrian berikutnya (kalau ada)
        }
    }

    private func displayToastLabel(_ text: String, completion: @escaping () -> Void) {
        let lbl = UILabel()
        lbl.text = text
        lbl.textColor = .white
        lbl.font = .boldSystemFont(ofSize: 12)
        lbl.backgroundColor = UIColor(white: 0.08, alpha: 0.9)
        lbl.textAlignment = .center
        lbl.layer.cornerRadius = 12
        lbl.clipsToBounds = true
        lbl.numberOfLines = 1
        lbl.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(lbl)
        NSLayoutConstraint.activate([
            lbl.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            lbl.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 40),
            lbl.widthAnchor.constraint(lessThanOrEqualTo: view.widthAnchor, constant: -40),
            lbl.heightAnchor.constraint(equalToConstant: 26),
        ])
        UIView.animate(withDuration: 0.25, animations: { lbl.alpha = 1 }, completion: { _ in
            UIView.animate(withDuration: 0.3, delay: 1.0, options: [], animations: { lbl.alpha = 0 }, completion: { _ in
                lbl.removeFromSuperview()
                completion()
            })
        })
    }
}

// MARK: - GameAlertAction & GameAlertController
// Pengganti UIAlertController/UIAlertAction bergaya GAME (panel gelap
// semi-transparan, sudut membulat, tombol putih tanpa border) -- mengatasi
// keluhan review eksternal: "Pop up menu masih belum menggunakan custom
// style game yang sudah dibangun". UIAlertController TIDAK BISA direstyle
// scr resmi oleh Apple (tak ada API publik utk ubah warna/background --
// hack via KVC/private API berisiko crash lintas versi iOS & ditolak App
// Store review), beda dgn Android yg mendukung dialog theme resmi.
// API SENGAJA meniru UIAlertController/UIAlertAction persis (title:,
// message:, preferredStyle:, addAction, style: .default/.cancel) supaya
// SEMUA 21 pemanggilan yg sudah ada di file ini cukup diganti nama class
// (UIAlertController->GameAlertController, UIAlertAction->GameAlertAction)
// TANPA menulis ulang logic apa pun.
struct GameAlertAction {
    enum Style { case `default`, cancel, destructive }
    let title: String
    let style: Style
    let handler: ((GameAlertAction) -> Void)?
    init(title: String, style: Style, handler: ((GameAlertAction) -> Void)? = nil) {
        self.title = title; self.style = style; self.handler = handler
    }
}

final class GameAlertController: UIViewController {
    enum Style { case alert, actionSheet }
    private let alertTitleText: String?
    private let alertMessageText: String?
    private let style: Style
    private var actions: [GameAlertAction] = []
    private var textFieldConfigured: ((UITextField) -> Void)?
    var textFields: [UITextField] { textFieldStack }
    private var textFieldStack: [UITextField] = []
    // View konten kustom OPSIONAL (mis. SeekBar/SegmentedControl pengaturan)
    // -- disisipkan di stack, SETELAH message/textField, SEBELUM actions.
    // Aditif: dialog lama yg tak set property ini TAK terpengaruh sama
    // sekali (nil, tak ditambahkan ke stack). Fitur baru diminta pengguna
    // ("tambahkan pengaturan sensivitas dan grafik") -- dipakai showSettingsDialog().
    var customContentView: UIView?

    init(title: String?, message: String?, preferredStyle: Style) {
        self.alertTitleText = title
        self.alertMessageText = message
        self.style = preferredStyle
        super.init(nibName: nil, bundle: nil)
        modalPresentationStyle = .overFullScreen
        modalTransitionStyle = .crossDissolve
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) tak dipakai") }

    func addAction(_ action: GameAlertAction) { actions.append(action) }
    // Dukungan textField SEDERHANA (dipakai 1 tempat di file ini utk input teks)
    func addTextField(configurationHandler: ((UITextField) -> Void)? = nil) {
        textFieldConfigured = configurationHandler
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = UIColor(white: 0, alpha: 0.55) // overlay gelap penuh layar, konsisten fadeOverlay Gameplay Mode

        let panel = UIView()
        panel.backgroundColor = UIColor(white: 0.10, alpha: 0.92)
        panel.layer.cornerRadius = 14
        panel.layer.borderWidth = 1
        panel.layer.borderColor = UIColor(white: 1, alpha: 0.25).cgColor
        panel.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(panel)

        // BUG diperbaiki (dilaporkan pengguna: "blok tidak dapat di pilih
        // karena jumlah lahan di atas 30"). AKAR MASALAH: SEBELUMNYA `stack`
        // (SEMUA tombol -- 1 per block, mis. showBlockSelector()) diletakkan
        // LANGSUNG di dalam `panel` TANPA UIScrollView sama sekali, dan
        // `panel` TIDAK PUNYA batasan tinggi maksimal (cuma di-constrain ke
        // centerYAnchor) -- dgn 30+ block (tiap tombol >=44pt + spacing
        // 10pt), tinggi total stack JAUH melebihi tinggi layar perangkat
        // manapun. Tombol yg jatuh di luar batas layar (atas/bawah) TAK
        // BISA DISENTUH SAMA SEKALI, dan TAK ADA cara scroll utk
        // menjangkaunya -- persis penyebab "tidak bisa dipilih".
        //
        // Diperbaiki: outerStack (title/message/textField/customContentView
        // -- SELALU pendek, tetap TANPA scroll) TERPISAH dari actionsScroll
        // (HANYA tombol actions, BISA banyak/1 per block, dibungkus
        // UIScrollView). panel dibatasi max 85% tinggi layar (lessThanOrEqualTo)
        // -- kalau konten LEBIH PENDEK dari itu, panel tetap melar sesuai
        // konten spt sebelumnya (tak ada regresi utk dialog kecil yg sudah
        // banyak dipakai), kalau LEBIH PANJANG, actionsScroll yg menangani
        // sisanya via scroll (bisa dijangkau SEMUA tombol, brp pun banyaknya).
        let outerStack = UIStackView()
        outerStack.axis = .vertical
        outerStack.spacing = 10
        outerStack.translatesAutoresizingMaskIntoConstraints = false
        panel.addSubview(outerStack)

        if let t = alertTitleText, !t.isEmpty {
            let l = UILabel(); l.text = t; l.textColor = .white
            l.font = .boldSystemFont(ofSize: 16); l.textAlignment = .center
            l.numberOfLines = 0
            outerStack.addArrangedSubview(l)
        }
        if let m = alertMessageText, !m.isEmpty {
            let l = UILabel(); l.text = m; l.textColor = UIColor(white: 0.9, alpha: 1)
            l.font = .systemFont(ofSize: 13); l.textAlignment = .center
            l.numberOfLines = 0
            outerStack.addArrangedSubview(l)
        }
        if let cfg = textFieldConfigured {
            let tf = UITextField()
            tf.borderStyle = .roundedRect
            tf.backgroundColor = UIColor(white: 1, alpha: 0.9)
            cfg(tf)
            textFieldStack.append(tf)
            outerStack.addArrangedSubview(tf)
        }
        if let cv = customContentView {
            outerStack.addArrangedSubview(cv)
        }

        // ScrollView KHUSUS utk tombol actions -- lihat catatan lengkap di
        // atas. actionsStack DI DALAM scrollView, width DIKUNCI ke
        // scrollView (supaya stack vertikal tak melebar horizontal, CUMA
        // tinggi yg discroll), height TIDAK dikunci (biar scrollView
        // hitung contentSize dari tinggi asli actionsStack).
        let actionsScroll = UIScrollView()
        actionsScroll.translatesAutoresizingMaskIntoConstraints = false
        actionsScroll.showsVerticalScrollIndicator = true
        // Priority EKSPLISIT -- UIScrollView TAK punya intrinsicContentSize yg
        // jelas utk UIStackView (outerStack) hitung otomatis; dipastikan
        // EKSPLISIT di sini spy actionsScroll TETAP UTUH/terjangkau saat ruang
        // terbatas (panel dibatasi 85% tinggi layar, lihat di bawah) --
        // compression resistance DINAIKKAN ke .defaultHigh (BUKAN .defaultLow
        // spt versi awal) krn tombol aksi (mis. "Tutup") HARUS SELALU
        // terlihat/terjangkau (satu2nya cara menutup dialog) -- BEDA dari
        // customContentView (mis. treeView SDM, lihat showHrTreeDialog())
        // yg AMAN dikompres LEBIH DULU krn sudah py UIScrollView internal
        // sendiri utk mengkompensasi (bisa di-scroll utk lihat konten
        // terpotong, TIDAK spt actionsScroll yg kalau terkompres/hilang
        // TAK ADA cara lain menutup dialog sama sekali).
        actionsScroll.setContentHuggingPriority(.defaultLow, for: .vertical)
        actionsScroll.setContentCompressionResistancePriority(.defaultHigh, for: .vertical)
        let actionsStack = UIStackView()
        actionsStack.axis = .vertical
        actionsStack.spacing = 10
        actionsStack.translatesAutoresizingMaskIntoConstraints = false
        actionsScroll.addSubview(actionsStack)
        outerStack.addArrangedSubview(actionsScroll)

        for action in actions {
            let b = UIButton(type: .system)
            b.setTitle(action.title, for: .normal)
            let isCancel = (action.style == .cancel)
            b.setTitleColor(isCancel ? UIColor(white: 0.75, alpha: 1) : UIColor(white: 1, alpha: 1), for: .normal)
            b.titleLabel?.font = isCancel ? .systemFont(ofSize: 14) : .boldSystemFont(ofSize: 14)
            b.backgroundColor = isCancel ? UIColor(white: 1, alpha: 0.08) : UIColor(white: 1, alpha: 0.16)
            b.layer.cornerRadius = 8
            b.contentEdgeInsets = UIEdgeInsets(top: 10, left: 14, bottom: 10, right: 14)
            b.heightAnchor.constraint(greaterThanOrEqualToConstant: 44).isActive = true // target sentuh min Apple HIG
            b.addAction(UIAction { [weak self] _ in
                self?.dismiss(animated: true) { action.handler?(action) }
            }, for: .touchUpInside)
            actionsStack.addArrangedSubview(b)
        }

        // BUG KRITIS diperbaiki (dilaporkan pengguna: dialog konfirmasi
        // rekrut SDM menampilkan judul+deskripsi dgn benar, TAPI tombol
        // "Rekrut"/"Batal" SAMA SEKALI TAK TERLIHAT & tak bisa disentuh --
        // "tidak ada aksi dan x close"). AKAR MASALAH: actionsScroll
        // (UIScrollView) SEBELUMNYA TAK PERNAH punya height constraint
        // eksplisit sama sekali -- bergantung penuh pada Auto Layout utk
        // "menebak" tinggi dari konten (via contentLayoutGuide), TAPI
        // UIScrollView TAK PUNYA intrinsicContentSize yg berguna utk
        // UIStackView (outerStack) hitung otomatis -- hasilnya actionsScroll
        // KOLAPS ke tinggi 0 (SEMUA tombol di dalamnya scr teknis ADA di
        // hierarki view, TAPI tak terlihat & tak bisa disentuh krn frame-nya
        // nol tinggi). Title/message TAK terpengaruh krn keduanya UILabel
        // biasa dgn intrinsicContentSize normal, BUKAN discroll.
        //
        // Diperbaiki: hitung tinggi IDEAL scr eksplisit dari jumlah actions
        // (48pt/tombol -- sedikit di atas minimum 44pt Apple HIG, beri
        // margin utk label) + spacing antar tombol, lalu terapkan sbg height
        // constraint PRIORITY .defaultHigh (BUKAN .required) -- utk dialog
        // biasa (2-3 tombol, mis. Rekrut/Batal) ini SELALU muat penuh dlm
        // batas panel 85% tinggi layar, TAK PERNAH terkompresi. Utk kasus
        // ekstrem (block selector 30+ tombol), priority fleksibel ini
        // MEMBOLEHKAN Auto Layout mengecilkan scroll scr graceful spy muat
        // dlm batas panel (TANPA conflict warning), dgn scroll internal
        // tetap berfungsi menjangkau SEMUA tombol via geser.
        let idealActionsHeight = CGFloat(actions.count) * 48.0 + CGFloat(max(0, actions.count - 1)) * actionsStack.spacing
        let actionsHeightConstraint = actionsScroll.heightAnchor.constraint(equalToConstant: idealActionsHeight)
        actionsHeightConstraint.priority = .defaultHigh

        NSLayoutConstraint.activate([
            panel.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            panel.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            panel.widthAnchor.constraint(equalToConstant: 300),
            panel.leadingAnchor.constraint(greaterThanOrEqualTo: view.leadingAnchor, constant: 20),
            // BUG diperbaiki -- batas tinggi maksimal panel (85% tinggi layar),
            // lihat catatan lengkap di atas.
            panel.heightAnchor.constraint(lessThanOrEqualTo: view.heightAnchor, multiplier: 0.85),
            outerStack.topAnchor.constraint(equalTo: panel.topAnchor, constant: 18),
            outerStack.bottomAnchor.constraint(equalTo: panel.bottomAnchor, constant: -18),
            outerStack.leadingAnchor.constraint(equalTo: panel.leadingAnchor, constant: 18),
            outerStack.trailingAnchor.constraint(equalTo: panel.trailingAnchor, constant: -18),
            // actionsStack: lebar dikunci ke actionsScroll (stack vertikal
            // murni, tak melebar horizontal), tinggi/lebar SISANYA (top/
            // bottom/leading/trailing) dikunci ke contentLayoutGuide --
            // ini yg menentukan contentSize scrollView utk di-scroll.
            actionsStack.topAnchor.constraint(equalTo: actionsScroll.contentLayoutGuide.topAnchor),
            actionsStack.bottomAnchor.constraint(equalTo: actionsScroll.contentLayoutGuide.bottomAnchor),
            actionsStack.leadingAnchor.constraint(equalTo: actionsScroll.contentLayoutGuide.leadingAnchor),
            actionsStack.trailingAnchor.constraint(equalTo: actionsScroll.contentLayoutGuide.trailingAnchor),
            actionsStack.widthAnchor.constraint(equalTo: actionsScroll.frameLayoutGuide.widthAnchor),
            actionsHeightConstraint,
        ])
    }
}

// MARK: - Izinkan pinch (zoom) & rotate (putar 360°) dikenali BERSAMAAN
// Tanpa ini, iOS secara default akan memblokir salah satu begitu yang lain
// mulai dikenali (mis. pinch-zoom sambil memutar 2 jari jadi tersendat-sendat).
extension GameViewController: UIGestureRecognizerDelegate {
    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer,
                            shouldRecognizeSimultaneouslyWith otherGestureRecognizer: UIGestureRecognizer) -> Bool {
        return true
    }
}
