/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef LIBRARY_H
#define LIBRARY_H
#include "sqlite3.h"
#include <string.h>

/* Private sort storage. Keep the scanner and catalogue behind this interface;
 * a sidecar backend must not change tag parsing, grouping or playback. */
#define LIBSORT_LEN 256
typedef struct { char value[3][LIBSORT_LEN]; unsigned native_aa; } libsort_tags;
static const char libsort_schema[] =
    "CREATE TABLE DISKOS_SORT_V1 (SONG_ID INTEGER PRIMARY KEY, PATH TEXT, TRACK INT, IS_CUE INT, IS_ISO INT,"
    " ARTIST TEXT, ALBUM_ARTIST TEXT, ALBUM TEXT, ARTIST_CODE INT, ALBUM_ARTIST_CODE INT, ALBUM_CODE INT,"
    " ARTIST_SORT TEXT, ALBUM_ARTIST_SORT TEXT, ALBUM_SORT TEXT)";
static const char libsort_update[] =
    "CREATE TRIGGER DISKOS_SORT_V1_UPDATE AFTER UPDATE OF ID,PATH,TRACK,DISC,IS_CUE,IS_ISO,NAME,TITLE,"
    "ARTIST,ALBUM_ARTIST,ALBUM,GENRE,NAME_CODE,TITLE_CODE,ARTIST_CODE,ALBUM_ARTIST_CODE,ALBUM_CODE,GENRE_CODE"
    " ON SONG BEGIN DELETE FROM DISKOS_SORT_V1 WHERE SONG_ID=OLD.ID OR SONG_ID=NEW.ID OR SONG_ID=0; END";
static const char libsort_delete[] =
    "CREATE TRIGGER DISKOS_SORT_V1_DELETE AFTER DELETE ON SONG BEGIN"
    " DELETE FROM DISKOS_SORT_V1 WHERE SONG_ID=OLD.ID OR SONG_ID=0; END";

