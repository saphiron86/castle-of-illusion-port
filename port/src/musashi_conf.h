/* Musashi configuration for the port, used through MUSASHI_CNF.
 *
 * Starts from Musashi's own m68kconf.h and changes only what the Genesis
 * needs: an interrupt acknowledge callback, so the bus can clear the pending
 * interrupt when the CPU takes it.
 */
#ifndef COI_MUSASHI_CONF_H
#define COI_MUSASHI_CONF_H

#include "m68kconf.h"

#undef M68K_EMULATE_INT_ACK
#define M68K_EMULATE_INT_ACK M68K_OPT_SPECIFY_HANDLER
#undef M68K_INT_ACK_CALLBACK
#define M68K_INT_ACK_CALLBACK(A) genesis_int_ack(A)

/* An instruction hook, set with m68k_set_instr_hook_callback: for the translator's test harness
 * (main.cpp), and in local development builds' Debug builds (COI_COVERAGE) for their coverage
 * log. Otherwise Musashi's default (no hook). */
#if defined(COI_COVERAGE) || defined(COI_NATIVE)
#undef M68K_INSTRUCTION_HOOK
#define M68K_INSTRUCTION_HOOK M68K_OPT_ON
#endif

/* Recompiler builds: before each instruction, coi_native_enter (main.cpp) may run translated
 * code from there instead; it returns the cycles that used (the loop then goes on from where
 * that code stopped), or -1 to run the instruction here. Inside m68k_execute's loop, so
 * `continue` and the cycle macros are Musashi's own. (Translated code never lowers the
 * interrupt mask itself, so no interrupt is due when it returns.) Otherwise the hook set with
 * m68k_set_instr_hook_callback runs. */
#ifdef COI_NATIVE
#undef M68K_INSTRUCTION_HOOK
#define M68K_INSTRUCTION_HOOK M68K_OPT_SPECIFY_HANDLER
#undef M68K_INSTRUCTION_CALLBACK
#define M68K_INSTRUCTION_CALLBACK(pc) { \
        int coi_used = coi_native_enter(pc, GET_CYCLES()); \
        if (coi_used >= 0) { USE_CYCLES(coi_used); continue; } \
        CALLBACK_INSTR_HOOK(pc); }
#endif

#ifdef __cplusplus
extern "C" {
#endif
int genesis_int_ack(int level);
int coi_native_enter(unsigned int pc, int cycles_left);
#ifdef __cplusplus
}
#endif

#endif
