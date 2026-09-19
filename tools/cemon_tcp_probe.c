/* Minimal cemon TCP probe: isolates the framework's client-connect path from the store.
 *
 * Written because the one-shot CLI (kdbctl) hung on its first request while the in-process
 * bench client worked - but that bench client uses the PIPE transport and never touches
 * cemon_tcp_connect, so "the control works" proved nothing about this path.  This probe has
 * no store, no raft and no framing: a cemon TCP server accepts and prints what it receives,
 * a cemon TCP client connects, sends 16 bytes and prints what it receives.
 *
 * usage: cemon_tcp_probe server <port> <seconds>
 *        cemon_tcp_probe client <port> <milliseconds_before_send>
 *
 * Build (no build.sh change; throwaway diagnostic):
 *   gcc -std=c89 -O1 -Wall -o build/cemon_tcp_probe.exe tools/cemon_tcp_probe.c -lws2_32
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include "../code/kbase.h"
#include "../code/kproto.h"
#define CEMON_IMPLEMENTATION
#include "../code/cemon.h"

static unsigned int now_ms(void){
#if defined(_WIN32)
  return (unsigned int)GetTickCount();
#else
  return 0;
#endif
}

/* ---- server ---- */
static cemon *g_loop;
static cemon_socket *g_conn;
static int g_received;
static int g_sends;

static void server_io(cemon_socket *sock,const cemon_event *event){
  if(!sock||!event) return;
  printf("[%u] SERVER event type=%d size=%u\n",now_ms(),(int)event->type,(unsigned)event->size);
  if(event->type==CEMON_ACCEPT){
    g_conn=(cemon_socket*)event->data;
    if(cemon_recv(g_conn)!=0) printf("[%u] SERVER recv arm failed\n",now_ms());
    return;
  }
  if(event->type==CEMON_DATA){
    g_received++;
    printf("[%u] SERVER DATA %u bytes: %.16s\n",now_ms(),(unsigned)event->size,(const char*)event->data);
    if(cemon_send(g_conn,"REPLY-16-BYTES!!",16)==0) g_sends++;
    if(cemon_recv(g_conn)!=0) printf("[%u] SERVER re-arm failed\n",now_ms());
    return;
  }
  if(event->type==CEMON_SENT) printf("[%u] SERVER SENT ack\n",now_ms());
  if(event->type==CEMON_EOF||event->type==CEMON_CLOSED) printf("[%u] SERVER connection ending\n",now_ms());
}

static void peer_io(cemon_socket *sock,const cemon_event *event){
  if(!sock||!event) return;
  printf("[%u] PEER event type=%d size=%u\n",now_ms(),(int)event->type,(unsigned)event->size);
  if(event->type==CEMON_ACCEPT){
    if(cemon_recv((cemon_socket*)event->data)!=0) printf("[%u] PEER arm failed\n",now_ms());
  }else if(event->type==CEMON_DATA){
    if(cemon_recv(sock)!=0) printf("[%u] PEER re-arm failed\n",now_ms());
  }
}

static int run_server(unsigned short port,int seconds){
  unsigned int end;
  g_loop=cemon_create();
  if(!g_loop){ printf("SERVER create failed\n"); return 1; }
  if(!cemon_tcp_listen(g_loop,"127.0.0.1",port,server_io,0)){ printf("SERVER listen failed\n"); return 1; }
  /* a SECOND listener, as kdbsvr has (client port + peer port) */
  if(!cemon_tcp_listen(g_loop,"127.0.0.1",(unsigned short)(port+1),peer_io,0)){ printf("SERVER second listen failed\n"); return 1; }
  printf("[%u] SERVER listening on %u and %u\n",now_ms(),(unsigned)port,(unsigned)(port+1));
  end=now_ms()+(unsigned int)seconds*1000u;
  while(now_ms()<end) cemon_poll(g_loop,50);
  printf("SERVER received=%d sent=%d\n",g_received,g_sends);
  return g_received>0?0:2;
}

/* ---- client ---- */
static cemon_socket *g_client;
static int g_client_received;
static unsigned int g_send_at;
static int g_sent;
static int g_send_delay=150;
static int g_send_in_callback;


static void client_io(cemon_socket *sock,const cemon_event *event){
  if(!sock||!event) return;
  printf("[%u] CLIENT event type=%d size=%u\n",now_ms(),(int)event->type,(unsigned)event->size);
  if(event->type==CEMON_CONNECT){
    if(g_send_in_callback){
      printf("[%u] CLIENT sending 16 bytes from INSIDE the connect callback\n",now_ms());
      if(cemon_send(sock,"REQUEST-16-BYTES",16)!=0) printf("[%u] CLIENT send call failed\n",now_ms());
      else printf("[%u] CLIENT send call ok\n",now_ms());
      g_sent=1;
    }else{
      g_send_at=now_ms()+g_send_delay;
    }
    if(cemon_recv(sock)!=0) printf("[%u] CLIENT recv arm failed\n",now_ms());
    return;
  }
  if(event->type==CEMON_DATA){
    g_client_received++;
    printf("[%u] CLIENT DATA %u bytes\n",now_ms(),(unsigned)event->size);
    return;
  }
  if(event->type==CEMON_EOF||event->type==CEMON_CLOSED) printf("[%u] CLIENT connection ending\n",now_ms());
}

