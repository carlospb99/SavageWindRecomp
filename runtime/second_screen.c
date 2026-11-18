// Optional, read-only browser dashboard. The game thread publishes a snapshot;
// the network thread only serves that copy and never dereferences game addresses.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET SSock;
#define SS_BAD INVALID_SOCKET
#define ss_close closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SSock;
#define SS_BAD (-1)
#define ss_close close
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rt.h"
#include "settings.h"
#include "second_screen.h"

enum { SS_PORT=8765, SS_JSON=1024 };
typedef struct { bool has_link,has_hour; float x,y,z,stamina,hour; int weather; char place[128],region[128]; } Snapshot;
static pthread_mutex_t state_lock=PTHREAD_MUTEX_INITIALIZER;
static Snapshot state;
static pthread_t server_thread;
static atomic_bool running,stop_requested;
static atomic_llong retry_after;
static bool bound_lan;

static bool send_all(SSock s,const void *data,size_t n) {
    const char *p=data;
    while(n) {
#ifdef _WIN32
        int sent=send(s,p,(int)(n>0x7fffffff?0x7fffffff:n),0);
#else
#ifdef __APPLE__
        ssize_t sent=send(s,p,n,0);
#else
        ssize_t sent=send(s,p,n,MSG_NOSIGNAL);
#endif
#endif
        if(sent<=0)return false;
        p+=sent;n-=(size_t)sent;
    }
    return true;
}
static void json_string(char *out,size_t cap,const char *s) {
    size_t n=0;if(!cap)return;out[n++]='"';
    for(;s&&*s&&n+7<cap;s++){unsigned char c=(unsigned char)*s;
        if(c=='"'||c=='\\'){out[n++]='\\';out[n++]=(char)c;}else if(c>=0x20)out[n++]=(char)c;}
    if(n+2<=cap)out[n++]='"';out[n]=0;
}
static size_t snapshot_json(char *out,size_t cap) {
    Snapshot s;pthread_mutex_lock(&state_lock);s=state;pthread_mutex_unlock(&state_lock);
    char p[300],r[300];json_string(p,sizeof p,s.place);json_string(r,sizeof r,s.region);
    char hour[24]; if(s.has_hour) snprintf(hour,sizeof hour,"%.2f",s.hour); else snprintf(hour,sizeof hour,"null");
    int n=snprintf(out,cap,"{\"connected\":%s,\"position\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f},\"stamina\":%.3f,\"hour\":%s,\"weather\":%d,\"place\":%s,\"region\":%s}",
        s.has_link?"true":"false",s.x,s.y,s.z,s.stamina,hour,s.weather,p,r);
    return n>0&&(size_t)n<cap?(size_t)n:0;
}
static uint32_t rol(uint32_t x,unsigned n){return(x<<n)|(x>>(32-n));}
static void sha1(const unsigned char *data,size_t len,unsigned char out[20]) {
    uint32_t h[5]={0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    size_t total=((len+9+63)/64)*64;
    for(size_t off=0;off<total;off+=64){
        unsigned char b[64]={0};
        for(size_t i=0;i<64&&off+i<len;i++)b[i]=data[off+i];
        if(off<=len&&len<off+64)b[len-off]=0x80;
        if(off+64==total){uint64_t bits=(uint64_t)len*8;for(int i=0;i<8;i++)b[63-i]=(unsigned char)(bits>>(i*8));}
        uint32_t w[80];for(int i=0;i<16;i++)w[i]=(uint32_t)b[i*4]<<24|(uint32_t)b[i*4+1]<<16|(uint32_t)b[i*4+2]<<8|b[i*4+3];
        for(int i=16;i<80;i++)w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        uint32_t a=h[0],bb=h[1],c=h[2],d=h[3],e=h[4];
        for(int i=0;i<80;i++){uint32_t f,k;
            if(i<20){f=(bb&c)|((~bb)&d);k=0x5a827999;}else if(i<40){f=bb^c^d;k=0x6ed9eba1;}
            else if(i<60){f=(bb&c)|(bb&d)|(c&d);k=0x8f1bbcdc;}else{f=bb^c^d;k=0xca62c1d6;}
            uint32_t t=rol(a,5)+f+e+k+w[i];e=d;d=c;c=rol(bb,30);bb=a;a=t;}
        h[0]+=a;h[1]+=bb;h[2]+=c;h[3]+=d;h[4]+=e;
    }
    for(int i=0;i<5;i++)for(int j=0;j<4;j++)out[i*4+j]=(unsigned char)(h[i]>>(24-j*8));
}
static void base64(const unsigned char *in,size_t n,char *out) {
    static const char t[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t j=0;for(size_t i=0;i<n;i+=3){uint32_t v=(uint32_t)in[i]<<16;
        if(i+1<n)v|=(uint32_t)in[i+1]<<8;if(i+2<n)v|=in[i+2];
        out[j++]=t[(v>>18)&63];out[j++]=t[(v>>12)&63];out[j++]=i+1<n?t[(v>>6)&63]:'=';out[j++]=i+2<n?t[v&63]:'=';}
    out[j]=0;
}
static bool websocket_accept(const char *req,char out[32]) {
    const char *p=strstr(req,"Sec-WebSocket-Key:");if(!p)return false;p+=18;
    while(*p==' '||*p=='\t')p++;char key[128];size_t n=0;
    while(*p&&*p!='\r'&&*p!='\n'&&n+1<sizeof key)key[n++]=*p++;key[n]=0;if(!n)return false;
    char input[180];snprintf(input,sizeof input,"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key);
    unsigned char digest[20];sha1((const unsigned char *)input,strlen(input),digest);base64(digest,20,out);return true;
}
static bool ws_frame(SSock s,const char *p,size_t n) {
    unsigned char h[4];size_t z=2;h[0]=0x81;
    if(n<126)h[1]=(unsigned char)n;else if(n<=65535){h[1]=126;h[2]=(unsigned char)(n>>8);h[3]=(unsigned char)n;z=4;}else return false;
    return send_all(s,h,z)&&send_all(s,p,n);
}
static void http_reply(SSock s,const char *type,const char *body) {
    char h[256];size_t n=strlen(body);int z=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",type,n);
    if(z>0){send_all(s,h,(size_t)z);send_all(s,body,n);}
}
static const char page[] =
"<!doctype html><html lang='es'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Savage Wind · Segunda pantalla</title><style>"
":root{color-scheme:dark;font:16px system-ui;background:#101812;color:#edf4ed}body{margin:0;padding:20px;max-width:900px;margin:auto}h1{font-size:1.35rem;margin:0 0 6px}"
"small,.muted{color:#9fb2a3}.status{margin:12px 0;padding:10px;border-radius:10px;background:#1d2a21}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}"
".tile{background:#1b2820;border:1px solid #304236;border-radius:14px;padding:14px}.k{color:#9fb2a3;font-size:.8rem}.v{font-size:1.15rem;font-weight:650;margin-top:4px;overflow-wrap:anywhere}"
"canvas{width:100%;height:220px;background:#17251c;border:1px solid #304236;border-radius:14px;margin-top:14px}@media(max-width:520px){body{padding:14px}.v{font-size:1rem}}</style>"
"<h1>Savage Wind · Segunda pantalla</h1><small>Panel de solo lectura · actualización en vivo</small><div class='status' id='status'>Conectando…</div><div class='grid'>"
"<div class='tile'><div class='k'>Ubicación</div><div class='v' id='place'>—</div></div><div class='tile'><div class='k'>Región</div><div class='v' id='region'>—</div></div>"
"<div class='tile'><div class='k'>Coordenadas X / Y / Z</div><div class='v' id='xyz'>—</div></div><div class='tile'><div class='k'>Resistencia</div><div class='v' id='stamina'>—</div></div>"
"<div class='tile'><div class='k'>Hora del juego</div><div class='v' id='hour'>—</div></div><div class='tile'><div class='k'>Clima (código)</div><div class='v' id='weather'>—</div></div></div>"
"<canvas id='map' width='800' height='360'></canvas><small class='muted'>Plano X/Z esquemático; no es cartografía de Hyrule.</small><script>"
"const $=id=>document.getElementById(id),c=$('map'),ctx=c.getContext('2d');function draw(d){ctx.clearRect(0,0,800,360);ctx.strokeStyle='#34483a';"
"for(let x=0;x<=800;x+=80){ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x,360);ctx.stroke()}for(let y=0;y<=360;y+=45){ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(800,y);ctx.stroke()}"
"if(d.connected){let x=Math.max(8,Math.min(792,(d.position.x+5000)/10000*800)),y=Math.max(8,Math.min(352,(d.position.z+4000)/8000*360));ctx.fillStyle='#8ee0a8';ctx.beginPath();ctx.arc(x,y,9,0,7);ctx.fill()}}"
"function update(d){$('status').textContent=d.connected?'Link detectado':'Esperando a que cargue el mundo';$('place').textContent=d.place||'—';$('region').textContent=d.region||'—';"
"$('xyz').textContent=d.connected?[d.position.x,d.position.y,d.position.z].map(v=>v.toFixed(1)).join(' / '):'—';$('stamina').textContent=d.stamina>=0?d.stamina.toFixed(2):'—';"
"$('hour').textContent=d.hour!==null?d.hour.toFixed(2)+' h':'—';$('weather').textContent=d.weather>=0?d.weather:'—';draw(d)}"
"function connect(){let w=new WebSocket('ws://'+location.host+'/ws');w.onmessage=e=>{try{update(JSON.parse(e.data))}catch(_){}};w.onclose=()=>setTimeout(connect,1500);w.onerror=()=>w.close()}connect();</script></html>";

static void *serve(void *unused) {
    (void)unused;
#ifdef _WIN32
    WSADATA wd;if(WSAStartup(MAKEWORD(2,2),&wd)){atomic_store(&running,false);return NULL;}
#endif
    SSock listener=socket(AF_INET,SOCK_STREAM,0);if(listener==SS_BAD)goto done;
    int yes=1;setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,(const char *)&yes,sizeof yes);
    struct sockaddr_in a;memset(&a,0,sizeof a);a.sin_family=AF_INET;a.sin_port=htons(SS_PORT);a.sin_addr.s_addr=htonl(bound_lan?INADDR_ANY:INADDR_LOOPBACK);
    if(bind(listener,(struct sockaddr *)&a,sizeof a)<0||listen(listener,4)<0){LOG("[second-screen] cannot bind TCP port %d\\n",SS_PORT);ss_close(listener);atomic_store(&retry_after,(long long)time(NULL)+5);goto done;}
    LOG("[second-screen] listening on %s:%d\n",bound_lan?"0.0.0.0":"127.0.0.1",SS_PORT);
    while(!atomic_load(&stop_requested)){
        fd_set rd;FD_ZERO(&rd);FD_SET(listener,&rd);struct timeval tv={0,250000};if(select((int)(listener+1),&rd,NULL,NULL,&tv)<=0)continue;
        struct sockaddr_in peer;
#ifdef _WIN32
        int plen=sizeof peer;
#else
        socklen_t plen=sizeof peer;
#endif
        SSock client=accept(listener,(struct sockaddr *)&peer,&plen);if(client==SS_BAD)continue;
#ifdef __APPLE__
        int no_sigpipe=1;setsockopt(client,SOL_SOCKET,SO_NOSIGPIPE,&no_sigpipe,sizeof no_sigpipe);
#endif
#ifdef _WIN32
        DWORD recv_timeout=2000;setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,(const char *)&recv_timeout,sizeof recv_timeout);
#else
        struct timeval recv_timeout={2,0};setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&recv_timeout,sizeof recv_timeout);
#endif
        char req[4096];size_t used=0;req[0]=0;
        while(used+1<sizeof req&&!strstr(req,"\r\n\r\n")){
#ifdef _WIN32
            int n=recv(client,req+used,(int)(sizeof req-used-1),0);
#else
            ssize_t n=recv(client,req+used,sizeof req-used-1,0);
#endif
            if(n<=0)break;
            used+=(size_t)n;req[used]=0;
        }
        if(used&&!strncmp(req,"GET / HTTP/",11))http_reply(client,"text/html; charset=utf-8",page);
        else if(used&&!strncmp(req,"GET /api/state HTTP/",19)){char json[SS_JSON];if(snapshot_json(json,sizeof json))http_reply(client,"application/json",json);}
        else if(used&&!strncmp(req,"GET /ws HTTP/",13)){
            char key[32],response[256];
            if(websocket_accept(req,key)){int n=snprintf(response,sizeof response,"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",key);
                if(n>0&&send_all(client,response,(size_t)n)){
                    while(!atomic_load(&stop_requested)){
                        char json[SS_JSON];size_t len=snapshot_json(json,sizeof json);if(!len||!ws_frame(client,json,len))break;
                        fd_set cr;FD_ZERO(&cr);FD_SET(client,&cr);struct timeval wait={0,250000};int ready=select((int)(client+1),&cr,NULL,NULL,&wait);
                        if(ready>0&&FD_ISSET(client,&cr)){unsigned char frame[256];
#ifdef _WIN32
                            int got=recv(client,(char *)frame,sizeof frame,0);
#else
                            ssize_t got=recv(client,frame,sizeof frame,0);
#endif
                            if(got<=0||(frame[0]&15)==8)break;
                        }
                    }
                }
            }
        }else{static const char nf[]="HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";send_all(client,nf,sizeof nf-1);}
        ss_close(client);
    }
    ss_close(listener);
done:
#ifdef _WIN32
    WSACleanup();
#endif
    atomic_store(&running,false);return NULL;
}
void second_screen_publish(bool has_link,float x,float y,float z,float stamina,float hour,int weather,const char *place,const char *region){
    if(!g_settings.second_screen)return;
    Snapshot s={0};s.has_link=has_link;s.x=x;s.y=y;s.z=z;s.stamina=stamina;s.hour=hour;s.has_hour=hour>=0;s.weather=weather;
    snprintf(s.place,sizeof s.place,"%s",place?place:"");snprintf(s.region,sizeof s.region,"%s",region?region:"");
    pthread_mutex_lock(&state_lock);state=s;pthread_mutex_unlock(&state_lock);
}
void second_screen_frame(void){
    static bool last_lan;bool wanted=g_settings.second_screen,lan=g_settings.second_screen_lan;
    if(!wanted){if(atomic_load(&running))atomic_store(&stop_requested,true);return;}
    if(atomic_load(&running)&&lan!=last_lan){atomic_store(&stop_requested,true);return;}
    if(!atomic_load(&running)){if((long long)time(NULL)<atomic_load(&retry_after))return;atomic_store(&stop_requested,false);bound_lan=lan;last_lan=lan;atomic_store(&running,true);
        if(pthread_create(&server_thread,NULL,serve,NULL)!=0){atomic_store(&running,false);LOG("[second-screen] cannot start server thread\n");}
        else pthread_detach(server_thread);}
}
