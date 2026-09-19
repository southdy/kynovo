/* kclient_test.c -- black-box deterministic tests for the kynovo client state
 * machine core (kclient.h).  Mirrors raft_test.c: injects a capture transport
 * and a capture output sink, feeds synthetic RESPONSE frames / connection
 * events, and asserts on the resulting topology + redirect + retry state.
 *
 * The two highest-value regressions live here (neither was covered before the
 * P4 extraction):
 *   C2 -- REDIRECT used to OVERWRITE hosts[host_index], erasing the node the
 *         client was abandoning; a later leader death then left no address for
 *         the sole surviving follower.  Fixed by find-endpoint + append.
 *   C1 -- the in-flight auto-discovery flag (discovering) was cleared only on
 *         the OK path, so a failed MEMBERS query left it set forever and every
 *         later `members` was silently suppressed.  Fixed by a unified reset on
 *         every terminal path + the connection-closed drop site.
 *
 * AI contract: BEGIN / PASS / SUMMARY prefixes, deterministic (no wall clock,
 * no sockets, injected now_us via app.now_us).
 */
#include "../code/kbase.h"
#include "../code/kproto.h"
#include "../code/kclient.h"
#include "test.h"
#include <stdarg.h>

/* ---- capture transport + capture output ---- */
static struct{
  int conn_calls;
  char conn_host[K_HOST_MAX];
  unsigned short conn_port;
  int send_count;
  k_u8 last_type;
  int close_calls;
  char out[4096];
  size_t outlen;
} g;

static void cap_reset(void){ memset(&g,0,sizeof(g)); }

static void *cap_connect(k_client_app *app,const char *host,unsigned short port){
  (void)app;
  g.conn_calls++;
  if(host) strncpy(g.conn_host,host,K_HOST_MAX-1);
  g.conn_port=port;
  return (void*)(size_t)1;   /* fake connected socket handle */
}
static int cap_send(k_client_app *app,void *sock,k_u32 magic,k_u8 type,const void *payload,k_u32 size,int control){
  (void)app;(void)sock;(void)magic;(void)payload;(void)size;(void)control;
  g.send_count++;
  g.last_type=type;
  return 0;
}
static int cap_recv(k_client_app *app,void *sock){ (void)app;(void)sock; return 0; }
static void cap_close(k_client_app *app,void *sock){ (void)app;(void)sock; g.close_calls++; }
static const k_client_transport cap_transport={"cap",cap_connect,cap_send,cap_recv,cap_close};

static void cap_output(void *ud,const char *fmt,...){
  va_list ap;
  (void)ud;
  va_start(ap,fmt);
#if defined(_MSC_VER)
  _vsnprintf(g.out+g.outlen,sizeof(g.out)-(g.outlen<sizeof(g.out)?g.outlen:0),fmt,ap);   /* MSVC 6 has no vsnprintf */
#else
  vsnprintf(g.out+g.outlen,sizeof(g.out)-(g.outlen<sizeof(g.out)?g.outlen:0),fmt,ap);
#endif
  va_end(ap);
  g.outlen=strlen(g.out);
}

static void setup(k_client_app *app){
  memset(app,0,sizeof(*app));
  cap_reset();
  app->transport=&cap_transport;
  app->output=cap_output;
  app->output_ud=0;
  app->now_us=0;
}

/* Build a RESPONSE wire payload (mirrors k_response_decode's reader). */
static void make_response(k_buf *b,k_u32 request_id,k_u8 status,int leader_id,const char *host,unsigned short port,const char *body){
  k_u32 host_len=host?(k_u32)strlen(host):0u;
  k_u32 body_len=body?(k_u32)strlen(body):0u;
  memset(b,0,sizeof(*b));
  k_buf_u32(b,request_id);
  k_buf_u8(b,status);
  k_buf_i32(b,(k_i32)leader_id);
  k_buf_u16(b,(k_u16)host_len);
  k_buf_bytes(b,host,host_len);
  k_buf_u16(b,port);
  k_buf_u32(b,body_len);
  k_buf_bytes(b,body,body_len);
}

/* `shutdown` must surface the server's reason ("stopping").  The OK-status print
   whitelist omitted K_REQ_SHUTDOWN, so its response body was swallowed and the
   CLI printed a bare "ok": an operator could not tell whether the node accepted
   the stop or answered something else entirely (the server does send a body -
   kserver.h replies K_STATUS_OK with "stopping"). */
