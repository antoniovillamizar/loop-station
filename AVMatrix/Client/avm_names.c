// AV Matrix — nombres personalizados de los canales. Ver avm_names.h.

#define _POSIX_C_SOURCE 200809L  // fileno, fsync
#include "avm_names.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AVM_NAMES_HEADER "AVMATRIX-NOMBRES 1"

static const char* const kKeys[AVM_NAMES_COUNT] = {
    // Fuentes (índices de la matriz 0..17)
    "smaart.out.1", "smaart.out.2", "smaart.out.3", "smaart.out.4",
    "smaart.out.5", "smaart.out.6", "smaart.out.7", "smaart.out.8",
    "listento.out.1", "listento.out.2", "listento.out.3", "listento.out.4",
    "listento.out.5", "listento.out.6", "listento.out.7", "listento.out.8",
    "music.out.1", "music.out.2",
    // Destinos (índices de la matriz 0..15)
    "smaart.in.1", "smaart.in.2", "smaart.in.3", "smaart.in.4",
    "smaart.in.5", "smaart.in.6", "smaart.in.7", "smaart.in.8",
    "listento.in.1", "listento.in.2", "listento.in.3", "listento.in.4",
    "listento.in.5", "listento.in.6", "listento.in.7", "listento.in.8",
};

void avm_names_init(avm_names* n)
{
    memset(n, 0, sizeof(*n));
}

const char* avm_names_key(int idx)
{
    return (idx >= 0 && idx < AVM_NAMES_COUNT) ? kKeys[idx] : NULL;
}

int avm_names_index_of_key(const char* key)
{
    if (key == NULL) return -1;
    for (int i = 0; i < AVM_NAMES_COUNT; i++)
        if (strcmp(key, kKeys[i]) == 0) return i;
    return -1;
}

void avm_names_number(int idx, char* buf, size_t len)
{
    if (idx == avm_names_src_index(AVM_SRC_MUSIC_BASE))          snprintf(buf, len, "L");
    else if (idx == avm_names_src_index(AVM_SRC_MUSIC_BASE + 1)) snprintf(buf, len, "R");
    else if (idx >= 0 && idx < AVM_NAMES_COUNT)                  snprintf(buf, len, "%d", (idx < AVM_NUM_SOURCES ? idx : idx - AVM_NUM_SOURCES) % 8 + 1);
    else if (len) buf[0] = 0;
}

void avm_names_default(int idx, char* buf, size_t len)
{
    char num[4];
    avm_names_number(idx, num, sizeof(num));
    if (idx < 0 || idx >= AVM_NAMES_COUNT) { if (len) buf[0] = 0; return; }
    if (idx < AVM_SRC_LINK_BASE)                snprintf(buf, len, "Salida %s", num);
    else if (idx < AVM_SRC_MUSIC_BASE)          snprintf(buf, len, "Retorno %s", num);
    else if (idx < AVM_NUM_SOURCES)             snprintf(buf, len, "Música %s", num);
    else if (idx < AVM_NUM_SOURCES + AVM_DST_LINK_BASE) snprintf(buf, len, "Entrada %s", num);
    else                                        snprintf(buf, len, "Envío %s", num);
}

// Longitud de la secuencia UTF-8 que empieza en p (1..4), o 0 si no es válida.
// Rechaza secuencias demasiado largas, sustitutos y valores > U+10FFFF.
static int utf8_seq(const unsigned char* p, size_t avail)
{
    unsigned char c = p[0];
    if (c < 0x80) return 1;
    int n;
    unsigned cp;
    if (c >= 0xC2 && c <= 0xDF)      { n = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; }
    else return 0;
    if ((size_t)n > avail) return 0;
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    return n;
}

