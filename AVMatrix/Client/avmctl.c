// avmctl — control y autoprueba de AV Matrix desde la Terminal.
//
//   avmctl status                      dispositivos, frecuencia y formatos
//   avmctl show                        matriz actual
//   avmctl connect <fuente> <destino>  p. ej. avmctl connect smaart.out.1 listento.in.1
//   avmctl disconnect <fuente> <destino>
//   avmctl preset default|direct|clear  (default = medición con referencia local en Entrada 8)
//   avmctl rate 44100|48000|96000
//   avmctl recover                     reaplica la frecuencia actual en los tres dispositivos
//   avmctl meters [--watch]            medidores de nivel (no abre audio)
//   avmctl format <smaart|listento|music> <in|out> <f32|s32|s24|s16>
//   avmctl test                        aislamiento, música, regla anti-lazo y retardo de transporte
//   avmctl test-all                    la misma prueba en las 3 frecuencias x 4 formatos

#include "avm_client.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static void print_err(const char* what, OSStatus err)
{
    char cc[5] = { 0 };
    UInt32 be = CFSwapInt32HostToBig((UInt32)err);
    memcpy(cc, &be, 4);
    bool printable = true;
    for (int i = 0; i < 4; i++) if (cc[i] < 32 || cc[i] > 126) printable = false;
    if (printable) fprintf(stderr, "Error en %s: '%s' (%d)\n", what, cc, (int)err);
    else           fprintf(stderr, "Error en %s: %d\n", what, (int)err);
}

static int require_installed(void)
{
    for (int k = 0; k < AVM_DEV_COUNT; k++) {
        if (avm_find_device((avm_device_kind)k) == kAudioObjectUnknown) {
            fprintf(stderr, "No se encuentra \"%s\". ¿Está instalado el driver y reiniciado coreaudiod?\n",
                    avm_device_label((avm_device_kind)k));
            return 0;
        }
    }
    return 1;
}

// ---------------------------------------------------------------------------

static int cmd_status(void)
{
    int ok = 1;
    for (int k = 0; k < AVM_DEV_COUNT; k++) {
        AudioObjectID d = avm_find_device((avm_device_kind)k);
        printf("%-20s %s", avm_device_label((avm_device_kind)k), d == kAudioObjectUnknown ? "NO ENCONTRADO\n" : "presente");
        if (d == kAudioObjectUnknown) { ok = 0; continue; }
        printf(" (id %u)\n", (unsigned)d);
        for (int dir = 0; dir < 2; dir++) {
            bool input = dir == 0;
            if (k == AVM_DEV_MUSIC && input) continue;
            AudioStreamBasicDescription asbd;
            if (avm_get_physical_format((avm_device_kind)k, input, &asbd) == noErr) {
                int f = avm_format_from_asbd(&asbd);
                printf("    %-8s %u canales, %.0f Hz, %s\n", input ? "entradas" : "salidas",
                       (unsigned)asbd.mChannelsPerFrame, asbd.mSampleRate, f >= 0 ? avm_format_name((avm_format)f) : "?");
            }
        }
    }
    if (ok) {
        int h = -1;
        OSStatus err = avm_get_rate_health(&h);
        if (err) { print_err("leer estado de frecuencia", err); ok = 0; }
        else if (h == AVM_RATE_HEALTH_CHANGING) { printf("Frecuencia: cambio en curso\n"); ok = 0; }
        else if (h == AVM_RATE_HEALTH_INCONSISTENT) {
            printf("Frecuencia: INCONSISTENTE. El audio no arranca hasta recuperar: avmctl recover\n");
            ok = 0;
        } else printf("Frecuencia: coherente en los tres dispositivos\n");
    }
    return ok ? 0 : 1;
}

static int cmd_show(void)
{
    avm_matrix m;
    OSStatus err = avm_get_matrix(&m);
    if (err) { print_err("leer matriz", err); return 1; }

    printf("%-38s", "destino \\ fuente");
    for (int s = 0; s < AVM_NUM_SOURCES; s++) {
        if (s < AVM_SRC_LINK_BASE) printf(" S%d", s + 1);
        else if (s < AVM_SRC_MUSIC_BASE) printf(" L%d", s - AVM_SRC_LINK_BASE + 1);
        else printf(" M%s", s == AVM_SRC_MUSIC_BASE ? "L" : "R");
    }
    printf("\n");
    for (int d = 0; d < AVM_NUM_DESTS; d++) {
        char name[48];
        avm_dest_name(d, name, sizeof(name));
        printf("%-38s", name);
        int n = 0;
        for (int s = 0; s < AVM_NUM_SOURCES; s++) {
            const char* c = m.cell[d][s] ? "●" : (avm_route_allowed(d, s) == AVM_ROUTE_OK ? "·" : "×");
            printf("  %s", c);
            n += m.cell[d][s];
        }
        if (n > 1) printf("   suma de %d fuentes", n);
        printf("\n");
    }
    printf("\nS = Smaart salida, L = LISTENTO retorno, M = Música.  ● conectado  · libre  × prohibido (lazo)\n");
    return 0;
}

