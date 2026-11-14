/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 diskOS contributors */
/* my_write6.c - diskOS rootfs writer WITH an installed-base gate (X2000 Disc, GD5F2GM7 SPI-NAND, mask-ROM DRAM tool).
 *
 * my_write5 + a gate that runs BEFORE the NAND is unlocked or erased, in the same usbboot session:
 *   1. the host's "flash plan" blob (BLOB) must be well-formed: magic, schema, size, reserved words zero, and its own
 *      SHA-256; it names the base the image was built for (main kernel, recovery kernel, recovery rootfs: exact length
 *      and SHA-256 each) and the image's exact length + SHA-256;
 *   2. the downloaded image at SRC must hash to the blob's image digest (binds the plan to the bytes being written and
 *      catches a corrupt download or a stale blob from another run);
 *   3. NAND reset; on-die ECC must be ON and block protection still ON (the power-on defaults) - if not, something ran
 *      before us: refuse, power-cycle and retry;
 *   4. the three base partitions are read (per-partition bad-block skipping with the same double-read OOB verdict the
 *      writer uses), each length taken from the partition's OWN header (uImage size+64; squashfs bytes_used rounded up
 *      to 4 KiB) and required to equal the blob's, each page's ECC status checked (uncorrectable = refuse), hashed;
 *   5. only an exact match of all three unlocks the NAND and continues exactly as my_write5 (scan, erase, program,
 *      verify). Any refusal returns with NOTHING unlocked, erased or programmed, and reports what it saw.
 * The writer only ever erases/programs from START_BLOCK (the main rootfs); the base partitions are read-only here.
 *
 * Builds (installer/flash/build_nand.sh):
 *   default      production writer
 *   -DPROBE_ONLY reads + reports the base (no blob, no image); contains NO erase/program/unlock code or commands
 *   -DGATE_ONLY  full gate (blob + image + base), stops after the verdict; NO erase/program/unlock code or commands
 * The debug block at DBG keeps my_write5's layout (words 0..103) and adds the gate at words 128..199. */
typedef unsigned int u32; typedef unsigned char u8;
#include "sha256_min.h"

#if defined(PROBE_ONLY) || defined(GATE_ONLY)
#define NO_WRITE_CODE 1
#endif
#ifndef START_BLOCK
#define START_BLOCK 80u     /* rootfs @0xA00000 / 0x20000 */
#endif
#ifndef NLOGBLOCKS
#define NLOGBLOCKS 768u     /* IMG_SIZE 100663296 B / 128KB = 768 blocks; MUST match imagebuild IMG_SIZE */
#endif
#ifdef PROBE_ONLY
#define VARIANT 2u
#elif defined(GATE_ONLY)
#define VARIANT 3u
#else
#define VARIANT 1u
#endif

