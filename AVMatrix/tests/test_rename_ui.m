// Prueba nativa (solo macOS) del panel de cambio de nombre:
//   1. tamaño explícito del NSPopover y de su vista (nunca 0 × 0);
//   2. el mensaje de error cabe entero, sin recortes;
//   3. sin ciclo de retención: abrir y cerrar libera los paneles anteriores.
//
// Incluye la app tal cual (sin su main). No escribe en el archivo de nombres:
// solo usa textos rechazados, que no llegan a disco.
//
// Compilar y ejecutar (lo hace scripts/build.sh):
//   clang -fobjc-arc -o test_rename_ui tests/test_rename_ui.m Client/avm_client.c \
//         Client/avm_names.c Driver/src/avm_core.c -framework Cocoa -framework CoreAudio
// AVM_UI_NO_WINDOW=1 omite la parte que muestra el panel en pantalla.

#define AVM_APP_NO_MAIN
#include "../App/AVMatrixApp.m"

static int g_fail, g_pass;
#define N_SHOWN 20
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FALLO: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void pump(double seconds)
{
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([until timeIntervalSinceNow] > 0)
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
}

// El texto de la etiqueta cabe entero en su marco y el marco cabe en la vista.
static BOOL labelFits(NSTextField* l, NSView* root)
{
    NSRect f = [root convertRect:l.bounds fromView:l];
    NSSize need = [l.cell cellSizeForBounds:NSMakeRect(0, 0, l.bounds.size.width, CGFLOAT_MAX)];
    return f.size.width > 0 && need.height <= l.bounds.size.height + 0.5
        && NSContainsRect(NSInsetRect(root.bounds, -0.5, -0.5), f);
}

static BOOL sizeOK(NSPopover* pop, AVMRenameController* rc)
{
    NSSize c = pop.contentSize, f = rc.view.frame.size, fit = rc.view.fittingSize;
    return c.width >= 300 && c.height >= 100
        && fabs(c.width - f.width) < 0.5 && fabs(c.height - f.height) < 0.5
        && c.width + 0.5 >= fit.width && c.height + 0.5 >= fit.height;
}

static void test_size(void)
{
    const int idxs[] = { 0, avm_names_src_index(AVM_SRC_MUSIC_BASE + 1),
                         avm_names_dst_index(AVM_DST_LOCAL_REFERENCE), AVM_NAMES_COUNT - 1 };
    for (size_t i = 0; i < sizeof(idxs) / sizeof(idxs[0]); i++) {
        @autoreleasepool {
            NSPopover* pop = AVMMakeRenamePopover(idxs[i], nil);
            AVMRenameController* rc = (AVMRenameController*)pop.contentViewController;
            CHECK(sizeOK(pop, rc), "canal %d: contentSize %.0f×%.0f, vista %.0f×%.0f, fitting %.0f×%.0f", idxs[i],
                  pop.contentSize.width, pop.contentSize.height, rc.view.frame.size.width, rc.view.frame.size.height,
                  rc.view.fittingSize.width, rc.view.fittingSize.height);
            CHECK(rc.errorLabel.hidden, "canal %d: sin error al abrir", idxs[i]);
        }
    }
}

static BOOL streq_alias(int idx, const char* s)
{
    const char* a = avm_names_get(&gNames, idx);
    return a && strcmp(a, s) == 0;
}

static void test_error_fits(void)
{
    // Alias de 32 caracteres en la referencia local: el título más largo posible.
    avm_names saved = gNames;
    int ref = avm_names_dst_index(AVM_DST_LOCAL_REFERENCE);
    avm_names_set(&gNames, ref, "Señal de referencia con nombre l");
    @autoreleasepool {
        NSPopover* pop = AVMMakeRenamePopover(ref, nil);
        AVMRenameController* rc = (AVMRenameController*)pop.contentViewController;
        CGFloat h0 = pop.contentSize.height;
        CHECK(sizeOK(pop, rc), "título largo: tamaño correcto");

        // Rechazado por largo: muestra error, no cambia el nombre ni el archivo.
        rc.field.stringValue = @"Ñandú ÁÉÍÓÚ áéíóú ñ ü 0123456789 demasiado largo";
        [rc save:nil];
        CHECK(!rc.errorLabel.hidden, "error visible al superar %d caracteres", AVM_ALIAS_MAX_CHARS);
        CHECK(streq_alias(ref, "Señal de referencia con nombre l"), "el alias no cambia al rechazar");
        CHECK(pop.contentSize.height > h0, "el panel crece al mostrar el error (%.0f → %.0f)", h0, pop.contentSize.height);
        CHECK(sizeOK(pop, rc), "con error: tamaño correcto");
        CHECK(labelFits(rc.errorLabel, rc.view), "mensaje «demasiado largo» sin recortar");

        // El mensaje más largo de la app (fallo al guardar) y uno aún más largo.
        [rc showMessage:kRenameSaveFailed];
        CHECK(sizeOK(pop, rc) && labelFits(rc.errorLabel, rc.view), "mensaje de fallo al guardar sin recortar");
        [rc showMessage:[@"" stringByPaddingToLength:300 withString:@"Mensaje de error con tildes ñ " startingAtIndex:0]];
        CHECK(sizeOK(pop, rc) && labelFits(rc.errorLabel, rc.view), "mensaje de 300 caracteres: varias líneas, sin recortar");
        CHECK(fabs(pop.contentSize.width - kRenameW) < 0.5, "el ancho no cambia con el error (%.0f)", pop.contentSize.width);

        rc.field.stringValue = @"caf￾";   // carácter válido en UTF-16: no debe dar error de UTF-8
        CHECK(rc.field.stringValue.length > 0, "campo editable");
    }
    gNames = saved;
}

