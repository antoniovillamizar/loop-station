// Prueba nativa (solo macOS) de la ventana «Mixer»:
//   1. dos secciones con las 34 tiras (18 entradas a la matriz, 16 salidas);
//   2. cada tira: número, nombre personalizado, medidor, fader con lectura en dB
//      y botón MUTE;
//   3. el fader solo atenúa (0 dB arriba, silencio abajo) y vuelve a 0 dB con
//      doble clic; MUTE conserva el volumen; «Silenciar todos» y «Restaurar»;
//   4. los envíos al driver se agrupan (no uno por evento del fader);
//   5. botón «Mixer» en la ventana principal.
//
// El controlador se usa en modo `offline`: nunca habla con el driver, así la
// prueba no cambia el audio del Mac. Incluye la app tal cual (sin su main).

#define AVM_APP_NO_MAIN
#ifndef AVM_APP_FILE
#define AVM_APP_FILE "../App/AVMatrixApp.m"
#endif
#include AVM_APP_FILE

@interface AVMController (Pruebas)
- (void)buildWindow;
- (void)showMixer:(id)sender;
@end

static int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FALLO: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void pump(double seconds)
{
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([until timeIntervalSinceNow] > 0)
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
}

static void collect(NSView* v, Class cls, NSMutableArray* out)
{
    if ([v isKindOfClass:cls]) [out addObject:v];
    for (NSView* s in v.subviews) collect(s, cls, out);
}

static BOOL hasText(NSView* root, NSString* t)
{
    NSMutableArray* fs = [NSMutableArray array];
    collect(root, [NSTextField class], fs);
    for (NSTextField* f in fs) if ([f.stringValue isEqualToString:t]) return YES;
    return NO;
}

static void test_layout(AVMMixerController* mc)
{
    NSView* content = mc.window.contentView;
    CHECK(hasText(content, @"ENTRADAS A LA MATRIZ") && hasText(content, @"SALIDAS DE LA MATRIZ"), "dos secciones: entradas y salidas de la matriz");
    for (NSString* g in @[ @"Smaart · Salidas", @"LISTENTO · Retornos", @"Música", @"Smaart · Entradas", @"LISTENTO · Envíos" ])
        CHECK(hasText(content, g), "grupo «%s»", g.UTF8String);
    NSMutableArray* strips = [NSMutableArray array];
    collect(content, [AVMMixerStrip class], strips);
    CHECK(strips.count == AVM_NAMES_COUNT, "34 tiras (%lu)", (unsigned long)strips.count);

    int complete = 0;
    for (int i = 0; i < AVM_NAMES_COUNT; i++) {
        AVMMixerStrip* s = [mc stripForIndex:i];
        BOOL ok = s && [s.number.stringValue isEqualToString:ChannelNumber(i)]
               && s.name.stringValue.length > 0 && s.meter && s.fader.vertical
               && s.fader.minValue == 0.0 && s.fader.maxValue == 1.0 && s.fader.doubleValue == 1.0
               && [s.gainLabel.stringValue isEqualToString:@"0.0 dB"]
               && [s.mute.title isEqualToString:@"MUTE"] && s.mute.state == NSControlStateValueOff;
        if (ok) complete++;
    }
    CHECK(complete == AVM_NAMES_COUNT, "cada tira: número, nombre, medidor, fader a 0 dB con lectura y MUTE apagado (%d de 34)", complete);
    CHECK([[mc stripForIndex:avm_names_src_index(AVM_SRC_MUSIC_BASE + 1)].number.stringValue isEqualToString:@"R"], "Música R numerada «R»");
    NSButton* mute = [mc stripForIndex:0].mute;
    CHECK(NSWidth(mute.frame) >= 40 && NSHeight(mute.frame) >= 20, "botón MUTE de tamaño claro (%.0f×%.0f)", NSWidth(mute.frame), NSHeight(mute.frame));

    // Ninguna tira se solapa con otra dentro de su grupo.
    BOOL overlap = NO;
    for (int i = 0; i + 1 < AVM_NAMES_COUNT; i++) {
        AVMMixerStrip* a = [mc stripForIndex:i]; AVMMixerStrip* b = [mc stripForIndex:i + 1];
        if (a.superview == b.superview && NSIntersectsRect(a.frame, b.frame)) overlap = YES;
    }
    CHECK(!overlap, "las tiras no se solapan");
}

static void test_names(AVMMixerController* mc)
{
    avm_names saved = gNames;
    int ref = avm_names_dst_index(AVM_DST_LOCAL_REFERENCE);
    avm_names_set(&gNames, ref, "Señal de referencia");
    [mc namesChanged];
    AVMMixerStrip* s = [mc stripForIndex:ref];
    CHECK([s.name.stringValue isEqualToString:@"Señal de referencia"] && [s.number.stringValue isEqualToString:@"8"],
          "nombre personalizado con tildes y número visible: «%s»", s.name.stringValue.UTF8String);
    CHECK([s.name.toolTip containsString:@"Smaart Entrada 8"], "la ayuda emergente da el canal completo");
    gNames = saved;
    [mc namesChanged];
    CHECK([[mc stripForIndex:ref].name.stringValue isEqualToString:@"Entrada 8"], "al restaurar vuelve el nombre original");
}

