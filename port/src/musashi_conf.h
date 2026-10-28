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

#ifdef __cplusplus
extern "C" {
#endif
int genesis_int_ack(int level);
#ifdef __cplusplus
}
#endif

#endif
