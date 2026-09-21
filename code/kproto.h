/* kproto.h -- the kynovo wire protocol SHARED layer: wire size limits,
   the client<->server request/response constants, the generic frame codec
   (header build + rx feed), and the on-the-wire node/cluster types.  Pure:
   depends only on kbase.h (bytes).  No cemon, no
   storage, no threads.  Single-header; every function is `static`.
   Single translation unit use.

   Peer-side codec (raft message codec + raft-mask codec + response
   encode) lives in kserver.h; client-side codec (request encode +
   response decode) lives here (shared by the client state machine and
   by server tests that drive the server as a client).
 */
#ifndef KPROTO_H
#define KPROTO_H
#include "kbase.h"

/* ---- wire size limits ---- */
#define K_URI_MAX 1024
#define K_HOST_MAX 64
#define K_MAX_NODES 16
#define K_FRAME_HEADER 12u
#define K_FRAME_MAX 1048576u
#define K_RESPONSE_BODY_MAX (K_FRAME_MAX-256u)
#define K_STATE_MAX 268435456u
#define K_KEY_MAX 65536u
#define K_VALUE_MAX 524288u

/* client wire frame magic (peer magic lives in kserver.h) */
#define K_CLIENT_MAGIC 0x314c434bu
#define K_PEER_MAGIC 0x3152504bu

/* wire format versions */
#define K_WIRE_VERSION 1u
/* 3: ReadIndex request/result carry a round identity (Sec. 6.4).  The peer
   frame header is checked before the payload is decoded, so bumping the version
   makes an older node REJECT the frame instead of failing mid-decode and looking
   like a corrupted connection. */
#define K_PEER_WIRE_VERSION 3u

/* request types: data commands first, then ops commands (wire values) */
#define K_REQ_GET 1u
#define K_REQ_MGET 2u
#define K_REQ_RGET 3u
#define K_REQ_SET 4u
#define K_REQ_MSET 5u
#define K_REQ_RSET 6u
#define K_REQ_DEL 7u
#define K_REQ_MDEL 8u
#define K_REQ_RDEL 9u
#define K_REQ_COUNT 10u
#define K_REQ_MIN 11u
#define K_REQ_MAX 12u
#define K_REQ_CAS 13u
#define K_REQ_FCALL 14u
#define K_REQ_INFO 15u
#define K_REQ_STATS 16u
#define K_REQ_MEMBER 17u
#define K_REQ_MEMBERS 18u
#define K_REQ_HELP 19u
#define K_REQ_SHUTDOWN 20u
#define K_REQ_TOPOLOGY 21u
/* member subcommands */
#define K_MEMBER_ADD 1u
#define K_MEMBER_REMOVE 2u
#define K_MEMBER_RECONFIG 3u
/* response + status codes */
#define K_RESPONSE 100u
#define K_STATUS_OK 0u
#define K_STATUS_NOT_FOUND 1u
#define K_STATUS_ERROR 2u
#define K_STATUS_REDIRECT 3u
#define K_STATUS_CONFLICT 4u
/* CAS sub-modes (first byte of the CAS value field) */
#define K_CAS_SETNX 0u
#define K_CAS_CMP 1u
#define K_CAS_PACKED_MAX (2u*K_VALUE_MAX+9u)

/* range-scan direction (wire value, independent of the storage engine) */
#define K_SCAN_ASC 0
#define K_SCAN_DESC 1

typedef struct k_rx{
  k_u8 *data;
  k_u32 len;
  k_u32 cap;
} k_rx;

typedef struct k_node_spec{
  int id;
  char host[K_HOST_MAX];
  unsigned short client_port;
  unsigned short peer_port;
} k_node_spec;
typedef struct k_cluster{
  k_node_spec nodes[K_MAX_NODES];
  int count;
} k_cluster;

/* ---- cluster-spec line format (id@host:client:peer, comma-separated) ----
   Shared by the server (cluster-spec parsing) and client (MEMBERS response).
   Pure string/line-format helpers: no server or link state. */
static int k_parse_uint(const char *text,int max_value,int *out){
  unsigned long value;
  char *end;
  if(!text||!text[0]||!out) return -1;
  value=strtoul(text,&end,10);
  if(*end||value==0||value>(unsigned long)max_value) return -1;
  *out=(int)value;
  return 0;
}

static int k_cluster_index(const k_cluster *cluster,int id){
  int i;
  if(!cluster) return -1;
  for(i=0;i<cluster->count;i++){
    if(cluster->nodes[i].id==id) return i;
  }
  return -1;
}

