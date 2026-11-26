/* SPDX-License-Identifier: MIT
 * Duchamp I2C0 GPIO137/138, IOCFG_BM input enables, and reset GPIO22.
 * Exact fields from pinctrl-mt6897.c; SET/CLR aliases preserve other pins.
 */
#ifndef HAKIOS_MTK_HAPTIC_GPIO_H
#define HAKIOS_MTK_HAPTIC_GPIO_H
#include <stdint.h>
struct mtk_haptic_pins {
    void *ctx;
    uint32_t (*read)(void *, int, unsigned);
    void (*write)(void *, int, unsigned, uint32_t);
    uint32_t mode[2], direction[2], output[2], ies[2];
};
static inline void mtk_haptic_field(struct mtk_haptic_pins *p, int cfg,
    unsigned off, uint32_t mask, uint32_t value)
{
    p->write(p->ctx,cfg,off+8,mask);
    if (value&mask) p->write(p->ctx,cfg,off+4,value&mask);
}
static inline void mtk_haptic_line(struct mtk_haptic_pins *p,unsigned pin,int release)
{
    unsigned bank=(pin/32)*16,bit=1u<<(pin%32);
    if (release) p->write(p->ctx,0,bank+8,bit);
    else {
        p->write(p->ctx,0,0x100+bank+8,bit);
        p->write(p->ctx,0,bank+4,bit);
    }
}
static inline void mtk_haptic_take(struct mtk_haptic_pins *p)
{
    const unsigned ies[2]={26,29};
    for(unsigned i=0;i<2;i++) {
        unsigned pin=137+i,bank=(pin/32)*16,bit=1u<<(pin%32);
        unsigned off=0x300+(pin/8)*16,mask=15u<<((pin%8)*4);
        p->mode[i]=p->read(p->ctx,0,off)&mask;
        p->direction[i]=p->read(p->ctx,0,bank)&bit;
        p->output[i]=p->read(p->ctx,0,0x100+bank)&bit;
        p->ies[i]=p->read(p->ctx,1,0x80)&(1u<<ies[i]);
        mtk_haptic_line(p,pin,1);
        mtk_haptic_field(p,1,0x80,1u<<ies[i],1u<<ies[i]);
        mtk_haptic_field(p,0,off,mask,0);
    }
}
static inline void mtk_haptic_restore(struct mtk_haptic_pins *p)
{
    const unsigned ies[2]={26,29};
    for(unsigned i=0;i<2;i++) mtk_haptic_line(p,137+i,1);
    for(unsigned i=0;i<2;i++) {
        unsigned pin=137+i,bank=(pin/32)*16,bit=1u<<(pin%32);
        unsigned off=0x300+(pin/8)*16,mask=15u<<((pin%8)*4);
        mtk_haptic_field(p,0,0x100+bank,bit,p->output[i]);
        mtk_haptic_field(p,0,off,mask,p->mode[i]);
        mtk_haptic_field(p,0,bank,bit,p->direction[i]);
        mtk_haptic_field(p,1,0x80,1u<<ies[i],p->ies[i]);
    }
}
static inline void mtk_haptic_reset(struct mtk_haptic_pins *p,int high)
{
    unsigned bit=1u<<22;
    mtk_haptic_field(p,0,0x100,bit,high?bit:0);
    mtk_haptic_field(p,0,0x320,15u<<24,0);
    p->write(p->ctx,0,4,bit);
}
#endif