static int cmd_connect(const char* src_t, const char* dst_t, int value)
{
    int s = avm_parse_source(src_t), d = avm_parse_dest(dst_t);
    if (s < 0) { fprintf(stderr, "Fuente desconocida: %s\n", src_t); return 2; }
    if (d < 0) { fprintf(stderr, "Destino desconocido: %s\n", dst_t); return 2; }
    if (value && avm_route_allowed(d, s) != AVM_ROUTE_OK) {
        fprintf(stderr, "Ruta prohibida: el retorno de LISTENTO no puede volver a LISTENTO (lazo de realimentación).\n");
        return 3;
    }
    avm_matrix m;
    OSStatus err = avm_get_matrix(&m);
    if (err) { print_err("leer matriz", err); return 1; }
    m.cell[d][s] = (uint8_t)value;
    err = avm_set_matrix(&m);
    if (err) { print_err("escribir matriz", err); return 1; }
    return cmd_show();
}

static int cmd_preset(const char* which)
{
    avm_matrix m;
    if (strcasecmp(which, "default") == 0 || strcasecmp(which, "measure") == 0) avm_matrix_default(&m);
    else if (strcasecmp(which, "direct") == 0) avm_matrix_direct(&m);
    else if (strcasecmp(which, "clear") == 0) avm_matrix_clear(&m);
    else { fprintf(stderr, "Preset desconocido: %s (default | direct | clear)\n", which); return 2; }
    OSStatus err = avm_set_matrix(&m);
    if (err) { print_err("escribir matriz", err); return 1; }
    return cmd_show();
}

static int wait_for_rate(Float64 rate)
{
    for (int i = 0; i < 40; i++) {
        Float64 r = 0;
        if (avm_get_sample_rate(&r) == noErr && r == rate) return 1;
        usleep(100000);
    }
    return 0;
}

static int cmd_rate(const char* s)
{
    Float64 r = atof(s);
    OSStatus err = avm_set_sample_rate(r);
    if (err == kAudioHardwareNotReadyError) {
        fprintf(stderr, "No se puede cambiar la frecuencia con audio activo en AV Matrix.\n"
                        "Detenga el audio en Smaart, LISTENTO y la música, y vuelva a intentarlo.\n");
        return 1;
    }
    if (err) { print_err("cambiar frecuencia", err); return 1; }
    if (!wait_for_rate(r)) { fprintf(stderr, "La frecuencia no cambió a %.0f Hz.\n", r); return 1; }
    printf("Frecuencia: %.0f Hz (los tres dispositivos comparten reloj)\n", r);
    return 0;
}

static int cmd_recover(void)
{
    OSStatus err = avm_recover_rate(0);
    if (err == kAudioHardwareNotReadyError) {
        fprintf(stderr, "No se puede recuperar con audio activo o con otro cambio en curso.\n"
                        "Detenga el audio en Smaart, LISTENTO y la música, y vuelva a intentarlo.\n");
        return 1;
    }
    if (err) { print_err("recuperar frecuencia", err); return 1; }
    for (int i = 0; i < 60; i++) {
        int h = -1;
        if (avm_get_rate_health(&h) == noErr && h == AVM_RATE_HEALTH_OK) {
            Float64 r = 0;
            avm_get_sample_rate(&r);
            printf("Recuperado: los tres dispositivos a %.0f Hz. El audio puede arrancar.\n", r);
            return 0;
        }
        usleep(100000);
    }
    fprintf(stderr, "La recuperación no se completó. Consulte: log show --last 2m | grep -i avmatrix\n");
    return 1;
}

static void fmt_level(const avm_meter_reading* r, char* buf, size_t len)
{
    if (!r->active) { snprintf(buf, len, "%9s", "inactivo"); return; }
    float db = avm_peak_to_dbfs(r->peak);
    if (isinf(db)) snprintf(buf, len, "%9s", "-inf dB");
    else snprintf(buf, len, "%+6.1f dB", db);
}

