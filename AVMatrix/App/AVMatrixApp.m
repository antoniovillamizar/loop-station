// AV Matrix — aplicación de matriz de enrutamiento.
//
// Organización visual tipo Dante Controller / Dante Preset Creator:
//   ARRIBA     transmisores (fuentes), nombres verticales, agrupados y desplegables:
//              Smaart (salidas 1-8), LISTENTO (retornos 1-8), Música (L/R).
//   IZQUIERDA  receptores (destinos), agrupados y desplegables:
//              Smaart (entradas 1-8; la 8 es REFERENCIA LOCAL), LISTENTO (envíos 1-8).
//   Cruce      casilla verde con ✓ = conectado; casilla vacía = desconectado;
//              casilla gris con × = bloqueada (lazo de realimentación).
//
// Cada casilla conecta una fuente con un destino a ganancia unidad. No hay
// ganancias, ecualización, efectos ni delay. El driver también rechaza los lazos.

#import <Cocoa/Cocoa.h>
#include "../Client/avm_client.h"

static const double kRates[3] = { 44100.0, 48000.0, 96000.0 };

// ---------------------------------------------------------------------------
#pragma mark Grupos de la matriz

typedef struct {
    const char* name;
    int first;      // primer índice de fuente o destino
    int count;
} AVMGroup;

static const AVMGroup kTxGroups[] = {
    { "Smaart",   AVM_SRC_SMAART_BASE, 8 },
    { "LISTENTO", AVM_SRC_LINK_BASE,   8 },
    { "Música",   AVM_SRC_MUSIC_BASE,  2 },
};
static const AVMGroup kRxGroups[] = {
    { "Smaart",   AVM_DST_SMAART_BASE, 8 },
    { "LISTENTO", AVM_DST_LINK_BASE,   8 },
};
#define kNumTx ((int)(sizeof(kTxGroups) / sizeof(kTxGroups[0])))
#define kNumRx ((int)(sizeof(kRxGroups) / sizeof(kRxGroups[0])))

static NSString* TxChannelName(int src)
{
    if (src < AVM_SRC_LINK_BASE)  return [NSString stringWithFormat:@"Salida %d", src - AVM_SRC_SMAART_BASE + 1];
    if (src < AVM_SRC_MUSIC_BASE) return [NSString stringWithFormat:@"Retorno %d", src - AVM_SRC_LINK_BASE + 1];
    return src == AVM_SRC_MUSIC_BASE ? @"Música L" : @"Música R";
}

static NSString* RxChannelName(int dst)
{
    if (dst < AVM_DST_LINK_BASE) return [NSString stringWithFormat:@"Entrada %d", dst - AVM_DST_SMAART_BASE + 1];
    return [NSString stringWithFormat:@"Envío %d", dst - AVM_DST_LINK_BASE + 1];
}

// Convierte un texto C en UTF-8 (nombres de grupos y dispositivos con tildes,
// p. ej. «Música») a NSString. Nunca usar %s con estos nombres: %s los
// interpreta con la codificación del sistema y muestra «M√∫sica».
static NSString* UTF8(const char* s)
{
    NSString* r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ?: @"";
}

static NSString* SourceLongName(int s) { char b[64]; avm_source_name(s, b, sizeof(b)); return UTF8(b); }
static NSString* DestLongName(int d)   { char b[64]; avm_dest_name(d, b, sizeof(b));   return UTF8(b); }

// Un carril es una fila o columna visible: la cabecera de un grupo o uno de sus canales.
typedef struct {
    int group;
    int channel;    // -1 = cabecera del grupo; si no, índice de fuente o destino
} AVMLane;

// ---------------------------------------------------------------------------
#pragma mark Vista de la matriz

@class AVMMatrixView;

@protocol AVMMatrixViewDelegate <NSObject>
- (void)matrixView:(AVMMatrixView*)view toggleDest:(int)dst source:(int)src;
- (void)matrixView:(AVMMatrixView*)view hoverInfo:(NSString*)info;
- (void)matrixViewDidChangeLayout:(AVMMatrixView*)view;
@end

