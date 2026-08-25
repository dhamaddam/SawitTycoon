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
    private var eaglContext: EAGLContext!
    private var lastUpdateTime: CFTimeInterval = 0

    private var panX: Float = 0
    private var panZ: Float = 0
    private var camDist: Float = 44
    // Rotasi kamera sekitar sumbu vertikal (radian) — fitur lihat 360°.
    // Default 45° = sudut isometrik klasik, sama spt sebelum fitur ini ada.
    private var camYaw: Float = 0.7854

    private var selectedTreeId: Int = -1
    private var trees: [SawitTreeView] = []
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

    // --- Log Aktivitas ---
    private let logButton = UIButton(type: .system)
    private let logOverlay = UIView()
    private let logTextView = UITextView()

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
        setupBulkActionUi()
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
    @objc func update() {
        let now = CACurrentMediaTime()
        var dt = lastUpdateTime == 0 ? 0 : (now - lastUpdateTime)
        lastUpdateTime = now
        if dt > 0.1 { dt = 0.1 } // clamp, hindari lonjakan simulasi kalau app sempat freeze
        engine.tick(dt) // simulasi tetap jalan; cuma TAMPILAN yg beda di mode inspector
        if inspectorTree == nil {
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
        // Hit-test langsung di RUANG LAYAR — pendekatan lama (screenToWorldX pada
        // asumsi y=0) gagal kalau yg diketuk adalah pelepah/bagian atas pohon,
        // karena posisinya di layar lebih tinggi dari dasar pohon dan salah
        // dipetakan ke lokasi tanah yg jauh dari pohon aslinya.
        let p = g.location(in: view)
        let scale = Float(view.contentScaleFactor)
        let screenX = Float(p.x) * scale, screenY = Float(p.y) * scale

        var best: SawitTreeView?
        var bestDist: Float = .greatestFiniteMagnitude
        for t in trees {
            let d = engine.hitTestDistanceScreenX(screenX, screenY: screenY, treeX: t.x, treeZ: t.z, ageYears: t.ageYears)
            if d < bestDist { bestDist = d; best = t }
        }
        if let b = best, bestDist < 80 { // ambang dlm PIKSEL layar, bukan unit dunia lagi
            selectedTreeId = b.treeId
            lastRenderedTreeSnapshot = (id: b.treeId, health: b.health, ffb: b.ffb, frond: b.frond, hasTbsReady: b.hasTbsReady)
            renderActionPanel(for: b)
        } else {
            selectedTreeId = -1
            lastRenderedTreeSnapshot = nil
            actionStack.isHidden = true
            panelTitle.text = "Ketuk sebuah pokok"
        }
    }

    @objc private func onPan(_ g: UIPanGestureRecognizer) {
        let t = g.translation(in: view)
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
        panX += dx
        panZ += dz
        g.setTranslation(.zero, in: view)
    }

    @objc private func onPinch(_ g: UIPinchGestureRecognizer) {
        if inspectorTree != nil { return } // inspector aktif -> abaikan zoom kebun
        camDist = min(70, max(22, camDist / Float(g.scale)))
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
        NSLayoutConstraint.activate([
            devBtn.topAnchor.constraint(equalTo: hudBlock.bottomAnchor, constant: 6),
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
        NSLayoutConstraint.activate([
            hrBtn.topAnchor.constraint(equalTo: landBtn.bottomAnchor, constant: 6),
            hrBtn.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 12),
        ])

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
            scroll.heightAnchor.constraint(equalToConstant: 44),
        ])
        bottomPanel = panel
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
        let closeTap = UITapGestureRecognizer(target: self, action: #selector(toggleLogOverlay))
        logOverlay.addGestureRecognizer(closeTap)
        view.addSubview(logOverlay)
        NSLayoutConstraint.activate([
            logOverlay.topAnchor.constraint(equalTo: view.topAnchor),
            logOverlay.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            logOverlay.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            logOverlay.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])

        logTextView.backgroundColor = .clear
        logTextView.textColor = UIColor(white: 0.95, alpha: 1)
        logTextView.font = .systemFont(ofSize: 13)
        logTextView.isEditable = false
        logTextView.isSelectable = false
        logTextView.textContainerInset = UIEdgeInsets(top: 60, left: 16, bottom: 24, right: 16)
        logTextView.translatesAutoresizingMaskIntoConstraints = false
        logOverlay.addSubview(logTextView)
        NSLayoutConstraint.activate([
            logTextView.topAnchor.constraint(equalTo: logOverlay.topAnchor),
            logTextView.leadingAnchor.constraint(equalTo: logOverlay.leadingAnchor),
            logTextView.trailingAnchor.constraint(equalTo: logOverlay.trailingAnchor),
            logTextView.bottomAnchor.constraint(equalTo: logOverlay.bottomAnchor),
        ])

        // Tombol tutup eksplisit -- UITextView yg scrollable bisa "mencuri" sentuhan
        // dari gesture ketuk-di-mana-saja di atas, jadi ini jalan tutup yg pasti bekerja.
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
    private func setupBulkActionUi() {
        let bar = UIStackView()
        bar.axis = .horizontal
        bar.distribution = .fillEqually
        bar.spacing = 4
        bar.backgroundColor = UIColor(red: 0.102, green: 0.173, blue: 0.082, alpha: 0.85)
        bar.isLayoutMarginsRelativeArrangement = true
        bar.layoutMargins = UIEdgeInsets(top: 6, left: 8, bottom: 6, right: 8)
        bar.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(bar)
        NSLayoutConstraint.activate([
            bar.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            bar.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            bar.bottomAnchor.constraint(equalTo: bottomPanel.topAnchor),
        ])

        func addBulkBtn(_ title: String, _ selector: Selector) {
            let b = UIButton(type: .system)
            b.setTitle(title, for: .normal)
            b.setTitleColor(.white, for: .normal)
            b.titleLabel?.font = .systemFont(ofSize: 10)
            b.titleLabel?.numberOfLines = 2
            b.titleLabel?.textAlignment = .center
            b.backgroundColor = UIColor(white: 1, alpha: 0.12)
            b.layer.cornerRadius = 6
            b.addTarget(self, action: selector, for: .touchUpInside)
            bar.addArrangedSubview(b)
        }
        addBulkBtn("🪓 Panen\nSemua", #selector(tapPanenSemua))
        addBulkBtn("🚚 Angkut\nSemua", #selector(tapAngkutSemua))
        addBulkBtn("🧪 Pupuk\nSemua", #selector(tapPupukSemua))
        addBulkBtn("🐛 Semprot\nSemua", #selector(tapPestisidaSemua))
        addBulkBtn("🍄 Obati\nSemua", #selector(tapFungisidaSemua))
    }

    // Target-action terpisah per jenis (bukan closure capturing self) — sama
    // spt pola tombol aksi pohon di atas, lebih aman dari retain-cycle.
    // Semua sekarang lewat confirmBulkAction -- dialog konfirmasi (jumlah pokok
    // + estimasi biaya dari Block) SEBELUM eksekusi. Review eksternal poin 10:
    // "jangan langsung 700 pokok secara magic, munculkan estimasi dulu".
    @objc private func tapPanenSemua() {
        let n = Int(engine.blockSummaries().first?.readyToHarvestCount ?? 0)
        confirmBulkAction("Panen Semua", previewCount: n, unitPrice: nil) { [weak self] in
            Int(self?.engine.actionPanenSemua() ?? 0)
        }
    }
    @objc private func tapAngkutSemua() {
        let n = Int(engine.blockSummaries().first?.tbsAwaitingPickupCount ?? 0)
        confirmBulkAction("Angkut Semua", previewCount: n, unitPrice: nil) { [weak self] in
            Int(self?.engine.actionAngkutSemua() ?? 0)
        }
    }
    @objc private func tapPupukSemua() {
        guard let b = engine.blockSummaries().first else { return }
        let n = Int(b.treeCount - b.deadCount)
        confirmBulkAction("Pupuk Semua", previewCount: n, unitPrice: engine.pricePupuk()) { [weak self] in
            Int(self?.engine.actionPupukSemua() ?? 0)
        }
    }
    @objc private func tapPestisidaSemua() {
        let n = Int(engine.blockSummaries().first?.hamaCount ?? 0)
        confirmBulkAction("Semprot Semua", previewCount: n, unitPrice: engine.pricePestisida()) { [weak self] in
            Int(self?.engine.actionPestisidaSemua() ?? 0)
        }
    }
    @objc private func tapFungisidaSemua() {
        let n = Int(engine.blockSummaries().first?.ganodermaCount ?? 0)
        confirmBulkAction("Obati Semua", previewCount: n, unitPrice: engine.priceFungisida()) { [weak self] in
            Int(self?.engine.actionFungisidaSemua() ?? 0)
        }
    }

    private func confirmBulkAction(_ label: String, previewCount: Int, unitPrice: Double?, action: @escaping () -> Int) {
        guard previewCount > 0 else {
            showToast("\(label): tidak ada pokok yg memenuhi syarat saat ini")
            return
        }
        let blockName = engine.blockSummaries().first?.name ?? "?"
        let costLine: String
        if let price = unitPrice {
            costLine = "\nEstimasi biaya: Rp \(Int(Double(previewCount) * price))"
        } else {
            costLine = "\nGratis (tanpa biaya)"
        }
        let alert = UIAlertController(
            title: label,
            message: "Block \(blockName): \(previewCount) pokok akan diproses.\(costLine)\n\nLanjutkan?",
            preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Batal", style: .cancel))
        alert.addAction(UIAlertAction(title: "Terapkan", style: .default) { [weak self] _ in
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
        var lines = "📋 LOG AKTIVITAS\n(ketuk di mana saja utk tutup)\n\n"
        if count == 0 {
            lines += "Belum ada aktivitas."
        } else {
            for i in 0..<count {
                lines += "• " + engine.activityLogEntry(i) + "\n"
            }
        }
        logTextView.text = lines
    }

    private func renderActionPanel(for t: SawitTreeView) {
        actionStack.arrangedSubviews.forEach { $0.removeFromSuperview() }
        actionStack.isHidden = false
        let tool = t.isMature ? "Egrek" : "Dodos"

        // Ringkasan kondisi pohon SAAT INI -- sebelumnya cuma judul umur, kondisi
        // kesehatan/TBS cuma implisit lewat tombol aktif/nonaktif (pengguna harus
        // menebak). Sekarang ditampilkan eksplisit spt "popup" kondisi.
        let healthText: String
        switch t.health {
        case 0: healthText = "✅ Sehat"
        case 1: healthText = "🐛 Terserang hama — perlu disemprot"
        case 2: healthText = "🍄 Ganoderma terdeteksi — segera obati!"
        case 3: healthText = "☠️ Pokok mati — perlu ditebang & tanam ulang"
        default: healthText = "?"
        }
        var ffbText = ""
        if t.health != 3 {
            switch t.ffb {
            case 0: ffbText = "🌱 Tandan belum terbentuk"
            case 1: ffbText = "🌾 TBS sedang tumbuh, belum matang"
            case 2: ffbText = "🔴 TBS matang — siap dipanen!"
            case 3: ffbText = "⚠️ TBS lewat matang — segera panen!"
            default: break
            }
        }
        let tbsReadyText = t.hasTbsReady ? "\n📦 TBS di dasar pokok, menunggu diangkut ke TPH" : ""
        panelTitle.numberOfLines = 0
        panelTitle.text = "Pokok #\(t.treeId) — \(t.isMature ? "Dewasa" : "Muda") (\(tool))\n\(healthText)" +
            (ffbText.isEmpty ? "" : "\n\(ffbText)") + tbsReadyText
        lastRenderedTree = t // dipakai tapDetail3D -- butuh SELURUH state pohon, bukan cuma treeId

        addActionButton(title: "✂️ Tunas", enabled: t.frond > 0.35 && t.health != 3, treeId: t.treeId, kind: .tunas)
        addActionButton(title: "🪓 Panen", enabled: (t.ffb == 2 || t.ffb == 3) && t.health != 3, treeId: t.treeId, kind: .panen)
        addActionButton(title: "🚚 Angkut", enabled: t.hasTbsReady, treeId: t.treeId, kind: .angkut)
        addActionButton(title: "🧪 Pupuk", enabled: t.health != 3, treeId: t.treeId, kind: .pupuk)
        addActionButton(title: "🐛 Pestisida", enabled: t.health == 1, treeId: t.treeId, kind: .pestisida)
        addActionButton(title: "🍄 Ganoderma", enabled: t.health == 2, treeId: t.treeId, kind: .fungisida)
        addActionButton(title: "🪵 Tebang", enabled: t.health == 3, treeId: t.treeId, kind: .tebang)
        addActionButton(title: "🔍 Detail 3D", enabled: true, treeId: t.treeId, kind: .detail3d)
    }

    private enum ActionKind { case tunas, panen, angkut, pupuk, pestisida, fungisida, tebang, detail3d }

    private func addActionButton(title: String, enabled: Bool, treeId: Int, kind: ActionKind) {
        let b = UIButton(type: .system)
        b.setTitle(title, for: .normal)
        b.isEnabled = enabled
        b.backgroundColor = UIColor(white: 1, alpha: enabled ? 0.16 : 0.05)
        b.setTitleColor(.white, for: .normal)
        b.layer.cornerRadius = 8
        b.contentEdgeInsets = UIEdgeInsets(top: 8, left: 10, bottom: 8, right: 10)
        b.tag = treeId
        switch kind {
        case .tunas: b.addTarget(self, action: #selector(tapTunas(_:)), for: .touchUpInside)
        case .panen: b.addTarget(self, action: #selector(tapPanen(_:)), for: .touchUpInside)
        case .angkut: b.addTarget(self, action: #selector(tapAngkut(_:)), for: .touchUpInside)
        case .pupuk: b.addTarget(self, action: #selector(tapPupuk(_:)), for: .touchUpInside)
        case .pestisida: b.addTarget(self, action: #selector(tapPestisida(_:)), for: .touchUpInside)
        case .fungisida: b.addTarget(self, action: #selector(tapFungisida(_:)), for: .touchUpInside)
        case .tebang: b.addTarget(self, action: #selector(tapTebang(_:)), for: .touchUpInside)
        case .detail3d: b.addTarget(self, action: #selector(tapDetail3D(_:)), for: .touchUpInside)
        }
        actionStack.addArrangedSubview(b)
    }

    // Target-action terpisah per jenis (bukan closure capturing self) — lebih aman
    // dari retain-cycle & lebih mudah dibaca dibanding closure bersarang di Swift.
    @objc private func tapTunas(_ sender: UIButton) { if engine.actionTunas(sender.tag) { refreshTreesAndHud() } }
    @objc private func tapPanen(_ sender: UIButton) { if engine.actionPanen(sender.tag) { refreshTreesAndHud() } }
    @objc private func tapAngkut(_ sender: UIButton) { if engine.actionAngkut(sender.tag) { refreshTreesAndHud() } }
    @objc private func tapPupuk(_ sender: UIButton) { if engine.actionPupuk(sender.tag) { refreshTreesAndHud() } }
    @objc private func tapPestisida(_ sender: UIButton) { if engine.actionPestisida(sender.tag) { refreshTreesAndHud() } }
    @objc private func tapFungisida(_ sender: UIButton) { if engine.actionFungisida(sender.tag) { refreshTreesAndHud() } }
    @objc private func tapTebang(_ sender: UIButton) { if engine.actionTebang(sender.tag) { refreshTreesAndHud() } }

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
        actionStack.isHidden = true
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
        if blocks.count <= 1 {
            showToast("Baru ada 1 Block -- beli lahan dulu utk buat block baru")
            return
        }
        let alert = UIAlertController(title: "Lompat ke Block", message: nil, preferredStyle: .actionSheet)
        for (idx, b) in blocks.enumerated() {
            var title = "\(b.statusEmoji) Block \(b.name) — \(b.treeCount) pokok"
            if b.readyToHarvestCount > 0 { title += " | \(b.readyToHarvestCount) siap panen" }
            alert.addAction(UIAlertAction(title: title, style: .default) { [weak self] _ in
                guard let self = self else { return }
                self.currentBlockIndex = idx
                // Lompat kamera ke pusat block yg dipilih -- jarak/rotasi
                // dipertahankan spy tetap terasa sbg "geser pandangan".
                self.panX = b.originX
                self.panZ = b.originZ
                self.refreshTreesAndHud()
                self.showToast("Kamera lompat ke Block \(b.name)")
            })
        }
        alert.addAction(UIAlertAction(title: "Tutup", style: .cancel))
        if let popover = alert.popoverPresentationController {
            popover.sourceView = view
            popover.sourceRect = CGRect(x: view.bounds.midX, y: view.bounds.midY, width: 0, height: 0)
        }
        present(alert, animated: true)
    }

    @objc private func tapHr() {
        let infos = engine.hrLevelInfos()
        let alert = UIAlertController(title: "👤 Rekrut SDM", message: nil, preferredStyle: .actionSheet)
        for i in infos {
            let status: String
            if !i.underMax { status = "MAKS TERCAPAI" }
            else if !i.prereqMet { status = "Butuh: \(i.prereqDesc)" }
            else { status = "Rp \(Int(i.cost)) (gaji Rp \(Int(i.salary))/hari)" }
            let title = "\(i.icon) \(i.name) (\(i.count)x) — \(status)"
            alert.addAction(UIAlertAction(title: title, style: .default) { [weak self] _ in
                guard let self = self else { return }
                if !i.recruitable {
                    self.showToast(!i.underMax ? "Sudah maksimum" : "Prasyarat belum terpenuhi: \(i.prereqDesc)")
                    return
                }
                let ok = self.engine.rekrutLevel(i.key)
                self.showToast(ok ? "\(i.name) berhasil direkrut!" : "Gagal -- uang tidak cukup")
                self.refreshTreesAndHud()
            })
        }
        alert.addAction(UIAlertAction(title: "Tutup", style: .cancel))
        // iPad: actionSheet butuh sourceView/popover anchor, kalau tidak app crash
        if let popover = alert.popoverPresentationController {
            popover.sourceView = view
            popover.sourceRect = CGRect(x: view.bounds.midX, y: view.bounds.midY, width: 0, height: 0)
        }
        present(alert, animated: true)
    }

    @objc private func tapLand() {
        let totalHa = engine.totalHa()
        let pricePerHa = engine.haPrice()
        let blockCount = engine.blockSummaries().count
        let msg = "Total lahan: \(totalHa) Ha\nJumlah Block: \(blockCount)\n" +
            "Harga beli 1 Ha berikutnya: Rp \(Int(pricePerHa))\n\n" +
            "Membeli 1 Ha akan membuka Block baru berisi 143 pokok sungguhan " +
            "(bukan cuma angka), ditempatkan di area terpisah dari block yg sudah ada."
        let alert = UIAlertController(title: "🌍 Lahan", message: msg, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Beli 1 Ha", style: .default) { [weak self] _ in
            guard let self = self else { return }
            if !self.engine.beliHa(1.0) {
                self.showToast("Gagal -- cek uang atau kapasitas afdeling")
            }
            self.refreshTreesAndHud()
        })
        alert.addAction(UIAlertAction(title: "Buka Afdeling Baru", style: .default) { [weak self] _ in
            guard let self = self else { return }
            if !self.engine.bukaAfdelingBaru() {
                self.showToast("Gagal -- cek syarat (Asisten Afdeling) atau uang")
            }
            self.refreshTreesAndHud()
        })
        alert.addAction(UIAlertAction(title: "Tutup", style: .cancel))
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
        actionStack.isHidden = false // BUG diperbaiki -- dulu panel tetap tersembunyi setelah
            // inspector ditutup, bikin tombol "Detail 3D" (& tombol aksi lain) terlihat
            // "kurang responsif" krn sebenarnya invisible, bukan macet.
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
        trees = engine.trees()
        hudMoney.text = "💰 Rp \(Int(engine.money()))"
        hudDay.text = "📅 Hari \(engine.day())"
        hudTph.text = "📦 TPH \(Int(engine.tphStock()))/\(Int(engine.tphCap()))"
        let allBlocks = engine.blockSummaries()
        if currentBlockIndex >= allBlocks.count { currentBlockIndex = 0 }
        if !allBlocks.isEmpty {
            let b = allBlocks[currentBlockIndex]
            var line = "\(b.statusEmoji) Block \(b.name) — \(b.treeCount) pokok | \(b.healthyCount) sehat"
            if b.hamaCount > 0 { line += " | \(b.hamaCount) hama" }
            if b.ganodermaCount > 0 { line += " | \(b.ganodermaCount) ganoderma" }
            if b.readyToHarvestCount > 0 { line += " | \(b.readyToHarvestCount) siap panen" }
            if allBlocks.count > 1 { line += "  ▾" } // isyarat visual bisa diketuk kalau block >1
            hudBlock.text = line
        }
        if selectedTreeId >= 0, let t = trees.first(where: { $0.treeId == selectedTreeId }) {
            let snap = (id: t.treeId, health: t.health, ffb: t.ffb, frond: t.frond, hasTbsReady: t.hasTbsReady)
            if lastRenderedTreeSnapshot == nil || snap != lastRenderedTreeSnapshot! {
                renderActionPanel(for: t)
                lastRenderedTreeSnapshot = snap
            }
        }
    }

    private func drainEvents() {
        for line in engine.pollEventsRaw() {
            let parts = line.components(separatedBy: "|")
            guard parts.count >= 2, let type = Int(parts[0]) else { continue }
            if type == 0 || type == 1 {
                showToast(parts[1])
            }
        }
    }

    // Toast minimal native (UILabel yang fade-out sendiri) — silakan ganti gaya sesuai selera.
    private func showToast(_ text: String) {
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
            UIView.animate(withDuration: 0.3, delay: 1.4, options: [], animations: { lbl.alpha = 0 }, completion: { _ in
                lbl.removeFromSuperview()
            })
        })
    }

    deinit {
        hudTimer?.invalidate()
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
