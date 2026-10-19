// AV Matrix — aplicación de matriz de enrutamiento.
//
// Organización visual tipo Dante Controller / Dante Preset Creator:
//   ARRIBA     transmisores (fuentes), nombres verticales, agrupados y desplegables:
//              Smaart (salidas 1-8), LISTENTO (retornos 1-8), Música (L/R).
//   IZQUIERDA  receptores (destinos), agrupados y desplegables:
//              Smaart (entradas 1-8; la 8 recibe la referencia local en el
//              preset de medición), LISTENTO (envíos 1-8).
//   Cruce      casilla verde con ✓ = conectado; casilla vacía = desconectado;
//              casilla gris con × = bloqueada (lazo de realimentación).
//   Medidores  junto a cada nombre: pico en dBFS, barra y LED rojo de saturación.
//              Fuentes: nivel antes de la matriz. Destinos: señal real entregada,
//              después del enrutamiento y de la suma. «—» = sin audio en ese canal.
//              La app solo lee la propiedad 'metr' del driver: no abre audio.
//   Ayuda      el manual completo (App/avm_manual.c) en una ventana aparte:
//              botón «Ayuda» y menú Ayuda → Manual de AV Matrix.
//
// Cada casilla conecta una fuente con un destino a ganancia unidad. No hay
// ganancias, ecualización, efectos ni delay. El driver también rechaza los lazos.

#import <Cocoa/Cocoa.h>
#include "../Client/avm_client.h"
#include "../Client/avm_names.h"
#include "avm_manual.h"
#include "avm_fader.h"

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

// Convierte un texto C en UTF-8 (nombres de grupos y dispositivos con tildes,
// p. ej. «Música») a NSString. Nunca usar %s con estos nombres: %s los
// interpreta con la codificación del sistema y muestra «M√∫sica».
static NSString* UTF8(const char* s)
{
    NSString* r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ?: @"";
}

// --- Nombres personalizados (solo visualización) ---------------------------
// Los alias viven en la app (Application Support) y nunca llegan al driver:
// no cambian conexiones, numeración interna, niveles ni funcionamiento.
static avm_names gNames;

static NSString* NamesFilePath(void)
{
    NSURL* base = [[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL* dir = [base URLByAppendingPathComponent:@"AV Matrix" isDirectory:YES];
    [[NSFileManager defaultManager] createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:NULL];
    return [dir URLByAppendingPathComponent:@"nombres_canales.txt"].path;
}

static NSString* ChannelAlias(int idx)   { const char* a = avm_names_get(&gNames, idx); return a ? UTF8(a) : nil; }
static NSString* ChannelNumber(int idx)  { char b[8]; avm_names_number(idx, b, sizeof(b)); return UTF8(b); }
static NSString* ChannelDefault(int idx) { char b[64]; avm_names_default(idx, b, sizeof(b)); return UTF8(b); }

// Cabecera vertical de un transmisor: «Salida 3», o «3 · alias» (el número siempre visible).
static NSString* TxChannelName(int src)
{
    int idx = avm_names_src_index(src);
    NSString* a = ChannelAlias(idx);
    return a ? [NSString stringWithFormat:@"%@ · %@", ChannelNumber(idx), a] : ChannelDefault(idx);
}

// Nombre de un receptor junto a su número (el número se dibuja aparte, en su columna).
static NSString* RxChannelName(int dst)
{
    int idx = avm_names_dst_index(dst);
    return ChannelAlias(idx) ?: ChannelDefault(idx);
}

// Descripciones de rutas: nombre completo original y, si existe, el alias entre comillas.
static NSString* SourceLongName(int s)
{
    char b[64];
    avm_source_name(s, b, sizeof(b));
    NSString* a = ChannelAlias(avm_names_src_index(s));
    return a ? [NSString stringWithFormat:@"%@ «%@»", UTF8(b), a] : UTF8(b);
}

static NSString* DestLongName(int d)
{
    NSString* a = ChannelAlias(avm_names_dst_index(d));
    if (d == AVM_DST_LOCAL_REFERENCE)   // la descripción de la ruta sigue indicando su función
        return a ? [NSString stringWithFormat:@"Smaart Entrada 8 «%@» (referencia local)", a] : @"Smaart Entrada 8 (referencia local)";
    char b[64];
    avm_dest_name(d, b, sizeof(b));
    return a ? [NSString stringWithFormat:@"%@ «%@»", UTF8(b), a] : UTF8(b);
}

// Recorta un texto con «…» para que quepa en `maxW` puntos.
static NSString* FitString(NSString* s, NSDictionary* attrs, CGFloat maxW)
{
    if ([s sizeWithAttributes:attrs].width <= maxW) return s;
    NSString* t = s;
    while (t.length > 1) {
        NSRange r = [t rangeOfComposedCharacterSequenceAtIndex:t.length - 1];
        t = [t substringToIndex:r.location];
        NSString* c = [t stringByAppendingString:@"…"];
        if ([c sizeWithAttributes:attrs].width <= maxW) return c;
    }
    return @"…";
}

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
// Renombrar: idx es el índice de avm_names (fuentes 0-17, destinos 18-33).
- (void)matrixView:(AVMMatrixView*)view renameChannel:(int)idx anchor:(NSRect)anchor;
- (void)matrixView:(AVMMatrixView*)view restoreChannelName:(int)idx;
@end

@interface AVMMatrixView : NSView
@property (weak) id<AVMMatrixViewDelegate> delegate;
@property (nonatomic, assign) BOOL enabled;
- (void)setMatrix:(const avm_matrix*)m;
- (void)setMeterSources:(const avm_meter_reading*)src dests:(const avm_meter_reading*)dst;
- (void)expandAll:(BOOL)expanded;
@end

// Geometría (puntos).
static const CGFloat kCell = 20.0;      // lado de cada casilla
static const CGFloat kLeftW = 268.0;    // nombres de los receptores + sus medidores (compacto)
static const CGFloat kTopH = 184.0;     // nombres verticales de los transmisores + sus medidores
static const CGFloat kSumW = 40.0;      // columna de aviso de suma, a la derecha

// Fila de un receptor, de izquierda a derecha: número (6-20), nombre (24 hasta
// kRxNameMaxX), barra, dBFS y LED de saturación. Los nombres largos se recortan
// con «…» y se leen completos en la ayuda emergente.
static const CGFloat kRxNameX = 24.0;
static const CGFloat kRxMeterX = 150.0, kRxMeterW = 56.0;
static const CGFloat kRxNameMaxX = 142.0;   // kRxMeterX - 8
static const CGFloat kRxDbX = 210.0, kRxDbW = 40.0;
static const CGFloat kRxClipX = 254.0;
// Medidores de los transmisores (debajo del nombre vertical, en cada columna).
static const CGFloat kTxMeterH = 80.0;  // LED + barra + dBFS
static const CGFloat kTxBarH = 50.0;

// Escala de los medidores: de -60 a 0 dBFS.
static const float kMeterFloorDb = -60.0f;

@implementation AVMMatrixView {
    avm_matrix _m;
    avm_meter_reading _srcM[AVM_NUM_SOURCES];
    avm_meter_reading _dstM[AVM_NUM_DESTS];
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
        memset(_srcM, 0, sizeof(_srcM));
        memset(_dstM, 0, sizeof(_dstM));
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

- (void)setMeterSources:(const avm_meter_reading*)src dests:(const avm_meter_reading*)dst
{
    if (src) memcpy(_srcM, src, sizeof(_srcM)); else memset(_srcM, 0, sizeof(_srcM));
    if (dst) memcpy(_dstM, dst, sizeof(_dstM)); else memset(_dstM, 0, sizeof(_dstM));
    [self setNeedsDisplay:YES];
}

// Texto de nivel: «—» sin audio, «−∞» silencio digital, si no el pico en dBFS.
static NSString* LevelText(avm_meter_reading r, BOOL decimals)
{
    if (!r.active) return @"—";
    float db = avm_peak_to_dbfs(r.peak);
    if (isinf(db)) return @"−∞";
    if (decimals) return [NSString stringWithFormat:@"%+.1f", db];
    return [NSString stringWithFormat:@"%.0f", db];
}

// Barra de nivel en `r` (horizontal o vertical, crece hacia la derecha o hacia
// arriba). La comparten la matriz y el mixer.
static void AVMDrawMeterBar(NSRect r, avm_meter_reading m, BOOL vertical)
{
    [[[NSColor labelColor] colorWithAlphaComponent:0.10] setFill];
    NSRectFillUsingOperation(r, NSCompositingOperationSourceOver);
    if (!m.active) {
        [[[NSColor labelColor] colorWithAlphaComponent:0.20] setStroke];
        NSBezierPath* p = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 0.5, 0.5)];
        CGFloat dash[2] = { 2, 2 };
        [p setLineDash:dash count:2 phase:0];
        [p stroke];
        return;
    }
    float db = avm_peak_to_dbfs(m.peak);
    float frac = isinf(db) ? 0.0f : (db - kMeterFloorDb) / -kMeterFloorDb;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    // Tramos: verde hasta -18, amarillo hasta -6, naranja hasta 0 dBFS.
    const float edges[4] = { 0.0f, (-18.0f - kMeterFloorDb) / -kMeterFloorDb, (-6.0f - kMeterFloorDb) / -kMeterFloorDb, 1.0f };
    NSColor* colors[3] = { [NSColor systemGreenColor], [NSColor systemYellowColor], [NSColor systemOrangeColor] };
    CGFloat len = vertical ? r.size.height : r.size.width;
    for (int i = 0; i < 3; i++) {
        float a = edges[i], b = edges[i + 1] < frac ? edges[i + 1] : frac;
        if (b <= a) break;
        NSRect seg = vertical
            ? NSMakeRect(r.origin.x, NSMaxY(r) - b * len, r.size.width, (b - a) * len)
            : NSMakeRect(r.origin.x + a * len, r.origin.y, (b - a) * len, r.size.height);
        [colors[i] setFill];
        NSRectFill(seg);
    }
}

static void AVMDrawClipLED(NSRect r, BOOL on, BOOL active)
{
    NSBezierPath* p = [NSBezierPath bezierPathWithOvalInRect:r];
    if (on) {
        [[NSColor systemRedColor] setFill];
        [p fill];
    } else {
        [[[NSColor labelColor] colorWithAlphaComponent:active ? 0.30 : 0.15] setStroke];
        p.lineWidth = 1;
        [p stroke];
    }
}

- (void)drawMeterBar:(NSRect)r reading:(avm_meter_reading)m vertical:(BOOL)vertical { AVMDrawMeterBar(r, m, vertical); }
- (void)drawClipLED:(NSRect)r on:(BOOL)on active:(BOOL)active { AVMDrawClipLED(r, on, active); }

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
    [self rebuildToolTips];
    [self setNeedsDisplay:YES];
}