static int cmd_meters(int watch)
{
    do {
        avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
        OSStatus err = avm_get_meters(s, d);
        if (err) { print_err("leer medidores", err); return 1; }
        if (watch) printf("\033[H\033[2J");
        printf("%-34s %-12s     %-38s %-12s\n", "FUENTE (antes de la matriz)", "pico dBFS", "DESTINO (después de la matriz)", "pico dBFS");
        for (int i = 0; i < AVM_NUM_SOURCES || i < AVM_NUM_DESTS; i++) {
            char sn[64] = "", sl[32] = "", dn[64] = "", dl[32] = "";
            const char *sc = "", *dc = "";
            if (i < AVM_NUM_SOURCES) { avm_source_name(i, sn, sizeof(sn)); fmt_level(&s[i], sl, sizeof(sl)); sc = s[i].clip ? " SAT" : ""; }
            if (i < AVM_NUM_DESTS)   { avm_dest_name(i, dn, sizeof(dn));   fmt_level(&d[i], dl, sizeof(dl)); dc = d[i].clip ? " SAT" : ""; }
            printf("%-34s %-12s%-4s %-38s %-12s%s\n", sn, sl, sc, dn, dl, dc);
        }
        if (watch) { fflush(stdout); usleep(200000); }
    } while (watch);
    return 0;
}

static int parse_format(const char* s)
{
    if (!strcasecmp(s, "f32")) return AVM_FMT_FLOAT32;
    if (!strcasecmp(s, "s32")) return AVM_FMT_SINT32;
    if (!strcasecmp(s, "s24")) return AVM_FMT_SINT24;
    if (!strcasecmp(s, "s16")) return AVM_FMT_SINT16;
    return -1;
}

static int set_format_checked(avm_device_kind k, bool input, avm_format f)
{
    OSStatus err = avm_set_physical_format(k, input, f);
    if (err) { print_err("cambiar formato", err); return 0; }
    for (int i = 0; i < 40; i++) {
        AudioStreamBasicDescription d;
        if (avm_get_physical_format(k, input, &d) == noErr && avm_format_from_asbd(&d) == (int)f) return 1;
        usleep(100000);
    }
    fprintf(stderr, "El formato no cambió.\n");
    return 0;
}

static int cmd_format(const char* dev, const char* dir, const char* fmt)
{
    int k = !strcasecmp(dev, "smaart") ? AVM_DEV_SMAART : !strcasecmp(dev, "listento") ? AVM_DEV_LISTENTO
          : !strcasecmp(dev, "music") ? AVM_DEV_MUSIC : -1;
    int f = parse_format(fmt);
    bool input = !strcasecmp(dir, "in");
    if (k < 0 || f < 0 || (!input && strcasecmp(dir, "out"))) { fprintf(stderr, "Uso: avmctl format <smaart|listento|music> <in|out> <f32|s32|s24|s16>\n"); return 2; }
    if (k == AVM_DEV_MUSIC && input) { fprintf(stderr, "El dispositivo Música no tiene entradas.\n"); return 2; }
    return set_format_checked((avm_device_kind)k, input, (avm_format)f) ? cmd_status() : 1;
}

// ---------------------------------------------------------------------------
// Autoprueba: abre los tres dispositivos a la vez, como lo harían Smaart,
// LISTENTO y Apple Music, y comprueba muestra por muestra lo que llega.

#define T_SMAART  0
#define T_LINK    1
#define T_MUSIC   2

static float val_smaart(int c) { return (float)(c + 1) / 64.0f; }
static float val_link(int c)   { return -(float)(c + 1) / 64.0f; }
static const float kMusicL = 0.375f, kMusicR = -0.4375f;

typedef struct {
    int             mode;               // 0 = valores constantes, 1 = impulso
    Float64         rate;
    Float64         first_in[3];        // primera sample time de entrada vista por dispositivo
    _Atomic long    correct[2][8];
    _Atomic long    silent[2][8];
    _Atomic long    wrong[2][8];
    // impulso
    Float64         first_out;
    _Atomic int     impulse_sent;
    Float64         impulse_time;
    _Atomic int     got_link, got_smaart;
    Float64         t_link, t_smaart;
} test_ctx;

static test_ctx g_t;