#define SFC_BASE 0xb3440000u
#define CPM_BASE 0xb0000000u
#define CPM_CLKGR 0x20u
#define CPM_SSICDR 0x74u
#define CPM_CPMPCR 0x14u
#define SFC_GLB 0x0000
#define SFC_DEV_CONF 0x0004
#define SFC_DEV_STA_EXP 0x0008
#define SFC_DEV_STA_MSK 0x0010
#define SFC_TRAN_CONF0 0x0014
#define SFC_TRAN_LEN 0x002c
#define SFC_DEV_ADDR0 0x0030
#define SFC_DEV_ADDR_PLUS0 0x0048
#define SFC_MEM_ADDR 0x0060
#define SFC_TRIG 0x0064
#define SFC_SR 0x0068
#define SFC_SCR 0x006c
#define SFC_INTC 0x0070
#define SFC_CGE 0x0078
#define SFC_CMD_IDX 0x007c
#define SFC_ARG0 0x0080
#define SFC_ARG1 0x0084
#define SFC_ARG2 0x0088
#define SFC_ARG3 0x008c
#define SFC_UNK0 0x009c
#define SFC_RM_DR 0x1000
#define GLB_TRAN_DIR_OFFSET 13
#define GLB_THRESHOLD_OFFSET 7
#define GLB_THRESHOLD_MSK (0x3f<<7)
#define GLB_PHASE_NUM_OFFSET 3
#define GLB_PHASE_NUM_MSK (0x7<<3)
#define THRESHOLD 32
#define END (1<<4)
#define TRAN_REQ (1<<3)
#define RECE_REQ (1<<2)
#define CLR_END (1<<4)
#define CLR_TREQ (1<<3)
#define CLR_RREQ (1<<2)
#define TRIG_START (1<<0)
#define TRIG_STOP (1<<1)
#define TRIG_FLUSH (1<<2)
#define CMD_RDID 0x9f
#define CMD_PARD 0x13
#define CMD_FRCH 0x0b
#define CMD_GET_FEATURE 0x0f
#define CMD_SET_FEATURE 0x1f
#define CMD_RESET 0xff
#define CMD_WREN 0x06
#define CMD_PLOAD 0x02
#define CMD_PEXEC 0x10
#define CMD_BERASE 0xd8
#define ADDR_STATUS 0xc0
#define ADDR_PROTECT 0xa0
#define ADDR_FEATURE 0xb0
#define ADDRLEN 2u
#define GUARD 800000u
#define ST_OIP 0x01
#define ST_EFAIL 0x04
#define ST_PFAIL 0x08
#ifdef HOST_SIM
#include HOST_SIM   /* tests: a simulated NAND at the command level (tests/release) replaces the MMIO layer below */
#else
static inline void w32(u32 a,u32 v){*(volatile u32*)a=v;}
static inline u32 r32(u32 a){return *(volatile u32*)a;}
#define sfc_writel(v,o) w32(SFC_BASE+(o),(u32)(v))
#define sfc_readl(o) r32(SFC_BASE+(o))
#define GPIO_PORTE 0xb0010400u
#define SFC_PINS   0x003f0000u
static void mux_sfc_pins(void){
    w32(GPIO_PORTE+0x18,SFC_PINS); w32(GPIO_PORTE+0x28,SFC_PINS);
    w32(GPIO_PORTE+0x38,SFC_PINS); w32(GPIO_PORTE+0x48,SFC_PINS);
}
static u32 g_id,g_len,g_arg0,g_arg1,g_arg2,g_arg3;
static void send_raw(int dir){
    u32 v;
    sfc_writel(TRIG_STOP,SFC_TRIG);
    v=sfc_readl(SFC_CMD_IDX); v=(v&~0x3fu)|(g_id&0x3fu); sfc_writel(v,SFC_CMD_IDX);
    sfc_writel(g_arg0,SFC_ARG0); sfc_writel(g_arg1,SFC_ARG1);
    sfc_writel(g_arg2,SFC_ARG2); sfc_writel(g_arg3,SFC_ARG3);
    v=sfc_readl(SFC_CMD_IDX)&0x7fffffffu; if(g_len)v|=0x80000000u; sfc_writel(v,SFC_CMD_IDX);
    if(g_len){ v=sfc_readl(SFC_CMD_IDX)&~0x40000000u; v|=((u32)dir&1u)<<30; sfc_writel(v,SFC_CMD_IDX); }
    {u32 g=sfc_readl(SFC_GLB); g&=~((1u<<GLB_TRAN_DIR_OFFSET)|GLB_PHASE_NUM_MSK|GLB_THRESHOLD_MSK|(1u<<6)|3u);
     g|=(THRESHOLD<<GLB_THRESHOLD_OFFSET)|(1u<<GLB_PHASE_NUM_OFFSET)|2u|0x4000u|(((u32)dir)<<GLB_TRAN_DIR_OFFSET);
     sfc_writel(g,SFC_GLB);}
    sfc_writel(g_len,SFC_TRAN_LEN); sfc_writel(0,SFC_MEM_ADDR);
    sfc_writel(TRIG_FLUSH,SFC_TRIG); sfc_writel(TRIG_START,SFC_TRIG);
}
#endif /* HOST_SIM */
#ifdef NO_WRITE_CODE
static void cmd_tick(void);   /* timing sample on every NAND command (no-write builds; defined with the gate words) */
#define CMD_TICK() cmd_tick()
#else
#define CMD_TICK() ((void)0)
#endif
static void cmd(u32 c,u32 len,u32 addr,u32 aw,u32 dmy,u32 den,int dir){
    (void)aw;(void)dmy;(void)den;
    CMD_TICK();
    g_arg0=g_arg1=g_arg2=g_arg3=0;
    if(c==0x9f) g_id=aw?2:1;
    else if(c==0xff) g_id=0;
    else if(c==0x0f){ g_id=4; g_arg2=addr; }
    else if(c==0x1f){ g_id=3; g_arg2=addr; }
    else if(c==0x13){ g_id=5; g_arg1=addr; g_arg2=ADDR_STATUS; }
    else if(c==0x0b){ g_id=7; g_arg0=addr; }
#ifndef NO_WRITE_CODE
    else if(c==0x06){ g_id=8; }                 /* WRITE_ENABLE: cmd-only */
    else if(c==0x02){ g_id=9; g_arg0=addr; }    /* PROGRAM_LOAD: COL addr, data-out */
    else if(c==0x10){ g_id=10; g_arg1=addr; }   /* PROGRAM_EXECUTE: ROW addr */
    else if(c==0xd8){ g_id=11; g_arg1=addr; }   /* BLOCK_ERASE: ROW addr */
#endif
    else g_id=0x3f;
    g_len=len; send_raw(dir);
}
#ifndef HOST_SIM
static int clr_end(void){u32 g=0; while(!(sfc_readl(SFC_SR)&END)){if(++g>GUARD)return -1;} sfc_writel(CLR_END,SFC_SCR); return 0;}
static int rd(u32*d,u32 length){
    u32 done=0,fn,sr,len=(length+3)/4,g=0; int i;
    while(done<len){ if(++g>GUARD)return -1; sr=sfc_readl(SFC_SR);
        if(sr&RECE_REQ){ sfc_writel(CLR_RREQ,SFC_SCR); fn=((len-done)>THRESHOLD)?THRESHOLD:(len-done);
            for(i=0;i<(int)fn;i++){*d++=sfc_readl(SFC_RM_DR);done++;} g=0; CMD_TICK();} }   /* sample per FIFO chunk */
    return clr_end();
}
static int wr(u32*d){u32 g=0; while(!(sfc_readl(SFC_SR)&TRAN_REQ)){if(++g>GUARD)return -1;} sfc_writel(CLR_TREQ,SFC_SCR); sfc_writel(*d,SFC_RM_DR); return clr_end();}
#ifndef NO_WRITE_CODE
__attribute__((noinline)) static int wr_bulk(u32*s,u32 length){
    u32 done=0,fn,sr,len=(length+3)/4,g=0; int i;
    while(done<len){ if(++g>GUARD)return -1; sr=sfc_readl(SFC_SR);
        if(sr&TRAN_REQ){ sfc_writel(CLR_TREQ,SFC_SCR); fn=((len-done)>THRESHOLD)?THRESHOLD:(len-done);
            for(i=0;i<(int)fn;i++){sfc_writel(*s++,SFC_RM_DR);done++;} g=0;} }
    return clr_end();
}
#endif
static void set_clock(void){
    u32 reg=CPM_BASE+CPM_SSICDR, v=r32(reg);
    u32 src=(v>>30)&3;
    u32 pcr=r32(CPM_BASE+(src?CPM_CPMPCR:0x10));
    u32 m=((pcr>>20)&0xfff)+1, n=((pcr>>14)&0x3f)+1, od=1u<<((pcr>>11)&7u);
    u32 pll=(24u*m*2u)/n/od, cdr=((pll+49u)/50u-1u)&0xff;
    w32(CPM_BASE+CPM_CLKGR, r32(CPM_BASE+CPM_CLKGR)&~(1u<<2));
    v&=~(0xffu|(3u<<27)); v|=(1u<<29)|cdr;
    w32(reg,v);
    {u32 g=0; while((r32(reg)&(1u<<28))){if(++g>GUARD)break;}}
}
static void sfc_reset_regs(void){
    int n; u32 v;
    for(n=0;n<6;n++){sfc_writel(0,SFC_TRAN_CONF0+n*4);sfc_writel(0,SFC_UNK0+n*4);sfc_writel(0,SFC_DEV_ADDR0+n*4);sfc_writel(0,SFC_DEV_ADDR_PLUS0+n*4);}
    sfc_writel(0,SFC_DEV_CONF);sfc_writel(0,SFC_DEV_STA_EXP);sfc_writel(0,SFC_DEV_STA_MSK);
    sfc_writel(0,SFC_TRAN_LEN);sfc_writel(0,SFC_MEM_ADDR);sfc_writel(0,SFC_TRIG);
    sfc_writel(0,SFC_SCR);sfc_writel(0,SFC_INTC);sfc_writel(0,SFC_CGE);sfc_writel(0,SFC_RM_DR);
    v=sfc_readl(SFC_GLB); v=(v&~3u)|2u; sfc_writel(v,SFC_GLB);
    sfc_writel(TRIG_STOP,SFC_TRIG);
    sfc_writel(0x1f,SFC_SCR); sfc_writel(0x1f,SFC_INTC);
    sfc_writel((1<<1)|(1<<0)|(1<<2)|(2<<5),SFC_DEV_CONF);
    v=sfc_readl(SFC_GLB); v&=~(GLB_THRESHOLD_MSK|(1u<<6)|3u);
    v|=(THRESHOLD<<GLB_THRESHOLD_OFFSET)|2u|0x4000u; sfc_writel(v,SFC_GLB);
}
#endif /* HOST_SIM */
#define SFC_CDT 0x0800
#define CDT_XFER(aw,dmy,den,cmd) (((u32)(aw)<<26)|(1u<<24)|((u32)(dmy)<<17)|((u32)(den)<<16)|((cmd)&0xffffu))
#define CDT_LINK(link,addrkind,tm) (((u32)(link)<<31)|((u32)(tm)<<4)|((addrkind)&0xfu))
#ifndef HOST_SIM
static void cdt_write(u32 idx,u32 w0,u32 w1,u32 w2,u32 w3){
    u32 b=SFC_BASE+SFC_CDT+idx*16u; w32(b,w0); w32(b+4,w1); w32(b+8,w2); w32(b+12,w3);
}
#endif
/* Read-side command templates only. The write templates (8..11) are installed by cdt_init_write, which exists only
 * in the production build and runs only after the gate passed. */