// Sin mostrar: crear y soltar el panel libera popover y controlador.
static void test_no_cycle_offscreen(void)
{
    __weak NSPopover* wp[50];
    __weak AVMRenameController* wr[50];
    for (int i = 0; i < 50; i++) {
        @autoreleasepool {
            NSPopover* pop = AVMMakeRenamePopover(i % AVM_NAMES_COUNT, ^{});
            wp[i] = pop;
            wr[i] = (AVMRenameController*)pop.contentViewController;
        }
    }
    int alive = 0;
    for (int i = 0; i < 50; i++) if (wp[i] || wr[i]) alive++;
    CHECK(alive == 0, "50 paneles creados y soltados: %d siguen vivos", alive);
}

// Mostrado de verdad a través del AVMController, como en la app.
static void test_no_cycle_shown(void)
{
    if (getenv("AVM_UI_NO_WINDOW")) { printf("(omitida la prueba con el panel en pantalla: AVM_UI_NO_WINDOW)\n"); return; }
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];

    NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, 600, 400)
                                              styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    w.releasedWhenClosed = NO;
    [w orderFrontRegardless];
    NSView* host = w.contentView;
    AVMController* c = [[AVMController alloc] init];

    __weak NSPopover* wp[N_SHOWN];
    __weak AVMRenameController* wr[N_SHOWN];
    int shown = 0, sized = 0;
    for (int i = 0; i < N_SHOWN; i++) {
        @autoreleasepool {
            [c matrixView:(AVMMatrixView*)host renameChannel:(i * 7) % AVM_NAMES_COUNT anchor:NSMakeRect(100, 100, 40, 20)];
            NSPopover* pop = c.renamePopover;
            AVMRenameController* rc = (AVMRenameController*)pop.contentViewController;
            wp[i] = pop;
            wr[i] = rc;
            pump(0.15);
            if (pop.shown) shown++;
            if (sizeOK(pop, rc) && rc.view.window && rc.view.window.contentView.frame.size.height + 0.5 >= rc.view.fittingSize.height) sized++;
            // Alterna las formas de cerrar: Escape/Cancelar, clic fuera (close),
            // y abrir otro canal con el panel aún abierto (i impar sin cerrar).
            if (i % 3 == 0)      [rc cancel:nil];
            else if (i % 3 == 1) [pop close];
            pop = nil; rc = nil;
        }
        if (i % 3 != 2) {
            for (int t = 0; t < 100 && c.renamePopover; t++) pump(0.02);
            CHECK(c.renamePopover == nil, "panel %d: el controlador lo suelta al cerrarse", i);
        }
    }
    [c.renamePopover close];
    for (int t = 0; t < 100 && c.renamePopover; t++) pump(0.02);
    pump(0.5);
    int alive = 0;
    for (int i = 0; i < N_SHOWN; i++) if (wp[i] || wr[i]) alive++;
    CHECK(shown == N_SHOWN, "paneles mostrados en pantalla: %d de %d", shown, N_SHOWN);
    CHECK(sized == N_SHOWN, "paneles con tamaño correcto en pantalla: %d de %d", sized, N_SHOWN);
    CHECK(alive == 0, "tras abrir y cerrar %d veces: %d paneles siguen vivos", N_SHOWN, alive);

    // El controlador de la app tampoco queda retenido por el panel.
    __weak AVMController* wc = c;
    @autoreleasepool {
        [c matrixView:(AVMMatrixView*)host renameChannel:3 anchor:NSMakeRect(100, 100, 40, 20)];
        pump(0.15);
        [c.renamePopover close];
        c = nil;
    }
    pump(0.5);
    CHECK(wc == nil, "el panel no retiene al AVMController");
    [w close];
}

int main(void)
{
    @autoreleasepool {
        avm_names_init(&gNames);
        [NSApplication sharedApplication];
        test_size();
        test_error_fits();
        test_no_cycle_offscreen();
        test_no_cycle_shown();
    }
    printf("Panel de nombres: %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