static float expected_in(int dev, int c)
{
    if (dev == T_LINK) return val_smaart(c);           // smaart.out.N -> listento.in.N
    if (c == 0) return val_link(0) + val_link(1);      // suma: listento.out.1 + .2 -> smaart.in.1
    if (c < 6) return val_link(c);                     // listento.out.2-6 -> smaart.in.2-6
    return c == 6 ? kMusicL : kMusicR;                 // music.L/R -> smaart.in.7/8
}

// Valores de la fase de saturación (modo 2), en Smaart Salidas 1-3.
static const float kClipOut[3] = { 1.0f, 0.6f, 0.6f };

static void classify_input(int dev, const AudioBufferList* in, const AudioTimeStamp* t)
{
    if (in == NULL || in->mNumberBuffers < 1 || in->mBuffers[0].mData == NULL) return;
    const float* p = (const float*)in->mBuffers[0].mData;
    UInt32 nch = in->mBuffers[0].mNumberChannels;
    UInt32 frames = in->mBuffers[0].mDataByteSize / (sizeof(float) * (nch ? nch : 1));
    if (g_t.first_in[dev] < 0) g_t.first_in[dev] = t->mSampleTime;
    int di = dev == T_LINK ? 1 : 0;

    if (g_t.mode == 0) {
        if (t->mSampleTime < g_t.first_in[dev] + 1.5 * g_t.rate) return;  // calentamiento
        for (UInt32 f = 0; f < frames; f++)
            for (UInt32 c = 0; c < nch && c < 8; c++) {
                float v = p[f * nch + c], e = expected_in(dev, (int)c);
                if (v == e) atomic_fetch_add(&g_t.correct[di][c], 1);
                else if (v == 0.0f) atomic_fetch_add(&g_t.silent[di][c], 1);
                else atomic_fetch_add(&g_t.wrong[di][c], 1);
            }
    } else if (atomic_load(&g_t.impulse_sent)) {
        for (UInt32 f = 0; f < frames; f++) {
            if (p[f * nch] > 0.5f) {
                Float64 when = t->mSampleTime + f;
                if (dev == T_LINK && !atomic_load(&g_t.got_link)) { g_t.t_link = when; atomic_store(&g_t.got_link, 1); }
                if (dev == T_SMAART && !atomic_load(&g_t.got_smaart)) { g_t.t_smaart = when; atomic_store(&g_t.got_smaart, 1); }
                break;
            }
        }
    }
}

static void fill_output(int dev, AudioBufferList* out, const AudioTimeStamp* t)
{
    if (out == NULL || out->mNumberBuffers < 1 || out->mBuffers[0].mData == NULL) return;
    float* p = (float*)out->mBuffers[0].mData;
    UInt32 nch = out->mBuffers[0].mNumberChannels;
    UInt32 frames = out->mBuffers[0].mDataByteSize / (sizeof(float) * (nch ? nch : 1));
    memset(p, 0, out->mBuffers[0].mDataByteSize);

    if (g_t.mode == 0) {
        for (UInt32 f = 0; f < frames; f++)
            for (UInt32 c = 0; c < nch; c++)
                p[f * nch + c] = dev == T_SMAART ? val_smaart((int)c)
                               : dev == T_LINK   ? val_link((int)c)
                               : (c == 0 ? kMusicL : kMusicR);
    } else if (g_t.mode == 2) {
        if (dev == T_SMAART)
            for (UInt32 f = 0; f < frames; f++)
                for (UInt32 c = 0; c < nch && c < 3; c++) p[f * nch + c] = kClipOut[c];
    } else if (dev == T_SMAART) {
        if (g_t.first_out < 0) g_t.first_out = t->mSampleTime;
        Float64 target = g_t.first_out + g_t.rate;  // impulso 1 s después de arrancar
        if (!atomic_load(&g_t.impulse_sent) && t->mSampleTime <= target && target < t->mSampleTime + frames) {
            UInt32 f = (UInt32)(target - t->mSampleTime);
            p[f * nch] = 1.0f;
            g_t.impulse_time = target;
            atomic_store(&g_t.impulse_sent, 1);
        }
    }
}

static OSStatus io_proc(AudioObjectID dev, const AudioTimeStamp* now, const AudioBufferList* in, const AudioTimeStamp* inTime,
                        AudioBufferList* out, const AudioTimeStamp* outTime, void* client)
{
    (void)dev; (void)now;
    int which = (int)(intptr_t)client;
    if (which != T_MUSIC && g_t.mode != 2) classify_input(which, in, inTime);
    fill_output(which, out, outTime);
    return noErr;
}