static void cdt_init_read(void){
    cdt_write(0, CDT_LINK(0,0,0), CDT_XFER(0,0,0,0xff), 0,0);            /* RESET */
    cdt_write(1, CDT_LINK(0,0,0), CDT_XFER(0,0,1,0x9f), 0,0);            /* RDID aw0 */
    cdt_write(2, CDT_LINK(0,1,0), CDT_XFER(1,0,1,0x9f), 0,0);            /* RDID aw1 */
    cdt_write(3, CDT_LINK(0,2,0), CDT_XFER(1,0,1,0x1f), 0,0);            /* SET_FEATURE */
    cdt_write(4, CDT_LINK(0,2,0), CDT_XFER(1,0,1,0x0f), 0,0);            /* GET_FEATURE */
    cdt_write(5, CDT_LINK(1,1,0), CDT_XFER(3,0,0,0x13), 0,0);            /* PAGE_READ linked->6 */
    cdt_write(6, CDT_LINK(0,2,0), (1u<<25)|CDT_XFER(1,0,1,0x0f), 0,1);   /* hw OIP poll */
    cdt_write(7, CDT_LINK(0,0,0), CDT_XFER(2,8,1,0x0b), 0,0);            /* READ_CACHE */
    cdt_write(8, 0,0,0,0); cdt_write(9, 0,0,0,0); cdt_write(10,0,0,0,0); cdt_write(11,0,0,0,0);   /* no write templates */
}
#ifndef NO_WRITE_CODE
__attribute__((noinline)) static void cdt_init_write(void){
    cdt_write(8, CDT_LINK(0,0,0), CDT_XFER(0,0,0,0x06), 0,0);            /* WRITE_ENABLE: cmd-only */
    cdt_write(9, CDT_LINK(0,2,0), CDT_XFER(2,0,1,0x02), 0,0);            /* PROGRAM_LOAD */
    cdt_write(10,CDT_LINK(0,1,0), CDT_XFER(3,0,0,0x10), 0,0);           /* PROGRAM_EXECUTE */
    cdt_write(11,CDT_LINK(0,1,0), CDT_XFER(3,0,0,0xd8), 0,0);           /* BLOCK_ERASE */
}
#endif
static int nand_reset(void){u32 st,g=0;
    cmd(CMD_RESET,0,0,0,0,0,0); if(clr_end())return -1;
    do{if(++g>GUARD)return -2; cmd(CMD_GET_FEATURE,1,ADDR_STATUS,1,0,1,0); if(rd(&st,1))return -3;}while(st&ST_OIP);
    return 0;}
