// Pruebas de la lógica de los puentes Dante y EVO (Client/avm_bridge_core.c),
// SIMULADAS en Linux: sin CoreAudio, sin DVS ni EVO reales. Comprueban el
// reparto de canales en el dispositivo agregado, la copia con canales separados,
// el fundido de entrada, que no queda audio viejo y los estados (desconexión,
// reconexión, frecuencias, realimentación, canales que la EVO no expone).

#include "../Client/avm_bridge_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static avm_bridge_env env_ok(double rate, uint32_t ch, const char* name)
{
    avm_bridge_env e;
    memset(&e, 0, sizeof(e));
    e.avm_present = true; e.avm_rate = rate; e.ext_present = true; e.ext_channels = ch; e.ext_rate = rate;
    snprintf(e.ext_name, sizeof(e.ext_name), "%s", name);
    return e;
}

static avm_bridge_config cfg(avm_bridge_kind k)
{
    avm_bridge_config c;
    avm_bridge_config_default(&c, k);
    c.enabled = true;
    snprintf(c.device_uid, sizeof(c.device_uid), "%s", k == AVM_BRIDGE_DANTE ? "DVS_UID" : "EVO8_UID");
    return c;
}

static void test_defaults(void)
{
    avm_bridge_config d, e;
    avm_bridge_config_default(&d, AVM_BRIDGE_DANTE);
    avm_bridge_config_default(&e, AVM_BRIDGE_EVO);
    CHECK(avm_bridge_channels(AVM_BRIDGE_DANTE) == 2 && d.channel[0] == 1 && d.channel[1] == 2 && !d.enabled, "Dante: canales 1 y 2 por defecto, detenido");
    CHECK(avm_bridge_channels(AVM_BRIDGE_EVO) == 4 && e.channel[0] == 1 && e.channel[3] == 4 && !e.enabled, "EVO: entradas 1-4 por defecto, detenido");
}

