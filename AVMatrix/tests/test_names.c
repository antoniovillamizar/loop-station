// Pruebas de los nombres personalizados de canales (sin CoreAudio).

#define _POSIX_C_SOURCE 200809L
#include "../Client/avm_names.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } else { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static char g_dir[256];

// strcmp que admite NULL (un alias rechazado devuelve NULL).
static int streq(const char* a, const char* b) { return a != NULL && b != NULL && strcmp(a, b) == 0; }

static void path_in_dir(char* out, size_t len, const char* name)
{
    snprintf(out, len, "%s/%s", g_dir, name);
}

static void test_keys_are_stable(void)
{
    // Las claves identifican dispositivo, dirección y número. No deben cambiar
    // nunca: si esta prueba falla, los nombres guardados dejarían de cargarse.
    const char* expected[AVM_NAMES_COUNT] = {
        "smaart.out.1", "smaart.out.2", "smaart.out.3", "smaart.out.4", "smaart.out.5", "smaart.out.6", "smaart.out.7", "smaart.out.8",
        "listento.out.1", "listento.out.2", "listento.out.3", "listento.out.4", "listento.out.5", "listento.out.6", "listento.out.7", "listento.out.8",
        "music.out.1", "music.out.2",
        "smaart.in.1", "smaart.in.2", "smaart.in.3", "smaart.in.4", "smaart.in.5", "smaart.in.6", "smaart.in.7", "smaart.in.8",
        "listento.in.1", "listento.in.2", "listento.in.3", "listento.in.4", "listento.in.5", "listento.in.6", "listento.in.7", "listento.in.8",
    };
    int ok = 1;
    for (int i = 0; i < AVM_NAMES_COUNT; i++) {
        if (strcmp(avm_names_key(i), expected[i]) != 0) ok = 0;
        if (avm_names_index_of_key(expected[i]) != i) ok = 0;
    }
    CHECK(ok, "34 claves estables y únicas (dispositivo.dirección.número)");
    CHECK(avm_names_key(-1) == NULL && avm_names_key(AVM_NAMES_COUNT) == NULL, "índices fuera de rango");
    CHECK(avm_names_index_of_key("smaart.in.9") == -1, "clave desconocida");
    CHECK(avm_names_src_index(AVM_SRC_MUSIC_BASE + 1) == 17 && avm_names_dst_index(AVM_DST_LOCAL_REFERENCE) == 25,
          "índices de fuentes y destinos coinciden con la matriz");
}

static void test_defaults_and_numbers(void)
{
    char b[64], n[8];
    avm_names_default(avm_names_src_index(2), b, sizeof(b));                  CHECK(!strcmp(b, "Salida 3"), "%s", b);
    avm_names_default(avm_names_src_index(AVM_SRC_LINK_BASE), b, sizeof(b));  CHECK(!strcmp(b, "Retorno 1"), "%s", b);
    avm_names_default(avm_names_src_index(AVM_SRC_MUSIC_BASE + 1), b, sizeof(b)); CHECK(!strcmp(b, "Música R"), "%s", b);
    avm_names_default(avm_names_dst_index(AVM_DST_LOCAL_REFERENCE), b, sizeof(b)); CHECK(!strcmp(b, "Entrada 8"), "%s", b);
    avm_names_default(avm_names_dst_index(AVM_DST_LINK_BASE + 1), b, sizeof(b));   CHECK(!strcmp(b, "Envío 2"), "%s", b);
    avm_names_number(avm_names_src_index(AVM_SRC_MUSIC_BASE), n, sizeof(n));   CHECK(!strcmp(n, "L"), "número de Música L");
    avm_names_number(avm_names_dst_index(AVM_DST_LINK_BASE + 7), n, sizeof(n)); CHECK(!strcmp(n, "8"), "número de Envío 8");
}

