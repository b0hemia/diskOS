/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "sdio.h"
#include "artcache.h"
#include "art.h"
#include "artstore.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>

static pthread_mutex_t cache_write_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_cover_only;

/* cache lives on the (large) SD card so it persists across reboots */
#ifndef ARTCACHE_MEDIA_ROOT
#define ARTCACHE_MEDIA_ROOT "/tmp/sdcard"
#endif
#define CACHE_ROOT ARTCACHE_MEDIA_ROOT "/.diskos/artcache"
#define MIN_FREE_BYTES (1024ULL*1024*1024)   /* stop caching if < 1GB free */
/* Bump when the art RECIPE changes (cover/thumb/backdrop generation in art.c) so old
 * cached files auto-invalidate: it's mixed into the fingerprint -> all keys change ->
 * miss -> re-decode. v2 = axis-wash backdrop (was gblur). v3 = 2x2 smooth colour gradient.
 * v4 = blurred cover art (gblur sigma 28). v5 = sharper blur (sigma 14). v6 = sigma 19. */
/* Recipe 7: diskos-artdec orientation, alpha and blur; shared salt in art_identity.h. */

/* fingerprint = FNV-1a(path + size + mtime) -> 16 hex chars. Changes if the file
 * is replaced/edited, so a stale cache entry simply misses (and is orphaned).
 * NO inode: exfat/vfat hand out inode numbers from a counter when a file is looked up, so the same file gets
 * a new number after every mount or icache eviction - a key built on it misses the whole cache every boot. */
