// AV Matrix — escala de los faders del mixer. Ver avm_fader.h.

#include "avm_fader.h"
#include "../Driver/src/avm_core.h"

#include <math.h>
#include <stdio.h>

float avm_fader_gain_from_pos(double pos)
{
    if (!(pos > 0.0)) return 0.0f;               // también NaN
    if (pos >= AVM_FADER_TOP_SNAP) return 1.0f;  // 0 dB exactos
    float g = (float)(pos * pos * pos);
    return g > 1.0f ? 1.0f : g;
}

double avm_fader_pos_from_gain(float gain)
{
    if (!(gain > 0.0f)) return 0.0;
    if (gain >= 1.0f) return 1.0;
    return cbrt((double)gain);
}

void avm_fader_format(float gain, char* buf, size_t len)
{
    if (len == 0) return;
    if (!(gain > 0.0f)) { snprintf(buf, len, "−∞"); return; }
    float db = avm_gain_to_db(gain);
    // Redondeo a 0,1 dB sin mostrar «-0.0».
    if (db > -0.05f) { snprintf(buf, len, "0.0 dB"); return; }
    snprintf(buf, len, "%.1f dB", (double)db);
}