// Ayuda emergente sobre el nombre de cada canal: el nombre completo aunque esté
// recortado en pantalla. El texto se calcula al mostrarse (sigue a los cambios de nombre).
- (void)rebuildToolTips
{
    [self removeAllToolTips];
    for (int ri = 0; ri < _nrows; ri++)
        if (_rows[ri].channel >= 0)
            [self addToolTipRect:[self rxNameRectForRow:ri] owner:self userData:NULL];
    for (int ci = 0; ci < _ncols; ci++)
        if (_cols[ci].channel >= 0)
            [self addToolTipRect:NSMakeRect(kLeftW + ci * kCell, 0, kCell, kTopH - kTxMeterH) owner:self userData:NULL];
}

- (NSRect)rxNameRectForRow:(int)ri
{
    return NSMakeRect(0, kTopH + ri * kCell, kRxNameMaxX, kCell);
}

// Texto completo de un canal: número, nombre mostrado y descripción larga.
- (NSString*)fullNameForIndex:(int)idx
{
    NSString* shown = ChannelAlias(idx) ?: ChannelDefault(idx);
    NSString* full = idx < AVM_NUM_SOURCES ? SourceLongName(idx) : DestLongName(idx - AVM_NUM_SOURCES);
    return [NSString stringWithFormat:@"%@ · %@\n%@\nDoble clic para cambiar el nombre.", ChannelNumber(idx), shown, full];
}

- (NSString*)view:(NSView*)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)point userData:(void*)data
{
    (void)view; (void)tag; (void)data;
    int idx = [self channelNameIndexAt:point];
    return idx >= 0 ? [self fullNameForIndex:idx] : @"";
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
    NSColor* green = [NSColor systemGreenColor];

    [bg setFill];
    NSRectFill(self.bounds);

    CGFloat gridW = _ncols * kCell, gridH = _nrows * kCell;
    NSDictionary* chanAttr = @{ NSFontAttributeName: [NSFont systemFontOfSize:11], NSForegroundColorAttributeName: [NSColor labelColor] };
    NSDictionary* groupAttr = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:12], NSForegroundColorAttributeName: [NSColor labelColor] };
    NSDictionary* dimAttr = @{ NSFontAttributeName: [NSFont systemFontOfSize:10], NSForegroundColorAttributeName: [NSColor secondaryLabelColor] };

    // Esquina: títulos de ejes y de las columnas de medidores.
    [@"Transmisores →" drawAtPoint:NSMakePoint(8, kTopH - kTxMeterH - 20) withAttributes:dimAttr];
    [@"nivel antes de la matriz →" drawAtPoint:NSMakePoint(kLeftW - 140, kTopH - kTxMeterH / 2 - 6) withAttributes:dimAttr];
    [@"Receptores ↓" drawAtPoint:NSMakePoint(8, kTopH - 18) withAttributes:dimAttr];
    [@"nivel entregado" drawAtPoint:NSMakePoint(kRxMeterX, kTopH - 30) withAttributes:dimAttr];
    // «dBFS» alineado a la derecha de su columna; «SAT» centrado sobre el LED, una
    // línea más arriba para no pegarse a «dBFS» en la zona compacta.
    [@"dBFS" drawAtPoint:NSMakePoint(kRxDbX + kRxDbW - [@"dBFS" sizeWithAttributes:dimAttr].width, kTopH - 18) withAttributes:dimAttr];
    [@"SAT" drawAtPoint:NSMakePoint(kRxClipX + 4 - [@"SAT" sizeWithAttributes:dimAttr].width / 2, kTopH - 30) withAttributes:dimAttr];

    // Fondos de cabeceras de grupo y carriles bajo el ratón.
    for (int ri = 0; ri < _nrows; ri++) {
        NSRect band = NSMakeRect(0, kTopH + ri * kCell, kLeftW + gridW, kCell);
        if (_rows[ri].channel < 0) { [groupFill setFill]; NSRectFillUsingOperation(band, NSCompositingOperationSourceOver); }
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
            label = FitString(TxChannelName(c.channel), chanAttr, kTopH - kTxMeterH - 10);
            attr = chanAttr;
        }
        [self drawVerticalString:label attributes:attr centerX:cx bottomY:kTopH - kTxMeterH - 4];

        // Medidor de la fuente (antes de la matriz): LED, barra vertical y dBFS.
        if (c.channel >= 0) {
            avm_meter_reading mr = _srcM[c.channel];
            CGFloat x0 = kLeftW + ci * kCell;
            CGFloat top = kTopH - kTxMeterH;
            [self drawClipLED:NSMakeRect(cx - 4, top + 2, 8, 8) on:mr.clip active:mr.active];
            [self drawMeterBar:NSMakeRect(cx - 4, top + 13, 8, kTxBarH) reading:mr vertical:YES];
            NSDictionary* da = @{ NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:8.5 weight:NSFontWeightRegular],
                                  NSForegroundColorAttributeName: mr.active ? [NSColor labelColor] : [NSColor tertiaryLabelColor] };
            NSString* t = LevelText(mr, NO);
            NSSize ts = [t sizeWithAttributes:da];
            [t drawAtPoint:NSMakePoint(x0 + (kCell - ts.width) / 2.0, top + 13 + kTxBarH + 2) withAttributes:da];
        }
    }

    // Nombres de los receptores.
    for (int ri = 0; ri < _nrows; ri++) {
        AVMLane r = _rows[ri];
        CGFloat y = kTopH + ri * kCell + 2;
        if (r.channel < 0) {
            NSString* label = [NSString stringWithFormat:@"%@ %@", _rxOpen[r.group] ? @"⊟" : @"⊞", UTF8(kRxGroups[r.group].name)];
            [label drawAtPoint:NSMakePoint(6, y) withAttributes:groupAttr];
        } else {
            // Número del canal, siempre visible, y su nombre (original o personalizado).
            NSDictionary* na = @{ NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:10 weight:NSFontWeightRegular],
                                  NSForegroundColorAttributeName: [NSColor secondaryLabelColor] };
            NSString* num = ChannelNumber(avm_names_dst_index(r.channel));
            [num drawAtPoint:NSMakePoint(20 - [num sizeWithAttributes:na].width, y + 2) withAttributes:na];
            [FitString(RxChannelName(r.channel), chanAttr, kRxNameMaxX - kRxNameX) drawAtPoint:NSMakePoint(kRxNameX, y + 1) withAttributes:chanAttr];

            // Medidor del destino (señal entregada, después de la matriz).
            avm_meter_reading mr = _dstM[r.channel];
            CGFloat rowY = kTopH + ri * kCell;
            [self drawMeterBar:NSMakeRect(kRxMeterX, rowY + 6, kRxMeterW, 8) reading:mr vertical:NO];
            NSDictionary* da = @{ NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:10 weight:NSFontWeightRegular],
                                  NSForegroundColorAttributeName: mr.active ? [NSColor labelColor] : [NSColor tertiaryLabelColor] };
            NSString* t = LevelText(mr, YES);
            NSSize ts = [t sizeWithAttributes:da];
            [t drawAtPoint:NSMakePoint(kRxDbX + kRxDbW - ts.width, rowY + (kCell - ts.height) / 2.0) withAttributes:da];
            [self drawClipLED:NSMakeRect(kRxClipX, rowY + 6, 8, 8) on:mr.clip active:mr.active];
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
        return [self levelDescription:_dstM[r.channel] name:DestLongName(r.channel) where:@"entregado, después de la matriz"];
    }
    AVMLane c = _cols[ci];
    if (c.channel < 0) return [NSString stringWithFormat:@"Transmisor %@: clic para %@", UTF8(kTxGroups[c.group].name), _txOpen[c.group] ? @"plegar" : @"desplegar"];
    return [self levelDescription:_srcM[c.channel] name:SourceLongName(c.channel) where:@"antes de la matriz"];
}