static int get_feature(u32 addr, u32 *out){ u32 v=0; cmd(CMD_GET_FEATURE,1,addr,1,0,1,0); if(rd(&v,1)) return -1; *out = v & 0xffu; return 0; }
static int read_page(u32 page,u32 col,u8*dst,u32 len){
    cmd(CMD_PARD,0,page,3,0,0,0); if(clr_end())return -2;
    cmd(CMD_FRCH,len,col,ADDRLEN,8,1,0); return rd((u32*)dst,len);}
/* A data page read with its on-die ECC verdict: 0 clean, 1 corrected, -1 uncorrectable, -2 read failure.
 * C0 ECCS (bits 5:4) after PAGE_READ: 00 clean, 01/11 corrected, 10 uncorrectable (GD5F2GM7 datasheet). */
static int read_page_ecc(u32 page, u8 *dst){
    u32 st;
    cmd(CMD_PARD,0,page,3,0,0,0); if(clr_end()) return -2;
    if(get_feature(ADDR_STATUS, &st)) return -2;
    if(((st >> 4) & 3u) == 2u) return -1;
    cmd(CMD_FRCH,2048,0,ADDRLEN,8,1,0); if(rd((u32*)dst,2048)) return -2;
    return ((st >> 4) & 3u) ? 1 : 0;
}
#define PAGE_SZ 2048u
#define MAXTRIES 6u
static int oob_read1(u32 pb, u8 *mk){
    u32 t, m;
    for(t=0;t<MAXTRIES;t++){
        if(t>0 && nand_reset()!=0) continue;
        m=0xFFFFFFFFu;
        if(read_page(pb*64u, PAGE_SZ, (u8*)&m, 4u)==0){ *mk=(u8)(m & 0xFFu); return 0; }
    }
    return -1;
}
/* 0 good, 1 bad, -1 undeterminable (two independent reads must agree) */
static int oob_verdict(u32 pb){
    u8 a, b;
    if(oob_read1(pb, &a)!=0) return -1;
    if(oob_read1(pb, &b)!=0) return -1;
    if(a!=b) return -1;
    return (a==0xFFu) ? 0 : 1;
}

/* ---- the base gate -------------------------------------------------------------------------------------------- */
#define DBG   0xa0a00000u
#define BLOB  0xa0b00000u
#define SRC   0xa1000000u
#ifndef HOST_SIM
#define DBGP  ((volatile u32 *)DBG)
#define BLOBP ((const volatile u32 *)BLOB)
#define SRCP  ((const u8 *)SRC)
#define SRC_AT(off) ((const u8*)(SRC + (u32)(off)))   /* the image at byte offset off */
#define SRC_WORD0   (*(volatile u32*)SRC)
#else
#define SRC_AT(off) (SRCP + (off))
#define SRC_WORD0   (*(const volatile u32*)SRCP)
#endif
#define BLOB_MAGIC  0x42534744u   /* "DGSB" little-endian */
#define BLOB_SCHEMA 1u
#define BLOB_WORDS  68u
/* blob words: 0 magic, 1 schema, 2 size (bytes), 3 flags(0), 4..7 base id (16 ASCII bytes),
 *   8 kernel len, 9..16 kernel sha | 17 rkernel len, 18..25 sha | 26 rrootfs len, 27..34 sha |
 *   35 image len, 36..43 image sha | 44..51 catalogue fingerprint | 52..59 reserved (0) | 60..67 sha256(words 0..59) */
