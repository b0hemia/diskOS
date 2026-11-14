/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "sdio.h"
#include "musicdb.h"
#include "sqlite3.h"
#include "library.h"
#include <stdint.h>
#define JSMN_HEADER      /* declarations only; the implementation lives in ipc.c */
#include "jsmn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <strings.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#define DB_PATH "/usr/data/fiio/db/song.db"

/* The whole library is held in a dynamically-sized array (no fixed cap): sized to
 * the actual SONG count at load. Reloads publish a complete replacement before
 * the UI rebuilds its row pointers. */
static mdb_song_t *g_songs = NULL;
static char **g_catalog_paths;

static void catalog_paths_free(char **paths, int n){
    if(!paths) return;
    for(int i=0;i<n;i++) free(paths[i]);
    free(paths);
}

static int catalog_paths_differ(char **a, char **b, int n){
    for(int i=0;i<n;i++)
        if(strcmp(a[i], b[i])) return 1;
    return 0;
}
static int g_n = 0;
static int g_cap = 0;
static int g_load_err = 0;   /* mdb_load hit a DB error (BUSY/IOERR/OOM) vs a genuinely empty library */

/* ---- cached group lists (Artists/Albums/Genres) --------------------------
 * These are derived from g_songs by parse+sort+dedup - expensive to recompute
 * on every list open.  Build once, cache, and return a fast memcpy thereafter
 * (so opening Artists/Albums is as instant as Songs).  mdb_load() invalidates. */
static char (*g_cart)[MDB_STR];                      static int  g_cart_n = -1;   /* artists */
static char (*g_calb)[MDB_STR], (*g_calb_ar)[MDB_STR]; static int *g_calb_ct; static int g_calb_n = -1; /* albums */
static char *g_calb_bucket;
static int *g_calb_rep;   /* per album: SONG.ID of its first song in library order (cover prewarm representative) */
static char (*g_cgen)[MDB_STR];                      static int *g_cgen_ct;  static int g_cgen_n = -1;   /* genres */
static int g_artist_mode;      /* 0 = Artists by ARTIST (collaborations split), 1 = by album artist (else artist) */
static void groups_free(void){
    free(g_cart);    g_cart=NULL;    g_cart_n=-1;
    free(g_calb_bucket); g_calb_bucket=NULL;
    free(g_calb);    free(g_calb_ar); free(g_calb_ct); free(g_calb_rep);
    g_calb=NULL; g_calb_ar=NULL; g_calb_ct=NULL; g_calb_rep=NULL; g_calb_n=-1;
    free(g_cgen);    free(g_cgen_ct); g_cgen=NULL; g_cgen_ct=NULL; g_cgen_n=-1;
}

static int mdb_ensure_cap(int need){
    if(need <= g_cap) return 1;
    /* The library loads ONCE with the known SONG count, so size to that exact count plus a small pad
     * (was: round up to the next power of two, which wasted ~515KB at 3282 songs). A later rescan with a
     * larger count just reallocs again. */
    if((size_t)need + 16 > ((size_t)-1) / sizeof *g_songs) return 0;   /* overflow guard */
    int nc = need + 16;
    mdb_song_t *p = realloc(g_songs, (size_t)nc * sizeof *g_songs);
    if(!p) return 0;
    g_songs = p; g_cap = nc; return 1;
}

static void trim(char *s){
    char *p = s; while(*p==' '||*p=='\t') p++;
    if(p!=s) memmove(s, p, strlen(p)+1);
    int n = (int)strlen(s);
    while(n>0 && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n')) s[--n]=0;
}

/* One cached read/write connection, opened lazily. UI-owned: no lock sleeps. Workers open their own handles, so a
 * worker's busy handler or statement mutex cannot stall the display thread. */
static sqlite3 *g_db;
static pthread_mutex_t g_db_mu = PTHREAD_MUTEX_INITIALIZER;
static sqlite3 *db(void){
    pthread_mutex_lock(&g_db_mu);
    if(!g_db){
        sqlite3 *tmp = NULL;
        if(sqlite3_open_v2(DB_PATH, &tmp, SQLITE_OPEN_READWRITE|SQLITE_OPEN_FULLMUTEX, NULL) == SQLITE_OK){
            sqlite3_busy_timeout(tmp, 0);
            int busy=0, rc;
            /* persistent per-song accent cache (computed once from album art, survives reboot).
             * ALTER is idempotent here: harmless error if the column already exists. */
            rc=sqlite3_exec(tmp, "ALTER TABLE SONG ADD COLUMN ACCENT INTEGER DEFAULT 0;", 0, 0, 0);
            busy |= rc==SQLITE_BUSY || rc==SQLITE_LOCKED;
            /* the cover fingerprint (artcache_key) the accent was computed from: a replaced cover -> new
             * fingerprint -> the accent is recomputed instead of staying on the old art's colour */
            rc=sqlite3_exec(tmp, "ALTER TABLE SONG ADD COLUMN ACCENT_KEY TEXT;", 0, 0, 0);
            busy |= rc==SQLITE_BUSY || rc==SQLITE_LOCKED;
            /* diskOS-owned play history (separate table -> no SONG schema change, survives rescans,
             * ignored by the stock player). Drives Most-Played / Recently-Played. */
            rc=sqlite3_exec(tmp, "CREATE TABLE IF NOT EXISTS PLAY_STATS(PATH TEXT PRIMARY KEY, "
                              "PLAYS INTEGER DEFAULT 0, LAST_PLAYED INTEGER DEFAULT 0);", 0, 0, 0);
            busy |= rc==SQLITE_BUSY || rc==SQLITE_LOCKED;
            /* diskOS-owned audiobook progress (separate table, path-keyed like PLAY_STATS, survives
             * rescans + the stock DB rebuild). One bookmark per book; MEMBER_PATH is the file that was
             * playing (a multipart book has several), POSITION_MS the spot within it. */
            rc=sqlite3_exec(tmp, "CREATE TABLE IF NOT EXISTS BOOK_PROGRESS(BOOK_KEY TEXT PRIMARY KEY, "
                              "MEMBER_PATH TEXT NOT NULL, POSITION_MS INTEGER NOT NULL DEFAULT 0, "
                              "UPDATED_AT INTEGER NOT NULL DEFAULT 0, COMPLETED INTEGER NOT NULL DEFAULT 0);", 0, 0, 0);
            busy |= rc==SQLITE_BUSY || rc==SQLITE_LOCKED;
            /* diskOS-owned monotone counter + per-playlist export identity. A playlist's export filename
             * is keyed by a UID from this counter, NOT its pid or creation time: the create-allocator can
             * reuse a pid after a delete (it UPDATEs a new row's id upward without advancing
             * sqlite_sequence), and two creations can share a whole-second ADD_TIME, so neither is a
             * reuse-proof identity. The counter only ever increases and is persisted, so a UID is never
             * reused across playlist lifetimes -> two different playlists never target the same .m3u. */
            rc=sqlite3_exec(tmp, "CREATE TABLE IF NOT EXISTS DISKOS_META(K TEXT PRIMARY KEY, V INTEGER NOT NULL);", 0, 0, 0);
            busy |= rc==SQLITE_BUSY || rc==SQLITE_LOCKED;
            rc=sqlite3_exec(tmp, "CREATE TABLE IF NOT EXISTS DISKOS_PL_EXPORT(PID INTEGER PRIMARY KEY, UID INTEGER NOT NULL);", 0, 0, 0);
            busy |= rc==SQLITE_BUSY || rc==SQLITE_LOCKED;
            if(busy) sqlite3_close(tmp);   /* retry schema setup after the lock clears */
            else g_db = tmp;
        } else if(tmp){ sqlite3_close(tmp); }
    }
    sqlite3 *ret = g_db;
    pthread_mutex_unlock(&g_db_mu);
    return ret;
}
/* A read snapshot is acquired before a UI list replaces its previous rows.
 * A successful probe holds a SHARED lock through rendering's DB queries. Busy
 * leaves the display alone; reopening retries. No worker uses this connection. */
static unsigned g_ui_read_depth;
static int g_ui_read_owns;
int mdb_ui_read_begin(void){
    sqlite3 *d=db();
    if(!d) return 0;
    int owns=sqlite3_get_autocommit(d);
    if(owns && !g_ui_read_depth && !mdb_playlist_sync_registry()) return 0;
    if(sqlite3_exec(d,"SAVEPOINT ui_read;",0,0,0)!=SQLITE_OK) return 0;
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(d,"SELECT ID FROM SONG LIMIT 1;",-1,&st,NULL);
    if(rc==SQLITE_OK) rc=sqlite3_step(st);
    sqlite3_finalize(st);
    if(rc==SQLITE_ROW || rc==SQLITE_DONE){
        if(!g_ui_read_depth) g_ui_read_owns=owns;
        g_ui_read_depth++; return 1;
    }
    sqlite3_exec(d,"ROLLBACK TO ui_read; RELEASE ui_read;",0,0,0);
    return 0;
}
void mdb_ui_read_end(void){
    if(!g_db || !g_ui_read_depth) return;
    g_ui_read_depth--;
    if(sqlite3_exec(g_db,"RELEASE ui_read;",0,0,0)!=SQLITE_OK){
        /* Close a failed snapshot without leaving a transaction open.
         * Registry writes commit before this read-only snapshot starts. */
        if(!g_ui_read_depth && g_ui_read_owns) sqlite3_exec(g_db,"ROLLBACK;",0,0,0);
        else sqlite3_exec(g_db,"ROLLBACK TO ui_read; RELEASE ui_read;",0,0,0);
    }
    if(!g_ui_read_depth) g_ui_read_owns=0;
}
/* Short-lived, worker-owned reader. No schema writes and no UI mutex. */
static sqlite3 *db_reader(void){
    sqlite3 *d=NULL;
    if(sqlite3_open_v2(DB_PATH,&d,SQLITE_OPEN_READONLY|SQLITE_OPEN_NOMUTEX,NULL)!=SQLITE_OK){
        if(d) sqlite3_close(d);
        return NULL;
    }
    sqlite3_busy_timeout(d,500);
    return d;
}
/* Dedicated connection for the worker-thread accent cache (mdb_set_song_accent / mdb_song_accent_fresh) ONLY.
 * Keeping that UPDATE off g_db means a worker's row-change can never be misread by a
 * UI-thread sqlite3_changes() (which follows the UI thread's own step()), and a worker
 * write can never join/rollback a g_db transaction. INVARIANT: nothing else may use
 * g_db_w, and no code here may call sqlite3_changes() on it (would reintroduce a race
 * between the two worker threads). A 500 ms busy timeout: a stuck UI txn must
 * not wedge art decode for seconds - the accent write is best-effort, retried next decode. */
static sqlite3 *g_db_w;
static pthread_mutex_t g_db_w_mu = PTHREAD_MUTEX_INITIALIZER;
static sqlite3 *db_w(void){
    pthread_mutex_lock(&g_db_w_mu);
    if(!g_db_w){
        sqlite3 *tmp = NULL;
        if(sqlite3_open_v2(DB_PATH, &tmp, SQLITE_OPEN_READWRITE|SQLITE_OPEN_FULLMUTEX, NULL) == SQLITE_OK){
            sqlite3_busy_timeout(tmp, 500);
            g_db_w = tmp;
        } else if(tmp){ sqlite3_close(tmp); }
    }
    sqlite3 *ret = g_db_w;
    pthread_mutex_unlock(&g_db_w_mu);
    return ret;
}
/* per-song accent (0xRRGGBB packed, 0 = not computed yet). Persisted in SONG.ACCENT. */
int mdb_song_accent(const char *path){
    sqlite3 *d = db(); if(!d || !path) return -1;
    sqlite3_stmt *st=NULL; int rgb = -1;
    if(sqlite3_prepare_v2(d, "SELECT ACCENT FROM SONG WHERE PATH=? LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        int rc=sqlite3_step(st);
        if(rc==SQLITE_ROW) rgb=sqlite3_column_int(st,0);
        else if(rc==SQLITE_DONE) rgb=0;
    }
    sqlite3_finalize(st);
    return rgb;
}

void mdb_set_song_accent(const char *path, int rgb, const char *cover_key){
    sqlite3 *d = db_w(); if(!d || !path) return;   /* worker-only write on its private connection */
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "UPDATE SONG SET ACCENT=?, ACCENT_KEY=? WHERE PATH=?;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int(st, 1, rgb);
        if(cover_key && cover_key[0]) sqlite3_bind_text(st, 2, cover_key, -1, SQLITE_STATIC); else sqlite3_bind_null(st, 2);
        sqlite3_bind_text(st, 3, path, -1, SQLITE_STATIC);
        sqlite3_step(st); sqlite3_finalize(st);
    }
}
/* 1 = the stored accent belongs to the cover whose fingerprint is `cover_key` (keep it); 0 = none yet, or it was
 * computed from a different cover (recompute). An accent stored before fingerprints existed (ACCENT_KEY NULL) is
 * adopted for the current cover once - no library-wide recompute - so only LATER cover changes are detected. */
int mdb_song_accent_fresh(const char *path, const char *cover_key){
    sqlite3 *d = db_w(); if(!d || !path || !cover_key || !cover_key[0]) return 0;
    sqlite3_stmt *st; int rgb = 0, fresh = 0, legacy = 0;
    if(sqlite3_prepare_v2(d, "SELECT ACCENT, ACCENT_KEY FROM SONG WHERE PATH=? LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        if(sqlite3_step(st) == SQLITE_ROW){
            rgb = sqlite3_column_int(st, 0);
            const unsigned char *k = sqlite3_column_text(st, 1);
            if(rgb && !k && strncmp(cover_key,"p1-",3)) legacy = 1;
            else if(rgb && k && !strcmp((const char *)k, cover_key)) fresh = 1;
        }
        sqlite3_finalize(st);
    }
    if(legacy && sqlite3_prepare_v2(d, "UPDATE SONG SET ACCENT_KEY=? WHERE PATH=? AND ACCENT_KEY IS NULL;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, cover_key, -1, SQLITE_STATIC); sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
        sqlite3_step(st); sqlite3_finalize(st);
        fresh = 1;
    }
    return fresh;
}
/* Copy a library path into a caller buffer only when ALL of it fits. A path cut short can name a
 * different real file, so an overflow reads as "no such path" (out emptied), never a prefix. */
static int path_fits(char *out, int cap, const char *src){
    if(!src) src = "";
    if(strlen(src) >= (size_t)cap){ out[0] = 0; return 0; }
    memcpy(out, src, strlen(src) + 1); return 1;
}
/* prewarm iterator: next SONG with ID > after_id, ordered by ID (covers ALL rows,
 * not just the in-memory cap). Fills *id and path. Returns 1 if a row was found.
 * Each call owns a read-only connection; never waits on the UI connection mutex. */
int mdb_prewarm_next(int after_id, int *id, char *path, int cap){
    sqlite3 *d = db_reader(); if(!d) return 0;
    sqlite3_stmt *st; int found = 0;
    if(sqlite3_prepare_v2(d, "SELECT ID, PATH FROM SONG WHERE ID > ? ORDER BY ID LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int(st, 1, after_id);
        if(sqlite3_step(st) == SQLITE_ROW){
            if(id) *id = sqlite3_column_int(st, 0);
            const char *p = (const char*)sqlite3_column_text(st, 1);
            if(path && cap>0) path_fits(path, cap, p);   /* too long for the caller: empty, never a prefix */
            found = 1;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(d);
    return found;
}
/* text column, never NULL (so snprintf "%s" is safe) */
static unsigned album_hash(const char *v){ unsigned h = 2166136261u; for(const unsigned char *q = (const unsigned char *)v; *q; q++) h = (h ^ *q) * 16777619u; return h; }   /* FNV-1a of the FULL album value */
static const char *colt(sqlite3_stmt *st, int i){
    const char *t = (const char*)sqlite3_column_text(st, i);
    return t ? t : "";
}
/* 1 if `table` has column `col`. Stock schemas differ by firmware: CUSTOM_PLAYLIST gained IS_M3U/M3U_PATH only in
 * V2.40 (V2.09/V2.28 players create it without them - fixture strings), so a copy that names them must check first. */
static int table_has_col(sqlite3 *d, const char *table, const char *col){
    char q[96]; snprintf(q, sizeof q, "PRAGMA table_info(%s);", table);
    sqlite3_stmt *st; int has = 0;
    if(sqlite3_prepare_v2(d, q, -1, &st, NULL) != SQLITE_OK) return 0;
    while(!has && sqlite3_step(st) == SQLITE_ROW){ const unsigned char *n = sqlite3_column_text(st, 1); has = n && !strcasecmp((const char *)n, col); }
    sqlite3_finalize(st);
    return has;
}
static int playlist_has_m3u_cols(sqlite3 *d){ return table_has_col(d, "CUSTOM_PLAYLIST", "IS_M3U") && table_has_col(d, "CUSTOM_PLAYLIST", "M3U_PATH"); }

/* Sparse group metadata: identities stay independent from their sort strings.
 * kind: artist, album artist, global album, owner/album, scoped SONG source. */
typedef struct { int kind, owner_null; unsigned h; char *name, *owner, *value; } mdb_sort_key;
static mdb_sort_key *g_sort_keys;
static int g_sort_n, g_sort_ready;
static int mdb_code4(const char *s);
static int mdb_code_name_cmp(int ca, const char *a, int cb, const char *b);
static void mdb_sort_free(mdb_sort_key *keys, int n){
    for(int i=0;i<n;i++){ free(keys[i].name); free(keys[i].owner); free(keys[i].value); }
    free(keys);
}
static int mdb_sort_identity(const void *a, const void *b){
    const mdb_sort_key *x=a,*y=b;
    if(x->kind!=y->kind) return x->kind<y->kind ? -1 : 1;
    int c=x->kind==0 ? strcasecmp(x->name,y->name) : strcmp(x->name,y->name);
    if(!c && x->kind==2) c=(x->h>y->h)-(x->h<y->h);
    if(!c && x->kind==3){
        c=(x->owner_null>y->owner_null)-(x->owner_null<y->owner_null);
        if(!c) c=strcmp(x->owner,y->owner);
    }
    return c;
}
static const char *mdb_sort_min(const char *a, const char *b){
    if(!a) return b;
    if(!b) return a;
    return mdb_code_name_cmp(mdb_code4(a),a,mdb_code4(b),b)<=0 ? a : b;
}
static const char *mdb_sort_find(int kind, const char *name, unsigned h, const char *owner, int owner_null){
    mdb_sort_key key={kind,owner_null,h,(char *)name,(char *)owner,NULL};
    const mdb_sort_key *hit=g_sort_n ? bsearch(&key,g_sort_keys,(size_t)g_sort_n,sizeof *g_sort_keys,mdb_sort_identity) : NULL;
    return hit ? hit->value : NULL;
}
static int mdb_sort_add(mdb_sort_key **keys, int *n, int *cap, int kind, const char *name,
                        unsigned h, const char *owner, int owner_null, const char *value){
    if(!value || !*value || !*name) return 1;
    if(*n==*cap){
        if(*cap>INT_MAX/2) return 0;
        int nc=*cap ? *cap*2 : 16;
        if((size_t)nc>SIZE_MAX/sizeof **keys) return 0;
        mdb_sort_key *p=realloc(*keys,(size_t)nc*sizeof *p); if(!p) return 0;
        *keys=p; *cap=nc;
    }
    mdb_sort_key k={kind,owner_null,h,strdup(name),strdup(owner ? owner : ""),strdup(value)};
    if(!k.name || !k.owner || !k.value){ free(k.name);free(k.owner);free(k.value);return 0; }
    (*keys)[(*n)++]=k; return 1;
}
static int mdb_sort_read(sqlite3 *d, mdb_sort_key **keys, int *n, int *ready){
    *keys=NULL; *n=0;
    sqlite3_stmt *st=NULL; int cap=0, rc;
    *ready=libsort_reader(d,&st);
    if(*ready!=1) return *ready==0;
    while((rc=sqlite3_step(st))==SQLITE_ROW){
        char value[3][LIBSORT_LEN], group[MDB_STR], album[MDB_STR], tokens[2][MDB_STR];
        for(int i=0;i<3;i++) libsort_text(value[i],(const unsigned char *)colt(st,i+1),(size_t)sqlite3_column_bytes(st,i+1));
        char song_id[24]; snprintf(song_id,sizeof song_id,"%d",sqlite3_column_int(st,0));
        const char *raw_album=colt(st,6), *owner=colt(st,7), *aa=colt(st,5);
        snprintf(group,sizeof group,"%s",*aa ? aa : colt(st,4));
        snprintf(album,sizeof album,"%s",raw_album); trim(album);
        int single=mdb_split_artists(colt(st,4),tokens,2)==1;
        unsigned h=album_hash(raw_album);
        if((single && !mdb_sort_add(keys,n,&cap,0,tokens[0],0,NULL,0,value[0])) ||
           !mdb_sort_add(keys,n,&cap,1,group,0,NULL,0,value[*aa ? 1 : 0]) ||
           !mdb_sort_add(keys,n,&cap,2,album,h,NULL,0,value[2]) ||
           !mdb_sort_add(keys,n,&cap,3,raw_album,0,owner,sqlite3_column_type(st,7)==SQLITE_NULL,value[2]) ||
           !mdb_sort_add(keys,n,&cap,4,song_id,0,NULL,0,value[2])){
            sqlite3_finalize(st); mdb_sort_free(*keys,*n); *keys=NULL; *n=0; return 0;
        }
    }
    sqlite3_finalize(st);
    if(rc!=SQLITE_DONE){ mdb_sort_free(*keys,*n); *keys=NULL; *n=0; return 0; }
    if(*n>1) qsort(*keys,(size_t)*n,sizeof **keys,mdb_sort_identity);
    int w=0;
    for(int i=0;i<*n;i++){
        mdb_sort_key *k=*keys+i;
        if(w && !mdb_sort_identity(*keys+w-1,k)){
            mdb_sort_key *prev=*keys+w-1;
            if(mdb_sort_min(prev->value,k->value)==k->value){ free(prev->value); prev->value=k->value; k->value=NULL; }
            free(k->name);free(k->owner);free(k->value);
        } else (*keys)[w++]=*k;
    }
    *n=w; return 1;
}
static int mdb_sort_equal(const mdb_sort_key *a, int an, const mdb_sort_key *b, int bn){
    if(an!=bn) return 0;
    for(int i=0;i<an;i++) if(mdb_sort_identity(a+i,b+i) || strcmp(a[i].value,b[i].value)) return 0;
    return 1;
}
static int mdb_sort_order(int ac, const char *a, const char *ak, int bc, const char *b, const char *bk){
    int c=mdb_code_name_cmp(ak ? mdb_code4(ak) : ac,ak ? ak : a,bk ? mdb_code4(bk) : bc,bk ? bk : b);
    if(!c){ c=strcasecmp(a,b); if(!c) c=strcmp(a,b); }
    if(!c) c=(ac>bc)-(ac<bc);
    return c;
}
static char mdb_bucket(const char *s){
    while(*s==' ') s++;
    unsigned char c=(unsigned char)*s; if(c>='a' && c<='z') c-=32;
    return c>='A' && c<='Z' ? (char)c : '#';
}
char mdb_browse_bucket(int kind, const char *name){
    const char *key=NULL;
    if(kind==1) key=mdb_sort_find(g_artist_mode ? 1 : 0,name,0,NULL,0);
    else if(kind==2){
        for(int i=0;i<g_sort_n;i++) if(g_sort_keys[i].kind==2 && !strcmp(g_sort_keys[i].name,name)) key=mdb_sort_min(key,g_sort_keys[i].value);
    }
    return mdb_bucket(key ? key : name);
}
int mdb_sort_rescan_needed(void){ return !g_sort_ready; }

/* The player's Songs order (mq_ui/mq_player V2.57 song-list SQL): plain files by title code (name code when untitled), sheet
 * and image tracks after them, then ID for ties. GROUP_CODE_EXPR = the Artists-view key's code under "Album Artist". */
#define MDB_SONG_ORDER \
    "CASE WHEN IS_CUE=0 AND IS_ISO=0 THEN 1 WHEN IS_CUE=1 OR IS_ISO=1 THEN 2 END," \
    "CASE WHEN (IS_CUE=0 AND IS_ISO=0) THEN CASE WHEN TITLE IS NOT NULL THEN TITLE_CODE ELSE NAME_CODE END END," \
    "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN NAME_CODE END,CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN ID END," \
    "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN TRACK END,ID"
#define MDB_GROUP_CODE_EXPR "CASE WHEN NULLIF(ALBUM_ARTIST,'') IS NOT NULL AND ALBUM_ARTIST_CODE IS NOT NULL THEN ALBUM_ARTIST_CODE ELSE ARTIST_CODE END"

static sqlite3_int64 g_catalog_version = -1;
static int g_catalog_changed;
static sqlite3_int64 mdb_data_version(void){
    sqlite3_stmt *st = NULL;
    sqlite3_int64 v = -1;
    sqlite3 *d = db();
    if(d && sqlite3_prepare_v2(d, "PRAGMA main.data_version;", -1, &st, NULL) == SQLITE_OK
       && sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static int mdb_read(int compare){
    int row = 0;
    if(!compare) g_n = 0;
    g_load_err = 0;
    sqlite3 *d = db(); if(!d){ g_load_err = 1; return 0; }
    if(!compare){
    /* size the array to the real count first (no 1500 cap) */
    sqlite3_stmt *cst;
    int count = 0, count_ok = 0;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM SONG;", -1, &cst, NULL) == SQLITE_OK){
        if(sqlite3_step(cst) == SQLITE_ROW){ count = sqlite3_column_int(cst, 0); count_ok = 1; }
        sqlite3_finalize(cst);
    }
    /* A FAILED count query (BUSY/IOERR/corruption) must NOT look like an empty library - the
     * startup auto-scan keys off mdb_song_count()==0, and a spurious rescan on a transient error
     * is wrong. Only count_ok + count==0 is a genuine empty. */
    if(!count_ok){ g_load_err = 1; return 0; }
    if(count > 0 && !mdb_ensure_cap(count)) g_load_err = 1;   /* OOM can't grow to `count`: the load below
                                                               * fills the old g_cap and stops at rc==ROW,
                                                               * which otherwise reads as a benign race -
                                                               * flag it so a partial library isn't silent */
    if(g_cap == 0){ if(count > 0) g_load_err = 1; return 0; }   /* count>0 but no cap => OOM (error); count==0 => empty */
    g_catalog_paths = calloc((size_t)g_cap, sizeof *g_catalog_paths);
    if(!g_catalog_paths){ g_load_err = 1; return 0; }
    }
    sqlite3_stmt *st;
    /* col 6 = the Artists-view key under "Album Artist": the album artist, else the artist. The player keys its type-2
     * queue with COALESCE(ALBUM_ARTIST,ARTIST) (V2.40 0x44b818 / V2.57 0x450f88), which files an EMPTY-STRING album
     * artist (common in stock-built libraries: every row on the owner's Disc) under '' - that would hide those songs,
     * so an empty album artist counts as none here and mdb_artist_plan plays an exact queue whenever the player's
     * own queue for the key holds different songs. An older schema without ALBUM_ARTIST keys by ARTIST. */
    int has_aa = table_has_col(d, "SONG", "ALBUM_ARTIST");
    /* The Songs list is in the player's own order (its ORDER BY on the stored title/name code, then ID for ties - the
     * firmware's sqlite3 keeps ties in ID order), so a name in another script lands where the stock UI puts it instead
     * of after Z. A database without the code columns falls back to a plain case-insensitive title order. */
    int has_codes = table_has_col(d, "SONG", "TITLE_CODE") && table_has_col(d, "SONG", "NAME_CODE")
                 && table_has_col(d, "SONG", "ARTIST_CODE") && table_has_col(d, "SONG", "ALBUM_CODE")
                 && table_has_col(d, "SONG", "GENRE_CODE") && table_has_col(d, "SONG", "IS_CUE") && table_has_col(d, "SONG", "IS_ISO");
    char codes[400], sql[1400];
    if(has_codes)
        snprintf(codes, sizeof codes, "IFNULL(ARTIST_CODE,0),IFNULL(ALBUM_CODE,0),IFNULL(GENRE_CODE,0),IFNULL(%s,0)",
                 has_aa && table_has_col(d, "SONG", "ALBUM_ARTIST_CODE") ? MDB_GROUP_CODE_EXPR : "ARTIST_CODE");
    else snprintf(codes, sizeof codes, "0,0,0,0");
    snprintf(sql, sizeof sql,
        "SELECT IFNULL(TITLE,IFNULL(NAME,'Untitled')),IFNULL(ARTIST,''),IFNULL(ALBUM,''),"
        "IFNULL(DURATION,0),ID,IFNULL(GENRE,''),%s,IFNULL(TRACK,0),%s,IFNULL(PATH,'') FROM SONG "
        "WHERE lower(PATH) NOT LIKE '%%.m4b' "   /* audiobooks live in the Books view, not the music lists */
        "ORDER BY %s;",
        has_aa ? "IFNULL(COALESCE(NULLIF(ALBUM_ARTIST,''),ARTIST),'')" : "IFNULL(ARTIST,'')",
        codes,
        has_codes ? MDB_SONG_ORDER : "1 COLLATE NOCASE");
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK){ g_load_err = 1; return 0; }
    int rc = SQLITE_DONE;
    while((compare || g_n < g_cap) && (rc = sqlite3_step(st)) == SQLITE_ROW){
        mdb_song_t next;
        mdb_song_t *s = compare ? &next : &g_songs[g_n++];
        memset(s, 0, sizeof *s);
        if(!compare){
            g_catalog_paths[g_n-1] = strdup(colt(st,12));
            if(!g_catalog_paths[g_n-1]){
                g_load_err = 1;
                sqlite3_finalize(st);
                return 0;
            }
        }
        snprintf(s->title,  MDB_STR, "%s", colt(st,0));
        snprintf(s->artist, MDB_STR, "%s", colt(st,1));
        snprintf(s->album,  MDB_STR, "%s", colt(st,2));
        s->album_h = album_hash(colt(st,2));
        s->dur_ms = sqlite3_column_int(st,3);
        s->id     = sqlite3_column_int(st,4);
        snprintf(s->genre,  MDB_STR, "%s", colt(st,5));
        snprintf(s->artist_group, MDB_STR, "%s", colt(st,6));   /* NOT trimmed: compared with the player's key as stored */
        s->track  = sqlite3_column_int(st,7);
        s->code_artist = sqlite3_column_int(st,8); s->code_album = sqlite3_column_int(st,9);
        s->code_genre  = sqlite3_column_int(st,10); s->code_group = sqlite3_column_int(st,11);
        trim(s->title); trim(s->artist); trim(s->album); trim(s->genre);   /* album_h keeps the exact (untrimmed, untruncated) identity */
        if(compare && (row >= g_n ||
           memcmp(&g_songs[row], s, sizeof *s) ||
           strcmp(g_catalog_paths[row], colt(st,12)))){
            sqlite3_finalize(st); return 1;
        }
        row++;
    }
    /* rc==ROW means we stopped only because the buffer filled (more rows than COUNT -> a benign
     * add-between-queries race, not an error). Anything other than ROW/DONE is a mid-query
     * BUSY/IOERR -> a partial load that must NOT read as "empty" to the startup auto-scan. */
    if(rc != SQLITE_ROW && rc != SQLITE_DONE) g_load_err = 1;
    sqlite3_finalize(st);
    return compare ? row != g_n : g_n;
}

/* Publish only a complete load. Keep old row pointers and group caches on error or
 * identical data (notably worker ACCENT writes). Sample before reading so a commit
 * during the load remains detectable at the next requested check. */
int mdb_load(void){
    sqlite3 *d = db();
    if(d) sqlite3_busy_timeout(d, 0); /* reload retries must never wait on a competing writer */
    sqlite3_int64 v = mdb_data_version();
    mdb_sort_key *old_keys=g_sort_keys; int old_kn=g_sort_n, old_ready=g_sort_ready;
    mdb_sort_key *new_keys=NULL; int new_kn=0, new_ready=0;
    int snapshot=d && sqlite3_exec(d,"SAVEPOINT catalogue_sort;",0,0,0)==SQLITE_OK;
    mdb_song_t *old = g_songs;
    char **old_paths = g_catalog_paths;
    int n = g_n, cap = g_cap;
    g_songs = NULL; g_cap = 0;
    g_catalog_paths = NULL;
    if(snapshot){
        mdb_read(0);
        if(!g_load_err && !mdb_sort_read(d,&new_keys,&new_kn,&new_ready)) g_load_err=1;
        if(sqlite3_exec(d,"RELEASE catalogue_sort;",0,0,0)!=SQLITE_OK) g_load_err=1;
        if(g_load_err) sqlite3_exec(d,"ROLLBACK TO catalogue_sort; RELEASE catalogue_sort;",0,0,0);
    } else { g_n=0; g_load_err=1; }
    g_catalog_changed = !g_load_err && (n != g_n || old_ready!=new_ready || !mdb_sort_equal(old_keys,old_kn,new_keys,new_kn) ||
        (n && (memcmp(old, g_songs, (size_t)n * sizeof *old) ||
               catalog_paths_differ(old_paths, g_catalog_paths, n))));
    if(!g_catalog_changed){
        mdb_sort_free(new_keys,new_kn);
        catalog_paths_free(g_catalog_paths, g_n);
        g_catalog_paths = old_paths;
        free(g_songs); g_songs = old; g_n = n; g_cap = cap;
    } else {
        mdb_sort_free(old_keys,old_kn); g_sort_keys=new_keys; g_sort_n=new_kn; g_sort_ready=new_ready;
        groups_free();
        catalog_paths_free(old_paths, n);
        free(old);
    }
    if(!g_load_err) g_catalog_version = v;
    if(d) sqlite3_busy_timeout(d, 0);
    return g_n;
}
int mdb_catalog_changed(void){ return g_catalog_changed; }
static int mdb_check_external(void){
    sqlite3_int64 v = mdb_data_version();
    if(!g_load_err && v >= 0 && v == g_catalog_version) return 0;
    if(v < 0){ g_load_err = 1; return 0; }
    if(g_load_err){ mdb_load(); return g_catalog_changed; }
    sqlite3 *d=db();
    if(sqlite3_exec(d,"SAVEPOINT sort_probe;",0,0,0)!=SQLITE_OK){ g_load_err=1; return 0; }
    int same=!g_load_err && !mdb_read(1);
    mdb_sort_key *keys=NULL; int n=0, ready=0;
    if(same && !g_load_err){
        if(!mdb_sort_read(d,&keys,&n,&ready)) g_load_err=1;
        else same=ready==g_sort_ready && mdb_sort_equal(keys,n,g_sort_keys,g_sort_n);
    }
    mdb_sort_free(keys,n);
    if(sqlite3_exec(d,"RELEASE sort_probe;",0,0,0)!=SQLITE_OK){
        sqlite3_exec(d,"ROLLBACK TO sort_probe; RELEASE sort_probe;",0,0,0); g_load_err=1;
    }
    if(g_load_err) return 0;
    if(same){ g_catalog_version=v; return 0; }
    mdb_load();
    return g_catalog_changed;
}

int mdb_refresh_external(void){
    sqlite3 *d = db();
    if(d) sqlite3_busy_timeout(d, 0);
    int changed = mdb_check_external();
    if(d) sqlite3_busy_timeout(d, 0);
    return changed;
}

int mdb_song_count(void){ return g_n; }
/* Total SONG rows INCLUDING audiobooks (which mdb_load excludes from g_songs). Used for "is the DB
 * empty?" and to size buffers that Favourites/Most-Played/Recent (unfiltered queries) also fill.
 * Returns -1 (NOT 0) if the query fails, so callers never mistake a query error for an empty DB. */
int mdb_total_song_count(void){
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st; int total = -1;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM SONG;", -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    /* Add BOOKS if that table exists: books live in their own table now, but an audiobook-only library
     * still isn't "empty" and must not trigger a rescan every boot. A missing BOOKS table (older DB /
     * feature off) simply fails the prepare and leaves the SONG-only count. */
    if(total >= 0 && sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM BOOKS;", -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_step(st) == SQLITE_ROW) total += sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return total;   /* >=0 on success, -1 on query failure */
}
int mdb_load_failed(void){ return g_load_err; }   /* 1 if the last mdb_load hit a DB error (not just empty) */
const mdb_song_t *mdb_song(int i){ return (i>=0 && i<g_n) ? &g_songs[i] : NULL; }

/* On-demand PATH lookup by song ID (not cached in mdb_song_t to save RAM;
 * taps are rare so a per-tap query is fine). 1 found, 0 absent, -1 retry. */
int mdb_song_path(int id, char *out, int cap){
    if(!out || cap<=0) return 0;
    sqlite3 *d=db(); if(!d) return -1;
    sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(d,"SELECT PATH FROM SONG WHERE ID=? LIMIT 1;",-1,&st,NULL)!=SQLITE_OK) return -1;
    sqlite3_bind_int(st,1,id);
    int rc=sqlite3_step(st), found=-1;
    if(rc==SQLITE_ROW){ path_fits(out,cap,colt(st,0)); found=out[0] ? 1 : 0; }
    else if(rc==SQLITE_DONE){ out[0]=0; found=0; }
    sqlite3_finalize(st);
    return found;
}

/* On-demand SONG.ID: 0 if not indexed, -1 if the read failed. */
int mdb_song_id_by_path(const char *path){
    if(!path || !*path) return 0;
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT ID FROM SONG WHERE PATH=? LIMIT 1;", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    int rc=sqlite3_step(st);
    int id = rc==SQLITE_ROW ? sqlite3_column_int(st,0) : (rc==SQLITE_DONE ? 0 : -1);
    sqlite3_finalize(st);
    return id;
}

/* On-demand ALBUM for a song ID. Returns 1 if a non-empty album was found. */
int mdb_song_album(int id, char *out, int cap){
    if(!out || cap<=0) return 0;
    out[0]=0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT IFNULL(ALBUM,'') FROM SONG WHERE ID=? LIMIT 1;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, id);
    if(sqlite3_step(st) == SQLITE_ROW) snprintf(out, cap, "%s", colt(st,0));
    sqlite3_finalize(st);
    return out[0] ? 1 : 0;
}

/* Canonical ALBUM + ARTIST for a song by its PATH. The player's a2 metadata
 * strings can differ from the DB's stored values (whitespace/encoding/suffix),
 * so a Go-to-Album/Artist drill must match by the DB record, not the metadata
 * string, or it finds nothing. 1 found, 0 absent, -1 retry (outputs preserved). */
int mdb_song_meta_by_path(const char *path, char *album, int acap, char *artist, int arcap){
    if(!path || !path[0]) return 0;
    sqlite3 *d = db_reader(); if(!d) return -1;
    sqlite3_busy_timeout(d,0);   /* UI and queue worker both call this helper */
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT IFNULL(ALBUM,''),IFNULL(ARTIST,'') FROM SONG WHERE PATH=? LIMIT 1;",
                          -1, &st, NULL) != SQLITE_OK){ sqlite3_close(d); return -1; }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    int found = -1;
    int rc=sqlite3_step(st);
    if(rc == SQLITE_ROW){
        if(album  && acap>0)  snprintf(album,  acap,  "%s", colt(st,0));
        if(artist && arcap>0) snprintf(artist, arcap, "%s", colt(st,1));
        found = 1;
    } else if(rc==SQLITE_DONE){
        found=0;
        if(album && acap>0) album[0]=0;
        if(artist && arcap>0) artist[0]=0;
    }
    sqlite3_finalize(st); sqlite3_close(d);
    return found;
}

/* The current/last "memory play" track (MEMORY_PLAY in song.db) + resume info,
 * so the UI can show what's playing on startup before any a2 frame arrives.
 * MEMORY_PLAY.MUSIC_ID maps to SONG.ID.  Returns 1 if a track was found. */
int mdb_current_play(mdb_song_t *out, int *pos_ms, int *is_playing){
    if(out) memset(out, 0, sizeof *out);
    if(pos_ms) *pos_ms = 0;
    if(is_playing) *is_playing = 0;
    sqlite3 *d = db(); if(!d) return 0;
    int mid = 0, pos = 0, play = 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT MUSIC_ID,IFNULL(POSITION,0),IFNULL(IS_PLAYING,0) "
                             "FROM MEMORY_PLAY ORDER BY ID DESC LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_step(st) == SQLITE_ROW){
            mid  = sqlite3_column_int(st,0);
            pos  = sqlite3_column_int(st,1);
            play = sqlite3_column_int(st,2);
        }
        sqlite3_finalize(st);
    }
    if(mid <= 0) return 0;
    if(sqlite3_prepare_v2(d, "SELECT IFNULL(TITLE,IFNULL(NAME,'Untitled')),IFNULL(ARTIST,''),"
                             "IFNULL(ALBUM,''),IFNULL(DURATION,0),ID FROM SONG WHERE ID=? LIMIT 1;",
                             -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, mid);
    int got = 0;
    if(sqlite3_step(st) == SQLITE_ROW){
        if(out){
            snprintf(out->title,  MDB_STR, "%s", colt(st,0));
            snprintf(out->artist, MDB_STR, "%s", colt(st,1));
            snprintf(out->album,  MDB_STR, "%s", colt(st,2));
            out->dur_ms = sqlite3_column_int(st,3);
            out->id     = sqlite3_column_int(st,4);
        }
        if(pos_ms) *pos_ms = pos;
        if(is_playing) *is_playing = play;
        got = 1;
    }
    sqlite3_finalize(st);
    return got;
}

