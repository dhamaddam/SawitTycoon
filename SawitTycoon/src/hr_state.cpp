#include "sawit/types.hpp"

namespace sawit {

int HrState::countFor(const std::string& key) const {
    if (key == "buruh") return buruh;
    if (key == "mandor") return mandor;
    if (key == "krani") return krani;
    if (key == "mandorBesar") return mandorBesar;
    if (key == "kraniKepala") return kraniKepala;
    if (key == "asistenAfdeling") return asistenAfdeling;
    if (key == "asistenKepala") return asistenKepala;
    if (key == "manager") return manager;
    return 0;
}

void HrState::add(const std::string& key, int n) {
    if (key == "buruh") buruh += n;
    else if (key == "mandor") mandor += n;
    else if (key == "krani") krani += n;
    else if (key == "mandorBesar") mandorBesar += n;
    else if (key == "kraniKepala") kraniKepala += n;
    else if (key == "asistenAfdeling") asistenAfdeling += n;
    else if (key == "asistenKepala") asistenKepala += n;
    else if (key == "manager") manager += n;
}

} // namespace sawit
