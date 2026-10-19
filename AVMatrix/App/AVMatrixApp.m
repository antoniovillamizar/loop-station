// AV Matrix — aplicación de matriz de enrutamiento.
//
// Muestra una cuadrícula destino x fuente. Cada casilla conecta una fuente con
// un destino a ganancia unidad. No hay ganancias, ecualización, efectos ni delay.
// Los cruces que formarían un lazo (retorno de LISTENTO -> envío a LISTENTO)
// aparecen bloqueados; el driver también los rechaza.

#import <Cocoa/Cocoa.h>
#include "../Client/avm_client.h"

@interface AVMController : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (strong) NSWindow* window;
@property (strong) NSTextField* statusLabel;
@property (strong) NSPopUpButton* ratePopup;
@property (strong) NSButton* recoverButton;
@property (strong) NSMutableArray<NSMutableArray<NSButton*>*>* cells;   // [destino][fuente]
@property (strong) NSMutableArray<NSTextField*>* sumLabels;
@property (assign) BOOL listening;
@end

static const double kRates[3] = { 44100.0, 48000.0, 96000.0 };

static NSString* SourceShort(int s)
{
    if (s < AVM_SRC_LINK_BASE) return [NSString stringWithFormat:@"%d", s - AVM_SRC_SMAART_BASE + 1];
    if (s < AVM_SRC_MUSIC_BASE) return [NSString stringWithFormat:@"%d", s - AVM_SRC_LINK_BASE + 1];
    return s == AVM_SRC_MUSIC_BASE ? @"L" : @"R";
}

static NSString* DestName(int d)
{
    char buf[64];
    avm_dest_name(d, buf, sizeof(buf));
    return [NSString stringWithUTF8String:buf];
}

static NSString* SourceName(int s)
{
    char buf[64];
    avm_source_name(s, buf, sizeof(buf));
    return [NSString stringWithUTF8String:buf];
}

static NSTextField* Label(NSString* text, CGFloat size, BOOL bold)
{
    NSTextField* t = [NSTextField labelWithString:text];
    t.font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
    return t;
}

@implementation AVMController