- (NSString*)levelDescription:(avm_meter_reading)m name:(NSString*)name where:(NSString*)where
{
    if (!m.active) return [NSString stringWithFormat:@"%@ — sin audio en este canal", name];
    float db = avm_peak_to_dbfs(m.peak);
    NSString* lvl = isinf(db) ? @"silencio (−∞ dBFS)" : [NSString stringWithFormat:@"pico %+.1f dBFS", db];
    return [NSString stringWithFormat:@"%@ — %@ (%@)%@", name, lvl, where, m.clip ? @" · SATURACIÓN en los últimos 2 s" : @""];
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

// Índice de avm_names si `p` está sobre el nombre de un canal (no de un grupo); si no, -1.
- (int)channelNameIndexAt:(NSPoint)p
{
    int ri = [self rowAtY:p.y], ci = [self colAtX:p.x];
    if (ri >= 0 && p.x < kRxNameMaxX && _rows[ri].channel >= 0) return avm_names_dst_index(_rows[ri].channel);
    if (ci >= 0 && p.y < kTopH - kTxMeterH && _cols[ci].channel >= 0) return avm_names_src_index(_cols[ci].channel);
    return -1;
}

- (NSRect)nameRectForIndex:(int)idx at:(NSPoint)p
{
    if (idx >= AVM_NUM_SOURCES) {
        int ri = [self rowAtY:p.y];
        return NSMakeRect(kRxNameX, kTopH + ri * kCell, kRxNameMaxX - kRxNameX, kCell);
    }
    int ci = [self colAtX:p.x];
    return NSMakeRect(kLeftW + ci * kCell, 0, kCell, kTopH - kTxMeterH);
}

// Menú contextual sobre el nombre de un canal.
- (NSMenu*)menuForEvent:(NSEvent*)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    int idx = [self channelNameIndexAt:p];
    if (idx < 0) return nil;
    NSMenu* m = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem* rename = [m addItemWithTitle:@"Cambiar nombre…" action:@selector(menuRename:) keyEquivalent:@""];
    rename.target = self;
    rename.tag = idx;
    rename.representedObject = [NSValue valueWithRect:[self nameRectForIndex:idx at:p]];
    NSMenuItem* restore = [m addItemWithTitle:@"Restaurar nombre original" action:@selector(menuRestore:) keyEquivalent:@""];
    restore.target = self;
    restore.tag = idx;
    restore.enabled = avm_names_get(&gNames, idx) != NULL;
    m.autoenablesItems = NO;
    return m;
}

- (void)menuRename:(NSMenuItem*)item  { [self.delegate matrixView:self renameChannel:(int)item.tag anchor:[item.representedObject rectValue]]; }
- (void)menuRestore:(NSMenuItem*)item { [self.delegate matrixView:self restoreChannelName:(int)item.tag]; }

- (void)mouseDown:(NSEvent*)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    int ri = [self rowAtY:p.y], ci = [self colAtX:p.x];

    // Doble clic sobre el nombre de un canal: cambiar su nombre.
    int renameIdx = [self channelNameIndexAt:p];
    if (e.clickCount == 2 && renameIdx >= 0) {
        [self.delegate matrixView:self renameChannel:renameIdx anchor:[self nameRectForIndex:renameIdx at:p]];
        return;
    }

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
#pragma mark Cambio de nombre de un canal

// Panel de edición: Enter guarda, Escape (o clic fuera) cancela, «Restaurar
// nombre original» quita el alias. Solo cambia el texto mostrado por la app.
//
// Propiedad: el AVMController retiene el NSPopover mientras está abierto; el
// popover retiene a este controlador (contentViewController) y este solo guarda
// una referencia DÉBIL al popover. Así no hay ciclo: al cerrarse y soltarlo el
// AVMController, se liberan los dos.
static const CGFloat kRenameW = 420.0;   // ancho fijo del panel (pt)
static NSString* const kRenameSaveFailed = @"No se pudo guardar el nombre en disco; no se ha cambiado.";

@interface AVMRenameController : NSViewController <NSTextFieldDelegate>
@property (assign) int index;
@property (weak) NSPopover* popover;
@property (strong) NSTextField* field;
@property (strong) NSTextField* errorLabel;
@property (copy) void (^onChange)(void);
@end

@implementation AVMRenameController

- (instancetype)initWithIndex:(int)idx
{
    if ((self = [super initWithNibName:nil bundle:nil])) _index = idx;
    return self;
}