/* debug words 128..199 (gate) */
#define G_RESULT   128u   /* 1 pass, else a DEAD code */
#define G_A0       129u
#define G_B0       130u
#define G_PART     131u   /* 3 x {len, sha[8], bad blocks in partition} = 3 x 10 words: 131..160 */
#define G_ECC_CORR 161u
#define G_ECC_BAD  162u   /* first uncorrectable page, else 0xFFFFFFFF */
#define G_IMG_SHA  163u   /* 163..170 */
#define G_SUBCODE  171u
#define G_ERASING  172u   /* production: set to 1 immediately before the first erase */
#define G_VARIANT  173u
#define G_SCHEMA   174u
#define G_BADPOS   200u   /* 200..223: up to 8 bad-block positions per base partition (3 x 8) */
#ifdef NO_WRITE_CODE
/* no-write builds only (device qualification): the NAND's protect/feature registers re-read AFTER the verdict, and the
 * run's duration. CP0 Count wraps within seconds at CPU clock rates, so it is sampled on every NAND command (cmd), every
 * FIFO chunk of a page read (rd), every 64 KiB of the image hash and every hashed page - so no two samples are more than
 * one GUARD-bounded poll loop apart, far below one wrap - and the deltas are accumulated into a
 * 64-bit total (lo/hi words). Snapshots are 64-bit too. CPCCR/CPAPCR are recorded raw for the host's conversion. */
#define G_POST_OK  175u   /* 1 = both registers re-read after the verdict */
#define G_POST_A0  176u
#define G_POST_B0  177u
#define G_TLAST    178u   /* last Count sample */
#define G_TACC     179u   /* accumulated ticks since entry, low word ... */
#define G_TACC_HI  184u   /* ... and high word */
#define G_T_IMG    180u   /* snapshot: end of the image hash (lo; hi at 186) */
#define G_T_END    181u   /* snapshot: the verdict (lo; hi at 187) */
#define G_CPCCR    182u
#define G_CPAPCR   183u
#define G_T_IMG0   185u   /* snapshot: start of the image hash (lo; hi at 188) */
#define G_T_IMG_HI  186u
#define G_T_END_HI  187u
#define G_T_IMG0_HI 188u
#ifdef HOST_SIM
static u32 g_simcount;                                          /* host tests: big steps, so the 64-bit carry is exercised */
static inline u32 cp0_count(void){ return g_simcount += 0x10000000u; }
#define CLKREG(o) 0u
#else
static inline u32 cp0_count(void){ u32 c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }
#define CLKREG(o) r32(CPM_BASE + (o))
#endif
static void tick(volatile u32 *dbg){
    u32 now = cp0_count(), d = now - dbg[G_TLAST], lo = dbg[G_TACC] + d;
    if(lo < d) dbg[G_TACC_HI]++;
    dbg[G_TACC] = lo; dbg[G_TLAST] = now;
}
static void snap(volatile u32 *dbg, u32 lo, u32 hi){ tick(dbg); dbg[lo] = dbg[G_TACC]; dbg[hi] = dbg[G_TACC_HI]; }
static void cmd_tick(void){ tick(DBGP); }
#define TICK(d) tick(d)
#else
#define TICK(d) ((void)0)
#endif
/* the three base partitions, in physical 128 KiB blocks (fixed Disc layout, docs/HARDWARE.md) */
static const u32 PART_START[3] = { 16u, 1104u, 1168u };   /* kernel, kernel2 (recovery), rootfs2 (recovery) */
static const u32 PART_BLOCKS[3] = { 64u, 64u, 200u };
static const u32 PART_KIND[3] = { 0u, 0u, 1u };            /* 0 uImage, 1 squashfs */

static u32 be32p(const u8 *p){ return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3]; }
static u32 le32p(const u8 *p){ return ((u32)p[3]<<24)|((u32)p[2]<<16)|((u32)p[1]<<8)|p[0]; }

/* Hash one base partition: logical blocks = its good physical blocks in order (per-partition skip, never past its
 * end). Length from its own header. Returns 0, or a DEAD code. Fills len + digest + bad count into dbg. */
