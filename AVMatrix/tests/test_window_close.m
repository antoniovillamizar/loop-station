// Prueba nativa (solo macOS) del cierre de la ventana principal (revisión 16):
// cerrar la ventana con el temporizador de los medidores en marcha no debe
// liberarla (bajo ARC, releasedWhenClosed = NO): pollMeters la sigue
// consultando sin fallar; se reabre la MISMA ventana desde el Dock o el menú;
// y un puente activado sigue activado, sin que la app se cierre.
//
// build.sh la ejecuta con NSZombieEnabled=YES: un acceso a la ventana liberada
// aborta el proceso. No abre audio: el puente usa un dispositivo inexistente.

#define AVM_APP_NO_MAIN
#ifndef AVM_APP_FILE
#define AVM_APP_FILE "../App/AVMatrixApp.m"
#endif
#include AVM_APP_FILE

@interface AVMController (Pruebas)
- (void)buildMenu;
- (void)buildWindow;
- (void)startMeters;
- (void)pollMeters;
- (void)showMainWindow:(id)sender;
- (BOOL)anyBridgeEnabled;
@end

static int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FALLO: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void pump(double seconds)
{
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([until timeIntervalSinceNow] > 0)
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
}

int main(void)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];
        avm_names_init(&gNames);

        AVMController* c = [[AVMController alloc] init];
        [c buildMenu];
        [c buildWindow];
        CHECK(c.window.releasedWhenClosed == NO, "la ventana principal no se libera al cerrarse");

        // Un puente «activado» (dispositivo inexistente: no abre audio).
        c.evoRow.bridge = avm_bridge_create(AVM_BRIDGE_EVO, NULL, NULL);
        avm_bridge_config cfg;
        avm_bridge_config_default(&cfg, AVM_BRIDGE_EVO);
        cfg.enabled = true;
        snprintf(cfg.device_uid, sizeof(cfg.device_uid), "%s", "AVM_TEST_DISPOSITIVO_INEXISTENTE");
        avm_bridge_configure(c.evoRow.bridge, &cfg);
        avm_bridge_state st0 = avm_bridge_get_state(c.evoRow.bridge, NULL, 0);

        [c.window makeKeyAndOrderFront:nil];
        [c startMeters];
        pump(0.3);
        __weak NSWindow* weakWin = c.window;

        // Cerrar la ventana con el temporizador en marcha.
        [c.window performClose:nil];
        if (c.window.visible) [c.window close];   // sin botón de cierre visible en algunos entornos
        @autoreleasepool { pump(0.6); }           // ~9 llamadas a pollMeters con la ventana cerrada
        for (int i = 0; i < 20; i++) [c pollMeters];
        CHECK(weakWin != nil && c.window == weakWin, "tras cerrar, la ventana sigue viva (no hay puntero inválido)");
        CHECK(!c.window.visible, "la ventana está cerrada (oculta)");
        CHECK(c.meterTimer.valid, "el temporizador de los medidores sigue en marcha");

        // Los puentes activos se conservan y la app no se cierra.
        CHECK(avm_bridge_get_config(c.evoRow.bridge)->enabled && avm_bridge_get_state(c.evoRow.bridge, NULL, 0) == st0,
              "el puente activado sigue activado al cerrar la ventana");
        CHECK([c anyBridgeEnabled] && ![c applicationShouldTerminateAfterLastWindowClosed:NSApp],
              "con un puente activo, cerrar la ventana no cierra la app");

        // Reabrir: clic en el Dock y menú «Mostrar ventana de AV Matrix».
        [c applicationShouldHandleReopen:NSApp hasVisibleWindows:NO];
        pump(0.2);
        CHECK(c.window.visible && c.window == weakWin, "el Dock reabre la MISMA ventana");
        [c pollMeters];
        CHECK(c.matrixView.window == c.window && c.danteRow.view.window == c.window, "la matriz y los puentes siguen en la ventana reabierta");

        // Segundo ciclo: cerrar con close y reabrir desde el menú.
        [c.window close];
        @autoreleasepool { pump(0.3); }
        NSMenuItem* show = nil;
        for (NSMenuItem* it in NSApp.mainMenu.itemArray.firstObject.submenu.itemArray)
            if ([it.title isEqualToString:@"Mostrar ventana de AV Matrix"]) show = it;
        CHECK(show && show.action == @selector(showMainWindow:) && show.target == c, "menú «Mostrar ventana de AV Matrix»");
        [c showMainWindow:nil];
        pump(0.2);
        CHECK(c.window.visible && c.window == weakWin, "el menú reabre la misma ventana");

        // Sin puentes activos, cerrar la última ventana sí cierra la app (comportamiento anterior).
        cfg.enabled = false;
        avm_bridge_configure(c.evoRow.bridge, &cfg);
        CHECK([c applicationShouldTerminateAfterLastWindowClosed:NSApp], "sin puentes activos, cerrar la ventana cierra la app");

        [c.meterTimer invalidate];
        avm_bridge_destroy(c.evoRow.bridge);
        c.evoRow.bridge = NULL;
        [c.window close];
    }
    printf("Cierre de la ventana: %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