- (void)applicationDidFinishLaunching:(NSNotification*)note
{
    (void)note;
    [self buildMenu];
    [self buildWindow];
    [self reload];
    [self startListening];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app
{
    (void)app;
    return YES;
}

- (void)buildMenu
{
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"Salir de AV Matrix" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    NSApp.mainMenu = bar;
}

- (void)buildWindow
{
    self.cells = [NSMutableArray array];
    self.sumLabels = [NSMutableArray array];

    // --- Cabecera
    NSTextField* title = Label(@"AV Matrix", 18, YES);
    self.statusLabel = Label(@"", 12, NO);
    self.statusLabel.textColor = [NSColor secondaryLabelColor];

    self.ratePopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < 3; i++)
        [self.ratePopup addItemWithTitle:[NSString stringWithFormat:@"%.1f kHz", kRates[i] / 1000.0]];
    self.ratePopup.target = self;
    self.ratePopup.action = @selector(rateChanged:);

    NSButton* presetBtn = [NSButton buttonWithTitle:@"Preset: Smaart ⇄ LISTENTO" target:self action:@selector(presetDefault:)];
    NSButton* clearBtn = [NSButton buttonWithTitle:@"Vaciar matriz" target:self action:@selector(presetClear:)];
    NSButton* reloadBtn = [NSButton buttonWithTitle:@"Recargar" target:self action:@selector(reloadPressed:)];
    self.recoverButton = [NSButton buttonWithTitle:@"Reparar frecuencia" target:self action:@selector(recoverPressed:)];
    self.recoverButton.hidden = YES;

    NSStackView* header = [NSStackView stackViewWithViews:@[ title, self.statusLabel ]];
    header.orientation = NSUserInterfaceLayoutOrientationVertical;
    header.alignment = NSLayoutAttributeLeading;
    header.spacing = 2;

    NSStackView* controls = [NSStackView stackViewWithViews:@[ Label(@"Frecuencia:", 12, NO), self.ratePopup, presetBtn, clearBtn, reloadBtn, self.recoverButton ]];
    controls.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    controls.spacing = 8;

    // --- Cuadrícula: fila 0 grupos, fila 1 números, filas 2.. destinos.
    NSInteger cols = 1 + AVM_NUM_SOURCES + 1;   // nombre + fuentes + aviso de suma
    NSMutableArray* rows = [NSMutableArray array];

    NSMutableArray* groupRow = [NSMutableArray arrayWithObject:Label(@"Destino ↓   Fuente →", 11, YES)];
    NSMutableArray* numRow = [NSMutableArray arrayWithObject:[NSGridCell emptyContentView]];
    for (int s = 0; s < AVM_NUM_SOURCES; s++) {
        NSString* g = (s == AVM_SRC_SMAART_BASE) ? @"Smaart salidas"
                    : (s == AVM_SRC_LINK_BASE) ? @"LISTENTO retornos"
                    : (s == AVM_SRC_MUSIC_BASE) ? @"Música" : nil;
        [groupRow addObject:g ? Label(g, 11, YES) : [NSGridCell emptyContentView]];
        NSTextField* n = Label(SourceShort(s), 11, NO);
        n.alignment = NSTextAlignmentCenter;
        n.toolTip = SourceName(s);
        [numRow addObject:n];
    }
    [groupRow addObject:[NSGridCell emptyContentView]];
    [numRow addObject:[NSGridCell emptyContentView]];
    [rows addObject:groupRow];
    [rows addObject:numRow];

    for (int d = 0; d < AVM_NUM_DESTS; d++) {
        NSMutableArray* row = [NSMutableArray array];
        [row addObject:Label(DestName(d), 12, NO)];
        NSMutableArray<NSButton*>* cellRow = [NSMutableArray array];
        for (int s = 0; s < AVM_NUM_SOURCES; s++) {
            NSButton* b = [NSButton checkboxWithTitle:@"" target:self action:@selector(cellToggled:)];
            b.tag = d * 100 + s;
            if (avm_route_allowed(d, s) != AVM_ROUTE_OK) {
                b.enabled = NO;
                b.toolTip = @"Bloqueado: el retorno de LISTENTO no puede volver a LISTENTO (lazo de realimentación).";
            } else {
                b.toolTip = [NSString stringWithFormat:@"%@ → %@", SourceName(s), DestName(d)];
            }
            [cellRow addObject:b];
            [row addObject:b];
        }
        [self.cells addObject:cellRow];
        NSTextField* sum = Label(@"", 11, NO);
        sum.textColor = [NSColor systemOrangeColor];
        [self.sumLabels addObject:sum];
        [row addObject:sum];
        [rows addObject:row];
    }

    NSGridView* grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 2;
    grid.columnSpacing = 4;
    for (NSInteger c = 1; c <= AVM_NUM_SOURCES; c++)
        [grid columnAtIndex:c].xPlacement = NSGridCellPlacementCenter;
    [grid mergeCellsInHorizontalRange:NSMakeRange(1 + AVM_SRC_SMAART_BASE, 8) verticalRange:NSMakeRange(0, 1)];
    [grid mergeCellsInHorizontalRange:NSMakeRange(1 + AVM_SRC_LINK_BASE, 8) verticalRange:NSMakeRange(0, 1)];
    [grid mergeCellsInHorizontalRange:NSMakeRange(1 + AVM_SRC_MUSIC_BASE, 2) verticalRange:NSMakeRange(0, 1)];
    // Separación visual entre los grupos de destinos.
    [grid rowAtIndex:2 + AVM_DST_LINK_BASE].topPadding = 8;
    (void)cols;

    NSTextField* legend = [NSTextField wrappingLabelWithString:
        @"Cada casilla conecta una fuente con un destino a ganancia unidad. "
        @"Varias casillas en una fila suman esas fuentes (se avisa en naranja). "
        @"Las casillas grises están bloqueadas para evitar lazos. "
        @"El driver solo transporta y enruta audio: no hay ecualización, efectos, análisis ni delay ajustable."];
    legend.font = [NSFont systemFontOfSize:11];
    legend.textColor = [NSColor secondaryLabelColor];
    legend.preferredMaxLayoutWidth = 760;

    NSStackView* root = [NSStackView stackViewWithViews:@[ header, controls, grid, legend ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 12;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);
    root.translatesAutoresizingMaskIntoConstraints = NO;

    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 820, 640)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"AV Matrix";
    self.window.delegate = self;
    NSView* content = self.window.contentView;
    [content addSubview:root];
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [root.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [root.topAnchor constraintEqualToAnchor:content.topAnchor],
        [root.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];
    [self.window center];
}

// --- Estado

- (void)setControlsEnabled:(BOOL)enabled
{
    for (int d = 0; d < AVM_NUM_DESTS; d++)
        for (int s = 0; s < AVM_NUM_SOURCES; s++)
            self.cells[d][s].enabled = enabled && avm_route_allowed(d, s) == AVM_ROUTE_OK;
    self.ratePopup.enabled = enabled;
}

- (void)reload
{
    AudioObjectID dev = avm_find_device(AVM_DEV_SMAART);
    if (dev == kAudioObjectUnknown) {
        self.statusLabel.stringValue = @"Driver no encontrado. Instálelo y reinicie el audio (ver README).";
        [self setControlsEnabled:NO];
        return;
    }
    avm_matrix m;
    OSStatus err = avm_get_matrix(&m);
    if (err != noErr) {
        self.statusLabel.stringValue = [NSString stringWithFormat:@"No se pudo leer la matriz (error %d).", (int)err];
        [self setControlsEnabled:NO];
        return;
    }
    [self setControlsEnabled:YES];
    for (int d = 0; d < AVM_NUM_DESTS; d++) {
        int n = 0;
        for (int s = 0; s < AVM_NUM_SOURCES; s++) {
            self.cells[d][s].state = m.cell[d][s] ? NSControlStateValueOn : NSControlStateValueOff;
            n += m.cell[d][s];
        }
        self.sumLabels[d].stringValue = n > 1 ? [NSString stringWithFormat:@"suma de %d", n] : @"";
    }

    Float64 rate = 0;
    if (avm_get_sample_rate(&rate) == noErr)
        for (int i = 0; i < 3; i++)
            if (kRates[i] == rate) [self.ratePopup selectItemAtIndex:i];

    NSMutableArray* parts = [NSMutableArray array];
    for (int k = 0; k < AVM_DEV_COUNT; k++)
        if (avm_find_device((avm_device_kind)k) == kAudioObjectUnknown)
            [parts addObject:[NSString stringWithFormat:@"falta %s", avm_device_label((avm_device_kind)k)]];
    int health = AVM_RATE_HEALTH_OK;
    avm_get_rate_health(&health);
    self.recoverButton.hidden = health != AVM_RATE_HEALTH_INCONSISTENT;
    if (health == AVM_RATE_HEALTH_INCONSISTENT)
        [parts addObject:@"FRECUENCIA INCONSISTENTE: el audio no arrancará hasta pulsar «Reparar frecuencia» (sin audio activo)"];
    else if (health == AVM_RATE_HEALTH_CHANGING)
        [parts addObject:@"cambio de frecuencia en curso"];
    self.statusLabel.textColor = health == AVM_RATE_HEALTH_INCONSISTENT ? [NSColor systemRedColor] : [NSColor secondaryLabelColor];
    self.statusLabel.stringValue = parts.count
        ? [parts componentsJoinedByString:@", "]
        : [NSString stringWithFormat:@"Dispositivos: AV Matrix Smaart · AV Matrix LISTENTO · AV Matrix Música — %.1f kHz", rate / 1000.0];
}

- (void)startListening
{
    if (self.listening) return;
    __weak AVMController* weakSelf = self;
    AudioObjectPropertyListenerBlock block = ^(UInt32 n, const AudioObjectPropertyAddress* a) {
        (void)n; (void)a;
        [weakSelf reload];
    };
    AudioObjectPropertyAddress devs = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &devs, dispatch_get_main_queue(), block);

    AudioObjectID dev = avm_find_device(AVM_DEV_SMAART);
    if (dev != kAudioObjectUnknown) {
        AudioObjectPropertyAddress mx = { AVM_PROPERTY_MATRIX, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectAddPropertyListenerBlock(dev, &mx, dispatch_get_main_queue(), block);
        AudioObjectPropertyAddress rs = { AVM_PROPERTY_RATE_STATE, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectAddPropertyListenerBlock(dev, &rs, dispatch_get_main_queue(), block);
        AudioObjectPropertyAddress sr = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectAddPropertyListenerBlock(dev, &sr, dispatch_get_main_queue(), block);
        self.listening = YES;
    }
}

- (void)showError:(NSString*)msg
{
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = msg;
    [a beginSheetModalForWindow:self.window completionHandler:nil];
}

- (void)apply:(const avm_matrix*)m
{
    OSStatus err = avm_set_matrix(m);
    if (err != noErr)
        [self showError:[NSString stringWithFormat:@"El driver rechazó el cambio (error %d).", (int)err]];
    [self reload];
}

// --- Acciones

- (void)cellToggled:(NSButton*)sender
{
    avm_matrix m;
    if (avm_get_matrix(&m) != noErr) { [self reload]; return; }
    int d = (int)(sender.tag / 100), s = (int)(sender.tag % 100);
    if (d < 0 || d >= AVM_NUM_DESTS || s < 0 || s >= AVM_NUM_SOURCES) return;
    m.cell[d][s] = sender.state == NSControlStateValueOn ? 1 : 0;
    [self apply:&m];
}

- (void)presetDefault:(id)sender
{
    (void)sender;
    avm_matrix m;
    avm_matrix_default(&m);
    [self apply:&m];
}

- (void)presetClear:(id)sender
{
    (void)sender;
    avm_matrix m;
    avm_matrix_clear(&m);
    [self apply:&m];
}

- (void)recoverPressed:(id)sender
{
    (void)sender;
    OSStatus err = avm_recover_rate(0);
    if (err == kAudioHardwareNotReadyError)
        [self showError:@"No se puede reparar con audio activo. Detenga el audio en Smaart, LISTENTO y la música, y vuelva a intentarlo."];
    else if (err != noErr)
        [self showError:[NSString stringWithFormat:@"No se pudo reparar la frecuencia (error %d).", (int)err]];
    [self reload];
}

- (void)reloadPressed:(id)sender
{
    (void)sender;
    [self startListening];
    [self reload];
}

- (void)rateChanged:(NSPopUpButton*)sender
{
    NSInteger i = sender.indexOfSelectedItem;
    if (i < 0 || i >= 3) return;
    OSStatus err = avm_set_sample_rate(kRates[i]);
    if (err == kAudioHardwareNotReadyError) {
        [self showError:@"No se puede cambiar la frecuencia con audio activo. Detenga el audio en Smaart, LISTENTO y la música, y vuelva a intentarlo."];
        [self reload];
    } else if (err != noErr) {
        [self showError:[NSString stringWithFormat:@"No se pudo cambiar la frecuencia (error %d).", (int)err]];
        [self reload];
    }
    // La recarga llega por la notificación de cambio de frecuencia.
}

@end

int main(int argc, const char* argv[])
{
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        AVMController* controller = [[AVMController alloc] init];
        app.delegate = controller;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
