/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* my_write.c - SFC SPI-NAND WRITE primitive self-test for GD5F2GM7 (X2000 Disc).
 * Built on the PROVEN my_read.c framework (same SFC 0x7c cmd-index + CDT + clock/pins).
 * v1: SINGLE-BLOCK round-trip self-test on the INACTIVE ro2 slot - STRICTLY contained:
 *   erase 1 block -> read back (expect 0xFF) -> program 4 pages w/ pattern -> read back -> compare.
 * Reports everything in the dbg block @0xa0a00000. No host data needed (pattern generated on-device).
 * Active stock slot (ro4) is never touched; this proves the primitive before any real flash. */
typedef unsigned int u32; typedef unsigned char u8;
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
static void cmd(u32 c,u32 len,u32 addr,u32 aw,u32 dmy,u32 den,int dir){
    (void)dmy;(void)den;
    g_arg0=g_arg1=g_arg2=g_arg3=0;
    if(c==0x9f) g_id=aw?2:1;                        /* FIX: RDID = CDT idx 1/2 (was 2/3) */
    else if(c==0xff) g_id=0;
    else if(c==0x0f){ g_id=4; g_arg2=addr; }
    else if(c==0x1f){ g_id=3; g_arg2=addr; }        /* FIX: SET_FEATURE = CDT idx 3, was 5 (=PAGE_READ!) -> unlock never landed */
    else if(c==0x13){ g_id=5; g_arg1=addr; g_arg2=ADDR_STATUS; }
    else if(c==0x0b){ g_id=7; g_arg0=addr; }
    else if(c==0x06){ g_id=8; }                 /* WRITE_ENABLE: cmd-only */
    else if(c==0x02){ g_id=9; g_arg0=addr; }    /* PROGRAM_LOAD: COL addr, data-out */
    else if(c==0x10){ g_id=10; g_arg1=addr; }   /* PROGRAM_EXECUTE: ROW addr (same path as PAGE_READ row) */
    else if(c==0xd8){ g_id=11; g_arg1=addr; }   /* BLOCK_ERASE: ROW addr */
    else g_id=0x3f;
    g_len=len; send_raw(dir);
}
static int clr_end(void){u32 g=0; while(!(sfc_readl(SFC_SR)&END)){if(++g>GUARD)return -1;} sfc_writel(CLR_END,SFC_SCR); return 0;}
static int rd(u32*d,u32 length){
    u32 done=0,fn,sr,len=(length+3)/4,g=0; int i;
    while(done<len){ if(++g>GUARD)return -1; sr=sfc_readl(SFC_SR);
        if(sr&RECE_REQ){ sfc_writel(CLR_RREQ,SFC_SCR); fn=((len-done)>THRESHOLD)?THRESHOLD:(len-done);
            for(i=0;i<(int)fn;i++){*d++=sfc_readl(SFC_RM_DR);done++;} g=0;} }
    return clr_end();
}
static int wr(u32*d){u32 g=0; while(!(sfc_readl(SFC_SR)&TRAN_REQ)){if(++g>GUARD)return -1;} sfc_writel(CLR_TREQ,SFC_SCR); sfc_writel(*d,SFC_RM_DR); return clr_end();}
static int wr_bulk(u32*s,u32 length){    /* transmit `length` bytes (PROGRAM_LOAD data phase) */
    u32 done=0,fn,sr,len=(length+3)/4,g=0; int i;
    while(done<len){ if(++g>GUARD)return -1; sr=sfc_readl(SFC_SR);
        if(sr&TRAN_REQ){ sfc_writel(CLR_TREQ,SFC_SCR); fn=((len-done)>THRESHOLD)?THRESHOLD:(len-done);
            for(i=0;i<(int)fn;i++){sfc_writel(*s++,SFC_RM_DR);done++;} g=0;} }
    return clr_end();
}
static u32 g_mpll,g_cdr,g_src;
static void set_clock(void){
    u32 reg=CPM_BASE+CPM_SSICDR, v=r32(reg);
    u32 src=(v>>30)&3;
    u32 pcr=r32(CPM_BASE+(src?CPM_CPMPCR:0x10));
    u32 m=((pcr>>20)&0xfff)+1, n=((pcr>>14)&0x3f)+1, od=1u<<((pcr>>11)&7u);
    u32 pll=(24u*m*2u)/n/od, cdr=((pll+49u)/50u-1u)&0xff;
    g_mpll=pll; g_cdr=cdr; g_src=src;
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
#define SFC_CDT 0x0800
#define CDT_XFER(aw,dmy,den,cmd) (((u32)(aw)<<26)|(1u<<24)|((u32)(dmy)<<17)|((u32)(den)<<16)|((cmd)&0xffffu))
#define CDT_LINK(link,addrkind,tm) (((u32)(link)<<31)|((u32)(tm)<<4)|((addrkind)&0xfu))
static void cdt_write(u32 idx,u32 w0,u32 w1,u32 w2,u32 w3){
    u32 b=SFC_BASE+SFC_CDT+idx*16u; w32(b,w0); w32(b+4,w1); w32(b+8,w2); w32(b+12,w3);
}
static void cdt_init(void){
    cdt_write(0, CDT_LINK(0,0,0), CDT_XFER(0,0,0,0xff), 0,0);            /* RESET */
    cdt_write(1, CDT_LINK(0,0,0), CDT_XFER(0,0,1,0x9f), 0,0);            /* RDID aw0 */
    cdt_write(2, CDT_LINK(0,1,0), CDT_XFER(1,0,1,0x9f), 0,0);            /* RDID aw1 */
    cdt_write(3, CDT_LINK(0,2,0), CDT_XFER(1,0,1,0x1f), 0,0);            /* SET_FEATURE */
    cdt_write(4, CDT_LINK(0,2,0), CDT_XFER(1,0,1,0x0f), 0,0);            /* GET_FEATURE */
    cdt_write(5, CDT_LINK(1,1,0), CDT_XFER(3,0,0,0x13), 0,0);            /* PAGE_READ linked->6 */
    cdt_write(6, CDT_LINK(0,2,0), (1u<<25)|CDT_XFER(1,0,1,0x0f), 0,1);   /* hw OIP poll */
    cdt_write(7, CDT_LINK(0,0,0), CDT_XFER(2,8,1,0x0b), 0,0);            /* READ_CACHE */
    cdt_write(8, CDT_LINK(0,0,0), CDT_XFER(0,0,0,0x06), 0,0);            /* WRITE_ENABLE: cmd-only */
    cdt_write(9, CDT_LINK(0,2,0), CDT_XFER(2,0,1,0x02), 0,0);            /* PROGRAM_LOAD: COL(2B), data-out, dir set at run */
    cdt_write(10,CDT_LINK(0,1,0), CDT_XFER(3,0,0,0x10), 0,0);           /* PROGRAM_EXECUTE: ROW(3B) */
    cdt_write(11,CDT_LINK(0,1,0), CDT_XFER(3,0,0,0xd8), 0,0);           /* BLOCK_ERASE: ROW(3B) */
}
static int nand_reset(void){u32 st,g=0;
    cmd(CMD_RESET,0,0,0,0,0,0); if(clr_end())return -1;
    do{if(++g>GUARD)return -2; cmd(CMD_GET_FEATURE,1,ADDR_STATUS,1,0,1,0); if(rd(&st,1))return -3;}while(st&ST_OIP);
    return 0;}
static int set_features(void){u32 x;             /* unlock block-protect + enable on-die ECC */
    cmd(CMD_SET_FEATURE,1,ADDR_PROTECT,1,0,1,1); x=0; if(wr(&x))return -1;          /* PROTECT(0xA0)=0 */
    cmd(CMD_SET_FEATURE,1,ADDR_FEATURE,1,0,1,1); x=(1<<4); return wr(&x);}          /* FEATURE(0xB0): ECC_EN */
static int wait_ready(u32*stout){u32 st,g=0;
    do{if(++g>GUARD)return -1; cmd(CMD_GET_FEATURE,1,ADDR_STATUS,1,0,1,0); if(rd(&st,1))return -2;}while(st&ST_OIP);
    if(stout)*stout=st; return 0;}
static int read_page(u32 page,u32 col,u8*dst,u32 len){
    cmd(CMD_PARD,0,page,3,0,0,0); if(clr_end())return -2;
    cmd(CMD_FRCH,len,col,ADDRLEN,8,1,0); return rd((u32*)dst,len);}
static int block_erase(u32 page){u32 st; int r;
    cmd(CMD_WREN,0,0,0,0,0,0); if(clr_end())return -10;
    cmd(CMD_BERASE,0,page,3,0,0,0); if(clr_end())return -11;
    r=wait_ready(&st); if(r)return -12;
    if(st&ST_EFAIL)return -13;
    return 0;}
static int program_page(u32 page,u8*src){u32 st; int r;
    cmd(CMD_WREN,0,0,0,0,0,0); if(clr_end())return -20;
    cmd(CMD_PLOAD,2048,0,2,0,1,1); if(wr_bulk((u32*)src,2048))return -21;   /* load cache @col0 */
    cmd(CMD_PEXEC,0,page,3,0,0,0); if(clr_end())return -22;                 /* commit row */
    r=wait_ready(&st); if(r)return -23;
    if(st&ST_PFAIL)return -24;
    return 0;}
/* LEAN writer (my_write2): programs NPAGES from host-loaded DRAM @SRC into NAND
 * starting at START_PAGE, erasing the covered blocks first. Reads back page 0 and
 * compares to SRC for one-run feedback. This is the REAL write path (data from DRAM),
 * not a self-generated pattern - host loads SRC, runs this, verifies via dbg + reader. */
/* my_write3 - MULTI-BLOCK writer for the real rootfs flash.
 * Programs NLOGBLOCKS logical 128KB blocks from host-loaded DRAM @SRC into physical
 * blocks starting at START_BLOCK, SKIPPING the factory bad block SKIP_BLOCK (left untouched
 * so its OOB bad-marker survives). This reconstructs the mtdblock_bbt logical->physical mapping. */
#ifndef START_BLOCK
#define START_BLOCK 80u     /* rootfs @0xA00000 / 0x20000 */
#endif
#ifndef NLOGBLOCKS
#define NLOGBLOCKS 768u     /* IMG_SIZE 100663296 B / 128KB = 768 blocks (fits v2.40's 88 MB rootfs;
                              covers v2.09/v2.28 too). MUST match imagebuild IMG_SIZE; the flasher's
                              writer-capacity check (dbg[6]) enforces it. Override via -D for other sizes. */
