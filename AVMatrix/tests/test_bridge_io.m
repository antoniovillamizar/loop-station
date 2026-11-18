// Prueba nativa (solo macOS) del callback REAL del puente, bridge_io, con
// AudioBufferList de CoreAudio: capacidades distintas entre buffers, tamaño 0,
// punteros NULL y forma distinta del plan (revisión 15). No abre dispositivos.
// Cada buffer se reserva con su tamaño exacto y una zona de guarda detrás.
// Compilar con -fsanitize=address para que ASan vigile además las escrituras.

#include "../Client/avm_bridge.c"

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FALLO: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define GUARD 64

static AudioBufferList* list_make(UInt32 n, const UInt32* ch, const UInt32* frames, int null_mask)
{
    AudioBufferList* l = (AudioBufferList*)calloc(1, offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * (n ? n : 1));
    l->mNumberBuffers = n;
    for (UInt32 i = 0; i < n; i++) {
        UInt32 bytes = frames[i] * ch[i] * (UInt32)sizeof(float);
        l->mBuffers[i].mNumberChannels = ch[i];
        l->mBuffers[i].mDataByteSize = bytes;
        if (!(null_mask & (1 << i))) {
            uint8_t* raw = (uint8_t*)malloc(bytes + GUARD);
            for (UInt32 k = 0; k < bytes / sizeof(float); k++) ((float*)raw)[k] = 9.0f;
            memset(raw + bytes, 0xA5, GUARD);
            l->mBuffers[i].mData = raw;
        }
    }
    return l;
}

static int list_guards_ok(const AudioBufferList* l)
{
    for (UInt32 i = 0; i < l->mNumberBuffers; i++) {
        const uint8_t* d = (const uint8_t*)l->mBuffers[i].mData;
        if (!d) continue;
        for (int k = 0; k < GUARD; k++) if (d[l->mBuffers[i].mDataByteSize + (UInt32)k] != 0xA5) return 0;
    }
    return 1;
}

static int buf_zero(const AudioBufferList* l, UInt32 i)
{
    const float* d = (const float*)l->mBuffers[i].mData;
    if (!d) return 1;
    for (UInt32 k = 0; k < l->mBuffers[i].mDataByteSize / sizeof(float); k++) if (d[k] != 0.0f) return 0;
    return 1;
}

static void list_free(AudioBufferList* l)
{
    for (UInt32 i = 0; i < l->mNumberBuffers; i++) free(l->mBuffers[i].mData);
    free(l);
}

static OSStatus call_io(avm_bridge* b, const AudioBufferList* in, AudioBufferList* out)
{
    AudioTimeStamp t;
    memset(&t, 0, sizeof(t));
    return bridge_io(0, &t, in, &t, out, &t, b);
}

int main(void)
{
    avm_bridge b;
    memset(&b, 0, sizeof(b));
    avm_bridge_config c;
    avm_bridge_config_default(&c, AVM_BRIDGE_DANTE);
    c.channel[0] = 1; c.channel[1] = 3;
    uint32_t in_b[1] = { 2 }, out_b[2] = { 2, 2 };
    char why[128];
    CHECK(avm_bridge_make_plan(&c, in_b, 1, out_b, 2, 2, 0, 0, 4, &b.plan, why, sizeof(why)), "plan (%s)", why);
    b.plan.fade_pos = AVM_BRIDGE_FADE_FRAMES;

    // Caso de Codex: salida 0 de 256 fotogramas, salida 1 de 128.
    UInt32 ich[1] = { 2 }, ifr[1] = { 256 }, och[2] = { 2, 2 }, ofr[2] = { 256, 128 };
    AudioBufferList* in = list_make(1, ich, ifr, 0);
    AudioBufferList* out = list_make(2, och, ofr, 0);
    for (int f = 0; f < 256; f++) { ((float*)in->mBuffers[0].mData)[f * 2] = 0.25f; ((float*)in->mBuffers[0].mData)[f * 2 + 1] = -0.5f; }
    CHECK(call_io(&b, in, out) == noErr, "bridge_io devuelve noErr");
    CHECK(list_guards_ok(out) && list_guards_ok(in), "256 y 128 fotogramas: ninguna escritura fuera de los buffers");
    const float* o0 = (const float*)out->mBuffers[0].mData;
    const float* o1 = (const float*)out->mBuffers[1].mData;
    CHECK(o0[0] == 0.25f && o1[0] == -0.5f && o0[127 * 2] == 0.25f && o1[127 * 2] == -0.5f && o0[128 * 2] == 0.0f, "copia hasta 128 y silencio después");
    list_free(out);

    // Tamaño 0 y NULL.
    UInt32 ofr0[2] = { 0, 256 };
    out = list_make(2, och, ofr0, 0);
    call_io(&b, in, out);
    CHECK(list_guards_ok(out) && buf_zero(out, 1), "salida de tamaño 0: no se escribe y la otra queda en silencio");
    list_free(out);
    UInt32 ofr2[2] = { 256, 256 };
    out = list_make(2, och, ofr2, 1);
    AudioBufferList* in_null = list_make(1, ich, ifr, 1);
    call_io(&b, in_null, out);
    CHECK(list_guards_ok(out) && buf_zero(out, 1), "salida NULL ignorada y entrada NULL = silencio");
    CHECK(call_io(&b, NULL, NULL) == noErr, "listas NULL: sin fallo");
    list_free(out); list_free(in_null);

    // Forma distinta (8 + 8 canales, tamaños distintos): silencio dentro de cada buffer y aviso.
    atomic_store(&b.plan.shape_mismatch, 0);
    UInt32 och8[2] = { 8, 8 }, ofr8[2] = { 256, 16 };
    out = list_make(2, och8, ofr8, 0);
    call_io(&b, in, out);
    CHECK(list_guards_ok(out) && buf_zero(out, 0) && buf_zero(out, 1) && atomic_load(&b.plan.shape_mismatch),
          "forma distinta: silencio dentro de cada buffer y aviso para reconstruir");
    list_free(out); list_free(in);

    printf("Callback del puente (Mac): %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