static void test_shutdown_body_printed(void){
  k_client_app app;
  k_buf b;
  TEST_BEGIN("client prints the SHUTDOWN response body (visibility)");
  setup(&app);
  k_client_seed_parse(&app,"h1:7000");
  TEST_ASSERT(k_client_queue(&app,K_REQ_SHUTDOWN,0,0,0,0)==0,"queue SHUTDOWN");
  TEST_ASSERT(app.pending!=0,"pending armed");
  make_response(&b,app.pending->id,K_STATUS_OK,1,0,0,"stopping");
  TEST_ASSERT(k_client_response_frame(&app,K_RESPONSE,b.data,b.len)==0,"feed OK stopping");
  TEST_ASSERT_STR_EQ(g.out,"stopping","the SHUTDOWN body is printed, not swallowed as ok");
  k_buf_free(&b);
  k_pending_free(app.pending);
  TEST_END();
}

static void test_seed_parse_basic(void){
  k_client_app app;
  TEST_BEGIN("client seed parse basic");
  setup(&app);
  TEST_ASSERT(k_client_seed_parse(&app,"h1:7000,h2:7001")==0,"seed parse ok");
  TEST_ASSERT(app.host_count==2,"host_count 2");
  TEST_ASSERT(app.seed_count==2,"seed_count 2");
  TEST_ASSERT_STR_EQ(app.hosts[0],"h1","hosts[0]");
  TEST_ASSERT_STR_EQ(app.hosts[1],"h2","hosts[1]");
  TEST_ASSERT_I64_EQ(app.ports[0],7000,"ports[0]");
  TEST_ASSERT_I64_EQ(app.ports[1],7001,"ports[1]");
  TEST_END();
}

static void test_find_endpoint(void){
  k_client_app app;
  TEST_BEGIN("client find endpoint");
  setup(&app);
  k_client_seed_parse(&app,"h1:7000,h2:7001");
  TEST_ASSERT_I64_EQ(k_client_find_endpoint(&app,"h2",7001),1,"finds second");
  TEST_ASSERT_I64_EQ(k_client_find_endpoint(&app,"h1",7000),0,"finds first");
  TEST_ASSERT_I64_EQ(k_client_find_endpoint(&app,"h3",7002),-1,"absent -1");
  TEST_ASSERT_I64_EQ(k_client_find_endpoint(&app,"h1",9999),-1,"port mismatch -1");
  TEST_END();
}

static void test_apply_members_dedup(void){
  k_client_app app;
  TEST_BEGIN("client apply members dedup");
  setup(&app);
  k_client_seed_parse(&app,"h1:7000");
  /* h1 duplicates the seed -> skipped; h2/h3 appended after the seed prefix */
  k_client_apply_members(&app,"1@h1:7000:7100,2@h2:7001:7101,3@h3:7002:7102");
  TEST_ASSERT_I64_EQ(app.host_count,3,"host_count 1 seed + 2 new");
  TEST_ASSERT_STR_EQ(app.hosts[0],"h1","seed kept");
  TEST_ASSERT_STR_EQ(app.hosts[1],"h2","h2 appended");
  TEST_ASSERT_STR_EQ(app.hosts[2],"h3","h3 appended");
  TEST_END();
}

static void test_redirect_appends_not_overwrite(void){
  k_client_app app;
  k_buf b;
  TEST_BEGIN("client redirect appends (no clobber) -- C2 regression");
  setup(&app);
  k_client_seed_parse(&app,"h1:7000,h2:7001");
  TEST_ASSERT(k_client_queue(&app,K_REQ_GET,"k",1,0,0)==0,"queue GET");
  TEST_ASSERT(app.pending!=0,"pending armed");
  make_response(&b,app.pending->id,K_STATUS_REDIRECT,5,"h3",7002,"");
  TEST_ASSERT(k_client_response_frame(&app,K_RESPONSE,b.data,b.len)==0,"feed redirect");
  TEST_ASSERT_I64_EQ(app.host_count,3,"host_count grew to 3 (append, not overwrite)");
  TEST_ASSERT_I64_EQ(app.host_index,2,"host_index points at the new leader");
  TEST_ASSERT_STR_EQ(app.hosts[0],"h1","h1 preserved -- the abandoned follower survives");
  TEST_ASSERT_STR_EQ(app.hosts[1],"h2","h2 preserved (was clobbered before the fix)");
  TEST_ASSERT_STR_EQ(app.hosts[2],"h3","h3 appended at the tail");
  TEST_ASSERT(app.pending!=0,"pending preserved for resend");
  TEST_ASSERT(app.pending->sent==0,"pending marked unsent to resend at redirect target");
  k_buf_free(&b);
  k_pending_free(app.pending);
  TEST_END();
}