- (void)loadView
{
    NSString* key = UTF8(avm_names_key(self.index));
    NSString* original = ChannelDefault(self.index);
    NSString* full = self.index < AVM_NUM_SOURCES ? SourceLongName(self.index) : DestLongName(self.index - AVM_NUM_SOURCES);

    // Todo el contenido tiene el ancho interior fijo (kRenameW menos márgenes):
    // los textos largos pasan a varias líneas o se recortan con «…», pero nunca
    // ensanchan el panel.
    const CGFloat inner = kRenameW - 24;

    // Título: hasta 2 líneas (alias de 32 caracteres + «(referencia local)»).
    NSTextField* title = [NSTextField wrappingLabelWithString:[NSString stringWithFormat:@"Nombre del canal %@ — %@", ChannelNumber(self.index), full]];
    title.font = [NSFont boldSystemFontOfSize:12];
    title.maximumNumberOfLines = 2;
    title.cell.truncatesLastVisibleLine = YES;
    title.preferredMaxLayoutWidth = inner;
    // Subtítulo: una línea, recortada en el centro si no cabe.
    NSTextField* sub = [NSTextField labelWithString:[NSString stringWithFormat:@"Original: «%@» · identificador: %@", original, key]];
    sub.font = [NSFont systemFontOfSize:10];
    sub.textColor = [NSColor secondaryLabelColor];
    sub.lineBreakMode = NSLineBreakByTruncatingMiddle;

    self.field = [NSTextField textFieldWithString:ChannelAlias(self.index) ?: original];
    self.field.placeholderString = original;
    self.field.delegate = self;

    // Mensaje de error en varias líneas si hace falta: nunca se recorta.
    self.errorLabel = [NSTextField wrappingLabelWithString:@""];
    self.errorLabel.font = [NSFont systemFontOfSize:10];
    self.errorLabel.textColor = [NSColor systemRedColor];
    self.errorLabel.preferredMaxLayoutWidth = inner;
    self.errorLabel.hidden = YES;

    NSTextField* help = [NSTextField wrappingLabelWithString:[NSString stringWithFormat:@"Enter guarda · Esc cancela · vacío = nombre original · máximo %d caracteres", AVM_ALIAS_MAX_CHARS]];
    help.font = [NSFont systemFontOfSize:10];
    help.textColor = [NSColor tertiaryLabelColor];
    help.preferredMaxLayoutWidth = inner;

    NSButton* restore = [NSButton buttonWithTitle:@"Restaurar nombre original" target:self action:@selector(restore:)];
    restore.enabled = avm_names_get(&gNames, self.index) != NULL;
    NSButton* cancel = [NSButton buttonWithTitle:@"Cancelar" target:self action:@selector(cancel:)];
    NSButton* save = [NSButton buttonWithTitle:@"Guardar" target:self action:@selector(save:)];
    save.keyEquivalent = @"\r";
    NSView* spacer = [NSView new];
    [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* buttons = [NSStackView stackViewWithViews:@[ restore, spacer, cancel, save ]];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSArray<NSView*>* rows = @[ title, sub, self.field, self.errorLabel, help, buttons ];
    NSStackView* v = [NSStackView stackViewWithViews:rows];
    v.orientation = NSUserInterfaceLayoutOrientationVertical;
    v.alignment = NSLayoutAttributeLeading;
    v.spacing = 6;
    v.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
    // Ancho obligatorio para cada fila; el texto cede (compresión baja), no el panel.
    for (NSView* row in rows) {
        [row.widthAnchor constraintEqualToConstant:inner].active = YES;
        [row setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    [v.widthAnchor constraintEqualToConstant:kRenameW].active = YES;
    self.view = v;
    [self updateSize];
}

// Tamaño explícito: NSPopover no lo deduce de la vista (quedaría en 0 × 0).
// Se recalcula al mostrar u ocultar el mensaje de error.
- (void)updateSize
{
    NSView* v = self.view;
    [v layoutSubtreeIfNeeded];
    NSSize s = v.fittingSize;
    s.width = kRenameW;          // el ancho es fijo; solo cambia el alto
    s.height = ceil(s.height);
    [v setFrameSize:s];
    self.preferredContentSize = s;
    self.popover.contentSize = s;
    [v layoutSubtreeIfNeeded];
}

- (void)showMessage:(NSString*)msg
{
    self.errorLabel.stringValue = msg;
    self.errorLabel.hidden = NO;
    [self updateSize];
}

- (void)viewDidAppear
{
    [super viewDidAppear];
    [self.view.window makeFirstResponder:self.field];
    [self.field selectText:nil];
}

// Enter y Escape dentro del campo.
- (BOOL)control:(NSControl*)c textView:(NSTextView*)tv doCommandBySelector:(SEL)sel
{
    (void)c; (void)tv;
    if (sel == @selector(insertNewline:)) { [self save:nil]; return YES; }
    if (sel == @selector(cancelOperation:)) { [self cancel:nil]; return YES; }
    return NO;
}

- (void)cancelOperation:(id)sender { [self cancel:sender]; }

- (void)cancel:(id)sender
{
    (void)sender;
    [self.popover performClose:nil];   // sin cambios
}

// Aplica un cambio y lo guarda; si no se puede guardar, se revierte.
- (BOOL)commit:(void (^)(void))change
{
    avm_names previous = gNames;
    change();
    if (!avm_names_save(&gNames, NamesFilePath().fileSystemRepresentation)) {
        gNames = previous;
        [self showMessage:kRenameSaveFailed];
        return NO;
    }
    if (self.onChange) self.onChange();
    [self.popover performClose:nil];
    return YES;
}

- (void)save:(id)sender
{
    (void)sender;
    // Forma compuesta (NFC): «é» como un solo carácter, igual en pantalla y en disco.
    NSString* text = [self.field.stringValue precomposedStringWithCanonicalMapping];
    const char* utf8 = text.UTF8String ?: "";
    char clean[AVM_ALIAS_MAX_BYTES + 1];
    avm_name_status st = avm_names_normalize(utf8, clean, sizeof(clean));
    if (st == AVM_NAME_TOO_LONG) {
        [self showMessage:[NSString stringWithFormat:@"El nombre tiene más de %d caracteres.", AVM_ALIAS_MAX_CHARS]];
        return;
    }
    if (st == AVM_NAME_INVALID_UTF8) {
        [self showMessage:@"El texto contiene caracteres no válidos."];
        return;
    }
    int idx = self.index;
    const char* cleanp = clean;   // el bloque se ejecuta de forma síncrona dentro de commit:
    [self commit:^{ avm_names_set(&gNames, idx, cleanp); }];
}

- (void)restore:(id)sender
{
    (void)sender;
    int idx = self.index;
    [self commit:^{ avm_names_restore(&gNames, idx); }];
}

@end

// Crea el panel de edición ya dimensionado (sin mostrarlo).
static NSPopover* AVMMakeRenamePopover(int idx, void (^onChange)(void))
{
    AVMRenameController* rc = [[AVMRenameController alloc] initWithIndex:idx];
    rc.onChange = onChange;
    (void)rc.view;                                // carga y calcula el tamaño
    NSPopover* pop = [[NSPopover alloc] init];
    pop.behavior = NSPopoverBehaviorTransient;   // clic fuera = cancelar
    pop.contentViewController = rc;
    rc.popover = pop;                            // débil
    pop.contentSize = rc.preferredContentSize;
    return pop;
}

// ---------------------------------------------------------------------------
#pragma mark Ayuda (manual de uso)

// Ventana independiente con el manual completo: índice de secciones a la
// izquierda y texto con desplazamiento a la derecha. Redimensionable. El texto
// está compilado en la app (avm_manual.c): no necesita internet ni archivos.
@interface AVMHelpController : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate>
@property (strong) NSWindow* window;
@property (strong) NSTableView* index;
@property (strong) NSTextView* text;
@property (strong) NSScrollView* textScroll;
- (void)show;
- (void)showSection:(int)i;
- (NSRange)rangeOfSection:(int)i;
- (int)sectionAtTop;
- (CGFloat)topOfSection:(int)i;
@end

@implementation AVMHelpController {
    NSRange _ranges[64];     // rango del título de cada sección en el texto
    BOOL _scrollingFromIndex;
}

// Convierte el marcado mínimo del manual (párrafos, «### », «- ») en texto con estilo.
static NSAttributedString* ManualText(NSRange* ranges, int maxRanges)
{
    NSMutableAttributedString* out = [[NSMutableAttributedString alloc] init];
    NSFont* body = [NSFont systemFontOfSize:13];
    NSMutableParagraphStyle* para = [[NSMutableParagraphStyle alloc] init];
    para.paragraphSpacing = 6;
    para.lineSpacing = 1.5;
    NSMutableParagraphStyle* bullet = [para mutableCopy];
    bullet.headIndent = 18;
    bullet.firstLineHeadIndent = 4;
    bullet.paragraphSpacing = 3;
    bullet.tabStops = @[ [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:18 options:@{}] ];
    NSMutableParagraphStyle* h1 = [para mutableCopy];
    h1.paragraphSpacingBefore = 18;
    h1.paragraphSpacing = 8;
    NSMutableParagraphStyle* h2 = [para mutableCopy];
    h2.paragraphSpacingBefore = 8;
    h2.paragraphSpacing = 3;
    NSDictionary* bodyA = @{ NSFontAttributeName: body, NSForegroundColorAttributeName: [NSColor labelColor], NSParagraphStyleAttributeName: para };
    NSDictionary* bulletA = @{ NSFontAttributeName: body, NSForegroundColorAttributeName: [NSColor labelColor], NSParagraphStyleAttributeName: bullet };
    NSDictionary* h1A = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:19], NSForegroundColorAttributeName: [NSColor labelColor], NSParagraphStyleAttributeName: h1 };
    NSDictionary* h2A = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:14], NSForegroundColorAttributeName: [NSColor labelColor], NSParagraphStyleAttributeName: h2 };

    for (int i = 0; i < avm_manual_count; i++) {
        NSString* title = [NSString stringWithFormat:@"%d. %@\n", i + 1, UTF8(avm_manual_sections[i].title)];
        if (i < maxRanges) ranges[i] = NSMakeRange(out.length, title.length - 1);
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:title attributes:h1A]];
        for (NSString* line in [UTF8(avm_manual_sections[i].body) componentsSeparatedByString:@"\n"]) {
            if (line.length == 0) continue;   // los párrafos ya llevan su separación
            if ([line hasPrefix:@"### "])
                [out appendAttributedString:[[NSAttributedString alloc] initWithString:[[line substringFromIndex:4] stringByAppendingString:@"\n"] attributes:h2A]];
            else if ([line hasPrefix:@"- "])
                [out appendAttributedString:[[NSAttributedString alloc] initWithString:[NSString stringWithFormat:@"•\t%@\n", [line substringFromIndex:2]] attributes:bulletA]];
            else
                [out appendAttributedString:[[NSAttributedString alloc] initWithString:[line stringByAppendingString:@"\n"] attributes:bodyA]];
        }
    }
    return out;
}

- (void)build
{
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 860, 620)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Manual de AV Matrix";
    self.window.releasedWhenClosed = NO;
    self.window.minSize = NSMakeSize(560, 360);
    self.window.delegate = self;
    self.window.frameAutosaveName = @"AVMatrixManual";

    // Índice.
    self.index = [[NSTableView alloc] initWithFrame:NSZeroRect];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"seccion"];
    col.title = @"Índice";
    col.resizingMask = NSTableColumnAutoresizingMask;
    [self.index addTableColumn:col];
    self.index.headerView = nil;
    self.index.dataSource = self;
    self.index.delegate = self;
    self.index.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    if (@available(macOS 11.0, *)) self.index.style = NSTableViewStyleSourceList;
    NSScrollView* indexScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 230, 620)];
    indexScroll.documentView = self.index;
    indexScroll.hasVerticalScroller = YES;
    indexScroll.autohidesScrollers = YES;
    indexScroll.autoresizingMask = NSViewHeightSizable;

    // Texto del manual.
    self.textScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 630, 620)];
    self.textScroll.hasVerticalScroller = YES;
    self.textScroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    NSSize cs = self.textScroll.contentSize;
    self.text = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, cs.width, cs.height)];
    self.text.editable = NO;
    self.text.selectable = YES;
    self.text.richText = YES;
    self.text.drawsBackground = YES;
    self.text.backgroundColor = [NSColor textBackgroundColor];
    self.text.textContainerInset = NSMakeSize(22, 14);
    self.text.minSize = NSMakeSize(0, cs.height);
    self.text.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    self.text.verticallyResizable = YES;
    self.text.horizontallyResizable = NO;
    self.text.autoresizingMask = NSViewWidthSizable;
    self.text.textContainer.widthTracksTextView = YES;
    self.text.textContainer.containerSize = NSMakeSize(cs.width, CGFLOAT_MAX);
    [self.text.textStorage setAttributedString:ManualText(_ranges, 64)];
    self.textScroll.documentView = self.text;

    NSSplitView* split = [[NSSplitView alloc] initWithFrame:self.window.contentView.bounds];
    split.vertical = YES;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    split.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [split addSubview:indexScroll];
    [split addSubview:self.textScroll];
    [split setHoldingPriority:NSLayoutPriorityDefaultHigh forSubviewAtIndex:0];
    [self.window.contentView addSubview:split];
    [split adjustSubviews];
    [split setPosition:230 ofDividerAtIndex:0];

    // Al desplazar el texto, el índice marca la sección visible.
    self.textScroll.contentView.postsBoundsChangedNotifications = YES;
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(textScrolled:)
                                                 name:NSViewBoundsDidChangeNotification object:self.textScroll.contentView];
    [self.window center];
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)show
{
    if (!self.window) {
        [self build];
        [self showSection:0];
    }
    [self.window makeKeyAndOrderFront:nil];
}

