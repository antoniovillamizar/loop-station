// AV Matrix — aplicación de matriz de enrutamiento.
//
// Organización visual tipo Dante Controller / Dante Preset Creator:
//   ARRIBA     transmisores (fuentes), nombres verticales, agrupados y desplegables:
//              Smaart (salidas 1-8), LISTENTO (retornos 1-8), Música (L/R).
//   IZQUIERDA  receptores (destinos), agrupados y desplegables:
//              Smaart (entradas 1-8; la 8 es REFERENCIA LOCAL), LISTENTO (envíos 1-8).
//   Cruce      casilla verde con ✓ = conectado; casilla vacía = desconectado;
//              casilla gris con × = bloqueada (lazo de realimentación).
//   Medidores  junto a cada nombre: pico en dBFS, barra y LED rojo de saturación.
//              Fuentes: nivel antes de la matriz. Destinos: señal real entregada,
//              después del enrutamiento y de la suma. «—» = sin audio en ese canal.
//              La app solo lee la propiedad 'metr' del driver: no abre audio.
//
// Cada casilla conecta una fuente con un destino a ganancia unidad. No hay
// ganancias, ecualización, efectos ni delay. El driver también rechaza los lazos.

#import <Cocoa/Cocoa.h>
#include "../Client/avm_client.h"
#include "../Client/avm_names.h"

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
    if (d == AVM_DST_LOCAL_REFERENCE)   // el distintivo REFERENCIA LOCAL se conserva siempre
        return a ? [NSString stringWithFormat:@"Smaart Entrada 8 «%@» (REFERENCIA LOCAL)", a] : @"Smaart Entrada 8 (REFERENCIA LOCAL)";
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
static const CGFloat kLeftW = 386.0;    // nombres de los receptores + sus medidores
static const CGFloat kTopH = 184.0;     // nombres verticales de los transmisores + sus medidores
static const CGFloat kSumW = 40.0;      // columna de aviso de suma, a la derecha

// Medidores de los receptores (a la derecha del nombre, en cada fila).
static const CGFloat kRxMeterX = 254.0, kRxMeterW = 64.0;
static const CGFloat kRxDbX = 322.0, kRxDbW = 44.0;
static const CGFloat kRxClipX = 370.0;
// Medidores de los transmisores (debajo del nombre vertical, en cada columna).
static const CGFloat kTxMeterH = 80.0;  // LED + barra + dBFS
static const CGFloat kTxBarH = 50.0;

// Ancho del distintivo REFERENCIA LOCAL (texto de 9 pt en negrita + márgenes).
static const CGFloat kRefBadgeW = 104.0;

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

// Barra de nivel en `r` (horizontal o vertical, crece hacia la derecha o hacia arriba).
- (void)drawMeterBar:(NSRect)r reading:(avm_meter_reading)m vertical:(BOOL)vertical
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

- (void)drawClipLED:(NSRect)r on:(BOOL)on active:(BOOL)active
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

    // Esquina: títulos de ejes y de las columnas de medidores.
    [@"Transmisores →" drawAtPoint:NSMakePoint(8, kTopH - kTxMeterH - 20) withAttributes:dimAttr];
    [@"nivel antes de la matriz →" drawAtPoint:NSMakePoint(kLeftW - 140, kTopH - kTxMeterH / 2 - 6) withAttributes:dimAttr];
    [@"Receptores ↓" drawAtPoint:NSMakePoint(8, kTopH - 18) withAttributes:dimAttr];
    [@"nivel entregado" drawAtPoint:NSMakePoint(kRxMeterX, kTopH - 30) withAttributes:dimAttr];
    [@"dBFS" drawAtPoint:NSMakePoint(kRxDbX + 14, kTopH - 18) withAttributes:dimAttr];
    [@"SAT" drawAtPoint:NSMakePoint(kRxClipX - 4, kTopH - 18) withAttributes:dimAttr];

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
            CGFloat nameMaxW = kRxMeterX - 8 - 24;
            if (r.channel == AVM_DST_LOCAL_REFERENCE) nameMaxW -= kRefBadgeW + 6;
            [FitString(RxChannelName(r.channel), chanAttr, nameMaxW) drawAtPoint:NSMakePoint(24, y + 1) withAttributes:chanAttr];

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
            if (r.channel == AVM_DST_LOCAL_REFERENCE) {
                NSString* badge = @"REFERENCIA LOCAL";
                NSDictionary* ba = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:9], NSForegroundColorAttributeName: [NSColor whiteColor] };
                NSSize bs = [badge sizeWithAttributes:ba];
                // Distintivo fijo a la derecha del nombre: se conserva con cualquier alias.
                NSRect br = NSMakeRect(kRxMeterX - 8 - kRefBadgeW, y + 1, bs.width + 10, kCell - 6);
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
    if (ri >= 0 && p.x < kRxMeterX - 4 && _rows[ri].channel >= 0) return avm_names_dst_index(_rows[ri].channel);
    if (ci >= 0 && p.y < kTopH - kTxMeterH && _cols[ci].channel >= 0) return avm_names_src_index(_cols[ci].channel);
    return -1;
}

- (NSRect)nameRectForIndex:(int)idx at:(NSPoint)p
{
    if (idx >= AVM_NUM_SOURCES) {
        int ri = [self rowAtY:p.y];
        return NSMakeRect(24, kTopH + ri * kCell, kRxMeterX - 32, kCell);
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

    // Título: hasta 2 líneas (alias de 32 caracteres + «(REFERENCIA LOCAL)»).
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
    self.meterTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 15.0 repeats:YES block:^(NSTimer* t) {
        (void)t;
        [weakSelf pollMeters];
    }];
    self.meterTimer.tolerance = 0.01;
}

- (void)pollMeters
{
    if (!(self.window.occlusionState & NSWindowOcclusionStateVisible)) return;
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    if (avm_find_device(AVM_DEV_SMAART) == kAudioObjectUnknown || avm_get_meters(s, d) != noErr) {
        [self.matrixView setMeterSources:NULL dests:NULL];   // todo «—»: nunca valores antiguos
        return;
    }
    [self.matrixView setMeterSources:s dests:d];
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

    NSTextField* legend = [NSTextField wrappingLabelWithString:@"Verde ✓ = conectado · vacía = desconectada · gris × = bloqueada (lazo) · ⊟/⊞ plegar o desplegar grupo · Σ = suma de fuentes.\n"
                                @"Medidores (pico dBFS): fuentes = antes de la matriz · destinos = señal entregada tras la suma · ● rojo = saturación (2 s) · — = sin audio. "
                                @"Solo transporte y enrutamiento: sin ecualización, efectos, análisis ni delay."];
    legend.font = [NSFont systemFontOfSize:10];
    legend.preferredMaxLayoutWidth = 900;
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