static void test_redirect_to_known_endpoint(void){
  k_client_app app;
  k_buf b;
  TEST_BEGIN("client redirect to known endpoint (no dup)");
  setup(&app);
  k_client_seed_parse(&app,"h1:7000,h2:7001");
  k_client_queue(&app,K_REQ_GET,"k",1,0,0);
  make_response(&b,app.pending->id,K_STATUS_REDIRECT,1,"h1",7000,"");
  k_client_response_frame(&app,K_RESPONSE,b.data,b.len);
  TEST_ASSERT_I64_EQ(app.host_count,2,"host_count unchanged (no duplicate append)");
  TEST_ASSERT_I64_EQ(app.host_index,0,"host_index jumps to the already-known leader");
  TEST_ASSERT_STR_EQ(app.hosts[0],"h1","h1 intact");
  TEST_ASSERT_STR_EQ(app.hosts[1],"h2","h2 intact");
  k_buf_free(&b);
  k_pending_free(app.pending);
  TEST_END();
}

static void test_discover_sends_members(void){
  k_client_app app;
  TEST_BEGIN("client on-connect arms MEMBERS discovery");
  setup(&app);
  k_client_seed_parse(&app,"h1:7000");
  TEST_ASSERT(k_client_connect(&app)==0,"connect (fake socket)");
  TEST_ASSERT(k_client_on_connected(&app)==0,"on_connected ok");
  TEST_ASSERT_I64_EQ(app.discovering,1,"auto-discovery flag set");
  TEST_ASSERT(app.pending!=0,"MEMBERS pending queued");
  TEST_ASSERT_I64_EQ(app.pending->type,K_REQ_MEMBERS,"pending is MEMBERS");
  k_client_poll(&app);   /* the loop, not on_connected, flushes the queued discovery frame */
  TEST_ASSERT(g.send_count>=1,"discovery frame sent");
  TEST_ASSERT_I64_EQ(g.last_type,K_REQ_MEMBERS,"sent frame is MEMBERS");
  k_pending_free(app.pending);
  TEST_END();
}

static void test_discovering_cleared_on_error(void){
  k_client_app app;
  k_buf b;
  TEST_BEGIN("client discovering cleared on error -- C1 regression");
  setup(&app);
  k_client_seed_parse(&app,"h1:7000");
  k_client_on_connected(&app);   /* arms MEMBERS discovery */
  TEST_ASSERT_I64_EQ(app.discovering,1,"discovering armed");
  make_response(&b,app.pending->id,K_STATUS_ERROR,0,"",0,"boom");
  k_client_response_frame(&app,K_RESPONSE,b.data,b.len);
  TEST_ASSERT_I64_EQ(app.discovering,0,"discovering cleared on terminal ERROR (was left set)");
  TEST_ASSERT(app.pending==0,"pending removed");
  k_buf_free(&b);
  k_pending_free(app.pending);
  TEST_END();
}

static void test_discovering_cleared_on_closed(void){
  k_client_app app;
  TEST_BEGIN("client discovering cleared on closed -- C1 drop site");
  setup(&app);
  k_client_seed_parse(&app,"h1:7000");
  k_client_on_connected(&app);   /* arms MEMBERS discovery */
  TEST_ASSERT_I64_EQ(app.discovering,1,"discovering armed");
  k_client_on_closed(&app);      /* connection dies before the reply */
  TEST_ASSERT_I64_EQ(app.discovering,0,"discovering cleared on connection drop");
  TEST_ASSERT(app.pending==0,"pending dropped with the failed discovery");
  k_pending_free(app.pending);
  TEST_END();
}

static void test_seed_edge_cases(void){
  k_client_app app;
  TEST_BEGIN("client seed parse rejects malformed input");
  setup(&app);
  TEST_ASSERT(k_client_seed_parse(&app,"")!=0,"empty rejected");
  TEST_ASSERT(k_client_seed_parse(&app,"h1")!=0,"no colon rejected");
  TEST_ASSERT(k_client_seed_parse(&app,":7000")!=0,"empty host rejected");
  TEST_ASSERT(k_client_seed_parse(&app,"h1:70000")!=0,"port > 65535 rejected");
  TEST_ASSERT(k_client_seed_parse(&app,"h1:abc")!=0,"non-numeric port rejected");
  TEST_END();
}

