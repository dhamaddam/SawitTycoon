import UIKit

/// Bagan/treeview jenjang SDM -- fitur baru diminta pengguna: "Buatkan
/// tampilan SDM menjadi bentuk treeview (bagan) menunjukkan jenjang".
/// Identik Android (HrTreeView.kt) -- lihat catatan lengkap struktur DAG
/// (asistenAfdeling butuh 2 prasyarat sekaligus, jadi 2 garis penghubung)
/// & alasan urutan baris (row) di sana.
final class HrTreeNode {
    let key: String
    let row: Int
    let col: CGFloat // 0.0=kiri, 0.5=tengah, 1.0=kanan
    let parents: [String]
    init(_ key: String, _ row: Int, _ col: CGFloat, _ parents: [String]) {
        self.key = key; self.row = row; self.col = col; self.parents = parents
    }
}

final class HrTreeView: UIView {
    // Layout MANUAL, identik Android -- lihat catatan lengkap di HrTreeView.kt.
    private let nodes: [HrTreeNode] = [
        HrTreeNode("manager", 0, 0.5, ["asistenKepala"]),
        HrTreeNode("asistenKepala", 1, 0.5, ["asistenAfdeling"]),
        HrTreeNode("asistenAfdeling", 2, 0.5, ["mandorBesar", "kraniKepala"]), // DAG -- 2 prasyarat sekaligus
        HrTreeNode("kraniKepala", 3, 0.72, ["krani"]),
        HrTreeNode("krani", 4, 0.72, ["mandor"]),
        HrTreeNode("mandorBesar", 4, 0.28, ["mandor"]),
        HrTreeNode("mandor", 5, 0.5, ["buruh"]),
        HrTreeNode("buruh", 6, 0.5, []),
    ]

    var infos: [SawitHrLevelInfo] = [] { didSet { setNeedsDisplay() } }
    var onNodeTap: ((SawitHrLevelInfo) -> Void)?

    private let boxW: CGFloat = 108
    private let boxH: CGFloat = 58
    private let rowGap: CGFloat = 78
    private let topPad: CGFloat = 24
    private var nodeRects: [String: CGRect] = [:]

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .clear
        isUserInteractionEnabled = true
        let tap = UITapGestureRecognizer(target: self, action: #selector(handleTap(_:)))
        addGestureRecognizer(tap)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) tak dipakai") }

    override var intrinsicContentSize: CGSize {
        let maxRow = nodes.map { $0.row }.max() ?? 0
        return CGSize(width: UIView.noIntrinsicMetric, height: topPad + CGFloat(maxRow + 1) * rowGap + boxH)
    }

    override func draw(_ rect: CGRect) {
        guard let ctx = UIGraphicsGetCurrentContext() else { return }
        let w = bounds.width
        nodeRects.removeAll()

        var centers: [String: CGPoint] = [:]
        for n in nodes {
            let cx = n.col * w
            let cy = topPad + CGFloat(n.row) * rowGap + boxH / 2
            centers[n.key] = CGPoint(x: cx, y: cy)
        }

        // Garis penghubung ke prasyarat (child -> parent)
        ctx.setStrokeColor(UIColor(white: 0.73, alpha: 1).cgColor)
        ctx.setLineWidth(2.5)
        for n in nodes {
            guard let c = centers[n.key] else { continue }
            for parentKey in n.parents {
                guard let p = centers[parentKey] else { continue }
                ctx.move(to: CGPoint(x: c.x, y: c.y - boxH / 2))
                ctx.addLine(to: CGPoint(x: p.x, y: p.y + boxH / 2))
            }
        }
        ctx.strokePath()

        // Kotak node (setelah garis)
        for n in nodes {
            guard let info = infos.first(where: { $0.key == n.key }), let c = centers[n.key] else { continue }
            let box = CGRect(x: c.x - boxW / 2, y: c.y - boxH / 2, width: boxW, height: boxH)
            nodeRects[n.key] = box
            let color: UIColor
            if !info.underMax { color = UIColor(red: 0.27, green: 0.35, blue: 0.38, alpha: 1) } // abu -- maksimum
            else if !info.prereqMet { color = UIColor(red: 0.36, green: 0.25, blue: 0.22, alpha: 1) } // coklat gelap -- terkunci
            else { color = UIColor(red: 0.18, green: 0.49, blue: 0.20, alpha: 1) } // hijau -- bisa direkrut

            let path = UIBezierPath(roundedRect: box, cornerRadius: 10)
            color.setFill(); path.fill()
            UIColor(white: 1, alpha: 0.63).setStroke(); path.lineWidth = 2; path.stroke()

            let nameFontSize: CGFloat = info.name.count > 14 ? 9.5 : 11
            let nameAttrs: [NSAttributedString.Key: Any] = [.font: UIFont.boldSystemFont(ofSize: nameFontSize), .foregroundColor: UIColor.white]
            let nameStr = "\(info.icon) \(info.name)" as NSString
            let nameSize = nameStr.size(withAttributes: nameAttrs)
            nameStr.draw(at: CGPoint(x: c.x - nameSize.width/2, y: c.y - 20), withAttributes: nameAttrs)

            let countAttrs: [NSAttributedString.Key: Any] = [.font: UIFont.boldSystemFont(ofSize: 15), .foregroundColor: UIColor(red: 1, green: 0.93, blue: 0.35, alpha: 1)]
            let countStr = "\(info.count)x" as NSString
            let countSize = countStr.size(withAttributes: countAttrs)
            countStr.draw(at: CGPoint(x: c.x - countSize.width/2, y: c.y + 6), withAttributes: countAttrs)
        }
    }

    @objc private func handleTap(_ gesture: UITapGestureRecognizer) {
        let pt = gesture.location(in: self)
        for (key, rect) in nodeRects {
            if rect.contains(pt) {
                if let info = infos.first(where: { $0.key == key }) { onNodeTap?(info) }
                return
            }
        }
    }
}