/* The player rebuilds LIST_SONG_0 with these exact ORDER BY clauses before it
 * starts (reverse-engineered from mq_player's INSERT...SELECT SQL).  To make a
 * song tap land on the EXACT track, we compute the song's 1-based rank within
 * the same filtered+ordered set.  list_type: 1=all,2=artist,3=album,10=genre
 * (anything else = unfiltered all-songs order).
 * Returns the 1-based position (>=1), or 1 if it can't be resolved. */
/* The player's "Artist" grouping setting (SYSCONFIG.ARTIST_CLASS_TYPE, set by 0648): 0 = ARTIST, 1 = album artist.
 * Its artist queue (list type 2) is built with ARTIST=? under 0 and COALESCE(ALBUM_ARTIST,ARTIST)=? under 1
 * (V2.40 0x44b818 / V2.57 0x450f88), so a position must be computed the same way. Returns 0/1, or -1 when it can't
 * be read cleanly (missing/locked DB, NULL or unexpected value). Read-only, 200 ms busy bound. */
#ifndef MDB_SYSCONFIG_PATH
#define MDB_SYSCONFIG_PATH "/usr/data/fiio/db/sysconfig.db"
#endif
int mdb_artist_class_type(void){
    sqlite3 *c = NULL; int v = -1;
    if(sqlite3_open_v2(MDB_SYSCONFIG_PATH, &c, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK){
        sqlite3_busy_timeout(c, 200);
        sqlite3_stmt *st;
        if(sqlite3_prepare_v2(c, "SELECT ARTIST_CLASS_TYPE FROM SYSCONFIG WHERE ID=1;", -1, &st, NULL) == SQLITE_OK){
            if(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_INTEGER){
                int x = sqlite3_column_int(st, 0);
                if(x == 0 || x == 1) v = x;
            }
            sqlite3_finalize(st);
        }
    }
    if(c) sqlite3_close(c);
    return v;
}

/* The player's per-list-type ORDER BY and filter column, shared by mdb_play_pos and mdb_list_song_ids so a Cast queue is in
 * exactly the order a position is computed in. MDB_ALBUM_ORDER is the one album-order text (types 3/7/8). */
#define MDB_ALBUM_ORDER "CASE WHEN DISC=0 THEN 1 ELSE 0 END,DISC,CASE WHEN TRACK=0 THEN 1 ELSE 0 END,TRACK," \
                        "CASE WHEN TITLE IS NOT NULL THEN TITLE_CODE ELSE NAME_CODE END"   /* player album order (3/7/8) */
/* The title-code ordering the player uses for non-album lists (kept in step with MDB_FAV_ORDER). */
static const char *const MDB_ORDER_TAIL =
    "CASE WHEN IS_CUE=0 AND IS_ISO=0 THEN 1 WHEN IS_CUE=1 OR IS_ISO=1 THEN 2 END,"
    "CASE WHEN (IS_CUE=0 AND IS_ISO=0) THEN CASE WHEN TITLE IS NOT NULL THEN TITLE_CODE ELSE NAME_CODE END END,"
    "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN NAME_CODE END,"
    "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN ID END,"
    "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN TRACK END";
/* order + filter column are CONSTANTS chosen by list_type (never user text); col NULL = unfiltered. Returns 0 when the
 * player's Artist grouping setting cannot be read (type 2 only). */
static int play_list_clause(int list_type, const char **order, const char **col){
    *order = (list_type==3) ? MDB_ALBUM_ORDER : MDB_ORDER_TAIL;
    *col   = (list_type==3) ? "ALBUM" : (list_type==2) ? "ARTIST" : (list_type==10) ? "GENRE" : NULL;
    if(list_type == 2){   /* the artist queue follows the player's Artist/Album-Artist setting */
        int cls = mdb_artist_class_type();
        if(cls < 0) return 0;                       /* unknown: "not found" -> the caller plays it via all songs */
        if(cls == 1) *col = "COALESCE(ALBUM_ARTIST,ARTIST)";
    }
    return 1;
}

int mdb_play_pos(int id, int list_type, const char *name){
    sqlite3 *d = db(); if(!d) return 0;
    /* the playlist NAME is bound, not interpolated. */
    const char *order, *col;
    if(!play_list_clause(list_type, &order, &col)) return 0;
    char sql[1400];
    /* Exclude .m4b from the row-numbering so the position is BOOK-FREE, matching the music queue once books
     * are migrated out of SONG (the play path gates on that). Without this, a book still sitting in SONG
     * (migration pending) would shift the position and play the wrong track after the play-time migration. */
    if(col)
        snprintf(sql, sizeof sql,
            "SELECT pos FROM (SELECT ID,ROW_NUMBER() OVER (ORDER BY %s) pos FROM SONG WHERE %s=? AND lower(PATH) NOT LIKE '%%.m4b') WHERE ID=?;",
            order, col);
    else
        snprintf(sql, sizeof sql,
            "SELECT pos FROM (SELECT ID,ROW_NUMBER() OVER (ORDER BY %s) pos FROM SONG WHERE lower(PATH) NOT LIKE '%%.m4b') WHERE ID=?;", order);
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    if(col){ sqlite3_bind_text(st,1,name?name:"",-1,SQLITE_STATIC); sqlite3_bind_int(st,2,id); }
    else   { sqlite3_bind_int(st,1,id); }
    int pos = 0;   /* 0 = song not found in this list (caller must check) */
    if(sqlite3_step(st) == SQLITE_ROW){ int v=sqlite3_column_int(st,0); if(v>=1) pos=v; }
    sqlite3_finalize(st);
    return pos;
}

/* ---- Album identity + play plans ------------------------------------------------------------------------------ */
#define MDB_TYPE7_MAX 1000 /* bytes per type-7 key: the player sscanf()s each into a 1024-byte stack buffer (V2.40
                           * 0x42399c/0x423bbc, V2.57 0x429008/0x429228 - static RE); keys are < MDB_STR anyway */
#define MDB_KEY_WHERE "((COALESCE(ALBUM_ARTIST,ARTIST)=?1) OR (?1 IS NULL AND COALESCE(ALBUM_ARTIST,ARTIST) IS NULL)) AND ALBUM=?2"
/* Every keyed album (non-empty ALBUM), ordered by name then owner; counts + representative (its first song in the
 * player's album order) optional. Returns how many were written (cap-limited), -1 on a DB error. */
typedef struct { mdb_album_key_t key; int count, rep, code, rank; const char *sort; } mdb_key_row;
static int mdb_key_row_cmp(const void *a, const void *b){
    const mdb_key_row *x=a,*y=b;
    int c=mdb_sort_order(x->code,x->key.album,x->sort,y->code,y->key.album,y->sort);
    if(!c) c=strcasecmp(x->key.owner,y->key.owner);
    if(!c) c=strcmp(x->key.owner,y->key.owner);
    if(!c) c=(x->key.owner_null<y->key.owner_null)-(x->key.owner_null>y->key.owner_null);
    return c ? c : (x->rank>y->rank)-(x->rank<y->rank);
}
int mdb_album_keys(mdb_album_key_t *keys, int *counts, int *rep_ids, int cap){
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st;
    /* ordered by the stored ALBUM_CODE first (the player's Albums order), then name/owner; a database without the
     * code column falls back to name order */
    static const char *SQL_CODE =
        "SELECT O, ALBUM, N, ID, C FROM (SELECT COALESCE(ALBUM_ARTIST,ARTIST) O, ALBUM, ID, COUNT(*) OVER w N, ALBUM_CODE C,"
        " ROW_NUMBER() OVER (w ORDER BY " MDB_ALBUM_ORDER ") RN FROM SONG"
        " WHERE ALBUM IS NOT NULL AND ALBUM<>'' AND lower(PATH) NOT LIKE '%.m4b'"
        " WINDOW w AS (PARTITION BY COALESCE(ALBUM_ARTIST,ARTIST), ALBUM)) WHERE RN=1"
        " ORDER BY IFNULL(C,0), ALBUM COLLATE NOCASE, ALBUM, O COLLATE NOCASE, O;";
    static const char *SQL_NAME =
        "SELECT O, ALBUM, N, ID, 0 FROM (SELECT COALESCE(ALBUM_ARTIST,ARTIST) O, ALBUM, ID, COUNT(*) OVER w N,"
        /* no code columns here: disc, track, then ID */
        " ROW_NUMBER() OVER (w ORDER BY CASE WHEN DISC=0 THEN 1 ELSE 0 END,DISC,CASE WHEN TRACK=0 THEN 1 ELSE 0 END,TRACK,ID) RN FROM SONG"
        " WHERE ALBUM IS NOT NULL AND ALBUM<>'' AND lower(PATH) NOT LIKE '%.m4b'"
        " WINDOW w AS (PARTITION BY COALESCE(ALBUM_ARTIST,ARTIST), ALBUM)) WHERE RN=1"
        " ORDER BY ALBUM COLLATE NOCASE, ALBUM, O COLLATE NOCASE, O;";
    if(sqlite3_prepare_v2(d, SQL_CODE, -1, &st, NULL) != SQLITE_OK && sqlite3_prepare_v2(d, SQL_NAME, -1, &st, NULL) != SQLITE_OK) return -1;
    int n=0, rc, allocated=0; mdb_key_row *rows=NULL;
    while((rc=sqlite3_step(st))==SQLITE_ROW){
        const char *owner=colt(st,0), *album=colt(st,1);
        if((n<cap || g_sort_n) && (strlen(owner)>=MDB_STR || strlen(album)>=MDB_STR)) continue;
        mdb_album_key_t k; memset(&k,0,sizeof k);
        k.owner_null=sqlite3_column_type(st,0)==SQLITE_NULL;
        snprintf(k.owner,MDB_STR,"%s",owner); snprintf(k.album,MDB_STR,"%s",album);
        if(g_sort_n){
            if(n==allocated){
                if(allocated>INT_MAX/2){ rc=SQLITE_NOMEM; break; }
                int nc=allocated ? allocated*2 : 16;
                mdb_key_row *p=realloc(rows,(size_t)nc*sizeof *p);
                if(!p){ rc=SQLITE_NOMEM; break; }
                rows=p; allocated=nc;
            }
            rows[n]=(mdb_key_row){k,sqlite3_column_int(st,2),sqlite3_column_int(st,3),sqlite3_column_int(st,4),n,
                mdb_sort_find(3,album,0,owner,k.owner_null)};
        } else if(n<cap){
            keys[n]=k;
            if(counts) counts[n]=sqlite3_column_int(st,2);
            if(rep_ids) rep_ids[n]=sqlite3_column_int(st,3);
        }
        n++;
    }
    sqlite3_finalize(st);
    if(rc!=SQLITE_DONE){ free(rows); return -1; }
    if(g_sort_n){
        qsort(rows,(size_t)n,sizeof *rows,mdb_key_row_cmp);
        for(int i=0;i<n && i<cap;i++){
            keys[i]=rows[i].key;
            if(counts) counts[i]=rows[i].count;
            if(rep_ids) rep_ids[i]=rows[i].rep;
        }
    }
    free(rows); return n<cap ? n : cap;
}

/* ids of the rows matched by `where`, in the player's album order. ?1 = p1 (SQL NULL when p1null), ?2 = p2 (unbound
 * when NULL). no_books drops .m4b rows - ONLY for the album's own (visible) list: a stock queue is compared exactly as
 * the player builds it, books included, so a book still in SONG makes it differ and forces the exact custom queue.
 * Returns the full match count (ids written up to cap), -1 on a DB error. */
static int ids_ordered(const char *where, const char *p1, int p1null, const char *p2, int no_books, int *ids, int cap){
    sqlite3 *d = db(); if(!d) return -1;
    char sql[700];
    snprintf(sql, sizeof sql, "SELECT ID FROM SONG WHERE %s%s ORDER BY " MDB_ALBUM_ORDER ";", where,
             no_books ? " AND lower(PATH) NOT LIKE '%.m4b'" : "");
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if(p1null) sqlite3_bind_null(st, 1); else sqlite3_bind_text(st, 1, p1 ? p1 : "", -1, SQLITE_STATIC);
    if(p2) sqlite3_bind_text(st, 2, p2, -1, SQLITE_STATIC);
    int n = 0, rc;
    while((rc = sqlite3_step(st)) == SQLITE_ROW){ if(n < cap && ids) ids[n] = sqlite3_column_int(st, 0); n++; }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? n : -1;
}
int mdb_album_key_song_ids(const mdb_album_key_t *k, int *ids, int cap){
    if(!k || !k->album[0]) return -1;
    return ids_ordered(MDB_KEY_WHERE, k->owner, k->owner_null, k->album, 1, ids, cap);
}
/* The SONG.IDs of one stock list, in the order the player queues them (Cast selection source; read-only, no reserved slot).
 * Types 1 (all), 2 (artist), 3 (album), 10 (genre) use play_list_clause - the clause mdb_play_pos ranks with - and, like it,
 * leave .m4b books out; type 8 (genre + album, name = the player's {"style":"..", "album":".."} payload, parsed with the
 * player's own %[^"] rule) goes through ids_ordered. Returns the full match count (ids written up to cap), -1 on a DB error,
 * an unknown list type or a name that does not fit the type. Ties in the sort key are left to SQLite exactly as the
 * player's own INSERT...SELECT leaves them. */
int mdb_list_song_ids(int list_type, const char *name, int *ids, int cap){
    if(!name) name = "";
    if(list_type == 8){
        char style[MDB_STR], album[MDB_STR];
        if(sscanf(name, "{\"style\":\"%159[^\"]\", \"album\":\"%159[^\"]\"}", style, album) != 2 || !album[0]) return -1;
        return ids_ordered("GENRE=?1 AND ALBUM=?2", style, 0, album, 1, ids, cap);
    }
    if(list_type != 1 && list_type != 2 && list_type != 3 && list_type != 10) return -1;
    sqlite3 *d = db(); if(!d) return -1;
    const char *order, *col;
    if(!play_list_clause(list_type, &order, &col)) return -1;
    char sql[1000];
    snprintf(sql, sizeof sql, "SELECT ID FROM SONG WHERE %s%slower(PATH) NOT LIKE '%%.m4b' ORDER BY %s;",
             col ? col : "", col ? "=? AND " : "", order);
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if(col) sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int n = 0, rc;
    while((rc = sqlite3_step(st)) == SQLITE_ROW){ if(n < cap && ids) ids[n] = sqlite3_column_int(st, 0); n++; }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? n : -1;
}
static int same_seq(const int *a, int na, const int *b, int nb){ return na == nb && (na == 0 || !memcmp(a, b, (size_t)na * sizeof *a)); }
/* The play plan for one album: the stock album queue (type 3, ALBUM=?) when it holds exactly this album's rows in the
 * same order (the usual case: a unique album name); else the stock artist+album queue (type 7) when, under the
 * player's current Artist setting, it holds exactly them; else an exact reserved-slot queue (type 5, built at play
 * time). A stock queue is chosen ONLY when its ordered song list equals the album's, so a position computed here is
 * always a position in the queue the player really builds. */
int mdb_album_plan(const mdb_album_key_t *k, mdb_plan_t *plan){
    if(!k || !plan || !k->album[0]) return 0;
    memset(plan, 0, sizeof *plan);
    int n = mdb_album_key_song_ids(k, NULL, 0);
    if(n <= 0) return 0;
    int *a = malloc((size_t)n * sizeof *a), *b = malloc((size_t)(n + 1) * sizeof *b);
    if(!a || !b){ free(a); free(b); return 0; }
    if(mdb_album_key_song_ids(k, a, n) != n){ free(a); free(b); return 0; }
    int m = ids_ordered("ALBUM=?1", k->album, 0, NULL, 0, b, n + 1);             /* the player's type-3 queue, as built */
    if(same_seq(a, n, b, m)){
        plan->list_type = 3; snprintf(plan->name, sizeof plan->name, "%s", k->album);
    } else {
        int cls = mdb_artist_class_type();
        /* type 7's payload is parsed by the player with sscanf %[^"] into 1024-byte buffers: no quote can be carried
         * and a key must stay inside the buffer (longer -> exact custom queue) */
        int quotable = !k->owner_null && !strchr(k->owner, '"') && !strchr(k->album, '"')
                    && strlen(k->owner) < MDB_TYPE7_MAX && strlen(k->album) < MDB_TYPE7_MAX;
        m = -1;
        if(cls >= 0 && quotable)
            m = ids_ordered(cls == 1 ? "COALESCE(ALBUM_ARTIST,ARTIST)=?1 AND ALBUM=?2" : "ARTIST=?1 AND ALBUM=?2",
                            k->owner, 0, k->album, 0, b, n + 1);
        if(m >= 0 && same_seq(a, n, b, m)){
            plan->list_type = 7;
            int w = snprintf(plan->name, sizeof plan->name, "{\"artist\":\"%s\", \"album\":\"%s\"}", k->owner, k->album);
            if(w < 0 || w >= (int)sizeof plan->name) plan->list_type = 5;       /* never send a truncated key */
        } else plan->list_type = 5;
        if(plan->list_type == 5) plan->name[0] = 0;
    }
    free(b);
    plan->ids = a; plan->count = n;
    return 1;
}
/* Build a type-5 plan's queue in the reserved slot and adopt the order the player will REALLY play it in: the player
 * reads CUSTOM_PLAYLIST WHERE PLAYLIST_ID=? with no ORDER BY, so the order is whatever that query returns (with the
 * stock UNIQUE(PLAYLIST_ID,PATH,TRACK) index: path/track order). The slot is read back with the same query shape and
 * each row mapped back to its song, so plan->ids becomes the real queue order. Fails (returns 0, slot untouched on a
 * write failure) if any song can't be written or read back exactly once - never a position against a different
 * queue. */
int mdb_plan_materialize(mdb_plan_t *plan){
    if(!plan || plan->list_type != 5 || !plan->ids || plan->count <= 0) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    if(sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) != SQLITE_OK) return 0;
    int ok = sqlite3_exec(d, "INSERT OR IGNORE INTO CUSTOM_PLAYLIST_INDEX (LIST_ID,LIST_NAME,M3U_PATH) VALUES ("
                             XSTR(DISKOS_RSV_LISTID) ",'diskos-book','');", 0, 0, 0) == SQLITE_OK
          && sqlite3_exec(d, "DELETE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=" XSTR(DISKOS_RSV_LISTID) ";", 0, 0, 0) == SQLITE_OK;
    sqlite3_stmt *ins = NULL;
    /* DURATION must be non-zero (a 0-length slot row is treated as already finished and skipped at once), and
     * IS_M3U/M3U_PATH are carried over because the player reads them and joins SONG on IS_M3U - where the slot table has
     * them (V2.40+; a V2.09/V2.28 CUSTOM_PLAYLIST has neither, and naming them would fail every album play there). */
    int m3u = ok && playlist_has_m3u_cols(d);
    if(ok && sqlite3_prepare_v2(d, m3u ?
        "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,"
        "ALBUM_ARTIST,IS_M3U,M3U_PATH) "
        "SELECT " XSTR(DISKOS_RSV_LISTID) ",PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,"
        "(CASE WHEN DURATION>0 THEN DURATION ELSE 86400000 END),ALBUM_ARTIST,IFNULL(IS_M3U,0),IFNULL(M3U_PATH,'') "
        "FROM SONG WHERE ID=?1;" :
        "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,"
        "ALBUM_ARTIST) "
        "SELECT " XSTR(DISKOS_RSV_LISTID) ",PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,"
        "(CASE WHEN DURATION>0 THEN DURATION ELSE 86400000 END),ALBUM_ARTIST "
        "FROM SONG WHERE ID=?1;", -1, &ins, NULL) != SQLITE_OK) ok = 0;
    for(int i = 0; ok && i < plan->count; i++){
        sqlite3_reset(ins); sqlite3_bind_int(ins, 1, plan->ids[i]);
        if(sqlite3_step(ins) != SQLITE_DONE || sqlite3_changes(d) != 1) ok = 0;   /* a UNIQUE collision loses a row: fail */
    }
    if(ins) sqlite3_finalize(ins);
    if(!ok || sqlite3_exec(d, "COMMIT;", 0, 0, 0) != SQLITE_OK){ sqlite3_exec(d, "ROLLBACK;", 0, 0, 0); return 0; }
    /* read back in the player's order and map each row to its song (PATH+TRACK+cue/iso identify it) */
    int *order = malloc((size_t)plan->count * sizeof *order);
    if(!order) return 0;
    int *sorted = malloc((size_t)plan->count * sizeof *sorted);   /* sorted ID lookups; NULL -> plain scans */
    if(sorted){
        memcpy(sorted, plan->ids, (size_t)plan->count * sizeof *sorted);
        qsort(sorted, (size_t)plan->count, sizeof *sorted, mdb_id_cmp);
    }
    sqlite3_stmt *rd, *map;
    int n = 0; ok = 1;
    if(sqlite3_prepare_v2(d, "SELECT PATH,TRACK,IS_CUE,IS_ISO FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=" XSTR(DISKOS_RSV_LISTID) ";",
                          -1, &rd, NULL) != SQLITE_OK){ free(sorted); free(order); return 0; }
    if(sqlite3_prepare_v2(d, "SELECT ID FROM SONG WHERE PATH=?1 AND TRACK IS ?2 AND IS_CUE IS ?3 AND IS_ISO IS ?4;", -1, &map, NULL) != SQLITE_OK){
        sqlite3_finalize(rd); free(sorted); free(order); return 0;
    }
    int rrc;
    while(ok && (rrc = sqlite3_step(rd)) == SQLITE_ROW){
        sqlite3_reset(map);
        for(int c = 0; c < 4; c++) sqlite3_bind_value(map, c + 1, sqlite3_column_value(rd, c));
        int id = 0, hits = 0, mrc;
        while((mrc = sqlite3_step(map)) == SQLITE_ROW){
            int cand = sqlite3_column_int(map, 0);
            int hit = 0;
            if(sorted) hit = bsearch(&cand, sorted, (size_t)plan->count, sizeof *sorted, mdb_id_cmp) != NULL;
            else for(int j = 0; j < plan->count && !hit; j++) hit = (plan->ids[j] == cand);
            if(hit){ id = cand; hits++; }
        }
        if(mrc != SQLITE_DONE || hits != 1 || n >= plan->count) ok = 0; else order[n++] = id;   /* a read error = failure */
    }
    if(ok && rrc != SQLITE_DONE) ok = 0;            /* the read-back must END cleanly, not stop on an error */
    sqlite3_finalize(rd); sqlite3_finalize(map);
    if(!ok || n != plan->count){ free(sorted); free(order); return 0; }
    int dup = 0;
    if(sorted){
        memcpy(sorted, order, (size_t)n * sizeof *sorted);
        qsort(sorted, (size_t)n, sizeof *sorted, mdb_id_cmp);
        for(int i = 1; i < n && !dup; i++) dup = (sorted[i-1] == sorted[i]);
    } else for(int i = 0; i < n && !dup; i++) for(int j = i + 1; j < n && !dup; j++) dup = (order[i] == order[j]);
    free(sorted);
    if(dup){ free(order); return 0; }
    free(plan->ids); plan->ids = order;
    return 1;
}
int mdb_plan_pos(const mdb_plan_t *plan, int song_id){
    if(!plan || !plan->ids) return 0;
    for(int i = 0; i < plan->count; i++) if(plan->ids[i] == song_id) return i + 1;
    return 0;
}
void mdb_plan_free(mdb_plan_t *plan){ if(plan){ free(plan->ids); plan->ids = NULL; plan->count = 0; } }

/* CUE / ISO sub-tracks kept under one file PATH (Browse Files): how many rows PATH has with IS_CUE or IS_ISO set
 * (iso_only = count IS_ISO rows only). 0 when none, -1 on a DB error. */
