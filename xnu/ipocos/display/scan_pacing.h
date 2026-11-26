/* SPDX-License-Identifier: MIT
 * Bounded scanout interval histogram. Serialized by the framebuffer presenter.
 * Bins 0..254 are millisecond ranges; bin 255 is overflow (>=255 ms).
 * Retain idle intervals: selecting an interaction is an analysis decision.
 */
#ifndef HAKIOS_SCAN_PACING_H
#define HAKIOS_SCAN_PACING_H
#include <stdint.h>
struct scan_pacing {
 uint64_t last_us,n,sum_us,max_us,over_50ms;
 uint32_t bins[256];
};
static inline void scan_pacing_add(struct scan_pacing *p,uint64_t us) {
 if (!us || us<=p->last_us) return;
 if (p->last_us) {
  uint64_t gap=us-p->last_us;
  p->n++;p->sum_us+=gap;if(gap>p->max_us)p->max_us=gap;
  if(gap>50000)p->over_50ms++;
  unsigned bin=gap/1000>255?255:(unsigned)(gap/1000);
  p->bins[bin]++;
 }
 p->last_us=us;
}
/* Quantized upper boundary in ms; 256 denotes the overflow bin, not an upper bound. */
static inline unsigned scan_pacing_quantile(const struct scan_pacing *p,unsigned pct) {
 if(!p->n || !pct || pct>100)return 0;
 uint64_t target=(p->n*pct+99)/100,seen=0;
 for(unsigned i=0;i<256;i++){seen+=p->bins[i];if(seen>=target)return i+1;}
 return 0;
}
static inline void scan_pacing_reset(struct scan_pacing *p) {
 uint64_t last=p->last_us;
 p->n=p->sum_us=p->max_us=p->over_50ms=0;
 for(unsigned i=0;i<256;i++)p->bins[i]=0;
 p->last_us=last;
}
#endif