static void test_states(void)
{
    char m[256];
    avm_bridge_config c = cfg(AVM_BRIDGE_DANTE);
    avm_bridge_env e = env_ok(48000, 64, "Dante Virtual Soundcard");
    CHECK(avm_bridge_evaluate(&c, &e, m, sizeof(m)) == AVM_BRIDGE_RUN && strstr(m, "canales 1 y 2") && strstr(m, "48.0 kHz"), "Dante a 48 kHz: conectado («%s»)", m);
    for (int r = 0; r < 3; r++) {
        double rate = r == 0 ? 44100 : r == 1 ? 48000 : 96000;
        avm_bridge_env er = env_ok(rate, 64, "DVS");
        CHECK(avm_bridge_evaluate(&c, &er, m, sizeof(m)) == AVM_BRIDGE_RUN, "misma frecuencia %.0f Hz: conectado", rate);
    }
    avm_bridge_env mis = env_ok(48000, 64, "DVS"); mis.ext_rate = 44100;
    CHECK(avm_bridge_evaluate(&c, &mis, m, sizeof(m)) == AVM_BRIDGE_RATE_MISMATCH && strstr(m, "44.1"), "frecuencias distintas: no arranca y lo dice («%s»)", m);
    mis.ext_rate = 96000;
    CHECK(avm_bridge_evaluate(&c, &mis, m, sizeof(m)) == AVM_BRIDGE_RATE_MISMATCH, "48 frente a 96 kHz: no arranca");

    avm_bridge_config off = c; off.enabled = false;
    CHECK(avm_bridge_evaluate(&off, &e, m, sizeof(m)) == AVM_BRIDGE_OFF, "sin activar: detenido");
    avm_bridge_env s = e; s.suspended = true;
    CHECK(avm_bridge_evaluate(&c, &s, m, sizeof(m)) == AVM_BRIDGE_SUSPENDED, "en pausa durante un cambio de frecuencia");
    avm_bridge_env noavm = e; noavm.avm_present = false;
    CHECK(avm_bridge_evaluate(&c, &noavm, m, sizeof(m)) == AVM_BRIDGE_WAIT_AVMATRIX && strstr(m, "driver"), "falta el driver nuevo");
    avm_bridge_config nouid = c; nouid.device_uid[0] = 0;
    CHECK(avm_bridge_evaluate(&nouid, &e, m, sizeof(m)) == AVM_BRIDGE_BAD_CONFIG, "sin dispositivo elegido");
    avm_bridge_env self = e; self.ext_is_avmatrix = true;
    CHECK(avm_bridge_evaluate(&c, &self, m, sizeof(m)) == AVM_BRIDGE_BAD_CONFIG && strstr(m, "realimentación"), "un dispositivo de AV Matrix se rechaza (realimentación)");
    avm_bridge_env other = e; other.ext_used_by_other = true;
    CHECK(avm_bridge_evaluate(&c, &other, m, sizeof(m)) == AVM_BRIDGE_BAD_CONFIG && strstr(m, "realimentación"), "el mismo dispositivo en los dos puentes se rechaza");
    avm_bridge_env gone = e; gone.ext_present = false;
    CHECK(avm_bridge_evaluate(&c, &gone, m, sizeof(m)) == AVM_BRIDGE_WAIT_DEVICE && strstr(m, "desconectada") && strstr(m, "sin audio viejo"),
          "DVS desaparece: envío detenido y se indica («%s»)", m);

    avm_bridge_config dup = c; dup.channel[1] = 1;
    CHECK(avm_bridge_evaluate(&dup, &e, m, sizeof(m)) == AVM_BRIDGE_BAD_CONFIG && strstr(m, "separados"), "L y R en el mismo canal: rechazado");
    avm_bridge_config zero = c; zero.channel[0] = 0;
    CHECK(avm_bridge_evaluate(&zero, &e, m, sizeof(m)) == AVM_BRIDGE_BAD_CONFIG, "Dante necesita canal para L y R");
    avm_bridge_config far = c; far.channel[1] = 65;
    CHECK(avm_bridge_evaluate(&far, &e, m, sizeof(m)) == AVM_BRIDGE_BAD_CONFIG && strstr(m, "64"), "canal de DVS inexistente: rechazado («%s»)", m);
    avm_bridge_config neg = c; neg.channel[0] = -3;
    CHECK(avm_bridge_evaluate(&neg, &e, m, sizeof(m)) == AVM_BRIDGE_BAD_CONFIG, "canal negativo rechazado");

    // EVO: la EVO 4 solo expone 2 entradas -> EVO 3 y 4 en silencio, con aviso.
    avm_bridge_config ev = cfg(AVM_BRIDGE_EVO);
    avm_bridge_env e2 = env_ok(48000, 2, "EVO4");
    CHECK(avm_bridge_evaluate(&ev, &e2, m, sizeof(m)) == AVM_BRIDGE_RUN && strstr(m, "2 entradas") && strstr(m, "silencio"),
          "EVO con 2 entradas: funciona y avisa («%s»)", m);
    avm_bridge_env e8 = env_ok(48000, 4, "EVO8");
    CHECK(avm_bridge_evaluate(&ev, &e8, m, sizeof(m)) == AVM_BRIDGE_RUN && !strstr(m, "silencio"), "EVO con 4 entradas: conectada");
    avm_bridge_config none = ev; for (int k = 0; k < 4; k++) none.channel[k] = 0;
    CHECK(avm_bridge_evaluate(&none, &e8, m, sizeof(m)) == AVM_BRIDGE_BAD_CONFIG, "EVO sin canales elegidos");
    avm_bridge_config evdup = ev; evdup.channel[3] = 2;
    CHECK(avm_bridge_evaluate(&evdup, &e8, m, sizeof(m)) == AVM_BRIDGE_BAD_CONFIG, "dos canales AV Matrix del mismo canal EVO: rechazado");
    avm_bridge_env evgone = e8; evgone.ext_present = false;
    CHECK(avm_bridge_evaluate(&ev, &evgone, m, sizeof(m)) == AVM_BRIDGE_WAIT_DEVICE && strstr(m, "EVO desconectada"), "EVO desconectada: se indica («%s»)", m);

    // Secuencia de desconexión y reconexión.
    avm_bridge_state seq[4];
    avm_bridge_env st = e8;
    seq[0] = avm_bridge_evaluate(&ev, &st, m, sizeof(m));
    st.ext_present = false; seq[1] = avm_bridge_evaluate(&ev, &st, m, sizeof(m));
    st.ext_present = true; st.ext_rate = 44100; seq[2] = avm_bridge_evaluate(&ev, &st, m, sizeof(m));
    st.ext_rate = 48000; seq[3] = avm_bridge_evaluate(&ev, &st, m, sizeof(m));
    CHECK(seq[0] == AVM_BRIDGE_RUN && seq[1] == AVM_BRIDGE_WAIT_DEVICE && seq[2] == AVM_BRIDGE_RATE_MISMATCH && seq[3] == AVM_BRIDGE_RUN,
          "conectada -> desconectada -> vuelve a otra frecuencia (espera) -> igualada (conectada)");
    CHECK(!strcmp(avm_bridge_state_label(AVM_BRIDGE_RUN), "Conectado") && !strcmp(avm_bridge_state_label(AVM_BRIDGE_WAIT_DEVICE), "Desconectado"), "etiquetas");
}