int mdb_subtrack_count(const char *path, int iso_only){
    if(!path) return 0;
    sqlite3 *d = db_reader(); if(!d) return -1;
    sqlite3_busy_timeout(d,0);   /* also called by UI; never sleep */
    sqlite3_stmt *st; int n = -1;
    if(sqlite3_prepare_v2(d, iso_only ? "SELECT COUNT(*) FROM SONG WHERE PATH=?1 AND COALESCE(IS_ISO,0)=1;"
                                      : "SELECT COUNT(*) FROM SONG WHERE PATH=?1 AND (COALESCE(IS_CUE,0)=1 OR COALESCE(IS_ISO,0)=1);", -1, &st, NULL) != SQLITE_OK){ sqlite3_close(d); return -1; }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    if(sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(d);
    return n;
}
/* The exact queue for the CUE/ISO tracks of `paths` (each in TRACK order, files in the order given): a type-5 plan, since
 * no stock list type holds them. 1 built (caller frees), 0 absent, -1 failed (no partial plan). */
int mdb_subtrack_plan(const char *const *paths, int npaths, mdb_plan_t *plan){
    if(!plan || !paths || npaths <= 0) return 0;
    memset(plan, 0, sizeof *plan);
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT ID FROM SONG WHERE PATH=?1 AND (COALESCE(IS_CUE,0)=1 OR COALESCE(IS_ISO,0)=1) ORDER BY TRACK,ID;", -1, &st, NULL) != SQLITE_OK) return -1;
    int cap = 0, n = 0; int *ids = NULL;
    for(int i = 0; i < npaths; i++){
        if(!paths[i]) continue;
        sqlite3_reset(st); sqlite3_bind_text(st, 1, paths[i], -1, SQLITE_STATIC);
        int rc;
        while((rc=sqlite3_step(st)) == SQLITE_ROW){
            if(n == cap){
                int nc = cap ? cap * 2 : 32; int *ni = realloc(ids, (size_t)nc * sizeof *ids);
                if(!ni){ free(ids); sqlite3_finalize(st); return -1; }
                ids = ni; cap = nc;
            }
            ids[n++] = sqlite3_column_int(st, 0);
        }
        if(rc!=SQLITE_DONE){ free(ids); sqlite3_finalize(st); return -1; }
    }
    sqlite3_finalize(st);
    if(n <= 0){ free(ids); return 0; }
    plan->list_type = 5; plan->ids = ids; plan->count = n;
    return 1;
}


typedef struct { const char *p, *skip; int after; } mdb_artist_iter;
static int mdb_artist_space(unsigned char c){ return c==' ' || c=='\t' || c=='\r' || c=='\n'; }
static int mdb_feature_len(const char *p){
    static const char *const words[]={"feat.","ft.","featuring"};
    for(unsigned i=0;i<sizeof words/sizeof words[0];i++){
        size_t n=strlen(words[i]);
        if(!strncasecmp(p,words[i],n) && mdb_artist_space((unsigned char)p[n])) return (int)n;
    }
    return 0;
}
/* A credit made only of ASCII spaces and control bytes (a stray DEL between two commas) names nobody. */
static int mdb_artist_visible(const char *s){
    for(const unsigned char *p=(const unsigned char *)s;*p;p++) if(*p>' ' && *p!=0x7f) return 1;
    return 0;
}
/* One streaming grammar for sizing, rows and membership. skip is only the
 * closing parenthesis of an explicitly recognised featured-credit wrapper. */
static int mdb_artist_next(mdb_artist_iter *it, char out[MDB_STR]){
    while(it->p && *it->p){
        const char *start=it->p, *end=start, *next=NULL, *skip=it->skip, *next_skip=skip;
        int after=it->after;
        for(;*end;end++){
            if(*end==',' || *end==';'){ next=end+1; break; }
            if(end==start || !mdb_artist_space((unsigned char)end[-1])) continue;
            int wrapper=*end=='(', len=mdb_feature_len(end+wrapper);
            if(!len) continue;
            const char *guest=end+wrapper+len;
            while(mdb_artist_space((unsigned char)*guest)) guest++;
            if(!*guest || *guest==',' || *guest==';' || *guest==')') continue;
            const char *close=wrapper ? strchr(guest,')') : NULL;
            if(wrapper && (!close || (memchr(guest,'(',(size_t)(close-guest))!=NULL))) continue;
            const char *left=start;
            while(left<end && mdb_artist_space((unsigned char)*left)) left++;
            if(left==end) continue;
            next=guest; if(wrapper) next_skip=close;
            break;
        }
        size_t n=0;
        for(const char *p=start;p<end;p++) if(p!=skip && n<MDB_STR-1) out[n++]=*p;
        out[n]=0; trim(out);
        it->p=next; it->skip=next_skip; it->after=1;
        if(after){
            const char *p=out;
            if(!strncasecmp(p,"and",3) && mdb_artist_space((unsigned char)p[3])) p+=3;
            else if(*p=='&' && mdb_artist_space((unsigned char)p[1])) p++;
            while(mdb_artist_space((unsigned char)*p)) p++;
            if(*p && p!=out) memmove(out,p,strlen(p)+1);
        }
        if(mdb_artist_visible(out)) return 1;
    }
    return 0;
}
int mdb_split_artists(const char *raw, char toks[][MDB_STR], int cap){
    mdb_artist_iter it={raw,NULL,0}; char token[MDB_STR]; int n=0;
    while((!toks || n<cap) && mdb_artist_next(&it,token)){
        if(toks) memcpy(toks[n],token,MDB_STR);
        n++;
    }
    return n;
}


/* Stock sort code of a name with no stored code (a split artist credit): first four UTF-8 bytes, ASCII lower-cased, top
 * bit cleared - the shape of every code the player stores (checked against a stock-built song.db). */
static int mdb_code4(const char *s){
    unsigned v = 0; int j = 0;
    for(const unsigned char *p = (const unsigned char *)(s ? s : ""); *p && j < 4; p++, j++) v = (v << 8) | (unsigned)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    for(; j < 4; j++) v <<= 8;
    return (int)(v & 0x7fffffffu);
}
/* Orders two names the way the player's lists do: stored sort code first, then case-insensitive name, then exact. */
static int mdb_code_name_cmp(int ca, const char *a, int cb, const char *b){
    if(ca != cb) return ca < cb ? -1 : 1;
    int c = strcasecmp(a, b);
    return c ? c : strcmp(a, b);
}
typedef struct { const char *name; int code; } mdb_nc_t;    /* a name and its stock sort code */
static int mdb_nc_cmp(const void *a, const void *b){ return strcmp(((const mdb_nc_t*)a)->name, ((const mdb_nc_t*)b)->name); }
/* Every distinct artist (group=0: ARTIST) or Artists-view key (group=1) in the library with its stored code, by exact name;
 * the lowest code when one name carries several. NULL with *n = 0 on OOM or an empty library. */
static mdb_nc_t *mdb_nc_build(int group, int *n){
    *n = 0;
    mdb_nc_t *t = malloc((size_t)(g_n > 0 ? g_n : 1) * sizeof *t);
    if(!t) return NULL;
    int m = 0;
    for(int i = 0; i < g_n; i++){
        const char *nm = group ? g_songs[i].artist_group : g_songs[i].artist;
        if(!nm[0]) continue;
        t[m].name = nm; t[m].code = group ? g_songs[i].code_group : g_songs[i].code_artist; m++;
    }
    qsort(t, (size_t)m, sizeof *t, mdb_nc_cmp);
    int w = 0;
    for(int i = 0; i < m; i++){
        if(w && !strcmp(t[w-1].name, t[i].name)){ if(t[i].code < t[w-1].code) t[w-1].code = t[i].code; }
        else t[w++] = t[i];
    }
    *n = w;
    return t;
}
static int mdb_nc_code(const mdb_nc_t *t, int n, const char *name){
    mdb_nc_t key = { name, 0 };
    const mdb_nc_t *hit = t ? bsearch(&key, t, (size_t)n, sizeof *t, mdb_nc_cmp) : NULL;
    return hit ? hit->code : mdb_code4(name);
}

typedef struct { const char *al; const char *ar; int idx; int cnt; int first; int code; unsigned h; const char *sort; } mdb_ai_t;   /* an album row: name, its artist, first song index, song count, lowest song index, sort code */
static int mdb_ai_cmp(const void *a, const void *b){
    const mdb_ai_t *x = a, *y = b;                            /* case-insensitive then exact then song index: a total order */
    int c = strcasecmp(x->al, y->al);
    if(!c) c = strcmp(x->al, y->al);
    if(!c) c = (x->h > y->h) - (x->h < y->h);           /* same first 159 bytes, different full value */
    return c ? c : (x->idx > y->idx) - (x->idx < y->idx);
}
static int mdb_ai_code_cmp(const void *a, const void *b){
    const mdb_ai_t *x = a, *y = b;
    int c=mdb_sort_order(x->code,x->al,x->sort,y->code,y->al,y->sort);
    return !c && (x->sort || y->sort) ? (x->h>y->h)-(x->h<y->h) : c;
}
int mdb_albums(char names[][MDB_STR], char artists[][MDB_STR], int *counts, int cap){
    if(g_calb_n < 0){                                        /* build once - the FULL set - then cache */
        mdb_ai_t *tmp = malloc((size_t)(g_n>0?g_n:1) * sizeof *tmp);
        if(!tmp) return 0;                                   /* transient OOM: don't cache, retry later */
        int m = 0;
        for(int i=0;i<g_n;i++) if(g_songs[i].album[0]){
            tmp[m].al=g_songs[i].album; tmp[m].ar=g_songs[i].artist; tmp[m].idx=i; tmp[m].cnt=1; tmp[m].first=i; tmp[m].code=g_songs[i].code_album; tmp[m].h=g_songs[i].album_h; m++; }
        qsort(tmp, (size_t)m, sizeof *tmp, mdb_ai_cmp);      /* sort by album, then group exact-equal names (adjacent) */
        /* dedup IN PLACE with NO cap, so a capped caller (e.g. the cover flow) can't truncate the shared
         * album cache and starve the full List view. */
        int n = 0;
        for(int i=0;i<m;i++){
            if(n>0 && !strcmp(tmp[i].al, tmp[n-1].al) && tmp[i].h == tmp[n-1].h){          /* exact ALBUM value, like stock's GROUP BY ALBUM: "Blue" and "blue" stay two albums */
                tmp[n-1].cnt++;
                if(tmp[i].idx < tmp[n-1].first) tmp[n-1].first = tmp[i].idx;
                if(tmp[i].code < tmp[n-1].code) tmp[n-1].code = tmp[i].code;   /* one row per merged name: its lowest code */
            } else tmp[n++] = tmp[i];
        }
        for(int i=0;i<n;i++) tmp[i].sort=mdb_sort_find(2,tmp[i].al,tmp[i].h,NULL,0);
        qsort(tmp, (size_t)n, sizeof *tmp, mdb_ai_code_cmp); /* the player's Albums order: stored ALBUM_CODE, then name */
        g_calb=malloc((size_t)(n>0?n:1)*MDB_STR); g_calb_ar=malloc((size_t)(n>0?n:1)*MDB_STR); g_calb_ct=malloc((size_t)(n>0?n:1)*sizeof(int));
        g_calb_rep=malloc((size_t)(n>0?n:1)*sizeof(int));
        g_calb_bucket=malloc((size_t)(n>0?n:1));
        if(g_calb && g_calb_ar && g_calb_ct && g_calb_rep && g_calb_bucket){
            for(int i=0;i<n;i++){ snprintf(g_calb[i],MDB_STR,"%s",tmp[i].al); snprintf(g_calb_ar[i],MDB_STR,"%s",tmp[i].ar); g_calb_ct[i]=tmp[i].cnt;
                                  g_calb_rep[i]=g_songs[tmp[i].first].id; g_calb_bucket[i]=mdb_bucket(tmp[i].sort ? tmp[i].sort : tmp[i].al); }
            g_calb_n=n;
        } else { free(g_calb_bucket); g_calb_bucket=NULL; free(g_calb); free(g_calb_ar); free(g_calb_ct); free(g_calb_rep); g_calb=NULL; g_calb_ar=NULL; g_calb_ct=NULL; g_calb_rep=NULL; free(tmp); return 0; }
        free(tmp);
        /* fall through to the capped copy-out */
    }
    int n = g_calb_n < cap ? g_calb_n : cap;                 /* return a cap-limited copy of the FULL cache */
    if(n>0){ memcpy(names,g_calb,(size_t)n*MDB_STR); memcpy(artists,g_calb_ar,(size_t)n*MDB_STR); memcpy(counts,g_calb_ct,(size_t)n*sizeof(int)); }
    return n;
}

/* Number of distinct albums in the library. Builds+caches the FULL set on first use (same cache as
 * mdb_albums), so a caller can size its buffers to the real count and never truncate a large library. */
/* Representative SONG.ID per album, in the same order as mdb_albums: the album's first song in library order -
 * the same song mdb_album_track_ids() lists first, found once while building the album cache instead of scanning
 * every song per album. Returns the number copied (cap-limited), or 0 if the cache can't be built. */
int mdb_album_rep_ids(int *out, int cap){
    if(g_calb_n < 0) mdb_albums(NULL, NULL, NULL, 0);
    if(g_calb_n <= 0 || !g_calb_rep || !out) return 0;
    int n = g_calb_n < cap ? g_calb_n : cap;
    memcpy(out, g_calb_rep, (size_t)n * sizeof *out);
    return n;
}

char mdb_album_bucket(int row){
    if(g_calb_n<0) mdb_albums(NULL,NULL,NULL,0);
    return row>=0 && row<g_calb_n ? g_calb_bucket[row] : '#';
}

int mdb_album_count(void){
    if(g_calb_n < 0) mdb_albums(NULL, NULL, NULL, 0);        /* cap 0 -> builds the full cache, copies nothing */
    return g_calb_n;                                          /* -1 if the build failed (OOM); >=0 = real count */
}

/* qsort comparator over the flat names[][MDB_STR] array (case-insensitive) */
static int mdb_name_ci_cmp(const void *a, const void *b){ return strcasecmp((const char*)a, (const char*)b); }

/* case-insensitive, then exact: a total order, so exact duplicates are adjacent for the dedup below */
static int mdb_name_ci_exact_cmp(const void *a, const void *b){
    int c = strcasecmp((const char*)a, (const char*)b);
    return c ? c : strcmp((const char*)a, (const char*)b);
}
/* Reorders n names (already de-duplicated) into the player's list order: stored sort code, then name. `group` picks the
 * artist or Artists-view code table; a name with no stored code (a split credit) gets the derived one. On OOM the list
 * stays in case-insensitive order. */
typedef struct { const char *name, *sort; int code; } mdb_order_name;
static int mdb_named_code_cmp(const void *a, const void *b){
    const mdb_order_name *x=a,*y=b;
    return mdb_sort_order(x->code,x->name,x->sort,y->code,y->name,y->sort);
}
static void mdb_order_names_by_code(char (*buf)[MDB_STR], int n, int group){
    if(n < 2) return;
    int tn = 0; mdb_nc_t *tbl = mdb_nc_build(group, &tn);
    mdb_order_name *arr = malloc((size_t)n * sizeof *arr);
    char (*out)[MDB_STR] = malloc((size_t)n * MDB_STR);
    if(arr && out){
        for(int i = 0; i < n; i++){ arr[i].name = buf[i]; arr[i].code = mdb_nc_code(tbl, tn, buf[i]); arr[i].sort=mdb_sort_find(group ? 1 : 0,buf[i],0,NULL,0); }
        qsort(arr, (size_t)n, sizeof *arr, mdb_named_code_cmp);
        for(int i = 0; i < n; i++) memcpy(out[i], arr[i].name, MDB_STR);
        memcpy(buf, out, (size_t)n * MDB_STR);
    }
    free(arr); free(out); free(tbl);
}
void mdb_set_artist_mode(int album_artist){
    album_artist = album_artist ? 1 : 0;
    if(album_artist == g_artist_mode) return;
    g_artist_mode = album_artist;
    free(g_cart); g_cart = NULL; g_cart_n = -1;              /* the Artists list is rebuilt in the new grouping */
}
int mdb_artist_mode(void){ return g_artist_mode; }

static int artist_credited(const char *raw, const char *artist);
/* Empty view key is missing metadata; raw song keys and the player's grouping stay intact. */
int mdb_artist_missing(const char *key){
    return !key || key[strspn(key, " \t\r\n\v\f")] == 0;
}
static int artist_has(const mdb_song_t *s, const char *key){
    const char *raw = g_artist_mode ? s->artist_group : s->artist;
    if(!key[0]) return g_artist_mode ? mdb_artist_missing(raw) : !mdb_split_artists(raw, NULL, 0);   /* no credit = Unknown */
    return g_artist_mode ? !strcmp(raw, key) : artist_credited(raw, key);
}

int mdb_artists(char names[][MDB_STR], int cap){
    if(g_cart_n < 0 && g_artist_mode){                       /* by album artist: the player's exact keys, unsplit */
        char (*buf)[MDB_STR] = malloc((size_t)(g_n > 0 ? g_n : 1) * MDB_STR);
        if(!buf) return 0;
        int n = 0;
        for(int i = 0; i < g_n; i++){
            if(mdb_artist_missing(g_songs[i].artist_group)) memset(buf[n++], 0, MDB_STR);
            else memcpy(buf[n++], g_songs[i].artist_group, MDB_STR);
        }
        qsort(buf, (size_t)n, MDB_STR, mdb_name_ci_exact_cmp);   /* total order: equal keys end up adjacent */
        int w = 0;
        for(int i = 0; i < n; i++)                           /* exact duplicates only: "ABBA" and "Abba" are two queues */
            if(w == 0 || strcmp(buf[w-1], buf[i]) != 0){ if(w != i) memcpy(buf[w], buf[i], MDB_STR); w++; }
        mdb_order_names_by_code(buf, w, 1);                  /* then the player's own order: ARTIST_CODE / ALBUM_ARTIST_CODE */
        g_cart = malloc((size_t)(w > 0 ? w : 1) * MDB_STR);
        if(g_cart){ if(w) memcpy(g_cart, buf, (size_t)w * MDB_STR); g_cart_n = w; }
        free(buf);
        if(g_cart_n < 0) return 0;
    }
    if(g_cart_n < 0){                                        /* build once, then cache */
        /* Size and fill with the same iterator, independently of the caller's cap,
         * so collaborations cannot truncate the shared cache before deduplication. */
        int maxtok = 0;
        for(int i=0;i<g_n;i++){
            int c=mdb_split_artists(g_songs[i].artist,NULL,0);
            if(!c) c=1;                                      /* no credit: one Unknown artist row */
            if(c>INT_MAX-maxtok || (size_t)(maxtok+c)>SIZE_MAX/MDB_STR) return 0;
            maxtok += c;
        }
        char (*buf)[MDB_STR] = malloc((size_t)(maxtok>0?maxtok:1) * MDB_STR);
        if(!buf) return 0;                                   /* transient OOM: leave -1, retry later */
        int n = 0;
        for(int i=0;i<g_n && n<maxtok;i++){
            int k = mdb_split_artists(g_songs[i].artist, buf + n, (int)(maxtok - n));
            if(k) n += k; else memset(buf[n++], 0, MDB_STR);
        }
        qsort(buf, (size_t)n, MDB_STR, g_sort_n ? mdb_name_ci_exact_cmp : mdb_name_ci_cmp);
        int w = 0;
        for(int i=0;i<n;i++)
            if(w==0 || strcasecmp(buf[w-1], buf[i]) != 0){
                if(w != i) memcpy(buf[w], buf[i], MDB_STR);
                w++;
            }
        mdb_order_names_by_code(buf, w, 0);                  /* then the player's own order: ARTIST_CODE */
        g_cart = malloc((size_t)(w>0?w:1) * MDB_STR);
        if(g_cart){ if(w) memcpy(g_cart, buf, (size_t)w * MDB_STR); g_cart_n = w; }
        free(buf);
        if(g_cart_n < 0) return 0;                           /* cache alloc failed: retry later */
    }
    int n = g_cart_n < cap ? g_cart_n : cap;                 /* cache hit -> instant copy */
    if(n>0) memcpy(names, g_cart, (size_t)n * MDB_STR);
    return n;
}

/* Number of distinct (tokenized) artists. Builds the cache on first use. Lets the caller size its
 * buffer so mdb_artists never has to clip. */
int mdb_artist_count(void){
    if(g_cart_n < 0){ char dummy[MDB_STR]; mdb_artists((char (*)[MDB_STR])dummy, 0); }   /* build cache, copy nothing */
    return g_cart_n < 0 ? 0 : g_cart_n;
}

typedef struct { const char *name; int code; int count; } mdb_gn_t;   /* a genre, its stock code, its song count */
static int mdb_gn_ci_cmp(const void *a, const void *b){ return strcasecmp(((const mdb_gn_t*)a)->name, ((const mdb_gn_t*)b)->name); }
static int mdb_gn_code_cmp(const void *a, const void *b){
    const mdb_gn_t *x = a, *y = b;
    return mdb_code_name_cmp(x->code, x->name, y->code, y->name);
}
int mdb_genres(char names[][MDB_STR], int *counts, int cap){
    if(g_cgen_n < 0){                                        /* build once, then cache */
        mdb_gn_t *tmp = malloc((size_t)(g_n>0?g_n:1) * sizeof *tmp);
        if(!tmp) return 0;
        int m=0;
        for(int i=0;i<g_n;i++) if(g_songs[i].genre[0]){ tmp[m].name=g_songs[i].genre; tmp[m].code=g_songs[i].code_genre; tmp[m].count=1; m++; }
        qsort(tmp, (size_t)m, sizeof *tmp, mdb_gn_ci_cmp);
        int n=0;                                             /* case variants merge into one row (its lowest code) */
        for(int i=0;i<m;i++){
            if(n>0 && !strcasecmp(tmp[i].name, tmp[n-1].name)){ tmp[n-1].count++; if(tmp[i].code < tmp[n-1].code) tmp[n-1].code = tmp[i].code; }
            else tmp[n++] = tmp[i];
        }
        qsort(tmp, (size_t)n, sizeof *tmp, mdb_gn_code_cmp); /* the player's Genres order: stored GENRE_CODE, then name */
        g_cgen=malloc((size_t)(n>0?n:1)*MDB_STR); g_cgen_ct=malloc((size_t)(n>0?n:1)*sizeof(int));
        if(g_cgen && g_cgen_ct){
            for(int i=0;i<n;i++){ snprintf(g_cgen[i],MDB_STR,"%s",tmp[i].name); g_cgen_ct[i]=tmp[i].count; }
            g_cgen_n=n;
        }
        else { free(g_cgen); free(g_cgen_ct); g_cgen=NULL; g_cgen_ct=NULL; free(tmp); return 0; }
        free(tmp);
    }
    int n = g_cgen_n < cap ? g_cgen_n : cap;                 /* cache hit -> instant copy */
    if(n>0){ memcpy(names,g_cgen,(size_t)n*MDB_STR); memcpy(counts,g_cgen_ct,(size_t)n*sizeof(int)); }
    return n;
}

int mdb_genre_count(void){
    if(g_cgen_n < 0) mdb_genres(NULL, NULL, 0);
    return g_cgen_n > 0 ? g_cgen_n : 0;
}

int mdb_genre_songs(const char *genre, const mdb_song_t **out, int cap){
    int n = 0;
    for(int i=0;i<g_n && n<cap;i++)
        if(!strcasecmp(g_songs[i].genre, genre)) out[n++] = &g_songs[i];
    return n;
}

/* Sort key for album ordering: the player lists album tracks by disc, then track, then title.
 * DISC=0 / TRACK=0 mean "unknown" and sort LAST (matches mq_player's ORDER_ALBUM CASE). `ord`
 * is the song's original (title-order) index, used as a stable tiebreak within a disc+track. */
typedef struct { int disc, track, ord; const mdb_song_t *s; } alb_key_t;
static int alb_cmp(const void *a, const void *b){
    const alb_key_t *x = a, *y = b;
    if(x->disc  != y->disc ) return (x->disc  < y->disc ) ? -1 : 1;
    if(x->track != y->track) return (x->track < y->track) ? -1 : 1;
    return x->ord - y->ord;
}

/* Up to `cap` SONG.IDs in an album, UNSORTED (linear scan only, no per-song DISC/TRACK query). For cover
 * lookup, where playback order is irrelevant - avoids mdb_album_songs' disc/track sort DB queries. */
int mdb_album_track_ids(const char *album, int *ids, int cap){
    int n = 0;
    for(int i=0;i<g_n && n<cap;i++) if(!strcmp(g_songs[i].album, album)) ids[n++] = g_songs[i].id;
    return n;
}

static int alb_id_cmp(const void *a, const void *b){
    const alb_key_t *x = a, *y = b;
    return (x->s->id > y->s->id) - (x->s->id < y->s->id);
}
static int alb_id_find(const void *a, const void *b){
    int id = *(const int *)a;
    const alb_key_t *k = b;
    return (id > k->s->id) - (id < k->s->id);
}
/* One DISC/TRACK read per key not read yet (disc 0); a miss or a read error sorts just that song last. */
static void alb_read_each(sqlite3 *d, alb_key_t *k, int n){
    sqlite3_stmt *st = NULL;
    if(sqlite3_prepare_v2(d, "SELECT DISC,TRACK FROM SONG WHERE ID=?;", -1, &st, NULL) != SQLITE_OK) st = NULL;
    for(int i=0;i<n;i++){
        if(k[i].disc) continue;
        int disc = 0, track = 0;
        if(st){
            sqlite3_reset(st);
            sqlite3_bind_int(st, 1, k[i].s->id);
            if(sqlite3_step(st) == SQLITE_ROW){ disc = sqlite3_column_int(st,0); track = sqlite3_column_int(st,1); }
        }
        k[i].disc  = (disc  > 0) ? disc  : INT_MAX;   /* unknown -> last */
        k[i].track = (track > 0) ? track : INT_MAX;
    }
    if(st) sqlite3_finalize(st);
}
int mdb_album_songs(const char *album, const mdb_song_t **out, int cap){
    if(cap <= 0) return 0;
    /* Select the album set by exact ALBUM value (the player's type-3 queue is ALBUM=?). Then reorder that set by the
     * player's disc/track order so the displayed rows match playback; g_songs' title order is the stable tiebreak. If
     * the DB is unavailable or the keys can't be allocated, the whole list keeps its title order; a single per-song
     * disc/track miss just sorts that one song last. Taps play by song ID via mdb_play_pos regardless. */
    int n = 0;
    for(int i=0;i<g_n && n<cap;i++)
        if(!strcmp(g_songs[i].album, album)) out[n++] = &g_songs[i];
    if(n <= 1) return n;
    sqlite3 *d = db(); if(!d) return n;
    alb_key_t *k = malloc((size_t)n * sizeof *k);
    if(!k) return n;
    for(int i=0;i<n;i++){ k[i].disc = 0; k[i].track = INT_MAX; k[i].ord = i; k[i].s = out[i]; }   /* disc 0 = not read yet */
    /* One read of the selected IDs (not the trimmed/truncated display album). Without its SQL buffer, or on a
     * prepare/read error, the keys not read yet fall back to one read each. */
    size_t size = (size_t)n > ((size_t)-1 - 64) / 12 ? 0 : (size_t)n * 12 + 64;
    char *sql = size ? malloc(size) : NULL;
    sqlite3_stmt *st = NULL;
    int rc = SQLITE_ERROR;
    if(sql){
        size_t used = (size_t)snprintf(sql, size, "SELECT ID,DISC,TRACK FROM SONG WHERE ID IN (");
        for(int i=0;i<n;i++) used += (size_t)snprintf(sql + used, size - used, "%s%d", i ? "," : "", out[i]->id);
        snprintf(sql + used, size - used, ");");
        if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) st = NULL;
        free(sql);
    }
    if(st){
        qsort(k, (size_t)n, sizeof *k, alb_id_cmp);
        while((rc = sqlite3_step(st)) == SQLITE_ROW){
            int id = sqlite3_column_int(st, 0);
            alb_key_t *hit = bsearch(&id, k, (size_t)n, sizeof *k, alb_id_find);
            if(hit){
                int disc = sqlite3_column_int(st, 1), track = sqlite3_column_int(st, 2);
                hit->disc = disc > 0 ? disc : INT_MAX;
                hit->track = track > 0 ? track : INT_MAX;
            }
        }
        sqlite3_finalize(st);
    }
    if(rc == SQLITE_DONE){ for(int i=0;i<n;i++) if(!k[i].disc) k[i].disc = INT_MAX; }   /* no row -> last */
    else alb_read_each(d, k, n);
    qsort(k, (size_t)n, sizeof *k, alb_cmp);
    for(int i=0;i<n;i++) out[i] = k[i].s;
    free(k);
    return n;
}

/* Membership uses the same iterator as cache sizing and enumeration. */
static int artist_credited(const char *raw, const char *artist){
    mdb_artist_iter it={raw,NULL,0}; char token[MDB_STR];
    while(mdb_artist_next(&it,token)) if(!strcasecmp(token,artist)) return 1;
    return 0;
}

int mdb_artist_songs(const char *artist, const mdb_song_t **out, int cap){
    int n = 0;
    for(int i=0;i<g_n && n<cap;i++)
        if(artist && artist_has(&g_songs[i], artist))
            out[n++] = &g_songs[i];
    return n;
}
/* The play plan for one Artists-view key under "Album Artist" (mdb_artist_mode 1): the player's own artist queue
 * (type 2, COALESCE(ALBUM_ARTIST,ARTIST)=key in its title order - the same ORDER BY as mdb_play_pos) when it holds
 * exactly the songs the view shows; else an exact reserved-slot queue (type 5) of the view's songs. plan->ids is the
 * queue in the order it will play, so mdb_plan_pos is a position in the queue the player really builds. */