static void test_retry_arms_and_exhausts(void){
  k_client_app app;
  k_buf b;
  int i;
  TEST_BEGIN("client retry arms on leader-unknown then exhausts");
  setup(&app);
  k_client_seed_parse(&app,"h1:7000");
  TEST_ASSERT(k_client_queue(&app,K_REQ_GET,"k",1,0,0)==0,"queue GET");
  for(i=0;i<K_CLIENT_RETRY_MAX;i++){
    make_response(&b,app.pending->id,K_STATUS_REDIRECT,0,"",0,"");
    TEST_ASSERT(k_client_response_frame(&app,K_RESPONSE,b.data,b.len)==0,"feed redirect");
    TEST_ASSERT(app.pending!=0,"pending preserved on retry");
    TEST_ASSERT_I64_EQ(app.retry_count,i+1,"retry_count increments");
    k_buf_free(&b);
  }
  TEST_ASSERT_I64_EQ(app.retry_pending,1,"retry armed");
  /* the (MAX+1)-th arm is exhausted: pending dropped, "leader unavailable" */
  make_response(&b,app.pending->id,K_STATUS_REDIRECT,0,"",0,"");
  k_client_response_frame(&app,K_RESPONSE,b.data,b.len);
  k_buf_free(&b);
  TEST_ASSERT(app.pending==0,"pending dropped on exhaustion");
  TEST_ASSERT_I64_EQ(app.retry_count,K_CLIENT_RETRY_MAX,"retry_count capped at MAX");
  k_pending_free(app.pending);
  TEST_END();
}

static void test_retry_resends_after_deadline(void){
  k_client_app app;
  k_buf b;
  TEST_BEGIN("client retry resends after deadline");
  setup(&app);
  k_client_seed_parse(&app,"h1:7000");
  TEST_ASSERT(k_client_connect(&app)==0,"connect (fake socket)");
  app.connected=1;   /* skip on_connected's MEMBERS discovery */
  TEST_ASSERT(k_client_queue(&app,K_REQ_GET,"k",1,0,0)==0,"queue GET");
  k_client_poll(&app);   /* queued frames are flushed by the loop, never from the queue call */
  TEST_ASSERT_I64_EQ(g.send_count,1,"initial send");
  make_response(&b,app.pending->id,K_STATUS_REDIRECT,0,"",0,"");
  k_client_response_frame(&app,K_RESPONSE,b.data,b.len);
  k_buf_free(&b);
  TEST_ASSERT_I64_EQ(app.retry_pending,1,"retry armed");
  TEST_ASSERT_I64_EQ(app.pending->sent,0,"pending marked unsent");
  app.now_us=app.retry_deadline_us;   /* advance the clock past the deadline */
  k_client_poll(&app);
  TEST_ASSERT_I64_EQ(g.send_count,2,"re-sent after deadline");
  k_pending_free(app.pending);
  TEST_END();
}

static void test_queue_rget(void){
  k_client_app app;
  TEST_BEGIN("client queue_rget arms RGET request");
  setup(&app);
  k_client_seed_parse(&app,"h1:7000");
  TEST_ASSERT(k_client_queue_rget(&app,"a",1,"z",1,K_SCAN_ASC,10u)==0,"queue ok");
  TEST_ASSERT(app.pending!=0,"pending armed");
  TEST_ASSERT_I64_EQ(app.pending->type,K_REQ_RGET,"type RGET");
  k_pending_free(app.pending);
  TEST_END();
}

static void test_queue_member(void){
  k_client_app app;
  int ids[1];
  char hosts[1][K_HOST_MAX];
  unsigned short cp[1],pp[1];
  TEST_BEGIN("client queue_member arms MEMBER request");
  setup(&app);
  k_client_seed_parse(&app,"h1:7000");
  ids[0]=2; strcpy(hosts[0],"h2"); cp[0]=7001; pp[0]=7101;
  TEST_ASSERT(k_client_queue_member(&app,K_MEMBER_ADD,ids,1,hosts,cp,pp)==0,"queue ok");
  TEST_ASSERT(app.pending!=0,"pending armed");
  TEST_ASSERT_I64_EQ(app.pending->type,K_REQ_MEMBER,"type MEMBER");
  k_pending_free(app.pending);
  TEST_END();
}


/* ---- pipelining: the list always held several pendings (responses match by id);
   inflight_limit is the policy that allows more than one at a time. ---- */