// Abre los tres dispositivos `seconds` segundos. Si `during` no es NULL, se
// llama justo antes de detenerlos (con el audio aún en marcha).
static int run_devices(double seconds, void (*during)(void))
{
    AudioObjectID devs[3] = { avm_find_device(AVM_DEV_SMAART), avm_find_device(AVM_DEV_LISTENTO), avm_find_device(AVM_DEV_MUSIC) };
    AudioDeviceIOProcID ids[3] = { 0 };
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        OSStatus err = AudioDeviceCreateIOProcID(devs[i], io_proc, (void*)(intptr_t)i, &ids[i]);
        if (err) { print_err("crear IOProc", err); ok = 0; break; }
    }
    for (int i = 0; ok && i < 3; i++) {
        OSStatus err = AudioDeviceStart(devs[i], ids[i]);
        if (err) { print_err("arrancar dispositivo", err); ok = 0; }
    }
    if (ok) usleep((useconds_t)(seconds * 1e6));
    if (ok && during) during();
    for (int i = 0; i < 3; i++) {
        if (ids[i]) {
            AudioDeviceStop(devs[i], ids[i]);
            AudioDeviceDestroyIOProcID(devs[i], ids[i]);
        }
    }
    return ok;
}

static void reset_ctx(int mode, Float64 rate)
{
    memset(&g_t, 0, sizeof(g_t));
    g_t.mode = mode;
    g_t.rate = rate;
    for (int i = 0; i < 3; i++) g_t.first_in[i] = -1;
    g_t.first_out = -1;
}

// --- Comprobación de los medidores del driver (propiedad 'metr') -----------

static int g_meter_pass = 1;
static int g_meter_verbose = 0;

static void meter_expect(const char* what, const avm_meter_reading* r, float peak, int clip)
{
    int good = r->active && fabsf(r->peak - peak) <= 1e-6f && (clip < 0 || (r->clip != 0) == (clip != 0));
    if (!good) g_meter_pass = 0;
    if (g_meter_verbose || !good)
        printf("  %-6s medidor %-38s %s %+7.2f dBFS (esperado %+7.2f)%s\n", good ? "OK" : "FALLO", what,
               r->active ? "activo" : "INACTIVO", avm_peak_to_dbfs(r->peak), avm_peak_to_dbfs(peak),
               r->clip ? " SAT" : "");
}

static void meters_during_isolation(void)
{
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    if (avm_get_meters(s, d) != noErr) { printf("  FALLO  no se pudieron leer los medidores\n"); g_meter_pass = 0; return; }
    char name[64];
    for (int i = 0; i < 8; i++) {
        avm_source_name(AVM_SRC_SMAART_BASE + i, name, sizeof(name)); meter_expect(name, &s[AVM_SRC_SMAART_BASE + i], val_smaart(i), 0);
        avm_source_name(AVM_SRC_LINK_BASE + i, name, sizeof(name));   meter_expect(name, &s[AVM_SRC_LINK_BASE + i], fabsf(val_link(i)), 0);
    }
    avm_source_name(AVM_SRC_MUSIC_BASE, name, sizeof(name));     meter_expect(name, &s[AVM_SRC_MUSIC_BASE], kMusicL, 0);
    avm_source_name(AVM_SRC_MUSIC_BASE + 1, name, sizeof(name)); meter_expect(name, &s[AVM_SRC_MUSIC_BASE + 1], fabsf(kMusicR), 0);
    for (int i = 0; i < 8; i++) {
        avm_dest_name(AVM_DST_SMAART_BASE + i, name, sizeof(name)); meter_expect(name, &d[AVM_DST_SMAART_BASE + i], fabsf(expected_in(T_SMAART, i)), 0);
        avm_dest_name(AVM_DST_LINK_BASE + i, name, sizeof(name));   meter_expect(name, &d[AVM_DST_LINK_BASE + i], fabsf(expected_in(T_LINK, i)), 0);
    }
}

// Formato físico actual de un stream (Float32 si no se puede leer).
static avm_format physical_format_of(avm_device_kind k, bool input)
{
    AudioStreamBasicDescription asbd;
    if (avm_get_physical_format(k, input, &asbd) == noErr) {
        int f = avm_format_from_asbd(&asbd);
        if (f >= 0) return (avm_format)f;
    }
    return AVM_FMT_FLOAT32;
}