- (NSRange)rangeOfSection:(int)i
{
    return (i >= 0 && i < avm_manual_count && i < 64) ? _ranges[i] : NSMakeRange(NSNotFound, 0);
}

// Posición vertical (en el texto) del título de una sección.
- (CGFloat)topOfSection:(int)i
{
    NSLayoutManager* lm = self.text.layoutManager;
    [lm ensureLayoutForTextContainer:self.text.textContainer];
    NSRange g = [lm glyphRangeForCharacterRange:[self rangeOfSection:i] actualCharacterRange:NULL];
    NSRect r = [lm boundingRectForGlyphRange:g inTextContainer:self.text.textContainer];
    return r.origin.y + self.text.textContainerOrigin.y;
}

// Lleva el título de la sección al principio de la zona visible.
- (void)showSection:(int)i
{
    if (i < 0 || i >= avm_manual_count) return;
    if (!self.window) [self build];
    NSClipView* clip = self.textScroll.contentView;
    CGFloat y = [self topOfSection:i] - 6;
    CGFloat maxY = MAX(0, NSHeight(self.text.frame) - NSHeight(clip.bounds));
    y = MAX(0, MIN(y, maxY));
    _scrollingFromIndex = YES;
    [clip scrollToPoint:NSMakePoint(0, y)];
    [self.textScroll reflectScrolledClipView:clip];
    _scrollingFromIndex = NO;
    if (self.index.selectedRow != i)
        [self.index selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)i] byExtendingSelection:NO];
}

// Última sección cuyo título ya está en (o por encima de) la parte superior visible.
- (int)sectionAtTop
{
    CGFloat top = NSMinY(self.textScroll.contentView.bounds) + 12;
    int best = 0;
    for (int i = 0; i < avm_manual_count; i++)
        if ([self topOfSection:i] <= top) best = i;
    return best;
}

- (void)textScrolled:(NSNotification*)n
{
    (void)n;
    if (_scrollingFromIndex) return;
    int i = [self sectionAtTop];
    if (self.index.selectedRow != i) {
        _scrollingFromIndex = YES;   // seleccionar no debe volver a desplazar el texto
        [self.index selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)i] byExtendingSelection:NO];
        [self.index scrollRowToVisible:i];
        _scrollingFromIndex = NO;
    }
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv { (void)tv; return avm_manual_count; }

- (id)tableView:(NSTableView*)tv objectValueForTableColumn:(NSTableColumn*)c row:(NSInteger)row
{
    (void)tv; (void)c;
    return [NSString stringWithFormat:@"%ld. %@", (long)row + 1, UTF8(avm_manual_sections[row].title)];
}

- (NSView*)tableView:(NSTableView*)tv viewForTableColumn:(NSTableColumn*)c row:(NSInteger)row
{
    NSTextField* f = [tv makeViewWithIdentifier:@"fila" owner:self];
    if (!f) {
        // Títulos largos en dos líneas (la altura de la fila se calcula abajo).
        f = [NSTextField wrappingLabelWithString:@""];
        f.identifier = @"fila";
        f.font = [NSFont systemFontOfSize:12];
        f.maximumNumberOfLines = 2;
        f.cell.truncatesLastVisibleLine = YES;
    }
    f.stringValue = [self tableView:tv objectValueForTableColumn:c row:row];
    f.toolTip = f.stringValue;
    return f;
}

- (CGFloat)tableView:(NSTableView*)tv heightOfRow:(NSInteger)row
{
    CGFloat w = MAX(80, (tv.tableColumns.firstObject.width ?: 200) - 12);
    NSString* t = [self tableView:tv objectValueForTableColumn:nil row:row];
    NSRect b = [t boundingRectWithSize:NSMakeSize(w, CGFLOAT_MAX) options:NSStringDrawingUsesLineFragmentOrigin
                            attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:12] }];
    return MIN(ceil(b.size.height), 32) + 8;   // una o dos líneas
}

- (void)tableViewColumnDidResize:(NSNotification*)n
{
    (void)n;
    [self.index noteHeightOfRowsWithIndexesChanged:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, (NSUInteger)avm_manual_count)]];
}

- (void)tableViewSelectionDidChange:(NSNotification*)n
{
    (void)n;
    if (_scrollingFromIndex) return;
    NSInteger row = self.index.selectedRow;
    if (row >= 0) [self showSection:(int)row];
}

@end

// ---------------------------------------------------------------------------
#pragma mark Mixer

// Ventana «Mixer»: volumen (solo atenuación, de 0 dB a silencio) y MUTE de cada
// canal. ENTRADAS A LA MATRIZ = las 18 fuentes (el volumen actúa antes de la
// matriz); SALIDAS DE LA MATRIZ = los 16 destinos (después del enrutamiento y la
// suma). Los ajustes viven en el driver (propiedad 'mixr'), que los guarda y
// aplica sin chasquidos (rampas de AVM_GAIN_RAMP_FRAMES muestras). Los medidores
// muestran el nivel resultante de esos controles.

static const CGFloat kStripW = 60.0, kStripH = 284.0;
static const CGFloat kStripMeterH = 150.0;

// Medidor vertical con el LED de saturación arriba.
@interface AVMMeterView : NSView
@property (nonatomic, assign) avm_meter_reading reading;
@end

@implementation AVMMeterView
- (BOOL)isFlipped { return YES; }
- (void)setReading:(avm_meter_reading)r
{
    if (r.active == _reading.active && r.clip == _reading.clip && r.peak == _reading.peak) return;
    _reading = r;
    [self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSRect b = self.bounds;
    AVMDrawClipLED(NSMakeRect(NSMidX(b) - 4, 0, 8, 8), _reading.clip, _reading.active);
    AVMDrawMeterBar(NSMakeRect(0, 12, b.size.width, b.size.height - 12), _reading, YES);
}
@end

// Fader vertical. Doble clic u Opción+clic: vuelve a 0 dB.
@interface AVMFader : NSSlider
@end

@implementation AVMFader
- (void)mouseDown:(NSEvent*)e
{
    if (e.clickCount == 2 || (e.modifierFlags & NSEventModifierFlagOption)) {
        self.doubleValue = 1.0;
        [self sendAction:self.action to:self.target];
        return;
    }
    [super mouseDown:e];
}
@end

// Tira de un canal: número, nombre, medidor, fader con lectura en dB y MUTE.
@interface AVMMixerStrip : NSView
@property (assign) int index;                 // índice de avm_names (fuentes 0-17, destinos 18-33)
@property (strong) NSTextField* number;
@property (strong) NSTextField* name;
@property (strong) AVMMeterView* meter;
@property (strong) AVMFader* fader;
@property (strong) NSTextField* gainLabel;
@property (strong) NSTextField* peakLabel;
@property (strong) NSButton* mute;
- (void)showGain:(float)gain mute:(BOOL)muted;
- (void)refreshName;
@end

@implementation AVMMixerStrip

- (BOOL)isFlipped { return YES; }

- (instancetype)initWithIndex:(int)idx target:(id)target
{
    if ((self = [super initWithFrame:NSMakeRect(0, 0, kStripW, kStripH)])) {
        _index = idx;
        self.translatesAutoresizingMaskIntoConstraints = NO;
        [self.widthAnchor constraintEqualToConstant:kStripW].active = YES;
        [self.heightAnchor constraintEqualToConstant:kStripH].active = YES;

        _number = [NSTextField labelWithString:ChannelNumber(idx)];
        _number.font = [NSFont boldSystemFontOfSize:12];
        _number.alignment = NSTextAlignmentCenter;
        _number.frame = NSMakeRect(0, 2, kStripW, 16);

        _name = [NSTextField wrappingLabelWithString:@""];
        _name.font = [NSFont systemFontOfSize:10];
        _name.alignment = NSTextAlignmentCenter;
        _name.maximumNumberOfLines = 2;
        _name.cell.truncatesLastVisibleLine = YES;
        _name.frame = NSMakeRect(1, 18, kStripW - 2, 28);

        _meter = [[AVMMeterView alloc] initWithFrame:NSMakeRect(9, 50, 10, kStripMeterH + 12)];

        _fader = [[AVMFader alloc] initWithFrame:NSMakeRect(24, 56, 28, kStripMeterH + 2)];
        _fader.vertical = YES;
        _fader.minValue = 0.0;
        _fader.maxValue = 1.0;
        _fader.doubleValue = 1.0;
        _fader.continuous = YES;
        _fader.target = target;
        _fader.action = @selector(faderMoved:);
        _fader.tag = idx;
        _fader.toolTip = @"Volumen (solo atenúa: de 0 dB a silencio). Doble clic u Opción+clic: 0 dB.";

        _gainLabel = [NSTextField labelWithString:@"0.0 dB"];
        _gainLabel.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightMedium];
        _gainLabel.alignment = NSTextAlignmentCenter;
        _gainLabel.frame = NSMakeRect(0, 216, kStripW, 16);

        _peakLabel = [NSTextField labelWithString:@"—"];
        _peakLabel.font = [NSFont monospacedDigitSystemFontOfSize:9 weight:NSFontWeightRegular];
        _peakLabel.textColor = [NSColor secondaryLabelColor];
        _peakLabel.alignment = NSTextAlignmentCenter;
        _peakLabel.frame = NSMakeRect(0, 232, kStripW, 13);
        _peakLabel.toolTip = @"Nivel de pico (dBFS) después del volumen y el MUTE.";

        _mute = [NSButton buttonWithTitle:@"MUTE" target:target action:@selector(muteToggled:)];
        [_mute setButtonType:NSButtonTypePushOnPushOff];
        _mute.bezelStyle = NSBezelStyleRegularSquare;
        _mute.tag = idx;
        _mute.font = [NSFont boldSystemFontOfSize:11];
        _mute.frame = NSMakeRect(5, 250, kStripW - 10, 26);
        _mute.toolTip = @"MUTE: silencia este canal sin cambiar su volumen.";

        for (NSView* v in @[ _number, _name, _meter, _fader, _gainLabel, _peakLabel, _mute ]) [self addSubview:v];
        [self refreshName];
    }
    return self;
}

