// Prueba nativa (solo macOS) de la pantalla principal simplificada y de la
// ventana de Ayuda:
//   1. receptores compactos: la columna izquierda mide kLeftW (≤ 280 pt), con
//      número, nombre, barra, dBFS y saturación en orden y sin solaparse;
//   2. sin el distintivo «REFERENCIA LOCAL»: la Entrada 8 se renombra igual y
//      la ayuda emergente muestra el nombre completo de cualquier canal;
//   3. la leyenda ya no está en la ventana principal; hay botón «Ayuda» y menú
//      Ayuda → Manual de AV Matrix;
//   4. la ventana del manual es redimensionable, tiene índice y desplazamiento,
//      y cada sección del índice lleva a su título.
//
// Incluye la app tal cual (sin su main). No escribe en el archivo de nombres
// ni cambia la matriz del driver.
// AVM_UI_NO_WINDOW=1 omite lo que necesita mostrar ventanas.

#define AVM_APP_NO_MAIN
#ifndef AVM_APP_FILE
#define AVM_APP_FILE "../App/AVMatrixApp.m"
#endif
#include AVM_APP_FILE

// Métodos internos de la app que usa la prueba.
@interface AVMMatrixView (Pruebas)
- (int)channelNameIndexAt:(NSPoint)p;
- (NSString*)view:(NSView*)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)point userData:(void*)data;
@end
@interface AVMController (Pruebas)
- (void)buildMenu;
- (void)buildWindow;
- (void)showHelp:(id)sender;
@end

static int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FALLO: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void pump(double seconds)
{
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([until timeIntervalSinceNow] > 0)
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
}

static void collect(NSView* v, Class cls, NSMutableArray* out)
{
    if ([v isKindOfClass:cls]) [out addObject:v];
    for (NSView* s in v.subviews) collect(s, cls, out);
}

static NSButton* buttonTitled(NSView* root, NSString* title)
{
    NSMutableArray* bs = [NSMutableArray array];
    collect(root, [NSButton class], bs);
    for (NSButton* b in bs) if ([b.title isEqualToString:title]) return b;
    return nil;
}

static void test_compact_geometry(void)
{
    CHECK(kLeftW <= 280.0, "zona de receptores compacta: %.0f pt (antes 386)", kLeftW);
    // Orden y separación: número | nombre | barra | dBFS | SAT, todo dentro de kLeftW.
    CHECK(kRxNameX >= 22 && kRxNameMaxX > kRxNameX + 100, "nombre visible con al menos 100 pt (%.0f)", kRxNameMaxX - kRxNameX);
    CHECK(kRxNameMaxX <= kRxMeterX - 4, "el nombre no pisa la barra");
    CHECK(kRxMeterX + kRxMeterW <= kRxDbX, "la barra no pisa el dBFS");
    CHECK(kRxDbX + kRxDbW <= kRxClipX, "el dBFS no pisa el LED de saturación");
    CHECK(kRxClipX + 8 <= kLeftW - 4, "el LED de saturación cabe antes de la matriz");

    // El dBFS más ancho («+12.3» y «−∞») cabe en su columna.
    NSDictionary* da = @{ NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:10 weight:NSFontWeightRegular] };
    CHECK([@"+12.3" sizeWithAttributes:da].width <= kRxDbW && [@"-60.0" sizeWithAttributes:da].width <= kRxDbW, "dBFS cabe en %.0f pt", kRxDbW);

    AVMMatrixView* mv = [[AVMMatrixView alloc] initWithFrame:NSZeroRect];
    [mv expandAll:YES];
    // 3 cabeceras + 18 transmisores; 2 cabeceras + 16 receptores.
    CGFloat w = kLeftW + 21 * kCell + kSumW, h = kTopH + 18 * kCell + 1;
    CHECK(fabs(mv.frame.size.width - w) < 0.5 && fabs(mv.frame.size.height - h) < 0.5,
          "tamaño de la matriz desplegada %.0f×%.0f (esperado %.0f×%.0f)", mv.frame.size.width, mv.frame.size.height, w, h);
}

