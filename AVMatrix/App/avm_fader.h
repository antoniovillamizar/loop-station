// AV Matrix — escala de los faders del mixer (C puro, sin AppKit).
//
// La posición del fader va de 0 (abajo, silencio) a 1 (arriba, 0 dB). Escala
// cúbica: ganancia = posición³ (−6 dB hacia el 79 % del recorrido, −20 dB al
// 46 %, −60 dB al 10 %). Solo atenúa: nunca devuelve una ganancia mayor que 1.

#ifndef AVM_FADER_H
#define AVM_FADER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Por encima de esta posición el fader se ajusta a 0 dB exactos (ganancia 1),
// para que volver arriba recupere el paso de señal sin tocar.
#define AVM_FADER_TOP_SNAP 0.998

float  avm_fader_gain_from_pos(double pos);   // 0 -> 0, >= AVM_FADER_TOP_SNAP -> 1
double avm_fader_pos_from_gain(float gain);   // inversa (0..1)
// Lectura en dB del fader: "0.0 dB", "-12.5 dB" o "−∞" (silencio).
void   avm_fader_format(float gain, char* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