- (void)refreshName
{
    NSString* shown = ChannelAlias(self.index) ?: ChannelDefault(self.index);
    NSString* full = self.index < AVM_NUM_SOURCES ? SourceLongName(self.index) : DestLongName(self.index - AVM_NUM_SOURCES);
    self.name.stringValue = shown;
    self.name.toolTip = [NSString stringWithFormat:@"%@ · %@\n%@", ChannelNumber(self.index), shown, full];
    self.number.toolTip = self.name.toolTip;
}

- (void)showGain:(float)gain mute:(BOOL)muted
{
    double pos = avm_fader_pos_from_gain(gain);
    if (fabs(self.fader.doubleValue - pos) > 1e-9 && avm_fader_gain_from_pos(self.fader.doubleValue) != gain)
        self.fader.doubleValue = pos;
    char b[32];
    avm_fader_format(gain, b, sizeof(b));
    self.gainLabel.stringValue = UTF8(b);
    self.gainLabel.textColor = gain < 1.0f ? [NSColor systemOrangeColor] : [NSColor labelColor];
    self.mute.state = muted ? NSControlStateValueOn : NSControlStateValueOff;
    self.mute.bezelColor = muted ? [NSColor systemRedColor] : nil;
    self.mute.attributedTitle = [[NSAttributedString alloc] initWithString:@"MUTE" attributes:@{
        NSFontAttributeName: [NSFont boldSystemFontOfSize:11],
        NSForegroundColorAttributeName: muted ? [NSColor whiteColor] : [NSColor labelColor] }];
    self.name.textColor = muted ? [NSColor tertiaryLabelColor] : [NSColor labelColor];
}

- (void)showReading:(avm_meter_reading)r
{
    self.meter.reading = r;
    self.peakLabel.stringValue = LevelText(r, YES);
}

@end

@interface AVMMixerController : NSObject <NSWindowDelegate>
@property (strong) NSWindow* window;
@property (strong) NSTextField* statusLabel;
@property (strong) NSButton* muteAllButton;
@property (strong) NSButton* restoreButton;
@property (assign) BOOL offline;          // pruebas: nunca habla con el driver
@property (assign) BOOL connected;        // hay driver y se pudo leer el mixer
@property (assign) NSUInteger sendCount;  // envíos al driver (diagnóstico y pruebas)
@property (copy) void (^onChange)(void);  // la ventana principal actualiza su estado
- (void)show;
- (void)reloadFromDriver;
- (void)setMeterSources:(const avm_meter_reading*)src dests:(const avm_meter_reading*)dst;
- (void)namesChanged;
- (void)muteAll:(id)sender;
- (void)restoreUnityConfirmed;
- (avm_mixer)mixer;
- (AVMMixerStrip*)stripForIndex:(int)idx;
- (void)faderMoved:(NSSlider*)sender;
- (void)muteToggled:(NSButton*)sender;
- (void)flush;
@end

@implementation AVMMixerController {
    avm_mixer _mx;
    AVMMixerStrip* _strips[AVM_NAMES_COUNT];
    NSTimer* _sendTimer;
}

- (instancetype)init
{
    if ((self = [super init])) avm_mixer_default(&_mx);
    return self;
}

- (avm_mixer)mixer { return _mx; }
- (AVMMixerStrip*)stripForIndex:(int)idx { return (idx >= 0 && idx < AVM_NAMES_COUNT) ? _strips[idx] : nil; }

static NSTextField* SectionTitle(NSString* t)
{
    NSTextField* f = [NSTextField labelWithString:t];
    f.font = [NSFont boldSystemFontOfSize:13];
    return f;
}

// Un grupo de tiras (p. ej. «Smaart · Salidas»), con su nombre encima.
- (NSView*)groupTitled:(NSString*)title first:(int)first count:(int)count
{
    NSMutableArray* strips = [NSMutableArray array];
    for (int i = 0; i < count; i++) {
        AVMMixerStrip* st = [[AVMMixerStrip alloc] initWithIndex:first + i target:self];
        _strips[first + i] = st;
        [strips addObject:st];
    }
    NSStackView* row = [NSStackView stackViewWithViews:strips];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing = 2;
    NSTextField* t = [NSTextField labelWithString:title];
    t.font = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    t.textColor = [NSColor secondaryLabelColor];
    NSStackView* g = [NSStackView stackViewWithViews:@[ t, row ]];
    g.orientation = NSUserInterfaceLayoutOrientationVertical;
    g.alignment = NSLayoutAttributeLeading;
    g.spacing = 3;
    return g;
}

- (NSView*)sectionTitled:(NSString*)title note:(NSString*)note groups:(NSArray<NSView*>*)groups
{
    NSStackView* row = [NSStackView stackViewWithViews:groups];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment = NSLayoutAttributeTop;
    row.spacing = 16;
    NSTextField* n = [NSTextField labelWithString:note];
    n.font = [NSFont systemFontOfSize:10];
    n.textColor = [NSColor secondaryLabelColor];
    NSStackView* sec = [NSStackView stackViewWithViews:@[ SectionTitle(title), n, row ]];
    sec.orientation = NSUserInterfaceLayoutOrientationVertical;
    sec.alignment = NSLayoutAttributeLeading;
    sec.spacing = 4;
    return sec;
}