#endif
#ifndef SKIP_BLOCK
#define SKIP_BLOCK 383u     /* verified factory bad block in the rootfs region (dump ground truth) */
#endif
#define PAGE_SZ 2048u
#define SRC 0xa1000000u     /* host-loaded squashfs (padded to NLOGBLOCKS*128KB), via usbboot --download */
/* my_write5 - PORTABLE robust bad-block-aware writer (my_write4 + an in-writer OOB scan).
 * Unlike my_write4 (which hardcoded SKIP_BLOCK=383, this unit's bad block), my_write5 first
 * SCANS each physical block's OOB bad-marker to learn THIS unit's factory-bad blocks, then
 * skips exactly those - so it maps logical->physical correctly on any device. For every good
 * physical block it does
 * erase -> program 64 pages -> READBACK-VERIFY all 64 pages, retrying the WHOLE block (with a
 * NAND reset + re-unlock between attempts) up to MAXTRIES. This recovers transient SFC/timeout
 * failures like the ones that killed the previous flash on blocks 80/81 (which are NOT OOB
 * bad and were written fine by the V2.09 flash). FAILS CLOSED: if a block cannot be written+
 * verified after MAXTRIES, it stops immediately and reports - it never silently mis-maps. */
#define MAXTRIES 6u
#define PART_END (START_BLOCK + NLOGBLOCKS + 64u)   /* generous physical ceiling (guards runaway) */
#define MAXBAD   64u   /* = PART_END-START_BLOCK-NLOGBLOCKS: max bad blocks the image can absorb */
/* linear membership test over the scanned bad-block list (nbad is tiny in practice) */
static int is_bad(u32 pb, const u32 *bl, u32 n){ u32 i; for(i=0;i<n;i++){ if(bl[i]==pb) return 1; } return 0; }
/* Read a block's OOB bad-marker byte (page0 spare byte0) once, retrying transient SFC read
 * failures with a NAND reset. 0=ok (*mk set), -1=could not read. On this GD5F2GM7 the marker
 * reads RAW regardless of the on-die ECC setting (proven by bbscan), so no ECC mgmt is needed. */