static int k_cluster_parse(k_cluster *cluster,const char *text){
  const char *cursor,*end;
  int count=0;
  if(!cluster||!text||!text[0]) return -1;
  memset(cluster,0,sizeof(*cluster));
  cursor=text;
  while(*cursor){
    char item[192],*at,*colon1,*colon2;
    int id,client_port,peer_port,i;
    size_t len,host_len;
    end=strchr(cursor,',');
    len=end?(size_t)(end-cursor):strlen(cursor);
    if(!len||len>=sizeof(item)||count>=K_MAX_NODES) return -1;
    memcpy(item,cursor,len);
    item[len]='\0';
    at=strchr(item,'@');
    if(!at) return -1;
    *at='\0';
    colon1=strchr(at+1,':');
    if(!colon1) return -1;
    *colon1='\0';
    colon2=strchr(colon1+1,':');
    if(!colon2||strchr(colon2+1,':')) return -1;
    *colon2='\0';
    host_len=strlen(at+1);
    if(!host_len||host_len>=K_HOST_MAX) return -1;
    if(k_parse_uint(item,2147483647,&id)!=0||k_parse_uint(colon1+1,65535,&client_port)!=0||k_parse_uint(colon2+1,65535,&peer_port)!=0) return -1;
    for(i=0;i<count;i++){
      if(cluster->nodes[i].id==id) return -1;
    }
    cluster->nodes[count].id=id;
    memcpy(cluster->nodes[count].host,at+1,host_len+1u);
    cluster->nodes[count].client_port=(unsigned short)client_port;
    cluster->nodes[count].peer_port=(unsigned short)peer_port;
    count++;
    if(!end) break;
    cursor=end+1;
  }
  cluster->count=count;
  return count>0?0:-1;
}
static const char *k_numeric_host(const char *host){
  return host&&strcmp(host,"localhost")==0?"127.0.0.1":host;
}

/* ---- frame codec ---- */
typedef int (*k_frame_fn)(void *ud,k_u8 type,const k_u8 *payload,k_u32 size);
/* Build the 12-byte frame header in `frame`: magic, wire version, type,
   reserved, payload size.  The payload follows at frame+K_FRAME_HEADER.
   This is the assemble half of the frame codec; k_rx_feed is the parse half. */
static void k_frame_header_build(k_u8 *frame,k_u32 magic,k_u8 type,k_u32 size){
  k_write_u32(frame,magic);
  k_write_u8(frame+4,magic==K_PEER_MAGIC?K_PEER_WIRE_VERSION:K_WIRE_VERSION);
  k_write_u8(frame+5,type);
  k_write_u16(frame+6,0);
  k_write_u32(frame+8,size);
}
static void k_rx_free(k_rx *rx){
  if(!rx) return;
  K_FREE(rx->data);
  memset(rx,0,sizeof(*rx));
}
static int k_rx_append(k_rx *rx,const void *data,k_u32 size){
  k_u32 need,cap;
  k_u8 *copy;
  if(!rx||(size&&!data)||size>K_FRAME_MAX+K_FRAME_HEADER-rx->len) return -1;
  need=rx->len+size;
  if(need>rx->cap){
    cap=rx->cap?rx->cap:2048u;
    while(cap<need) cap*=2u;
    copy=(k_u8 *)K_REALLOC(rx->data,cap);
    if(!copy) return -1;
    rx->data=copy;
    rx->cap=cap;
  }
  if(size) memcpy(rx->data+rx->len,data,size);
  rx->len=need;
  return 0;
}
static int k_rx_feed(k_rx *rx,k_u32 magic,const void *data,k_u32 size,k_frame_fn fn,void *ud){
  k_u32 payload_size,total;
  k_u8 type;
  k_u8 version=magic==K_PEER_MAGIC?K_PEER_WIRE_VERSION:K_WIRE_VERSION;
  if(!rx||!fn||k_rx_append(rx,data,size)!=0) return -1;
  while(rx->len>=K_FRAME_HEADER){
    if(k_read_u32(rx->data)!=magic||k_read_u8(rx->data+4)!=version||k_read_u16(rx->data+6)!=0) return -1;
    type=k_read_u8(rx->data+5);
    payload_size=k_read_u32(rx->data+8);
    if(payload_size>K_FRAME_MAX) return -1;
    total=K_FRAME_HEADER+payload_size;
    if(rx->len<total) break;
    if(fn(ud,type,rx->data+K_FRAME_HEADER,payload_size)!=0) return -1;
    /* The handler may have closed the connection, which frees this very buffer (k_conn_closed ->
       k_rx_free zeroes data and len).  Without this check len -= total wraps and the next turn reads
       through a NULL data pointer; today that is only avoided because every closing handler happens to
       return non-zero (fourth-round review B4).  Bail out instead of relying on that. */
    if(rx->data==0||rx->len<total) return -1;
    if(rx->len>total) memmove(rx->data,rx->data+total,rx->len-total);
    rx->len-=total;
  }
  return 0;
}

/* ---- request/response payload codec (client side: request encode +
   response decode; shared by the client state machine and by server
   tests that drive the server as a client) ---- */