static void test_names_and_tooltips(void)
{
    avm_names saved = gNames;
    int ref = avm_names_dst_index(AVM_DST_LOCAL_REFERENCE);
    const char* longAlias = "Señal de referencia con nombre l";   // 32 caracteres
    avm_names_set(&gNames, ref, longAlias);
    NSString* alias = UTF8(longAlias);

    AVMMatrixView* mv = [[AVMMatrixView alloc] initWithFrame:NSZeroRect];
    [mv expandAll:YES];
    // Fila de la Entrada 8: cabecera Smaart (fila 0) + 8 entradas.
    NSPoint p = NSMakePoint(kRxNameX + 10, kTopH + 8 * kCell + kCell / 2);
    CHECK([mv channelNameIndexAt:p] == ref, "el nombre de la Entrada 8 sigue siendo editable (doble clic / menú)");
    NSString* tip = [mv view:mv stringForToolTip:0 point:p userData:NULL];
    CHECK([tip containsString:alias], "ayuda emergente con el nombre completo: «%s»", tip.UTF8String);
    CHECK([tip hasPrefix:@"8 · "], "la ayuda emergente incluye el número del canal");
    CHECK([tip containsString:@"Smaart Entrada 8"], "la ayuda emergente incluye el dispositivo y el canal original");
    CHECK(![tip containsString:@"REFERENCIA LOCAL"], "sin el rótulo en mayúsculas");

    NSDictionary* chanAttr = @{ NSFontAttributeName: [NSFont systemFontOfSize:11] };
    NSString* shown = FitString(RxChannelName(AVM_DST_LOCAL_REFERENCE), chanAttr, kRxNameMaxX - kRxNameX);
    CHECK([shown hasSuffix:@"…"] && [shown sizeWithAttributes:chanAttr].width <= kRxNameMaxX - kRxNameX,
          "nombre largo recortado con «…» dentro de su columna: «%s»", shown.UTF8String);

    // Un transmisor con alias: la ayuda emergente también lo muestra completo.
    avm_names_set(&gNames, avm_names_src_index(AVM_SRC_MUSIC_BASE), "Música izquierda del escenario");
    int col = 1 + 8 + 1 + 8 + 1;   // Smaart (1+8), LISTENTO (1+8), cabecera Música → Música L
    NSPoint q = NSMakePoint(kLeftW + col * kCell + kCell / 2, 20);
    NSString* tip2 = [mv view:mv stringForToolTip:0 point:q userData:NULL];
    CHECK([tip2 containsString:@"Música izquierda del escenario"] && [tip2 hasPrefix:@"L · "], "ayuda emergente de un transmisor: «%s»", tip2.UTF8String);

    // Fuera de un nombre (sobre el medidor): sin ayuda emergente.
    CHECK([[mv view:mv stringForToolTip:0 point:NSMakePoint(kRxMeterX + 10, p.y) userData:NULL] length] == 0, "sin ayuda emergente sobre el medidor");

    // Las descripciones de rutas siguen funcionando con la referencia.
    CHECK([DestLongName(AVM_DST_LOCAL_REFERENCE) containsString:@"(referencia local)"], "la ruta a la Entrada 8 indica su función");
    gNames = saved;

    // El preset de medición no cambia: Salida 1 → Entrada 8, sin el retorno 8.
    avm_matrix m;
    avm_matrix_default(&m);
    CHECK(m.cell[AVM_DST_LOCAL_REFERENCE][AVM_SRC_SMAART_BASE] && !m.cell[AVM_DST_LOCAL_REFERENCE][AVM_SRC_LINK_BASE + 7],
          "preset medición intacto (Salida 1 → Entrada 8, sin retorno 8)");
}

static void test_main_window(void)
{
    AVMController* c = [[AVMController alloc] init];
    [c buildMenu];
    NSMenuItem* helpTop = nil;
    for (NSMenuItem* it in NSApp.mainMenu.itemArray) if ([it.title isEqualToString:@"Ayuda"]) helpTop = it;
    NSMenuItem* manual = [helpTop.submenu itemWithTitle:@"Manual de AV Matrix"];
    CHECK(helpTop && manual, "menú Ayuda → Manual de AV Matrix");
    CHECK(manual.action == @selector(showHelp:) && manual.target == c, "el menú abre el manual");
    CHECK(NSApp.helpMenu == helpTop.submenu, "es el menú de ayuda de macOS");

    [c buildWindow];
    NSView* content = c.window.contentView;
    NSButton* help = buttonTitled(content, @"Ayuda");
    CHECK(help && help.action == @selector(showHelp:), "botón «Ayuda» en la barra superior");
    for (NSString* t in @[ @"Preset medición", @"Directo 8×8", @"Vaciar", @"Desplegar todo", @"Plegar todo", @"Recargar", @"Reparar frecuencia" ])
        CHECK(buttonTitled(content, t) != nil, "se conserva el control «%s»", t.UTF8String);
    CHECK(c.ratePopup.numberOfItems == 3, "se conserva el selector de frecuencia");
    CHECK(c.statusLabel.superview && c.infoLabel.superview, "se conservan la línea de estado y la descripción de rutas");

    NSMutableArray* fields = [NSMutableArray array];
    collect(content, [NSTextField class], fields);
    BOOL legend = NO;
    for (NSTextField* f in fields) if ([f.stringValue containsString:@"Verde ✓"] || [f.stringValue containsString:@"Solo transporte"]) legend = YES;
    CHECK(!legend, "la leyenda ya no está en la pantalla principal");

    [c matrixView:c.matrixView hoverInfo:@"Smaart Salida 1 → Smaart Entrada 8 (referencia local): conectado"];
    CHECK([c.infoLabel.stringValue containsString:@"→"], "la descripción de rutas se sigue mostrando");
}

