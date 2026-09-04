import AVFoundation

/// Musik & efek suara -- semua file di Resources/*.wav SENGAJA disintesis
/// sendiri (bukan diunduh) krn network sandbox pengembangan tak bisa akses
/// situs SFX gratis manapun (freesound/opengameart/kenney dst SEMUA diblokir
/// egress) -- lihat catatan lengkap di percakapan pengembangan. Hasilnya
/// 100% bebas lisensi (dibuat dari nol via sintesis matematis di Python,
/// bukan salinan karya siapa pun) & ukuran SANGAT kecil (total ~930KB utk
/// 6 SFX + 1 BGM 20 detik loop).
///
/// AVAudioPlayer BARU dibuat tiap kali SFX diputar (bukan 1 instance dipakai
/// ulang) -- SFX di game ini SANGAT PENDEK (<0.5 detik) & jarang tumpang
/// tindih lebih dari 2-3x bersamaan, jadi overhead ini dapat diterima &
/// jauh lebih SEDERHANA drpd bikin pool manual spt SoundPool Android.
final class SoundManager {
    private var bgmPlayer: AVAudioPlayer?
    private var activeSfxPlayers: [AVAudioPlayer] = [] // tahan referensi spy tak di-dealloc di tengah main
    private var sfxEnabled = true
    private var bgmEnabled = true

    init() {
        try? AVAudioSession.sharedInstance().setCategory(.ambient, options: [.mixWithOthers])
        try? AVAudioSession.sharedInstance().setActive(true)
    }

    private func url(for name: String, ext: String = "wav") -> URL? {
        // BGM sekarang .mp3 (jauh lebih kecil dari .wav mentah), SFX tetap
        // .wav (sudah sangat kecil sejak awal, tak perlu diubah) -- default
        // param ext="wav" spy pemanggilan SFX yg sudah ada tak perlu diubah.
        Bundle.main.url(forResource: name, withExtension: ext)
    }

    private func play(_ name: String) {
        guard sfxEnabled, let u = url(for: name) else { return }
        guard let player = try? AVAudioPlayer(contentsOf: u) else { return }
        player.volume = 0.8
        activeSfxPlayers.removeAll { !$0.isPlaying } // buang referensi yg sudah selesai
        activeSfxPlayers.append(player)
        player.play()
    }

    func playClick() { play("click") }
    func playSuccess() { play("success") }
    func playCash() { play("cash") }
    func playError() { play("error") }
    func playNotification() { play("notification") }
    func playLevelUp() { play("levelup") }

    /// Pilih SFX berdasar ISI TEKS toast -- notifikasi generik sbg default,
    /// tapi kata kunci umum (uang kurang/gagal/direkrut/dibangun) dapat SFX
    /// yg lebih sesuai konteksnya tanpa perlu ubah tipe event di engine.
    func playForToast(_ text: String) {
        let t = text.lowercased()
        if t.contains("tidak cukup") || t.contains("gagal") || t.contains("belum terpenuhi") ||
           t.contains("sudah sedang dikerjakan") || t.contains("penuh") {
            playError()
        } else if t.contains("direkrut") || t.contains("dibangun") || t.contains("naik") ||
                  t.contains("dibuka dengan") {
            playLevelUp()
        } else if t.contains("dipanen") || t.contains("diobati") || t.contains("sembuh") ||
                  t.contains("dipupuk") {
            playSuccess()
        } else {
            playNotification()
        }
    }

    func startBgm() {
        guard bgmPlayer == nil, let u = url(for: "bgm_loop", ext: "mp3") else { return }
        bgmPlayer = try? AVAudioPlayer(contentsOf: u)
        bgmPlayer?.numberOfLoops = -1 // ulang tanpa henti
        bgmPlayer?.volume = 0.35 // latar belakang -- jangan menutupi SFX/toast
        if bgmEnabled { bgmPlayer?.play() }
    }

    @discardableResult
    func toggleBgm() -> Bool {
        bgmEnabled.toggle()
        if bgmEnabled { bgmPlayer?.play() } else { bgmPlayer?.pause() }
        return bgmEnabled
    }

    @discardableResult
    func toggleSfx() -> Bool {
        sfxEnabled.toggle()
        return sfxEnabled
    }

    func onPause() { bgmPlayer?.pause() }
    func onResume() { if bgmEnabled { bgmPlayer?.play() } }
}
