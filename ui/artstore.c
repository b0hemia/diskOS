/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "artstore.h"
#include "art.h"
#include "artcache.h"
#include "sdio.h"
#include "sqlite3.h"
#include <mbedtls/sha256.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#ifndef ARTSTORE_ROOT
#define ARTSTORE_ROOT "/tmp/sdcard"
#endif
#ifndef ARTSTORE_BUDGET
#define ARTSTORE_BUDGET (4ULL<<30)
#endif
#ifndef ARTSTORE_RESERVE
#define ARTSTORE_RESERVE (1ULL<<30)
#endif
#define ARTSTORE_RECIPE ART_RECIPE_VER
/* Leave space for a one-set transaction's journal plus cluster tails. The VFS
 * enforces the total even during recovery or a failed/partially extended write. */
#ifndef ARTSTORE_JOURNAL
#define ARTSTORE_JOURNAL (8ULL<<20)
#endif
#ifndef ARTSTORE_EVENT
#define ARTSTORE_EVENT(step) ((void)0)
#endif
#include "artquota.h"
static pthread_mutex_t store_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pin_mu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t pin_epoch;
static pthread_once_t store_once=PTHREAD_ONCE_INIT;
static sqlite3 *store_db;
/* Pinned tracks are kept as a 64-bit FNV-1a key of the whole path (0 = empty slot): a copy of a path that may be
 * over 1000 bytes would cost 144 KiB, and a cut copy would never match the full database path. A collision only
 * protects one extra picture, which is the safe direction. */