static void test_controls(AVMMixerController* mc)
{
    int s2 = avm_names_src_index(AVM_SRC_SMAART_BASE + 1);
    int e4 = avm_names_dst_index(AVM_DST_LINK_BASE + 3);
    AVMMixerStrip* a = [mc stripForIndex:s2];
    AVMMixerStrip* b = [mc stripForIndex:e4];
    NSUInteger sends0 = mc.sendCount;

    // Fader de una fuente a media altura: ganancia = 0,5³ = 0,125 (−18,1 dB).
    a.fader.doubleValue = 0.5;
    [mc faderMoved:a.fader];
    avm_mixer mx = [mc mixer];
    CHECK(mx.src_gain[AVM_SRC_SMAART_BASE + 1] == 0.125f, "fader de entrada: ganancia %g (esperada 0,125)", (double)mx.src_gain[AVM_SRC_SMAART_BASE + 1]);
    CHECK([a.gainLabel.stringValue isEqualToString:@"-18.1 dB"], "lectura del fader: «%s»", a.gainLabel.stringValue.UTF8String);
    int others = 1;
    for (int i = 0; i < AVM_NUM_SOURCES; i++) if (i != AVM_SRC_SMAART_BASE + 1 && mx.src_gain[i] != 1.0f) others = 0;
    for (int d = 0; d < AVM_NUM_DESTS; d++) if (mx.dst_gain[d] != 1.0f) others = 0;
    CHECK(others, "los demás canales siguen a 0 dB");

    // Muchos movimientos seguidos: se agrupan en pocos envíos.
    for (int k = 0; k < 50; k++) { b.fader.doubleValue = 1.0 - k / 100.0; [mc faderMoved:b.fader]; }
    pump(0.1);
    NSUInteger sends = mc.sendCount - sends0;
    CHECK(sends >= 1 && sends <= 3, "50 movimientos del fader -> %lu envíos al driver (agrupados)", (unsigned long)sends);
    mx = [mc mixer];
    CHECK(mx.dst_gain[AVM_DST_LINK_BASE + 3] == avm_fader_gain_from_pos(0.51), "se envía el último valor del fader de salida");

    // Abajo del todo: silencio; arriba: 0 dB exactos. Nunca por encima de 0 dB.
    b.fader.doubleValue = 0.0; [mc faderMoved:b.fader];
    CHECK([mc mixer].dst_gain[AVM_DST_LINK_BASE + 3] == 0.0f && [b.gainLabel.stringValue isEqualToString:@"−∞"], "fader abajo: silencio («−∞»)");
    b.fader.doubleValue = 0.9991; [mc faderMoved:b.fader];
    CHECK([mc mixer].dst_gain[AVM_DST_LINK_BASE + 3] == 1.0f && [b.gainLabel.stringValue isEqualToString:@"0.0 dB"], "fader arriba: 0 dB exactos");
    b.fader.doubleValue = 5.0; [mc faderMoved:b.fader];
    CHECK([mc mixer].dst_gain[AVM_DST_LINK_BASE + 3] <= 1.0f, "el fader no amplifica");
    CHECK(b.fader.maxValue == 1.0, "el recorrido termina en 0 dB");

    // MUTE: conserva el volumen; al quitarlo vuelve al mismo nivel.
    NSUInteger before = mc.sendCount;
    a.mute.state = NSControlStateValueOn; [mc muteToggled:a.mute];
    mx = [mc mixer];
    CHECK(mx.src_mute[AVM_SRC_SMAART_BASE + 1] == 1 && mx.src_gain[AVM_SRC_SMAART_BASE + 1] == 0.125f, "MUTE activo y el volumen se conserva");
    CHECK(mc.sendCount > before, "el MUTE se envía al momento");
    CHECK(a.mute.bezelColor != nil, "MUTE activo resaltado en rojo");
    a.mute.state = NSControlStateValueOff; [mc muteToggled:a.mute];
    mx = [mc mixer];
    CHECK(mx.src_mute[AVM_SRC_SMAART_BASE + 1] == 0 && mx.src_gain[AVM_SRC_SMAART_BASE + 1] == 0.125f && a.mute.bezelColor == nil,
          "quitar el MUTE vuelve al volumen anterior");

    // Doble clic en el fader: 0 dB.
    a.fader.doubleValue = 1.0; [mc faderMoved:a.fader];
    CHECK([mc mixer].src_gain[AVM_SRC_SMAART_BASE + 1] == 1.0f, "fader de vuelta a 0 dB");

    // Silenciar todos y restaurar.
    b.fader.doubleValue = 0.6; [mc faderMoved:b.fader];
    [mc muteAll:nil];
    mx = [mc mixer];
    int allMuted = 1;
    for (int i = 0; i < AVM_NUM_SOURCES; i++) if (!mx.src_mute[i]) allMuted = 0;
    for (int d = 0; d < AVM_NUM_DESTS; d++) if (!mx.dst_mute[d]) allMuted = 0;
    CHECK(allMuted, "«Silenciar todos»: los 34 canales en MUTE");
    CHECK(mx.dst_gain[AVM_DST_LINK_BASE + 3] == avm_fader_gain_from_pos(0.6), "«Silenciar todos» conserva los volúmenes");
    int red = 0;
    for (int i = 0; i < AVM_NAMES_COUNT; i++) if ([mc stripForIndex:i].mute.state == NSControlStateValueOn) red++;
    CHECK(red == AVM_NAMES_COUNT, "los 34 botones MUTE encendidos");
    [mc restoreUnityConfirmed];
    mx = [mc mixer];
    CHECK(avm_mixer_is_unity(&mx), "«Restaurar 0 dB y quitar mutes»: todo a 0 dB sin MUTE");
    int labels = 0;
    for (int i = 0; i < AVM_NAMES_COUNT; i++) {
        AVMMixerStrip* s = [mc stripForIndex:i];
        if ([s.gainLabel.stringValue isEqualToString:@"0.0 dB"] && s.mute.state == NSControlStateValueOff && s.fader.doubleValue == 1.0) labels++;
    }
    CHECK(labels == AVM_NAMES_COUNT, "las 34 tiras muestran 0 dB sin MUTE");
    CHECK(avm_mixer_is_valid(&mx), "los ajustes enviados son siempre válidos para el driver");
}

