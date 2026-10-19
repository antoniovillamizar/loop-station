// AV Matrix — nombres personalizados de los canales (C puro, sin CoreAudio).
//
// Un alias solo cambia el texto que muestra la app: no toca conexiones,
// numeración interna, niveles ni el driver. Cada alias se identifica por
// dispositivo, dirección y número de canal con una clave estable:
//
//   smaart.out.1-8     Smaart Salidas         listento.out.1-8   LISTENTO Retornos
//   music.out.1-2      Música L / R           smaart.in.1-8      Smaart Entradas
//   listento.in.1-8    LISTENTO Envíos
//
// Las claves no deben cambiar nunca: son las que se guardan en disco y las que
// permiten conservar los nombres al actualizar la aplicación.
//
// Formato del archivo (UTF-8): primera línea "AVMATRIX-NOMBRES 1"; después una
// línea por alias: <clave><TAB><alias>. Las líneas desconocidas o dañadas se
// ignoran. Se guarda de forma atómica (archivo temporal + rename).

#ifndef AVM_NAMES_H
#define AVM_NAMES_H

#include <stdbool.h>
#include <stddef.h>

#include "../Driver/src/avm_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AVM_NAMES_COUNT        (AVM_NUM_SOURCES + AVM_NUM_DESTS)   // 34
#define AVM_ALIAS_MAX_CHARS    32      // caracteres (puntos de código), no bytes
#define AVM_ALIAS_MAX_BYTES    (AVM_ALIAS_MAX_CHARS * 4)

typedef struct {
    char alias[AVM_NAMES_COUNT][AVM_ALIAS_MAX_BYTES + 1];   // "" = nombre original
} avm_names;

typedef enum {
    AVM_NAME_OK = 0,          // alias guardado
    AVM_NAME_RESTORED,        // vacío o igual al original: se usa el nombre original
    AVM_NAME_TOO_LONG,        // más de AVM_ALIAS_MAX_CHARS caracteres
    AVM_NAME_INVALID_UTF8,    // texto que no es UTF-8 válido
    AVM_NAME_BAD_INDEX
} avm_name_status;

// Índices: fuentes 0..17 (igual que la matriz), destinos 18..33.
static inline int avm_names_src_index(int src) { return src; }
static inline int avm_names_dst_index(int dst) { return AVM_NUM_SOURCES + dst; }

void avm_names_init(avm_names* n);

// Clave estable del canal (p. ej. "smaart.in.8"). NULL si el índice no existe.
const char* avm_names_key(int idx);
// Índice de una clave, o -1.
int avm_names_index_of_key(const char* key);

// Nombre original corto del canal, sin dispositivo: "Salida 3", "Retorno 1",
// "Música L", "Entrada 8", "Envío 2".
void avm_names_default(int idx, char* buf, size_t len);
// Número visible del canal: "1".."8", o "L"/"R" para Música.
void avm_names_number(int idx, char* buf, size_t len);

// Normaliza un alias: valida UTF-8, convierte tabuladores y saltos de línea en
// espacios, quita espacios al principio y al final y une espacios repetidos.
// No modifica tildes ni otros caracteres. El resultado vacío significa
// «restaurar el nombre original».
avm_name_status avm_names_normalize(const char* in, char* out, size_t out_len);

// Asigna un alias (normalizado). Vacío o igual al nombre original = restaurar.
avm_name_status avm_names_set(avm_names* n, int idx, const char* alias);
void avm_names_restore(avm_names* n, int idx);
// Alias del canal, o NULL si usa el nombre original.
const char* avm_names_get(const avm_names* n, int idx);

// Persistencia.
bool avm_names_save(const avm_names* n, const char* path);
// Carga los alias; un archivo inexistente deja todos los nombres originales.
bool avm_names_load(avm_names* n, const char* path);

#ifdef __cplusplus
}
#endif

#endif