static uint64_t protected_tracks[3][96];
static uint64_t pin_key(const char *s){
    if(!s || !*s) return 0;
    uint64_t h=UINT64_C(14695981039346656037);
    for(; *s; s++){
        h^=(unsigned char)*s;
        h*=UINT64_C(1099511628211);
    }
    return h ? h : 1;
}
static struct { char key[24]; uint64_t tick; } touches[128];
static unsigned touch_next;
static uint64_t touch_clock;
static uint64_t touch_media=UINT64_MAX;
static int sql(const char *s){ return sqlite3_exec(store_db,s,NULL,NULL,NULL)==SQLITE_OK; }
static sqlite3_stmt *stmt(const char *s){
    sqlite3_stmt *q=NULL; sqlite3_prepare_v2(store_db,s,-1,&q,NULL); return q;
}
static void textarg(sqlite3_stmt *q,int i,const char *s){ sqlite3_bind_text(q,i,s,-1,SQLITE_TRANSIENT); }
static int step_done(sqlite3_stmt *q){ int ok=sqlite3_step(q)==SQLITE_DONE; sqlite3_finalize(q); return ok; }
static int safe_dir(int parent,const char *name,int create){
    if(create && mkdirat(parent,name,0755) && errno!=EEXIST) return -1;
    return openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
}
static void store_close(void){
    if(store_db){ sqlite3_close(store_db); store_db=NULL; }
    if(quota_dir>=0){ close(quota_dir); quota_dir=-1; }
    sd_write_end(); sd_io_end(); pthread_mutex_unlock(&store_mu);
}
static int store_open(int create){
    pthread_mutex_lock(&store_mu);
    if(!sd_io_begin()){ pthread_mutex_unlock(&store_mu); return 0; }
    if(!sd_write_begin()){ sd_io_end(); pthread_mutex_unlock(&store_mu); return 0; }
    quota_media=sd_io_generation();
    int root=open(ARTSTORE_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root>=0){
        struct stat existing; struct statvfs space;
        int missing=fstatat(root,".diskos",&existing,AT_SYMLINK_NOFOLLOW)!=0 && errno==ENOENT;
        int room=!missing || (!fstatvfs(root,&space) && space.f_frsize &&
            (uint64_t)space.f_bavail*space.f_frsize>=ARTSTORE_RESERVE+space.f_frsize);
        if(room) quota_dir=safe_dir(root,".diskos",create);
        close(root);
    }
    if(quota_dir<0){ store_close(); return 0; }
    struct statvfs v;
    if(fstatvfs(quota_dir,&v) || !v.f_frsize){ store_close(); return 0; }
    quota_cluster=v.f_frsize;
    const char *names[]={"artcache-v2.sqlite","artcache-v2.sqlite-journal"};
    for(int i=0;i<2;i++){
        struct stat s;
        if(fstatat(quota_dir,names[i],&s,AT_SYMLINK_NOFOLLOW)){
            if(errno!=ENOENT){ store_close(); return 0; }
            quota_sizes[i]=0;
        }else{
            if(!S_ISREG(s.st_mode) || s.st_nlink!=1){ store_close(); return 0; }
            quota_sizes[i]=s.st_size;
        }
    }
    if(!create && !quota_sizes[0]){ store_close(); return 0; }
    int fresh=!quota_sizes[0];
    pthread_once(&store_once,quota_init);
    /* /proc directory handle pins the checked .diskos directory during this lease. */
    char path[96]; snprintf(path,sizeof path,"/proc/self/fd/%d/artcache-v2.sqlite",quota_dir);
    int flags=SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX;
    if(create) flags|=SQLITE_OPEN_CREATE;
    if(sqlite3_open_v2(path,&store_db,flags,"diskos-artquota")!=SQLITE_OK){ store_close(); return 0; }
    sqlite3_busy_timeout(store_db,100);
    sqlite3_limit(store_db,SQLITE_LIMIT_LENGTH,2<<20);
    if(!sql("PRAGMA page_size=4096;"
            "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"
            "PRAGMA cache_size=-512; PRAGMA cache_spill=OFF; PRAGMA mmap_size=0; PRAGMA temp_store=MEMORY;") ||
       /* Setting auto_vacuum writes a header cookie even when unchanged. */
       (fresh && !sql("PRAGMA auto_vacuum=INCREMENTAL;"))){
        store_close(); return 0;
    }
    char limit[96];
    uint64_t allowance=ARTSTORE_JOURNAL+2*quota_cluster;
    if(ARTSTORE_BUDGET<=allowance+65536){ store_close(); return 0; }
    snprintf(limit,sizeof limit,"PRAGMA max_page_count=%llu;",(unsigned long long)((ARTSTORE_BUDGET-allowance)/4096));
    if(!sql(limit)){ store_close(); return 0; }
    if(create){
        if(!sql(
            "CREATE TABLE IF NOT EXISTS picture_index(id INTEGER PRIMARY KEY,sha BLOB NOT NULL,n INTEGER NOT NULL,"
            "recipe INTEGER NOT NULL,used INTEGER NOT NULL DEFAULT 0,UNIQUE(sha,n,recipe));"
            "CREATE TABLE IF NOT EXISTS derivatives(id INTEGER PRIMARY KEY,pic INTEGER NOT NULL,kind INTEGER NOT NULL,"
            "data BLOB NOT NULL,UNIQUE(pic,kind));"
            "CREATE VIEW IF NOT EXISTS pictures AS SELECT p.id,p.sha,p.n,p.recipe,p.used,c.data AS c,t.data AS t,b.data AS b "
            "FROM picture_index p LEFT JOIN derivatives c ON c.pic=p.id AND c.kind=0 "
            "LEFT JOIN derivatives t ON t.pic=p.id AND t.kind=1 LEFT JOIN derivatives b ON b.pic=p.id AND b.kind=2;"
            "CREATE TABLE IF NOT EXISTS sources(key TEXT PRIMARY KEY,track TEXT,revision TEXT,record BLOB,pic INTEGER,suppressed INTEGER DEFAULT 0);"
            "CREATE INDEX IF NOT EXISTS source_pic ON sources(pic);"
            "CREATE INDEX IF NOT EXISTS picture_used ON picture_index(used);"
            "CREATE TABLE IF NOT EXISTS retirement(key TEXT PRIMARY KEY,manifest BLOB NOT NULL,reason INTEGER NOT NULL,pic INTEGER);")){
            store_close(); return 0;
        }
    }
    if(touch_media!=quota_media){
        memset(touches,0,sizeof touches); touch_next=0; touch_clock=0; touch_media=quota_media;
        sqlite3_stmt *q=stmt("SELECT coalesce(max(used),0) FROM picture_index");
        if(q){ if(sqlite3_step(q)==SQLITE_ROW) touch_clock=sqlite3_column_int64(q,0); sqlite3_finalize(q); }
    }
    return 1;
}
uint64_t artstore_pin_key(const char *track){ return pin_key(track); }
void artstore_protect_keys(int group,const uint64_t *keys,int n){
    if(group<0 || group>=3) return;
    pthread_mutex_lock(&pin_mu);
    pin_epoch++;
    memset(protected_tracks[group],0,sizeof protected_tracks[group]);
    for(int i=0;i<n && i<96;i++) protected_tracks[group][i]=keys[i];
    pthread_mutex_unlock(&pin_mu);
}
void artstore_protect(int group,const char *const *tracks,int n){
    if(group<0 || group>=3) return;
    pthread_mutex_lock(&pin_mu);
    pin_epoch++;
    memset(protected_tracks[group],0,sizeof protected_tracks[group]);
    for(int i=0;i<n && i<96;i++) protected_tracks[group][i]=pin_key(tracks[i]);
    pthread_mutex_unlock(&pin_mu);
}
static void touched(const char *key){
    for(unsigned i=0;i<128;i++) if(!strcmp(touches[i].key,key)){ touches[i].tick=++touch_clock; return; }
    unsigned i=touch_next++%128; snprintf(touches[i].key,24,"%s",key); touches[i].tick=++touch_clock;
}
static uint64_t recency(sqlite3_int64 id,uint64_t disk){
    sqlite3_stmt *q=stmt("SELECT key FROM sources WHERE pic=?");
    if(!q) return UINT64_MAX;
    sqlite3_bind_int64(q,1,id);
    while(sqlite3_step(q)==SQLITE_ROW){
        const char *key=(const char*)sqlite3_column_text(q,0);
        for(unsigned i=0;i<128;i++) if(key && !strcmp(key,touches[i].key) && touches[i].tick>disk) disk=touches[i].tick;
    }
    sqlite3_finalize(q); return disk;
}
static int pinned(sqlite3_int64 id){
    sqlite3_stmt *q=stmt("SELECT track FROM sources WHERE pic=?");
    if(!q) return 1;
    int yes=0; sqlite3_bind_int64(q,1,id);
    while(!yes && sqlite3_step(q)==SQLITE_ROW){
        const char *track=(const char*)sqlite3_column_text(q,0);
        uint64_t key=pin_key(track);
        pthread_mutex_lock(&pin_mu);
        for(int g=0;g<3;g++) for(int i=0;i<96;i++)
            if(key && protected_tracks[g][i]==key) yes=1;
        pthread_mutex_unlock(&pin_mu);
    }
    sqlite3_finalize(q); return yes;
}
/* Recency is collected in bounded RAM and flushed only in an idle window. */
static void flush_touches(void){
    if(!sql("BEGIN IMMEDIATE")) return;
    sqlite3_stmt *q=stmt("UPDATE picture_index SET used=max(used,?) WHERE id IN (SELECT pic FROM sources WHERE key=?)");
    int ok=q!=NULL;
    for(unsigned i=0;i<128 && ok;i++) if(touches[i].key[0]){
        sqlite3_bind_int64(q,1,touches[i].tick); textarg(q,2,touches[i].key);
        ok=sqlite3_step(q)==SQLITE_DONE;
        sqlite3_reset(q); sqlite3_clear_bindings(q);
    }
    sqlite3_finalize(q);
    if(ok && quota_current() && sql("COMMIT")) memset(touches,0,sizeof touches);
    else sql("ROLLBACK");
}
static void flush_due(void){
    static time_t last;
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now)) return;
    if(!last){ last=now.tv_sec; return; }
    if(now.tv_sec-last>=30){ flush_touches(); last=now.tv_sec; }
}
static int record_open(const char *key,const char *revision,art_resolution *out,int *complete){
    sqlite3_stmt *q=stmt("SELECT s.revision,s.record,"
        "(SELECT length(data) FROM derivatives WHERE pic=p.id AND kind=0),"
        "(SELECT length(data) FROM derivatives WHERE pic=p.id AND kind=1),"
        "(SELECT length(data) FROM derivatives WHERE pic=p.id AND kind=2) "
        "FROM sources s LEFT JOIN picture_index p ON p.id=s.pic WHERE s.key=?");
    if(!q) return 0;
    textarg(q,1,key); int ok=0;
    if(sqlite3_step(q)==SQLITE_ROW && sqlite3_column_bytes(q,1)==sizeof *out){
        const char *rev=(const char*)sqlite3_column_text(q,0);
        if(rev && (!revision || !strcmp(rev,revision))){
            memcpy(out,sqlite3_column_blob(q,1),sizeof *out); ok=out->state==ART_PRESENT && out->picture.length;
            if(complete) *complete=sqlite3_column_int(q,2)>0 && sqlite3_column_int(q,3)>0 && sqlite3_column_int(q,4)>0;
        }
    }
    sqlite3_finalize(q); return ok;
}
int artstore_record(const char *key,const char *revision,art_resolution *out,int *complete){
    if(complete) *complete=0;
    if(!store_open(0)) return 0;
    int ok=record_open(key,revision,out,complete); store_close(); return ok;
}
static int blob_copy(sqlite3_int64 id,const char *column,int fd,int write_blob,unsigned char digest[32]){
    sqlite3_blob *b=NULL; int ok=0;
    int kind=!strcmp(column,"c")?0:!strcmp(column,"t")?1:2;
    sqlite3_stmt *q=stmt("SELECT id FROM derivatives WHERE pic=? AND kind=?");
    if(!q) return 0;
    sqlite3_bind_int64(q,1,id); sqlite3_bind_int(q,2,kind);
    sqlite3_int64 row=sqlite3_step(q)==SQLITE_ROW?sqlite3_column_int64(q,0):0;
    sqlite3_finalize(q);
    if(!row || sqlite3_blob_open(store_db,"main","derivatives","data",row,write_blob,&b)!=SQLITE_OK) return 0;
    int size=sqlite3_blob_bytes(b); unsigned char buf[16384];
    mbedtls_sha256_context hash; mbedtls_sha256_init(&hash); mbedtls_sha256_starts(&hash,0);
    ok=1;
    for(int off=0;off<size && ok;){
        int n=size-off; if(n>(int)sizeof buf) n=sizeof buf;
        if(write_blob){
            ssize_t got=pread(fd,buf,n,off);
            ok=got==n && sqlite3_blob_write(b,buf,n,off)==SQLITE_OK;
        }else{
            ok=sqlite3_blob_read(b,buf,n,off)==SQLITE_OK;
            if(ok && fd>=0) ok=write(fd,buf,n)==n;
        }
        if(ok) ok=!mbedtls_sha256_update(&hash,buf,n) && quota_current();
        off+=n;
    }
    if(ok) ok=!mbedtls_sha256_finish(&hash,digest);
    mbedtls_sha256_free(&hash);
    if(sqlite3_blob_close(b)!=SQLITE_OK) ok=0;
    return ok;
}
int artstore_read(const char *key,const char *cover,const char *thumb,const char *bg,int demand){
    if(!store_open(0)) return -1;
    sqlite3_stmt *q=stmt("SELECT pic FROM sources WHERE key=?"); sqlite3_int64 id=0;
    if(q){ textarg(q,1,key); if(sqlite3_step(q)==SQLITE_ROW) id=sqlite3_column_int64(q,0); sqlite3_finalize(q); }
    const char *paths[]={cover,thumb,bg}, *cols[]={"c","t","b"}; int ok=id>0;
    for(int i=0;i<3 && ok;i++) if(paths[i]){
        char tmp[640]; int n=snprintf(tmp,sizeof tmp,"%s.store-XXXXXX",paths[i]);
        if(n<0 || n>=(int)sizeof tmp){ ok=0; break; }
        int fd=mkstemp(tmp); unsigned char hash[32];
        ok=fd>=0 && blob_copy(id,cols[i],fd,0,hash);
        if(fd>=0 && close(fd)) ok=0;
        if(ok){
            int valid=art_bmp_check(tmp,i==0?148:i==1?42:360);
            if(valid==0 && quota_current()){
                /* A complete read proved this derivative malformed. Drop only
                 * that row so a fresh decode can refill it; keeping it would
                 * make save_open treat corrupt pixels as an existing hit.
                 * blob_copy has closed its handle before this transaction. */
                sqlite3_stmt *bad=stmt("DELETE FROM derivatives WHERE pic=? AND kind=?");
                if(bad){
                    sqlite3_bind_int64(bad,1,id); sqlite3_bind_int(bad,2,i);
                    (void)step_done(bad);
                }
            }
            ok=valid==1 && quota_current() && !rename(tmp,paths[i]);
        }
        if(!ok) unlink(tmp);
    }
    if(ok && demand) touched(key);
    store_close(); return ok?0:-1;
}
static int evict_one(sqlite3_int64 keep){
    sqlite3_int64 victim=0; int back=0; uint64_t oldest=UINT64_MAX;
    pthread_mutex_lock(&pin_mu); uint64_t pins=pin_epoch; pthread_mutex_unlock(&pin_mu);
    /* Cold backdrops are considered before any small cover/thumb. */
    for(int pass=0;pass<2 && !victim;pass++){
        sqlite3_stmt *q=stmt(pass==0?"SELECT p.id,p.used FROM picture_index p WHERE EXISTS(SELECT 1 FROM derivatives b WHERE b.pic=p.id AND b.kind=2) ORDER BY p.used,p.id":"SELECT id,used FROM picture_index ORDER BY used,id");
        if(!q) return 0;
        while(sqlite3_step(q)==SQLITE_ROW){
            uint64_t disk=sqlite3_column_int64(q,1);
            if(victim && disk>=oldest) break;
            sqlite3_int64 id=sqlite3_column_int64(q,0);
            if(id==keep || pinned(id)) continue;
            uint64_t age=recency(id,sqlite3_column_int64(q,1));
            if(age<oldest){ oldest=age; victim=id; back=pass==0; }
        }
        sqlite3_finalize(q);
    }
    if(!victim) return 0;
    ARTSTORE_EVENT("eviction-selected");
    if(!sql("BEGIN IMMEDIATE")) return 0;
    sqlite3_stmt *q=stmt(back?"UPDATE sources SET suppressed=1 WHERE pic=?":"UPDATE sources SET suppressed=1,pic=NULL WHERE pic=?");
    if(q) sqlite3_bind_int64(q,1,victim);
    int ok=q && step_done(q);
    q=stmt("UPDATE retirement SET reason=2 WHERE pic=?");
    if(q) sqlite3_bind_int64(q,1,victim);
    ok=q && step_done(q) && ok;
    q=stmt(back?"DELETE FROM derivatives WHERE pic=? AND kind=2":"DELETE FROM derivatives WHERE pic=?");
    if(q) sqlite3_bind_int64(q,1,victim);
    ok=q && step_done(q) && ok;
    if(!back && ok){
        q=stmt("DELETE FROM picture_index WHERE id=?");
        if(q) sqlite3_bind_int64(q,1,victim);
        ok=q && step_done(q);
    }
    /* Pin publication never waits for SD I/O. A changed pin set cancels this
     * transaction before its commit decision; later requests see its outcome
     * through the same store mutex before opening any BLOB. */
    pthread_mutex_lock(&pin_mu); int unchanged=pins==pin_epoch; pthread_mutex_unlock(&pin_mu);
    if(!ok || !unchanged || !sql("COMMIT")){ sql("ROLLBACK"); return 0; }
    return 1;
}
int artstore_suppressed(const char *key,const char *revision){
    if(!store_open(0)) return 0;
    sqlite3_stmt *q=stmt("SELECT suppressed FROM sources WHERE key=? AND revision=?"); int yes=0;
    if(q){ textarg(q,1,key); textarg(q,2,revision); if(sqlite3_step(q)==SQLITE_ROW) yes=sqlite3_column_int(q,0); sqlite3_finalize(q); }
    store_close(); return yes;
}
static sqlite3_int64 find_picture(const art_resolution *r){
    sqlite3_stmt *q=stmt("SELECT id FROM picture_index WHERE sha=? AND n=? AND recipe=?");
    if(!q) return 0;
    sqlite3_bind_blob(q,1,r->picture.sha256,32,SQLITE_TRANSIENT); sqlite3_bind_int64(q,2,r->picture.length);
    sqlite3_bind_int(q,3,ARTSTORE_RECIPE);
    sqlite3_int64 id=sqlite3_step(q)==SQLITE_ROW ? sqlite3_column_int64(q,0) : 0;
    sqlite3_finalize(q); return id;
}
static int save_open(const char *track,const char *key,const art_resolution *r,int files[3],int demand,sqlite3_int64 *saved){
    const char *cols[]={"c","t","b"}; struct stat sizes[3];
    for(int i=0;i<3;i++) if(fstat(files[i],&sizes[i]) || !S_ISREG(sizes[i].st_mode) || sizes[i].st_size>500000) return 0;
    sqlite3_int64 id=find_picture(r);
    for(int attempt=0;attempt<256;attempt++){
        if(!sql("BEGIN IMMEDIATE")) return 0;
        int ok=1, missing[3]={1,1,1};
        if(id){
            sqlite3_stmt *q=stmt("SELECT c IS NULL,t IS NULL,b IS NULL FROM pictures WHERE id=?");
            if(q){ sqlite3_bind_int64(q,1,id); if(sqlite3_step(q)==SQLITE_ROW) for(int i=0;i<3;i++) missing[i]=sqlite3_column_int(q,i); sqlite3_finalize(q); }
        }
        if(!id){
            sqlite3_stmt *q=stmt("INSERT INTO picture_index(sha,n,recipe,used) VALUES(?,?,?,?)");
            if(q){
                sqlite3_bind_blob(q,1,r->picture.sha256,32,SQLITE_TRANSIENT); sqlite3_bind_int64(q,2,r->picture.length);
                sqlite3_bind_int(q,3,ARTSTORE_RECIPE); sqlite3_bind_int64(q,4,demand?++touch_clock:0);
                ok=step_done(q); id=sqlite3_last_insert_rowid(store_db);
            }else ok=0;
        }else{
            sqlite3_stmt *q=stmt("UPDATE picture_index SET used=max(used,?) WHERE id=?");
            if(q){ sqlite3_bind_int64(q,1,demand?++touch_clock:0); sqlite3_bind_int64(q,2,id); ok=step_done(q); }else ok=0;
        }
        for(int i=0;i<3 && ok;i++) if(missing[i]){
            sqlite3_stmt *q=stmt("INSERT INTO derivatives(pic,kind,data) VALUES(?,?,zeroblob(?))");
            if(q){ sqlite3_bind_int64(q,1,id); sqlite3_bind_int(q,2,i); sqlite3_bind_int(q,3,sizes[i].st_size); ok=step_done(q); }else ok=0;
        }
        unsigned char hash[32];
        for(int i=0;i<3 && ok;i++) if(missing[i]) ok=blob_copy(id,cols[i],files[i],1,hash);
        if(ok){
            sqlite3_stmt *q=stmt("INSERT OR REPLACE INTO sources(key,track,revision,record,pic,suppressed) VALUES(?,?,?,?,?,0)");
            if(q){
                textarg(q,1,key); textarg(q,2,track); textarg(q,3,r->revision);
                sqlite3_bind_blob(q,4,r,sizeof *r,SQLITE_TRANSIENT); sqlite3_bind_int64(q,5,id); ok=step_done(q);
            }else ok=0;
        }
        ARTSTORE_EVENT("import-written");
        if(ok && quota_current() && sql("COMMIT")){ *saved=id; return 1; }
        int error=sqlite3_errcode(store_db); sql("ROLLBACK");
        id=find_picture(r);
        if(error!=SQLITE_FULL || !quota_current() || !evict_one(id)) return 0;
    }
    return 0;
}
static int valid_fd(int fd,int n){
    char path[64]; snprintf(path,sizeof path,"/proc/self/fd/%d",fd); return fd>=0 && art_bmp_valid(path,n);
}
int artstore_link(const char *track,const char *key,const art_resolution *r,int background){
    if(!store_open(0)) return 0;
    sqlite3_int64 id=find_picture(r); int ok=0;
    sqlite3_stmt *q=stmt("SELECT 1 FROM sources WHERE key=? AND revision=? AND suppressed=1");
    int blocked=0;
    if(q){ textarg(q,1,key); textarg(q,2,r->revision); blocked=sqlite3_step(q)==SQLITE_ROW; sqlite3_finalize(q); }
    /* Linking retained pixels does not restore an evicted derivative. Only a
     * completed save may clear its background-preparation suppression. */
    if(id && !(background && blocked)){
        q=stmt("SELECT 1 FROM sources WHERE key=? AND track=? AND revision=? AND pic=?");
        if(q){
            textarg(q,1,key); textarg(q,2,track); textarg(q,3,r->revision);
            sqlite3_bind_int64(q,4,id); ok=sqlite3_step(q)==SQLITE_ROW; sqlite3_finalize(q);
        }
    }
    if(!ok && id && !(background && blocked) && sql("BEGIN IMMEDIATE")){
        q=stmt("INSERT OR REPLACE INTO sources(key,track,revision,record,pic,suppressed) VALUES(?,?,?,?,?,?)");
        if(q){
            textarg(q,1,key); textarg(q,2,track); textarg(q,3,r->revision); sqlite3_bind_blob(q,4,r,sizeof *r,SQLITE_TRANSIENT); sqlite3_bind_int64(q,5,id); sqlite3_bind_int(q,6,blocked);
            ok=step_done(q) && quota_current() && sql("COMMIT");
        }
        if(!ok) sql("ROLLBACK");
    }
    if(ok && !background){ touched(key); flush_due(); }
    store_close(); return ok;
}
int artstore_save(const char *track,const char *key,const art_resolution *r,const char *cover,const char *thumb,const char *bg,int background){
    if(r->state!=ART_PRESENT || !r->picture.length || !store_open(1)) return 0;
    int ok=0, files[3]={-1,-1,-1}; const char *paths[]={cover,thumb,bg};
    if(background){
        sqlite3_stmt *q=stmt("SELECT suppressed FROM sources WHERE key=? AND revision=?");
        if(q){ textarg(q,1,key); textarg(q,2,r->revision); int blocked=sqlite3_step(q)==SQLITE_ROW && sqlite3_column_int(q,0); sqlite3_finalize(q); if(blocked) goto done; }
    }
    for(int i=0;i<3;i++){
        files[i]=open(paths[i],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(files[i]<0 || !valid_fd(files[i],i==0?148:i==1?42:360)) goto done;
    }
    sqlite3_int64 id=0; ok=save_open(track,key,r,files,!background,&id);
    if(ok && !background){ touched(key); flush_due(); }
 done:
    for(int i=0;i<3;i++) if(files[i]>=0) close(files[i]);
    store_close(); return ok;
}
#include "artmigrate.h"

/* Idle reclamation needs no library snapshot because it retires no legacy files. */
void artstore_idle(void){
    static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
    static time_t last; static uint64_t media=UINT64_MAX;
    struct timespec now; if(clock_gettime(CLOCK_MONOTONIC,&now)) return;
    uint64_t gen=sd_io_generation();
    pthread_mutex_lock(&gate);
    int due=gen!=media || now.tv_sec-last>=30;
    if(due){ media=gen; last=now.tv_sec; }
    pthread_mutex_unlock(&gate);
    if(due && store_open(0)){ flush_touches(); sql("PRAGMA incremental_vacuum(32)"); store_close(); }
}