@interface AVMMatrixView : NSView
@property (weak) id<AVMMatrixViewDelegate> delegate;
@property (nonatomic, assign) BOOL enabled;
- (void)setMatrix:(const avm_matrix*)m;
- (void)expandAll:(BOOL)expanded;
@end

// Geometría (puntos).
static const CGFloat kCell = 20.0;      // lado de cada casilla
static const CGFloat kLeftW = 200.0;    // ancho de los nombres de los receptores
static const CGFloat kTopH = 104.0;     // alto de los nombres verticales de los transmisores
static const CGFloat kSumW = 40.0;       // columna de aviso de suma, a la derecha

@implementation AVMMatrixView {
    avm_matrix _m;
    BOOL _txOpen[kNumTx];
    BOOL _rxOpen[kNumRx];
    AVMLane _cols[AVM_NUM_SOURCES + kNumTx];
    AVMLane _rows[AVM_NUM_DESTS + kNumRx];
    int _ncols, _nrows;
    int _hoverRow, _hoverCol;
    NSTrackingArea* _tracking;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        memset(&_m, 0, sizeof(_m));
        NSUserDefaults* ud = [NSUserDefaults standardUserDefaults];
        for (int i = 0; i < kNumTx; i++) {
            NSString* k = [NSString stringWithFormat:@"tx_open_%d", i];
            _txOpen[i] = [ud objectForKey:k] ? [ud boolForKey:k] : YES;
        }
        for (int i = 0; i < kNumRx; i++) {
            NSString* k = [NSString stringWithFormat:@"rx_open_%d", i];
            _rxOpen[i] = [ud objectForKey:k] ? [ud boolForKey:k] : YES;
        }
        _hoverRow = _hoverCol = -1;
        _enabled = YES;
        [self rebuildLanes];
    }
    return self;
}

- (BOOL)isFlipped { return YES; }

- (void)setMatrix:(const avm_matrix*)m
{
    _m = *m;
    [self setNeedsDisplay:YES];
}

- (void)setEnabled:(BOOL)enabled
{
    _enabled = enabled;
    [self setNeedsDisplay:YES];
}

- (void)rebuildLanes
{
    _ncols = 0;
    for (int g = 0; g < kNumTx; g++) {
        _cols[_ncols++] = (AVMLane){ g, -1 };
        if (_txOpen[g])
            for (int i = 0; i < kTxGroups[g].count; i++) _cols[_ncols++] = (AVMLane){ g, kTxGroups[g].first + i };
    }
    _nrows = 0;
    for (int g = 0; g < kNumRx; g++) {
        _rows[_nrows++] = (AVMLane){ g, -1 };
        if (_rxOpen[g])
            for (int i = 0; i < kRxGroups[g].count; i++) _rows[_nrows++] = (AVMLane){ g, kRxGroups[g].first + i };
    }
    NSSize size = NSMakeSize(kLeftW + _ncols * kCell + kSumW, kTopH + _nrows * kCell + 1);
    [self setFrameSize:size];
    [self setNeedsDisplay:YES];
}

- (void)saveOpenState
{
    NSUserDefaults* ud = [NSUserDefaults standardUserDefaults];
    for (int i = 0; i < kNumTx; i++) [ud setBool:_txOpen[i] forKey:[NSString stringWithFormat:@"tx_open_%d", i]];
    for (int i = 0; i < kNumRx; i++) [ud setBool:_rxOpen[i] forKey:[NSString stringWithFormat:@"rx_open_%d", i]];
}

- (void)layoutChanged
{
    [self saveOpenState];
    _hoverRow = _hoverCol = -1;
    [self rebuildLanes];
    [self.delegate matrixViewDidChangeLayout:self];
}

- (void)expandAll:(BOOL)expanded
{
    for (int i = 0; i < kNumTx; i++) _txOpen[i] = expanded;
    for (int i = 0; i < kNumRx; i++) _rxOpen[i] = expanded;
    [self layoutChanged];
}

// --- Modelo