static int oob_read1(u32 pb, u8 *mk){
    u32 t, m;
    for(t=0;t<MAXTRIES;t++){
        if(t>0 && nand_reset()!=0) continue;
        m=0xFFFFFFFFu;
        if(read_page(pb*64u, PAGE_SZ, (u8*)&m, 4u)==0){ *mk=(u8)(m & 0xFFu); return 0; }
    }
    return -1;
}
/* Bad-block verdict from the OOB marker, FAIL-CLOSED: 0=good, 1=bad, -1=undeterminable.
 * EVERY verdict - good AND bad - is confirmed by two independent reads that must agree, so a
 * single successful-but-corrupted read can't produce a false-good (missed bad block) OR a
 * false-bad (skipped good block); either would mis-map against the kernel's BBT. An unreadable
 * or self-inconsistent marker returns -1 and the caller aborts. */
static int oob_verdict(u32 pb){
    u8 a, b;
    if(oob_read1(pb, &a)!=0) return -1;
    if(oob_read1(pb, &b)!=0) return -1;
    if(a!=b) return -1;
    return (a==0xFFu) ? 0 : 1;
}
/* Erase + program 64 pages + verify-readback a single physical block from src. 0=ok, else rc<0. */
static int write_block_verify(u32 pb, const u8 *src, u32 *tries_out){
    u32 t, k, i;
    int er, pr;
    u8 rb[2048] __attribute__((aligned(4)));
    u32 page = pb*64u;
    for(t=0;t<MAXTRIES;t++){
        /* On a retry, fully re-init the NAND/SFC state and REQUIRE it to succeed before
         * touching the block again - never program without confirmed unlock + ECC. */
        if(t>0){
            if(nand_reset()!=0) continue;
            if(set_features()!=0) continue;
        }
        er = block_erase(page);
        if(er) continue;                                           /* erase fail -> retry (re-inits) */
        pr = 0;
        for(k=0;k<64u;k++){ pr = program_page(page+k, (u8*)(src + k*PAGE_SZ)); if(pr) break; }
        if(pr) continue;                                           /* program fail -> retry */
        /* verify every page against the source; a READ failure fails the attempt (stale rb
         * must never be allowed to compare-equal and pass). */
        pr = 0;
        for(k=0;k<64u && !pr;k++){
            if(read_page(page+k, 0, rb, PAGE_SZ)!=0){ pr = 1; break; }   /* read error -> retry */
            for(i=0;i<PAGE_SZ;i++){ if(rb[i] != src[k*PAGE_SZ+i]){ pr = 1; break; } }
        }
        if(!pr){ *tries_out = t+1u; return 0; }                    /* verified good */
    }
    *tries_out = MAXTRIES;
    return -1;                                                     /* persistent failure -> fail closed */
}
__attribute__((section(".text.entry"))) void my_write(void){
    volatile u32 *dbg=(volatile u32*)0xa0a00000u;
    u32 lb=0, pb=START_BLOCK, skipped=0, retried=0, maxtries=0, feat=0, tries;
    u32 nbad=0, badlist[MAXBAD], b;
    int rr;
    for(lb=0; lb<256u; lb++) dbg[lb]=0;                            /* clear the whole 1KB dbg region */
    lb=0;
    dbg[0]=0x4004E005u;                                            /* magic: my_write5 (portable) ran */
    mux_sfc_pins(); set_clock(); sfc_reset_regs(); cdt_init();
    dbg[3]=(u32)nand_reset();
    dbg[4]=(u32)set_features();
    cmd(CMD_GET_FEATURE,1,ADDR_FEATURE,1,0,1,0); rd(&feat,1); dbg[15]=feat;
    dbg[5]=START_BLOCK; dbg[6]=NLOGBLOCKS;
    /* ABORT if the NAND/ECC state didn't come up - never write blind. */
    if(dbg[3]!=0 || dbg[4]!=0 || (feat & 0x10u)==0){
        dbg[16]=0xDEAD0001u;                                      /* init-fail abort code */
        dbg[9]=0x55555555u; return;
    }
    /* ---- PORTABLE SCAN PASS ------------------------------------------------------------
     * Learn THIS unit's factory-bad blocks from their OOB bad-marker (byte0 of page0 spare
     * != 0xFF, read with on-die ECC disabled) - the SAME criterion the kernel's mtdblock_bbt
     * uses to build the logical->physical map. Replaces the hardcoded SKIP_BLOCK so the mapping
     * is correct on ANY device. The identical read is proven by bbscan (it found block 383). */
    /* Try to disable on-die ECC so the spare returns raw; record the readback (informational).
     * On this NAND ECC may stay enabled (readback still 0x10) yet the marker byte still reads
     * RAW - so ECC state is NOT a correctness gate; each marker read is validated per-block. */
    cmd(CMD_SET_FEATURE,1,ADDR_FEATURE,1,0,1,1); { u32 z=0; wr(&z); }
    cmd(CMD_GET_FEATURE,1,ADDR_FEATURE,1,0,1,0); rd(&feat,1); dbg[22]=feat;
    for(b=START_BLOCK; b<PART_END; b++){
        int v = oob_verdict(b);
        if(v < 0){ dbg[16]=0xDEAD0006u; dbg[17]=b;                     /* marker unreadable/ambiguous -> fail closed */
                   dbg[9]=0x55555555u; return; }
        if(v == 1){ if(nbad < MAXBAD){ badlist[nbad]=b; dbg[40u+nbad]=b; }   /* list -> dbg[40..40+MAXBAD) */
                    nbad++; }
    }
    dbg[20]=nbad;
    /* More bad blocks than the image can absorb -> dead NAND or a mis-scan. Fail closed. */
    if(nbad > MAXBAD){ dbg[16]=0xDEAD0004u; dbg[9]=0x55555555u; return; }
    /* Re-enable on-die ECC + re-assert block-unlock for the program/verify pass; REQUIRE it. */
    rr = set_features();
    cmd(CMD_GET_FEATURE,1,ADDR_FEATURE,1,0,1,0); rd(&feat,1); dbg[21]=feat;
    if(rr!=0 || (feat & 0x10u)==0){ dbg[16]=0xDEAD0005u; dbg[9]=0x55555555u; return; }
    /* ------------------------------------------------------------------------------------ */
    while(lb<NLOGBLOCKS){
        if(is_bad(pb, badlist, nbad)){ skipped++; pb++; continue; }    /* scanned factory-bad block */
        if(pb>=PART_END){ dbg[16]=0xDEAD0002u; dbg[17]=pb; dbg[18]=lb;
                          dbg[9]=0x55555555u; return; }            /* out of physical space -> abort */
        tries=0;
        rr = write_block_verify(pb, (const u8*)(SRC + (u32)lb*64u*PAGE_SZ), &tries);
        if(rr){                                                   /* FAIL CLOSED */
            dbg[16]=0xDEAD0003u;                                  /* persistent block-write failure */
            dbg[17]=pb; dbg[18]=lb; dbg[19]=tries;
            dbg[9]=0x55555555u; return;                           /* stop - do NOT continue mis-mapped */
        }
        if(tries>1u){ retried++; if(tries>maxtries) maxtries=tries; }
        lb++; pb++;
        dbg[1]=lb;                                                /* progress */
        dbg[11]=pb;
    }
    dbg[7]=0;                                                     /* 0 = all blocks written+verified */
    dbg[8]=0;
    dbg[10]=skipped;                                              /* bad blocks skipped within the written span (<= dbg[20]=nbad) */
    dbg[11]=pb;                                                   /* last physical block +1 */
    dbg[12]=*(volatile u32*)SRC;                                 /* first source word */
    dbg[13]=r32(CPM_BASE+CPM_SSICDR);
    dbg[16]=0x600DF10Cu;                                         /* SUCCESS sentinel */
    dbg[17]=retried;                                             /* # blocks that needed a retry */
    dbg[18]=maxtries;                                            /* worst-case retry count */
    dbg[9]=0x55555555u;                                          /* DONE - written LAST */
}