static void meters_during_clip(void)
{
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    if (avm_get_meters(s, d) != noErr) { printf("  FALLO  no se pudieron leer los medidores\n"); g_meter_pass = 0; return; }
    // Valores esperados exactos según los formatos físicos reales: lo que envía
    // la prueba pasa por el formato de Smaart (salidas) antes de llegar al driver,
    // y el destino suma esos valores ya cuantizados y se entrega en el formato de
    // LISTENTO (entradas). Ejemplo SInt16: 0,6 -> 19661/32768 = 0,600006104.
    const avm_format src_fmt = physical_format_of(AVM_DEV_SMAART, false);
    const avm_format dst_fmt = physical_format_of(AVM_DEV_LISTENTO, true);
    const float q0 = avm_quantize(kClipOut[0], src_fmt);
    const float q1 = avm_quantize(kClipOut[1], src_fmt);
    const float q2 = avm_quantize(kClipOut[2], src_fmt);
    const float dst_clip = avm_format_delivery_clip_level(dst_fmt);
    meter_expect("Smaart Salida 1 a 0 dBFS (SAT)", &s[AVM_SRC_SMAART_BASE + 0], q0, 1);
    meter_expect("Smaart Salida 2 a 0,6 (sin SAT)", &s[AVM_SRC_SMAART_BASE + 1], q1, 0);
    meter_expect("Smaart Salida 3 a 0,6 (sin SAT)", &s[AVM_SRC_SMAART_BASE + 2], q2, 0);
    meter_expect("LISTENTO Envío 1 = Salida 1", &d[AVM_DST_LINK_BASE + 0], q0, q0 >= dst_clip);
    meter_expect("LISTENTO Envío 2 = 0,6 + 0,6 (SAT)", &d[AVM_DST_LINK_BASE + 1], q1 + q2, 1);
    meter_expect("LISTENTO Envío 3 sin ruta (silencio)", &d[AVM_DST_LINK_BASE + 2], 0.0f, 0);
}

// Tras detener el audio, ningún medidor puede seguir mostrando un valor.
static void meters_after_stop(int verbose)
{
    usleep(400000);
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    if (avm_get_meters(s, d) != noErr) { printf("  FALLO  no se pudieron leer los medidores\n"); g_meter_pass = 0; return; }
    int stale = 0;
    for (int i = 0; i < AVM_NUM_SOURCES; i++) if (s[i].active || s[i].peak != 0) stale++;
    for (int i = 0; i < AVM_NUM_DESTS; i++) if (d[i].active || d[i].peak != 0) stale++;
    if (stale) { g_meter_pass = 0; printf("  FALLO  %d medidores siguen mostrando nivel tras detener el audio\n", stale); }
    else if (verbose) printf("  OK     al detener el audio, los 34 medidores pasan a inactivo\n");
}

