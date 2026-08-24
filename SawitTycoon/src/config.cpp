#include "sawit/types.hpp"

namespace sawit {

GameConfig GameConfig::makeDefault() {
    GameConfig c;
    c.pokokPerHa = 143;
    c.afdelingMaxHa = 25;
    c.toolAgeThresholdYears = 6;

    c.pricePupuk = 25000;
    c.pricePestisida = 20000;
    c.priceFungisida = 60000;
    c.priceTebang = 40000;
    c.pricePekerja = 900000;
    c.pricePerTandan = 55000;

    c.startMoney = 750000;
    c.dayLengthSeconds = 75;
    c.tphCapStart = 30;
    c.tandanPerPokokPerHari = 0.045;

    c.haBasePrice = 1200000;
    c.haPriceGrowth = 1.045;
    c.afdelingBaseCost = 15000000;
    c.afdelingCostGrowth = 1.4;

    c.oerStart = 0.20;
    c.oerMax = 0.24;
    c.oerStep = 0.01;
    c.kerRate = 0.05;
    c.avgTandanKg = 20;
    c.cpoPrice = 9800;
    c.pkPrice = 6200;
    c.pksBuildCost = 55000000;
    c.pksUpgradeBaseCost = 35000000;
    c.pksUpgradeGrowth = 1.4;
    c.pksCapacityBase = 20;
    c.pksCapacityPerLevel = 15;

    // Jenjang SDM — identik dengan HR_LEVELS di prototipe web,
    // sumber literatur dicantumkan di field `cite`.
    c.hrLevels = {
        { "buruh", "Karyawan Pelaksana (Buruh Lapangan)", "\xF0\x9F\x91\xB7", 0, 1.0, 110000,
          "Tenaga harian/kontrak: pemanen, pemupuk, penyemprot gulma, tukang rawat tanaman.",
          "jurnal.unpad.ac.id, borneoreview.co", false, {}, -1 },
        { "mandor", "Mandor", "\xF0\x9F\xA7\xAD", 3200000, 1.22, 260000,
          "Pengawas lini pertama yang memimpin kelompok kecil pekerja lapangan.",
          "borneoreview.co", true, HrPrereq{"buruh", 3, false, "", 0}, -1 },
        { "krani", "Krani (Krani Buah)", "\xF0\x9F\x93\x8B", 3000000, 1.22, 250000,
          "Mencatat hasil kerja, absensi & volume buah -- menekan losses di TPH.",
          "borneoreview.co", true, HrPrereq{"mandor", 1, false, "", 0}, -1 },
        { "mandorBesar", "Mandor Besar", "\xF0\x9F\xA7\xAD", 8500000, 1.25, 520000,
          "Mengoordinasikan beberapa Mandor sekaligus di satu afdeling.",
          "borneoreview.co", true, HrPrereq{"mandor", 2, false, "", 0}, -1 },
        { "kraniKepala", "Krani Kepala", "\xF0\x9F\x97\x82", 8000000, 1.25, 500000,
          "Merekapitulasi administrasi data afdeling dari beberapa Krani.",
          "borneoreview.co", true, HrPrereq{"krani", 2, false, "", 0}, -1 },
        { "asistenAfdeling", "Asisten Afdeling (Sinder)", "\xF0\x9F\xA7\x91\xE2\x80\x8D\xF0\x9F\x92\xBC",
          22000000, 1.3, 1400000,
          "Penanggung jawab satu afdeling -- mengatur operasional harian & membuka afdeling baru.",
          "borneoreview.co", true, HrPrereq{"mandorBesar", 1, true, "kraniKepala", 1}, -1 },
        { "asistenKepala", "Asisten Kepala (Askep)", "\xF0\x9F\x8E\xA9", 60000000, 1.35, 3200000,
          "Manajer menengah yang membawahi beberapa Asisten Afdeling.",
          "academia.edu, terajufoundation.org", true, HrPrereq{"asistenAfdeling", 2, false, "", 0}, -1 },
        { "manager", "Manager Kebun / Administratur (ADM)", "\xF0\x9F\x91\x91", 180000000, 1.0, 9000000,
          "Pimpinan tertinggi kebun -- bertanggung jawab penuh atas produksi & keuangan, membuka akses membangun PKS sendiri.",
          "asianagri.com, borneoreview.co", true, HrPrereq{"asistenKepala", 1, false, "", 0}, 1 },
    };

    c.stations = {
        {"Penerimaan & Sortasi", "\xE2\x9A\x96", "Jembatan timbang & sortasi kematangan TBS untuk menentukan mutu dan ALB."},
        {"Sterilisasi (Sterilizer)", "\xE2\x99\xA8", "Rebus TBS dengan uap panas -- nonaktifkan enzim lipase agar ALB tidak naik & buah melunak."},
        {"Penebahan (Thresher)", "\xF0\x9F\xA5\x81", "Perontokan brondolan dari janjang di drum thresher."},
        {"Pelumatan & Pengempaan (Digester + Screw Press)", "\xF0\x9F\x8C\x80", "Daging buah dilumat lalu dikempa memisahkan minyak kasar dari ampas (cake)."},
        {"Klarifikasi", "\xF0\x9F\xA7\xAA", "Pemurnian minyak kasar (purifier, vacuum dryer) menjadi CPO siap tangki timbun."},
        {"Pengolahan Biji (Kernel Plant)", "\xF0\x9F\xA5\xA5", "Ampas press dipecah (ripple mill) memisahkan cangkang dari inti sawit (kernel/PK)."},
    };

    return c;
}

} // namespace sawit