static void test_locate(void)
{
    uint32_t bufs[3] = { 2, 1, 4 };
    int b, c;
    CHECK(avm_bridge_locate(bufs, 3, 0, &b, &c) && b == 0 && c == 0, "canal 0");
    CHECK(avm_bridge_locate(bufs, 3, 2, &b, &c) && b == 1 && c == 0, "canal 2 en el buffer mono");
    CHECK(avm_bridge_locate(bufs, 3, 6, &b, &c) && b == 2 && c == 3, "último canal");
    CHECK(!avm_bridge_locate(bufs, 3, 7, &b, &c), "fuera de rango");
}

// Rellena cada buffer con un valor por canal y por fotograma: 1000·buffer + 10·canal + f/1000.
static void fill(float* buf, uint32_t ch, uint32_t frames, int bufidx)
{
    for (uint32_t f = 0; f < frames; f++)
        for (uint32_t c = 0; c < ch; c++) buf[f * ch + c] = 1000.0f * (float)bufidx + 10.0f * (float)c + (float)f / 1000.0f;
}

static void test_dante_plan_and_render(void)
{
    // Agregado [AV Matrix Dante (2 entradas), DVS (64 entradas y 64 salidas en un stream)].
    avm_bridge_config c = cfg(AVM_BRIDGE_DANTE);
    uint32_t in_b[2] = { 2, 64 }, out_b[1] = { 64 };
    avm_bridge_plan p;
    char why[128];
    CHECK(avm_bridge_make_plan(&c, in_b, 2, out_b, 1, 2, 0, 64, 64, &p, why, sizeof(why)) && p.ncopy == 2, "plan Dante (%s)", why);
    CHECK(p.copy[0].in_buf == 0 && p.copy[0].in_ch == 0 && p.copy[0].out_buf == 0 && p.copy[0].out_ch == 0
          && p.copy[1].in_ch == 1 && p.copy[1].out_ch == 1, "Dante L -> DVS 1, Dante R -> DVS 2");

    enum { F = 512 };
    static float avm[F * 2], dvs_in[F * 64], dvs_out[F * 64];
    fill(avm, 2, F, 0); fill(dvs_in, 64, F, 1);
    for (int i = 0; i < F * 64; i++) dvs_out[i] = 99.0f;   // basura previa: debe desaparecer
    const float* in[2] = { avm, dvs_in };
    float* out[1] = { dvs_out };
    avm_bridge_render(&p, F, in, out);
    int fade_ok = 1, exact = 1, zero = 1;
    for (uint32_t f = 0; f < F; f++) {
        for (int k = 0; k < 2; k++) {
            float v = dvs_out[f * 64 + (uint32_t)k], want = avm[f * 2 + (uint32_t)k];
            if (f < AVM_BRIDGE_FADE_FRAMES) { float g = (float)(f + 1) / AVM_BRIDGE_FADE_FRAMES; if (fabsf(v - want * g) > 1e-6f * fabsf(want) + 1e-9f) fade_ok = 0; }
            else if (v != want) exact = 0;
        }
        for (int c2 = 2; c2 < 64; c2++) if (dvs_out[f * 64 + (uint32_t)c2] != 0.0f) zero = 0;
    }
    CHECK(fade_ok, "fundido de entrada de %d muestras al arrancar (sin chasquido)", AVM_BRIDGE_FADE_FRAMES);
    CHECK(exact, "después del fundido, copia exacta de Dante L/R");
    CHECK(zero, "las demás salidas de DVS en silencio (nada de basura previa ni de sus propias entradas)");
    CHECK(dvs_out[0] != avm[1] && dvs_out[1] != avm[0], "L y R no se cruzan");
    // Siguiente ciclo: sin fundido, exacto desde la primera muestra.
    fill(avm, 2, F, 5);
    avm_bridge_render(&p, F, in, out);
    CHECK(dvs_out[0] == avm[0] && dvs_out[1] == avm[1] && dvs_out[64 * 3 + 1] == avm[3 * 2 + 1], "ciclo siguiente: exacto");

    // DVS con un stream por canal (64 mono) y Dante en los canales 5 y 6, invertidos.
    c.channel[0] = 6; c.channel[1] = 5;
    uint32_t mono[64];
    for (int i = 0; i < 64; i++) mono[i] = 1;
    avm_bridge_plan q;
    CHECK(avm_bridge_make_plan(&c, in_b, 2, mono, 64, 2, 0, 64, 64, &q, why, sizeof(why)), "plan con 64 streams mono");
    CHECK(q.copy[0].out_buf == 5 && q.copy[1].out_buf == 4 && q.copy[0].out_ch == 0, "L -> canal 6 y R -> canal 5");
    // Totales que no cuadran: no arranca.
    uint32_t bad_in[1] = { 64 };
    CHECK(!avm_bridge_make_plan(&c, bad_in, 1, out_b, 1, 2, 0, 64, 64, &q, why, sizeof(why)) && strstr(why, "esperados"), "agregado con canales inesperados: rechazado («%s»)", why);
}