static int run_client(unsigned short port,int delay_ms){
  unsigned int end;
  g_loop=cemon_create();
  if(!g_loop){ printf("CLIENT create failed\n"); return 1; }
  g_client=cemon_tcp_connect(g_loop,"127.0.0.1",port,client_io,0);
  if(!g_client){ printf("CLIENT connect call failed\n"); return 1; }
  printf("[%u] CLIENT connecting to %u (will send %d ms after CONNECT)\n",now_ms(),(unsigned)port,delay_ms);
  end=now_ms()+5000u;
  g_send_at=0;
  while(now_ms()<end){
    cemon_poll(g_loop,20);
    if(g_send_at&&!g_sent&&now_ms()>=g_send_at){
      g_sent=1;
      printf("[%u] CLIENT sending 16 bytes\n",now_ms());
      if(cemon_send(g_client,"REQUEST-16-BYTES",16)!=0) printf("[%u] CLIENT send call failed\n",now_ms());
      else printf("[%u] CLIENT send call ok\n",now_ms());
    }
  }
  printf("CLIENT received=%d\n",g_client_received);
  return g_client_received>0?0:2;
}

/* ---- speak the real client protocol (K_CLIENT_MAGIC + K_REQUEST) ---- */
static cemon_socket *g_proto_sock;
static int g_proto_reply;

static int proto_send_request(cemon_socket *sock,k_u8 type,k_u32 id){
  k_buf payload;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  memset(&payload,0,sizeof(payload));
  if(k_request_payload(&payload,id,type,0,0,0,0)!=0){ printf("proto: payload build failed\n"); return -1; }
  if(payload.len>64u){ printf("proto: payload too big\n"); k_buf_free(&payload); return -1; }
  k_frame_header_build(frame,K_CLIENT_MAGIC,type,payload.len);
  if(payload.len) memcpy(frame+K_FRAME_HEADER,payload.data,payload.len);
  total=K_FRAME_HEADER+payload.len;
  printf("[%u] PROTO sending type=%d frame=%u bytes\n",now_ms(),(int)type,(unsigned)total);
  if(cemon_send(sock,frame,(int)total)!=0){ printf("proto: send failed\n"); k_buf_free(&payload); return -1; }
  k_buf_free(&payload);
  return 0;
}

static void proto_io(cemon_socket *sock,const cemon_event *event){
  if(!sock||!event) return;
  printf("[%u] PROTO event type=%d size=%u\n",now_ms(),(int)event->type,(unsigned)event->size);
  if(event->type==CEMON_CONNECT){
    if(cemon_recv(sock)!=0) printf("[%u] PROTO recv arm failed\n",now_ms());
    if(proto_send_request(sock,K_REQ_MEMBERS,1u)!=0) g_proto_reply=-1;
    return;
  }
  if(event->type==CEMON_DATA){
    g_proto_reply++;
    printf("[%u] PROTO got %u bytes: %.24s\n",now_ms(),(unsigned)event->size,(const char*)event->data);
    if(cemon_recv(sock)!=0) printf("[%u] PROTO re-arm failed\n",now_ms());
    return;
  }
  if(event->type==CEMON_CLOSED||event->type==CEMON_EOF) printf("[%u] PROTO connection ending\n",now_ms());
}

static int run_proto(unsigned short port,int seconds){
  unsigned int end;
  g_loop=cemon_create();
  if(!g_loop){ printf("PROTO create failed\n"); return 1; }
  g_proto_sock=cemon_tcp_connect(g_loop,"127.0.0.1",port,proto_io,0);
  if(!g_proto_sock){ printf("PROTO connect call failed\n"); return 1; }
  end=now_ms()+(unsigned int)seconds*1000u;
  while(now_ms()<end) cemon_poll(g_loop,20);
  printf("PROTO replies=%d\n",g_proto_reply);
  return g_proto_reply>0?0:2;
}

int main(int argc,char **argv){
  if(argc<3){ printf("usage: %s server|client <port> [seconds|delay_ms]\n",argv[0]); return 1; }
  if(strcmp(argv[1],"server")==0) return run_server((unsigned short)atoi(argv[2]),argc>3?atoi(argv[3]):6);
  if(strcmp(argv[1],"proto")==0) return run_proto((unsigned short)atoi(argv[2]),argc>3?atoi(argv[3]):6);
  if(strcmp(argv[1],"client")==0){
    int mode_cb=0;
    if(argc>3) g_send_delay=atoi(argv[3]);
    if(argc>4&&strcmp(argv[4],"callback")==0) mode_cb=1;
    g_send_in_callback=mode_cb;
    return run_client((unsigned short)atoi(argv[2]),g_send_delay);
  }
  printf("unknown mode %s\n",argv[1]);
  return 1;
}