int mdb_artist_plan(const char *key, mdb_plan_t *plan){
    if(!key || !plan) return 0;
    memset(plan, 0, sizeof *plan);
    if(!key[0]){                      /* multiple raw keys: exact songs, in unchanged library order */
        int n = 0;
        for(int i = 0; i < g_n; i++) if(artist_has(&g_songs[i], key)) n++;
        if(!n) return 0;
        plan->ids = malloc((size_t)n * sizeof *plan->ids);
        if(!plan->ids) return 0;
        for(int i = 0; i < g_n; i++) if(artist_has(&g_songs[i], key)) plan->ids[plan->count++] = g_songs[i].id;
        plan->list_type = 5;
        return 1;
    }
    int n = 0;
    for(int i = 0; i < g_n; i++) if(!strcmp(g_songs[i].artist_group, key)) n++;
    if(n <= 0) return 0;
    int *a = malloc((size_t)n * sizeof *a), *b = malloc((size_t)(n + 1) * sizeof *b);
    if(!a || !b){ free(a); free(b); return 0; }
    int k = 0;
    for(int i = 0; i < g_n && k < n; i++) if(!strcmp(g_songs[i].artist_group, key)) a[k++] = g_songs[i].id;
    int m = -1;
    sqlite3 *d = db();
    sqlite3_stmt *st;
    /* the player's artist-queue order: the same title-code ORDER BY as mdb_play_pos's ORDER_TAIL / MDB_FAV_ORDER */
    if(d && sqlite3_prepare_v2(d, "SELECT ID FROM SONG WHERE COALESCE(ALBUM_ARTIST,ARTIST)=?1 ORDER BY "
        "CASE WHEN IS_CUE=0 AND IS_ISO=0 THEN 1 WHEN IS_CUE=1 OR IS_ISO=1 THEN 2 END,"
        "CASE WHEN (IS_CUE=0 AND IS_ISO=0) THEN CASE WHEN TITLE IS NOT NULL THEN TITLE_CODE ELSE NAME_CODE END END,"
        "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN NAME_CODE END,"
        "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN ID END,"
        "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN TRACK END;",
                               -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        int rc; m = 0;
        while((rc = sqlite3_step(st)) == SQLITE_ROW){ if(m < n + 1) b[m] = sqlite3_column_int(st, 0); m++; }
        sqlite3_finalize(st);
        if(rc != SQLITE_DONE) m = -1;
    }
    int same = m == n;                                   /* same membership (the queue order is the player's) */
    if(same){
        qsort(a, (size_t)n, sizeof *a, mdb_id_cmp);
        for(int i = 0; same && i < n; i++)
            same = bsearch(&b[i], a, (size_t)n, sizeof *a, mdb_id_cmp) != NULL;
    }
    /* a key that filled MDB_STR may be a truncated name: never the player's queue (always exact) */
    if(same && mdb_artist_class_type() == 1 && strlen(key) < MDB_STR - 1 && strlen(key) < sizeof plan->name){
        plan->list_type = 2; snprintf(plan->name, sizeof plan->name, "%s", key);
        memcpy(a, b, (size_t)n * sizeof *a);             /* the player's order */
    } else {
        plan->list_type = 5; plan->name[0] = 0;
        if(m == n){                                     /* restore view order after membership sorting */
            k = 0;
            for(int i = 0; i < g_n && k < n; i++) if(!strcmp(g_songs[i].artist_group, key)) a[k++] = g_songs[i].id;
        }
    }
    free(b);
    plan->ids = a; plan->count = n;
    return 1;
}

/* ---- Scoped albums: Artist -> Albums -> Songs and Genre -> Albums -> Songs (parity L20/L23) ------------------------
 * V2.57 mq_ui lists an artist's / a genre's albums and plays them with the stock artist+album (type 7, payload
 * {"artist":"X", "album":"Y"}) and genre+album (type 8, payload {"style":"X", "album":"Y"}) queues (mq_ui
 * snprintf sites 0x44c46c / 0x44c504). The scope membership below is the same predicate the flat drills use
 * (mdb_artist_songs / mdb_genre_songs), so an album row's count matches the songs it opens. */
static int scope_has(int kind, const mdb_song_t *s, const char *key){
    if(kind == MDB_SCOPE_GENRE) return !strcasecmp(s->genre, key);
    return artist_has(s, key);
}
static int scope_song_cmp(const void *a, const void *b){        /* the player's album order (code, name; exact name = one group), then library order */
    const mdb_song_t *x = *(const mdb_song_t *const *)a, *y = *(const mdb_song_t *const *)b;
    /* No stored code (a database without the code columns): the exact name alone, as stock's GROUP BY ALBUM groups it. */
    int c = (x->code_album == 0 && y->code_album == 0) ? strcmp(x->album, y->album)
          : mdb_code_name_cmp(x->code_album, x->album, y->code_album, y->album);   /* exact value last, so equal names stay one group */
    return c ? c : (x < y ? -1 : x > y);
}
static int scope_id_cmp(const void *a, const void *b){
    int x = (*(const mdb_song_t *const *)a)->id, y = (*(const mdb_song_t *const *)b)->id;
    return (x > y) - (x < y);
}
static const char *mdb_song_album_sort(int id){
    char key[24]; snprintf(key,sizeof key,"%d",id);
    return mdb_sort_find(4,key,0,NULL,0);
}
static int mdb_scope_group_cmp(const void *a, const void *b){
    const mdb_ai_t *x=a,*y=b;
    if(!x->sort && !y->sort && !x->code && !y->code) return strcmp(x->al,y->al);
    return mdb_ai_code_cmp(a,b);
}
static int mdb_scope_sorted(int kind, const char *key, char names[][MDB_STR], char artists[][MDB_STR], int *counts, int cap, char *buckets){
    mdb_ai_t *rows=calloc((size_t)(g_n ? g_n : 1),sizeof *rows);
    if(!rows) return 0;
    int m=0;
    for(int i=0;i<g_n;i++) if(g_songs[i].album[0] && scope_has(kind,&g_songs[i],key)){
        rows[m].al=g_songs[i].album; rows[m].ar=g_songs[i].artist; rows[m].idx=i;
        rows[m].cnt=1; rows[m].code=g_songs[i].code_album;
        rows[m].sort=mdb_song_album_sort(g_songs[i].id); m++;
    }
    qsort(rows,(size_t)m,sizeof *rows,mdb_ai_cmp);
    int n=0;
    for(int i=0;i<m;i++){
        if(n && !strcmp(rows[n-1].al,rows[i].al)){
            rows[n-1].cnt++;
            if(rows[i].code<rows[n-1].code) rows[n-1].code=rows[i].code;
            rows[n-1].sort=mdb_sort_min(rows[n-1].sort,rows[i].sort);
        } else rows[n++]=rows[i];
    }
    qsort(rows,(size_t)n,sizeof *rows,mdb_scope_group_cmp);
    for(int i=0;names && i<n && i<cap;i++){
        snprintf(names[i],MDB_STR,"%s",rows[i].al);
        if(artists) snprintf(artists[i],MDB_STR,"%s",rows[i].ar);
        if(counts) counts[i]=rows[i].cnt;
        if(buckets) buckets[i]=mdb_bucket(rows[i].sort ? rows[i].sort : rows[i].al);
    }
    free(rows); return n;
}
char mdb_scope_album_bucket(int kind, const char *key, const char *album){
    const char *sort=NULL;
    for(int i=0;i<g_n;i++) if(!strcmp(g_songs[i].album,album) && scope_has(kind,&g_songs[i],key))
        sort=mdb_sort_min(sort,mdb_song_album_sort(g_songs[i].id));
    return mdb_bucket(sort ? sort : album);
}
int mdb_scope_albums_buckets(int kind, const char *key, char names[][MDB_STR], char artists[][MDB_STR], int *counts, int cap, char *buckets){
    if(!key || (!key[0] && kind != MDB_SCOPE_ARTIST)) return 0;
    for(int i=0;i<g_sort_n;i++) if(g_sort_keys[i].kind==2)
        return mdb_scope_sorted(kind,key,names,artists,counts,cap,buckets);
    const mdb_song_t **tmp = malloc((size_t)(g_n > 0 ? g_n : 1) * sizeof *tmp);
    if(!tmp) return 0;
    int m = 0;
    for(int i = 0; i < g_n; i++) if(g_songs[i].album[0] && scope_has(kind, &g_songs[i], key)) tmp[m++] = &g_songs[i];
    qsort(tmp, (size_t)m, sizeof *tmp, scope_song_cmp);
    int n = 0;
    for(int i = 0; i < m; ){
        int j = i + 1;
        while(j < m && !strcmp(tmp[j]->album, tmp[i]->album)) j++;
        if(names && n < cap){
            snprintf(names[n], MDB_STR, "%s", tmp[i]->album);
            if(artists) snprintf(artists[n], MDB_STR, "%s", tmp[i]->artist);
            if(counts)  counts[n] = j - i;
            if(buckets) buckets[n]=mdb_bucket(tmp[i]->album);
        }
        n++; i = j;
    }
    free(tmp);
    return n;
}
int mdb_scope_albums(int kind, const char *key, char names[][MDB_STR], char artists[][MDB_STR], int *counts, int cap){
    return mdb_scope_albums_buckets(kind,key,names,artists,counts,cap,NULL);
}
/* The songs of one album inside a scope, in the player's album order (disc, track, title code; unknown last). Fills up to
 * cap rows of out and returns the number written (out NULL: the full count). *exact = 1 when the order came from the DB; 0 = the DB order could not be
 * read for every row, so the rows are in library order (the plan then always uses the exact custom queue). */
static int scope_album_rows(int kind, const char *key, const char *album, const mdb_song_t **out, int cap, int *exact){
    if(exact) *exact = 0;
    if(!key || (!key[0] && kind != MDB_SCOPE_ARTIST) || !album || !album[0]) return 0;
    const mdb_song_t **cand = malloc((size_t)(g_n > 0 ? g_n : 1) * sizeof *cand);
    if(!cand) return 0;
    int n = 0;
    for(int i = 0; i < g_n; i++) if(!strcmp(g_songs[i].album, album) && scope_has(kind, &g_songs[i], key)) cand[n++] = &g_songs[i];
    if(n == 0){ free(cand); return 0; }
    const mdb_song_t **srt = malloc((size_t)n * sizeof *srt), **ord = malloc((size_t)n * sizeof *ord);
    const mdb_song_t **use = cand;
    if(srt && ord){
        memcpy(srt, cand, (size_t)n * sizeof *srt);
        qsort(srt, (size_t)n, sizeof *srt, scope_id_cmp);
        sqlite3 *d = db();
        sqlite3_stmt *st;
        if(d && sqlite3_prepare_v2(d, "SELECT ID FROM SONG WHERE trim(ALBUM, ' ' || char(9,10,13)) = ?1 ORDER BY "
                                      MDB_ALBUM_ORDER ";", -1, &st, NULL) == SQLITE_OK){
            sqlite3_bind_text(st, 1, album, -1, SQLITE_STATIC);
            int k = 0, rc;
            while((rc = sqlite3_step(st)) == SQLITE_ROW){
                mdb_song_t probe; probe.id = sqlite3_column_int(st, 0);
                const mdb_song_t *pp = &probe;
                const mdb_song_t **hit = bsearch(&pp, srt, (size_t)n, sizeof *srt, scope_id_cmp);
                if(hit && k < n) ord[k++] = *hit;
            }
            sqlite3_finalize(st);
            if(rc == SQLITE_DONE && k == n){ use = ord; if(exact) *exact = 1; }
        }
    }
    int w = n < cap ? n : cap;
    if(out) for(int i = 0; i < w; i++) out[i] = use[i];
    free(cand); free(srt); free(ord);
    return out ? w : n;      /* rows written when out is given (never more than cap); the full count for a count-only call */
}
int mdb_scope_album_songs(int kind, const char *key, const char *album, const mdb_song_t **out, int cap){
    return scope_album_rows(kind, key, album, out, cap, NULL);
}
/* The play plan for one album inside a scope: the stock artist+album queue (type 7) or genre+album queue (type 8) when the
 * player builds exactly these songs in exactly this order from a payload that can carry the key (no '"': the player
 * reads each value with sscanf %[^"]); else the exact reserved-slot queue (type 5, built at play time). Same rule as
 * mdb_album_plan, so a position computed from plan->ids is always a position in the queue the player really builds. */
int mdb_scope_album_plan(int kind, const char *key, const char *album, mdb_plan_t *plan){
    if(!plan) return 0;
    memset(plan, 0, sizeof *plan);
    if(!key || (!key[0] && kind != MDB_SCOPE_ARTIST) || !album || !album[0]) return 0;
    int n = scope_album_rows(kind, key, album, NULL, 0, NULL);
    if(n <= 0) return 0;
    const mdb_song_t **rows = malloc((size_t)n * sizeof *rows);
    int *a = malloc((size_t)n * sizeof *a), *b = malloc((size_t)(n + 1) * sizeof *b);
    int exact = 0;
    if(!rows || !a || !b || scope_album_rows(kind, key, album, rows, n, &exact) != n){ free(rows); free(a); free(b); return 0; }
    for(int i = 0; i < n; i++) a[i] = rows[i]->id;
    free(rows);
    plan->list_type = 5;
    int quotable = key[0] && !strchr(key, '"') && !strchr(album, '"') && strlen(key) < MDB_TYPE7_MAX && strlen(album) < MDB_TYPE7_MAX;
    const char *where = NULL, *fmt = NULL; int lt = 0;
    if(kind == MDB_SCOPE_GENRE){
        where = "GENRE=?1 AND ALBUM=?2"; lt = 8; fmt = "{\"style\":\"%s\", \"album\":\"%s\"}";
    } else {
        int cls = mdb_artist_class_type();          /* the player's own Artist setting decides which field its queue matches */
        if(cls >= 0){
            where = cls == 1 ? "COALESCE(ALBUM_ARTIST,ARTIST)=?1 AND ALBUM=?2" : "ARTIST=?1 AND ALBUM=?2";
            lt = 7; fmt = "{\"artist\":\"%s\", \"album\":\"%s\"}";
        }
    }
    if(exact && quotable && where){
        int m = ids_ordered(where, key, 0, album, 0, b, n + 1);
        if(m >= 0 && same_seq(a, n, b, m)){
            int w = snprintf(plan->name, sizeof plan->name, fmt, key, album);
            if(w > 0 && w < (int)sizeof plan->name) plan->list_type = lt;   /* never send a truncated key */
        }
    }
    if(plan->list_type == 5) plan->name[0] = 0;
    free(b);
    plan->ids = a; plan->count = n;
    return 1;
}

/* The V2.57 favourites queue order: the same title-code ORDER BY as mdb_play_pos's ORDER_TAIL, on MY_LOVE
 * (device-checked 2026-09-27: LIST_SONG_0 rows came out in exactly this order). Keep the two in step. */
#define MDB_FAV_ORDER \
    "CASE WHEN IS_CUE=0 AND IS_ISO=0 THEN 1 WHEN IS_CUE=1 OR IS_ISO=1 THEN 2 END," \
    "CASE WHEN (IS_CUE=0 AND IS_ISO=0) THEN CASE WHEN TITLE IS NOT NULL THEN TITLE_CODE ELSE NAME_CODE END END," \
    "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN NAME_CODE END," \
    "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN ID END," \
    "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN TRACK END"
static int fav_prepare(sqlite3 *d, int stock_order, sqlite3_stmt **st){   /* shared by mdb_favorites and mdb_favorite_ids: one SQL text */
    /* MY_LOVE.ID is its OWN autoincrement, NOT a SONG.ID. Rows carry both: the SONG.ID (resolved by PATH,
     * 0 if the song left the library) for the all-songs play, and the MY_LOVE.ID for removal and the V2.57
     * favourites queue. With stock_order the rows follow that queue, so row N is queue position N. */
    char sql[1400];
    snprintf(sql, sizeof sql,
        "SELECT IFNULL(TITLE,IFNULL(NAME,'Untitled')),IFNULL(ARTIST,''),IFNULL(ALBUM,''),"
        /* the song row: the same track of the same file (a CUE track is PATH + TRACK), else the file's whole-file row,
         * else its first CUE track - never an arbitrary track of a sheet. (SQLite does not let a subquery's ORDER BY
         * see the outer row, so each choice is its own correlated WHERE.) */
        "IFNULL(DURATION,0),COALESCE("
        "(SELECT S.ID FROM SONG S WHERE S.PATH=MY_LOVE.PATH AND COALESCE(S.IS_CUE,0)=COALESCE(MY_LOVE.IS_CUE,0) "
        "AND COALESCE(S.IS_ISO,0)=COALESCE(MY_LOVE.IS_ISO,0) AND COALESCE(S.TRACK,0)=COALESCE(MY_LOVE.TRACK,0) LIMIT 1),"
        "(SELECT S.ID FROM SONG S WHERE S.PATH=MY_LOVE.PATH AND COALESCE(S.IS_CUE,0)=0 AND COALESCE(S.IS_ISO,0)=0 LIMIT 1),"
        "(SELECT S.ID FROM SONG S WHERE S.PATH=MY_LOVE.PATH ORDER BY S.TRACK, S.ID LIMIT 1)),ID "
        "FROM MY_LOVE WHERE lower(PATH) NOT LIKE '%%.m4b' ORDER BY %s;",   /* books have their own view */
        stock_order ? MDB_FAV_ORDER : "1 COLLATE NOCASE");
    return sqlite3_prepare_v2(d, sql, -1, st, NULL) == SQLITE_OK;
}
int mdb_favorites(mdb_song_t *out, int cap, int stock_order){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(!fav_prepare(d, stock_order, &st)) return 0;
    int n=0;
    while(n<cap && sqlite3_step(st) == SQLITE_ROW){
        mdb_song_t *s = &out[n++];
        snprintf(s->title,  MDB_STR, "%s", colt(st,0));
        snprintf(s->artist, MDB_STR, "%s", colt(st,1));
        snprintf(s->album,  MDB_STR, "%s", colt(st,2));
        s->album_h = album_hash(colt(st,2));
        s->dur_ms = sqlite3_column_int(st,3);
        s->id     = sqlite3_column_int(st,4);
        s->love_id = sqlite3_column_int(st,5);
        s->genre[0] = 0;
        trim(s->title); trim(s->artist); trim(s->album);
    }
    sqlite3_finalize(st); return n;
}

/* Cast: the favourites that still have a library song, in the stock queue order, ids only (4 + 4 bytes each, not an 836-byte mdb_song_t). */
int mdb_favorite_ids(int *song_ids, int *love_ids, int cap){
    sqlite3 *d = db(); sqlite3_stmt *st; int n = 0;
    if(!d || !song_ids || !love_ids || cap <= 0 || !fav_prepare(d, 1, &st)) return -1;
    int rc;
    while((rc = sqlite3_step(st)) == SQLITE_ROW){
        int sid = sqlite3_column_int(st, 4);
        if(sid <= 0) continue;                          /* the song left the library: nothing to cast */
        if(n == cap){ n = -1; break; }
        song_ids[n] = sid; love_ids[n++] = sqlite3_column_int(st, 5);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? n : -1;
}
int mdb_favorite_count(void){
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st; int n = -1;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM MY_LOVE WHERE lower(PATH) NOT LIKE '%.m4b';", -1, &st, NULL) != SQLITE_OK) return -1;
    if(sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st); return n;
}
int mdb_love_path(int love_id, char *out, int cap){
    if(cap > 0) out[0] = 0;
    sqlite3 *d = db(); if(!d || love_id <= 0 || cap <= 0) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT PATH FROM MY_LOVE WHERE ID=?1;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, love_id);
    int ok = 0;
    if(sqlite3_step(st) == SQLITE_ROW){ path_fits(out, cap, colt(st,0)); ok = out[0] != 0; }
    sqlite3_finalize(st); return ok;
}

/* ---- Play history (diskOS PLAY_STATS): Most-Played / Recently-Played ------- */
/* Record one play; failure leaves the caller's retry identity unchanged. */
int mdb_record_play(const char *path){
    if(!path || !path[0]) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d,
        "INSERT INTO PLAY_STATS(PATH,PLAYS,LAST_PLAYED) VALUES(?1,1,?2) "
        "ON CONFLICT(PATH) DO UPDATE SET PLAYS=PLAYS+1, LAST_PLAYED=?2;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text (st, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
    int rc=sqlite3_step(st); sqlite3_finalize(st);
    return rc==SQLITE_DONE;
}
/* Fill `out` with songs from PLAY_STATS joined to SONG, ordered by plays (by_recent=0) or by
 * last-played time (by_recent=1). Playback is by SONG.ID, same as favourites. Returns count. */
static int mdb_stats_list(mdb_song_t *out, int cap, int by_recent){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    const char *sql = by_recent
      ? "SELECT IFNULL(S.TITLE,IFNULL(S.NAME,'Untitled')),IFNULL(S.ARTIST,''),IFNULL(S.ALBUM,''),"
        "IFNULL(S.DURATION,0),S.ID FROM PLAY_STATS P JOIN SONG S ON S.PATH=P.PATH "
        "WHERE lower(S.PATH) NOT LIKE '%.m4b' ORDER BY P.LAST_PLAYED DESC LIMIT ?1;"   /* books have their own view */
      : "SELECT IFNULL(S.TITLE,IFNULL(S.NAME,'Untitled')),IFNULL(S.ARTIST,''),IFNULL(S.ALBUM,''),"
        "IFNULL(S.DURATION,0),S.ID FROM PLAY_STATS P JOIN SONG S ON S.PATH=P.PATH "
        "WHERE P.PLAYS>0 AND lower(S.PATH) NOT LIKE '%.m4b' ORDER BY P.PLAYS DESC, P.LAST_PLAYED DESC LIMIT ?1;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, cap);
    int n=0;
    while(n<cap && sqlite3_step(st) == SQLITE_ROW){
        mdb_song_t *s = &out[n++];
        snprintf(s->title,  MDB_STR, "%s", colt(st,0));
        snprintf(s->artist, MDB_STR, "%s", colt(st,1));
        snprintf(s->album,  MDB_STR, "%s", colt(st,2));
        s->album_h = album_hash(colt(st,2));
        s->dur_ms = sqlite3_column_int(st,3);
        s->id     = sqlite3_column_int(st,4);
        s->genre[0] = 0;
        trim(s->title); trim(s->artist); trim(s->album);
    }
    sqlite3_finalize(st); return n;
}
int mdb_stats_count(int by_recent){
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st;
    const char *sql = by_recent
        ? "SELECT COUNT(*) FROM PLAY_STATS P JOIN SONG S ON S.PATH=P.PATH WHERE lower(S.PATH) NOT LIKE '%.m4b';"
        : "SELECT COUNT(*) FROM PLAY_STATS P JOIN SONG S ON S.PATH=P.PATH WHERE P.PLAYS>0 AND lower(S.PATH) NOT LIKE '%.m4b';";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    int n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return n;
}
int mdb_mostplayed(mdb_song_t *out, int cap){ return mdb_stats_list(out, cap, 0); }
int mdb_recent(mdb_song_t *out, int cap){ return mdb_stats_list(out, cap, 1); }

/* GENRE of the SONG row for `path` (or its sub-track `track`; the first row when 0), for the Song Info page. 1 = found (out set, "" when
 * untagged), 0 = no row, -1 = retry (out untouched). Worker owns a short-lived read-only connection. */
int mdb_song_genre(const char *path, int track, char *out, int cap){
    if(!path || !path[0] || cap <= 0) return 0;
    sqlite3 *d = db_reader(); if(!d) return -1;
    sqlite3_stmt *st; int found = -1;
    /* track > 0 = a CUE/ISO sub-track: that exact (PATH, TRACK) row, since the rows of one sheet can carry different genres */
    if(sqlite3_prepare_v2(d, track > 0 ? "SELECT IFNULL(GENRE,'') FROM SONG WHERE PATH=?1 AND TRACK=?2 AND (IS_CUE=1 OR IS_ISO=1) ORDER BY ID LIMIT 1;"
                                       : "SELECT IFNULL(GENRE,'') FROM SONG WHERE PATH=?1 ORDER BY ID LIMIT 1;", -1, &st, NULL) != SQLITE_OK){ sqlite3_close(d); return -1; }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    if(track > 0) sqlite3_bind_int(st, 2, track);
    int rc=sqlite3_step(st);
    if(rc == SQLITE_ROW){ snprintf(out, (size_t)cap, "%s", colt(st, 0)); found = 1; }
    else if(rc==SQLITE_DONE){ out[0]=0; found=0; }
    sqlite3_finalize(st);
    sqlite3_close(d);
    return found;
}

/* ---- Audiobooks (v1: single-file .m4b books) ------------------------------ */
int mdb_is_book_path(const char *path){
    if(!DISKOS_AUDIOBOOKS) return 0;   /* compile-time gate (currently 1): when built out, a .m4b is treated as ordinary audio, not a book */
    if(!path) return 0;
    size_t n = strlen(path);
    return n > 4 && !strcasecmp(path + n - 4, ".m4b");
}

/* List audiobooks (one book per .m4b), each joined to its saved progress. Ordered so a book with the
 * most recent bookmark leads ("continue listening"), then by add time. */
/* Returns the number of books (>=0), or -1 on a DB/read error (db unavailable, prepare failed, or a step
 * error). A caller must distinguish -1 (show a retry state) from 0 (genuinely no audiobooks), or a
 * transient DB failure looks like an empty library. */
int mdb_books(book_t *out, int max){
    if(max <= 0) return 0;
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st;
    const char *sql =
        "SELECT S.ID, IFNULL(S.TITLE,IFNULL(S.NAME,'Untitled')), IFNULL(S.ARTIST,''), S.PATH, "
        "IFNULL(B.POSITION_MS,0), IFNULL(B.COMPLETED,0) "
        "FROM BOOKS S LEFT JOIN BOOK_PROGRESS B ON B.BOOK_KEY = S.PATH "
        "ORDER BY IFNULL(B.UPDATED_AT,0) DESC, IFNULL(S.ADD_TIME,0) DESC, S.ID DESC;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    int n = 0, rc;
    while(n < max && (rc = sqlite3_step(st)) == SQLITE_ROW){
        book_t *b = &out[n++];
        b->id = sqlite3_column_int(st, 0);
        snprintf(b->title,  MDB_STR, "%s", colt(st, 1));
        snprintf(b->author, MDB_STR, "%s", colt(st, 2));
        path_fits(b->path, sizeof b->path, colt(st, 3));   /* too long: empty (the Books screen refuses it), never a prefix */
        b->position_ms = (long)sqlite3_column_int64(st, 4);
        b->completed   = sqlite3_column_int(st, 5);
        trim(b->title); trim(b->author);
    }
    int err = (n == 0 && rc != SQLITE_DONE && rc != SQLITE_ROW);   /* first step errored (not "no rows") -> report as error */
    sqlite3_finalize(st);
    return err ? -1 : n;
}

/* Saved resume position for a book. Returns 1 (fills member_out/position_ms) if a bookmark exists. */
/* Returns 1 = bookmark found, 0 = no bookmark, -1 = read error (DB unavailable / prepare failed). A -1
 * must NOT be treated as "start from zero": a caller that then checkpoints would erase a real bookmark
 * that was only temporarily unreadable. */
int mdb_book_progress(const char *key, char *member_out, int member_cap, long *position_ms, int *completed_out, long *updated_out){
    if(completed_out) *completed_out = 0;
    if(updated_out) *updated_out = 0;
    if(!key) return -1;
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st; int found = 0;
    /* read POSITION, COMPLETED and UPDATED_AT together so a caller can decide resume-vs-restart from the
     * LATEST saved state (the book may have finished since the list was built) and how long it's been idle. */
    if(sqlite3_prepare_v2(d, "SELECT MEMBER_PATH,POSITION_MS,COMPLETED,UPDATED_AT FROM BOOK_PROGRESS WHERE BOOK_KEY=? LIMIT 1;", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    if(rc == SQLITE_ROW){
        if(member_out && member_cap > 0) snprintf(member_out, (size_t)member_cap, "%s", colt(st, 0));
        if(position_ms) *position_ms = (long)sqlite3_column_int64(st, 1);
        if(completed_out) *completed_out = sqlite3_column_int(st, 2);
        if(updated_out) *updated_out = (long)sqlite3_column_int64(st, 3);
        found = 1;
    } else if(rc != SQLITE_DONE){
        found = -1;   /* BUSY / IOERR etc. -> a read error, not "no bookmark" */
    }
    sqlite3_finalize(st);
    return found;
}

/* Checkpoint a book's position (upsert). Called periodically during playback and on pause/exit.
 * Returns 1 if the row was actually written, 0 on any failure (so the caller can retry rather than
 * advance its save-throttle timestamp and silently drop the bookmark). */
int mdb_book_save(const char *key, const char *member_path, long position_ms, int completed){
    if(!key || !member_path) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    const char *sql =
        "INSERT INTO BOOK_PROGRESS(BOOK_KEY,MEMBER_PATH,POSITION_MS,UPDATED_AT,COMPLETED) VALUES(?1,?2,?3,?4,?5) "
        "ON CONFLICT(BOOK_KEY) DO UPDATE SET MEMBER_PATH=?2,POSITION_MS=?3,UPDATED_AT=?4,COMPLETED=?5;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, member_path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, position_ms < 0 ? 0 : position_ms);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)time(NULL));
    sqlite3_bind_int(st, 5, completed ? 1 : 0);
    int ok = (sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return ok;
}

/* Audiobook isolation: the stock player builds the music queue as an unfiltered SELECT FROM SONG, so a
 * book must NOT be in SONG (or it leaks into Play-All/shuffle). Instead a book plays from a RESERVED
 * custom-playlist slot: the player resolves a list_type-5 play by seq via
 *   SELECT LIST_ID FROM CUSTOM_PLAYLIST_INDEX ORDER BY LIST_ID LIMIT 1 OFFSET <seq>
 * and diskOS's play frame always sends seq 0, so the reserved slot must sit at the LOWEST LIST_ID.
 * Device-verified: the player re-reads BOTH the registry and the membership FRESH on each play (no
 * mq_player restart needed), and a member with no SONG row still builds + decodes. This idempotently
 * ensures the reserved registry row exists and sets its single member to `path`, copying the book's
 * metadata from SONG when present, else a bare-path row. Returns 1 on success. */
int mdb_reserved_slot_set(const char *path){
    sqlite3 *d = db(); if(!d || !path || !*path) return 0;
    if(sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) != SQLITE_OK) return 0;
    int ok = 1;
    if(sqlite3_exec(d, "INSERT OR IGNORE INTO CUSTOM_PLAYLIST_INDEX (LIST_ID,LIST_NAME,M3U_PATH) VALUES ("
                       XSTR(DISKOS_RSV_LISTID) ",'diskos-book','');", 0, 0, 0) != SQLITE_OK) ok = 0;
    if(ok && sqlite3_exec(d, "DELETE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=" XSTR(DISKOS_RSV_LISTID) ";", 0, 0, 0) != SQLITE_OK) ok = 0;
    if(ok){
        sqlite3_stmt *st;
        /* DURATION must be NON-ZERO: a 0-length slot makes the player treat the book as already finished
         * (instant EOF in Single mode). Use the real duration when known, else a large placeholder - the
         * player overwrites it with the true length once it decodes the moov (diskOS shows the player's
         * live duration, not this value). */
        const char *sql =
            "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DURATION,IS_CUE,IS_ISO,IS_DSD,OFFSET) "
            "SELECT " XSTR(DISKOS_RSV_LISTID) ",PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,"
            "(CASE WHEN DURATION>0 THEN DURATION ELSE 86400000 END),0,0,0,0 FROM BOOKS WHERE PATH=?1;";
        if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) == SQLITE_OK){
            sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
            if(sqlite3_step(st) != SQLITE_DONE) ok = 0;
            sqlite3_finalize(st);
        } else ok = 0;
        if(ok && sqlite3_changes(d) == 0){   /* book not in SONG (books live outside SONG) -> bare-path row (self-contained; the player only needs PATH to decode) */
            sqlite3_stmt *s2;
            if(sqlite3_prepare_v2(d, "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,NAME,TITLE,DURATION) VALUES ("
                                     XSTR(DISKOS_RSV_LISTID) ",?1,?1,?1,86400000);", -1, &s2, NULL) == SQLITE_OK){
                sqlite3_bind_text(s2, 1, path, -1, SQLITE_STATIC);
                if(sqlite3_step(s2) != SQLITE_DONE) ok = 0;
                sqlite3_finalize(s2);
            } else ok = 0;
        }
    }
    if(ok && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK) return 1;
    sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);   /* a failed COMMIT (e.g. reader BUSY) must not leave the txn open with the membership half-applied */
    return 0;
}

/* Reserved-slot playback for a user PLAYLIST. diskOS's type-5 play always resolves to seq 0, so before
 * the reservation a playlist play hit the wrong list, and after it hit the reserved BOOK slot. This
 * copies the playlist's members INTO the reserved slot so seq 0 plays the playlist isolated (and also
 * finally makes diskOS playlist playback work). Non-zero DURATION per row (0 -> placeholder) so the
 * player doesn't instant-skip a 0-length CUSTOM_PLAYLIST member. Returns the member count, 0 on fail/empty. */
int mdb_reserved_slot_set_playlist(long pid){
    sqlite3 *d = db(); if(!d) return 0;
    if(sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) != SQLITE_OK) return 0;
    int ok = 1, n = 0;
    if(sqlite3_exec(d, "INSERT OR IGNORE INTO CUSTOM_PLAYLIST_INDEX (LIST_ID,LIST_NAME,M3U_PATH) VALUES ("
                       XSTR(DISKOS_RSV_LISTID) ",'diskos-book','');", 0, 0, 0) != SQLITE_OK) ok = 0;
    if(ok && sqlite3_exec(d, "DELETE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=" XSTR(DISKOS_RSV_LISTID) ";", 0, 0, 0) != SQLITE_OK) ok = 0;
    if(ok){
        sqlite3_stmt *st;
        /* IS_M3U/M3U_PATH carried over as in mdb_plan_materialize, where the table has them (V2.40+) */
        const char *sql = playlist_has_m3u_cols(d) ?
            "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,ALBUM_ARTIST,"
            "IS_M3U,M3U_PATH) "
            "SELECT " XSTR(DISKOS_RSV_LISTID) ",PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,"
            "(CASE WHEN DURATION>0 THEN DURATION ELSE 86400000 END),ALBUM_ARTIST,IFNULL(IS_M3U,0),IFNULL(M3U_PATH,'') "
            "FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?1 ORDER BY ID;" :
            "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,ALBUM_ARTIST) "
            "SELECT " XSTR(DISKOS_RSV_LISTID) ",PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,"
            "(CASE WHEN DURATION>0 THEN DURATION ELSE 86400000 END),ALBUM_ARTIST "
            "FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?1 "
            /* copied in the playlist's own order (mdb_playlist_songs: the order its songs were added - an M3U's file
             * order). The player reads the slot with no ORDER BY: without a UNIQUE(PLAYLIST_ID,PATH,TRACK) index (the
             * owner's V2.57 DB) that is rowid = this insertion order; with one it is PATH order, and
             * mdb_reserved_slot_player_pos maps the tapped row onto it. */
            "ORDER BY ID;";
        if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) == SQLITE_OK){
            sqlite3_bind_int64(st, 1, (sqlite3_int64)pid);
            if(sqlite3_step(st) != SQLITE_DONE) ok = 0; else n = sqlite3_changes(d);
            sqlite3_finalize(st);
        } else ok = 0;
    }
    if(ok && n > 0 && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK) return n;
    sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    return 0;
}
/* The 1-based position, in the player's own read of the reserved slot, of display row `row` (1-based, the playlist's
 * own order) of playlist `pid`; 0 if it can't be established (the caller then refuses rather than guess). The read has
 * the player's shape (no ORDER BY), so a DB with the stock UNIQUE(PLAYLIST_ID,PATH,TRACK) index - read in PATH order -
 * still starts the song that was tapped. A song listed twice (possible only without that index) is matched by
 * occurrence: the 2nd "A" on screen is the 2nd "A" in the slot. */
int mdb_reserved_slot_player_pos(long pid, int row){
    sqlite3 *d = db(); if(!d || pid <= 0 || row < 1) return 0;
    sqlite3_stmt *st; char *path = NULL; sqlite3_int64 track = 0; int occ = 0;
    /* the tapped row's identity, and which occurrence of it (among rows 1..row) it is */
    if(sqlite3_prepare_v2(d, "SELECT PATH, IFNULL(TRACK,0), (SELECT COUNT(*) FROM CUSTOM_PLAYLIST c2 WHERE c2.PLAYLIST_ID=?1 "
                             "AND c2.PATH=c.PATH AND IFNULL(c2.TRACK,0)=IFNULL(c.TRACK,0) AND c2.ID<=c.ID) "
                             "FROM CUSTOM_PLAYLIST c WHERE PLAYLIST_ID=?1 ORDER BY ID LIMIT 1 OFFSET ?2;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid); sqlite3_bind_int(st, 2, row - 1);
    if(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)){
        path = strdup((const char *)sqlite3_column_text(st, 0));   /* full length: no truncated compare */
        track = sqlite3_column_int64(st, 1); occ = sqlite3_column_int(st, 2);
    }
    sqlite3_finalize(st);
    if(!path || occ < 1){ free(path); return 0; }
    int pos = 0, n = 0, rc = SQLITE_ERROR;
    if(sqlite3_prepare_v2(d, "SELECT PATH,TRACK,IS_CUE,IS_ISO FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=" XSTR(DISKOS_RSV_LISTID) ";",
                          -1, &st, NULL) == SQLITE_OK){
        while((rc = sqlite3_step(st)) == SQLITE_ROW){
            n++;
            const unsigned char *p = sqlite3_column_text(st, 0);
            if(p && !strcmp((const char *)p, path) && sqlite3_column_int64(st, 1) == track && --occ == 0){ pos = n; break; }
        }
        sqlite3_finalize(st);
    }
    free(path);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? pos : 0;
}

/* One-time migration: move any .m4b rows left in SONG (a pre-BOOKS build, or a device whose non-empty
 * DB skipped the startup rescan) into BOOKS, so books leave the music queue even without a rescan.
 * Idempotent + guarded so it's a no-op once BOOKS holds them. Safe to call at every startup. */
/* Returns 1 when books are (now) out of every music-facing store - either nothing needed moving, or the
 * relocation committed. Returns 0 ONLY when the state could not be established or the move failed (DB null,
 * detection query errored, BEGIN/COMMIT lost a lock race) - so the caller can RETRY rather than leave a
 * .m4b sitting in the stock player's unfiltered music queue. A failed detection is NOT treated as
 * "already clean" (that was the old bug: it silently returned success and left books in SONG). */
int mdb_migrate_books(void){
    /* Runs regardless of DISKOS_AUDIOBOOKS: a .m4b left in ANY music-facing store (SONG, favourites
     * MY_LOVE, or a non-reserved playlist) leaks into the stock player's music/favourites/playlist
     * queues. Sanitising is safe with the feature off too (books just sit hidden in BOOKS). */
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_exec(d, "CREATE TABLE IF NOT EXISTS BOOKS (ID INTEGER PRIMARY KEY autoincrement,"
        "PATH TEXT UNIQUE, NAME TEXT, TITLE TEXT, ARTIST TEXT, ALBUM TEXT, GENRE TEXT,"
        "DURATION BIGINT, ADD_TIME INT8);", 0, 0, 0);
    sqlite3_stmt *c; int have = 0, detect_ok = 0;
    if(sqlite3_prepare_v2(d,
        "SELECT 1 FROM SONG WHERE lower(PATH) LIKE '%.m4b' "
        "UNION ALL SELECT 1 FROM MY_LOVE WHERE lower(PATH) LIKE '%.m4b' "
        "UNION ALL SELECT 1 FROM CUSTOM_PLAYLIST WHERE lower(PATH) LIKE '%.m4b' AND PLAYLIST_ID<>" XSTR(DISKOS_RSV_LISTID)
        " LIMIT 1;", -1, &c, NULL) == SQLITE_OK){
        int rc = sqlite3_step(c);
        if(rc == SQLITE_ROW){ have = 1; detect_ok = 1; }
        else if(rc == SQLITE_DONE){ have = 0; detect_ok = 1; }   /* proven empty */
        sqlite3_finalize(c);                                     /* else a step error -> detect_ok stays 0 */
    }
    if(!detect_ok) return 0;   /* couldn't establish the state -> report failure so the caller retries */
    if(!have) return 1;        /* no book left in a music-facing store -> already sanitised */
    if(sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) != SQLITE_OK) return 0;
    /* OR IGNORE, not OR REPLACE: a book already in BOOKS keeps its metadata (ADD_TIME, any real
     * DURATION) - a re-index of the same path in SONG carries only DURATION=0 + a fresh ADD_TIME. */
    int ok = sqlite3_exec(d,
        "INSERT OR IGNORE INTO BOOKS(PATH,NAME,TITLE,ARTIST,ALBUM,GENRE,DURATION,ADD_TIME) "
        "SELECT PATH,NAME,TITLE,ARTIST,ALBUM,GENRE,DURATION,ADD_TIME FROM SONG WHERE lower(PATH) LIKE '%.m4b';",0,0,0)==SQLITE_OK
      && sqlite3_exec(d, "DELETE FROM SONG WHERE lower(PATH) LIKE '%.m4b';",0,0,0)==SQLITE_OK
      /* keep a record of the favourites / playlist entries that leave with the books (the music queues must not
       * hold a book, but the user's choice is not simply thrown away: DISKOS_BOOK_MOVED keeps what and where) */
      && sqlite3_exec(d, "CREATE TABLE IF NOT EXISTS DISKOS_BOOK_MOVED(KIND TEXT NOT NULL, LIST_ID INTEGER, PATH TEXT NOT NULL, "
                         "TRACK INTEGER, MOVED_AT INTEGER, UNIQUE(KIND, LIST_ID, PATH, TRACK));",0,0,0)==SQLITE_OK
      /* an earlier build keyed favourites with LIST_ID NULL (UNIQUE ignores NULLs): fold those into the 0 key */
      && sqlite3_exec(d, "UPDATE OR IGNORE DISKOS_BOOK_MOVED SET LIST_ID=0 WHERE LIST_ID IS NULL;",0,0,0)==SQLITE_OK
      && sqlite3_exec(d, "DELETE FROM DISKOS_BOOK_MOVED WHERE LIST_ID IS NULL;",0,0,0)==SQLITE_OK
      && sqlite3_exec(d, "INSERT OR IGNORE INTO DISKOS_BOOK_MOVED(KIND,LIST_ID,PATH,TRACK,MOVED_AT) "
                         "SELECT 'favourite',0,PATH,IFNULL(TRACK,0),strftime('%s','now') FROM MY_LOVE WHERE lower(PATH) LIKE '%.m4b';",0,0,0)==SQLITE_OK
      && sqlite3_exec(d, "INSERT OR IGNORE INTO DISKOS_BOOK_MOVED(KIND,LIST_ID,PATH,TRACK,MOVED_AT) "
                         "SELECT 'playlist',PLAYLIST_ID,PATH,IFNULL(TRACK,0),strftime('%s','now') FROM CUSTOM_PLAYLIST "
                         "WHERE lower(PATH) LIKE '%.m4b' AND PLAYLIST_ID<>" XSTR(DISKOS_RSV_LISTID) ";",0,0,0)==SQLITE_OK
      && sqlite3_exec(d, "DELETE FROM MY_LOVE WHERE lower(PATH) LIKE '%.m4b';",0,0,0)==SQLITE_OK
      && sqlite3_exec(d, "DELETE FROM CUSTOM_PLAYLIST WHERE lower(PATH) LIKE '%.m4b' AND PLAYLIST_ID<>" XSTR(DISKOS_RSV_LISTID) ";",0,0,0)==SQLITE_OK;
    if(ok && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK) return 1;
    sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);   /* a failed COMMIT (reader BUSY) must not leave the txn open on the persistent connection */
    return 0;
}