// Conexiones en el bloque formado por una fila y una columna (cabeceras incluidas).
- (int)countForRow:(AVMLane)r col:(AVMLane)c
{
    int d0 = r.channel >= 0 ? r.channel : kRxGroups[r.group].first;
    int d1 = r.channel >= 0 ? r.channel + 1 : kRxGroups[r.group].first + kRxGroups[r.group].count;
    int s0 = c.channel >= 0 ? c.channel : kTxGroups[c.group].first;
    int s1 = c.channel >= 0 ? c.channel + 1 : kTxGroups[c.group].first + kTxGroups[c.group].count;
    int n = 0;
    for (int d = d0; d < d1; d++)
        for (int s = s0; s < s1; s++) n += _m.cell[d][s] ? 1 : 0;
    return n;
}

- (int)rowSourceCount:(int)dst
{
    int n = 0;
    for (int s = 0; s < AVM_NUM_SOURCES; s++) n += _m.cell[dst][s] ? 1 : 0;
    return n;
}

// --- Geometría

- (NSRect)cellRectRow:(int)ri col:(int)ci
{
    return NSMakeRect(kLeftW + ci * kCell, kTopH + ri * kCell, kCell, kCell);
}

- (int)colAtX:(CGFloat)x { int c = (int)floor((x - kLeftW) / kCell); return (x >= kLeftW && c < _ncols) ? c : -1; }
- (int)rowAtY:(CGFloat)y { int r = (int)floor((y - kTopH) / kCell); return (y >= kTopH && r < _nrows) ? r : -1; }

// --- Dibujo