static void test_meters(AVMMixerController* mc)
{
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    memset(s, 0, sizeof(s)); memset(d, 0, sizeof(d));
    s[AVM_SRC_SMAART_BASE].active = true; s[AVM_SRC_SMAART_BASE].peak = 0.5f;
    d[AVM_DST_LINK_BASE + 3].active = true; d[AVM_DST_LINK_BASE + 3].peak = 0.25f; d[AVM_DST_LINK_BASE + 3].clip = true;
    [mc setMeterSources:s dests:d];
    AVMMixerStrip* a = [mc stripForIndex:0];
    AVMMixerStrip* b = [mc stripForIndex:avm_names_dst_index(AVM_DST_LINK_BASE + 3)];
    CHECK(a.meter.reading.active && a.meter.reading.peak == 0.5f && [a.peakLabel.stringValue isEqualToString:@"-6.0"], "medidor de entrada: «%s»", a.peakLabel.stringValue.UTF8String);
    CHECK(b.meter.reading.clip && b.meter.reading.peak == 0.25f, "medidor de salida con saturación");
    CHECK([[mc stripForIndex:5].peakLabel.stringValue isEqualToString:@"—"], "canal sin audio: «—»");
    [mc setMeterSources:NULL dests:NULL];
    CHECK(!a.meter.reading.active && [a.peakLabel.stringValue isEqualToString:@"—"], "sin driver: todos los medidores en «—»");
}

static void test_main_button(void)
{
    AVMController* c = [[AVMController alloc] init];
    [c buildWindow];
    NSMutableArray* bs = [NSMutableArray array];
    collect(c.window.contentView, [NSButton class], bs);
    NSButton* mixerBtn = nil;
    for (NSButton* b in bs) if ([b.title isEqualToString:@"Mixer"]) mixerBtn = b;
    CHECK(mixerBtn && mixerBtn.action == @selector(showMixer:) && mixerBtn.target == c, "botón «Mixer» en la ventana principal");
    // Se abre en modo offline para no tocar el driver.
    c.mixer = [[AVMMixerController alloc] init];
    c.mixer.offline = YES;
    [c showMixer:nil];
    CHECK(c.mixer.window != nil, "el botón abre la ventana del mixer");
    CHECK(c.mixer.window.styleMask & NSWindowStyleMaskResizable, "ventana del mixer redimensionable");
    [c.mixer.window close];
}

int main(void)
{
    @autoreleasepool {
        avm_names_init(&gNames);
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        AVMMixerController* mc = [[AVMMixerController alloc] init];
        mc.offline = YES;
        [mc show];
        if (!getenv("AVM_UI_NO_WINDOW")) pump(0.2); else [mc.window orderOut:nil];
        test_layout(mc);
        test_names(mc);
        test_controls(mc);
        test_meters(mc);
        test_main_button();
        [mc.window close];
    }
    printf("Mixer (ventana): %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