/* Remove one favourite by its MY_LOVE.ID (mdb_favorites love_id): exact, and works for a favourite whose song
 * has left SONG. Direct DB delete - the player re-reads MY_LOVE on demand, so refreshing the list reflects it. */
int mdb_unfavorite(int love_id){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "DELETE FROM MY_LOVE WHERE ID=?1;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, love_id);
    int rc = sqlite3_step(st);
    int changed = sqlite3_changes(d);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && changed > 0;   /* true only if a MY_LOVE row was removed */
}

/* ---- custom playlists (PLAYLIST_INFO + CUSTOM_PLAYLIST) ------------------ */
/* The player builds a playlist by copying SONG rows into CUSTOM_PLAYLIST keyed
 * by a PLAYLIST_ID; PLAYLIST_INFO holds the names. We manage both directly. */
#define PL_COLS "PLAYLIST_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,NAME_CODE,TITLE_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,ADD_TIME,SAMPLE_RATE,BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST,ALBUM_ARTIST_CODE"
#define PL_SRC  "PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,NAME_CODE,TITLE_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,ADD_TIME,SAMPLE_RATE,BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST,ALBUM_ARTIST_CODE"

/* Only rows the playlist does not hold yet. The owner's V2.57 DB has NO UNIQUE(PLAYLIST_ID,PATH,TRACK) index, so OR IGNORE
 * alone would insert duplicates; the callers also GROUP BY (PATH,TRACK) so one insert cannot repeat a source row. ?1 = pid. */
#define PL_NODUP "AND NOT EXISTS (SELECT 1 FROM CUSTOM_PLAYLIST c WHERE c.PLAYLIST_ID=?1 AND c.PATH=S.PATH AND IFNULL(c.TRACK,0)=IFNULL(S.TRACK,0))"
/* M3U comment line naming the CUE/ISO track of the entry that follows it (see mdb_playlist_export / import_m3u_file). */
#define M3U_TRACK_TAG "#DISKOS-TRACK:"

/* Create a new playlist; returns its id (>0) or 0 on failure. */
/* run a one-row, one-column integer query. Sets *ok=1 and returns the value on success; *ok=0 and
 * returns 0 on any failure (prepare error, no row) - callers treat that as a hard error. */
static sqlite3_int64 mdb_scalar_i64(sqlite3 *d, const char *sql, int *ok){
    sqlite3_stmt *st; sqlite3_int64 v = 0; *ok = 0;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    if(sqlite3_step(st) == SQLITE_ROW){ v = sqlite3_column_int64(st, 0); *ok = 1; }
    sqlite3_finalize(st);
    return v;
}

/* Allocate the next monotone export UID (>=1) and advance the persisted counter. The caller MUST already
 * be inside a transaction (SAVEPOINT or BEGIN), so the read+bump is atomic. Returns 0 on any failure or
 * overflow. A UID handed out here is never reused for the life of the DB.
 *
 * THREADING: the read and the bump are two calls on the shared g_db connection. That is safe only because
 * playlist creation/import is UI-thread-only (mdb_playlist_create from npmenus, mdb_import_m3u_* from
 * settings - both LVGL handlers on the one UI thread; the scanner thread never creates playlists). This is
 * not a new constraint: mdb_playlist_create already opens a SAVEPOINT on g_db, and SQLite cannot run two
 * transactions on one connection, so concurrent creation was never supported. Any future off-thread
 * creator must serialise the WHOLE create/allocate transaction, not rely on FULLMUTEX (which serialises
 * single calls, not transactions). */
static long long diskos_next_export_uid(sqlite3 *d){
    /* Ensure the export tables exist even if db()'s startup DDL lost a busy-timeout race (it ignores its
     * CREATE results). Every UID assignment funnels through here first, so this guarantees a create never
     * fails permanently just because the one-time init DDL was starved. IF NOT EXISTS is a cheap no-op once
     * they exist; it runs inside the caller's write transaction. */
    if(sqlite3_exec(d, "CREATE TABLE IF NOT EXISTS DISKOS_META(K TEXT PRIMARY KEY, V INTEGER NOT NULL);", 0, 0, 0) != SQLITE_OK) return 0;
    if(sqlite3_exec(d, "CREATE TABLE IF NOT EXISTS DISKOS_PL_EXPORT(PID INTEGER PRIMARY KEY, UID INTEGER NOT NULL);", 0, 0, 0) != SQLITE_OK) return 0;
    int ok = 0;
    sqlite3_int64 next = mdb_scalar_i64(d,
        "SELECT COALESCE((SELECT V FROM DISKOS_META WHERE K='pl_export_seq'),1);", &ok);
    if(!ok || next < 1 || next >= 0x7fffffffffffffffLL) return 0;
    char sql[96];
    snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO DISKOS_META(K,V) VALUES('pl_export_seq',%lld);", (long long)next + 1);
    if(sqlite3_exec(d, sql, 0, 0, 0) != SQLITE_OK) return 0;
    return (long long)next;
}
/* Bind pid -> an already-allocated UID (overwriting any stale row a reused pid inherited). Caller in a
 * txn. Returns 1 on success. */
static int diskos_assign_export_uid_val(sqlite3 *d, sqlite3_int64 pid, long long uid){
    if(uid <= 0) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "INSERT OR REPLACE INTO DISKOS_PL_EXPORT(PID,UID) VALUES(?,?);", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)uid);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}
/* Allocate a fresh UID and bind it to pid. Caller in a txn. Returns 1 on success. */
static int diskos_assign_export_uid(sqlite3 *d, sqlite3_int64 pid){
    return diskos_assign_export_uid_val(d, pid, diskos_next_export_uid(d));
}

/* ---- stock playlist registry (CUSTOM_PLAYLIST_INDEX) ------------------------------------------------------
 * Stock V2.57 names its playlists in CUSTOM_PLAYLIST_INDEX(LIST_ID, LIST_NAME), keyed by the same PLAYLIST_ID as
 * the shared CUSTOM_PLAYLIST membership; diskOS names them in PLAYLIST_INFO(ID, NAME). Device-checked 2026-09-27:
 * the player's migration filled the index with placeholders "custom list <id>" for the playlists that existed then,
 * and diskOS playlists made later were missing from it - so stock showed placeholder names and lost two lists.
 * diskOS now keeps the two in step (same id in both): its create/rename/delete write both, and
 * mdb_playlist_sync_registry adopts stock-only playlists and fills the index's gaps and placeholders. Hidden diskOS
 * playlists (a leading 0x01 in the name, e.g. the book scope) and the reserved slot (LIST_ID < 0) never cross over.
 * Firmware without the index (no table) is left alone. */