/* Complete, bounded UTF-8 only. Invalid/oversized values never become prefixes. */
static inline int libsort_blank(unsigned char c){ return c==' ' || (c>='\t' && c<='\r'); }
static inline int libsort_text(char out[LIBSORT_LEN], const unsigned char *p, size_t n){
    if(n>=LIBSORT_LEN){ out[0]=0; return 0; }
    while(n && libsort_blank(*p)){ p++; n--; }
    while(n && libsort_blank(p[n-1])) n--;
    out[0]=0;
    if(!n || n >= LIBSORT_LEN) return 0;
    for(size_t i=0;i<n;){
        unsigned c=p[i++], cp=c, more=0, min=0;
        if(c<32 || c==127) return 0;
        if(c>=128){
            if(c>=0xc2 && c<=0xdf){ more=1; cp=c&31; min=128; }
            else if(c>=0xe0 && c<=0xef){ more=2; cp=c&15; min=2048; }
            else if(c>=0xf0 && c<=0xf4){ more=3; cp=c&7; min=65536; }
            else return 0;
            if(more>n-i) return 0;
            while(more--){ if((p[i]&0xc0)!=0x80) return 0; cp=(cp<<6)|(p[i++]&63); }
            if(cp<min || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return 0;
        }
    }
    memcpy(out,p,n); out[n]=0; return 1;
}

/* 1 = exact contract, 0 = missing/unsupported, -1 = read failure. */
static inline int libsort_contract(sqlite3 *db){
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db,"SELECT name,sql FROM sqlite_master WHERE name IN"
        " ('DISKOS_SORT_V1','DISKOS_SORT_V1_UPDATE','DISKOS_SORT_V1_DELETE');",-1,&st,NULL);
    if(rc!=SQLITE_OK) return -1;
    unsigned seen=0;
    while((rc=sqlite3_step(st))==SQLITE_ROW){
        const char *name=(const char *)sqlite3_column_text(st,0), *sql=(const char *)sqlite3_column_text(st,1);
        const char *want=!strcmp(name,"DISKOS_SORT_V1") ? libsort_schema :
                         !strcmp(name,"DISKOS_SORT_V1_UPDATE") ? libsort_update : libsort_delete;
        if(!sql || strcmp(sql,want)){ sqlite3_finalize(st); return 0; }
        seen |= want==libsort_schema ? 1 : want==libsort_update ? 2 : 4;
    }
    sqlite3_finalize(st);
    return rc==SQLITE_DONE ? seen==7 : -1;
}
static inline int libsort_setup(sqlite3 *db){
    if(sqlite3_get_autocommit(db)) return 0;
    sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(db,"SELECT sql FROM sqlite_master WHERE name='DISKOS_SORT_V1';",-1,&st,NULL)!=SQLITE_OK) return 0;
    int rc=sqlite3_step(st), exists=rc==SQLITE_ROW;
    int match=exists && sqlite3_column_text(st,0) && !strcmp((const char *)sqlite3_column_text(st,0),libsort_schema);
    sqlite3_finalize(st);
    if((exists && !match) || (rc!=SQLITE_ROW && rc!=SQLITE_DONE)) return 0;
    if(!exists && sqlite3_exec(db,libsort_schema,0,0,0)!=SQLITE_OK) return 0;
    int trusted=libsort_contract(db);
    if(trusted<0) return 0;
    if(!trusted && sqlite3_exec(db,"DELETE FROM DISKOS_SORT_V1;",0,0,0)!=SQLITE_OK) return 0;
    static const char *const steps[]={
        "DROP TRIGGER IF EXISTS DISKOS_SORT_V1_UPDATE;", "DROP TRIGGER IF EXISTS DISKOS_SORT_V1_DELETE;",
        libsort_update, libsort_delete, "DELETE FROM DISKOS_SORT_V1 WHERE SONG_ID=0;"
    };
    for(unsigned i=0;i<sizeof steps/sizeof steps[0];i++)
        if(sqlite3_get_autocommit(db) || sqlite3_exec(db,steps[i],0,0,0)!=SQLITE_OK) return 0;
    return libsort_contract(db)==1;
}
static inline int libsort_write(sqlite3 *db, const char *path, const libsort_tags *tags){
    if(sqlite3_get_autocommit(db)) return 0;
    if(!tags->value[0][0] && !tags->value[1][0] && !tags->value[2][0]) return 1;
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO DISKOS_SORT_V1 SELECT ID,PATH,TRACK,IS_CUE,IS_ISO,ARTIST,ALBUM_ARTIST,ALBUM,"
        "ARTIST_CODE,ALBUM_ARTIST_CODE,ALBUM_CODE,?2,?3,?4 FROM SONG WHERE PATH=?1"
        " AND COALESCE(IS_CUE,0)=0 AND COALESCE(IS_ISO,0)=0;",-1,&st,NULL);
    if(rc==SQLITE_OK) rc=sqlite3_bind_text(st,1,path,-1,SQLITE_STATIC);
    for(int i=0;i<3 && rc==SQLITE_OK;i++)
        rc=tags->value[i][0] ? sqlite3_bind_text(st,i+2,tags->value[i],-1,SQLITE_STATIC) : sqlite3_bind_null(st,i+2);
    if(rc==SQLITE_OK) rc=sqlite3_step(st);
    sqlite3_finalize(st); return rc==SQLITE_DONE;
}
static inline int libsort_finish(sqlite3 *db){
    if(sqlite3_get_autocommit(db)) return 0;
    return sqlite3_exec(db,"DELETE FROM DISKOS_SORT_V1 WHERE SONG_ID>0 AND NOT EXISTS"
        " (SELECT 1 FROM SONG WHERE ID=SONG_ID);",0,0,0)==SQLITE_OK
        && sqlite3_exec(db,"INSERT OR REPLACE INTO DISKOS_SORT_V1(SONG_ID,PATH) VALUES(0,'complete-v1');",0,0,0)==SQLITE_OK;
}
/* Returns 1 with a prepared sparse reader, 0 for legacy order, -1 on failure. */
static inline int libsort_reader(sqlite3 *db, sqlite3_stmt **out){
    *out=NULL;
    int ready=libsort_contract(db);
    if(ready!=1) return ready;
    sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(db,"SELECT 1 FROM DISKOS_SORT_V1 WHERE SONG_ID=0 AND PATH='complete-v1';",-1,&st,NULL)!=SQLITE_OK) return -1;
    int rc=sqlite3_step(st); sqlite3_finalize(st);
    if(rc==SQLITE_DONE) return 0;
    if(rc!=SQLITE_ROW) return -1;
    rc=sqlite3_prepare_v2(db,"SELECT S.ID,D.ARTIST_SORT,D.ALBUM_ARTIST_SORT,D.ALBUM_SORT,"
        "S.ARTIST,S.ALBUM_ARTIST,S.ALBUM,COALESCE(S.ALBUM_ARTIST,S.ARTIST) FROM DISKOS_SORT_V1 D JOIN SONG S ON S.ID=D.SONG_ID"
        " WHERE S.PATH IS D.PATH AND S.TRACK IS D.TRACK AND S.IS_CUE IS D.IS_CUE AND S.IS_ISO IS D.IS_ISO"
        " AND S.ARTIST IS D.ARTIST AND S.ALBUM_ARTIST IS D.ALBUM_ARTIST AND S.ALBUM IS D.ALBUM"
        " AND S.ARTIST_CODE IS D.ARTIST_CODE AND S.ALBUM_ARTIST_CODE IS D.ALBUM_ARTIST_CODE AND S.ALBUM_CODE IS D.ALBUM_CODE"
        " AND lower(S.PATH) NOT LIKE '%.m4b' ORDER BY S.ID;",-1,out,NULL);
    return rc==SQLITE_OK ? 1 : -1;
}
#endif
