/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Private SQLite VFS shim. Model: sqlite.org/vfs.html (quota shim).
 * Included by artstore.c; the store mutex serializes every use of this VFS.
 * WAL, mmap, preallocation, attached DBs and temporary disk databases are disabled.
 * Check allocation BEFORE growth, including a rollback journal's cluster tail. */
typedef struct { sqlite3_file base; sqlite3_file *real; sqlite3_int64 size; int kind; } quota_file;
static sqlite3_vfs quota_vfs, *quota_parent;
static sqlite3_int64 quota_sizes[2];
static uint64_t quota_cluster, quota_media;
static int quota_dir = -1;
static uint64_t quota_round(uint64_t n){ return n ? ((n-1)/quota_cluster+1)*quota_cluster : 0; }
static int quota_current(void){ return sd_io_allowed() && sd_io_generation()==quota_media; }
static int quota_grow(quota_file *q, sqlite3_int64 end){
    if(end<0 || !quota_current()) return SQLITE_IOERR_WRITE;
    if(end<=q->size) return SQLITE_OK;
    uint64_t old=quota_round(q->size), next=quota_round(end);
    if(next>old){
        struct statvfs v;
        uint64_t other=quota_round(quota_sizes[1-q->kind]);
        if(next>ARTSTORE_BUDGET || other>ARTSTORE_BUDGET-next) return SQLITE_FULL;
        if(fstatvfs(quota_dir,&v)) return SQLITE_IOERR_FSTAT;
        uint64_t free=(uint64_t)v.f_bavail*v.f_frsize;
        if(free<ARTSTORE_RESERVE || next-old>free-ARTSTORE_RESERVE) return SQLITE_FULL;
    }
    return SQLITE_OK;
}
static int q_close(sqlite3_file *f){ quota_file *q=(void*)f; return q->real->pMethods->xClose(q->real); }
static int q_read(sqlite3_file *f,void *p,int n,sqlite3_int64 o){
    quota_file *q=(void*)f;
    if(!quota_current()) return SQLITE_IOERR_READ;
    return q->real->pMethods->xRead(q->real,p,n,o);
}
static int q_write(sqlite3_file *f,const void *p,int n,sqlite3_int64 o){
    quota_file *q=(void*)f;
    if(o<0 || n<0 || o>INT64_MAX-n) return SQLITE_FULL;
    ARTSTORE_EVENT("sqlite-before-write");
    int rc=quota_grow(q,o+n);
    if(rc==SQLITE_OK) rc=q->real->pMethods->xWrite(q->real,p,n,o);
    /* Query after errors too: a failed write can have partially extended a file. */
    if(rc==SQLITE_OK){ if(o+n>q->size) q->size=o+n; }
    else q->real->pMethods->xFileSize(q->real,&q->size);
    quota_sizes[q->kind]=q->size;
    ARTSTORE_EVENT("sqlite-after-write");
    return rc;
}
static int q_truncate(sqlite3_file *f,sqlite3_int64 n){
    ARTSTORE_EVENT("sqlite-before-truncate");
    quota_file *q=(void*)f; int rc=quota_grow(q,n);
    if(rc==SQLITE_OK) rc=q->real->pMethods->xTruncate(q->real,n);
    q->real->pMethods->xFileSize(q->real,&q->size); quota_sizes[q->kind]=q->size;
    ARTSTORE_EVENT("sqlite-after-truncate");
    return rc;
}
static int q_sync(sqlite3_file *f,int flags){
    quota_file *q=(void*)f;
    ARTSTORE_EVENT("sqlite-before-sync");
    int rc=quota_current() ? q->real->pMethods->xSync(q->real,flags) : SQLITE_IOERR_FSYNC;
    if(rc==SQLITE_OK && q->kind){
        ARTSTORE_EVENT("sqlite-before-journal-directory-sync");
        if(!quota_current() || fsync(quota_dir)) rc=SQLITE_IOERR_DIR_FSYNC;
        ARTSTORE_EVENT("sqlite-after-journal-directory-sync");
    }
    ARTSTORE_EVENT("sqlite-after-sync"); return rc;
}
static int q_size(sqlite3_file *f,sqlite3_int64 *n){ quota_file *q=(void*)f; return q->real->pMethods->xFileSize(q->real,n); }
static int q_lock(sqlite3_file *f,int n){ quota_file *q=(void*)f; return q->real->pMethods->xLock(q->real,n); }
static int q_unlock(sqlite3_file *f,int n){ quota_file *q=(void*)f; return q->real->pMethods->xUnlock(q->real,n); }
static int q_reserved(sqlite3_file *f,int *n){ quota_file *q=(void*)f; return q->real->pMethods->xCheckReservedLock(q->real,n); }
static int q_control(sqlite3_file *f,int op,void *arg){
    quota_file *q=(void*)f;
    if(op==SQLITE_FCNTL_SIZE_HINT || op==SQLITE_FCNTL_CHUNK_SIZE) return SQLITE_NOTFOUND;
    return q->real->pMethods->xFileControl(q->real,op,arg);
}
static int q_sector(sqlite3_file *f){ quota_file *q=(void*)f; return q->real->pMethods->xSectorSize(q->real); }
static int q_device(sqlite3_file *f){ (void)f; return 0; } /* assume no atomic-write/powersafe shortcuts on exFAT */
static const sqlite3_io_methods quota_io={
    .iVersion=1,.xClose=q_close,.xRead=q_read,.xWrite=q_write,.xTruncate=q_truncate,
    .xSync=q_sync,.xFileSize=q_size,.xLock=q_lock,.xUnlock=q_unlock,
    .xCheckReservedLock=q_reserved,.xFileControl=q_control,.xSectorSize=q_sector,.xDeviceCharacteristics=q_device
};
static int q_open(sqlite3_vfs *v,const char *name,sqlite3_file *f,int flags,int *out){
    (void)v; quota_file *q=(void*)f; memset(q,0,sizeof *q);
    if(!name || !(flags&(SQLITE_OPEN_MAIN_DB|SQLITE_OPEN_MAIN_JOURNAL)) || !quota_current()) return SQLITE_CANTOPEN;
    q->kind=(flags&SQLITE_OPEN_MAIN_JOURNAL)!=0;
    const char *base=strrchr(name,'/'); base=base?base+1:name;
    if(strcmp(base,q->kind ? "artcache-v2.sqlite-journal" : "artcache-v2.sqlite")) return SQLITE_CANTOPEN;
    struct stat st;
    if(!fstatat(quota_dir,base,&st,AT_SYMLINK_NOFOLLOW)){
        if(!S_ISREG(st.st_mode) || st.st_nlink!=1) return SQLITE_CANTOPEN;
    }else if(errno!=ENOENT) return SQLITE_CANTOPEN;
    q->real=(void*)(q+1);
    int rc=quota_parent->xOpen(quota_parent,name,q->real,flags|SQLITE_OPEN_NOFOLLOW,out);
    if(rc!=SQLITE_OK) return rc;
    rc=q->real->pMethods->xFileSize(q->real,&q->size);
    if(rc!=SQLITE_OK){ q->real->pMethods->xClose(q->real); return rc; }
    quota_sizes[q->kind]=q->size; q->base.pMethods=&quota_io; return SQLITE_OK;
}
static int q_delete(sqlite3_vfs *v,const char *name,int sync){
    (void)v;
    ARTSTORE_EVENT("sqlite-before-journal-delete");
    if(!quota_current()) return SQLITE_IOERR_DELETE;
    int rc=quota_parent->xDelete(quota_parent,name,sync);
    if(rc==SQLITE_OK){
        quota_sizes[1]=0;
        ARTSTORE_EVENT("sqlite-before-delete-directory-sync");
        if(!quota_current() || fsync(quota_dir)) rc=SQLITE_IOERR_DIR_FSYNC;
        ARTSTORE_EVENT("sqlite-after-delete-directory-sync");
    }
    ARTSTORE_EVENT("sqlite-after-journal-delete");
    return rc;
}
static void quota_init(void){
    quota_parent=sqlite3_vfs_find(NULL); quota_vfs=*quota_parent;
    quota_vfs.zName="diskos-artquota"; quota_vfs.pNext=NULL;
    quota_vfs.szOsFile=sizeof(quota_file)+quota_parent->szOsFile;
    quota_vfs.xOpen=q_open; quota_vfs.xDelete=q_delete;
    sqlite3_vfs_register(&quota_vfs,0);
}