static int fingerprint(const char *track, char *out, int cap){
    struct stat stt;
    if(!track || stat(track, &stt) != 0) return -1;
    uint64_t h = 1469598103934665603ULL;
    h ^= (unsigned)ART_RECIPE_VER; h *= 1099511628211ULL;
    /* recipe salt -> bump invalidates old cache */
    for(const char *p=track; *p; p++){ h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    uint64_t sz=(uint64_t)stt.st_size, mt=(uint64_t)stt.st_mtime;
    for(int i=0;i<8;i++){ h ^= (sz>>(i*8))&0xff; h *= 1099511628211ULL; }
    for(int i=0;i<8;i++){ h ^= (mt>>(i*8))&0xff; h *= 1099511628211ULL; }
    art_sidecar_signature(track, &h);                       /* sidecar covers replaced/added/removed -> new key */
    snprintf(out, cap, "%016llx", (unsigned long long)h);
    return 0;
}
static int source_revision(const char *track, const char *cache_key, char *out, int cap){
    uint64_t h = 1469598103934665603ULL ^ ART_ID_VERSION;
    for(const char *p=cache_key;*p;p++){ h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    if(art_source_signature(track, &h)) return -1;
    snprintf(out, cap, "%016llx", (unsigned long long)h); return 0;
}

/* fsync the directory that CONTAINS `path` so a just-completed rename is durable: on a non-journaled
 * FS a power cut can otherwise leave the directory block without the new entry even though the file
 * data hit the disk. Best-effort - a driver that doesn't implement directory fsync just no-ops. */
static void fsync_parent_dir(const char *path){
    char d[600]; snprintf(d, sizeof d, "%s", path);
    char *slash = strrchr(d, '/');
    if(!slash) return;
    if(slash == d) slash[1] = 0; else slash[0] = 0;   /* keep a leading "/" as the dir */
    int fd = open(d, O_RDONLY | O_DIRECTORY);
    if(fd < 0) return;
    fsync(fd);                                         /* errors (incl. unsupported) are non-fatal */
    close(fd);
}
/* A cache entry is used only if it has the exact layout a fresh decode must have (see art_bmp_valid). */
#define CV_OK(p) art_bmp_valid(p, 148)
#define TH_OK(p) art_bmp_valid(p, 42)
#define BG_OK(p) art_bmp_valid(p, 360)
static int copy_file(const char *src, const char *dst){
    int infd=open(src,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    FILE *in=infd<0?NULL:fdopen(infd,"rb"); if(!in){ if(infd>=0) close(infd); return -1; }
    /* unique tmp per call: a live decode + a prewarm decode can copy the SAME dst
     * concurrently; a shared "<dst>.tmp" would let them interleave and expose a
     * partial file. Unique tmp + the atomic rename below means dst is only ever
     * replaced by a complete file. */
    static _Atomic unsigned g_tmp_ctr = 0;
    unsigned seq = atomic_fetch_add(&g_tmp_ctr, 1u);
    char tmp[600]; snprintf(tmp,sizeof tmp,"%s.tmp%u",dst,seq);
    FILE *out=fopen(tmp,"wb"); if(!out){ fclose(in); return -1; }
    char buf[16384]; size_t n; int ok=1;
    while((n=fread(buf,1,sizeof buf,in))>0){ if(fwrite(buf,1,n,out)!=n){ ok=0; break; } }
    if(ferror(in)) ok=0;                       /* read error mid-copy -> don't publish a truncated file */
    fclose(in);
    if(fflush(out)!=0) ok=0;
    /* fsync the data onto the card BEFORE the rename publishes it, so a power cut / yanked card can't
     * surface a rename pointing at unflushed content (the exact non-journaled-exFAT corruption window).
     * A driver that doesn't implement fsync (EINVAL/ENOSYS/EOPNOTSUPP) is tolerated. */
    if(ok){
        int fd = fileno(out);
        if(fd >= 0 && fsync(fd) != 0 && errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) ok = 0;
    }
    fclose(out);
    if(!ok){ unlink(tmp); return -1; }
    if(rename(tmp,dst)!=0){ unlink(tmp); return -1; }
    fsync_parent_dir(dst);                     /* make the rename itself durable */
    return 0;
}
static int legacy_dir(const char *fp){
    /* Check each cache-owned component without traversing a symlink. */
    int root=open(ARTCACHE_MEDIA_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0) return -1;
    const char *names[]={".diskos","artcache",fp};
    for(unsigned i=0;i<3;i++){
        int next=openat(root,names[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); close(root); root=next;
        if(root<0) break;
    }
    return root;
}
static int legacy_valid(int dir,const char *name,int n){
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC); if(fd<0) return 0;
    char path[64]; snprintf(path,sizeof path,"/proc/self/fd/%d",fd);
    struct stat st; int ok=!fstat(fd,&st) && S_ISREG(st.st_mode) && art_bmp_valid(path,n);
    close(fd); return ok;
}
static int entry_ok(const char *fp){
    int dir=legacy_dir(fp); if(dir<0) return 0;
    int ok=legacy_valid(dir,"c.bmp",148) && legacy_valid(dir,"t.bmp",42) && legacy_valid(dir,"b.bmp",360);
    close(dir); return ok;
}
static int packed_complete(const char *key){
    art_resolution r; int full=0; artstore_record(key,NULL,&r,&full); return full;
}
static int artcache_copy_fp(const char *fp,const char *cover,const char *thumb,const char *bg,int demand){
    if(artstore_read(fp,cover,thumb,bg,demand)==0) return 0;
    int dir=legacy_dir(fp); if(dir<0) return -1;
    const char *outputs[]={cover,thumb,bg},*names[]={"c.bmp","t.bmp","b.bmp"};
    int ok=1;
    for(unsigned i=0;i<3 && ok;i++) if(outputs[i]){
        char path[96]; snprintf(path,sizeof path,"/proc/self/fd/%d/%s",dir,names[i]);
        ok=legacy_valid(dir,names[i],i==0?148:i==1?42:360) && !copy_file(path,outputs[i]);
    }
    close(dir); if(!ok) return -1;
    return 0;
}
static int track_copy(const char *track,const char *c,const char *t,const char *b,int demand){
    if(!sd_io_begin()) return -1;
    uint64_t media=sd_io_generation();
    char fp[24]; int rc=fingerprint(track,fp,sizeof fp)?-1:artcache_copy_fp(fp,c,t,b,demand);
    if(!sd_io_allowed() || media!=sd_io_generation()) rc=-1;
    sd_io_end(); return rc;
}
/* Monotonic counter bumped whenever a new cover lands in the cache (any decoder: live NP art,
 * per-track prewarm, or the album prewarm). The cover flow watches it to re-bake placeholders. */
static _Atomic unsigned g_ac_gen;
unsigned artcache_gen(void){ return atomic_load(&g_ac_gen); }

int artcache_lookup(const char *track,char *key,int cap){
    if(cap<24 || !sd_io_begin()) return -1;
    int r=fingerprint(track,key,cap)?-1:(packed_complete(key) || entry_ok(key));
    sd_io_end(); return r;
}
int artcache_has(const char *track){ char key[24]; return artcache_lookup(track,key,sizeof key)==1; }
int artcache_get(const char *track,const char *c,const char *t,const char *b){ return track_copy(track,c,t,b,1); }
int artcache_get_thumb(const char *track,const char *out){ return track_copy(track,NULL,out,NULL,1); }
int artcache_get_cover(const char *track,const char *out){ return track_copy(track,out,NULL,NULL,1); }
int artcache_get_cover_background(const char *track,const char *out){ return track_copy(track,out,NULL,NULL,0); }
/* Path consumers borrow one private RAM cover for their thread's lifetime.
 * POSIX thread-specific-data destruction retires it when a reader exits. */
#ifndef ARTCACHE_TMP_ROOT
#define ARTCACHE_TMP_ROOT "/tmp"
#endif
static pthread_key_t cover_path_key;
static pthread_once_t cover_path_once=PTHREAD_ONCE_INIT;
static int cover_path_ready;
static void cover_path_free(void *p){ if(p){ unlink(p); free(p); } }
static void cover_path_init(void){ cover_path_ready=pthread_key_create(&cover_path_key,cover_path_free)==0; }
int artcache_cover_path(const char *track,char *out,int cap){
    pthread_once(&cover_path_once,cover_path_init); if(!cover_path_ready) return -1;
    char *path=pthread_getspecific(cover_path_key);
    if(!path){
        path=strdup(ARTCACHE_TMP_ROOT "/diskos-cover-XXXXXX"); if(!path) return -1;
        int fd=mkstemp(path); if(fd<0){ free(path); return -1; } close(fd);
        if(pthread_setspecific(cover_path_key,path)){ cover_path_free(path); return -1; }
    }
    if((int)strlen(path)>=cap || artcache_get_cover(track,path)) return -1;
    snprintf(out,cap,"%s",path); return 0;
}
int artcache_cover_candidate(const char *track,char *out,int cap){ return artcache_cover_path(track,out,cap); }
int artcache_key(const char *track,char *out,int cap){
    if(cap<24 || !sd_io_begin()) return -1;
    int r=fingerprint(track,out,cap); sd_io_end(); return r;
}
/* Compatibility writers require a verified identity; no unbounded legacy writes. */
void artcache_put_if(const char *track,const char *before,const char *c,const char *t,const char *b){
    char key[24]; art_resolution r;
    if(artcache_key(track,key,sizeof key) || (before && strcmp(before,key)) || !artcache_resolution(track,&r)) return;
    if(artstore_save(track,key,&r,c,t,b,0)) atomic_fetch_add(&g_ac_gen,1);
}
void artcache_put(const char *track,const char *c,const char *t,const char *b){ artcache_put_if(track,NULL,c,t,b); }
void artcache_accent_key(const art_picture_id *picture,char *out,int cap){
    char key[112]; int n=snprintf(key,sizeof key,"p1-%llu-",(unsigned long long)picture->length);
    for(unsigned i=0;i<32;i++) n+=snprintf(key+n,sizeof key-n,"%02x",picture->sha256[i]);
    snprintf(out,cap,"%s",key);
}
void artcache_protect(int group,const char *const *tracks,int n){ artstore_protect(group,tracks,n); }
/* Resolution metadata is separate from legacy BMPs: a legacy entry cannot
 * establish picture equality. Disk metadata only supplies a known candidate to
 * the helper, which hashes the current payload before trusting it. */
#define RES_MAX 128
static struct { char track[512]; art_resolution value; } resolutions[RES_MAX];
static pthread_mutex_t res_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned res_next;
static uint64_t res_epoch = 1;
void artcache_resolution_invalidate(void){
    pthread_mutex_lock(&res_mu); res_epoch++; memset(resolutions, 0, sizeof resolutions); pthread_mutex_unlock(&res_mu);
}
int artcache_resolution(const char *track, art_resolution *out){
    uint64_t media = sd_io_generation();
    int found = 0; memset(out, 0, sizeof *out);
    if(!sd_io_allowed()) return 0;
    pthread_mutex_lock(&res_mu);
    for(unsigned i=0;i<RES_MAX;i++) if(!strcmp(track, resolutions[i].track) &&
        resolutions[i].value.media == media && resolutions[i].value.epoch == res_epoch){
        *out = resolutions[i].value; found = 1; break;
    }
    pthread_mutex_unlock(&res_mu);
    return found;
}
int artcache_resolution_current(const char *track, const art_resolution *expected){
    if(!sd_io_allowed() || expected->media != sd_io_generation()) return 0;
    pthread_mutex_lock(&res_mu);
    int ok = expected->epoch == res_epoch;
    unsigned slot = RES_MAX;
    for(unsigned i=0;ok && i<RES_MAX;i++) if(!strcmp(track, resolutions[i].track)){
        const art_resolution *r = &resolutions[i].value;
        if(r->epoch != res_epoch || r->media != expected->media) continue;
        slot = i;
        ok = !strcmp(r->revision, expected->revision) &&
             (expected->state == ART_FAILED || (r->state == expected->state &&
              (r->state != ART_PRESENT || art_picture_equal(&r->picture, &expected->picture))));
        break;
    }
    if(ok && slot == RES_MAX && expected->state != ART_FAILED){
        slot = res_next++ % RES_MAX;
        snprintf(resolutions[slot].track, sizeof resolutions[slot].track, "%s", track);
        resolutions[slot].value = *expected;
    }
    pthread_mutex_unlock(&res_mu);
    return ok;
}

static int resolution_read(const char *fp, const char *revision, art_resolution *out){
    if(artstore_record(fp,revision,out,NULL)) return 1;
    int dir=legacy_dir(fp); if(dir<0) return 0;
    int fd=openat(dir,"resolution-v1",O_RDONLY|O_NOFOLLOW|O_CLOEXEC); close(dir);
    FILE *f=fd<0?NULL:fdopen(fd,"rb"); if(!f){ if(fd>=0) close(fd); return 0; }
    art_resolution value;
    int ok = fread(&value, 1, sizeof value, f) == sizeof value && fgetc(f) == EOF;
    fclose(f);
    if(!ok || value.state != ART_PRESENT || !value.picture.length || value.picture.length > (8u << 20) || value.candidate >= 16 ||
       !memchr(value.revision, 0, sizeof value.revision) || (revision && strcmp(value.revision, revision)) || !memchr(value.derivative, 0, sizeof value.derivative) || strcmp(value.derivative, fp) ||
       !memchr(value.source, 0, sizeof value.source)) return 0;
    *out = value; return 1;
}

static int source_changed(const char *track, const art_resolution *proof, uint64_t media){
    /* Even a failed local decode can have a stable source revision, retained
     * when online artwork supplies the pixels. This is not a picture proof. */
    if(!track[0] || proof->media != media || !proof->revision[0]) return 0;
    char key[24], revision[24];
    return fingerprint(track, key, sizeof key) ||
           source_revision(track, key, revision, sizeof revision) ||
           strcmp(revision, proof->revision);
}
int artcache_revalidate(const char *track, const art_resolution *displayed){
    uint64_t media = sd_io_generation();
    if(!sd_io_begin()) return 0; /* Revocation already advances the generation. */
    art_resolution proof = *displayed, cached;
    /* A newer table proof must not conceal a change to the pixels still shown.
     * Only consult it when the current display has no local proof (e.g. online). */
    if(track[0] && !proof.revision[0] && artcache_resolution(track, &cached)) proof = cached;
    int changed = source_changed(track, &proof, media);
    for(unsigned i=0; !changed && i<RES_MAX; i++){
        char path[512]; art_resolution proof;
        pthread_mutex_lock(&res_mu);
        snprintf(path, sizeof path, "%.511s", resolutions[i].track);
        proof = resolutions[i].value;
        int current = proof.epoch == res_epoch;
        pthread_mutex_unlock(&res_mu);
        if(current) changed = source_changed(path, &proof, media);
    }
    sd_io_end();
    return changed;
}
/* Background preparation only: a complete derivative set whose disk record matches the source's current
 * revision needs no work now. It is a candidate, not a display proof: nothing enters the RAM table, so the
 * playback path still resolves the track. Direct BMP readers do not gain a picture proof. */
int artcache_prepared(const char *track, const char *key){
    if(!sd_io_begin()) return 0;
    char revision[24]; art_resolution disk;
    int ok = !source_revision(track,key,revision,sizeof revision) &&
        (artstore_suppressed(key,revision) || ((packed_complete(key) || entry_ok(key)) && resolution_read(key,revision,&disk)));
    sd_io_end(); return ok;
}
int artcache_background_skip(const char *track,const char *key){
    if(!sd_io_begin()) return 1;
    char rev[24]; int ok=!source_revision(track,key,rev,sizeof rev) && artstore_suppressed(key,rev);
    sd_io_end(); return ok;
}
int artcache_accent_candidate(const char *track,const char *key,char *out,int cap){
    if(!sd_io_begin()) return 0;
    art_resolution r; char rev[24];
    int ok=!source_revision(track,key,rev,sizeof rev) && resolution_read(key,rev,&r);
    if(ok) artcache_accent_key(&r.picture,out,cap);
    sd_io_end(); return ok;
}
int artcache_resolve_if(const char *revision, const char *track, const char *cover, const char *thumb, const char *bg,
                     int cancellable, unsigned gen, const art_picture_id *known, art_resolution *out){
    memset(out, 0, sizeof *out); out->state = ART_FAILED;
    uint64_t media = sd_io_generation();
    pthread_mutex_lock(&res_mu); uint64_t epoch = res_epoch; pthread_mutex_unlock(&res_mu);
    out->media = media; out->epoch = epoch;
    if(!sd_io_begin()) return -1;
    pthread_mutex_lock(&cache_write_mu);
    char before[24], after[24], revision0[24], revision1[24]; art_resolution disk = {0};
    int rc = -1;
    if(revision) snprintf(before, sizeof before, "%s", revision);
    else if(fingerprint(track, before, sizeof before)) goto done;
    if(source_revision(track, before, revision0, sizeof revision0)) goto done;
    /* A displayed picture is always preferred: matching it needs no BMP copy. */
    int retaining = known && known->length;
    char derivative[24] = "";
    const art_picture_id *probe = retaining ? known : NULL;
    if(resolution_read(before, revision0, &disk) && (t_cover_only || packed_complete(before) || entry_ok(before))){
        if(!probe || !art_picture_equal(probe, &disk.picture)){
            probe = &disk.picture; retaining = 0;
        }
        snprintf(derivative, sizeof derivative, "%s", before);
    }
    if(!probe){
        /* Recheck the donor metadata under the writer lock: the stable
         * thumbnail key can be reused after timestamp-preserving edits. */
        for(unsigned k=0;k<RES_MAX;k++){
            art_resolution recent = {0}, proof;
            pthread_mutex_lock(&res_mu);
            unsigned i = (res_next + RES_MAX - 1 - k) % RES_MAX;
            if(resolutions[i].value.state == ART_PRESENT && resolutions[i].value.media == media &&
               resolutions[i].value.epoch == epoch) recent = resolutions[i].value;
            pthread_mutex_unlock(&res_mu);
            if(recent.derivative[0] && resolution_read(recent.derivative, NULL, &proof) &&
               art_picture_equal(&recent.picture, &proof.picture) && (t_cover_only || packed_complete(recent.derivative) || entry_ok(recent.derivative))){
                disk = proof; probe = &disk.picture;
                snprintf(derivative, sizeof derivative, "%s", recent.derivative); break;
            }
        }
    }
    int copied = 0;
    rc = art_resolve(track, cover, thumb, bg, cancellable, gen, probe, out);
    if(rc == ART_ID_REUSED && !retaining){
        if(artcache_copy_fp(derivative,cover,t_cover_only?NULL:thumb,t_cover_only?NULL:bg,cancellable!=2)==0){ rc = 0; copied = 1; }
        else rc = art_resolve(track, cover, thumb, bg, cancellable, gen, NULL, out);
    }
    if(fingerprint(track, after, sizeof after) || strcmp(before, after) ||
       source_revision(track, after, revision1, sizeof revision1) || strcmp(revision0, revision1) ||
       media != sd_io_generation() || !art_request_current(cancellable,gen)){ rc = -2; goto done; }
    pthread_mutex_lock(&res_mu);
    int current = epoch == res_epoch;
    pthread_mutex_unlock(&res_mu);
    if(!current){ rc = -2; goto done; }
    snprintf(out->revision, sizeof out->revision, "%s", revision0);
    out->media = media; out->epoch = epoch;
    if(rc == ART_ID_REUSED){
        if(artstore_link(track,before,out,cancellable==2)) snprintf(derivative,sizeof derivative,"%s",before);
        snprintf(out->derivative,sizeof out->derivative,"%s",derivative);
    }
    if(rc == 0){
        /* A locator is reusable only with matching published derivative proof.
         * An unwritable cache still permits display, but conveys no cache hit. */
        snprintf(out->derivative, sizeof out->derivative, "%s", before);
        int was_stored=copied && !strcmp(derivative,before) && !strcmp(disk.revision,revision0);
        int stored=copied && artstore_link(track,before,out,cancellable==2);
        if(!stored && (!t_cover_only || !copied)) stored=artstore_save(track,before,out,cover,thumb,bg,cancellable==2);
        if(stored && !was_stored) atomic_fetch_add(&g_ac_gen,1);
        if(!stored) snprintf(out->derivative, sizeof out->derivative, "%s", copied ? derivative : "");
    }
    if(rc == 0 || rc == ART_ID_REUSED || out->state == ART_ABSENT){
        pthread_mutex_lock(&res_mu);
        if(epoch == res_epoch){
            unsigned slot = RES_MAX;
            for(unsigned i=0;i<RES_MAX;i++) if(!strcmp(track, resolutions[i].track)){ slot=i; break; }
            if(slot == RES_MAX) slot = res_next++ % RES_MAX;
            snprintf(resolutions[slot].track, sizeof resolutions[slot].track, "%s", track);
            resolutions[slot].value = *out;
        } else rc = -2;
        pthread_mutex_unlock(&res_mu);
    }
done:
    out->media = media; out->epoch = epoch;
    if(rc == -2 || (rc != 0 && rc != ART_ID_REUSED && out->state != ART_ABSENT)) out->state = ART_FAILED;
    pthread_mutex_unlock(&cache_write_mu);
    sd_io_end(); return rc;
}

int artcache_resolve(const char *track, const char *cover, const char *thumb, const char *bg,
                     int cancellable, unsigned gen, const art_picture_id *known, art_resolution *out){
    return artcache_resolve_if(NULL, track, cover, thumb, bg, cancellable, gen, known, out);
}

/* A warm wall request proves identity but streams only the small cover. A cold
 * decode still emits the unchanged complete recipe, ready for persistence. */
int artcache_resolve_cover(const char *track,const char *cover,const char *thumb,const char *bg,
                          int cancellable,unsigned gen,const art_picture_id *known,art_resolution *out){
    int previous=t_cover_only; t_cover_only=1;
    int rc=artcache_resolve(track,cover,thumb,bg,cancellable,gen,known,out);
    t_cover_only=previous; return rc;
}