- (void)drawVerticalString:(NSString*)s attributes:(NSDictionary*)attrs centerX:(CGFloat)cx bottomY:(CGFloat)by
{
    NSSize sz = [s sizeWithAttributes:attrs];
    NSGraphicsContext* ctx = [NSGraphicsContext currentContext];
    [ctx saveGraphicsState];
    NSAffineTransform* t = [NSAffineTransform transform];
    [t translateXBy:cx yBy:by];
    [t rotateByDegrees:-90];   // en vista invertida: el texto se lee de abajo arriba
    [t concat];
    [s drawAtPoint:NSMakePoint(0, -sz.height / 2.0) withAttributes:attrs];
    [ctx restoreGraphicsState];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSColor* bg = [NSColor controlBackgroundColor];
    NSColor* groupFill = [[NSColor labelColor] colorWithAlphaComponent:0.10];
    NSColor* gridColor = [[NSColor labelColor] colorWithAlphaComponent:0.18];
    NSColor* hoverFill = [[NSColor controlAccentColor] colorWithAlphaComponent:0.12];
    NSColor* refFill = [[NSColor systemOrangeColor] colorWithAlphaComponent:0.10];
    NSColor* green = [NSColor systemGreenColor];

    [bg setFill];
    NSRectFill(self.bounds);

    CGFloat gridW = _ncols * kCell, gridH = _nrows * kCell;
    NSDictionary* chanAttr = @{ NSFontAttributeName: [NSFont systemFontOfSize:11], NSForegroundColorAttributeName: [NSColor labelColor] };
    NSDictionary* groupAttr = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:12], NSForegroundColorAttributeName: [NSColor labelColor] };
    NSDictionary* dimAttr = @{ NSFontAttributeName: [NSFont systemFontOfSize:10], NSForegroundColorAttributeName: [NSColor secondaryLabelColor] };

    // Esquina: títulos de ejes.
    [@"Transmisores →" drawAtPoint:NSMakePoint(8, kTopH - 36) withAttributes:dimAttr];
    [@"Receptores ↓" drawAtPoint:NSMakePoint(8, kTopH - 20) withAttributes:dimAttr];

    // Fondos de cabeceras de grupo, referencia local y carriles bajo el ratón.
    for (int ri = 0; ri < _nrows; ri++) {
        NSRect band = NSMakeRect(0, kTopH + ri * kCell, kLeftW + gridW, kCell);
        if (_rows[ri].channel < 0) { [groupFill setFill]; NSRectFillUsingOperation(band, NSCompositingOperationSourceOver); }
        if (_rows[ri].channel == AVM_DST_LOCAL_REFERENCE) { [refFill setFill]; NSRectFillUsingOperation(band, NSCompositingOperationSourceOver); }
        if (ri == _hoverRow) { [hoverFill setFill]; NSRectFillUsingOperation(band, NSCompositingOperationSourceOver); }
    }
    for (int ci = 0; ci < _ncols; ci++) {
        NSRect band = NSMakeRect(kLeftW + ci * kCell, 0, kCell, kTopH + gridH);
        if (_cols[ci].channel < 0) { [groupFill setFill]; NSRectFillUsingOperation(band, NSCompositingOperationSourceOver); }
        if (ci == _hoverCol) { [hoverFill setFill]; NSRectFillUsingOperation(band, NSCompositingOperationSourceOver); }
    }

    // Nombres verticales de los transmisores.
    for (int ci = 0; ci < _ncols; ci++) {
        AVMLane c = _cols[ci];
        CGFloat cx = kLeftW + ci * kCell + kCell / 2.0;
        NSString* label;
        NSDictionary* attr;
        if (c.channel < 0) {
            label = [NSString stringWithFormat:@"%@ %@", _txOpen[c.group] ? @"⊟" : @"⊞", UTF8(kTxGroups[c.group].name)];
            attr = groupAttr;
        } else {
            label = TxChannelName(c.channel);
            attr = chanAttr;
        }
        [self drawVerticalString:label attributes:attr centerX:cx bottomY:kTopH - 4];
    }

    // Nombres de los receptores.
    for (int ri = 0; ri < _nrows; ri++) {
        AVMLane r = _rows[ri];
        CGFloat y = kTopH + ri * kCell + 2;
        if (r.channel < 0) {
            NSString* label = [NSString stringWithFormat:@"%@ %@", _rxOpen[r.group] ? @"⊟" : @"⊞", UTF8(kRxGroups[r.group].name)];
            [label drawAtPoint:NSMakePoint(6, y) withAttributes:groupAttr];
        } else {
            [RxChannelName(r.channel) drawAtPoint:NSMakePoint(24, y + 1) withAttributes:chanAttr];
            if (r.channel == AVM_DST_LOCAL_REFERENCE) {
                NSString* badge = @"REFERENCIA LOCAL";
                NSDictionary* ba = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:9], NSForegroundColorAttributeName: [NSColor whiteColor] };
                NSSize bs = [badge sizeWithAttributes:ba];
                NSRect br = NSMakeRect(86, y + 1, bs.width + 10, kCell - 6);
                [[NSColor systemOrangeColor] setFill];
                [[NSBezierPath bezierPathWithRoundedRect:br xRadius:4 yRadius:4] fill];
                [badge drawAtPoint:NSMakePoint(br.origin.x + 5, br.origin.y + (br.size.height - bs.height) / 2.0) withAttributes:ba];
            }
        }
    }

    // Casillas.
    for (int ri = 0; ri < _nrows; ri++) {
        for (int ci = 0; ci < _ncols; ci++) {
            AVMLane r = _rows[ri], c = _cols[ci];
            NSRect cell = [self cellRectRow:ri col:ci];
            NSRect box = NSInsetRect(cell, 3, 3);
            if (r.channel >= 0 && c.channel >= 0) {
                if (avm_route_allowed(r.channel, c.channel) != AVM_ROUTE_OK) {
                    [[[NSColor labelColor] colorWithAlphaComponent:0.12] setFill];
                    NSRectFillUsingOperation(box, NSCompositingOperationSourceOver);
                    NSDictionary* xa = @{ NSFontAttributeName: [NSFont systemFontOfSize:9], NSForegroundColorAttributeName: [NSColor tertiaryLabelColor] };
                    NSSize xs = [@"×" sizeWithAttributes:xa];
                    [@"×" drawAtPoint:NSMakePoint(NSMidX(box) - xs.width / 2, NSMidY(box) - xs.height / 2) withAttributes:xa];
                } else if (_m.cell[r.channel][c.channel]) {
                    [(self.enabled ? green : [green colorWithAlphaComponent:0.4]) setFill];
                    [[NSBezierPath bezierPathWithRoundedRect:box xRadius:2 yRadius:2] fill];
                    NSDictionary* ca = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:11], NSForegroundColorAttributeName: [NSColor whiteColor] };
                    NSSize cs = [@"✓" sizeWithAttributes:ca];
                    [@"✓" drawAtPoint:NSMakePoint(NSMidX(box) - cs.width / 2, NSMidY(box) - cs.height / 2) withAttributes:ca];
                } else {
                    [[[NSColor labelColor] colorWithAlphaComponent:self.enabled ? 0.35 : 0.15] setStroke];
                    NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(box, 0.5, 0.5) xRadius:2 yRadius:2];
                    p.lineWidth = 1;
                    [p stroke];
                }
            } else if ([self countForRow:r col:c] > 0) {
                // Resumen de un grupo plegado o de una cabecera: hay conexiones dentro.
                [green setFill];
                NSRect dot = NSMakeRect(NSMidX(cell) - 3.5, NSMidY(cell) - 3.5, 7, 7);
                [[NSBezierPath bezierPathWithOvalInRect:dot] fill];
            }
        }
    }

    // Rejilla.
    [gridColor setStroke];
    NSBezierPath* g = [NSBezierPath bezierPath];
    g.lineWidth = 1;
    for (int ri = 0; ri <= _nrows; ri++) {
        CGFloat y = kTopH + ri * kCell + 0.5;
        BOOL major = ri == _nrows || _rows[ri].channel < 0;
        [g moveToPoint:NSMakePoint(major ? 0 : kLeftW, y)];
        [g lineToPoint:NSMakePoint(kLeftW + gridW, y)];
    }
    for (int ci = 0; ci <= _ncols; ci++) {
        CGFloat x = kLeftW + ci * kCell + 0.5;
        BOOL major = ci == _ncols || _cols[ci].channel < 0;
        [g moveToPoint:NSMakePoint(x, major ? 0 : kTopH)];
        [g lineToPoint:NSMakePoint(x, kTopH + gridH)];
    }
    [g stroke];

    // Aviso de suma a la derecha de cada fila con más de una fuente.
    NSDictionary* sumAttr = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:11], NSForegroundColorAttributeName: [NSColor systemOrangeColor] };
    for (int ri = 0; ri < _nrows; ri++) {
        if (_rows[ri].channel < 0) continue;
        int n = [self rowSourceCount:_rows[ri].channel];
        if (n > 1)
            [[NSString stringWithFormat:@"Σ %d", n] drawAtPoint:NSMakePoint(kLeftW + gridW + 6, kTopH + ri * kCell + 2) withAttributes:sumAttr];
    }
}