- (void)build
{
    self.muteAllButton = [NSButton buttonWithTitle:@"Silenciar todos" target:self action:@selector(muteAll:)];
    self.muteAllButton.toolTip = @"Activa el MUTE de los 34 canales (los volúmenes se conservan).";
    self.restoreButton = [NSButton buttonWithTitle:@"Restaurar 0 dB y quitar mutes…" target:self action:@selector(restoreUnity:)];
    self.restoreButton.toolTip = @"Pone los 34 canales a 0 dB y quita todos los MUTE (pide confirmación).";
    NSButton* help = [NSButton buttonWithTitle:@"Ayuda del mixer" target:self action:@selector(showMixerHelp:)];
    for (NSButton* b in @[ self.muteAllButton, self.restoreButton, help ]) b.controlSize = NSControlSizeSmall, b.font = [NSFont systemFontOfSize:11];
    self.statusLabel = [NSTextField labelWithString:@" "];
    self.statusLabel.font = [NSFont systemFontOfSize:11];
    self.statusLabel.textColor = [NSColor secondaryLabelColor];
    NSStackView* bar = [NSStackView stackViewWithViews:@[ self.muteAllButton, self.restoreButton, help, self.statusLabel ]];
    bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    bar.spacing = 8;

    NSView* inputs = [self sectionTitled:@"ENTRADAS A LA MATRIZ"
                                    note:@"El volumen actúa antes de la matriz: afecta a todos los destinos conectados a ese canal."
                                  groups:@[ [self groupTitled:@"Smaart · Salidas" first:avm_names_src_index(AVM_SRC_SMAART_BASE) count:8],
                                            [self groupTitled:@"LISTENTO · Retornos" first:avm_names_src_index(AVM_SRC_LINK_BASE) count:8],
                                            [self groupTitled:@"Música" first:avm_names_src_index(AVM_SRC_MUSIC_BASE) count:2] ]];
    NSView* outputs = [self sectionTitled:@"SALIDAS DE LA MATRIZ"
                                     note:@"El volumen actúa después del enrutamiento y la suma: solo afecta a ese destino."
                                   groups:@[ [self groupTitled:@"Smaart · Entradas" first:avm_names_dst_index(AVM_DST_SMAART_BASE) count:8],
                                             [self groupTitled:@"LISTENTO · Envíos" first:avm_names_dst_index(AVM_DST_LINK_BASE) count:8] ]];
    NSBox* sep = [[NSBox alloc] init];
    sep.boxType = NSBoxSeparator;

    NSStackView* content = [NSStackView stackViewWithViews:@[ bar, inputs, sep, outputs ]];
    content.orientation = NSUserInterfaceLayoutOrientationVertical;
    content.alignment = NSLayoutAttributeLeading;
    content.spacing = 10;
    content.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [sep.widthAnchor constraintEqualToAnchor:content.widthAnchor constant:-24].active = YES;

    // Documento con desplazamiento (la ventana puede ser más pequeña que el mixer).
    NSView* doc = [[NSView alloc] initWithFrame:NSZeroRect];
    [doc addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:doc.leadingAnchor],
        [content.topAnchor constraintEqualToAnchor:doc.topAnchor],
    ]];
    NSSize fit = content.fittingSize;
    doc.frame = NSMakeRect(0, 0, fit.width, fit.height);
    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, fit.width, fit.height)];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.drawsBackground = NO;
    scroll.documentView = doc;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    // Con el origen arriba: el documento se ancla al borde superior.
    [doc scrollPoint:NSMakePoint(0, fit.height)];

    NSRect screen = [NSScreen mainScreen].visibleFrame;
    NSSize win = NSMakeSize(MIN(fit.width, screen.size.width - 40), MIN(fit.height, screen.size.height - 60));
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, win.width, win.height)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Mixer — AV Matrix";
    self.window.releasedWhenClosed = NO;
    self.window.delegate = self;
    self.window.minSize = NSMakeSize(480, 320);
    self.window.contentMaxSize = fit;
    scroll.frame = self.window.contentView.bounds;
    [self.window.contentView addSubview:scroll];
    [self.window center];
    self.window.frameAutosaveName = @"AVMatrixMixer";
}

- (void)show
{
    if (!self.window) [self build];
    [self reloadFromDriver];
    [self.window makeKeyAndOrderFront:nil];
}

- (void)applyToUI
{
    for (int i = 0; i < AVM_NUM_SOURCES; i++) [_strips[i] showGain:_mx.src_gain[i] mute:_mx.src_mute[i]];
    for (int d = 0; d < AVM_NUM_DESTS; d++) [_strips[AVM_NUM_SOURCES + d] showGain:_mx.dst_gain[d] mute:_mx.dst_mute[d]];
    int muted = 0, att = 0;
    for (int i = 0; i < AVM_NUM_SOURCES; i++) { muted += _mx.src_mute[i]; att += _mx.src_gain[i] < 1.0f; }
    for (int d = 0; d < AVM_NUM_DESTS; d++) { muted += _mx.dst_mute[d]; att += _mx.dst_gain[d] < 1.0f; }
    if (!self.connected && !self.offline)
        self.statusLabel.stringValue = @"Driver no encontrado: el mixer no está disponible.";
    else if (muted == 0 && att == 0)
        self.statusLabel.stringValue = @"Todos los canales a 0 dB y sin MUTE (audio sin tocar).";
    else
        self.statusLabel.stringValue = [NSString stringWithFormat:@"%d en MUTE · %d atenuados", muted, att];
    BOOL on = self.connected || self.offline;
    for (int i = 0; i < AVM_NAMES_COUNT; i++) { _strips[i].fader.enabled = on; _strips[i].mute.enabled = on; }
    self.muteAllButton.enabled = on;
    self.restoreButton.enabled = on;
}

// Lee los ajustes del driver. No pisa un cambio local que aún no se ha enviado.
- (void)reloadFromDriver
{
    if (self.offline) { [self applyToUI]; return; }
    if (_sendTimer) return;
    avm_mixer mx;
    self.connected = avm_find_device(AVM_DEV_SMAART) != kAudioObjectUnknown && avm_get_mixer(&mx) == noErr;
    if (self.connected) _mx = mx;
    [self applyToUI];
}

// Envío agrupado: un movimiento del fader genera muchos eventos; se envía como
// mucho cada 30 ms el último valor (el driver hace la rampa sin chasquidos).
- (void)scheduleSend
{
    [self applyToUI];
    if (_sendTimer) return;
    __weak AVMMixerController* weakSelf = self;
    _sendTimer = [NSTimer timerWithTimeInterval:0.03 repeats:NO block:^(NSTimer* t) {
        (void)t;
        [weakSelf flush];
    }];
    // También mientras se arrastra el fader (modo de seguimiento de eventos).
    [[NSRunLoop currentRunLoop] addTimer:_sendTimer forMode:NSRunLoopCommonModes];
}

- (void)flush
{
    [_sendTimer invalidate];
    _sendTimer = nil;
    self.sendCount++;
    if (!self.offline) {
        OSStatus err = avm_set_mixer(&_mx);
        if (err != noErr) {
            self.statusLabel.stringValue = [NSString stringWithFormat:@"El driver rechazó el ajuste (error %d).", (int)err];
            [self reloadFromDriver];
            return;
        }
    }
    if (self.onChange) self.onChange();
}

- (void)faderMoved:(NSSlider*)sender
{
    int idx = (int)sender.tag;
    float g = avm_fader_gain_from_pos(sender.doubleValue);
    if (idx < AVM_NUM_SOURCES) _mx.src_gain[idx] = g;
    else _mx.dst_gain[idx - AVM_NUM_SOURCES] = g;
    [self scheduleSend];
}

- (void)muteToggled:(NSButton*)sender
{
    int idx = (int)sender.tag;
    uint8_t on = sender.state == NSControlStateValueOn;
    if (idx < AVM_NUM_SOURCES) _mx.src_mute[idx] = on;
    else _mx.dst_mute[idx - AVM_NUM_SOURCES] = on;
    [self scheduleSend];
    [self flush];   // el MUTE se aplica al momento
}

- (void)muteAll:(id)sender
{
    (void)sender;
    for (int i = 0; i < AVM_NUM_SOURCES; i++) _mx.src_mute[i] = 1;
    for (int d = 0; d < AVM_NUM_DESTS; d++) _mx.dst_mute[d] = 1;
    [self scheduleSend];
    [self flush];
}

- (void)restoreUnityConfirmed
{
    avm_mixer_default(&_mx);
    [self scheduleSend];
    [self flush];
}

- (void)restoreUnity:(id)sender
{
    (void)sender;
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = @"¿Restaurar todos los canales a 0 dB y quitar los MUTE?";
    a.informativeText = @"Los 34 canales vuelven a 0 dB sin MUTE: el audio pasa sin tocar. Si había canales en MUTE o atenuados, pueden sonar más fuerte de inmediato.";
    [a addButtonWithTitle:@"Restaurar"];
    [a addButtonWithTitle:@"Cancelar"];
    __weak AVMMixerController* weakSelf = self;
    [a beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
        if (r == NSAlertFirstButtonReturn) [weakSelf restoreUnityConfirmed];
    }];
}

- (void)showMixerHelp:(id)sender
{
    (void)sender;
    [NSApp sendAction:@selector(showMixerManual:) to:nil from:self];
}

- (void)setMeterSources:(const avm_meter_reading*)src dests:(const avm_meter_reading*)dst
{
    if (!self.window.visible) return;
    avm_meter_reading none = { 0.0f, false, false };
    for (int i = 0; i < AVM_NUM_SOURCES; i++) [_strips[i] showReading:src ? src[i] : none];
    for (int d = 0; d < AVM_NUM_DESTS; d++) [_strips[AVM_NUM_SOURCES + d] showReading:dst ? dst[d] : none];
}

- (void)namesChanged
{
    for (int i = 0; i < AVM_NAMES_COUNT; i++) [_strips[i] refreshName];
}

// Al cerrar la ventana se envía cualquier cambio pendiente.
- (void)windowWillClose:(NSNotification*)n
{
    (void)n;
    if (_sendTimer) [self flush];
}

@end

// ---------------------------------------------------------------------------
#pragma mark Controlador