// Devuelve 1 si todo pasó.
static int run_test_once(int verbose)
{
    Float64 rate = 48000;
    avm_get_sample_rate(&rate);
    int pass = 1;

    // 1) Regla anti-lazo: la matriz con retorno->envío de LISTENTO debe rechazarse.
    avm_matrix bad;
    avm_matrix_default(&bad);
    bad.cell[AVM_DST_LINK_BASE][AVM_SRC_LINK_BASE] = 1;
    OSStatus err = avm_set_matrix(&bad);
    if (err == noErr) { printf("  FALLO  el driver aceptó un lazo LISTENTO retorno -> envío\n"); pass = 0; }
    else if (verbose) printf("  OK     el driver rechaza el lazo LISTENTO retorno -> envío\n");

    // 2) Aislamiento y música con valores exactos por canal.
    avm_matrix m;
    avm_matrix_clear(&m);
    for (int i = 0; i < 8; i++) m.cell[AVM_DST_LINK_BASE + i][AVM_SRC_SMAART_BASE + i] = 1;
    for (int i = 0; i < 6; i++) m.cell[AVM_DST_SMAART_BASE + i][AVM_SRC_LINK_BASE + i] = 1;
    m.cell[AVM_DST_SMAART_BASE + 6][AVM_SRC_MUSIC_BASE + 0] = 1;
    m.cell[AVM_DST_SMAART_BASE + 7][AVM_SRC_MUSIC_BASE + 1] = 1;
    m.cell[AVM_DST_SMAART_BASE + 0][AVM_SRC_LINK_BASE + 1] = 1;   // suma en smaart.in.1
    // listento.out.7 y .8 quedan sin ruta: no deben aparecer en ningún destino.
    if ((err = avm_set_matrix(&m))) { print_err("escribir matriz de prueba", err); return 0; }

    reset_ctx(0, rate);
    g_meter_pass = 1;
    g_meter_verbose = verbose;
    if (!run_devices(3.5, meters_during_isolation)) return 0;
    const char* dn[2] = { "Smaart entrada", "LISTENTO envío" };
    for (int di = 0; di < 2; di++)
        for (int c = 0; c < 8; c++) {
            long ok = atomic_load(&g_t.correct[di][c]), z = atomic_load(&g_t.silent[di][c]), w = atomic_load(&g_t.wrong[di][c]);
            int good = ok > 0 && z == 0 && w == 0;
            if (!good) pass = 0;
            if (verbose || !good)
                printf("  %-6s %s %d: %ld correctas, %ld en silencio, %ld con señal ajena\n",
                       good ? "OK" : "FALLO", dn[di], c + 1, ok, z, w);
        }

    meters_after_stop(verbose);

    // 2b) Medidores con saturación: fuente a 0 dBFS y destino que suma 0,6 + 0,6.
    avm_matrix_clear(&m);
    m.cell[AVM_DST_LINK_BASE + 0][AVM_SRC_SMAART_BASE + 0] = 1;
    m.cell[AVM_DST_LINK_BASE + 1][AVM_SRC_SMAART_BASE + 1] = 1;
    m.cell[AVM_DST_LINK_BASE + 1][AVM_SRC_SMAART_BASE + 2] = 1;
    if ((err = avm_set_matrix(&m))) { print_err("escribir matriz de prueba", err); return 0; }
    reset_ctx(2, rate);
    if (!run_devices(1.5, meters_during_clip)) return 0;
    meters_after_stop(verbose);
    if (!g_meter_pass) pass = 0;

    // 3) Retardo de transporte con un impulso (informativo; no es un objetivo de latencia).
    avm_matrix_clear(&m);
    m.cell[AVM_DST_LINK_BASE + 0][AVM_SRC_SMAART_BASE + 0] = 1;
    m.cell[AVM_DST_SMAART_BASE + 0][AVM_SRC_SMAART_BASE + 0] = 1;
    if ((err = avm_set_matrix(&m))) { print_err("escribir matriz de prueba", err); return 0; }
    reset_ctx(1, rate);
    if (!run_devices(2.5, NULL)) return 0;
    if (atomic_load(&g_t.got_link) && atomic_load(&g_t.got_smaart)) {
        double dl = g_t.t_link - g_t.impulse_time, ds = g_t.t_smaart - g_t.impulse_time;
        printf("  INFO   retardo Smaart salida 1 -> LISTENTO envío 1: %.0f frames (%.2f ms)\n", dl, 1000.0 * dl / rate);
        printf("  INFO   retardo Smaart salida 1 -> Smaart entrada 1: %.0f frames (%.2f ms)\n", ds, 1000.0 * ds / rate);
    } else {
        printf("  FALLO  el impulso no llegó (enviado=%d, LISTENTO=%d, Smaart=%d)\n",
               atomic_load(&g_t.impulse_sent), atomic_load(&g_t.got_link), atomic_load(&g_t.got_smaart));
        pass = 0;
    }
    return pass;
}