avm_name_status avm_names_normalize(const char* in, char* out, size_t out_len)
{
    if (out_len == 0) return AVM_NAME_TOO_LONG;
    out[0] = 0;
    if (in == NULL) return AVM_NAME_RESTORED;
    const unsigned char* p = (const unsigned char*)in;
    size_t avail = strlen(in), o = 0, chars = 0;
    bool pending_space = false;
    while (avail > 0) {
        int n = utf8_seq(p, avail);
        if (n == 0) { out[0] = 0; return AVM_NAME_INVALID_UTF8; }
        bool space = (n == 1) && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f');
        bool control = (n == 1) && (*p < 0x20 || *p == 0x7F) && !space;
        if (space) {
            pending_space = o > 0;   // los espacios iniciales se descartan
        } else if (!control) {
            if (pending_space) {
                if (chars + 1 > AVM_ALIAS_MAX_CHARS || o + 1 >= out_len) { out[0] = 0; return AVM_NAME_TOO_LONG; }
                out[o++] = ' ';
                chars++;
                pending_space = false;
            }
            if (chars + 1 > AVM_ALIAS_MAX_CHARS || o + (size_t)n >= out_len) { out[0] = 0; return AVM_NAME_TOO_LONG; }
            memcpy(out + o, p, (size_t)n);
            o += (size_t)n;
            chars++;
        }
        p += n;
        avail -= (size_t)n;
    }
    out[o] = 0;   // los espacios finales no se copian (pending_space se descarta)
    return o == 0 ? AVM_NAME_RESTORED : AVM_NAME_OK;
}

avm_name_status avm_names_set(avm_names* n, int idx, const char* alias)
{
    if (idx < 0 || idx >= AVM_NAMES_COUNT) return AVM_NAME_BAD_INDEX;
    char clean[AVM_ALIAS_MAX_BYTES + 1];
    avm_name_status st = avm_names_normalize(alias, clean, sizeof(clean));
    if (st == AVM_NAME_TOO_LONG || st == AVM_NAME_INVALID_UTF8) return st;  // no se modifica el alias actual
    char def[64];
    avm_names_default(idx, def, sizeof(def));
    if (st == AVM_NAME_RESTORED || strcmp(clean, def) == 0) {
        n->alias[idx][0] = 0;
        return AVM_NAME_RESTORED;
    }
    memcpy(n->alias[idx], clean, strlen(clean) + 1);
    return AVM_NAME_OK;
}

void avm_names_restore(avm_names* n, int idx)
{
    if (idx >= 0 && idx < AVM_NAMES_COUNT) n->alias[idx][0] = 0;
}

const char* avm_names_get(const avm_names* n, int idx)
{
    if (idx < 0 || idx >= AVM_NAMES_COUNT || n->alias[idx][0] == 0) return NULL;
    return n->alias[idx];
}

bool avm_names_save(const avm_names* n, const char* path)
{
    if (path == NULL) return false;
    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(tmp)) return false;
    FILE* f = fopen(tmp, "w");
    if (f == NULL) return false;
    bool ok = fprintf(f, "%s\n", AVM_NAMES_HEADER) > 0;
    for (int i = 0; ok && i < AVM_NAMES_COUNT; i++)
        if (n->alias[i][0]) ok = fprintf(f, "%s\t%s\n", kKeys[i], n->alias[i]) > 0;
    if (ok) ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

bool avm_names_load(avm_names* n, const char* path)
{
    avm_names_init(n);
    if (path == NULL) return false;
    FILE* f = fopen(path, "r");
    if (f == NULL) return errno == ENOENT;   // sin archivo: nombres originales
    char line[1024];
    bool header = fgets(line, sizeof(line), f) != NULL
               && strncmp(line, AVM_NAMES_HEADER, strlen(AVM_NAMES_HEADER)) == 0;
    if (header) {
        while (fgets(line, sizeof(line), f) != NULL) {
            size_t L = strlen(line);
            if (L > 0 && line[L - 1] == '\n') {
                line[--L] = 0;
            } else if (!feof(f)) {
                // Línea más larga que el búfer: se descarta entera.
                int ch;
                while ((ch = fgetc(f)) != EOF && ch != '\n') {}
                continue;
            }   // si no, es la última línea sin salto final: se acepta
            if (L > 0 && line[L - 1] == '\r') line[--L] = 0;
            char* tab = strchr(line, '\t');
            if (tab == NULL) continue;
            *tab = 0;
            int idx = avm_names_index_of_key(line);
            if (idx < 0) continue;                         // clave desconocida (versión futura): se ignora
            avm_names_set(n, idx, tab + 1);                // valida y normaliza; lo inválido se ignora
        }
    }
    fclose(f);
    return header;
}
