/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Private resumable legacy importer. The durable retirement manifest is committed
 * with the candidate set (or an explicit superseded/evicted decision). A new
 * process verifies its BLOB hashes before it may resume any deletion. */
#define LEGACY_FILES 64
struct legacy_file { char name[64]; uint64_t size; unsigned char sha[32]; };
struct legacy_manifest { unsigned n; struct legacy_file file[LEGACY_FILES]; };
static char (*snapshot_keys)[24];
static size_t snapshot_n, snapshot_cap;
static uint64_t snapshot_library, snapshot_media;
static int snapshot_complete, snapshot_building;
static int (*cleanup_guard)(void);
static int retirement_current(void){ return quota_current() && (!cleanup_guard || cleanup_guard()); }
static char (*migration_dirs)[24];
static size_t migration_n,migration_pos;
static int migration_inventoried;
static int keycmp(const void *a,const void *b){ return strcmp(a,b); }
static int hex_key(const char *s){
    if(strlen(s)!=16) return 0;
    for(int i=0;i<16;i++) if(!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f'))) return 0;
    return 1;
}
void artstore_snapshot_begin(uint64_t library){
    pthread_mutex_lock(&store_mu);
    snapshot_complete=0; snapshot_building=1; snapshot_n=0; migration_n=migration_pos=0; migration_inventoried=0;
    snapshot_library=library; snapshot_media=sd_io_generation();
    pthread_mutex_unlock(&store_mu);
}
int artstore_snapshot_add(const char *key){
    pthread_mutex_lock(&store_mu); int ok=snapshot_building && hex_key(key);
    if(ok && snapshot_n==snapshot_cap){
        size_t cap=snapshot_cap?snapshot_cap*2:1024;
        /* At most 768 KiB for a complete key snapshot. Bigger libraries fail closed. */
        void *p=cap<=32768?realloc(snapshot_keys,cap*sizeof *snapshot_keys):NULL;
        if(p){ snapshot_keys=p; snapshot_cap=cap; }else ok=0;
    }
    if(ok) snprintf(snapshot_keys[snapshot_n++],24,"%s",key);
    else snapshot_building=0;
    pthread_mutex_unlock(&store_mu); return ok;
}
void artstore_snapshot_finish(int complete){
    pthread_mutex_lock(&store_mu);
    /* Admission/generation, rather than the last connection's quota token. */
    snapshot_complete=complete && snapshot_building && sd_io_allowed() && snapshot_media==sd_io_generation();
    if(snapshot_complete) qsort(snapshot_keys,snapshot_n,sizeof *snapshot_keys,keycmp);
    snapshot_building=0;
    pthread_mutex_unlock(&store_mu);
}
static int snapshot_live(const char *key){ return snapshot_n && bsearch(key,snapshot_keys,snapshot_n,sizeof *snapshot_keys,keycmp)!=NULL; }
static int generated_name(const char *name){
    const char *bases[]={"c.bmp","t.bmp","b.bmp","resolution-v1"};
    for(unsigned i=0;i<4;i++){
        size_t n=strlen(bases[i]);
        if(!strcmp(name,bases[i])) return 1;
        if(!strncmp(name,bases[i],n) && !strncmp(name+n,".tmp",4)){
            const char *s=name+n+4; if(!*s) return 0;
            for(;*s;s++) if(*s<'0' || *s>'9') return 0;
            return strlen(name)<64;
        }
    }
    return 0;
}
static int file_hash(int dir,const char *name,struct legacy_file *out,struct stat *witness){
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC); if(fd<0) return 0;
    struct stat a,b; int ok=!fstat(fd,&a) && S_ISREG(a.st_mode) && a.st_nlink==1 && a.st_size>=0 && a.st_size<=(2<<20);
    unsigned char buf[16384]; mbedtls_sha256_context h; mbedtls_sha256_init(&h); mbedtls_sha256_starts(&h,0);
    uint64_t count=0;
    while(ok){
        ssize_t n=read(fd,buf,sizeof buf);
        if(n<0 && errno==EINTR) continue;
        if(n<0){ ok=0; break; } if(!n) break;
        count+=n; if(count>(2<<20) || mbedtls_sha256_update(&h,buf,n) || !quota_current()){ ok=0; break; }
    }
    if(ok) ok=!fstat(fd,&b) && a.st_size==b.st_size && a.st_mtim.tv_sec==b.st_mtim.tv_sec && a.st_mtim.tv_nsec==b.st_mtim.tv_nsec &&
        a.st_ctim.tv_sec==b.st_ctim.tv_sec && a.st_ctim.tv_nsec==b.st_ctim.tv_nsec && count==(uint64_t)a.st_size &&
        !mbedtls_sha256_finish(&h,out->sha);
    mbedtls_sha256_free(&h); close(fd);
    if(ok){ if(witness) *witness=b; snprintf(out->name,sizeof out->name,"%s",name); out->size=count; }
    return ok;
}
static int inventory(int dir,struct legacy_manifest *m){
    memset(m,0,sizeof *m);
    int dupfd=dup(dir); if(dupfd<0) return 0;
    DIR *d=fdopendir(dupfd); if(!d){ close(dupfd); return 0; }
    int ok=1;
    for(;;){
        errno=0; struct dirent *e=readdir(d);
        if(!e){ if(errno) ok=0; break; }
        if(!generated_name(e->d_name)) continue; /* unknown files never enter a deletion manifest */
        struct stat st;
        if(fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){ ok=0; break; }
        if(!S_ISREG(st.st_mode) || st.st_nlink!=1) continue;
        if(m->n==LEGACY_FILES || !file_hash(dir,e->d_name,&m->file[m->n],NULL)){ ok=0; break; }
        m->n++;
    }
    closedir(d); return ok;
}
static const struct legacy_file *manifest_file(const struct legacy_manifest *m,const char *name){
    for(unsigned i=0;i<m->n;i++) if(!strcmp(m->file[i].name,name)) return &m->file[i];
    return NULL;
}
static int same_file(const struct legacy_file *a,const struct legacy_file *b){
    return a && b && a->size==b->size && !memcmp(a->sha,b->sha,32);
}
static int verify_import(sqlite3_int64 id,const struct legacy_manifest *m){
    const char *names[]={"c.bmp","t.bmp","b.bmp"},*cols[]={"c","t","b"};
    for(int i=0;i<3;i++){
        unsigned char hash[32]; const struct legacy_file *f=manifest_file(m,names[i]);
        if(!f || !blob_copy(id,cols[i],-1,0,hash) || memcmp(hash,f->sha,32)) return 0;
    }
    return quota_current();
}
static int legacy_resolution(int dir,const char *key,art_resolution *r){
    int fd=openat(dir,"resolution-v1",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0) return 0;
    struct stat st; int ok=!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_nlink==1 && st.st_size==sizeof *r && read(fd,r,sizeof *r)==sizeof *r;
    close(fd);
    return ok && r->state==ART_PRESENT && r->picture.length && r->picture.length<=(8u<<20) && r->candidate<16 &&
        memchr(r->revision,0,sizeof r->revision) && memchr(r->derivative,0,sizeof r->derivative) && !strcmp(r->derivative,key) &&
        memchr(r->source,0,sizeof r->source);
}
static int retire_open(int root,const char *key){
    int dir=safe_dir(root,key,0);
    if(dir<0){
        if(errno==ENOENT && retirement_current()){
            sqlite3_stmt *q=stmt("DELETE FROM retirement WHERE key=?");
            if(q){ textarg(q,1,key); step_done(q); }
        }
        return 0;
    }
    struct legacy_manifest m; memset(&m,0,sizeof m); int reason=0; sqlite3_int64 id=0;
    sqlite3_stmt *q=stmt("SELECT manifest,reason,pic FROM retirement WHERE key=?");
    if(q){
        textarg(q,1,key);
        if(sqlite3_step(q)==SQLITE_ROW && sqlite3_column_bytes(q,0)==sizeof m){
            memcpy(&m,sqlite3_column_blob(q,0),sizeof m); reason=sqlite3_column_int(q,1); id=sqlite3_column_int64(q,2);
        }
        sqlite3_finalize(q);
    }
    int ok=m.n<=LEGACY_FILES && (reason==0 || reason==1 || reason==2);
    if(!reason){
        if(!inventory(dir,&m)){ close(dir); return 0; }
        art_resolution old={0}, current={0}; int full=0;
        /* Older warm entries may predate resolution-v1. Their pixels cannot
         * establish encoded identity: retain complete live sets until a current
         * demand proof supplies a canonical replacement. Orphans remain eligible
         * for generated-cache eviction under the complete snapshot. */
        int mapped=record_open(key,NULL,&current,&full);
        if(!mapped && snapshot_live(key) && !legacy_resolution(dir,key,&old)){
            const char *names[]={"c.bmp","t.bmp","b.bmp"}; int complete=1;
            for(int i=0;i<3;i++){
                int fd=openat(dir,names[i],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
                if(fd<0 || !valid_fd(fd,i==0?148:i==1?42:360)) complete=0;
                if(fd>=0) close(fd);
            }
            if(complete){ close(dir); return 0; }
        }
        reason=2; /* superseded, or explicitly selected for generated-cache eviction */
        if(!record_open(key,NULL,&current,&full) && snapshot_live(key) && legacy_resolution(dir,key,&old)){
            const char *names[]={"c.bmp","t.bmp","b.bmp"}; int files[3]={-1,-1,-1};
            ok=1;
            for(int i=0;i<3;i++){
                files[i]=openat(dir,names[i],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
                char path[96]; snprintf(path,sizeof path,"/proc/self/fd/%d/%s",dir,names[i]);
                if(files[i]<0 || !art_bmp_valid(path,i==0?148:i==1?42:360)) ok=0;
            }
            if(ok) ok=save_open(old.source,key,&old,files,0,&id);
            for(int i=0;i<3;i++) if(files[i]>=0) close(files[i]);
            if(!ok){ close(dir); return 0; }
            reason=1;
        }
        /* Import and manifest use separate commits: a crash between them leaves
         * all legacy files intact. Recovery re-inventories and verifies the set. */
        if(record_open(key,NULL,&current,&full)){
            if(legacy_resolution(dir,key,&old) && art_picture_equal(&old.picture,&current.picture) && full){
                id=find_picture(&current); reason=1;
            }
        }
        if(!sql("BEGIN IMMEDIATE")){ close(dir); return 0; }
        q=stmt("INSERT OR REPLACE INTO retirement(key,manifest,reason,pic) VALUES(?,?,?,?)");
        if(q){
            textarg(q,1,key); sqlite3_bind_blob(q,2,&m,sizeof m,SQLITE_TRANSIENT);
            sqlite3_bind_int(q,3,reason); sqlite3_bind_int64(q,4,id); ok=step_done(q);
        }else ok=0;
        if(!ok || !retirement_current() || !sql("COMMIT")){ sql("ROLLBACK"); close(dir); return 0; }
        ARTSTORE_EVENT("manifest-committed");
    }
    if(!ok || (reason==1 && !verify_import(id,&m))){ close(dir); return 0; }
    /* A prior failed commit may have unlinked its journal without making
     * that unlink durable. Re-establish durability before retiring files. */
    if(!retirement_current() || fsync(quota_dir)){ close(dir); return 0; }
    ARTSTORE_EVENT("import-verified");
    /* Fixed order: derivatives, resolution record, then inventoried temps.
     * Every retry verifies exact content again. Symlinks/unknown files survive. */
    const char *first[]={"c.bmp","t.bmp","b.bmp","resolution-v1"};
    for(unsigned pass=0;pass<5 && ok;pass++) for(unsigned i=0;i<m.n && ok;i++){
        const struct legacy_file *f=&m.file[i];
        if(!memchr(f->name,0,sizeof f->name) || !generated_name(f->name)){ ok=0; break; }
        int which=4; for(int j=0;j<4;j++) if(!strcmp(f->name,first[j])) which=j;
        if(which!=(int)pass) continue;
        struct stat st;
        if(fstatat(dir,f->name,&st,AT_SYMLINK_NOFOLLOW)){
            if(errno==ENOENT) continue;
            ok=0; break;
        }
        struct legacy_file now; struct stat witness,again;
        if(!file_hash(dir,f->name,&now,&witness) || !same_file(f,&now) || !retirement_current()){ ok=0; break; }
        ARTSTORE_EVENT("before-unlink");
        if(!retirement_current() || fstatat(dir,f->name,&again,AT_SYMLINK_NOFOLLOW) || !S_ISREG(again.st_mode) ||
           again.st_nlink!=1 || again.st_dev!=witness.st_dev || again.st_ino!=witness.st_ino || again.st_size!=witness.st_size ||
           again.st_mtim.tv_sec!=witness.st_mtim.tv_sec || again.st_mtim.tv_nsec!=witness.st_mtim.tv_nsec ||
           again.st_ctim.tv_sec!=witness.st_ctim.tv_sec || again.st_ctim.tv_nsec!=witness.st_ctim.tv_nsec ||
           unlinkat(dir,f->name,0)){ ok=0; break; }
        ARTSTORE_EVENT("after-unlink-before-directory-sync");
        if(!retirement_current() || fsync(dir)){ ok=0; break; }
        ARTSTORE_EVENT(f->name);
    }
    close(dir);
    if(ok && retirement_current()){
        /* rmdir only removes an empty directory; an unknown file always keeps it. */
        ARTSTORE_EVENT("before-rmdir");
        if(!retirement_current()){ ok=0; }
        else if(unlinkat(root,key,AT_REMOVEDIR) && errno!=ENOTEMPTY && errno!=EEXIST && errno!=ENOENT) ok=0;
        ARTSTORE_EVENT("after-rmdir-before-parent-sync");
        if(ok && fsync(root)) ok=0;
        ARTSTORE_EVENT("directory-retired");
        q=stmt("DELETE FROM retirement WHERE key=?"); if(q){ textarg(q,1,key); step_done(q); }
    }
    return ok;
}
int artstore_maintain_checked(uint64_t library,int (*current)(void)){
    if(!store_open(1)) return -1;
    if(!snapshot_complete || library!=snapshot_library || quota_media!=snapshot_media){ store_close(); return -1; }
    cleanup_guard=current;
    if(!retirement_current()){ cleanup_guard=NULL; store_close(); return -1; }
    int worked=0;
    int root=safe_dir(quota_dir,"artcache",0);
    if(root>=0){
        if(!migration_inventoried){
            int dupfd=dup(root); DIR *d=dupfd>=0?fdopendir(dupfd):NULL;
            if(!d && dupfd>=0) close(dupfd);
            size_t n=0; int valid=d!=NULL;
            if(!migration_dirs) migration_dirs=malloc(32768*sizeof *migration_dirs);
            if(!migration_dirs) valid=0;
            if(d){
                for(;;){
                    errno=0; struct dirent *e=readdir(d);
                    if(!e){ if(errno) valid=0; break; }
                    if(!hex_key(e->d_name)) continue;
                    if(!valid || n==32768){ valid=0; continue; }
                    snprintf(migration_dirs[n++],24,"%.16s",e->d_name);
                }
                closedir(d);
            }
            if(valid){
                sqlite3_stmt *q=stmt("SELECT key FROM retirement");
                int rc=SQLITE_ERROR;
                if(q){
                    while((rc=sqlite3_step(q))==SQLITE_ROW){
                        const char *key=(const char*)sqlite3_column_text(q,0);
                        if(!key || !hex_key(key) || n==32768){ valid=0; break; }
                        snprintf(migration_dirs[n++],24,"%s",key);
                    }
                    sqlite3_finalize(q);
                }
                if(rc!=SQLITE_DONE) valid=0;
            }
            if(valid && quota_current()){
                qsort(migration_dirs,n,sizeof *migration_dirs,keycmp);
                size_t unique=0;
                for(size_t i=0;i<n;i++) if(!unique || strcmp(migration_dirs[i],migration_dirs[unique-1])){
                    if(unique!=i) memcpy(migration_dirs[unique],migration_dirs[i],24);
                    unique++;
                }
                migration_n=unique; migration_pos=0; migration_inventoried=1;
            }
        }
        if(migration_inventoried && migration_pos<migration_n && retirement_current()){
            retire_open(root,migration_dirs[migration_pos++]); worked=1;
        }else if(!migration_inventoried) worked=-1;
        close(root);
    }else if(errno!=ENOENT) worked=-1;
    /* Small incremental reclamation only in this explicitly idle maintenance call. */
    if(quota_current()){ flush_touches(); sql("PRAGMA incremental_vacuum(32)"); }
    cleanup_guard=NULL; store_close(); return worked;
}
int artstore_maintain(uint64_t library){ return artstore_maintain_checked(library,NULL); }