static int cmd_test(int all)
{
    avm_matrix saved;
    OSStatus err = avm_get_matrix(&saved);
    if (err) { print_err("leer matriz", err); return 1; }
    Float64 saved_rate = 48000;
    avm_get_sample_rate(&saved_rate);
    AudioStreamBasicDescription f_s_in, f_s_out, f_l_in, f_l_out, f_m_out;
    avm_get_physical_format(AVM_DEV_SMAART, true, &f_s_in);
    avm_get_physical_format(AVM_DEV_SMAART, false, &f_s_out);
    avm_get_physical_format(AVM_DEV_LISTENTO, true, &f_l_in);
    avm_get_physical_format(AVM_DEV_LISTENTO, false, &f_l_out);
    avm_get_physical_format(AVM_DEV_MUSIC, false, &f_m_out);

    printf("Cierre Smaart, LISTENTO y la música durante la prueba: el driver mezcla lo que envíen.\n");
    int total_pass = 1;
    if (!all) {
        printf("Prueba a %.0f Hz:\n", saved_rate);
        total_pass = run_test_once(1);
    } else {
        const Float64 rates[3] = { 44100, 48000, 96000 };
        for (int r = 0; r < 3; r++) {
            if (avm_set_sample_rate(rates[r]) || !wait_for_rate(rates[r])) { printf("FALLO al cambiar a %.0f Hz\n", rates[r]); total_pass = 0; continue; }
            for (int f = 0; f < AVM_FMT_COUNT; f++) {
                int ok = set_format_checked(AVM_DEV_SMAART, true, (avm_format)f)
                      && set_format_checked(AVM_DEV_SMAART, false, (avm_format)f)
                      && set_format_checked(AVM_DEV_LISTENTO, true, (avm_format)f)
                      && set_format_checked(AVM_DEV_LISTENTO, false, (avm_format)f)
                      && set_format_checked(AVM_DEV_MUSIC, false, (avm_format)f);
                printf("== %.0f Hz, %s ==\n", rates[r], avm_format_name((avm_format)f));
                int p = ok && run_test_once(0);
                printf("   %s\n", p ? "CORRECTO" : "FALLO");
                if (!p) total_pass = 0;
            }
        }
    }

    // Restaurar el estado del usuario.
    avm_set_sample_rate(saved_rate);
    wait_for_rate(saved_rate);
    int fs[5] = { avm_format_from_asbd(&f_s_in), avm_format_from_asbd(&f_s_out), avm_format_from_asbd(&f_l_in),
                  avm_format_from_asbd(&f_l_out), avm_format_from_asbd(&f_m_out) };
    if (fs[0] >= 0) avm_set_physical_format(AVM_DEV_SMAART, true, (avm_format)fs[0]);
    if (fs[1] >= 0) avm_set_physical_format(AVM_DEV_SMAART, false, (avm_format)fs[1]);
    if (fs[2] >= 0) avm_set_physical_format(AVM_DEV_LISTENTO, true, (avm_format)fs[2]);
    if (fs[3] >= 0) avm_set_physical_format(AVM_DEV_LISTENTO, false, (avm_format)fs[3]);
    if (fs[4] >= 0) avm_set_physical_format(AVM_DEV_MUSIC, false, (avm_format)fs[4]);
    if ((err = avm_set_matrix(&saved))) print_err("restaurar matriz", err);
    else printf("Matriz, frecuencia y formatos restaurados.\n");

    printf("RESULTADO: %s\n", total_pass ? "CORRECTO" : "HAY FALLOS");
    return total_pass ? 0 : 1;
}

// ---------------------------------------------------------------------------

static void usage(void)
{
    printf("avmctl — control de AV Matrix\n\n"
           "  avmctl status\n"
           "  avmctl show\n"
           "  avmctl connect <fuente> <destino>\n"
           "  avmctl disconnect <fuente> <destino>\n"
           "  avmctl preset default|direct|clear   (default = medición, Entrada 8 = referencia local)\n"
           "  avmctl rate 44100|48000|96000\n"
           "  avmctl recover\n"
           "  avmctl meters [--watch]\n"
           "  avmctl format <smaart|listento|music> <in|out> <f32|s32|s24|s16>\n"
           "  avmctl test\n"
           "  avmctl test-all\n\n"
           "Fuentes:  smaart.out.1-8  listento.out.1-8  music.L  music.R\n"
           "Destinos: smaart.in.1-8   listento.in.1-8\n");
}

int main(int argc, char** argv)
{
    // Notificaciones de CoreAudio en su propio hilo (no hay run loop en una herramienta de Terminal).
    CFRunLoopRef rl = NULL;
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyRunLoop, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectSetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, sizeof(rl), &rl);

    if (argc < 2) { usage(); return 2; }
    const char* c = argv[1];
    if (!strcmp(c, "status")) return cmd_status();
    if (!require_installed()) return 1;
    if (!strcmp(c, "show")) return cmd_show();
    if (!strcmp(c, "connect") && argc == 4) return cmd_connect(argv[2], argv[3], 1);
    if (!strcmp(c, "disconnect") && argc == 4) return cmd_connect(argv[2], argv[3], 0);
    if (!strcmp(c, "preset") && argc == 3) return cmd_preset(argv[2]);
    if (!strcmp(c, "rate") && argc == 3) return cmd_rate(argv[2]);
    if (!strcmp(c, "recover")) return cmd_recover();
    if (!strcmp(c, "meters")) return cmd_meters(argc == 3 && !strcmp(argv[2], "--watch"));
    if (!strcmp(c, "format") && argc == 5) return cmd_format(argv[2], argv[3], argv[4]);
    if (!strcmp(c, "test")) return cmd_test(0);
    if (!strcmp(c, "test-all")) return cmd_test(1);
    usage();
    return 2;
}