static int stock_index_present(sqlite3 *d){
    int ok; sqlite3_int64 n = mdb_scalar_i64(d,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='CUSTOM_PLAYLIST_INDEX' COLLATE NOCASE;", &ok);
    return ok && n > 0;
}
static int hidden_playlist_name(const char *name){ return name && name[0] == 0x01; }
/* drop a UTF-8 sequence cut short at the end of s (a copy into a fixed buffer can split an emoji) */
static void utf8_clip(char *s){
    size_t n = strlen(s), i = n;
    while(i > 0 && ((unsigned char)s[i-1] & 0xC0) == 0x80) i--;          /* back over continuation bytes */
    if(i == 0) return;
    unsigned char lead = (unsigned char)s[i-1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if(lead >= 0xC0 && n - (i - 1) < need) s[i-1] = 0;                    /* incomplete: remove it */
}
/* set the stock index name for `pid` (insert its row when missing). 1 = done or nothing to do. */
static int stock_index_put(sqlite3 *d, sqlite3_int64 pid, const char *name){
    if(pid <= 0 || hidden_playlist_name(name) || !stock_index_present(d)) return 1;
    /* update-then-insert, not ON CONFLICT: only V2.57's schema (LIST_ID UNIQUE) is confirmed */
    sqlite3_stmt *st; int ok = 0, changed = 0;
    if(sqlite3_prepare_v2(d, "UPDATE CUSTOM_PLAYLIST_INDEX SET LIST_NAME=?2 WHERE LIST_ID=?1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int64(st, 1, pid);
        sqlite3_bind_text(st, 2, name ? name : "", -1, SQLITE_STATIC);
        ok = sqlite3_step(st) == SQLITE_DONE; changed = sqlite3_changes(d);
        sqlite3_finalize(st);
    }
    if(ok && changed == 0){
        ok = 0;
        if(sqlite3_prepare_v2(d, "INSERT INTO CUSTOM_PLAYLIST_INDEX (LIST_ID,LIST_NAME) VALUES (?1,?2);", -1, &st, NULL) == SQLITE_OK){
            sqlite3_bind_int64(st, 1, pid);
            sqlite3_bind_text(st, 2, name ? name : "", -1, SQLITE_STATIC);
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
        }
    }
    /* now in both registries: remember it, so a later stock-side deletion is followed, not undone (sync step -1) */
    if(ok){
        char q[200];
        snprintf(q, sizeof q, "INSERT OR REPLACE INTO DISKOS_PL_SYNCED(PID, MEMBERS) VALUES(%lld, "
                              "(SELECT COUNT(*) FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=%lld));", (long long)pid, (long long)pid);
        ok = sqlite3_exec(d, "CREATE TABLE IF NOT EXISTS DISKOS_PL_SYNCED(PID INTEGER PRIMARY KEY, MEMBERS INTEGER NOT NULL DEFAULT 0);",
                          0, 0, 0) == SQLITE_OK;
        if(ok) sqlite3_exec(d, "ALTER TABLE DISKOS_PL_SYNCED ADD COLUMN MEMBERS INTEGER NOT NULL DEFAULT 0;", 0, 0, 0);
        ok = ok && sqlite3_exec(d, q, 0, 0, 0) == SQLITE_OK;
    }
    return ok;
}
static int stock_index_drop(sqlite3 *d, sqlite3_int64 pid){
    if(pid <= 0 || !stock_index_present(d)) return 1;
    sqlite3_stmt *st; int ok = 0;
    if(sqlite3_prepare_v2(d, "DELETE FROM CUSTOM_PLAYLIST_INDEX WHERE LIST_ID=?1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int64(st, 1, pid); ok = sqlite3_step(st) == SQLITE_DONE; sqlite3_finalize(st);
    }
    return ok;
}
/* Reconcile the two registries (idempotent; a no-op once they agree). Atomic: all of it or none. Returns 2 when the
 * stock index exists and now agrees, 1 when there is no stock index (nothing to do yet), 0 on failure. */
int mdb_playlist_sync_registry(void){
    sqlite3 *d = db(); if(!d) return 0;
    if(!stock_index_present(d)) return 1;
    sqlite3_exec(d, "CREATE TABLE IF NOT EXISTS PLAYLIST_INFO (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT, ADD_TIME INT8);", 0, 0, 0);
    int owns_txn = sqlite3_get_autocommit(d);
    if(sqlite3_exec(d, "SAVEPOINT plsync;", 0, 0, 0) != SQLITE_OK) return 0;
    int ok = 1;
    /* -1. follow a deletion made on the stock side. DISKOS_PL_SYNCED remembers which playlists have been in BOTH
     *     registries and how many songs each had then. Stock's own delete removes a playlist's index row AND its
     *     songs; a rebuilt or partly filled index loses rows but never songs. So a remembered playlist that HAD songs
     *     and has now lost both its stock row and every song was deleted in stock's UI: drop diskOS's (now empty)
     *     row so the sync below doesn't resurrect it. An empty playlist missing from the index carries no such
     *     evidence and is simply re-added (nothing is lost either way); songs are never touched here. */
    if(sqlite3_exec(d, "CREATE TABLE IF NOT EXISTS DISKOS_PL_SYNCED(PID INTEGER PRIMARY KEY, MEMBERS INTEGER NOT NULL DEFAULT 0);",
                    0, 0, 0) != SQLITE_OK) ok = 0;
    sqlite3_exec(d, "ALTER TABLE DISKOS_PL_SYNCED ADD COLUMN MEMBERS INTEGER NOT NULL DEFAULT 0;", 0, 0, 0);   /* an earlier build's table */
    const char *gone = "FROM DISKOS_PL_SYNCED y WHERE y.MEMBERS>0 "
        "AND NOT EXISTS (SELECT 1 FROM CUSTOM_PLAYLIST_INDEX i WHERE i.LIST_ID=y.PID) "
        "AND NOT EXISTS (SELECT 1 FROM CUSTOM_PLAYLIST m WHERE m.PLAYLIST_ID=y.PID)";
    char q[600];
    snprintf(q, sizeof q, "DELETE FROM PLAYLIST_INFO WHERE ID IN (SELECT PID %s);", gone);
    if(ok && sqlite3_exec(d, q, 0, 0, 0) != SQLITE_OK) ok = 0;
    snprintf(q, sizeof q, "DELETE FROM DISKOS_PL_SYNCED WHERE PID IN (SELECT PID %s);", gone);
    if(ok && sqlite3_exec(d, q, 0, 0, 0) != SQLITE_OK) ok = 0;
    /* 0. every playlist that has members gets a stock row (the diskOS name, else stock's own placeholder form). This is
     *    the player's own first-start migration done in full: if diskOS writes into a fresh, still-empty index before
     *    the player migrates, the player may treat it as migrated and skip - so nothing may depend on it doing so. */
    if(sqlite3_exec(d,
        "INSERT INTO CUSTOM_PLAYLIST_INDEX (LIST_ID,LIST_NAME) "
        "SELECT m.PLAYLIST_ID, IFNULL((SELECT NAME FROM PLAYLIST_INFO p WHERE p.ID=m.PLAYLIST_ID AND substr(p.NAME,1,1)<>char(1)), "
        "'custom list '||m.PLAYLIST_ID) FROM (SELECT DISTINCT PLAYLIST_ID FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID>0) m "
        "WHERE NOT EXISTS (SELECT 1 FROM CUSTOM_PLAYLIST_INDEX i WHERE i.LIST_ID=m.PLAYLIST_ID) "
        "AND NOT EXISTS (SELECT 1 FROM PLAYLIST_INFO p WHERE p.ID=m.PLAYLIST_ID AND substr(p.NAME,1,1)=char(1));", 0, 0, 0) != SQLITE_OK) ok = 0;
    /* 1. adopt stock-only playlists under their stock id + name, each with its own export identity */
    sqlite3_stmt *sel = NULL;
    if(sqlite3_prepare_v2(d, "SELECT LIST_ID, IFNULL(LIST_NAME,'') FROM CUSTOM_PLAYLIST_INDEX i WHERE LIST_ID>0 "
                             "AND NOT EXISTS (SELECT 1 FROM PLAYLIST_INFO p WHERE p.ID=i.LIST_ID) ORDER BY LIST_ID;",
                          -1, &sel, NULL) != SQLITE_OK) ok = 0;
    int rc = ok ? SQLITE_DONE : SQLITE_ERROR;
    while(ok && (rc = sqlite3_step(sel)) == SQLITE_ROW){
        sqlite3_int64 id = sqlite3_column_int64(sel, 0);
        char nm[MDB_STR]; snprintf(nm, sizeof nm, "%s", colt(sel, 1)); utf8_clip(nm); trim(nm);
        if(!nm[0] || hidden_playlist_name(nm)) snprintf(nm, sizeof nm, "custom list %lld", (long long)id);
        sqlite3_stmt *ins;
        if(sqlite3_prepare_v2(d, "INSERT INTO PLAYLIST_INFO (ID,NAME,ADD_TIME) VALUES (?1,?2,strftime('%s','now'));",
                              -1, &ins, NULL) != SQLITE_OK){ ok = 0; break; }
        sqlite3_bind_int64(ins, 1, id); sqlite3_bind_text(ins, 2, nm, -1, SQLITE_TRANSIENT);
        if(sqlite3_step(ins) != SQLITE_DONE) ok = 0;
        sqlite3_finalize(ins);
        if(ok && !diskos_assign_export_uid(d, id)) ok = 0;
    }
    if(ok && rc != SQLITE_DONE) ok = 0;   /* a read that failed part-way must not commit a partial adoption */
    if(sel) sqlite3_finalize(sel);
    /* 2. give stock every visible diskOS playlist it is missing, and 3. replace stock's migration placeholders
     *    ("custom list <id>") with the diskOS name. A name someone set on the stock side is left as it is. */
    if(ok && sqlite3_exec(d,
        "INSERT INTO CUSTOM_PLAYLIST_INDEX (LIST_ID,LIST_NAME) "   /* M3U_PATH not named: only V2.57's index is confirmed */
        "SELECT ID, NAME FROM PLAYLIST_INFO p WHERE ID>0 AND substr(NAME,1,1)<>char(1) "
        "AND NOT EXISTS (SELECT 1 FROM CUSTOM_PLAYLIST_INDEX i WHERE i.LIST_ID=p.ID);", 0, 0, 0) != SQLITE_OK) ok = 0;
    if(ok && sqlite3_exec(d,
        "UPDATE CUSTOM_PLAYLIST_INDEX SET LIST_NAME=(SELECT NAME FROM PLAYLIST_INFO p WHERE p.ID=LIST_ID) "
        "WHERE LIST_ID>0 AND LIST_NAME='custom list '||LIST_ID AND EXISTS (SELECT 1 FROM PLAYLIST_INFO p "
        "WHERE p.ID=LIST_ID AND p.NAME<>LIST_NAME AND substr(p.NAME,1,1)<>char(1));", 0, 0, 0) != SQLITE_OK) ok = 0;
    /* 4. remember every playlist now in both registries (step -1's evidence for a later stock-side deletion); only a
     *    new row or a changed member count is written, so a sync of agreeing registries writes nothing */
    if(ok && sqlite3_exec(d, "INSERT OR REPLACE INTO DISKOS_PL_SYNCED(PID, MEMBERS) "
                             "SELECT c.ID,c.MEMBERS FROM (SELECT p.ID,COUNT(m.PLAYLIST_ID) AS MEMBERS "
                             "FROM PLAYLIST_INFO p LEFT JOIN CUSTOM_PLAYLIST m ON m.PLAYLIST_ID=p.ID "
                             "WHERE p.ID>0 AND EXISTS (SELECT 1 FROM CUSTOM_PLAYLIST_INDEX i WHERE i.LIST_ID=p.ID) "
                             "GROUP BY p.ID) c WHERE NOT EXISTS (SELECT 1 FROM DISKOS_PL_SYNCED y "
                             "WHERE y.PID=c.ID AND y.MEMBERS=c.MEMBERS);", 0, 0, 0) != SQLITE_OK) ok = 0;
    if(ok && sqlite3_exec(d, "RELEASE plsync;", 0, 0, 0) == SQLITE_OK) return 2;
    sqlite3_exec(d, "ROLLBACK TO plsync;", 0, 0, 0);
    sqlite3_exec(d, "RELEASE plsync;", 0, 0, 0);
    if(owns_txn && !sqlite3_get_autocommit(d)) sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    return 0;
}

long mdb_playlist_create(const char *name){
    if(!name || strlen(name) >= MDB_STR) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    /* Store the CANONICAL (trimmed) name - the same form mdb_playlists shows and export writes. Leading/
     * trailing whitespace in the raw name would otherwise let a display "Rock" (from stored " Rock") miss
     * playlist_id_by_name("Rock") and create a duplicate on re-import. Trim at the source so stored,
     * displayed, exported and idempotency-checked names all agree. */
    char nm[MDB_STR]; snprintf(nm, sizeof nm, "%s", name ? name : ""); trim(nm); name = nm;
    sqlite3_exec(d, "CREATE TABLE IF NOT EXISTS PLAYLIST_INFO "
                    "(ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT, ADD_TIME INT8);", 0, 0, 0);
    /* CUSTOM_PLAYLIST (the membership table) is SHARED with the stock player, keyed by PLAYLIST_ID.
     * PLAYLIST_INFO's AUTOINCREMENT hands back low ids (1,2,3...) that can collide with a STOCK
     * playlist's id. The old code DELETED those "orphan" rows - which ERASED the stock playlist's
     * membership (data loss; reproduced by the 2026-09-05 firmware audit: new diskOS id=1 wiped stock
     * id 1's members). Instead we place the new playlist ABOVE every id known to any playlist table
     * (shared membership, the stock index if present, and our own rows), so it can never touch another
     * playlist's rows and always starts empty. Done atomically in a SAVEPOINT; fail closed (roll back
     * and return 0) on ANY sql error so we never report a colliding/misplaced playlist as created. */
    /* Did THIS call open the outermost transaction? (autocommit on = yes; off = we are nested inside a
     * caller's BEGIN, e.g. import_m3u_file). Determines cleanup: if we own it, a failed RELEASE must be
     * force-closed with a plain ROLLBACK, or the shared connection is left stuck IN a transaction and every
     * later BEGIN/SAVEPOINT (book playback, import) fails. If we are nested, we only unwind our savepoint
     * and leave the outer transaction for its owner. */
    int owns_txn = sqlite3_get_autocommit(d);
    if(sqlite3_exec(d, "SAVEPOINT plcreate;", 0, 0, 0) != SQLITE_OK) return 0;
    sqlite3_stmt *st;
    sqlite3_int64 id = 0;
    int ok = 1;

    if(sqlite3_prepare_v2(d, "INSERT INTO PLAYLIST_INFO (NAME,ADD_TIME) VALUES (?, strftime('%s','now'));",
                          -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_bind_text(st, 1, name?name:"", -1, SQLITE_STATIC) != SQLITE_OK) ok = 0;
        else if(sqlite3_step(st) != SQLITE_DONE) ok = 0;
        sqlite3_finalize(st);
    } else ok = 0;

    if(ok){
        id = sqlite3_last_insert_rowid(d);
        int q;
        sqlite3_int64 hi = mdb_scalar_i64(d, "SELECT COALESCE(MAX(PLAYLIST_ID),0) FROM CUSTOM_PLAYLIST;", &q);
        if(!q) ok = 0;
        if(ok){
            /* other diskOS playlists (EXCLUDE the row we just inserted, else id<=hi is always true and we
             * renumber every time). AUTOINCREMENT already keeps our own ids distinct, but we UPDATE ids
             * upward without advancing the sequence, so a later low id must still clear existing rows. */
            char qs[96]; snprintf(qs, sizeof qs, "SELECT COALESCE(MAX(ID),0) FROM PLAYLIST_INFO WHERE ID<>%lld;", (long long)id);
            sqlite3_int64 v = mdb_scalar_i64(d, qs, &q);
            if(!q) ok = 0; else if(v > hi) hi = v;
        }
        if(ok){
            /* The stock playlist index. Empty stock playlists have NO membership rows, so only this
             * table reveals their ids. If we cannot READ an index that exists, we cannot establish
             * collision safety -> fail closed. A genuinely-absent table is safe to skip. */
            int has;
            sqlite3_int64 t = mdb_scalar_i64(d,
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='CUSTOM_PLAYLIST_INDEX' COLLATE NOCASE;", &has);
            if(!has) ok = 0;
            else if(t > 0){
                int q2; sqlite3_int64 v = mdb_scalar_i64(d, "SELECT COALESCE(MAX(LIST_ID),0) FROM CUSTOM_PLAYLIST_INDEX;", &q2);
                if(!q2) ok = 0; else if(v > hi) hi = v;
            }
        }
        if(ok && id <= hi){
            if(hi >= 0x7fffffffffffffffLL) ok = 0;   /* overflow guard */
            else {
                sqlite3_int64 safe = hi + 1;
                if(sqlite3_prepare_v2(d, "UPDATE PLAYLIST_INFO SET ID=? WHERE ID=?;", -1, &st, NULL) == SQLITE_OK){
                    sqlite3_bind_int64(st, 1, safe); sqlite3_bind_int64(st, 2, id);
                    if(sqlite3_step(st) != SQLITE_DONE || sqlite3_changes(d) != 1) ok = 0;
                    sqlite3_finalize(st);
                } else ok = 0;
                if(ok) id = safe;
            }
        }
        if(ok && id > 0x7fffffffLL) ok = 0;   /* the API returns long (32-bit here); refuse to narrow wrongly */
        /* bind this (possibly pid-reused) playlist to a FRESH monotone export UID, overwriting any stale
         * DISKOS_PL_EXPORT row a recycled pid inherited. Inside the savepoint, so it is atomic with the
         * create; fail closed so every diskOS playlist has a reuse-proof export identity. */
        if(ok && !diskos_assign_export_uid(d, id)) ok = 0;
        if(ok && !stock_index_put(d, id, name)) ok = 0;   /* stock sees it under the same id + name */
    }

    /* commit only if RELEASE actually succeeds (a busy DB can fail it, leaving the txn open) */
    if(ok && sqlite3_exec(d, "RELEASE plcreate;", 0, 0, 0) == SQLITE_OK) return (long)id;
    sqlite3_exec(d, "ROLLBACK TO plcreate;", 0, 0, 0);
    sqlite3_exec(d, "RELEASE plcreate;", 0, 0, 0);
    /* ROLLBACK TO does not end the transaction, and the RELEASE above can also fail under a reader lock. If
     * we opened the outer transaction, force it closed so the shared connection isn't left mid-transaction
     * (which would break every later BEGIN/SAVEPOINT). If we're nested, leave the outer txn to its owner. */
    if(owns_txn && !sqlite3_get_autocommit(d)) sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    return 0;
}
/* Copy a song (by path, from the SONG table) into a playlist. 1 on success. */
/* Add one SONG-resident path to a custom playlist. Returns 1 if a row was actually inserted,
 * 0 otherwise. `hard_err` (optional) distinguishes a real DB failure (prepare/step error) from a
 * benign 0-row result (path not in SONG, or already a member): *hard_err is set to 1 ONLY on a real
 * failure, so an importer can tell "nothing to add" from "the DB write failed mid-import". */
int mdb_playlist_add_song_ex(long pid, const char *path, int *hard_err){
    if(hard_err) *hard_err = 0;
    if(pid<=0 || !path || !path[0]) return 0;
    sqlite3 *d = db(); if(!d){ if(hard_err) *hard_err = 1; return 0; }
    sqlite3_stmt *st;
    const char *sql = "INSERT OR IGNORE INTO CUSTOM_PLAYLIST (" PL_COLS ") "
                      "SELECT ?1," PL_SRC " FROM SONG S WHERE S.PATH=?2 " PL_NODUP " GROUP BY S.PATH,IFNULL(S.TRACK,0) ORDER BY MIN(S.ID);";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK){ if(hard_err) *hard_err = 1; return 0; }
    sqlite3_bind_int64(st, 1, pid);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    if(rc != SQLITE_DONE){ if(hard_err) *hard_err = 1; return 0; }
    /* OR IGNORE + SELECT-from-SONG can succeed (DONE) yet insert 0 rows (path not in
     * SONG, or already present) - report a real add only when a row changed. */
    return (sqlite3_changes(d) > 0) ? 1 : 0;
}
int mdb_playlist_add_song(long pid, const char *path){
    return mdb_playlist_add_song_ex(pid, path, NULL);
}
/* CUE / SACD-ISO sub-tracks share one PATH, so a path alone names the whole file. A playlist row's identity is
 * (PATH, TRACK) - the stock UNIQUE(PLAYLIST_ID,PATH,TRACK) - and the copied IS_CUE/IS_ISO/OFFSET make the player start
 * that track. The functions below name one sub-track exactly. */
/* Identity of the SONG row playing now: `pos_id` is the a2 pos_id (a LIST_SONG_0.ID) and `title` the player's current title.
 * The queue row must match BOTH the path and the title. Returns 1 and fills track/cue/iso
 * only when that queue row is a CUE or ISO sub-track of `path`; 0 for a plain file or anything not established. */
static int song_subtrack_core(const char *path, int pos_id, const char *title, int need_title, int *track, int *is_cue, int *is_iso){
    if(!path || !path[0] || pos_id <= 0) return 0;
    if(need_title && (!title || !title[0])) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st; int r = 0;
    if(sqlite3_prepare_v2(d, "SELECT IFNULL(TRACK,0),IFNULL(IS_CUE,0),IFNULL(IS_ISO,0) FROM LIST_SONG_0 WHERE ID=?1 AND PATH=?2 "
                          "AND (?4=0 OR TITLE=?3 OR NAME=?3);",
                          -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, pos_id); sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, title ? title : "", -1, SQLITE_STATIC); sqlite3_bind_int(st, 4, need_title ? 1 : 0);
    if(sqlite3_step(st) == SQLITE_ROW && (sqlite3_column_int(st, 1) || sqlite3_column_int(st, 2))){
        if(track) *track = sqlite3_column_int(st, 0);
        if(is_cue) *is_cue = sqlite3_column_int(st, 1) != 0;
        if(is_iso) *is_iso = sqlite3_column_int(st, 2) != 0;
        r = 1;
    }
    sqlite3_finalize(st);
    return r;
}
/* Path + queue row only (for display, e.g. Song Info). */
int mdb_song_subtrack(const char *path, int pos_id, int *track, int *is_cue, int *is_iso){
    return song_subtrack_core(path, pos_id, NULL, 0, track, is_cue, is_iso);
}
/* For an ADD: the queue row must match the path AND the player's current title (else the track changed under us). */
int mdb_song_subtrack_verified(const char *path, int pos_id, const char *title, int *track, int *is_cue, int *is_iso){
    return song_subtrack_core(path, pos_id, title, 1, track, is_cue, is_iso);
}
/* 1 if `path` holds more than one DISTINCT (track, cue, iso) identity (a CUE / ISO file; repeated rows of one identity do not count): a path alone then does not name one track. */
int mdb_song_multi(const char *path){
    if(!path || !path[0]) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st; int r = 0;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(DISTINCT IFNULL(TRACK,0)||'/'||IFNULL(IS_CUE,0)||'/'||IFNULL(IS_ISO,0)) FROM SONG WHERE PATH=?1;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    if(sqlite3_step(st) == SQLITE_ROW) r = sqlite3_column_int(st, 0) > 1;
    sqlite3_finalize(st);
    return r;
}
/* Add exactly the (path, track) sub-track (a CUE or ISO row) to a playlist. Returns 1 if a row was inserted; *hard_err as
 * in mdb_playlist_add_song_ex. */
int mdb_playlist_add_subtrack(long pid, const char *path, int track, int *hard_err){
    if(hard_err) *hard_err = 0;
    if(pid<=0 || !path || !path[0]) return 0;
    sqlite3 *d = db(); if(!d){ if(hard_err) *hard_err = 1; return 0; }
    sqlite3_stmt *st;
    const char *sql = "INSERT OR IGNORE INTO CUSTOM_PLAYLIST (" PL_COLS ") "
                      "SELECT ?1," PL_SRC " FROM SONG S WHERE S.PATH=?2 AND IFNULL(S.TRACK,0)=?3 "
                      "AND (IFNULL(S.IS_CUE,0)<>0 OR IFNULL(S.IS_ISO,0)<>0) " PL_NODUP " ORDER BY S.ID LIMIT 1;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK){ if(hard_err) *hard_err = 1; return 0; }
    sqlite3_bind_int64(st, 1, pid); sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC); sqlite3_bind_int(st, 3, track);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    if(rc != SQLITE_DONE){ if(hard_err) *hard_err = 1; return 0; }
    return (sqlite3_changes(d) > 0) ? 1 : 0;
}
/* 1 if the playlist already holds that exact sub-track. */
int mdb_playlist_has_subtrack(long pid, const char *path, int track){
    if(pid<=0 || !path || !path[0]) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT 1 FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=? AND PATH=? AND IFNULL(TRACK,0)=? LIMIT 1;",
                          -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid); sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC); sqlite3_bind_int(st, 3, track);
    int yes = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return yes;
}
/* ---- single-book playback scope --------------------------------------------------------------------
 * A reserved, hidden custom playlist that holds ONLY the currently-playing audiobook. Playing it
 * (list_type 5) isolates the book: the player's queue is the one file, so next/prev/end can't wander
 * into music. The name carries a leading 0x01 so it can't collide with a user playlist and is filtered
 * out of the Playlists view. */
#define BOOK_SCOPE_NAME "\x01""diskos-book"
static long book_scope_pid(void){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st; long pid = 0;
    if(sqlite3_prepare_v2(d, "SELECT ID FROM PLAYLIST_INFO WHERE NAME=? LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, BOOK_SCOPE_NAME, -1, SQLITE_STATIC);
        if(sqlite3_step(st) == SQLITE_ROW) pid = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if(pid <= 0) pid = mdb_playlist_create(BOOK_SCOPE_NAME);
    return pid;
}
/* Point the reserved book-scope playlist at exactly `path`, returning its id (0 on failure). */
long mdb_book_scope_set(const char *path){
    if(!path || !path[0]) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    long pid = book_scope_pid();
    if(pid <= 0) return 0;
    sqlite3_stmt *st;   /* clear the previous member, then add this book (copied from its SONG row) */
    if(sqlite3_prepare_v2(d, "DELETE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int64(st, 1, pid); sqlite3_step(st); sqlite3_finalize(st);
    }
    if(!mdb_playlist_add_song(pid, path)) return 0;
    return pid;
}

/* Bulk-add every SONG in a group (all songs of an ALBUM/ARTIST/GENRE) to a custom playlist.
 * `col` MUST be one of the whitelisted column names (it is interpolated into SQL, so it can never be
 * user text). Skips duplicates (INSERT OR IGNORE against UNIQUE(PLAYLIST_ID,PATH,TRACK)). Returns the
 * number of rows actually added. */
int mdb_playlist_add_group(long pid, const char *col, const char *val){
    if(pid<=0 || !col || !val || !val[0]) return 0;
    if(strcmp(col,"ALBUM") && strcmp(col,"ARTIST") && strcmp(col,"GENRE")) return 0;   /* whitelist */
    sqlite3 *d = db(); if(!d) return -1;
    char sql[1600];
    snprintf(sql, sizeof sql,
             "INSERT OR IGNORE INTO CUSTOM_PLAYLIST (" PL_COLS ") SELECT ?1," PL_SRC " FROM SONG S WHERE S.%s=?2 " PL_NODUP
             " GROUP BY S.PATH,IFNULL(S.TRACK,0) ORDER BY MIN(S.ID);", col);
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, pid);
    sqlite3_bind_text(st, 2, val, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? sqlite3_changes(d) : -1;
}
/* Add exactly these SONG rows (by SONG.ID: the songs a list shows, so any grouping - artist credits, album artist, a CUE
 * track - is preserved as displayed). Rows already held, and repeats, are skipped. Returns the number added, or -1 if the
 * write failed (nothing is then added: one savepoint). */
int mdb_playlist_add_ids(long pid, const int *ids, int n){
    if(pid <= 0 || !ids || n <= 0) return 0;
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st;
    int owns = sqlite3_get_autocommit(d);
    if(sqlite3_exec(d, "SAVEPOINT pladdids;", 0, 0, 0) != SQLITE_OK) return -1;
    if(sqlite3_prepare_v2(d, "INSERT OR IGNORE INTO CUSTOM_PLAYLIST (" PL_COLS ") SELECT ?1," PL_SRC " FROM SONG S WHERE S.ID=?2 " PL_NODUP ";",
                          -1, &st, NULL) != SQLITE_OK){ sqlite3_exec(d, "ROLLBACK TO pladdids;", 0, 0, 0); sqlite3_exec(d, "RELEASE pladdids;", 0, 0, 0); return -1; }
    int added = 0, ok = 1;
    for(int i = 0; i < n && ok; i++){
        sqlite3_reset(st); sqlite3_bind_int64(st, 1, pid); sqlite3_bind_int(st, 2, ids[i]);
        if(sqlite3_step(st) != SQLITE_DONE) ok = 0; else if(sqlite3_changes(d) > 0) added++;
    }
    sqlite3_finalize(st);
    if(ok && sqlite3_exec(d, "RELEASE pladdids;", 0, 0, 0) == SQLITE_OK) return added;
    sqlite3_exec(d, "ROLLBACK TO pladdids;", 0, 0, 0);
    sqlite3_exec(d, "RELEASE pladdids;", 0, 0, 0);
    if(owns && !sqlite3_get_autocommit(d)) sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    return -1;
}
/* 1 if the playlist already contains this song path. */
int mdb_playlist_has_song(long pid, const char *path){
    if(pid<=0 || !path || !path[0]) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT 1 FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=? AND PATH=? LIMIT 1;",
                          -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    int yes = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return yes;
}
/* Remove the song at 1-based ORDINAL position from a custom playlist. The ordinal matches
 * mdb_playlist_songs()'s display order (ORDER BY ID - the playlist's own order), so the UI can remove by row
 * index; the row is deleted by its ID. Returns 1 if a row was deleted. */
int mdb_playlist_remove_at(long pid, int position){
    if(pid<=0 || position<=0) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    const char *sql =
        "DELETE FROM CUSTOM_PLAYLIST WHERE ID = ("
        "SELECT ID FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?1 ORDER BY ID LIMIT 1 OFFSET ?2);";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid);
    sqlite3_bind_int(st, 2, position-1);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return (rc == SQLITE_DONE && sqlite3_changes(d) > 0) ? 1 : 0;
}
int sd_write_begin(void); void sd_write_end(void);   /* SD-write ownership guard (main.c, M18) */
/* Sanitise a playlist name into a safe filename base (keep alnum/space/-/_; others -> _). */
static void pl_sanitize(const char *name, char *out, int cap){
    int j=0;
    for(const char *s = (name && name[0]) ? name : "playlist"; *s && j < cap - 1; s++){
        char c = *s;
        out[j++] = ((c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||c==' '||c=='-'||c=='_') ? c : '_';
    }
    out[j] = 0;
    if(!out[0]){ out[0]='p'; out[1]=0; }
}
/* Persist a random ownership token as well as the filename counter: a rebuilt DB can reuse that counter.
 * REPLACE on playlist creation clears the token, so even a recycled pid gets a fresh identity. */
static int pl_export_token(sqlite3 *d, long pid, long long uid, char *token){
    unsigned char bytes[16];
    char fresh[33];
    int fd = open("/dev/urandom", O_RDONLY);
    if(fd < 0) return 0;
    size_t done = 0;
    while(done < sizeof bytes){
        ssize_t n = read(fd, bytes + done, sizeof bytes - done);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0){ close(fd); return 0; }
        done += (size_t)n;
    }
    if(close(fd) != 0) return 0;
    for(size_t i = 0; i < sizeof bytes; i++){
        fresh[2*i] = "0123456789abcdef"[bytes[i] >> 4];
        fresh[2*i+1] = "0123456789abcdef"[bytes[i] & 15];
    }
    fresh[32] = 0;
    const char *sql = "UPDATE DISKOS_PL_EXPORT SET TOKEN=?3 WHERE PID=?1 AND UID=?2 AND TOKEN IS NULL;";
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK){
        /* Upgrade the old two-column table on the first export. Other failures refuse the export. */
        if(sqlite3_exec(d, "ALTER TABLE DISKOS_PL_EXPORT ADD COLUMN TOKEN TEXT;", 0, 0, 0) != SQLITE_OK) return 0;
        if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    }
    sqlite3_bind_int64(st, 1, pid); sqlite3_bind_int64(st, 2, uid);
    sqlite3_bind_text(st, 3, fresh, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    if(rc != SQLITE_DONE) return 0;
    if(sqlite3_prepare_v2(d, "SELECT TOKEN FROM DISKOS_PL_EXPORT WHERE PID=? AND UID=?;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid); sqlite3_bind_int64(st, 2, uid);
    int ok = 0;
    if(sqlite3_step(st) == SQLITE_ROW){
        const char *s = colt(st, 0);
        if(strlen(s) == 32 && strspn(s, "0123456789abcdef") == 32){ memcpy(token, s, 33); ok = 1; }
    }
    sqlite3_finalize(st);
    return ok;
}
/* Only the exact diskOS ownership directive proves this is our playlist's previous export. */
static int pl_export_is_ours(const char *path, const char *marker){
    FILE *f = fopen(path, "r"); if(!f) return 0;   /* unreadable -> treat as not-ours, don't clobber */
    char line[256]; int ok = 0, i = 0;
    while(i < 8 && fgets(line, sizeof line, f)){
        if(i == 0 && strcmp(line, "#EXTM3U\n") != 0) break;
        if(strcmp(line, marker) == 0){ ok = 1; break; }
        i++;
    }
    if(ferror(f)) ok = 0;
    fclose(f);
    return ok;
}
static int pl_export_available(const char *path, const char *marker){
    if(access(path, F_OK) != 0 && errno == ENOENT) return 1;
    return pl_export_is_ours(path, marker);
}

/* Export via a temporary file + rename under the SD-write guard. outname receives the actual filename. */
static int mdb_playlist_export_leased(long pid, const char *name, char *outname, int cap){
    if(pid<=0) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    char safe[96]; pl_sanitize(name, safe, sizeof safe);
    /* fetch this playlist's export uid; lazily assign one (in its own txn) for a playlist created before
     * this feature existed. If neither read nor assign yields a uid, REFUSE the export rather than fall
     * back to a non-unique name that could clobber another playlist's file. */
    long long uid = 0; int have_uid = 0;
    { sqlite3_stmt *st;
      if(sqlite3_prepare_v2(d, "SELECT UID FROM DISKOS_PL_EXPORT WHERE PID=?;", -1, &st, NULL) == SQLITE_OK){
          sqlite3_bind_int64(st, 1, pid);
          if(sqlite3_step(st) == SQLITE_ROW){ uid = sqlite3_column_int64(st, 0); have_uid = 1; }
          sqlite3_finalize(st);
      } }
    if(!have_uid && sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) == SQLITE_OK){
        long long u = diskos_next_export_uid(d);
        int okk = (u > 0) && diskos_assign_export_uid_val(d, pid, u);
        if(okk && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK){ uid = u; have_uid = 1; }
        else sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    }
    if(!have_uid) return 0;
    char token[33], marker[96];
    if(!pl_export_token(d, pid, uid, token)) return 0;
    snprintf(marker, sizeof marker, "#DISKOS-PLAYLIST:%lld:%s\n", uid, token);
    char base[160];   /* safe(<=95) + "-" + uid (<=19 digits) [+ "_N" disambiguator], no truncation of the unique suffix */
    snprintf(base, sizeof base, "%s-%lld", safe, uid);
    char path[200]; snprintf(path, sizeof path, "/tmp/sdcard/%s.m3u", base);
    /* Reuse this playlist's own prior export before looking for a free name, even if an earlier
     * collision disappeared. Never overwrite a user's file or a stale export from a rebuilt DB. */
    {
        int found = 0;
        for(int pass = 0; pass < 2 && !found; pass++){
            for(int nsfx = 1; nsfx <= 999; nsfx++){
                char nb[160], cand[200];
                if(nsfx == 1) snprintf(nb, sizeof nb, "%s", base);
                else snprintf(nb, sizeof nb, "%.116s_%d", base, nsfx);
                snprintf(cand, sizeof cand, "/tmp/sdcard/%s.m3u", nb);
                if(pass == 0 ? pl_export_is_ours(cand, marker)
                             : pl_export_available(cand, marker)){
                    snprintf(base, sizeof base, "%s", nb);
                    snprintf(path, sizeof path, "%s", cand);
                    found = 1; break;
                }
            }
        }
        if(!found) return 0;   /* every candidate belongs to someone else -> refuse rather than overwrite a user file */
    }
    /* The temp is created EXCLUSIVELY under a random name (mkstemp: O_CREAT|O_EXCL), so it can never be an
     * existing user file: a fixed "<name>.m3u.part" would have been truncated by fopen("w") and then removed
     * on failure if the card already held a file with that name. Cleanup below only ever removes this file. */
    char tmp[64];  snprintf(tmp, sizeof tmp, "/tmp/sdcard/.diskos-export-XXXXXX");
    if(!sd_write_begin()) return 0;                 /* card host-owned or an export is pending -> skip */
    int tfd = mkstemp(tmp);                          /* write a temp; swap over the real file only when complete */
    if(tfd < 0){ sd_write_end(); return 0; }
    FILE *f = fdopen(tfd, "w");
    if(!f){ close(tfd); unlink(tmp); sd_write_end(); return 0; }
    int ok = 1;
    if(fprintf(f, "#EXTM3U\n") < 0) ok = 0;
    if(fputs(marker, f) == EOF) ok = 0;
    /* Record the real playlist name in a standard extended-M3U directive (a comment, so any other player
     * ignores it). Import reads it back to restore the true name and stay idempotent, so re-importing an
     * exported "<name>-<uid>.m3u" updates/skips the same playlist instead of creating a "<name>-<uid>"
     * duplicate. Strip any newline from the name so the directive stays one line. */
    if(name && name[0]){
        char nm1[160]; int k=0;
        for(const char *s=name; *s && k<(int)sizeof nm1-1; s++){ char c=*s; if(c!='\r'&&c!='\n') nm1[k++]=c; }
        nm1[k]=0;
        if(nm1[0] && fprintf(f, "#PLAYLIST:%s\n", nm1) < 0) ok = 0;
    }
    sqlite3_stmt *st = NULL;
    if(sqlite3_prepare_v2(d, "SELECT PATH,IFNULL(TRACK,0),(IFNULL(IS_CUE,0)<>0 OR IFNULL(IS_ISO,0)<>0) FROM CUSTOM_PLAYLIST "
                             "WHERE PLAYLIST_ID=? ORDER BY ID;", -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_bind_int64(st, 1, pid) == SQLITE_OK){
            int rc;
            while((rc = sqlite3_step(st)) == SQLITE_ROW){
                const char *p = colt(st,0);
                /* A CUE / SACD-ISO sub-track is the file plus a track number. Other players only know the file, so the
                 * number goes in a comment line (ignored by them) that our import reads back to add just that track. */
                if(p && p[0] && sqlite3_column_int(st,2) && fprintf(f, "%s%d\n", M3U_TRACK_TAG, sqlite3_column_int(st,1)) < 0) ok = 0;
                if(p && p[0] && fprintf(f, "%s\n", p) < 0) ok = 0;
            }
            if(rc != SQLITE_DONE) ok = 0;   /* a step error -> the export is incomplete, not a success */
        } else { ok = 0; }
        sqlite3_finalize(st);               /* always finalize a prepared stmt (bind-fail path too) */
    } else { ok = 0; }                      /* prepare failure -> not a valid export */
    if(ferror(f)) ok = 0;                 /* any earlier stream write error */
    if(fflush(f) != 0) ok = 0;
    /* fsync the data to the card before the rename makes it visible, so a yanked/remounted SD can't
     * surface a rename pointing at unflushed content - and so a deferred writeback error (EIO) is caught
     * HERE rather than being swallowed by fclose. A driver that simply doesn't implement fsync
     * (EINVAL/ENOSYS/EOPNOTSUPP) is tolerated; any other error fails the export. */
    if(ok){
        int fd = fileno(f);
        if(fd >= 0 && fsync(fd) != 0 && errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) ok = 0;
    }
    if(fclose(f) != 0) ok = 0;
    /* swap into place only on full success; a failed/partial export leaves any previous good
     * <name>.m3u untouched (writing straight to the real path with "w" would truncate it up front). */
    if(ok && rename(tmp, path) != 0) ok = 0;
    if(ok){
        /* fsync the containing directory so the rename entry itself is durable on a non-journaled
         * card (file data was fsync'd above). Best-effort: an unsupported dir fsync just no-ops. */
        int dfd = open("/tmp/sdcard", O_RDONLY | O_DIRECTORY);
        if(dfd >= 0){ fsync(dfd); close(dfd); }
    }
    if(!ok) unlink(tmp);                             /* only ever our own exclusively-created temp */
    sd_write_end();
    if(ok && outname && cap > 0) snprintf(outname, cap, "%s.m3u", base);
    return ok;
}
/* Rename a playlist. */
/* Write a custom EQ curve into the player's PEQ table (STYLE_PRESET slot). The player
 * reloads bands -> biquad coeffs when 0689 selects this preset. Format captured from the
 * stock UI (RE_CATALOGUE section 3): PARAMS_JSON = 10 band objects, gain/qValue as STRINGS. */
int mdb_set_peq(int style_preset, double master_gain, const char *params_json){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "UPDATE PEQ SET MASTER_GAIN=?, PARAMS_JSON=? WHERE STYLE_PRESET=?;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_double(st, 1, master_gain);
    sqlite3_bind_text(st, 2, params_json?params_json:"", -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, style_preset);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    if(rc != SQLITE_DONE) return 0;
    if(sqlite3_changes(d) == 0){
        /* slot doesn't exist yet -> create it. Name USER slots (STYLE_PRESET 11..20) "USER1".."USER10"
         * to match the stock player's labels; anything else stays "Custom". */
        char nm[16] = "Custom";
        if(style_preset >= 11 && style_preset <= 20) snprintf(nm, sizeof nm, "USER%d", style_preset - 10);
        if(sqlite3_prepare_v2(d, "INSERT INTO PEQ (STYLE_NAME, MASTER_GAIN, PARAMS_JSON, STYLE_PRESET) VALUES (?,?,?,?);", -1, &st, NULL) != SQLITE_OK) return 0;
        sqlite3_bind_text(st, 1, nm, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 2, master_gain);
        sqlite3_bind_text(st, 3, params_json?params_json:"", -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 4, style_preset);
        rc = sqlite3_step(st); sqlite3_finalize(st);
        if(rc != SQLITE_DONE) return 0;
    }
    return 1;
}
/* Read a PEQ slot's stored curve (by STYLE_PRESET). Fills *master_out (dB) and up to 10 band
 * gains (integer dB, in band order) parsed from PARAMS_JSON. Returns 1 if the slot row exists,
 * 0 if not (caller treats a missing row as a flat/empty slot). gains_out must hold >=10 ints.
 * Lets the EQ editor show + preserve an un-edited USER slot's existing curve instead of
 * overwriting a stock preset with flat. Integer-only parse (the stored gains are "N.0"). */
/* Read a slot's stored PEQ curve. Returns 1 = found (out filled), 0 = no such slot (out is a real flat
 * curve), -1 = READ FAILED (DB unavailable / prepare error - the true curve is UNKNOWN, out is flat only
 * as a placeholder). The -1 vs 0 split matters: a caller must never persist the placeholder-flat of a
 * failed read over a real stock curve. */
int mdb_get_peq(int style_preset, int *master_out, int *gains_out){
    if(gains_out) for(int i=0;i<10;i++) gains_out[i]=0;
    if(master_out) *master_out=0;
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT MASTER_GAIN, PARAMS_JSON FROM PEQ WHERE STYLE_PRESET=?;", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, style_preset);
    int found = 0, rc = sqlite3_step(st);
    if(rc == SQLITE_ROW){
        found = 1;
        if(master_out) *master_out = sqlite3_column_int(st, 0);   /* stored REAL "N.0" -> int */
        const char *js = (const char*)sqlite3_column_text(st, 1);
        if(gains_out && js){
            int n=0; const char *p=js;
            while(n<10 && (p=strstr(p, "\"gain\"")) != NULL){
                p += 6;                                  /* past the "gain" key */
                while(*p==':'||*p==' '||*p=='"') p++;     /* skip ':' and the opening quote/space */
                int neg=0; if(*p=='-'){ neg=1; p++; }
                int g=0; while(*p>='0'&&*p<='9' && g<100000){ g=g*10+(*p-'0'); p++; }   /* bound: no signed-overflow UB on a garbage-long number */
                gains_out[n++] = neg ? -g : g;           /* integer dB part (caller clamps to -12..12) */
            }
        }
    }
    sqlite3_finalize(st);
    if(!found && rc != SQLITE_DONE) return -1;   /* a step error (BUSY/corrupt) is a read failure, not an empty slot */
    return found;
}

/* Is a PEQ slot's stored curve representable by diskOS's 10-band graphic editor (fixed freqs, peaking
 * filter, Q=0.7, integer dB)? 1 = yes (or empty/absent - nothing to lose); 0 = it uses parametric params
 * (non-zero filterType, a non-0.7 qValue, or a fractional gain) that the graphic editor would FLATTEN, so
 * the caller must not overwrite it; -1 = read failed. Lets the editor preserve a stock parametric preset
 * instead of destroying it (full parametric editing is out of scope until the filterType enum is RE'd). */
/* Parse an INTEGER-VALUED JSON number at p: [-]?digits, optionally an all-zero ".0..." fraction, and NO
 * exponent. Rejects a non-zero fraction (32.5), an exponent (1e-1, 0.7e1), or a malformed number. On success
 * store the value + the end pointer, return 1. This is why the graphic editor (integer dB) can round-trip it. */
/* Parse a jsmn token's text as an INTEGER value ([-]digits, optional all-zero fraction, NO exponent), fully
 * consumed within the token. The token bounds (from a real JSON parse) already validate delimiters/quotes/
 * escapes, so "1 junk", 32.5, 1e-1 and trailing junk are all rejected. */
static int peq_tok_int(const char *js, const jsmntok_t *t, long *out){
    const char *p = js + t->start, *end = js + t->end;
    int neg = 0; if(p < end && *p == '-'){ neg = 1; p++; }
    if(p >= end || !(*p>='0'&&*p<='9')) return 0;
    if(*p == '0' && p+1 < end && p[1]>='0'&&p[1]<='9') return 0;   /* leading zero (032) -> invalid */
    long v = 0; while(p < end && *p>='0'&&*p<='9'){ if(v > 10000000) return 0; v = v*10 + (*p-'0'); p++; }
    if(p < end && *p == '.'){ p++; if(p >= end || !(*p>='0'&&*p<='9')) return 0;   /* "32." -> require a fraction digit */
                              while(p < end && *p>='0'&&*p<='9'){ if(*p != '0') return 0; p++; } }
    if(p != end) return 0;   /* exponent / extra dot / junk inside the token */
    *out = neg ? -v : v; return 1;
}
static int peq_key_is(const char *js, const jsmntok_t *t, const char *s){   /* object key == s (raw, no escapes) */
    int n = t->end - t->start;
    return n == (int)strlen(s) && strncmp(js + t->start, s, (size_t)n) == 0;
}

/* Is a PEQ slot representable by diskOS's 10-band graphic editor? Uses a REAL JSON parse (jsmn) so escaped
 * keys, nested objects, trailing junk, whitespace and quoting are all handled correctly. Graphic REQUIRES:
 * PARAMS_JSON is an array of EXACTLY 10 flat objects; band i has filterType==0 (number), frequency==the fixed
 * GFREQ[i] (number, position-preserving), qValue=="0.7" (string), gain=an integer string in [-12,12]; and
 * MASTER_GAIN is an integer in [-12,12]. Anything else (parametric params, out-of-range gains the editor
 * would clamp, or a structure it can't round-trip) is NOT graphic -> the caller preserves it.
 * The V2.57 snapshot also accepts 0.70/0.71 and requires each position, retaining Q verbatim. */
static int peq_read_graphic(int style_preset, mdb_peq_snapshot *out){
    mdb_peq_snapshot row = {0};
    static const int GFREQ[10] = {32,64,125,250,500,1000,2000,4000,8000,16000};
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT MASTER_GAIN, PARAMS_JSON FROM PEQ WHERE STYLE_PRESET=?;", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, style_preset);
    int rc = sqlite3_step(st), graphic = 1;
    if(rc == SQLITE_ROW){
        /* master gain: an integer in [-12,12], fully consumed (a fraction/exponent/out-of-range would be
         * dropped or clamped by the editor on save). An empty/NULL master is 0 -> fine. */
        const char *ms = (const char*)sqlite3_column_text(st, 0);
        if(ms && ms[0]){
            const char *p = ms; int neg = 0, ok = 1; long mv = 0;
            if(*p == '-'){ neg = 1; p++; }
            if(!(*p>='0'&&*p<='9')) ok = 0;
            while(*p>='0'&&*p<='9'){ mv = (mv < 100000) ? mv*10 + (*p-'0') : 100000; p++; }  /* saturate: no signed overflow on an oversized value; out-of-range is rejected below */
            if(*p == '.'){ p++; while(*p>='0'&&*p<='9'){ if(*p != '0') ok = 0; p++; } }
            if(*p) ok = 0;                 /* trailing junk */
            if(neg) mv = -mv;
            if(!ok || mv < -12 || mv > 12) graphic = 0;
        }
        row.master = sqlite3_column_int(st, 0);
        const char *js = (const char*)sqlite3_column_text(st, 1);
        if(out && !js) graphic = 0;
        int jlen = js ? sqlite3_column_bytes(st, 1) : 0;
        if(graphic && js){
            jsmn_parser jp; jsmntok_t tok[256]; jsmn_init(&jp);
            int nt = jsmn_parse(&jp, js, jlen, tok, (unsigned)(sizeof tok / sizeof tok[0]));
            if(nt < 1 || tok[0].type != JSMN_ARRAY || tok[0].size != 10) graphic = 0;
            else {
                /* jsmn is a lenient tokenizer: reject anything but whitespace after the array root, so a
                 * "valid curve + trailing junk / second value" can't slip through. */
                int re = tok[0].end;
                while(re < jlen && (js[re]==' '||js[re]=='\t'||js[re]=='\n'||js[re]=='\r')) re++;
                if(re != jlen) graphic = 0;
                int ti = 1;   /* walk the array's tokens in preorder */
                for(int band = 0; graphic && band < 10; band++){
                    if(ti >= nt || tok[ti].type != JSMN_OBJECT){ graphic = 0; break; }
                    int nfields = tok[ti].size; ti++;
                    int have_ft = 0, have_fr = 0, have_q = 0, have_g = 0, have_pos = 0;
                    for(int f = 0; graphic && f < nfields; f++){
                        if(ti + 1 >= nt || tok[ti].type != JSMN_STRING){ graphic = 0; break; }
                        const jsmntok_t *k = &tok[ti], *val = &tok[ti + 1];
                        if(val->type == JSMN_OBJECT || val->type == JSMN_ARRAY){ graphic = 0; break; }  /* no nested values */
                        long v;
                        if(peq_key_is(js, k, "filterType")){ if(have_ft || val->type != JSMN_PRIMITIVE || !peq_tok_int(js, val, &v) || v != 0) graphic = 0; else have_ft = 1; }
                        else if(peq_key_is(js, k, "frequency")){ if(have_fr || val->type != JSMN_PRIMITIVE || !peq_tok_int(js, val, &v) || v != GFREQ[band]) graphic = 0; else have_fr = 1; }
                        else if(peq_key_is(js, k, "qValue")){
                            int len = val->end - val->start;
                            int stock_q = len == 3 && !strncmp(js + val->start, "0.7", 3);
                            if(out && len == 4 && (!strncmp(js + val->start, "0.70", 4) || !strncmp(js + val->start, "0.71", 4))) stock_q = 1;
                            if(have_q || val->type != JSMN_STRING || !stock_q) graphic = 0;
                            else { have_q = 1; memcpy(row.q[band], js + val->start, len); }
                        }
                        else if(peq_key_is(js, k, "gain")){ if(have_g || val->type != JSMN_STRING || !peq_tok_int(js, val, &v) || v < -12 || v > 12) graphic = 0; else { have_g = 1; row.gains[band] = (int)v; } }
                        else if(out && peq_key_is(js, k, "position")){ if(have_pos || val->type != JSMN_PRIMITIVE || !peq_tok_int(js, val, &v) || v != band) graphic = 0; else have_pos = 1; }
                        /* Older full-row callers retain their existing position policy. */
                        ti += 2;   /* key + flat value */
                    }
                    if(graphic && !(have_ft && have_fr && have_q && have_g && (!out || have_pos))) graphic = 0;
                }
            }
        }
    }
    sqlite3_finalize(st);
    if(rc != SQLITE_ROW && rc != SQLITE_DONE) return -1;   /* step error -> read failed */
    if(out && rc == SQLITE_DONE) return 0;
    if(out && graphic) *out = row;
    return graphic;
}
int mdb_peq_is_graphic(int style_preset){ return peq_read_graphic(style_preset, NULL); }
int mdb_get_peq_snapshot(int slot, mdb_peq_snapshot *out){ return peq_read_graphic(slot, out); }

int mdb_playlist_rename(long pid, const char *name){
    if(!name || strlen(name) >= MDB_STR) return 0;
    if(pid<=0) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    int owns_txn = sqlite3_get_autocommit(d);
    if(sqlite3_exec(d, "SAVEPOINT plrename;", 0, 0, 0) != SQLITE_OK) return 0;
    sqlite3_stmt *st; int ok = 0;
    if(sqlite3_prepare_v2(d, "UPDATE PLAYLIST_INFO SET NAME=? WHERE ID=?;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, name?name:"", -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, pid);
        ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(d) > 0;   /* real rename only */
        sqlite3_finalize(st);
    }
    if(ok && !stock_index_put(d, pid, name)) ok = 0;   /* stock shows the same name */
    if(ok && sqlite3_exec(d, "RELEASE plrename;", 0, 0, 0) == SQLITE_OK) return 1;
    sqlite3_exec(d, "ROLLBACK TO plrename;", 0, 0, 0);
    sqlite3_exec(d, "RELEASE plrename;", 0, 0, 0);
    if(owns_txn && !sqlite3_get_autocommit(d)) sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    return 0;
}
/* Delete a playlist: removes only the playlist + its membership rows; the SONG
 * table (the actual songs/files) is never touched. */
int mdb_playlist_delete(long pid){
    if(pid<=0) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    /* atomic: drop membership + the playlist row together, or roll back (no orphan rows).
     * Every step's rc is checked: a failed BEGIN aborts; any DELETE failure or a failed
     * COMMIT (e.g. SQLITE_BUSY) rolls back and reports failure - so the transaction can
     * never be left open on the shared g_db connection. */
    if(sqlite3_exec(d, "BEGIN;", 0, 0, 0) != SQLITE_OK) return 0;
    sqlite3_stmt *st;
    int memb_ok = 0, info_ok = 0, changed = 0;
    /* only a playlist diskOS knows: a stale or foreign id must never erase another playlist's members, or (V2.57's
     * CUSTOM_PLAYLIST -> CUSTOM_PLAYLIST_INDEX ON DELETE CASCADE) its stock row */
    char qs[80]; snprintf(qs, sizeof qs, "SELECT COUNT(*) FROM PLAYLIST_INFO WHERE ID=%ld;", pid);
    int q; if(mdb_scalar_i64(d, qs, &q) != 1 || !q){ sqlite3_exec(d, "ROLLBACK;", 0, 0, 0); return 0; }
    if(sqlite3_prepare_v2(d, "DELETE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int64(st, 1, pid); memb_ok = (sqlite3_step(st) == SQLITE_DONE); sqlite3_finalize(st);
    }
    if(memb_ok && sqlite3_prepare_v2(d, "DELETE FROM PLAYLIST_INFO WHERE ID=?;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int64(st, 1, pid);
        if(sqlite3_step(st) == SQLITE_DONE){ info_ok = 1; changed = (sqlite3_changes(d) > 0); }
        sqlite3_finalize(st);
    }
    if(memb_ok && info_ok && !stock_index_drop(d, pid)) info_ok = 0;   /* and stock's name for it */
    if(memb_ok && info_ok){   /* and the sync's memory of it (the table may not exist yet: that is fine) */
        char fq[80]; snprintf(fq, sizeof fq, "DELETE FROM DISKOS_PL_SYNCED WHERE PID=%ld;", pid);
        sqlite3_exec(d, fq, 0, 0, 0);
    }
    /* success only if every DELETE stepped clean AND COMMIT actually succeeded */
    if(memb_ok && info_ok && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK) return changed;
    sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    return 0;
}
/* List a playlist's songs in its OWN order - the order they were added (an imported M3U keeps its file order;
 * owner decision 2026-09-27). Display, removal, export and the play slot all use this order. A play copies the
 * members into the reserved slot in this order; the player reads that slot with no ORDER BY, which is this order
 * without a UNIQUE(PLAYLIST_ID,PATH,TRACK) index (the owner's V2.57 DB) and PATH order with one - there
 * mdb_reserved_slot_player_pos still starts the tapped song (next/prev then follow PATH order). */
int mdb_playlist_songs(long pid, mdb_song_t *out, int cap){
    if(pid<=0) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    const char *sql =
        "SELECT IFNULL(TITLE,IFNULL(NAME,'Untitled')),IFNULL(ARTIST,''),IFNULL(DURATION,0) "
        "FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=? ORDER BY ID;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid);
    int n=0;
    while(n<cap && sqlite3_step(st) == SQLITE_ROW){
        snprintf(out[n].title,  MDB_STR, "%s", colt(st,0));
        snprintf(out[n].artist, MDB_STR, "%s", colt(st,1));
        out[n].album[0]=0; out[n].album_h=0; out[n].genre[0]=0;
        out[n].dur_ms = sqlite3_column_int(st,2); out[n].id = 0;
        trim(out[n].title); trim(out[n].artist);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

/* The UI retains only display fields and the occurrence identity, never a
 * second catalogue-sized song structure. Read into a private snapshot first. */
int mdb_playlist_display(long pid, mdb_playlist_display_t **out){
    *out = NULL;
    sqlite3 *d = db(); if(!d || pid<=0) return -1;
    int cap = mdb_playlist_count(pid);
    if(cap < 0) return -1;
    if(cap > 8192) cap = 8192;
    if(!cap) return 0;
    mdb_playlist_display_t *rows = calloc((size_t)cap,sizeof *rows);
    if(!rows) return -1;
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT IFNULL(TITLE,IFNULL(NAME,'Untitled')),IFNULL(ARTIST,''),IFNULL(DURATION,0),ID "
                      "FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=? ORDER BY ID;";
    if(sqlite3_prepare_v2(d,sql,-1,&st,NULL)!=SQLITE_OK){ free(rows); return -1; }
    sqlite3_bind_int64(st,1,pid);
    int n=0,rc=SQLITE_DONE;
    while(n<cap && (rc=sqlite3_step(st))==SQLITE_ROW){
        snprintf(rows[n].title,MDB_STR,"%s",colt(st,0));
        snprintf(rows[n].artist,MDB_STR,"%s",colt(st,1));
        trim(rows[n].title); trim(rows[n].artist);
        rows[n].dur_ms=sqlite3_column_int(st,2);
        rows[n].occurrence=sqlite3_column_int64(st,3); n++;
    }
    sqlite3_finalize(st);
    if(rc!=SQLITE_DONE && n<cap){ free(rows); return -1; }
    *out=rows; return n;
}
int mdb_playlist_occurrence_pos(long pid, long long occurrence){
    sqlite3 *d=db(); if(!d) return 0;
    sqlite3_stmt *st;
    const char *sql="SELECT COUNT(*) FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?1 AND ID<=?2 "
                    "AND EXISTS(SELECT 1 FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?1 AND ID=?2);";
    if(sqlite3_prepare_v2(d,sql,-1,&st,NULL)!=SQLITE_OK) return 0;
    sqlite3_bind_int64(st,1,pid); sqlite3_bind_int64(st,2,occurrence);
    int pos=sqlite3_step(st)==SQLITE_ROW ? sqlite3_column_int(st,0) : 0;
    sqlite3_finalize(st); return pos;
}
int mdb_playlist_remove_occurrence(long pid, long long occurrence){
    sqlite3 *d=db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d,"DELETE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=? AND ID=?;",-1,&st,NULL)!=SQLITE_OK) return 0;
    sqlite3_bind_int64(st,1,pid); sqlite3_bind_int64(st,2,occurrence);
    int rc=sqlite3_step(st); sqlite3_finalize(st);
    return rc==SQLITE_DONE && sqlite3_changes(d)>0;
}

/* Cast (14g): the row ids of a playlist in the order mdb_playlist_songs lists them (same WHERE and ORDER BY ID), duplicates kept. */
int mdb_playlist_rows(long pid, int *ids, int cap){
    if(pid <= 0) return -1;
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT ID FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=? ORDER BY ID;", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, pid);
    int n = 0, rc;
    while((rc = sqlite3_step(st)) == SQLITE_ROW){ if(n < cap && ids) ids[n] = sqlite3_column_int(st, 0); n++; }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? n : -1;
}

/* Track count in a playlist. */
int mdb_playlist_count(long pid){
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?;", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, pid);
    int n=-1; if(sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st,0);
    sqlite3_finalize(st);
    return n;
}

/* An existing playlist whose name, with the characters exFAT cannot store in a file name ( " * / : < > ? \ | ) each
 * shown as '_', equals `stem` - so a file named after a playlist ("punjabi music_.m3u" for "punjabi music?") is
 * recognised as that playlist instead of becoming a near-duplicate. Only for names taken from a file name. */
static long playlist_id_by_file_name(const char *stem){
    if(!strchr(stem, '_')) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st; long id = 0; int hits = 0;
    if(sqlite3_prepare_v2(d, "SELECT ID, NAME FROM PLAYLIST_INFO;", -1, &st, NULL) != SQLITE_OK) return 0;
    while(sqlite3_step(st) == SQLITE_ROW){
        const char *nm = colt(st, 1); size_t k = 0;
        for(; nm[k] && stem[k]; k++){
            char c = nm[k] && strchr("\"*/:<>?\\|", nm[k]) ? '_' : nm[k];
            if(c != stem[k]) break;
        }
        if(!nm[k] && !stem[k]){ id = (long)sqlite3_column_int64(st, 0); hits++; }
    }
    sqlite3_finalize(st);
    return hits == 1 ? id : 0;   /* "A?" and "A*" both read "A_": ambiguous -> not treated as already imported */
}
/* Resolve an m3u entry to a real SONG.PATH: exact match first, else by filename
 * (so m3u files with relative paths or a different root still resolve). 1=found. */
/* Escape LIKE metachars (% _ \) so a filename can't wildcard-match the wrong song. Returns 1 on
 * success, 0 if the escaped form would not fit (caller must then reject the match rather than use a
 * silently-truncated pattern that could match the wrong path). */
static int like_escape(const char *s, char *out, int cap){
    int j=0;
    for(; *s; s++){
        int need = (*s=='%'||*s=='_'||*s=='\\') ? 2 : 1;
        if(j + need > cap - 1) return 0;                 /* would truncate -> fail */
        if(need==2) out[j++]='\\';
        out[j++]=*s;
    }
    out[j]=0; return 1;
}
/* Fill out[] and return 1 iff EXACTLY ONE song PATH matches the LIKE pattern (unambiguous). LIMIT 2
 * so a second row means "ambiguous" -> reject. Uniqueness is only established when the step AFTER the
 * single row is SQLITE_DONE (an error there leaves it unproven). A path too long for out[cap] is also
 * rejected rather than silently truncated (it would fail playlist insertion). */
/* `herr` (optional) is set to 1 on a real DB error (prepare/bind/step failure) so a caller can tell it
 * apart from a genuine no-match/ambiguous/too-long reject (all of which leave *herr untouched). */
static int resolve_like(sqlite3 *d, const char *like, char *out, int cap, int *herr){
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT PATH FROM SONG WHERE PATH LIKE ? ESCAPE '\\' LIMIT 2;", -1, &st, NULL)!=SQLITE_OK){ if(herr)*herr=1; return 0; }
    if(sqlite3_bind_text(st, 1, like, -1, SQLITE_STATIC)!=SQLITE_OK){ sqlite3_finalize(st); if(herr)*herr=1; return 0; }
    int r1 = sqlite3_step(st);
    if(r1 != SQLITE_ROW){ sqlite3_finalize(st); if(r1 != SQLITE_DONE && herr)*herr=1; return 0; }   /* DONE=no match; else DB error */
    const char *p = (const char*)sqlite3_column_text(st, 0);   /* NULL here = OOM conversion error, not a real path */
    if(!p || !p[0]){ sqlite3_finalize(st); if(herr)*herr=1; return 0; }
    int plen = (int)strlen(p);
    int fits = (plen < cap);
    if(fits) memcpy(out, p, (size_t)plen + 1);          /* copy while the row pointer is still valid */
    int r2 = sqlite3_step(st);                          /* SQLITE_DONE proves uniqueness; ROW=ambiguous; else error */
    int fz = sqlite3_finalize(st);
    if(r2 != SQLITE_DONE || !fits){
        if((r2 != SQLITE_DONE && r2 != SQLITE_ROW) && herr)*herr=1;   /* a step error (not the benign ambiguous ROW) */
        if(cap>0) out[0]=0;
        return 0;
    }
    if(fz != SQLITE_OK){ if(herr)*herr=1; if(cap>0) out[0]=0; return 0; }   /* deferred error at finalize */
    return 1;
}
/* Resolve an m3u entry to a real SONG.PATH: exact match first, then the most SPECIFIC path suffix the
 * entry provides (the full relative path - disambiguates the same basename in different album dirs),
 * then a bare basename. A suffix/basename match is accepted ONLY if UNambiguous; otherwise we reject
 * rather than add the wrong track to a playlist (L36). 1=found. */
static int song_resolve(const char *entry, char *out, int cap, int *herr){
    sqlite3 *d = db(); if(!d){ if(herr)*herr=1; return 0; }
    sqlite3_stmt *st; int got = 0;
    if(sqlite3_prepare_v2(d, "SELECT PATH FROM SONG WHERE PATH=? LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        int rc;
        if(sqlite3_bind_text(st, 1, entry, -1, SQLITE_STATIC) != SQLITE_OK){ if(herr)*herr=1; rc = SQLITE_ERROR; }
        else rc = sqlite3_step(st);
        if(rc == SQLITE_ROW){
            const char *pth = (const char*)sqlite3_column_text(st, 0);   /* NULL = OOM conversion error, not a real empty path */
            if(pth && pth[0]){ got = path_fits(out, cap, pth); }   /* too long for the caller: no match, never a prefix */
            else if(herr)*herr=1;
        } else if(rc != SQLITE_DONE && herr)*herr=1;      /* a step error, not a clean no-match */
        if(sqlite3_finalize(st) != SQLITE_OK && herr)*herr=1;   /* a deferred error surfaces at finalize */
    } else if(herr)*herr=1;                               /* prepare failed -> DB error */
    if(got) return 1;
    if(herr && *herr) return 0;                           /* stop probing once the DB is erroring */
    /* strip a leading "/" or "./" so a relative entry ("Album/Track.flac") reads as a path suffix */
    const char *rel = entry; while(rel[0]=='/' || (rel[0]=='.' && rel[1]=='/')) rel += (rel[0]=='/')?1:2;
    if(!rel[0]) return 0;
    char esc[2 * TRACK_PATH_MAX], like[2 * TRACK_PATH_MAX + 2];
    /* 1) most-specific: the full relative path (only if it carries directory components) */
    if(strchr(rel, '/') && like_escape(rel, esc, sizeof esc)){
        snprintf(like, sizeof like, "%%/%s", esc);
        if(resolve_like(d, like, out, cap, herr)) return 1;
        if(herr && *herr) return 0;
    }
    /* 2) bare basename - accepted only if unambiguous */
    const char *base = strrchr(rel, '/'); base = base ? base+1 : rel;
    if(!base[0]) return 0;
    if(like_escape(base, esc, sizeof esc)){
        snprintf(like, sizeof like, "%%/%s", esc);
        if(resolve_like(d, like, out, cap, herr)) return 1;
    }
    if(!(herr && *herr))
        fprintf(stderr,"m3u resolve: '%s' unresolved (no match, ambiguous, or too long)\n", entry);
    return 0;
}
/* Import one .m3u/.m3u8 file as a playlist. The name is the file's own "#PLAYLIST:" directive if present
 * (so a file diskOS exported round-trips under its true name), else the filename stem. If a playlist of
 * that name exists it is left untouched and the file imports as "<name> (N)", unless its tracks equal one already there
 * (idempotent re-import). Returns tracks added, or 0 if skipped/empty. */
#define M3U_CLASH_MAX 100   /* an existing name plus its "(2)".."(99)" copies */
/* 1 if two playlists hold the same tracks (same PATH + TRACK set; order and duplicates ignored), 0 if they differ, -1 if
 * the comparison could not run (the caller then aborts: an unknown answer is never "different, so add a copy"). */
static int playlist_same_members(sqlite3 *d, long a, long b){
    static const char *sql =
        "SELECT (SELECT COUNT(*) FROM (SELECT PATH,IFNULL(TRACK,0) FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?1 "
        "EXCEPT SELECT PATH,IFNULL(TRACK,0) FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?2)) + "
        "(SELECT COUNT(*) FROM (SELECT PATH,IFNULL(TRACK,0) FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?2 "
        "EXCEPT SELECT PATH,IFNULL(TRACK,0) FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?1));";
    sqlite3_stmt *st; int same = -1;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, a); sqlite3_bind_int64(st, 2, b);
    if(sqlite3_step(st) == SQLITE_ROW) same = (sqlite3_column_int(st, 0) == 0) ? 1 : 0;
    sqlite3_finalize(st);
    return same;
}
/* Cut a string that snprintf truncated mid-character back to the last whole UTF-8 character. */
static void utf8_fix(char *s){
    size_t n = strlen(s), i = n;
    while(i > 0 && n - i < 4 && ((unsigned char)s[i-1] & 0xC0) == 0x80) i--;   /* over the continuation bytes */
    if(i == 0) return;
    unsigned char lead = (unsigned char)s[i-1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if(need > 1 && n - (i-1) < need) s[i-1] = 0;
    else if(need == 1 && lead >= 0x80) s[i-1] = 0;   /* a stray continuation byte */
}
/* 1 if SONG has a CUE/ISO row (path, track). */
static int song_subtrack_exists(sqlite3 *d, const char *path, int track){
    sqlite3_stmt *st; int yes = 0;
    if(sqlite3_prepare_v2(d, "SELECT 1 FROM SONG WHERE PATH=?1 AND IFNULL(TRACK,0)=?2 AND (IFNULL(IS_CUE,0)<>0 OR IFNULL(IS_ISO,0)<>0) LIMIT 1;",
                          -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC); sqlite3_bind_int(st, 2, track);
    int rc=sqlite3_step(st);
    yes = rc==SQLITE_ROW ? 1 : (rc==SQLITE_DONE ? 0 : -1);
    sqlite3_finalize(st);
    return yes;
}
#define M3U_MAX_BYTES (4L * 1024 * 1024)   /* ~50k entries; the owner's largest is 32 KB. Bounds the synchronous import */
static int import_m3u_file(const char *m3u_path){
    FILE *fp = fopen(m3u_path, "r"); if(!fp) return 0;
    struct stat fst;
    if(fstat(fileno(fp), &fst) != 0 || fst.st_size > M3U_MAX_BYTES){ fclose(fp); return 0; }   /* too big (or unknown): skip */
    int from_directive = 0;   /* the name came from #PLAYLIST: (exact) rather than the file name (maybe sanitised) */
    const char *b = strrchr(m3u_path, '/'); b = b ? b+1 : m3u_path;
    char name[600]; snprintf(name, sizeof name, "%.599s", b);   /* roomy: a long name is shortened with a hash below, never silently cut */
    char *dot = strrchr(name, '.'); if(dot) *dot = 0;
    /* Pre-scan the header for a "#PLAYLIST:" directive. If found, it is the canonical name - use it for both
     * the idempotency check and the created playlist, so re-importing an exported "<name>-<uid>.m3u"
     * resolves to the same "<name>" instead of a "<name>-<uid>" duplicate. Blank/whitespace-only lines and
     * other comments (#EXTM3U/#EXTINF) are skipped; the scan stops at the first real (non-comment) entry.
     * The directive value is trim()'d to the same canonical form the export writes (mdb_playlists trims the
     * displayed name), so the round-trip name is consistent. */
    /* Read the header a logical line at a time with fgetc so the byte count is exact (fgets + strlen
     * miscount an embedded NUL, and can't tell a full buffer from a real line). The directive lives in the
     * short comment header, so: an over-long line (past a small cap) is NOT a directive and ENDS the header
     * scan (it's a track or junk); a blank line is skipped; a real (non-#) entry ends the scan; a
     * #PLAYLIST: value is trim()'d to the display/export canonical form, and an empty one is ignored so a
     * later real directive still wins. */
    { char hl[192];
      for(;;){
          int n = 0, ch, overlong = 0, binary = 0;
          while((ch = fgetc(fp)) != EOF && ch != '\n'){
              if(ch == 0) binary = 1;                  /* a NUL byte -> this is not a text header line */
              if(n < (int)sizeof hl - 1) hl[n++] = (char)ch; else overlong = 1;   /* count EVERY byte (incl '\r') */
          }
          if(ch == EOF && n == 0) break;               /* end of file */
          if(binary || overlong) break;                /* binary or too-long line -> past the text header */
          if(n > 0 && hl[n-1] == '\r') n--;            /* strip ONE terminal CR (CRLF); keep any interior CR */
          hl[n] = 0;
          char *q = hl;
          if((unsigned char)q[0]==0xEF && (unsigned char)q[1]==0xBB && (unsigned char)q[2]==0xBF) q += 3;
          while(*q==' '||*q=='\t') q++;
          if(!*q) continue;                            /* blank / whitespace-only line -> keep scanning */
          if(!strncmp(q, "#PLAYLIST:", 10)){
              char v[192]; snprintf(v, sizeof v, "%s", q+10);
              trim(v);                                 /* trim the WHOLE value before the name-length limit */
              if(v[0]){ snprintf(name, sizeof name, "%s", v); from_directive = 1; break; }
              continue;                                /* empty directive -> keep looking for a real one */
          }
          if(*q != '#') break;                         /* first real entry line: no directive present */
          /* any other comment (#EXTM3U, #EXTINF, ...) -> keep scanning */
      }
      rewind(fp);
    }
    utf8_fix(name);   /* snprintf may have cut a multi-byte character */
    trim(name);
    /* Names must fit a playlist name (MDB_STR): a longer one keeps its first 130 bytes (whole characters) plus "~" and an
     * 8-digit hash of the FULL name, so two different long names can never become the same name, and the same long name
     * always maps to the same short one (re-import stays idempotent). */
    if(strlen(name) > 139){
        unsigned h = 2166136261u; for(const unsigned char *q = (const unsigned char *)name; *q; q++){ h ^= *q; h *= 16777619u; }
        int cut = 130; while(cut > 0 && ((unsigned char)name[cut] & 0xC0) == 0x80) cut--;
        snprintf(name + cut, sizeof name - (size_t)cut, "~%08x", h);
    }
    /* canonicalise the FINAL name (directive or filename stem, post-truncation) so the
                   * idempotency lookup matches what mdb_playlist_create stores (which also trims) */
    char dir[TRACK_PATH_MAX];
    if(snprintf(dir, sizeof dir, "%s", m3u_path) >= (int)sizeof dir){ fclose(fp); return 0; }   /* never resolve against a cut path */
    char *sl = strrchr(dir, '/'); if(sl) *sl = 0; else dir[0] = 0;
    /* Import the whole file ATOMICALLY: the playlist row + every member commit together, or nothing
     * does. A mid-import failure (a track fails to insert, or the final COMMIT loses a lock race) then
     * leaves NO partial playlist behind, so the name lookup cannot block a clean retry on the next
     * scan. This replaces a best-effort delete that could itself fail under the same lock and strand the
     * partial forever. mdb_playlist_create's inner SAVEPOINT nests cleanly inside this transaction. */
    sqlite3 *d = db(); if(!d){ fclose(fp); return -1; }
    if(sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) != SQLITE_OK){ fclose(fp); return -1; }   /* writer busy -> retry next scan */
    /* A playlist of this name already exists (or, for a file-name stem, "punjabi music?" saved as "punjabi music_"): never
     * touch it. The file imports under the first free "<name> (N)" instead - unless, after reading, its tracks turn out
     * to be exactly those of the existing playlist or ANY of its "(N)" copies, in which case it is the same playlist
     * imported before and nothing is added (so pressing Import twice stays idempotent). The names are read once, inside
     * the write transaction (no other writer can slip a copy in between), and a failed read aborts the import. */
    long clash[M3U_CLASH_MAX]; int nclash = 0;
    char cname[MDB_STR];
    snprintf(cname, sizeof cname, "%.139s", name);
    { unsigned char used[M3U_CLASH_MAX]; memset(used, 0, sizeof used);
      char nb[MDB_STR]; snprintf(nb, sizeof nb, "%.140s", name); utf8_fix(nb);   /* the base numbered copies are made from */
      size_t bl = strlen(nb), nl = strlen(name);
      sqlite3_stmt *ns; int rc = SQLITE_ERROR;
      if(sqlite3_prepare_v2(d, "SELECT ID,NAME FROM PLAYLIST_INFO;", -1, &ns, NULL) == SQLITE_OK){
          while((rc = sqlite3_step(ns)) == SQLITE_ROW){
              const char *nm = colt(ns, 1); long id = (long)sqlite3_column_int64(ns, 0); int k = 0;
              if(!strcmp(nm, name)) k = 1;
              else if(!strncmp(nm, nb, bl) && nm[bl] == ' ' && nm[bl+1] == '('){
                  char *e; long v = strtol(nm + bl + 2, &e, 10);
                  if(v >= 2 && v < M3U_CLASH_MAX && !strcmp(e, ")")) k = (int)v;
              }
              if(k){ used[k] = 1; if(nclash < M3U_CLASH_MAX) clash[nclash++] = id; }
          }
          sqlite3_finalize(ns);
      }
      if(rc != SQLITE_DONE){ sqlite3_exec(d, "ROLLBACK;", 0, 0, 0); fclose(fp); return -1; }   /* not "no copies": unknown */
      (void)nl;
      if(!used[1] && !from_directive){
          long id0 = playlist_id_by_file_name(name);
          if(id0 > 0 && nclash < M3U_CLASH_MAX){ used[1] = 1; clash[nclash++] = id0; }
      }
      if(used[1]){
          int k = 2; while(k < M3U_CLASH_MAX && used[k]) k++;
          if(k >= M3U_CLASH_MAX){ sqlite3_exec(d, "ROLLBACK;", 0, 0, 0); fclose(fp); return 0; }   /* 98 copies already */
          snprintf(cname, sizeof cname, "%.140s (%d)", nb, k);
      }
    }
    long pid = 0; int added = 0; int db_err = 0; char line[TRACK_PATH_MAX + 2];   /* a full path plus CR/LF: a CRLF line of the longest path is kept */
    int pend_track = 0, has_pend = 0;   /* "#DISKOS-TRACK:n" applies to the next entry line only */
    while(fgets(line, sizeof line, fp)){
        /* A logical line longer than the buffer would otherwise be split by fgets into
         * fragments, each resolved as a bogus separate track (e.g. a tail "other.flac").
         * When the buffer filled with no newline, peek the next byte: EOF or a line break
         * means the buffered content is a complete entry whose terminator just didn't fit -
         * keep it; any other byte means a genuinely over-long line - drain it and skip. */
        size_t ll = strlen(line);
        if(ll == sizeof line - 1 && line[ll-1] != '\n'){
            int ch = fgetc(fp);
            if(ch != EOF && ch != '\n' && ch != '\r'){
                while((ch = fgetc(fp)) != '\n' && ch != EOF){ }
                has_pend = 0;                                /* the skipped line consumed any pending track tag */
                continue;                                    /* over-long: skip, don't fragment */
            }
        }
        char *nl = strpbrk(line, "\r\n"); if(nl) *nl = 0;
        char *p = line; while(*p==' '||*p=='\t') p++;
        if((unsigned char)p[0]==0xEF && (unsigned char)p[1]==0xBB && (unsigned char)p[2]==0xBF) p += 3; /* UTF-8 BOM */
        while(*p==' '||*p=='\t') p++;
        for(char *q=p; *q; q++) if(*q=='\\') *q='/';            /* Windows backslash -> '/' so paths resolve */
        if(!strncmp(p, M3U_TRACK_TAG, sizeof M3U_TRACK_TAG - 1)){
            char *endp; long tv = strtol(p + sizeof M3U_TRACK_TAG - 1, &endp, 10);
            has_pend = (endp != p + sizeof M3U_TRACK_TAG - 1 && tv >= 0 && tv < 100000);
            pend_track = has_pend ? (int)tv : 0;
            continue;
        }
        if(!*p || *p=='#') continue;                            /* comment / #EXTINF / blank */
        int want_track = has_pend, track_no = pend_track; has_pend = 0;
        char entry[TRACK_PATH_MAX]; int en;
        if(*p=='/') en = snprintf(entry, sizeof entry, "%s", p);
        else        en = snprintf(entry, sizeof entry, "%s/%s", dir, p);
        int entry_ok = (en > 0 && en < (int)sizeof entry);   /* joined path fits (don't resolve a truncated one) */
        char songpath[TRACK_PATH_MAX];
        int rherr = 0;
        int found = (entry_ok && song_resolve(entry, songpath, sizeof songpath, &rherr));
        if(!found && !rherr) found = song_resolve(p, songpath, sizeof songpath, &rherr);   /* still try the intact relative entry */
        if(rherr){ db_err = 1; break; }   /* a resolver DB error (not a clean miss) -> abandon, don't commit a partial */
        if(!found) continue;              /* genuine no-match / ambiguous -> skip this line */
        if(want_track){
            int exists=song_subtrack_exists(d,songpath,track_no);
            if(exists<=0){
                fprintf(stderr,"m3u import: '%s' track %d unavailable - import incomplete, rolled back\n",songpath,track_no);
                db_err=exists<0 ? 1 : 2; break; /* a failed lookup retries; a missing tagged track skips this file */
            }
        }
        if(!pid){ pid = mdb_playlist_create(cname); if(pid<=0){ db_err = 1; break; } }
        int aerr = 0;
        /* a tagged entry is ONE CUE/ISO track; an untagged path adds every row of that file (foreign M3U: only the file is known) */
        if(want_track ? mdb_playlist_add_subtrack(pid, songpath, track_no, &aerr) : mdb_playlist_add_song_ex(pid, songpath, &aerr)) added++;
        if(aerr){ db_err = 1; break; }   /* a resolved track failed to insert (DB error) -> abandon the import */
    }
    int read_err = ferror(fp);
    fclose(fp);
    /* Commit ONLY a complete import (at least one track resolved+added, no read error, no DB error, and
     * the COMMIT itself succeeds). Anything else rolls the whole transaction back - no partial playlist
     * persists, so the next scan retries this file cleanly. */
    if(pid && added > 0 && !read_err && !db_err){
        for(int i = 0; i < nclash; i++)
        { int sm = playlist_same_members(d, pid, clash[i]);
          if(sm < 0){ fprintf(stderr, "m3u import: could not compare with playlist %ld - import incomplete, rolled back\n", clash[i]);
                      sqlite3_exec(d, "ROLLBACK;", 0, 0, 0); return -1; }
          if(sm){ sqlite3_exec(d, "ROLLBACK;", 0, 0, 0); return 0; } }   /* imported before */
        if(sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK) return added;
        db_err=1;
    }
    sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    return db_err==1 || read_err ? -1 : 0;
}
/* Scan a directory (one level) for *.m3u / *.m3u8 and import each new one.
 * Returns the number of NEW playlists imported. */
/* budget (may be NULL = unlimited): [0] M3U files still allowed, [1] directory entries still allowed */
static int import_m3u_dir_budget(const char *dir, int *budget){
    DIR *d = opendir(dir); if(!d) return 0;
    int total = 0; struct dirent *e;
    while((!budget || (budget[0] > 0 && budget[1] > 0)) && (e = readdir(d))){
        if(budget) budget[1]--;
        const char *n = e->d_name; int L = (int)strlen(n);
        int ism3u = (L>4 && !strcasecmp(n+L-4, ".m3u")) || (L>5 && !strcasecmp(n+L-5, ".m3u8"));
        if(!ism3u) continue;
        char path[TRACK_PATH_MAX];
        if(snprintf(path, sizeof path, "%s/%s", dir, n) >= (int)sizeof path) continue;   /* too long: skip, never truncate */
        if(budget) budget[0]--;
        int added=import_m3u_file(path);
        if(added<0){ total=-1; break; }
        if(added>0) total++;
    }
    closedir(d);
    return total;
}
static int mdb_import_m3u_dir_leased(const char *dir){ return import_m3u_dir_budget(dir, NULL); }

/* Import .m3u/.m3u8 from anywhere on the card, like stock V2.57 (its M3U collector recurses: P257@0x4676a8). Bounded so
 * a huge or looping tree can't hang the UI: at most M3U_WALK_DEPTH levels below <root> and M3U_WALK_DIRS folders.
 * Hidden folders (".x"), "System Volume Information" and symlinks are never entered (a link can loop back up the tree).
 * Folders are visited in readdir order; a playlist whose name already exists is skipped (import_m3u_file), so a
 * repeat import only adds new ones. Returns total playlists imported. */
#define M3U_WALK_DEPTH   8
#define M3U_WALK_DIRS    4096
#define M3U_WALK_FILES   512     /* M3U files opened per import */
#define M3U_WALK_ENTRIES 65536   /* directory entries read per import (both passes) */
/* budget: [0] folders, [1] M3U files, [2] directory entries still allowed */
static void import_m3u_walk(const char *dir, int depth, int *budget, int *total){
    if(*total<0 || budget[0] <= 0 || budget[1] <= 0 || budget[2] <= 0) return;
    budget[0]--;
    int n=import_m3u_dir_budget(dir,budget+1);
    if(n<0){ *total=-1; return; }
    *total+=n;
    if(depth >= M3U_WALK_DEPTH) return;
    DIR *d = opendir(dir); if(!d) return;
    struct dirent *e;
    while(budget[0] > 0 && budget[2] > 0 && (e = readdir(d))){
        budget[2]--;
        const char *nm = e->d_name;
        if(nm[0] == '.' || !strcasecmp(nm, "System Volume Information")) continue;
        char path[TRACK_PATH_MAX];
        if(snprintf(path, sizeof path, "%s/%s", dir, nm) >= (int)sizeof path) continue;   /* too long: skip, never truncate */
        int isdir = e->d_type == DT_DIR;
        if(e->d_type == DT_UNKNOWN){ struct stat st; isdir = lstat(path, &st) == 0 && S_ISDIR(st.st_mode); }   /* lstat: a link is not a dir */
        if(isdir) import_m3u_walk(path, depth + 1, budget, total);
    }
    closedir(d);
}
static int mdb_import_m3u_sd_leased(const char *root){
    int budget[3] = { M3U_WALK_DIRS, M3U_WALK_FILES, M3U_WALK_ENTRIES }, total = 0;
    import_m3u_walk(root, 0, budget, &total);
    return total;
}

/* number of playlists - lets callers size a dynamic buffer to the real count (no fixed cap). */
int mdb_playlist_num(void){
    sqlite3 *d = db(); if(!d) return 0;
    if(!g_ui_read_depth && !mdb_playlist_sync_registry()) return -1;
    sqlite3_stmt *st; int n=0;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM PLAYLIST_INFO;", -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st,0);
        sqlite3_finalize(st);
    }
    return n;
}

/* counts (optional) gets each playlist's track count from the same query, so a listing that shows counts
 * needs no per-playlist COUNT(*). */
int mdb_playlists_counted(char names[][MDB_STR], long *ids, int *counts, int cap){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    const char *q = counts
        ? "SELECT p.NAME,p.ID,COUNT(m.PLAYLIST_ID) FROM PLAYLIST_INFO p LEFT JOIN CUSTOM_PLAYLIST m "
          "ON m.PLAYLIST_ID=p.ID WHERE p.NAME<>? GROUP BY p.ID ORDER BY p.ADD_TIME,p.ID;"
        : "SELECT NAME,ID FROM PLAYLIST_INFO WHERE NAME<>? ORDER BY ADD_TIME,ID;";
    if(sqlite3_prepare_v2(d, q, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, BOOK_SCOPE_NAME, -1, SQLITE_STATIC);   /* hide the reserved book-scope playlist */
    int n=0;
    while(n<cap && sqlite3_step(st) == SQLITE_ROW){
        snprintf(names[n], MDB_STR, "%s", colt(st,0)); trim(names[n]);
        ids[n] = (long)sqlite3_column_int64(st,1);
        if(counts) counts[n] = sqlite3_column_int(st,2);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int mdb_playlists(char names[][MDB_STR], long *ids, int cap){
    return mdb_playlists_counted(names, ids, NULL, cap);
}

int mdb_search(const char *q, const mdb_song_t **out, int cap){
    if(!q || !q[0]) return 0;
    int n = 0;
    for(int i=0;i<g_n && n<cap;i++){
        const mdb_song_t *s = &g_songs[i];
        if(strcasestr(s->title, q) || strcasestr(s->artist, q) || strcasestr(s->album, q))
            out[n++] = s;
    }
    return n;
}

int mdb_import_m3u_dir(const char *dir){
    if(!sd_io_begin()) return -1;
    int result = mdb_import_m3u_dir_leased(dir);
    sd_io_end();
    return result;
}

int mdb_import_m3u_sd(const char *root){
    if(!sd_io_begin()) return -1;
    int result = mdb_import_m3u_sd_leased(root);
    sd_io_end();
    return result;
}

int mdb_playlist_export(long pid, const char *name, char *outname, int cap){
    if(!sd_io_begin()) return 0;
    int result = mdb_playlist_export_leased(pid, name, outname, cap);
    sd_io_end();
    return result;
}

/* Up Next (see musicdb.h). A dedicated READ-ONLY connection with a short busy timeout: the queue is read on the UI
 * thread, so it must never wait seconds for the player's write lock (BUSY -> "updating", retried) nor queue behind the
 * art threads on the shared handle. Each read is ONE statement, so the current-song lookup (pos_id, else a path only
 * one queue row has), the base order, the shuffle order, the coverage counts and the positions all come from one
 * snapshot. In shuffle, LIST_SONG_3 must be an exact permutation of LIST_SONG_0 (same count, every POS_ID distinct
 * and present) or the queue is reported as being rebuilt, never shown as a guessed order. Lengths come from the
 * queue row itself: the player copies SONG.DURATION into LIST_SONG_n when it builds a queue (0 = unknown). */
static sqlite3 *g_qdb;
static sqlite3 *qdb(void){
    if(!g_qdb){
        sqlite3 *tmp = NULL;
        if(sqlite3_open_v2(DB_PATH, &tmp, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK){
            sqlite3_busy_timeout(tmp, 200);
            g_qdb = tmp;
        } else if(tmp) sqlite3_close(tmp);
    }
    return g_qdb;
}
static int q_busy(int rc){ rc &= 0xFF; return rc == SQLITE_BUSY || rc == SQLITE_LOCKED; }

/* A queue row's length for display. A queue built from the reserved slot carries 86400000 (24 h) when the source song's
 * length is unknown (the slot builders need a non-zero value, see mdb_plan_materialize), and the player copies it into
 * LIST_SONG_n. A real recording of exactly that length is stored the same way, so the value alone cannot tell them
 * apart; the source can: the placeholder is written only when the source has no length. So 86400000 is shown only if the
 * library row it came from (SONG by PATH+TRACK, or BOOKS by PATH) really holds 86400000; otherwise it is unknown (0).
 * Nothing stored or sent to the player changes. A lookup that cannot be made hides the value rather than show 1440:00. */
static int q_src_holds(sqlite3 *d, const char *sql, const char *path, sqlite3_int64 track, int by_track){
    sqlite3_stmt *s = NULL; int hit = 0;
    if(sqlite3_prepare_v2(d, sql, -1, &s, NULL) != SQLITE_OK){ if(s) sqlite3_finalize(s); return -1; }
    sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
    if(by_track) sqlite3_bind_int64(s, 2, track);
    int rc = sqlite3_step(s);
    if(rc == SQLITE_ROW){
        hit = sqlite3_column_int64(s, 0) == 86400000 ? 1 : -1;
        if(sqlite3_step(s) != SQLITE_DONE) hit = -1;
    } else if(rc != SQLITE_DONE) hit = -1;
    sqlite3_finalize(s);
    return hit;
}
static int q_real_dur(sqlite3 *d, sqlite3_int64 dm, const char *path, sqlite3_int64 track){
    if(dm <= 0 || dm > 2147483647) return 0;
    if(dm != 86400000) return (int)dm;
    if(!path || !path[0]) return 0;
    int song = q_src_holds(d,
        "SELECT DURATION FROM SONG WHERE PATH=?1 AND IFNULL(TRACK,0)=?2 LIMIT 2;", path, track, 1);
    int book = q_src_holds(d,
        "SELECT DURATION FROM BOOKS WHERE PATH=?1 LIMIT 2;", path, track, 0);
    if((song == 1 && book == 0) || (song == 0 && book == 1)) return (int)dm;
    return 0;
}

int mdb_upnext(int shuffle, int cur_pos_id, const char *cur_path, mdb_qrow_t *out, int cap, int *more){
    if(more) *more = 0;
    if(!out || cap < 1) return MDB_UPNEXT_ERROR;
    sqlite3 *d = qdb(); if(!d) return MDB_UPNEXT_ERROR;
    const char *sql = shuffle
        ? "WITH B AS (SELECT ID,ROW_NUMBER() OVER (ORDER BY ID) AS ORD,TITLE,NAME,ARTIST,PATH,DURATION,TRACK FROM LIST_SONG_0),"
          "C AS (SELECT (SELECT COUNT(*) FROM LIST_SONG_0) AS N0,(SELECT COUNT(*) FROM LIST_SONG_3) AS N3,"
          "(SELECT COUNT(DISTINCT POS_ID) FROM LIST_SONG_3) AS ND,"
          "CASE WHEN ?1>0 THEN ?1 ELSE (SELECT CASE WHEN COUNT(*)=1 THEN MAX(ID) END FROM LIST_SONG_0 WHERE PATH=?2) END AS TID),"
          "X AS (SELECT R.ID AS RID,B.ID AS BID,B.ORD,B.TITLE,B.NAME,B.ARTIST,B.PATH,B.DURATION,B.TRACK FROM LIST_SONG_3 R LEFT JOIN B ON B.ID=R.POS_ID) "
          "SELECT C.N0,C.N3,C.ND,C.TID,X.RID,X.BID,X.ORD,X.TITLE,X.NAME,X.ARTIST,X.PATH,X.DURATION,X.TRACK FROM C LEFT JOIN X ON 1 ORDER BY X.RID;"
        : "WITH B AS (SELECT ID,ROW_NUMBER() OVER (ORDER BY ID) AS ORD,TITLE,NAME,ARTIST,PATH,DURATION,TRACK FROM LIST_SONG_0),"
          "C AS (SELECT (SELECT COUNT(*) FROM LIST_SONG_0) AS N0,"
          "CASE WHEN ?1>0 THEN ?1 ELSE (SELECT CASE WHEN COUNT(*)=1 THEN MAX(ID) END FROM LIST_SONG_0 WHERE PATH=?2) END AS TID) "
          "SELECT C.N0,C.N0,C.N0,C.TID,B.ID,B.ID,B.ORD,B.TITLE,B.NAME,B.ARTIST,B.PATH,B.DURATION,B.TRACK FROM C LEFT JOIN B ON 1 ORDER BY B.ID;";
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_prepare_v2(d, sql, -1, &q, NULL);
    if(rc != SQLITE_OK){ int b = q_busy(sqlite3_extended_errcode(d)); if(q) sqlite3_finalize(q); return b ? MDB_UPNEXT_BUSY : MDB_UPNEXT_ERROR; }
    sqlite3_bind_int(q, 1, cur_pos_id > 0 ? cur_pos_id : 0);
    if(cur_path && cur_path[0]) sqlite3_bind_text(q, 2, cur_path, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(q, 2);
    int n = 0, found = 0, first = 1, status = 0, have_status = 0, target = 0, extra = 0;   /* MDB_UPNEXT_EMPTY is 0 */
    while((rc = sqlite3_step(q)) == SQLITE_ROW){
        if(first){
            first = 0;
            int n0 = sqlite3_column_int(q, 0), n3 = sqlite3_column_int(q, 1), nd = sqlite3_column_int(q, 2);
            if(sqlite3_column_type(q, 4) == SQLITE_NULL){                 /* no queue rows in this order */
                status = n0 == 0 ? MDB_UPNEXT_EMPTY : MDB_UPNEXT_NOTFOUND; /* shuffle list not built yet */
                have_status = 1; break;
            }
            if(n3 != n0 || nd != n0){ status = MDB_UPNEXT_NOTFOUND; have_status = 1; break; }   /* not an exact permutation */
            if(sqlite3_column_type(q, 3) == SQLITE_NULL){ status = MDB_UPNEXT_NOTFOUND; have_status = 1; break; }   /* current unknown/ambiguous */
            target = sqlite3_column_int(q, 3);
        }
        if(sqlite3_column_type(q, 5) == SQLITE_NULL){ status = MDB_UPNEXT_NOTFOUND; have_status = 1; break; }   /* shuffle row -> no base row */
        int id = sqlite3_column_int(q, 5);
        if(!found){ if(id != target) continue; found = 1; }
        if(n >= cap){ extra++; continue; }
        mdb_qrow_t *r = &out[n++];
        memset(r, 0, sizeof *r);
        r->base_id = id;
        r->ord = sqlite3_column_int(q, 6);
        const char *t = (const char *)sqlite3_column_text(q, 7);
        if(!t || !t[0]) t = (const char *)sqlite3_column_text(q, 8);
        const char *p = (const char *)sqlite3_column_text(q, 10);
        if(!t || !t[0]){ const char *sl = p ? strrchr(p, '/') : NULL; t = sl ? sl + 1 : p; }
        snprintf(r->title, sizeof r->title, "%s", t ? t : "");
        const char *ar = (const char *)sqlite3_column_text(q, 9);
        snprintf(r->artist, sizeof r->artist, "%s", ar ? ar : "");
        if(!path_fits(r->path, sizeof r->path, p)){
            sqlite3_finalize(q);
            return MDB_UPNEXT_NOTFOUND;
        }
        sqlite3_int64 dm = sqlite3_column_int64(q, 11);           /* copied from SONG by the player at queue build */
        r->dur_ms = q_real_dur(d, dm, p, sqlite3_column_int64(q, 12));
    }
    int busy = (rc != SQLITE_ROW && rc != SQLITE_DONE) && q_busy(sqlite3_extended_errcode(d));
    sqlite3_finalize(q);
    if(have_status) return status;
    if(rc != SQLITE_DONE) return busy ? MDB_UPNEXT_BUSY : MDB_UPNEXT_ERROR;
    if(first) return MDB_UPNEXT_ERROR;                                   /* C always yields a row */
    if(!found) return MDB_UPNEXT_NOTFOUND;
    if(more) *more = extra;
    return n;
}

/* Is (base_id, ord, path) still exactly that row of the live queue? Checked right before a jump, so a list read
 * before a queue rebuild can never jump to a different song. 1 = yes, 0 = no, -1 = couldn't tell (busy/error). */
int mdb_queue_row_valid(int base_id, int ord, const char *path){
    sqlite3 *d = qdb(); if(!d || !path) return -1;
    sqlite3_stmt *q = NULL;
    if(sqlite3_prepare_v2(d, "SELECT ORD,PATH FROM (SELECT ID,ROW_NUMBER() OVER (ORDER BY ID) AS ORD,PATH FROM LIST_SONG_0) "
                             "WHERE ID=?1;", -1, &q, NULL) != SQLITE_OK){ if(q) sqlite3_finalize(q); return -1; }
    sqlite3_bind_int(q, 1, base_id);
    int v, rc = sqlite3_step(q);
    if(rc == SQLITE_ROW){
        const char *p = (const char *)sqlite3_column_text(q, 1);
        v = sqlite3_column_int(q, 0) == ord && p && !strcmp(p, path);
    } else v = rc == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(q);
    return v;
}

/* Cast queue snapshot (see musicdb.h). A private read connection per call, one BEGIN ... close: the counts, the base order,
 * the shuffle list and the current-row lookup all see one database snapshot, so a player rewriting LIST_SONG_0/3 meanwhile
 * can never yield a mixed result. The caller runs it on a worker thread, never the UI thread. */
static sqlite3 *qsnap_open(void){
    sqlite3 *d = NULL;
    if(sqlite3_open_v2(DB_PATH, &d, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK){
        sqlite3_busy_timeout(d, 200);
        if(sqlite3_exec(d, "BEGIN;", NULL, NULL, NULL) == SQLITE_OK) return d;
    }
    if(d) sqlite3_close(d);
    return NULL;
}
static int qsnap_rc(sqlite3 *d){ return q_busy(sqlite3_extended_errcode(d)) ? MDB_UPNEXT_BUSY : MDB_UPNEXT_ERROR; }
void mdb_qsnap_free(mdb_qsnap_t *s){
    if(!s) return;
    free(s->base); free(s->shuf); free(s->flags);
    memset(s, 0, sizeof *s);
}
int mdb_queue_snapshot(int shuffle, int cur_pos_id, const char *cur_path, int cap, mdb_qsnap_t *o){
    if(!o) return MDB_UPNEXT_ERROR;
    memset(o, 0, sizeof *o);
    if(cap < 1) return MDB_UPNEXT_ERROR;
    sqlite3 *d = qsnap_open(); if(!d) return MDB_UPNEXT_ERROR;
    sqlite3_stmt *q = NULL;
    mdb_qsnap_t s; memset(&s, 0, sizeof s);
    int r = MDB_UPNEXT_ERROR, rc, n0, n3, i, hits = 0, cur_id = 0;
    rc = sqlite3_prepare_v2(d, shuffle ? "SELECT (SELECT COUNT(*) FROM LIST_SONG_0),(SELECT COUNT(*) FROM LIST_SONG_3);"
                                       : "SELECT COUNT(*),0 FROM LIST_SONG_0;", -1, &q, NULL);
    if(rc != SQLITE_OK || sqlite3_step(q) != SQLITE_ROW){ r = qsnap_rc(d); goto out; }
    n0 = sqlite3_column_int(q, 0); n3 = sqlite3_column_int(q, 1);
    sqlite3_finalize(q); q = NULL;
    if(n0 == 0){ r = (shuffle && n3) ? MDB_UPNEXT_NOTFOUND : MDB_UPNEXT_EMPTY; goto out; }
    if(n0 > cap){ r = MDB_QSNAP_TOO_LARGE; goto out; }                       /* never truncated */
    if(shuffle && n3 != n0){ r = MDB_UPNEXT_NOTFOUND; goto out; }            /* the player is still building it */
    s.base = malloc((size_t)n0 * sizeof(int)); s.flags = calloc((size_t)n0, 1);
    if(shuffle) s.shuf = malloc((size_t)n0 * sizeof(int));
    if(!s.base || !s.flags || (shuffle && !s.shuf)) goto out;
    if(sqlite3_prepare_v2(d, "SELECT ID,IFNULL(IS_CUE,0),IFNULL(IS_ISO,0) FROM LIST_SONG_0 ORDER BY ID;", -1, &q, NULL) != SQLITE_OK){ r = qsnap_rc(d); goto out; }
    for(i = 0; (rc = sqlite3_step(q)) == SQLITE_ROW; i++){
        sqlite3_int64 id = sqlite3_column_int64(q, 0);
        if(i >= n0 || id < 1 || id > INT_MAX) goto out;
        s.base[i] = (int)id;
        s.flags[i] = (unsigned char)((sqlite3_column_int(q, 1) ? 1 : 0) | (sqlite3_column_int(q, 2) ? 2 : 0));
    }
    if(rc != SQLITE_DONE || i != n0){ r = rc == SQLITE_DONE ? MDB_UPNEXT_ERROR : qsnap_rc(d); goto out; }
    sqlite3_finalize(q); q = NULL;
    if(shuffle){                          /* captured as stored; only an exact permutation of the base is accepted */
        if(sqlite3_prepare_v2(d, "SELECT POS_ID FROM LIST_SONG_3 ORDER BY ID;", -1, &q, NULL) != SQLITE_OK){ r = qsnap_rc(d); goto out; }
        for(i = 0; (rc = sqlite3_step(q)) == SQLITE_ROW; i++){
            sqlite3_int64 id = sqlite3_column_type(q, 0) == SQLITE_NULL ? 0 : sqlite3_column_int64(q, 0);
            int key = (id > 0 && id <= INT_MAX) ? (int)id : 0, *hit = key ? bsearch(&key, s.base, (size_t)n0, sizeof(int), mdb_id_cmp) : NULL;
            if(i >= n0 || !hit || (s.flags[hit - s.base] & 4)){ r = MDB_UPNEXT_NOTFOUND; goto out; }   /* extra, unknown or repeated */
            s.flags[hit - s.base] |= 4; s.shuf[i] = key;
        }
        if(rc != SQLITE_DONE){ r = qsnap_rc(d); goto out; }
        if(i != n0){ r = MDB_UPNEXT_NOTFOUND; goto out; }
        for(i = 0; i < n0; i++) s.flags[i] &= 3;
        sqlite3_finalize(q); q = NULL;
    }
    if(sqlite3_prepare_v2(d, "SELECT ID FROM LIST_SONG_0 WHERE (?1>0 AND ID=?1 AND (?2 IS NULL OR PATH=?2)) "
                             "OR (?1<=0 AND ?2 IS NOT NULL AND PATH=?2) LIMIT 2;", -1, &q, NULL) != SQLITE_OK){ r = qsnap_rc(d); goto out; }
    sqlite3_bind_int(q, 1, cur_pos_id > 0 ? cur_pos_id : 0);
    if(cur_path && cur_path[0]) sqlite3_bind_text(q, 2, cur_path, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(q, 2);
    while((rc = sqlite3_step(q)) == SQLITE_ROW){ cur_id = sqlite3_column_int(q, 0); hits++; }
    if(rc != SQLITE_DONE){ r = qsnap_rc(d); goto out; }
    int *cp = hits == 1 ? bsearch(&cur_id, s.base, (size_t)n0, sizeof(int), mdb_id_cmp) : NULL;
    if(!cp){ r = MDB_QSNAP_AMBIGUOUS; goto out; }                           /* current row missing or matched by several */
    s.n = n0; s.cur = (int)(cp - s.base);
    *o = s; memset(&s, 0, sizeof s); r = n0;
out:
    if(q) sqlite3_finalize(q);
    sqlite3_close(d);                                                       /* ends the read transaction */
    mdb_qsnap_free(&s);
    return r;
}

int mdb_queue_rows(const int *ids, int n, mdb_qrow_t *out, unsigned char *flags){
    if(!ids || !out || n < 1) return MDB_UPNEXT_ERROR;
    sqlite3 *d = qsnap_open(); if(!d) return MDB_UPNEXT_ERROR;
    sqlite3_stmt *q = NULL; int r = MDB_UPNEXT_ERROR, rc;
    if(sqlite3_prepare_v2(d, "SELECT TITLE,NAME,ARTIST,PATH,DURATION,IFNULL(IS_CUE,0),IFNULL(IS_ISO,0),IFNULL(TRACK,0) FROM LIST_SONG_0 WHERE ID=?1;", -1, &q, NULL) != SQLITE_OK){ r = qsnap_rc(d); goto out; }
    for(int i = 0; i < n; i++){
        sqlite3_reset(q); sqlite3_bind_int(q, 1, ids[i]);
        rc = sqlite3_step(q);
        if(rc == SQLITE_DONE){ r = MDB_UPNEXT_NOTFOUND; goto out; }           /* the queue was rebuilt since the snapshot */
        if(rc != SQLITE_ROW){ r = qsnap_rc(d); goto out; }
        mdb_qrow_t *w = &out[i]; memset(w, 0, sizeof *w);
        const char *t = (const char *)sqlite3_column_text(q, 0), *p = (const char *)sqlite3_column_text(q, 3);
        if(!t || !t[0]) t = (const char *)sqlite3_column_text(q, 1);
        if(!t || !t[0]){ const char *sl = p ? strrchr(p, '/') : NULL; t = sl ? sl + 1 : p; }
        const char *ar = (const char *)sqlite3_column_text(q, 2);
        w->base_id = ids[i];
        snprintf(w->title, sizeof w->title, "%s", t ? t : "");
        snprintf(w->artist, sizeof w->artist, "%s", ar ? ar : "");
        if(!path_fits(w->path, sizeof w->path, p)){
            r = MDB_UPNEXT_NOTFOUND;
            goto out;
        }
        sqlite3_int64 dm = sqlite3_column_int64(q, 4);
        w->dur_ms = q_real_dur(d, dm, p, sqlite3_column_int64(q, 7));
        if(flags) flags[i] = (unsigned char)((sqlite3_column_int(q, 5) ? 1 : 0) | (sqlite3_column_int(q, 6) ? 2 : 0));
    }
    r = n;
out:
    if(q) sqlite3_finalize(q);
    sqlite3_close(d);
    return r;
}

/* Display rows for SONG.IDs (the Cast selection source), one read transaction like mdb_queue_rows. out may be NULL when only
 * flags are wanted (the snapshot build checks every id this way in one pass); a repeated id yields a repeated row. */
int mdb_song_rows(const int *ids, int n, mdb_srow_t *out, unsigned char *flags){
    if(!ids || n < 1 || (!out && !flags)) return MDB_UPNEXT_ERROR;
    sqlite3 *d = qsnap_open(); if(!d) return MDB_UPNEXT_ERROR;
    sqlite3_stmt *q = NULL; int r = MDB_UPNEXT_ERROR, rc;
    if(sqlite3_prepare_v2(d, "SELECT TITLE,NAME,ARTIST,PATH,DURATION,IFNULL(IS_CUE,0),IFNULL(IS_ISO,0),ALBUM FROM SONG WHERE ID=?1;", -1, &q, NULL) != SQLITE_OK){ r = qsnap_rc(d); goto out; }
    for(int i = 0; i < n; i++){
        sqlite3_reset(q); sqlite3_bind_int(q, 1, ids[i]);
        rc = sqlite3_step(q);
        if(rc == SQLITE_DONE){ r = MDB_UPNEXT_NOTFOUND; goto out; }           /* a rescan removed it since the selection was made */
        if(rc != SQLITE_ROW){ r = qsnap_rc(d); goto out; }
        if(out){
            mdb_srow_t *w = &out[i]; memset(w, 0, sizeof *w);
            const char *t = (const char *)sqlite3_column_text(q, 0), *p = (const char *)sqlite3_column_text(q, 3);
            if(!t || !t[0]) t = (const char *)sqlite3_column_text(q, 1);
            if(!t || !t[0]){ const char *sl = p ? strrchr(p, '/') : NULL; t = sl ? sl + 1 : p; }
            const char *ar = (const char *)sqlite3_column_text(q, 2), *al = (const char *)sqlite3_column_text(q, 7);
            w->id = ids[i];
            snprintf(w->title, sizeof w->title, "%s", t ? t : "");
            snprintf(w->artist, sizeof w->artist, "%s", ar ? ar : "");
            snprintf(w->album, sizeof w->album, "%s", al ? al : "");
            if(!path_fits(w->path, sizeof w->path, p)){
                r = MDB_UPNEXT_NOTFOUND;
                goto out;
            }
            sqlite3_int64 dm = sqlite3_column_int64(q, 4);
            w->dur_ms = (dm > 0 && dm <= 2147483647) ? (int)dm : 0;
        }
        if(flags) flags[i] = (unsigned char)((sqlite3_column_int(q, 5) ? 1 : 0) | (sqlite3_column_int(q, 6) ? 2 : 0));
    }
    r = n;
out:
    if(q) sqlite3_finalize(q);
    sqlite3_close(d);
    return r;
}

/* Display rows for CUSTOM_PLAYLIST.IDs (the Cast playlist source), the shape of mdb_song_rows: one read transaction, a repeated id yields a repeated row,
 * a missing id (the playlist was edited since) is MDB_UPNEXT_NOTFOUND. Nothing requires the path to have a SONG row. */
int mdb_playlist_rows_get(const int *ids, int n, mdb_srow_t *out, unsigned char *flags){
    if(!ids || n < 1 || (!out && !flags)) return MDB_UPNEXT_ERROR;
    sqlite3 *d = qsnap_open(); if(!d) return MDB_UPNEXT_ERROR;
    sqlite3_stmt *q = NULL; int r = MDB_UPNEXT_ERROR, rc;
    if(sqlite3_prepare_v2(d, "SELECT TITLE,NAME,ARTIST,PATH,DURATION,IFNULL(IS_CUE,0),IFNULL(IS_ISO,0),ALBUM FROM CUSTOM_PLAYLIST WHERE ID=?1;", -1, &q, NULL) != SQLITE_OK){ r = qsnap_rc(d); goto out; }
    for(int i = 0; i < n; i++){
        sqlite3_reset(q); sqlite3_bind_int(q, 1, ids[i]);
        rc = sqlite3_step(q);
        if(rc == SQLITE_DONE){ r = MDB_UPNEXT_NOTFOUND; goto out; }
        if(rc != SQLITE_ROW){ r = qsnap_rc(d); goto out; }
        if(out){
            mdb_srow_t *w = &out[i]; memset(w, 0, sizeof *w);
            const char *t = (const char *)sqlite3_column_text(q, 0), *p = (const char *)sqlite3_column_text(q, 3);
            if(!t || !t[0]) t = (const char *)sqlite3_column_text(q, 1);
            if(!t || !t[0]){ const char *sl = p ? strrchr(p, '/') : NULL; t = sl ? sl + 1 : p; }
            const char *ar = (const char *)sqlite3_column_text(q, 2), *al = (const char *)sqlite3_column_text(q, 7);
            w->id = ids[i];
            snprintf(w->title, sizeof w->title, "%s", t ? t : "");
            snprintf(w->artist, sizeof w->artist, "%s", ar ? ar : "");
            snprintf(w->album, sizeof w->album, "%s", al ? al : "");
            if(!path_fits(w->path, sizeof w->path, p)){
                r = MDB_UPNEXT_NOTFOUND;
                goto out;
            }
            sqlite3_int64 dm = sqlite3_column_int64(q, 4);
            w->dur_ms = (dm > 0 && dm <= 2147483647) ? (int)dm : 0;
        }
        if(flags) flags[i] = (unsigned char)((sqlite3_column_int(q, 5) ? 1 : 0) | (sqlite3_column_int(q, 6) ? 2 : 0));
    }
    r = n;
out:
    if(q) sqlite3_finalize(q);
    sqlite3_close(d);
    return r;
}

/* The player's own saved play mode (SYSCONFIG.PLAY_MODE, 0..4), -1 if unreadable. Only meaningful while diskOS has
 * never set a mode: the player loads it at start, and diskOS's 0102 sends override it from then on. */
int mdb_sysconfig_play_mode(void){
    sqlite3 *c = NULL; int v = -1;
    if(sqlite3_open_v2(MDB_SYSCONFIG_PATH, &c, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK){
        sqlite3_busy_timeout(c, 200);
        sqlite3_stmt *st;
        if(sqlite3_prepare_v2(c, "SELECT PLAY_MODE FROM SYSCONFIG WHERE ID=1;", -1, &st, NULL) == SQLITE_OK){
            if(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_INTEGER){
                int x = sqlite3_column_int(st, 0);
                if(x >= 0 && x <= 4) v = x;
            }
            sqlite3_finalize(st);
        }
    }
    if(c) sqlite3_close(c);
    return v;
}

/* Read-only quality evidence; BlueALSA reports LDAC's family, not its quality.
 * V2.57 loads this column into 0x83a740 then 0x8a89c0+0x34. */
int mdb_sysconfig_bt_codec(void){
    sqlite3 *c = NULL; sqlite3_stmt *st = NULL; int v = -1;
    if(sqlite3_open_v2(MDB_SYSCONFIG_PATH, &c, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK){
        sqlite3_busy_timeout(c, 50);
        if(sqlite3_prepare_v2(c, "SELECT BT_CODEC FROM SYSCONFIG WHERE ID=1;", -1, &st, NULL) == SQLITE_OK &&
           sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_INTEGER){
            sqlite3_int64 n = sqlite3_column_int64(st, 0);
            if(n >= 0 && n <= 4) v = n;
        }
    }
    if(st) sqlite3_finalize(st);
    if(c) sqlite3_close(c);
    return v;
}

/* ---- Date & Time: the stock player's automatic time switch (SYSCONFIG.AUTO_TIME) -------------------------------
 * RE (V2.57 mq_player): the player loads AUTO_TIME from SYSCONFIG only when it starts (row loader 0x4eb47c ->
 * settings byte 68), and when Wi-Fi gets an address it runs its timezone lookup (ipinfo.io) + ntpdate + ntpd only if
 * that byte is set (0x4bde78). There is no command to change it live, so a change here applies from the player's
 * next start. Returns 0/1, or -1 when unreadable. */
/* 0/1 of a SYSCONFIG on/off column the player owns, -1 when unreadable (missing, locked, NULL, other value).
 * `sql` is a fixed literal chosen by the caller, never built from input. */
static int sysconfig_flag(const char *sql){
    sqlite3 *c = NULL; int v = -1;
    if(sqlite3_open_v2(MDB_SYSCONFIG_PATH, &c, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK){
        sqlite3_busy_timeout(c, 200);
        sqlite3_stmt *st;
        if(sqlite3_prepare_v2(c, sql, -1, &st, NULL) == SQLITE_OK){
            if(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_INTEGER){
                int x = sqlite3_column_int(st, 0);
                if(x == 0 || x == 1) v = x;
            }
            sqlite3_finalize(st);
        }
    }
    if(c) sqlite3_close(c);
    return v;
}
/* P257@4eee10 selects/repairs, then 4eee64..4eee6c stores EQ_TYPE (key 17).
 * A new read-only connection avoids retaining an earlier selection snapshot. */
int mdb_peq_selected(void){
    sqlite3 *c = NULL; int v = -1;
    if(sqlite3_open_v2(MDB_SYSCONFIG_PATH, &c, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK){
        sqlite3_stmt *st;
        if(sqlite3_prepare_v2(c, "SELECT EQ_TYPE FROM SYSCONFIG WHERE ID=1;", -1, &st, NULL) == SQLITE_OK){
            if(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_INTEGER){
                int x = sqlite3_column_int(st, 0);
                if(x >= 0 && x <= 20) v = x;
            }
            sqlite3_finalize(st);
        }
    }
    if(c) sqlite3_close(c);
    return v;
}
/* Play Through Folders (0687 -> SYSCONFIG.FOLDER_JUMP, default 0; docs/COMMAND_MAP.md V2.57 preference setters) */
int mdb_sysconfig_folder_jump(void){ return sysconfig_flag("SELECT FOLDER_JUMP FROM SYSCONFIG WHERE ID=1;"); }
/* the player's UI language index (stock uses it to pick the iTunes storefront for online covers) */
int mdb_sysconfig_language(void){
    sqlite3 *c = NULL; int v = -1;
    if(sqlite3_open_v2(MDB_SYSCONFIG_PATH, &c, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK){
        sqlite3_busy_timeout(c, 200);
        sqlite3_stmt *st;
        if(sqlite3_prepare_v2(c, "SELECT LANGUAGE FROM SYSCONFIG WHERE ID=1;", -1, &st, NULL) == SQLITE_OK){
            if(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_INTEGER){
                int x = sqlite3_column_int(st, 0);
                if(x >= 0 && x < 64) v = x;
            }
            sqlite3_finalize(st);
        }
    }
    if(c) sqlite3_close(c);
    return v;
}
int mdb_sysconfig_auto_time(void){
    sqlite3 *c = NULL; int v = -1;
    if(sqlite3_open_v2(MDB_SYSCONFIG_PATH, &c, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK){
        sqlite3_busy_timeout(c, 200);
        sqlite3_stmt *st;
        if(sqlite3_prepare_v2(c, "SELECT AUTO_TIME FROM SYSCONFIG WHERE ID=1;", -1, &st, NULL) == SQLITE_OK){
            if(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_INTEGER){
                int x = sqlite3_column_int(st, 0);
                if(x == 0 || x == 1) v = x;
            }
            sqlite3_finalize(st);
        }
    }
    if(c) sqlite3_close(c);
    return v;
}
/* 0 = written and read back, -1 = not (the player may hold the database: bounded wait, never forced) */
int mdb_sysconfig_set_auto_time(int on){
    sqlite3 *c = NULL; int ok = -1;
    on = on ? 1 : 0;
    if(sqlite3_open_v2(MDB_SYSCONFIG_PATH, &c, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK){
        sqlite3_busy_timeout(c, 1500);
        sqlite3_stmt *st;
        if(sqlite3_prepare_v2(c, "UPDATE SYSCONFIG SET AUTO_TIME=? WHERE ID=1;", -1, &st, NULL) == SQLITE_OK){
            sqlite3_bind_int(st, 1, on);
            if(sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(c) == 1) ok = 0;
            sqlite3_finalize(st);
        }
    }
    if(c) sqlite3_close(c);
    return (ok == 0 && mdb_sysconfig_auto_time() == on) ? 0 : -1;
}

/* Keep picture resolution off both the metadata scanner and the drawing thread.
 * Prefer subsequent album rows, then wrap to earlier entries. This is a bounded
 * hint, not playback queue ownership. */
int mdb_art_nearby(const char *track, int pos, int mode, char paths[][TRACK_PATH_MAX], int cap){
    if(cap <= 0 || cap > 16) return 0;
    int n = 0;
    if(mode == 0 || mode == 1 || mode == 3){
        mdb_qsnap_t queue;
        if(mdb_queue_snapshot(mode == 1, pos, track, 4096, &queue) > 0){
            int ids[4], count = 0, cur = queue.cur;
            const int *order = mode == 1 ? queue.shuf : queue.base;
            if(mode == 1) for(int i=0;i<queue.n;i++) if(order[i] == queue.base[queue.cur]){ cur=i; break; }
            for(int i=cur+1;i<queue.n && count<4 && count<cap;i++) ids[count++]=order[i];
            if(mode == 3 && count<4) for(int i=0;i<cur && count<4 && count<cap;i++) ids[count++]=order[i];
            mdb_qrow_t rows[4];
            if(count && mdb_queue_rows(ids, count, rows, NULL) == count)
                for(int i=0;i<count;i++) if(strcmp(track, rows[i].path) && strlen(rows[i].path) < TRACK_PATH_MAX) snprintf(paths[n++], TRACK_PATH_MAX, "%s", rows[i].path);   /* a longer path is skipped, never cut */
            mdb_qsnap_free(&queue);
        }
    }
    sqlite3 *d = db_reader(); if(!d) return n;
    sqlite3_stmt *q;
    const char *sql = "SELECT DISTINCT PATH FROM SONG WHERE ALBUM != '' AND ALBUM = "
        "(SELECT ALBUM FROM SONG WHERE PATH=? LIMIT 1) AND PATH != ? "
        "ORDER BY PATH < ?, PATH LIMIT ?;";
    if(sqlite3_prepare_v2(d, sql, -1, &q, NULL) == SQLITE_OK){
        for(int i=1;i<=3;i++) sqlite3_bind_text(q, i, track, -1, SQLITE_STATIC);
        sqlite3_bind_int(q, 4, cap - n);
        while(n < cap && sqlite3_step(q) == SQLITE_ROW){
            const char *p = (const char *)sqlite3_column_text(q, 0);
            if(p && strlen(p) < TRACK_PATH_MAX) snprintf(paths[n++], TRACK_PATH_MAX, "%s", p);
        }
        sqlite3_finalize(q);
    }
    sqlite3_close(d); return n;
}

/* Cache migration is worker-only. Hold a read-only library transaction across
 * retirement so another SQLite writer cannot change the catalogue underneath it.
 * Enumeration errors, unreadable tracks, scans and generation changes fail closed. */
#include "artcache.h"
#include "artstore.h"
#include "scanner.h"
#include "ipc.h"
#include <stdatomic.h>
static _Atomic uint32_t maintenance_wake;
static uint32_t maintenance_active_wake;

void artcache_maintenance_wake(void){
    atomic_fetch_add(&maintenance_wake, 1);
}
static struct stat art_library_stamp;
static uint64_t art_library_epoch,art_media_epoch;
static int art_library_unchanged(void){
    struct stat now;
    return maintenance_active_wake==atomic_load(&maintenance_wake) &&
        !scanner_active() && !scanner_skipped() && !scanner_no_sd() &&
        ipc_library_epoch()==art_library_epoch && sd_io_generation()==art_media_epoch &&
        !stat(DB_PATH,&now) && now.st_dev==art_library_stamp.st_dev && now.st_ino==art_library_stamp.st_ino &&
        now.st_size==art_library_stamp.st_size && now.st_mtim.tv_sec==art_library_stamp.st_mtim.tv_sec &&
        now.st_mtim.tv_nsec==art_library_stamp.st_mtim.tv_nsec && now.st_ctim.tv_sec==art_library_stamp.st_ctim.tv_sec &&
        now.st_ctim.tv_nsec==art_library_stamp.st_ctim.tv_nsec;
}
int artcache_maintenance(void){
    static uint32_t last_wake=UINT32_MAX,finished_wake=UINT32_MAX;
    static uint64_t finished_library=UINT64_MAX,finished_media=UINT64_MAX;
    static unsigned finished_art,inventory_art;
    static uint64_t last_library=UINT64_MAX,last_media=UINT64_MAX;
    static struct stat last_db;
    if(scanner_active() || scanner_skipped()>0 || scanner_no_sd() || !sd_io_begin()) return 0;
    uint32_t wake=atomic_load(&maintenance_wake);
    maintenance_active_wake=wake;
    uint64_t library=ipc_library_epoch(),media=sd_io_generation();
    unsigned art=artcache_gen();
    if(wake==finished_wake && library==finished_library && media==finished_media){
        if(art==finished_art){ artstore_idle(); sd_io_end(); return 0; }
        /* A demand proof can supersede a previously unrecorded warm legacy set. */
        last_library=UINT64_MAX;
    }
    int worked=0;
    sqlite3 *d=db_reader(); sqlite3_stmt *q=NULL; struct stat before={0},after;
    int ok=d && !stat(DB_PATH,&before) && sqlite3_exec(d,"BEGIN",NULL,NULL,NULL)==SQLITE_OK;
    if(ok) ok=sqlite3_prepare_v2(d,"SELECT PATH FROM SONG ORDER BY ID",-1,&q,NULL)==SQLITE_OK;
    int rebuild=wake!=last_wake || library!=last_library || media!=last_media || before.st_dev!=last_db.st_dev || before.st_ino!=last_db.st_ino || before.st_size!=last_db.st_size ||
        before.st_ctim.tv_sec!=last_db.st_ctim.tv_sec || before.st_ctim.tv_nsec!=last_db.st_ctim.tv_nsec ||
        before.st_mtim.tv_sec!=last_db.st_mtim.tv_sec || before.st_mtim.tv_nsec!=last_db.st_mtim.tv_nsec;
    if(rebuild){ inventory_art=art; artstore_snapshot_begin(library); }
    int rc=SQLITE_ERROR;
    if(ok && rebuild){
        while((rc=sqlite3_step(q))==SQLITE_ROW){
            const char *track=(const char*)sqlite3_column_text(q,0); char key[24];
            if(!track || !*track || artcache_key(track,key,sizeof key) || !artstore_snapshot_add(key)){ ok=0; break; }
        }
        ok=ok && rc==SQLITE_DONE;
    }else if(ok){ /* Establish the shared read lock even with the cached key list. */
        rc=sqlite3_step(q); ok=rc==SQLITE_ROW || rc==SQLITE_DONE;
    }
    if(q) sqlite3_finalize(q);
    ok=ok && wake==atomic_load(&maintenance_wake) &&
        !scanner_active() && library==ipc_library_epoch() && media==sd_io_generation() &&
        !stat(DB_PATH,&after) && before.st_dev==after.st_dev && before.st_ino==after.st_ino && before.st_ctim.tv_sec==after.st_ctim.tv_sec && before.st_ctim.tv_nsec==after.st_ctim.tv_nsec && before.st_size==after.st_size && before.st_mtim.tv_sec==after.st_mtim.tv_sec && before.st_mtim.tv_nsec==after.st_mtim.tv_nsec;
    if(rebuild) artstore_snapshot_finish(ok);
    if(ok){
        last_wake=wake;
        last_library=library; last_media=media; last_db=before;
        art_library_stamp=before; art_library_epoch=library; art_media_epoch=media;
        worked=artstore_maintain_checked(library,art_library_unchanged);
        if(worked==0){ finished_wake=wake; finished_library=library; finished_media=media; finished_art=inventory_art; }
    }else{ artstore_snapshot_begin(library); artstore_snapshot_finish(0); last_library=UINT64_MAX; }
    if(d){ sqlite3_exec(d,"ROLLBACK",NULL,NULL,NULL); sqlite3_close(d); }
    sd_io_end(); return worked>0;
}
