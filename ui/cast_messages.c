/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cast_messages.h"
#define JSMN_STATIC
#define JSMN_STRICT
#include "jsmn.h"
#include <math.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
/* jsmn (MIT, already vendored) supplies bounded tokens. Validate punctuation,
 * primitives and decoded key uniqueness too: tokenization alone is permissive. */
typedef struct { const char *s; jsmntok_t t[512]; int next[512], n; } json;
static int ws(int c) { return c==' '||c=='\r'||c=='\n'||c=='\t'; }
static int utf8(const unsigned char **p, const unsigned char *e) {
    unsigned c=*(*p)++, v, min; int n;
    if(c<128)return c>=32;
    if(c>=0xc2&&c<=0xdf) { n=1;v=c&31;min=128; }
    else if(c>=0xe0&&c<=0xef) { n=2;v=c&15;min=2048; }
    else if(c>=0xf0&&c<=0xf4) { n=3;v=c&7;min=65536; } else return 0;
    while(n--) { if(*p==e||(**p&192)!=128)return 0;v=(v<<6)|(*(*p)++&63); }
    return v>=min&&v<=0x10ffff&&!(v>=0xd800&&v<=0xdfff);
}
static unsigned hex4(const char *s) {
    unsigned v=0;for(int i=0;i<4;i++)v=v*16+(s[i]<='9'?s[i]-'0':(s[i]|32)-'a'+10);return v;
}
static int text(json *j,int i,char *out,size_t cap) {
    if(i<0||j->t[i].type!=JSMN_STRING)return 0;
    const unsigned char *p=(const unsigned char *)j->s+j->t[i].start,*e=(const unsigned char *)j->s+j->t[i].end;
    size_t n=0;
    while(p<e) {
        unsigned char b[4];size_t k=1;
        if(*p!='\\') { const unsigned char *q=p;if(!utf8(&p,e))return 0;k=p-q;memcpy(b,q,k); }
        else {
            p++;unsigned c=*p++;
            if(c=='u') {
                c=hex4((const char *)p);p+=4;
                if(c>=0xd800&&c<=0xdbff) {
                    if(e-p<6||p[0]!='\\'||p[1]!='u')return 0;
                    unsigned lo=hex4((const char *)p+2);if(lo<0xdc00||lo>0xdfff)return 0;
                    c=0x10000+((c-0xd800)<<10)+lo-0xdc00;p+=6;
                } else if(c>=0xdc00&&c<=0xdfff)return 0;
                if(!c)return 0;
                if(c<128)b[0]=c;
                else if(c<2048) { k=2;b[0]=(unsigned char)(0xc0|(c>>6));b[1]=(unsigned char)(0x80|(c&63)); }
                else if(c<65536) { k=3;b[0]=(unsigned char)(0xe0|(c>>12));b[1]=(unsigned char)(0x80|((c>>6)&63));b[2]=(unsigned char)(0x80|(c&63)); }
                else { k=4;b[0]=(unsigned char)(0xf0|(c>>18));b[1]=(unsigned char)(0x80|((c>>12)&63));b[2]=(unsigned char)(0x80|((c>>6)&63));b[3]=(unsigned char)(0x80|(c&63)); }
            } else { const char *a="\"\\/bfnrt",*z=strchr(a,c);if(!z)return 0;b[0]="\"\\/\b\f\n\r\t"[z-a]; }
        }
        if(n+k>=cap)return 0;
        memcpy(out+n,b,k);n+=k;
    }
    out[n]=0;return 1;
}
static int equal(json *j,int i,const char *s) { return i>=0&&j->t[i].type==JSMN_PRIMITIVE&&(size_t)(j->t[i].end-j->t[i].start)==strlen(s)&&!memcmp(j->s+j->t[i].start,s,strlen(s)); }
static int number(json *j,int i,double *v,int integer) {
    if(i<0||j->t[i].type!=JSMN_PRIMITIVE)return 0;
    int n=j->t[i].end-j->t[i].start;char b[64],*p=b;
    if(n<1||n>63)return 0;
    memcpy(b,j->s+j->t[i].start,n);b[n]=0;
    if(*p=='-')p++;
    if(*p=='0')p++;else { if(*p<'1'||*p>'9')return 0;while(*p>='0'&&*p<='9')p++; }
    if(!integer&&*p=='.') { p++;if(*p<'0'||*p>'9')return 0;while(*p>='0'&&*p<='9')p++; }
    if(!integer&&(*p=='e'||*p=='E')) { p++;if(*p=='+'||*p=='-')p++;if(*p<'0'||*p>'9')return 0;while(*p>='0'&&*p<='9')p++; }
    if(*p)return 0;
    errno=0;*v=strtod(b,NULL);return errno!=ERANGE&&isfinite(*v);
}
static int gap(json *j,int a,int b,int want) {
    while(a<b&&ws(j->s[a]))a++;
    if(want) { if(a==b||j->s[a++]!=want)return 0;while(a<b&&ws(j->s[a]))a++; }
    return a==b;
}
static int start(json *j,int i) { return j->t[i].start-(j->t[i].type==JSMN_STRING); }
static int end(json *j,int i) { return j->t[i].end+(j->t[i].type==JSMN_STRING); }
static int validate(json *j,int i,int depth) {
    if(i>=j->n||depth>16)return -1;
    jsmntok_t *t=&j->t[i];char a[CM_TEXT],b[CM_ID];double v;int k=i+1,pos=t->start+1;
    if(t->type==JSMN_STRING) { if(!text(j,i,a,sizeof a))return -1; }
    else if(t->type==JSMN_PRIMITIVE) { if(!equal(j,i,"null")&&!equal(j,i,"true")&&!equal(j,i,"false")&&!number(j,i,&v,0))return -1; }
    else {
        int first=1;
        while(k<j->n&&start(j,k)<t->end-1) {
            if(!gap(j,pos,start(j,k),first?0:','))return -1;
            if(t->type==JSMN_OBJECT) {
                if(!text(j,k,a,CM_ID))return -1;
                for(int q=i+1;q<k;q=j->next[q+1])if(!text(j,q,b,sizeof b)||!strcmp(a,b))return -1;
                int key=k++;if(k>=j->n||!gap(j,end(j,key),start(j,k),':'))return -1;
            }
            int last=k;k=validate(j,k,depth+1);if(k<0)return -1;
            pos=end(j,last);first=0;
        }
        if(!gap(j,pos,t->end-1,0))return -1;
    }
    return j->next[i]=k;
}
static int field(json *j,int obj,const char *name) {
    if(obj<0||j->t[obj].type!=JSMN_OBJECT)return -2;
    char b[CM_ID];for(int k=obj+1;k<j->next[obj];k=j->next[k+1])if(text(j,k,b,sizeof b)&&!strcmp(b,name))return equal(j,k+1,"null")?-1:k+1;
    return -1;
}
static int strfield(json *j,int o,const char *key,char *b,size_t n) { int i=field(j,o,key);return i==-1||(i>=0&&text(j,i,b,n)); }
static int numfield(json *j,int o,const char *key,double *v,unsigned *bits,unsigned flag,double max,int integer) {
    int i=field(j,o,key);if(i==-1)return 1;
    if(i<0||!number(j,i,v,integer)||*v<0||*v>max)return 0;
    *bits|=flag;return 1;
}
static int volume(json *j,int o,cm_volume *v) {
    if(o==-1)return 1;
    if(!numfield(j,o,"level",&v->level,&v->present,CM_LEVEL,1,0))return 0;
    int i=field(j,o,"muted");if(i==-1)return 1;
    if(i<0||(!equal(j,i,"true")&&!equal(j,i,"false")))return 0;
    v->muted=equal(j,i,"true");v->present|=CM_MUTED;return 1;
}
static int parse_json(json *j,const char *s,size_t n) {
    if(!s||n<2||n>=CM_JSON||memchr(s,0,n))return 0;
    j->s=s;jsmn_parser p;jsmn_init(&p);j->n=jsmn_parse(&p,s,n,j->t,512);
    return j->n>=1&&j->t[0].type==JSMN_OBJECT&&validate(j,0,0)==j->n&&gap(j,0,start(j,0),0)&&gap(j,end(j,0),n,0);
}
int cm_type(const char *s,size_t n,char *out,size_t cap) {
    json j={0};char type[CM_ID];
    if(!out||!cap||!parse_json(&j,s,n)||!text(&j,field(&j,0,"type"),type,sizeof type)||strlen(type)>=cap)return 0;
    memcpy(out,type,strlen(type)+1);return 1;
}
int cm_parse(const char *s,size_t n,int expected,cm_status *out) {
    json j={0};
    if(!out||expected<0||!parse_json(&j,s,n))return 0;
    cm_status r={0};double d=0;unsigned bits=0;
    if(!strfield(&j,0,"type",r.type,sizeof r.type)||!numfield(&j,0,"requestId",&d,&bits,1,INT32_MAX,1))return 0;
    r.request_id=(int)d;
    int receiver=!strcmp(r.type,"RECEIVER_STATUS"),media=!strcmp(r.type,"MEDIA_STATUS");
    if(!receiver&&!media&&strcmp(r.type,"LAUNCH_ERROR")&&strcmp(r.type,"LOAD_FAILED")&&strcmp(r.type,"LOAD_CANCELLED")&&strcmp(r.type,"INVALID_REQUEST")&&strcmp(r.type,"INVALID_PLAYER_STATE"))return 0;
    if(r.request_id!=expected&&r.request_id!=0)return 0;
    if(!receiver&&!media) { if(!r.request_id)return 0;*out=r;return 1; }
    int status=field(&j,0,"status");r.has_status=status>=0;if(status==-1) { *out=r;return 1; }
    if(receiver) {
        if(!volume(&j,field(&j,status,"volume"),&r.volume))return 0;
        int ar=field(&j,status,"applications");if(ar==-2)return 0;
        if(ar>=0) {
            if(j.t[ar].type!=JSMN_ARRAY||j.t[ar].size>CM_ENTRIES)return 0;
            r.has_apps=1;
            for(int k=ar+1;k<j.next[ar];k=j.next[k]) {
                cm_app *a=&r.apps[r.app_count++];
                char name[CM_TEXT]={0};
                if(!strfield(&j,k,"appId",a->app_id,CM_ID)||!strfield(&j,k,"sessionId",a->session_id,CM_ID)||!strfield(&j,k,"transportId",a->transport_id,CM_ID)||!strfield(&j,k,"displayName",name,sizeof name))return 0;
                if(strlen(name)<sizeof a->display_name)strcpy(a->display_name,name);
                int ii=field(&j,k,"isIdleScreen");if(ii!=-1) { if(ii<0||(!equal(&j,ii,"true")&&!equal(&j,ii,"false")))return 0;a->idle=equal(&j,ii,"true"); }
            }
        }
    } else {
        if(status<0||j.t[status].type!=JSMN_ARRAY||j.t[status].size>CM_ENTRIES)return 0;
        for(int k=status+1;k<j.next[status];k=j.next[k]) {
            cm_media *m=&r.media[r.media_count++];d=0;
            if(!numfield(&j,k,"mediaSessionId",&d,&m->present,CM_MEDIA_ID,INT32_MAX,1)||(m->present&&d==0))return 0;
            m->media_session_id=(int)d;
            /* Consumers convert seconds to signed 32-bit milliseconds. */
            if(!numfield(&j,k,"currentTime",&m->current_time,&m->present,CM_TIME,INT32_MAX / 1000.0,0)||!strfield(&j,k,"playerState",m->player_state,CM_ID)||!strfield(&j,k,"idleReason",m->idle_reason,CM_ID)||!volume(&j,field(&j,k,"volume"),&m->volume))return 0;
            int mi=field(&j,k,"media");if(mi!=-1&&!strfield(&j,mi,"contentId",m->content_id,CM_TEXT))return 0;
        }
    }
    *out=r;return 1;
}
int cm_identity(uint64_t g,uint64_t want,const cm_app *a,const cm_media *m,const cm_app *ea,int id,const char *content) {
    return id>=0&&(!content||*content)&&g==want&&(!ea||(*ea->app_id&&*ea->session_id&&*ea->transport_id&&a&&!strcmp(a->app_id,ea->app_id)&&!strcmp(a->session_id,ea->session_id)&&!strcmp(a->transport_id,ea->transport_id)))&&(!id||(m&&(m->present&CM_MEDIA_ID)&&m->media_session_id==id))&&(!content||(m&&!strcmp(m->content_id,content)));
}
typedef struct { char *p;size_t n;int ok; } writer;
static void put(writer *w,const char *fmt,...) {
    if(!w->ok)return;
    va_list ap;va_start(ap,fmt);int n=vsnprintf(w->p,w->n,fmt,ap);va_end(ap);
    if(n<0||(size_t)n>=w->n)w->ok=0;else { w->p+=n;w->n-=n; }
}
static void quote(writer *w,const char *s,size_t cap) {
    if(!s||strnlen(s,cap)>=cap) { w->ok=0;return; }
    put(w,"\"");const unsigned char *p=(const unsigned char *)s,*e=p+strlen(s);
    while(p<e) {
        unsigned c=*p;if(c<32) { put(w,"\\u%04x",c);p++; }
        else if(c=='"'||c=='\\') { put(w,"\\%c",c);p++; }
        else { const unsigned char *q=p;if(!utf8(&p,e)) { w->ok=0;return; }put(w,"%.*s",(int)(p-q),q); }
    }
    put(w,"\"");
}
const char *cm_namespace(const cm_command *c) {
    if(!c||c->kind<CM_CONNECT||c->kind>CM_SET_VOLUME)return NULL;
    const char *suffix=c->kind<=CM_CLOSE?"connection":c->kind<=CM_PONG?"heartbeat":NULL;
    if(suffix)return c->kind<=CM_CLOSE?"urn:x-cast:com.google.cast.tp.connection":"urn:x-cast:com.google.cast.tp.heartbeat";
    return c->kind==CM_LAUNCH||((c->kind==CM_GET_STATUS||c->kind==CM_SET_VOLUME)&&c->receiver)?"urn:x-cast:com.google.cast.receiver":"urn:x-cast:com.google.cast.media";
}
int cm_encode(char *out,size_t cap,const cm_command *c) {
    static const char *types[]={"CONNECT","CLOSE","PING","PONG","GET_STATUS","LAUNCH","LOAD","PLAY","PAUSE","SEEK","STOP","SET_VOLUME"};
    if(!out||!cap||cap>CM_JSON||!cm_namespace(c))return 0;
    char staged[CM_JSON];
    writer w={staged,cap,1};put(&w,"{\"type\":\"%s\"",types[c->kind]);
    if(c->kind==CM_CONNECT && c->sender_name){
        put(&w,",\"origin\":{},\"userAgent\":"); quote(&w,c->sender_name,33);
        put(&w,",\"senderInfo\":{\"displayName\":");
        quote(&w,c->sender_name,33); put(&w,"}");
    }
    if(c->kind>=CM_GET_STATUS) { if(c->request_id<=0)return 0;put(&w,",\"requestId\":%d",c->request_id); }
    if(c->kind==CM_LAUNCH)put(&w,",\"appId\":\"" CM_APP "\"");
    if(c->kind>=CM_PLAY&&!(c->kind==CM_SET_VOLUME&&c->receiver)) {
        if(c->media_session_id<=0)return 0;
        put(&w,",\"mediaSessionId\":%d",c->media_session_id);
    }
    if(c->kind==CM_LOAD||c->kind==CM_SEEK) {
        if(!isfinite(c->time)||c->time<0||c->time>INT32_MAX)return 0;
        put(&w,",\"currentTime\":%.17g",c->time);
    }
    if(c->kind==CM_LOAD) {
        if(!c->content_id||!*c->content_id||!c->content_type||!*c->content_type||!c->stream_type||(strcmp(c->stream_type,"BUFFERED")&&strcmp(c->stream_type,"LIVE")&&strcmp(c->stream_type,"NONE")))return 0;
        put(&w,",\"autoplay\":false,\"media\":{\"contentId\":");quote(&w,c->content_id,CM_TEXT);
        put(&w,",\"contentType\":");quote(&w,c->content_type,CM_ID);put(&w,",\"streamType\":");quote(&w,c->stream_type,CM_ID);
        put(&w,",\"metadata\":{\"metadataType\":3,\"title\":");quote(&w,c->title?c->title:"",CM_TEXT);
        put(&w,",\"artist\":");quote(&w,c->artist?c->artist:"",CM_TEXT);put(&w,",\"albumName\":");quote(&w,c->album?c->album:"",CM_TEXT);
        put(&w,",\"images\":[");if(c->image) { put(&w,"{\"url\":");quote(&w,c->image,CM_TEXT);put(&w,"}"); }put(&w,"]}}");
    }
    if(c->kind==CM_SET_VOLUME) {
        if(!isfinite(c->level)||c->level<0||c->level>1||(c->muted!=0&&c->muted!=1))return 0;
        put(&w,",\"volume\":{\"level\":%.17g,\"muted\":%s}",c->level,c->muted?"true":"false");
    }
    put(&w,"}");
    if(!w.ok)return 0;
    memcpy(out,staged,(size_t)(w.p-staged)+1);
    return 1;
}
int cm_encode_stop(char *out,size_t cap,int id,const char *session) {
    if(!out||!cap||cap>CM_JSON||id<=0||!session||!*session)return 0;
    char staged[CM_JSON];writer w={staged,cap,1};
    put(&w,"{\"type\":\"STOP\",\"sessionId\":");quote(&w,session,CM_ID);put(&w,",\"requestId\":%d}",id);
    if(!w.ok)return 0;
    memcpy(out,staged,(size_t)(w.p-staged)+1);
    return 1;
}