static void test_evo_plan_and_render(void)
{
    // Agregado [AV Matrix EVO (4 salidas), EVO (4 entradas en 2 streams estéreo, 2 salidas)].
    avm_bridge_config c = cfg(AVM_BRIDGE_EVO);
    uint32_t in_b[2] = { 2, 2 }, out_b[2] = { 4, 2 };
    avm_bridge_plan p;
    char why[128];
    CHECK(avm_bridge_make_plan(&c, in_b, 2, out_b, 2, 0, 4, 4, 2, &p, why, sizeof(why)) && p.ncopy == 4, "plan EVO (%s)", why);
    enum { F = 300 };
    static float e01[F * 2], e23[F * 2], avm_out[F * 4], evo_out[F * 2];
    fill(e01, 2, F, 0); fill(e23, 2, F, 1);
    for (int i = 0; i < F * 4; i++) avm_out[i] = -5.0f;
    for (int i = 0; i < F * 2; i++) evo_out[i] = -5.0f;
    const float* in[2] = { e01, e23 };
    float* out[2] = { avm_out, evo_out };
    avm_bridge_render(&p, F, in, out);
    avm_bridge_render(&p, F, in, out);   // segundo ciclo: sin fundido
    int iso = 1, quiet = 1;
    for (uint32_t f = 0; f < F; f++) {
        float want[4] = { e01[f * 2], e01[f * 2 + 1], e23[f * 2], e23[f * 2 + 1] };
        for (int k = 0; k < 4; k++) if (avm_out[f * 4 + (uint32_t)k] != want[k]) iso = 0;
        if (evo_out[f * 2] != 0.0f || evo_out[f * 2 + 1] != 0.0f) quiet = 0;
    }
    CHECK(iso, "EVO 1-4 -> AV Matrix EVO 1-4, exactos y separados (de dos streams estéreo)");
    CHECK(quiet, "las salidas propias de la EVO quedan en silencio (el puente no le envía nada)");

    // Reasignación: AV Matrix EVO 1 <- entrada 4, EVO 2 <- entrada 3; EVO 3 y 4 sin canal.
    c.channel[0] = 4; c.channel[1] = 3; c.channel[2] = 0; c.channel[3] = 0;
    CHECK(avm_bridge_make_plan(&c, in_b, 2, out_b, 2, 0, 4, 4, 2, &p, why, sizeof(why)) && p.ncopy == 2, "plan reasignado");
    p.fade_pos = AVM_BRIDGE_FADE_FRAMES;
    avm_bridge_render(&p, F, in, out);
    CHECK(avm_out[0] == e23[1] && avm_out[1] == e23[0] && avm_out[2] == 0.0f && avm_out[3] == 0.0f, "reasignación y canales sin entrada en silencio");

    // La EVO 4 solo expone 2 entradas: EVO 3 y 4 quedan en silencio.
    avm_bridge_config_default(&c, AVM_BRIDGE_EVO);
    uint32_t in2[1] = { 2 }, out2[2] = { 4, 2 };
    CHECK(avm_bridge_make_plan(&c, in2, 1, out2, 2, 0, 4, 2, 2, &p, why, sizeof(why)) && p.ncopy == 2, "EVO de 2 entradas: solo 2 copias");
    p.fade_pos = AVM_BRIDGE_FADE_FRAMES;
    const float* in1[1] = { e01 };
    avm_bridge_render(&p, F, in1, out);
    CHECK(avm_out[0] == e01[0] && avm_out[1] == e01[1] && avm_out[2] == 0.0f && avm_out[3] == 0.0f, "EVO 3 y 4 en silencio");

    // Entrada ausente (NULL): silencio, sin leer memoria inválida.
    const float* none[1] = { NULL };
    avm_bridge_render(&p, F, none, out);
    int z = 1; for (int i = 0; i < F * 4; i++) if (avm_out[i] != 0.0f) z = 0;
    CHECK(z, "sin datos de entrada: silencio");
}