typedef struct k_response_data{
  k_u32 request_id;
  k_u8 status;
  int leader_id;
  char host[K_HOST_MAX];
  unsigned short port;
  k_u8 *body;
  k_u32 body_size;
} k_response_data;
static void k_response_data_free(k_response_data *response){
  if(!response) return;
  K_FREE(response->body);
  memset(response,0,sizeof(*response));
}
static int k_response_decode(k_response_data *response,const k_u8 *data,k_u32 size){
  k_reader reader;
  k_u16 host_len;
  const k_u8 *host,*body;
  if(!response||!data) return -1;
  memset(response,0,sizeof(*response));
  memset(&reader,0,sizeof(reader));
  reader.data=data;
  reader.len=size;
  response->request_id=k_reader_u32(&reader);
  response->status=k_reader_u8(&reader);
  response->leader_id=(int)k_reader_i32(&reader);
  host_len=k_reader_u16(&reader);
  host=k_reader_bytes(&reader,host_len);
  response->port=(unsigned short)k_reader_u16(&reader);
  response->body_size=k_reader_u32(&reader);
  body=k_reader_bytes(&reader,response->body_size);
  if(reader.err||reader.off!=reader.len||host_len>=K_HOST_MAX) return -1;
  if(host_len) memcpy(response->host,host,host_len);
  response->host[host_len]='\0';
  if(response->body_size){
    response->body=(k_u8 *)K_MALLOC(response->body_size+1u);
    if(!response->body) return -1;
    memcpy(response->body,body,response->body_size);
    response->body[response->body_size]=0;
  }
  return 0;
}

static int k_request_payload(k_buf *payload,k_u32 request_id,k_u8 type,const void *key,k_u32 key_len,const void *value,k_u32 value_len){
  if(!payload||key_len>K_FRAME_MAX||value_len>K_CAS_PACKED_MAX||(key_len&&!key)||(value_len&&!value)) return -1;
  memset(payload,0,sizeof(*payload));
  k_buf_u32(payload,request_id);
  if(type==K_REQ_SET||type==K_REQ_FCALL||type==K_REQ_CAS||type==K_REQ_RSET){
    if(!key_len) payload->err=1;
    k_buf_u32(payload,key_len);
    k_buf_bytes(payload,key,key_len);
    k_buf_u32(payload,value_len);
    k_buf_bytes(payload,value,value_len);
  }else if(type==K_REQ_RDEL||type==K_REQ_COUNT||type==K_REQ_MIN||type==K_REQ_MAX){
    k_buf_u32(payload,key_len);
    k_buf_bytes(payload,key,key_len);
    k_buf_u32(payload,value_len);
    k_buf_bytes(payload,value,value_len);
  }else if(type==K_REQ_GET||type==K_REQ_DEL){
    if(!key_len) payload->err=1;
    k_buf_u32(payload,key_len);
    k_buf_bytes(payload,key,key_len);
  }else if(type==K_REQ_MSET||type==K_REQ_MDEL||type==K_REQ_MGET){
    /* batch: key carries the whole [count][...] list verbatim */
    if(!key_len) payload->err=1;
    k_buf_bytes(payload,key,key_len);
  }else if(type==K_REQ_MEMBER){
    /* admin membership change: the caller passes the [subcmd][ids...] body verbatim in key */
    if(!key_len) payload->err=1;
    k_buf_bytes(payload,key,key_len);
  }else if(type!=K_REQ_INFO&&type!=K_REQ_SHUTDOWN&&type!=K_REQ_HELP&&type!=K_REQ_STATS&&type!=K_REQ_MEMBERS&&type!=K_REQ_TOPOLOGY) payload->err=1;
  if(payload->err||payload->len>K_FRAME_MAX){
    k_buf_free(payload);
    return -1;
  }
  return 0;
}
static int k_rget_request_payload(k_buf *payload,k_u32 request_id,const void *begin,k_u32 begin_len,const void *end,k_u32 end_len,int direction,k_u32 limit){
  if(!payload||begin_len>K_KEY_MAX||end_len>K_KEY_MAX||(begin_len&&!begin)||(end_len&&!end)||(direction!=K_SCAN_ASC&&direction!=K_SCAN_DESC)) return -1;
  memset(payload,0,sizeof(*payload));
  k_buf_u32(payload,request_id);
  k_buf_u32(payload,begin_len);
  k_buf_bytes(payload,begin,begin_len);
  k_buf_u32(payload,end_len);
  k_buf_bytes(payload,end,end_len);
  k_buf_u8(payload,(k_u8)direction);
  k_buf_u32(payload,limit);
  if(payload->err||payload->len>K_FRAME_MAX){
    k_buf_free(payload);
    return -1;
  }
  return 0;
}

#endif