@interface AVMController : NSObject <NSApplicationDelegate, NSWindowDelegate, AVMMatrixViewDelegate, NSPopoverDelegate>
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
@property (strong) NSTimer* meterTimer;
@property (strong) NSPopover* renamePopover;   // panel abierto; se suelta al cerrarse
@property (strong) AVMHelpController* help;    // ventana del manual (se crea al abrirla)
@property (strong) AVMMixerController* mixer;  // ventana del mixer (se crea al abrirla)
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
    [self loadNames];
    [self buildMenu];
    [self buildWindow];
    [self reload];
    [self startListening];
    [self startMeters];
    [self fitWindow];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

// Medidores: se consulta la propiedad 'metr' 15 veces por segundo, solo con la
// ventana visible. Es una lectura de propiedad: no abre ni arranca audio.
- (void)startMeters
{
    __weak AVMController* weakSelf = self;
    self.meterTimer = [NSTimer timerWithTimeInterval:1.0 / 15.0 repeats:YES block:^(NSTimer* t) {
        (void)t;
        [weakSelf pollMeters];
    }];
    // Los medidores siguen moviéndose mientras se arrastra un fader.
    [[NSRunLoop currentRunLoop] addTimer:self.meterTimer forMode:NSRunLoopCommonModes];
    self.meterTimer.tolerance = 0.01;
}

- (void)pollMeters
{
    BOOL mainVisible = (self.window.occlusionState & NSWindowOcclusionStateVisible) != 0;
    BOOL mixerVisible = self.mixer.window.visible && (self.mixer.window.occlusionState & NSWindowOcclusionStateVisible);
    if (!mainVisible && !mixerVisible) return;
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    if (avm_find_device(AVM_DEV_SMAART) == kAudioObjectUnknown || avm_get_meters(s, d) != noErr) {
        [self.matrixView setMeterSources:NULL dests:NULL];   // todo «—»: nunca valores antiguos
        [self.mixer setMeterSources:NULL dests:NULL];
        return;
    }
    if (mainVisible) [self.matrixView setMeterSources:s dests:d];
    if (mixerVisible) [self.mixer setMeterSources:s dests:d];
}

- (void)applicationWillTerminate:(NSNotification*)note
{
    (void)note;
    [self.meterTimer invalidate];
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

    // Menú Ayuda → Manual de AV Matrix (⌘?).
    NSMenuItem* helpItem = [[NSMenuItem alloc] initWithTitle:@"Ayuda" action:NULL keyEquivalent:@""];
    [bar addItem:helpItem];
    NSMenu* helpMenu = [[NSMenu alloc] initWithTitle:@"Ayuda"];
    NSMenuItem* manual = [helpMenu addItemWithTitle:@"Manual de AV Matrix" action:@selector(showHelp:) keyEquivalent:@"?"];
    manual.target = self;
    helpItem.submenu = helpMenu;
    NSApp.mainMenu = bar;
    NSApp.helpMenu = helpMenu;
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
    NSButton* mixerBtn = [NSButton buttonWithTitle:@"Mixer" target:self action:@selector(showMixer:)];
    mixerBtn.toolTip = @"Volumen (solo atenuación) y MUTE de cada canal: entradas antes de la matriz, salidas después de la suma.";
    NSButton* helpBtn = [NSButton buttonWithTitle:@"Ayuda" target:self action:@selector(showHelp:)];
    helpBtn.toolTip = @"Manual de AV Matrix: dispositivos, matriz, presets, medidores, nombres, frecuencias y problemas.";
    for (NSButton* b in @[ measureBtn, directBtn, clearBtn, openBtn, foldBtn, reloadBtn, self.recoverButton, mixerBtn, helpBtn ])
        b.controlSize = NSControlSizeSmall, b.font = [NSFont systemFontOfSize:11];
    self.ratePopup.controlSize = NSControlSizeSmall;
    self.ratePopup.font = [NSFont systemFontOfSize:11];

    NSStackView* toolbar = [NSStackView stackViewWithViews:@[ Label(@"AV Matrix", 14, YES), Label(@"Frecuencia:", 11, NO), self.ratePopup,
                                                              measureBtn, directBtn, clearBtn, openBtn, foldBtn, reloadBtn, self.recoverButton, mixerBtn, helpBtn ]];
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

    // La leyenda y las explicaciones generales están en la ventana de Ayuda.
    self.root = [NSStackView stackViewWithViews:@[ toolbar, self.statusLabel, self.scrollView, self.infoLabel ]];
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
    NSString* text = parts.count
        ? [parts componentsJoinedByString:@", "]
        : [NSString stringWithFormat:@"AV Matrix Smaart · AV Matrix LISTENTO · AV Matrix Música — %.1f kHz", rate / 1000.0];
    // Aviso del mixer: un canal atenuado o en MUTE cambia lo que llega a Smaart.
    avm_mixer mx;
    if (avm_get_mixer(&mx) == noErr && !avm_mixer_is_unity(&mx)) {
        int muted = 0, att = 0;
        for (int i = 0; i < AVM_NUM_SOURCES; i++) { muted += mx.src_mute[i]; att += mx.src_gain[i] < 1.0f; }
        for (int i = 0; i < AVM_NUM_DESTS; i++) { muted += mx.dst_mute[i]; att += mx.dst_gain[i] < 1.0f; }
        text = [text stringByAppendingFormat:@" · Mixer: %d en MUTE, %d atenuados", muted, att];
        if (health != AVM_RATE_HEALTH_INCONSISTENT) self.statusLabel.textColor = [NSColor systemOrangeColor];
    }
    self.statusLabel.stringValue = text;
    [self.mixer reloadFromDriver];
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
        // Mixer cambiado desde otra ventana o desde avmctl.
        AudioObjectPropertyAddress mixr = { AVM_PROPERTY_MIXER, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectAddPropertyListenerBlock(dev, &mixr, dispatch_get_main_queue(), block);
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

// --- Nombres de los canales

- (void)loadNames
{
    NSString* path = NamesFilePath();
    if (!avm_names_load(&gNames, path.fileSystemRepresentation)) {
        // Archivo ilegible: se aparta (no se pierde) y se usan los nombres originales.
        NSString* aside = [path stringByAppendingFormat:@".dañado-%ld", (long)[NSDate date].timeIntervalSince1970];
        [[NSFileManager defaultManager] moveItemAtPath:path toPath:aside error:NULL];
        NSLog(@"AV Matrix: no se pudo leer %@; se apartó como %@", path, aside);
        avm_names_init(&gNames);
    }
}

- (void)matrixView:(AVMMatrixView*)view renameChannel:(int)idx anchor:(NSRect)anchor
{
    if (idx < 0 || idx >= AVM_NAMES_COUNT) return;
    [self.renamePopover close];
    self.renamePopover = nil;
    __weak AVMController* weakSelf = self;
    NSPopover* pop = AVMMakeRenamePopover(idx, ^{ [weakSelf namesChanged]; });
    pop.delegate = self;
    self.renamePopover = pop;
    [pop showRelativeToRect:anchor ofView:view preferredEdge:(idx >= AVM_NUM_SOURCES ? NSRectEdgeMaxX : NSRectEdgeMaxY)];
}

- (void)popoverDidClose:(NSNotification*)n
{
    NSPopover* closed = n.object;
    if (closed != self.renamePopover) return;
    self.renamePopover = nil;
    // Se libera en la siguiente vuelta del bucle, nunca dentro de una acción
    // del propio panel (Guardar, Restaurar, Cancelar).
    dispatch_async(dispatch_get_main_queue(), ^{ (void)closed; });
}

- (void)matrixView:(AVMMatrixView*)view restoreChannelName:(int)idx
{
    (void)view;
    avm_names previous = gNames;
    avm_names_restore(&gNames, idx);
    if (!avm_names_save(&gNames, NamesFilePath().fileSystemRepresentation)) {
        gNames = previous;
        [self showError:@"No se pudo guardar el cambio de nombre en disco."];
        return;
    }
    [self namesChanged];
}

- (void)namesChanged
{
    [self.matrixView setNeedsDisplay:YES];
    [self.mixer namesChanged];
    self.infoLabel.stringValue = @" ";
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

- (void)showHelp:(id)sender
{
    (void)sender;
    if (!self.help) self.help = [[AVMHelpController alloc] init];
    [self.help show];
}

- (void)showMixer:(id)sender
{
    (void)sender;
    if (!self.mixer) {
        // El estado de la ventana principal se actualiza con la notificación
        // del driver ('mixr'), igual que si el cambio viene de avmctl.
        self.mixer = [[AVMMixerController alloc] init];
    }
    [self.mixer show];
}

// Ayuda → sección del mixer (botón «Ayuda del mixer»).
- (void)showMixerManual:(id)sender
{
    [self showHelp:sender];
    for (int i = 0; i < avm_manual_count; i++)
        if (!strcmp(avm_manual_sections[i].id, "mixer")) [self.help showSection:i];
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

#ifndef AVM_APP_NO_MAIN   // las pruebas nativas incluyen este archivo sin main
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
#endif