// Reconexión: un plan nuevo empieza de cero (fundido desde 0), sin nada del anterior.
static void test_no_old_audio_on_reconnect(void)
{
    avm_bridge_config c = cfg(AVM_BRIDGE_DANTE);
    uint32_t in_b[1] = { 2 }, out_b[1] = { 2 };
    avm_bridge_plan p;
    char why[64];
    avm_bridge_make_plan(&c, in_b, 1, out_b, 1, 2, 0, 0, 2, &p, why, sizeof(why));
    float in[64 * 2], out[64 * 2];
    for (int i = 0; i < 128; i++) in[i] = 0.9f;
    const float* ip[1] = { in }; float* op[1] = { out };
    for (int k = 0; k < 10; k++) avm_bridge_render(&p, 64, ip, op);
    CHECK(out[0] == 0.9f, "en marcha: exacto");
    // Desconexión y reconexión: nuevo agregado, nuevo plan.
    avm_bridge_plan p2;
    avm_bridge_make_plan(&c, in_b, 1, out_b, 1, 2, 0, 0, 2, &p2, why, sizeof(why));
    CHECK(p2.fade_pos == 0, "el plan nuevo no arrastra estado del anterior");
    for (int i = 0; i < 128; i++) in[i] = 0.5f;
    avm_bridge_render(&p2, 64, ip, op);
    CHECK(out[0] == 0.5f / AVM_BRIDGE_FADE_FRAMES && out[0] < 0.01f, "al reconectar, primera muestra casi en silencio (fundido) y solo con audio nuevo");
    int mono = 1; for (int f = 1; f < 64; f++) if (!(out[f * 2] > out[(f - 1) * 2])) mono = 0;
    CHECK(mono, "fundido creciente, sin saltos");
}

int main(void)
{
    test_defaults();
    test_states();
    test_locate();
    test_dante_plan_and_render();
    test_evo_plan_and_render();
    test_no_old_audio_on_reconnect();
    printf("Puentes (simulados): %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