// --- Ratón

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                               owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
}

- (NSString*)describeRow:(int)ri col:(int)ci
{
    if (ri < 0 && ci < 0) return @"";
    if (ri >= 0 && ci >= 0) {
        AVMLane r = _rows[ri], c = _cols[ci];
        if (r.channel >= 0 && c.channel >= 0) {
            NSString* path = [NSString stringWithFormat:@"%@ → %@", SourceLongName(c.channel), DestLongName(r.channel)];
            if (avm_route_allowed(r.channel, c.channel) != AVM_ROUTE_OK)
                return [path stringByAppendingString:@": BLOQUEADO (el retorno de LISTENTO no puede volver a LISTENTO)"];
            return [path stringByAppendingString:_m.cell[r.channel][c.channel] ? @": conectado (clic para desconectar)" : @": desconectado (clic para conectar)"];
        }
        int n = [self countForRow:r col:c];
        return [NSString stringWithFormat:@"%d conexión(es) en este bloque. Clic para desplegar los grupos.", n];
    }
    if (ri >= 0) {
        AVMLane r = _rows[ri];
        if (r.channel < 0) return [NSString stringWithFormat:@"Receptor %@: clic para %@", UTF8(kRxGroups[r.group].name), _rxOpen[r.group] ? @"plegar" : @"desplegar"];
        return DestLongName(r.channel);
    }
    AVMLane c = _cols[ci];
    if (c.channel < 0) return [NSString stringWithFormat:@"Transmisor %@: clic para %@", UTF8(kTxGroups[c.group].name), _txOpen[c.group] ? @"plegar" : @"desplegar"];
    return SourceLongName(c.channel);
}