static int g_hook_calls;
static k_u64 g_hook_last_us;
static k_u8 g_hook_last_status;
static void cap_on_done(void *ud,k_u32 id,k_u8 status,k_u64 duration_us){
  (void)ud;(void)id;
  g_hook_calls++;
  g_hook_last_status=status;
  g_hook_last_us=duration_us;
}
static void test_pipeline_default_still_one_at_a_time(void){
  k_client_app app;
  TEST_BEGIN("client without pipelining still refuses a second in-flight request");
  setup(&app);
  k_client_seed_parse(&app,"h1:7000");
  TEST_ASSERT(k_client_queue(&app,K_REQ_GET,"k",1,0,0)==0,"first queue ok");
  TEST_ASSERT(k_client_queue(&app,K_REQ_GET,"k2",2,0,0)==0,"second queue is refused (not an error)");
  TEST_ASSERT_I64_EQ(k_client_inflight_count(&app),1,"still one in flight");
  TEST_ASSERT(strstr(g.out,"wait for the current request")!=0,"the refusal is visible");
  k_pending_free(app.pending);
  TEST_END();
}
static void test_pipeline_keeps_k_in_flight(void){
  k_client_app app;
  k_buf b;
  TEST_BEGIN("client pipelining: K in flight, matched by id, completions accounted");
  setup(&app);
  app.inflight_limit=3;
  app.on_done=cap_on_done;
  app.on_done_ud=0;
  g_hook_calls=0;
  k_client_seed_parse(&app,"h1:7000");
  TEST_ASSERT(k_client_connect(&app)==0,"connect (fake socket)");
  app.connected=1;   /* skip on_connected's MEMBERS discovery */
  TEST_ASSERT(k_client_queue(&app,K_REQ_GET,"a",1,0,0)==0,"queue 1");
  TEST_ASSERT(k_client_queue(&app,K_REQ_GET,"b",1,0,0)==0,"queue 2");
  TEST_ASSERT(k_client_queue(&app,K_REQ_GET,"c",1,0,0)==0,"queue 3");
  TEST_ASSERT(k_client_queue(&app,K_REQ_GET,"d",1,0,0)==0,"queue 4 refused (limit 3)");
  TEST_ASSERT_I64_EQ(k_client_inflight_count(&app),3,"three in flight");
  k_client_poll(&app);   /* frames go out from the loop, in submission order */
  TEST_ASSERT_I64_EQ(g.send_count,3,"all three sent in one loop turn");
  /* complete the middle one first: matching is by id, not by position */
  make_response(&b,app.pending->next->id,K_STATUS_OK,1,"",0,"vv");
  k_client_response_frame(&app,K_RESPONSE,b.data,b.len);
  k_buf_free(&b);
  TEST_ASSERT_I64_EQ(k_client_inflight_count(&app),2,"one completed, two still in flight");
  TEST_ASSERT_I64_EQ(app.done_count,1,"done_count");
  TEST_ASSERT_I64_EQ(g_hook_calls,1,"completion hook fired");
  TEST_ASSERT_I64_EQ(g_hook_last_status,K_STATUS_OK,"hook saw OK");
  TEST_ASSERT(strstr(g.out,"vv")!=0,"response body printed");
  make_response(&b,app.pending->id,K_STATUS_OK,1,"",0,"vv");
  k_client_response_frame(&app,K_RESPONSE,b.data,b.len);
  k_buf_free(&b);
  make_response(&b,app.pending->id,K_STATUS_ERROR,1,"",0,"boom");
  k_client_response_frame(&app,K_RESPONSE,b.data,b.len);
  k_buf_free(&b);
  TEST_ASSERT_I64_EQ(k_client_inflight_count(&app),0,"all completed");
  TEST_ASSERT_I64_EQ(app.done_count,3,"done_count after three completions");
  TEST_ASSERT_I64_EQ(app.error_count,1,"one failing status counted");
  TEST_ASSERT(k_client_busy(&app)==0,"no longer busy");
  TEST_END();
}

int main(void){
  TEST_PLAN(16);
  test_shutdown_body_printed();
  test_seed_parse_basic();
  test_find_endpoint();
  test_apply_members_dedup();
  test_redirect_appends_not_overwrite();
  test_redirect_to_known_endpoint();
  test_discover_sends_members();
  test_discovering_cleared_on_error();
  test_discovering_cleared_on_closed();
  test_seed_edge_cases();
  test_retry_arms_and_exhausts();
  test_retry_resends_after_deadline();
  test_queue_rget();
  test_queue_member();
  test_pipeline_default_still_one_at_a_time();
  test_pipeline_keeps_k_in_flight();
  TEST_SUMMARY();
  return TEST_EXIT_CODE();
}