static u32 hash_partition(volatile u32 *dbg, u32 which, u32 want_len, int check_len, u32 *ecc_corr){
    u32 good[200], ngood = 0, nbad = 0, b, i;
    u8 page[2048] __attribute__((aligned(4)));
    volatile u32 *out = dbg + G_PART + which * 10u;
    for(b = 0; b < PART_BLOCKS[which]; b++){
        int v = oob_verdict(PART_START[which] + b);
        if(v < 0){ dbg[G_SUBCODE] = PART_START[which] + b; return 0xDEAD000Cu; }   /* marker unreadable */
        if(v == 1){ if(nbad < 8u) dbg[G_BADPOS + which * 8u + nbad] = PART_START[which] + b; nbad++; }
        else good[ngood++] = PART_START[which] + b;
    }
    out[9] = nbad;
    if(ngood == 0) return 0xDEAD000Du;
    int e = read_page_ecc(good[0] * 64u, page);
    if(e == -1){ dbg[G_ECC_BAD] = good[0] * 64u; return 0xDEAD000Bu; }
    if(e < 0) return 0xDEAD000Eu;
    if(e == 1) (*ecc_corr)++;
    u32 len;
    if(PART_KIND[which] == 0){
        if(be32p(page) != 0x27051956u) return 0xDEAD0010u;                     /* not a uImage */
        u32 sz = be32p(page + 12);
        if(sz > 0xFFFFFFFFu - 64u) return 0xDEAD0010u;
        len = sz + 64u;
    } else {
        if(le32p(page) != 0x73717368u) return 0xDEAD0011u;                     /* not a squashfs ("hsqs") */
        u32 lo = le32p(page + 40), hi = le32p(page + 44);                        /* bytes_used, 64-bit */
        if(hi != 0 || lo < 96u || lo > 0xFFFFFFFFu - 4095u) return 0xDEAD0011u;
        len = (lo + 4095u) & ~4095u;
    }
    out[0] = len;
    if(len > ngood * 64u * PAGE_SZ) return 0xDEAD0012u;                         /* longer than the partition */
    if(check_len && len != want_len) return 0xDEAD0008u;                         /* not the expected base */
    sha256_ctx c; sha256_init(&c);
    u32 left = len, lb = 0, pg = 0;
    while(left){
        u32 n = left > PAGE_SZ ? PAGE_SZ : left;
        if(!(lb == 0 && pg == 0)){                                               /* page 0 of block 0 already read */
            e = read_page_ecc(good[lb] * 64u + pg, page);
            if(e == -1){ dbg[G_ECC_BAD] = good[lb] * 64u + pg; return 0xDEAD000Bu; }
            if(e < 0) return 0xDEAD000Eu;
            if(e == 1) (*ecc_corr)++;
        }
        sha256_update(&c, page, n);
        TICK(dbg);
        left -= n;
        if(++pg == 64u){ pg = 0; lb++; }
    }
    u32 d[8]; sha256_final(&c, d);
    for(i = 0; i < 8; i++) out[1 + i] = d[i];
    return 0;
}

/* Returns 1 when the gate passed. PROBE_ONLY: reports the base, never passes. */
static int base_gate(volatile u32 *dbg){
    u32 i, a0 = 0, b0 = 0, corr = 0, r;
    int expect = 1;
#ifdef PROBE_ONLY
    expect = 0;
#endif
    const volatile u32 *bl = BLOBP;
    if(expect){
        /* 1. the flash plan */
        if(bl[0] != BLOB_MAGIC || bl[1] != BLOB_SCHEMA || bl[2] != BLOB_WORDS * 4u || bl[3] != 0u){ dbg[G_SUBCODE] = 1; return (dbg[G_RESULT] = 0xDEAD0007u), 0; }
        for(i = 52; i < 60; i++) if(bl[i] != 0u){ dbg[G_SUBCODE] = 2; return (dbg[G_RESULT] = 0xDEAD0007u), 0; }
        { sha256_ctx c; u32 d[8], j; u8 w[4]; sha256_init(&c);
          for(j = 0; j < 60; j++){ u32 x = bl[j]; w[0] = (u8)x; w[1] = (u8)(x >> 8); w[2] = (u8)(x >> 16); w[3] = (u8)(x >> 24); sha256_update(&c, w, 4); }
          sha256_final(&c, d);
          for(j = 0; j < 8; j++) if(d[j] != bl[60 + j]){ dbg[G_SUBCODE] = 3; return (dbg[G_RESULT] = 0xDEAD0007u), 0; } }
        /* 2. the image being written must be the one the plan was made for */
        { u32 ilen = bl[35], j, d[8]; sha256_ctx c;
          if(ilen != NLOGBLOCKS * 64u * PAGE_SZ){ dbg[G_SUBCODE] = 4; return (dbg[G_RESULT] = 0xDEAD0007u), 0; }   /* the plan must cover EVERY byte written */
          sha256_init(&c);
#ifdef NO_WRITE_CODE
          snap(dbg, G_T_IMG0, G_T_IMG0_HI);
          { u32 off; for(off = 0; off < ilen; off += 65536u){ sha256_update(&c, SRCP + off, ilen - off < 65536u ? ilen - off : 65536u); tick(dbg); } }
#else
          sha256_update(&c, SRCP, ilen);
#endif
          sha256_final(&c, d);
#ifdef NO_WRITE_CODE
          snap(dbg, G_T_IMG, G_T_IMG_HI);
#endif
          for(j = 0; j < 8; j++) dbg[G_IMG_SHA + j] = d[j];
          for(j = 0; j < 8; j++) if(d[j] != bl[36 + j]) return (dbg[G_RESULT] = 0xDEAD0009u), 0; }
    }
    /* 3. NAND at its power-on defaults: ECC on, blocks protected */
    if(nand_reset() != 0) return (dbg[G_RESULT] = 0xDEAD0001u), 0;
    if(get_feature(ADDR_FEATURE, &b0) || get_feature(ADDR_PROTECT, &a0)) return (dbg[G_RESULT] = 0xDEAD0001u), 0;
    dbg[G_A0] = a0; dbg[G_B0] = b0;
    /* A0: BP2..BP0 (bits 5:3) = 111 with CMP (bit 1) = 0 is "every block protected" (the power-on state; GD5F2GM7
     * datasheet - the exact value is recorded in dbg and qualified by the PROBE_ONLY run). Anything else means
     * something changed the NAND before us. */
    if((b0 & 0x10u) == 0u || (a0 & 0x3Au) != 0x38u) return (dbg[G_RESULT] = 0xDEAD000Au), 0;   /* power-cycle and retry */
    /* 4. the base partitions */
    for(i = 0; i < 3u; i++){
        r = hash_partition(dbg, i, expect ? bl[8u + i * 9u] : 0u, expect, &corr);
        dbg[G_ECC_CORR] = corr;
        if(r) return (dbg[G_RESULT] = r), 0;
    }
    if(!expect){ dbg[G_RESULT] = 0x9A7E0B5Eu; return 0; }                           /* PROBE_ONLY: reported, not passed */
    for(i = 0; i < 3u; i++){
        u32 j;
        for(j = 0; j < 8u; j++) if(dbg[G_PART + i * 10u + 1u + j] != bl[9u + i * 9u + j]) return (dbg[G_RESULT] = 0xDEAD0008u), 0;
    }
    dbg[G_RESULT] = 1u;
    return 1;
}

