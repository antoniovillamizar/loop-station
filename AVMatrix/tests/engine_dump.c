// Vuelca la salida del motor (audio de los 16 destinos de Smaart y LISTENTO y
// sus medidores) con matrices y entradas aleatorias en los 4 formatos. Lo usa
// tests/compare_engine_unity.sh para comparar con una revisión anterior: solo
// usa los canales que existían entonces (18 fuentes, 16 destinos, sin EVO ni
// Dante), así el mismo programa compila con los dos motores.
#include "avm_core.h"
#include <stdio.h>
#include <stdlib.h>
#define OLD_SRC 18
#define OLD_DST 16
#define OLD_SRC_GROUPS 3
#define OLD_DST_GROUPS 2
static uint32_t r = 777; static uint32_t rnd(void){ r = r*1664525u+1013904223u; return r; }
static float rf(void){ float v = ((float)(rnd()>>8)/16777216.0f)*2.2f-1.1f; switch(rnd()%20){case 0: v=-0.0f;break;case 1: v=1e-41f;break;case 2: v=1.0f;break;case 3: v=-1.0f;break;} return v; }
static void put_reading(const avm_meter_reading* m){ fwrite(&m->peak,4,1,stdout); unsigned char b[2] = { m->active, m->clip }; fwrite(b,1,2,stdout); }
int main(void){
  avm_engine* e = malloc(sizeof(avm_engine)); avm_engine_init(e);
  uint8_t in[512*8*4], out[512*8*4];
  for (int fmt=0; fmt<AVM_FMT_COUNT; fmt++){
    avm_engine_reset(e);
    uint32_t bps = avm_format_bytes_per_sample((avm_format)fmt);
    int64_t t = 0;
    for (int b=0; b<200; b++){
      if (b%20==0){ avm_matrix m; avm_matrix_clear(&m); for(int d=0;d<OLD_DST;d++) for(int s=0;s<OLD_SRC;s++) if(rnd()%4==0 && avm_route_allowed(d,s)==AVM_ROUTE_OK) m.cell[d][s]=1; avm_engine_set_matrix(e,&m); }
      uint32_t frames = 64 + rnd()%448;
      for (int g=0; g<OLD_SRC_GROUPS; g++){
        int nch = avm_src_group_channels((avm_src_group)g);
        for (uint32_t i=0;i<frames*(uint32_t)nch;i++) avm_float_to_sample(rf(), in+(size_t)i*bps, (avm_format)fmt);
        avm_engine_write_metered(e,(avm_src_group)g,t,frames,in,(avm_format)fmt,(uint32_t)nch,1000000ull*(uint64_t)(b+1));
      }
      for (int g=0; g<OLD_DST_GROUPS; g++){
        avm_engine_read_metered(e,(avm_dst_group)g,t-(int64_t)(rnd()%64),frames,out,(avm_format)fmt,8,1000000ull*(uint64_t)(b+1));
        fwrite(out,1,(size_t)frames*8*bps,stdout);
      }
      avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
      avm_engine_meter_snapshot(e,1000000ull*(uint64_t)(b+1),s,d);
      for (int i=0;i<OLD_SRC;i++) put_reading(&s[i]);
      for (int i=0;i<OLD_DST;i++) put_reading(&d[i]);
      t += frames;
    }
  }
  return 0;
}