- (void)mouseMoved:(NSEvent*)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    int ri = [self rowAtY:p.y], ci = [self colAtX:p.x];
    if (p.x > kLeftW + _ncols * kCell) ci = -1;
    if (ri != _hoverRow || ci != _hoverCol) {
        _hoverRow = ri;
        _hoverCol = ci;
        [self setNeedsDisplay:YES];
        [self.delegate matrixView:self hoverInfo:[self describeRow:ri col:ci]];
    }
}

- (void)mouseExited:(NSEvent*)e
{
    (void)e;
    _hoverRow = _hoverCol = -1;
    [self setNeedsDisplay:YES];
    [self.delegate matrixView:self hoverInfo:@""];
}

- (void)mouseDown:(NSEvent*)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    int ri = [self rowAtY:p.y], ci = [self colAtX:p.x];

    // Clic en el nombre de un grupo receptor (izquierda) o transmisor (arriba): plegar/desplegar.
    if (p.x < kLeftW && ri >= 0) {
        if (_rows[ri].channel < 0) { _rxOpen[_rows[ri].group] = !_rxOpen[_rows[ri].group]; [self layoutChanged]; }
        return;
    }
    if (p.y < kTopH && ci >= 0) {
        if (_cols[ci].channel < 0) { _txOpen[_cols[ci].group] = !_txOpen[_cols[ci].group]; [self layoutChanged]; }
        return;
    }
    if (ri < 0 || ci < 0) return;

    AVMLane r = _rows[ri], c = _cols[ci];
    if (r.channel < 0 || c.channel < 0) {
        // Casilla de resumen: despliega los grupos implicados (nunca cambia conexiones).
        if (r.channel < 0) _rxOpen[r.group] = YES;
        if (c.channel < 0) _txOpen[c.group] = YES;
        [self layoutChanged];
        return;
    }
    if (!self.enabled) { NSBeep(); return; }
    if (avm_route_allowed(r.channel, c.channel) != AVM_ROUTE_OK) {
        NSBeep();
        [self.delegate matrixView:self hoverInfo:[self describeRow:ri col:ci]];
        return;
    }
    [self.delegate matrixView:self toggleDest:r.channel source:c.channel];
}

@end

// ---------------------------------------------------------------------------
#pragma mark Controlador

