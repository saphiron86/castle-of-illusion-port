// ymfm's OPN chips (ymfm_opn.cpp), built through this file so the engine templates the port
// calls directly are instantiated explicitly: Opn2::generate_sample (sound.cpp, Clear FM) calls
// fm_engine_base::output, which ymfm_opn.cpp only uses inline, so GCC kept no copy of it (and
// of the register functions it needs) to link to. MSVC happened to link without this.
#include "ymfm_opn.cpp"

namespace ymfm {
template class opn_registers_base<true>;
template class fm_engine_base<opn_registers_base<true>>;
}
