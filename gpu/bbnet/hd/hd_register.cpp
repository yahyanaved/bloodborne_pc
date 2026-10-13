// SPDX-License-Identifier: GPL-3.0-or-later
// The Hunter's Dream backend's exports, by name, as bbhost's register_hle_fn table has
// them. The loader asks here first for every sceNp* import when the backend is on
// (src/runtime_services.c); what is not here stays with the shadNet libraries or the
// offline runtime (the network sockets, sceHttp, trophies, the dialogs).
#include <cstring>

#include "hd/hd.h"
#include "hd/hd_np.h"

namespace Hd {

namespace {

const Exports& Table() {
    static const Exports table = [] {
        Exports t;
        RegisterNp(t);
        RegisterNpMatching2(t);
        RegisterNpSignaling(t);
        return t;
    }();
    return table;
}

} // namespace

std::uintptr_t Resolve(const char* symbol) {
    if (!symbol || !Enabled()) return 0;
    for (const Export& e : Table()) {
        if (std::strcmp(e.name, symbol) == 0) return reinterpret_cast<std::uintptr_t>(e.function);
    }
    return 0;
}

} // namespace Hd

extern "C" std::uintptr_t bbnet_hd_resolve(const char* symbol) {
    return Hd::Resolve(symbol);
}