@interface AVMController : NSObject <NSApplicationDelegate, NSWindowDelegate, AVMMatrixViewDelegate>
@property (strong) NSWindow* window;
@property (strong) NSTextField* statusLabel;
@property (strong) NSTextField* infoLabel;
@property (strong) NSPopUpButton* ratePopup;
@property (strong) NSButton* recoverButton;
@property (strong) AVMMatrixView* matrixView;
@property (strong) NSScrollView* scrollView;
@property (strong) NSLayoutConstraint* scrollW;
@property (strong) NSLayoutConstraint* scrollH;
@property (strong) NSStackView* root;
@property (assign) BOOL listening;
@end

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
    [self fitWindow];
    [self.window center];
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
    // --- Barra superior compacta
    self.ratePopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < 3; i++)
        [self.ratePopup addItemWithTitle:[NSString stringWithFormat:@"%.1f kHz", kRates[i] / 1000.0]];
    self.ratePopup.target = self;
    self.ratePopup.action = @selector(rateChanged:);

    NSButton* measureBtn = [NSButton buttonWithTitle:@"Preset medición" target:self action:@selector(presetMeasure:)];
    measureBtn.toolTip = @"Smaart Salidas → LISTENTO Envíos 1-8; LISTENTO Retornos 1-7 → Smaart Entradas 1-7; Smaart Salida 1 → Entrada 8 (referencia local, sin el retorno 8).";
    NSButton* directBtn = [NSButton buttonWithTitle:@"Directo 8×8" target:self action:@selector(presetDirect:)];
    directBtn.toolTip = @"Smaart Salida N → LISTENTO Envío N y LISTENTO Retorno N → Smaart Entrada N, de 1 a 8 (sin referencia local).";
    NSButton* clearBtn = [NSButton buttonWithTitle:@"Vaciar" target:self action:@selector(presetClear:)];
    NSButton* openBtn = [NSButton buttonWithTitle:@"Desplegar todo" target:self action:@selector(expandAll:)];
    NSButton* foldBtn = [NSButton buttonWithTitle:@"Plegar todo" target:self action:@selector(collapseAll:)];
    NSButton* reloadBtn = [NSButton buttonWithTitle:@"Recargar" target:self action:@selector(reloadPressed:)];
    self.recoverButton = [NSButton buttonWithTitle:@"Reparar frecuencia" target:self action:@selector(recoverPressed:)];
    self.recoverButton.hidden = YES;
    for (NSButton* b in @[ measureBtn, directBtn, clearBtn, openBtn, foldBtn, reloadBtn, self.recoverButton ])
        b.controlSize = NSControlSizeSmall, b.font = [NSFont systemFontOfSize:11];
    self.ratePopup.controlSize = NSControlSizeSmall;
    self.ratePopup.font = [NSFont systemFontOfSize:11];

    NSStackView* toolbar = [NSStackView stackViewWithViews:@[ Label(@"AV Matrix", 14, YES), Label(@"Frecuencia:", 11, NO), self.ratePopup,
                                                              measureBtn, directBtn, clearBtn, openBtn, foldBtn, reloadBtn, self.recoverButton ]];
    toolbar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    toolbar.spacing = 6;

    self.statusLabel = Label(@"", 11, NO);
    self.statusLabel.textColor = [NSColor secondaryLabelColor];

    // --- Matriz
    self.matrixView = [[AVMMatrixView alloc] initWithFrame:NSZeroRect];
    self.matrixView.delegate = self;
    self.scrollView = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    self.scrollView.documentView = self.matrixView;
    self.scrollView.hasVerticalScroller = YES;
    self.scrollView.hasHorizontalScroller = YES;
    self.scrollView.autohidesScrollers = YES;
    self.scrollView.borderType = NSBezelBorder;
    self.scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    self.scrollW = [self.scrollView.widthAnchor constraintEqualToConstant:100];
    self.scrollH = [self.scrollView.heightAnchor constraintEqualToConstant:100];
    self.scrollW.priority = NSLayoutPriorityDefaultHigh;
    self.scrollH.priority = NSLayoutPriorityDefaultHigh;
    self.scrollW.active = YES;
    self.scrollH.active = YES;

    self.infoLabel = Label(@" ", 11, NO);
    self.infoLabel.textColor = [NSColor secondaryLabelColor];
    self.infoLabel.lineBreakMode = NSLineBreakByTruncatingTail;

    NSTextField* legend = Label(@"Verde ✓ = conectado · vacía = desconectada · gris × = bloqueada (lazo) · ⊟/⊞ plegar o desplegar grupo · Σ = suma de fuentes. "
                                @"Solo transporte y enrutamiento: sin ecualización, efectos, análisis ni delay.", 10, NO);
    legend.textColor = [NSColor tertiaryLabelColor];

    self.root = [NSStackView stackViewWithViews:@[ toolbar, self.statusLabel, self.scrollView, self.infoLabel, legend ]];
    self.root.orientation = NSUserInterfaceLayoutOrientationVertical;
    self.root.alignment = NSLayoutAttributeLeading;
    self.root.spacing = 6;
    self.root.edgeInsets = NSEdgeInsetsMake(10, 10, 10, 10);
    self.root.translatesAutoresizingMaskIntoConstraints = NO;

    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 900, 600)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"AV Matrix";
    self.window.delegate = self;
    NSView* content = self.window.contentView;
    [content addSubview:self.root];
    [NSLayoutConstraint activateConstraints:@[
        [self.root.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [self.root.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [self.root.topAnchor constraintEqualToAnchor:content.topAnchor],
        [self.root.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];
}

// Ajusta la ventana al tamaño de la matriz (sin espacio vacío), limitado a la pantalla.
- (void)fitWindow
{
    NSSize m = self.matrixView.frame.size;
    NSRect screen = (self.window.screen ?: [NSScreen mainScreen]).visibleFrame;
    CGFloat maxW = screen.size.width - 40, maxH = screen.size.height - 160;
    self.scrollW.constant = MIN(m.width + 4, maxW);
    self.scrollH.constant = MIN(m.height + 4, maxH);
    [self.root layoutSubtreeIfNeeded];
    NSSize fit = self.root.fittingSize;
    NSRect frame = self.window.frame;
    NSRect contentRect = [self.window contentRectForFrameRect:frame];
    CGFloat top = NSMaxY(frame);
    contentRect.size = fit;
    NSRect newFrame = [self.window frameRectForContentRect:contentRect];
    newFrame.origin.y = top - newFrame.size.height;   // conservar el borde superior
    [self.window setFrame:newFrame display:YES animate:NO];
}

// --- Estado

- (void)reload
{
    AudioObjectID dev = avm_find_device(AVM_DEV_SMAART);
    if (dev == kAudioObjectUnknown) {
        self.statusLabel.stringValue = @"Driver no encontrado. Instálelo y reinicie el audio (ver INSTALAR.md).";
        self.statusLabel.textColor = [NSColor systemRedColor];
        self.matrixView.enabled = NO;
        self.ratePopup.enabled = NO;
        return;
    }
    avm_matrix m;
    OSStatus err = avm_get_matrix(&m);
    if (err != noErr) {
        self.statusLabel.stringValue = [NSString stringWithFormat:@"No se pudo leer la matriz (error %d).", (int)err];
        self.statusLabel.textColor = [NSColor systemRedColor];
        self.matrixView.enabled = NO;
        return;
    }
    self.matrixView.enabled = YES;
    self.ratePopup.enabled = YES;
    [self.matrixView setMatrix:&m];

    Float64 rate = 0;
    if (avm_get_sample_rate(&rate) == noErr)
        for (int i = 0; i < 3; i++)
            if (kRates[i] == rate) [self.ratePopup selectItemAtIndex:i];

    NSMutableArray* parts = [NSMutableArray array];
    for (int k = 0; k < AVM_DEV_COUNT; k++)
        if (avm_find_device((avm_device_kind)k) == kAudioObjectUnknown)
            [parts addObject:[NSString stringWithFormat:@"falta %@", UTF8(avm_device_label((avm_device_kind)k))]];
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
        : [NSString stringWithFormat:@"AV Matrix Smaart · AV Matrix LISTENTO · AV Matrix Música — %.1f kHz", rate / 1000.0];
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

// --- Delegado de la matriz

- (void)matrixView:(AVMMatrixView*)view toggleDest:(int)dst source:(int)src
{
    (void)view;
    avm_matrix m;
    if (avm_get_matrix(&m) != noErr) { [self reload]; return; }
    if (dst < 0 || dst >= AVM_NUM_DESTS || src < 0 || src >= AVM_NUM_SOURCES) return;
    m.cell[dst][src] = m.cell[dst][src] ? 0 : 1;
    [self apply:&m];
}

- (void)matrixView:(AVMMatrixView*)view hoverInfo:(NSString*)info
{
    (void)view;
    self.infoLabel.stringValue = info.length ? info : @" ";
}

- (void)matrixViewDidChangeLayout:(AVMMatrixView*)view
{
    (void)view;
    [self fitWindow];
}

// --- Acciones

- (void)presetMeasure:(id)sender
{
    (void)sender;
    avm_matrix m;
    avm_matrix_default(&m);
    [self apply:&m];
}

- (void)presetDirect:(id)sender
{
    (void)sender;
    avm_matrix m;
    avm_matrix_direct(&m);
    [self apply:&m];
}

- (void)presetClear:(id)sender
{
    (void)sender;
    avm_matrix m;
    avm_matrix_clear(&m);
    [self apply:&m];
}

- (void)expandAll:(id)sender   { (void)sender; [self.matrixView expandAll:YES]; }
- (void)collapseAll:(id)sender { (void)sender; [self.matrixView expandAll:NO]; }

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
    // Si se acepta, la recarga llega por la notificación de cambio de frecuencia.
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