#ifndef NO_WRITE_CODE
__attribute__((noinline)) static int set_features(void){u32 x;             /* unlock block-protect + enable on-die ECC */
    cmd(CMD_SET_FEATURE,1,ADDR_PROTECT,1,0,1,1); x=0; if(wr(&x))return -1;
    cmd(CMD_SET_FEATURE,1,ADDR_FEATURE,1,0,1,1); x=(1<<4); return wr(&x);}
__attribute__((noinline)) static int wait_ready(u32*stout){u32 st,g=0;
    do{if(++g>GUARD)return -1; cmd(CMD_GET_FEATURE,1,ADDR_STATUS,1,0,1,0); if(rd(&st,1))return -2;}while(st&ST_OIP);
    if(stout)*stout=st; return 0;}
__attribute__((noinline)) static int block_erase(u32 page){u32 st; int r;
    cmd(CMD_WREN,0,0,0,0,0,0); if(clr_end())return -10;
    cmd(CMD_BERASE,0,page,3,0,0,0); if(clr_end())return -11;
    r=wait_ready(&st); if(r)return -12;
    if(st&ST_EFAIL)return -13;
    return 0;}
__attribute__((noinline)) static int program_page(u32 page,u8*src){u32 st; int r;
    cmd(CMD_WREN,0,0,0,0,0,0); if(clr_end())return -20;
    cmd(CMD_PLOAD,2048,0,2,0,1,1); if(wr_bulk((u32*)src,2048))return -21;
    cmd(CMD_PEXEC,0,page,3,0,0,0); if(clr_end())return -22;
    r=wait_ready(&st); if(r)return -23;
    if(st&ST_PFAIL)return -24;
    return 0;}
#define PART_END (START_BLOCK + NLOGBLOCKS + 64u)
#define MAXBAD   64u
static int is_bad(u32 pb, const u32 *bl, u32 n){ u32 i; for(i=0;i<n;i++){ if(bl[i]==pb) return 1; } return 0; }
__attribute__((noinline)) static int write_block_verify(volatile u32 *dbg, u32 pb, const u8 *src, u32 *tries_out){
    u32 t, k, i;
    int er, pr;
    u8 rb[2048] __attribute__((aligned(4)));
    u32 page = pb*64u;
    for(t=0;t<MAXTRIES;t++){
        if(t>0){
            if(nand_reset()!=0) continue;
            if(set_features()!=0) continue;
        }
        dbg[G_ERASING] = 1u;
        er = block_erase(page);
        if(er) continue;
        pr = 0;
        for(k=0;k<64u;k++){ pr = program_page(page+k, (u8*)(src + k*PAGE_SZ)); if(pr) break; }
        if(pr) continue;
        pr = 0;
        for(k=0;k<64u && !pr;k++){
            if(read_page_ecc(page+k, rb)<0){ pr = 1; break; }
            for(i=0;i<PAGE_SZ;i++){ if(rb[i] != src[k*PAGE_SZ+i]){ pr = 1; break; } }
        }
        if(!pr){ *tries_out = t+1u; return 0; }
    }
    *tries_out = MAXTRIES;
    return -1;
}
#endif

/* (the write-path functions are noinline so build_nand.sh can prove by symbol that the production build has them and
 * the PROBE_ONLY/GATE_ONLY builds do not) */
/* Build descriptor: lets the installer identify this binary's protocol without trusting code offsets. */
__attribute__((used, section(".rodata.desc"))) static const u32 DESCRIPTOR[8] = {
    0x44574744u /* "DGWD" */, 6u /* my_write6 */, VARIANT, BLOB_SCHEMA,
#ifdef NO_WRITE_CODE
    0u, 0u,
#else
    START_BLOCK, NLOGBLOCKS,
#endif
    BLOB, DBG };

