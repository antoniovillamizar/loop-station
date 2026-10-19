// AV Matrix — manual de uso incluido en la app (C puro, UTF-8, sin internet).
//
// Cada sección tiene un identificador estable, un título (el índice de la
// ventana de Ayuda) y un cuerpo con un marcado mínimo:
//   línea en blanco      separa párrafos
//   "### texto"          subtítulo
//   "- texto"            viñeta
// El texto nunca se usa como cadena de formato.

#ifndef AVM_MANUAL_H
#define AVM_MANUAL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* id;
    const char* title;
    const char* body;
} avm_manual_section;

extern const avm_manual_section avm_manual_sections[];
extern const int avm_manual_count;

#ifdef __cplusplus
}
#endif

#endif
