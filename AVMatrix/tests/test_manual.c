// Pruebas del manual incluido en la app (App/avm_manual.c): estructura,
// UTF-8 válido, marcado correcto y que cubre todos los temas pedidos.

#include "../App/avm_manual.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FALLO: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// UTF-8 estricto (sin secuencias sobrelargas ni sustitutos).
static bool valid_utf8(const char* s)
{
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        unsigned c = *p, cp, n;
        if (c < 0x80) { p++; continue; }
        if (c >= 0xC2 && c <= 0xDF)      { n = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; }
        else return false;
        for (unsigned i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        p += n;
    }
    return true;
}

// Sección cuyo título o cuerpo contiene `needle`, o -1.
static int find(const char* needle)
{
    for (int i = 0; i < avm_manual_count; i++)
        if (strstr(avm_manual_sections[i].title, needle) || strstr(avm_manual_sections[i].body, needle)) return i;
    return -1;
}

static bool in_section(const char* id, const char* needle)
{
    for (int i = 0; i < avm_manual_count; i++)
        if (strcmp(avm_manual_sections[i].id, id) == 0) return strstr(avm_manual_sections[i].body, needle) != NULL;
    return false;
}

static void test_structure(void)
{
    CHECK(avm_manual_count >= 12, "al menos 12 secciones (hay %d)", avm_manual_count);
    for (int i = 0; i < avm_manual_count; i++) {
        const avm_manual_section* s = &avm_manual_sections[i];
        CHECK(s->id && s->title && s->body && *s->id && *s->title && strlen(s->body) > 200,
              "sección %d completa (identificador, título y cuerpo)", i);
        for (int j = 0; j < i; j++) {
            CHECK(strcmp(s->id, avm_manual_sections[j].id) != 0, "identificador repetido: %s", s->id);
            CHECK(strcmp(s->title, avm_manual_sections[j].title) != 0, "título repetido: %s", s->title);
        }
        CHECK(valid_utf8(s->title) && valid_utf8(s->body), "%s: UTF-8 válido", s->id);
        CHECK(!strchr(s->body, '\t') && !strchr(s->body, '\r'), "%s: sin tabuladores ni CR", s->id);
        CHECK(s->body[strlen(s->body) - 1] != '\n', "%s: sin salto de línea final sobrante", s->id);
        // Marcado: «### » solo al principio de línea y con texto; sin títulos vacíos.
        const char* b = s->body;
        bool ok = true;
        for (const char* p = b; (p = strstr(p, "###")) != NULL; p += 3)
            if ((p != b && p[-1] != '\n') || p[3] != ' ' || p[4] == '\n' || p[4] == 0) ok = false;
        CHECK(ok, "%s: subtítulos «### » bien formados", s->id);
        CHECK(strlen(s->title) < 80, "%s: título corto para el índice", s->id);
    }
}

static void test_topics(void)
{
    // Temas obligatorios: cada uno debe aparecer en el manual.
    const struct { const char* topic; const char* needle; } req[] = {
        { "dispositivo Smaart",              "AV Matrix Smaart" },
        { "dispositivo LISTENTO",            "AV Matrix LISTENTO" },
        { "dispositivo Música",              "AV Matrix Música" },
        { "cómo seleccionarlos",             "Configuración de Audio MIDI" },
        { "transmisores",                    "Transmisores" },
        { "receptores",                      "Receptores" },
        { "conectar",                        "Conectar y desconectar" },
        { "grupos desplegables",             "Grupos desplegables" },
        { "colores de las casillas",         "Verde con ✓" },
        { "sumas",                           "Σ" },
        { "conexiones bloqueadas",           "Conexiones bloqueadas" },
        { "preset medición",                 "Preset medición" },
        { "preset directo",                  "Directo 8×8" },
        { "preset vaciar",                   "Vaciar" },
        { "referencia en Smaart",            "Canal de referencia: Entrada 8" },
        { "envío con LISTENTO",              "Envío (hacia el otro extremo)" },
        { "retorno con LISTENTO",            "Retorno (de vuelta)" },
        { "música estéreo",                  "Música L y Música R" },
        { "medidores",                       "barra de nivel" },
        { "dBFS",                            "dBFS es el nivel" },
        { "saturación",                      "### Saturación" },
        { "silencio",                        "«−∞»" },
        { "inactividad",                     "«—»" },
        { "cambiar nombre",                  "Cambiar nombre…" },
        { "guardar nombres",                 "nombres_canales.txt" },
        { "restaurar nombre",                "Restaurar nombre original" },
        { "frecuencias",                     "44,1 kHz, 48 kHz y 96 kHz" },
        { "reparar frecuencia",              "Reparar frecuencia" },
        { "formatos",                        "32 bits coma flotante" },
        { "diagnóstico",                     "Diagnóstico de problemas" },
        { "limitaciones",                    "Limitaciones conocidas" },
        { "silencios a 96 kHz",              "A 96 kHz hay silencios pendientes" },
        { "sin internet",                    "funciona sin internet" },
        { "nombres completos al pasar el ratón", "se ve completo" },
    };
    for (size_t i = 0; i < sizeof(req) / sizeof(req[0]); i++)
        CHECK(find(req[i].needle) >= 0, "el manual explica: %s («%s»)", req[i].topic, req[i].needle);

    // Hechos que deben coincidir con la app y el driver.
    CHECK(in_section("presets", "Smaart Salida 1 → Smaart Entrada 8"), "preset medición: Salida 1 → Entrada 8");
    CHECK(in_section("presets", "LISTENTO Retorno N → Smaart Entrada N, de 1 a 7"), "preset medición: retornos 1-7");
    CHECK(in_section("grupos", "LISTENTO Retorno → LISTENTO Envío está siempre bloqueado"), "bloqueo del lazo de LISTENTO");
    CHECK(in_section("medidores", "2 segundos"), "saturación mantenida 2 s");
    CHECK(in_section("medidores", "0,25 s"), "inactividad a 0,25 s");
    CHECK(in_section("nombres", "32 caracteres"), "límite de 32 caracteres");
    CHECK(in_section("limitaciones", "verificado"), "no presenta como verificada la compatibilidad ni la medición por internet");
    // La interfaz ya no tiene el distintivo naranja: el manual no lo menciona.
    CHECK(find("REFERENCIA LOCAL") < 0, "sin el rótulo «REFERENCIA LOCAL» en mayúsculas");
    CHECK(find("distintivo") < 0, "sin mencionar un distintivo de la Entrada 8");
}

int main(void)
{
    test_structure();
    test_topics();
    printf("Manual: %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