__attribute__((section(".text.entry"))) void my_write(void){
    volatile u32 *dbg=DBGP;
    u32 i;
    for(i=0; i<256u; i++) dbg[i]=0;
    dbg[0]=0x4006E006u;                                            /* magic: my_write6 ran */
    dbg[G_VARIANT]=VARIANT; dbg[G_SCHEMA]=BLOB_SCHEMA; dbg[G_ECC_BAD]=0xFFFFFFFFu;
#ifdef NO_WRITE_CODE
    dbg[G_TLAST] = cp0_count(); dbg[G_CPCCR] = CLKREG(0x00u); dbg[G_CPAPCR] = CLKREG(0x10u);
#endif
    mux_sfc_pins(); set_clock(); sfc_reset_regs(); cdt_init_read();
#ifdef NO_WRITE_CODE
    {   /* every outcome, then the post-verdict re-read (a plain GET FEATURE, no reset): proves the verdict left the NAND
         * protected even where the gate refused before its own power-on check; the done marker comes last */
        int passed = base_gate(dbg);
        u32 pa = 0, pb = 0;
        snap(dbg, G_T_END, G_T_END_HI);
        if(!get_feature(ADDR_PROTECT, &pa) && !get_feature(ADDR_FEATURE, &pb)){ dbg[G_POST_A0] = pa; dbg[G_POST_B0] = pb; dbg[G_POST_OK] = 1u; }
        dbg[16] = passed ? 0x6A7E0000u : dbg[G_RESULT];               /* GATE_ONLY pass: gate passed, nothing written */
        dbg[9] = 0x55555555u; return;
    }
#else
    if(!base_gate(dbg)){ dbg[16]=dbg[G_RESULT]; dbg[9]=0x55555555u; return; }   /* refused: nothing unlocked/erased */
    {
    u32 lb=0, pb=START_BLOCK, skipped=0, retried=0, maxtries=0, feat=0, tries;
    u32 nbad=0, badlist[MAXBAD], b;
    int rr;
    cdt_init_write();
    dbg[3]=(u32)nand_reset();
    dbg[4]=(u32)set_features();                                    /* unlock + ECC: only now */
    cmd(CMD_GET_FEATURE,1,ADDR_FEATURE,1,0,1,0); rd(&feat,1); dbg[15]=feat;
    { u32 a0 = 0xFFu; get_feature(ADDR_PROTECT, &a0); dbg[23] = a0;
      if(a0 != 0u){ dbg[16]=0xDEAD0005u; dbg[9]=0x55555555u; return; } }   /* unlock didn't take */
    dbg[5]=START_BLOCK; dbg[6]=NLOGBLOCKS;
    if(dbg[3]!=0 || dbg[4]!=0 || (feat & 0x10u)==0){
        dbg[16]=0xDEAD0001u;
        dbg[9]=0x55555555u; return;
    }
    cmd(CMD_SET_FEATURE,1,ADDR_FEATURE,1,0,1,1); { u32 z=0; wr(&z); }
    cmd(CMD_GET_FEATURE,1,ADDR_FEATURE,1,0,1,0); rd(&feat,1); dbg[22]=feat;
    for(b=START_BLOCK; b<PART_END; b++){
        int v = oob_verdict(b);
        if(v < 0){ dbg[16]=0xDEAD0006u; dbg[17]=b; dbg[9]=0x55555555u; return; }
        if(v == 1){ if(nbad < MAXBAD){ badlist[nbad]=b; dbg[40u+nbad]=b; } nbad++; }
    }
    dbg[20]=nbad;
    if(nbad > MAXBAD){ dbg[16]=0xDEAD0004u; dbg[9]=0x55555555u; return; }
    rr = set_features();
    cmd(CMD_GET_FEATURE,1,ADDR_FEATURE,1,0,1,0); rd(&feat,1); dbg[21]=feat;
    if(rr!=0 || (feat & 0x10u)==0){ dbg[16]=0xDEAD0005u; dbg[9]=0x55555555u; return; }
    while(lb<NLOGBLOCKS){
        if(is_bad(pb, badlist, nbad)){ skipped++; pb++; continue; }
        if(pb>=PART_END){ dbg[16]=0xDEAD0002u; dbg[17]=pb; dbg[18]=lb; dbg[9]=0x55555555u; return; }
        tries=0;
        rr = write_block_verify(dbg, pb, SRC_AT((u32)lb*64u*PAGE_SZ), &tries);
        if(rr){ dbg[16]=0xDEAD0003u; dbg[17]=pb; dbg[18]=lb; dbg[19]=tries; dbg[9]=0x55555555u; return; }
        if(tries>1u){ retried++; if(tries>maxtries) maxtries=tries; }
        lb++; pb++;
        dbg[1]=lb;
        dbg[11]=pb;
    }
    dbg[7]=0; dbg[8]=0;
    dbg[10]=skipped;
    dbg[11]=pb;
    dbg[12]=SRC_WORD0;
#ifndef HOST_SIM
    dbg[13]=r32(CPM_BASE+CPM_SSICDR);
#endif
    dbg[16]=0x600DF10Cu;
    dbg[17]=retried;
    dbg[18]=maxtries;
    dbg[9]=0x55555555u;
    }
#endif
}