static void test_edit_restore_and_accents(void)
{
    avm_names n;
    avm_names_init(&n);
    int ref = avm_names_dst_index(AVM_DST_LOCAL_REFERENCE);
    int s1 = avm_names_src_index(AVM_SRC_SMAART_BASE);

    CHECK(avm_names_get(&n, ref) == NULL, "sin alias: nombre original");
    CHECK(avm_names_set(&n, s1, "Generador rosa") == AVM_NAME_OK, "editar");
    CHECK(streq(avm_names_get(&n, s1), "Generador rosa"), "alias guardado");

    const char* accents = "Señal pública · Ñandú ÁÉÍÓÚ üç";   // 30 caracteres
    CHECK(avm_names_set(&n, ref, accents) == AVM_NAME_OK, "alias con tildes y eñes");
    CHECK(streq(avm_names_get(&n, ref), accents), "las tildes se conservan byte a byte");

    // Forma descompuesta (e + acento combinante): se conserva tal cual, también es UTF-8 válido.
    const char* nfd = "Cafe\xCC\x81";
    CHECK(avm_names_set(&n, avm_names_src_index(3), nfd) == AVM_NAME_OK && streq(avm_names_get(&n, 3), nfd), "acento combinante");

    // Restaurar: botón, texto vacío, solo espacios, o el nombre original.
    avm_names_restore(&n, s1);
    CHECK(avm_names_get(&n, s1) == NULL, "restaurar nombre original");
    avm_names_set(&n, s1, "X");
    CHECK(avm_names_set(&n, s1, "") == AVM_NAME_RESTORED && avm_names_get(&n, s1) == NULL, "vacío = restaurar");
    avm_names_set(&n, s1, "X");
    CHECK(avm_names_set(&n, s1, "   \t ") == AVM_NAME_RESTORED && avm_names_get(&n, s1) == NULL, "solo espacios = restaurar");
    avm_names_set(&n, s1, "X");
    CHECK(avm_names_set(&n, s1, " Salida 1 ") == AVM_NAME_RESTORED && avm_names_get(&n, s1) == NULL, "igual al original = restaurar");

    // El alias de la referencia local es independiente del resto.
    CHECK(streq(avm_names_get(&n, ref), accents), "restaurar otro canal no afecta a la Entrada 8");
}

static void test_normalization_and_limits(void)
{
    char out[AVM_ALIAS_MAX_BYTES + 1];
    CHECK(avm_names_normalize("  PA   izquierda \n", out, sizeof(out)) == AVM_NAME_OK && !strcmp(out, "PA izquierda"),
          "espacios recortados y unidos: «%s»", out);
    CHECK(avm_names_normalize("Línea\tcon\ttab", out, sizeof(out)) == AVM_NAME_OK && !strcmp(out, "Línea con tab"),
          "tabuladores -> espacio (el archivo usa TAB como separador)");
    CHECK(avm_names_normalize("Dos\nlíneas", out, sizeof(out)) == AVM_NAME_OK && !strcmp(out, "Dos líneas"), "saltos de línea -> espacio");
    CHECK(avm_names_normalize("Bip\x07", out, sizeof(out)) == AVM_NAME_OK && !strcmp(out, "Bip"), "caracteres de control eliminados");

    // 32 caracteres con tildes (64 bytes) se admiten; 33 no.
    char ok32[200] = "", bad33[200] = "";
    for (int i = 0; i < 32; i++) strcat(ok32, "é");
    strcpy(bad33, ok32); strcat(bad33, "é");
    CHECK(avm_names_normalize(ok32, out, sizeof(out)) == AVM_NAME_OK, "32 caracteres con tilde: válido (se cuentan caracteres, no bytes)");
    CHECK(avm_names_normalize(bad33, out, sizeof(out)) == AVM_NAME_TOO_LONG, "33 caracteres: demasiado largo");

    CHECK(avm_names_normalize("caf\xE9", out, sizeof(out)) == AVM_NAME_INVALID_UTF8, "Latin-1 (no UTF-8): rechazado");
    CHECK(avm_names_normalize("\xC0\xAF", out, sizeof(out)) == AVM_NAME_INVALID_UTF8, "UTF-8 sobrelargo: rechazado");
    CHECK(avm_names_normalize("\xED\xA0\x80", out, sizeof(out)) == AVM_NAME_INVALID_UTF8, "sustituto UTF-16: rechazado");

    avm_names n;
    avm_names_init(&n);
    avm_names_set(&n, 5, "Bueno");
    CHECK(avm_names_set(&n, 5, bad33) == AVM_NAME_TOO_LONG && streq(avm_names_get(&n, 5), "Bueno"),
          "un alias rechazado no borra el anterior");
    CHECK(avm_names_set(&n, 99, "x") == AVM_NAME_BAD_INDEX, "índice inválido");
}

