/* ACH2, NOT UPSTREAM: single-symbol compat shim.
 *
 * PCSX2's GE1 archives bundle a newer rcheevos than our vendored pin
 * (f87c0de) and their Achievements TU calls
 * rc_client_set_unofficial_enabled(), which does not exist at f87c0de.
 * We exclude their bundled librcheevos.a from the link (duplicate rc_*
 * symbols, different pin) and satisfy their TU from our copy instead,
 * so this one missing symbol needs a definition.
 *
 * Behavior: f87c0de's client never loads unofficial achievements, which
 * is exactly the "disabled" behavior; record the requested flag in a
 * global for inspection. Our engine never calls rc_client at all
 * (offline local achievements only), and PCSX2's Achievements path never
 * logs in on our targets (no credentials/UI), so this is link-only.
 */
#include "rc_client.h"

int g_ps2x_ach2_unofficial_enabled = 0;

/* Matches the newer-upstream declaration PCSX2 compiled against. */
void rc_client_set_unofficial_enabled(rc_client_t* client, int enabled)
{
  (void)client;
  g_ps2x_ach2_unofficial_enabled = (enabled != 0);
}