static void test_help_window(void)
{
    if (getenv("AVM_UI_NO_WINDOW")) { printf("(omitida la ventana del manual en pantalla: AVM_UI_NO_WINDOW)\n"); return; }
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];
    AVMController* c = [[AVMController alloc] init];
    [c showHelp:nil];
    AVMHelpController* h = c.help;
    pump(0.2);
    CHECK(h.window.visible, "la ventana del manual se abre");
    CHECK(h.window.styleMask & NSWindowStyleMaskResizable, "la ventana del manual es redimensionable");
    CHECK(h.textScroll.hasVerticalScroller, "el texto tiene desplazamiento");
    CHECK(h.index.numberOfRows == avm_manual_count, "índice con las %d secciones (%ld)", avm_manual_count, (long)h.index.numberOfRows);

    NSString* all = h.text.string;
    int titled = 0;
    for (int i = 0; i < avm_manual_count; i++)
        if ([all containsString:UTF8(avm_manual_sections[i].title)]) titled++;
    CHECK(titled == avm_manual_count, "todas las secciones están en el texto (%d de %d)", titled, avm_manual_count);
    CHECK(![all containsString:@"###"], "el marcado del manual no aparece en pantalla");
    CHECK(NSHeight(h.text.frame) > NSHeight(h.textScroll.contentView.bounds) * 2, "el manual es más largo que la ventana (hay desplazamiento)");

    // Cada entrada del índice lleva su título a la parte superior visible.
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) { [h.window setContentSize:NSMakeSize(600, 380)]; pump(0.1); }   // ventana pequeña
        int ok = 0;
        for (int i = 0; i < avm_manual_count; i++) {
            [h.index selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)i] byExtendingSelection:NO];
            pump(0.02);
            NSRect visible = h.textScroll.contentView.bounds;
            CGFloat top = [h topOfSection:i];
            BOOL atTop = top >= NSMinY(visible) - 1 && top <= NSMinY(visible) + 40;
            BOOL lastPage = NSMaxY(visible) >= NSHeight(h.text.frame) - 1 && top >= NSMinY(visible) - 1 && top < NSMaxY(visible);
            if ((atTop || lastPage) && h.index.selectedRow == i) ok++;
            else printf("  sección %d: título en %.0f, visible %.0f-%.0f\n", i + 1, top, NSMinY(visible), NSMaxY(visible));
        }
        CHECK(ok == avm_manual_count, "%s: el índice lleva a las %d secciones (%d correctas)", pass ? "ventana pequeña" : "tamaño normal", avm_manual_count, ok);
    }

    // Al desplazar el texto a mano, el índice marca la sección visible.
    int mid = avm_manual_count / 2;
    NSClipView* clip = h.textScroll.contentView;
    [clip scrollToPoint:NSMakePoint(0, [h topOfSection:mid] - 6 + 20)];
    [h.textScroll reflectScrolledClipView:clip];
    pump(0.05);
    CHECK(h.index.selectedRow == mid, "al desplazar, el índice marca la sección %d (marca %ld)", mid + 1, (long)h.index.selectedRow + 1);

    // Redimensionar: el texto se ajusta al ancho (sin desplazamiento horizontal).
    [h.window setContentSize:NSMakeSize(1100, 800)];
    pump(0.1);
    CHECK(fabs(NSWidth(h.text.frame) - h.textScroll.contentSize.width) < 1.5, "el texto sigue el ancho de la ventana");
    CHECK(h.window.minSize.width >= 500 && h.window.minSize.height >= 300, "tamaño mínimo razonable");

    // Cerrar y volver a abrir reutiliza la misma ventana.
    NSWindow* w = h.window;
    [w performClose:nil];
    pump(0.1);
    [c showHelp:nil];
    CHECK(c.help.window == w && w.visible, "cerrar y volver a abrir el manual");
    [w close];
}

int main(void)
{
    @autoreleasepool {
        avm_names_init(&gNames);
        [NSApplication sharedApplication];
        test_compact_geometry();
        test_names_and_tooltips();
        test_main_window();
        test_help_window();
    }
    printf("Pantalla principal y ayuda: %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