static void test_persistence(void)
{
    char path[512];
    path_in_dir(path, sizeof(path), "nombres_canales.txt");
    unlink(path);

    avm_names a, b;
    CHECK(avm_names_load(&a, path), "sin archivo: carga correcta con nombres originales");
    int any = 0;
    for (int i = 0; i < AVM_NAMES_COUNT; i++) if (avm_names_get(&a, i)) any = 1;
    CHECK(!any, "sin archivo: ningún alias");

    avm_names_set(&a, avm_names_src_index(0), "Generador rosa");
    avm_names_set(&a, avm_names_src_index(AVM_SRC_LINK_BASE + 2), "Micrófono de sala — Bogotá");
    avm_names_set(&a, avm_names_src_index(AVM_SRC_MUSIC_BASE), "Música izquierda");
    avm_names_set(&a, avm_names_dst_index(AVM_DST_LOCAL_REFERENCE), "Señal de referencia");
    avm_names_set(&a, avm_names_dst_index(AVM_DST_LINK_BASE + 7), "Envío Colombia 8");
    CHECK(avm_names_save(&a, path), "guardar");

    // «Reinicio»: estructura nueva, misma ruta.
    CHECK(avm_names_load(&b, path), "volver a cargar");
    CHECK(memcmp(&a, &b, sizeof(a)) == 0, "los 34 nombres se recuperan exactamente, con tildes");

    // Sin restos del guardado atómico.
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    CHECK(access(tmp, F_OK) != 0, "no queda archivo temporal");

    // Restaurar y guardar: el alias desaparece del archivo.
    avm_names_restore(&b, avm_names_src_index(0));
    avm_names_save(&b, path);
    avm_names c;
    avm_names_load(&c, path);
    CHECK(avm_names_get(&c, 0) == NULL && streq(avm_names_get(&c, avm_names_dst_index(AVM_DST_LOCAL_REFERENCE)), "Señal de referencia"),
          "restaurar persiste y no afecta a los demás");

    // Contenido legible en UTF-8 con las claves por dispositivo/dirección/número.
    FILE* f = fopen(path, "r");
    char buf[2048] = "";
    size_t r = fread(buf, 1, sizeof(buf) - 1, f);
    buf[r] = 0;
    fclose(f);
    CHECK(strstr(buf, "AVMATRIX-NOMBRES 1\n") == buf, "cabecera de formato");
    CHECK(strstr(buf, "smaart.in.8\tSeñal de referencia\n") != NULL, "línea de la Entrada 8 con su clave");
    CHECK(strstr(buf, "listento.out.3\tMicrófono de sala — Bogotá\n") != NULL, "línea de un retorno con tildes");
    CHECK(strstr(buf, "smaart.out.1") == NULL, "el canal restaurado ya no está en el archivo");
}

static void test_robust_load(void)
{
    char path[512];
    path_in_dir(path, sizeof(path), "dañado.txt");
    FILE* f = fopen(path, "w");
    fputs("AVMATRIX-NOMBRES 1\n", f);
    fputs("smaart.out.2\tBombo\n", f);
    fputs("clave.futura.9\tSe ignora\n", f);          // versión posterior
    fputs("linea sin tabulador\n", f);
    fputs("smaart.in.3\tcaf\xE9 latin1\n", f);        // UTF-8 inválido: se ignora
    fputs("listento.in.1\t", f);                      // alias vacío
    fputs("\n", f);
    for (int i = 0; i < 1500; i++) fputc('x', f);     // línea demasiado larga
    fputs("\nmusic.out.2\tMúsica derecha\r\n", f);    // fin de línea Windows
    fputs("smaart.out.2\tBombo bis", f);              // última línea sin salto: se acepta (gana la última)
    fclose(f);
    avm_names n;
    CHECK(avm_names_load(&n, path), "archivo con líneas dañadas: se carga");
    CHECK(streq(avm_names_get(&n, 1), "Bombo bis"), "duplicado: gana la última línea (también sin salto final)");
    CHECK(avm_names_get(&n, avm_names_dst_index(2)) == NULL, "UTF-8 inválido ignorado");
    CHECK(avm_names_get(&n, avm_names_dst_index(AVM_DST_LINK_BASE)) == NULL, "alias vacío ignorado");
    CHECK(streq(avm_names_get(&n, avm_names_src_index(AVM_SRC_MUSIC_BASE + 1)), "Música derecha"), "CRLF admitido");

    path_in_dir(path, sizeof(path), "otro.txt");
    f = fopen(path, "w");
    fputs("no es un archivo de nombres\nsmaart.out.1\tX\n", f);
    fclose(f);
    CHECK(!avm_names_load(&n, path) && avm_names_get(&n, 0) == NULL, "sin cabecera: se rechaza y se usan los originales");
}

static void test_save_failure_keeps_previous(void)
{
    char path[512];
    path_in_dir(path, sizeof(path), "bloqueado/nombres.txt");   // el directorio no existe
    avm_names n;
    avm_names_init(&n);
    avm_names_set(&n, 0, "A");
    CHECK(!avm_names_save(&n, path), "guardar en una ruta imposible devuelve error (no se ignora)");
}

int main(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/avm_names_test_%ld", (long)getpid());
    mkdir(g_dir, 0700);
    test_keys_are_stable();
    test_defaults_and_numbers();
    test_edit_restore_and_accents();
    test_normalization_and_limits();
    test_persistence();
    test_robust_load();
    test_save_failure_keeps_previous();
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) printf("aviso: no se pudo borrar %s\n", g_dir);
    printf("Nombres: %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
