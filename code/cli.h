/* CLI.H -- Single-header C89 async CLI framework
   ================================================
   Windows XP+, Linux, macOS  |  zero-malloc  |  non-blocking I/O

   LINE EDITING
     Left/Right        move cursor
     Home/End          jump to start/end of line
     Backspace         delete character left of cursor
     Delete            delete character at cursor
     Ctrl+A / Ctrl+E   Home / End
     Ctrl+K            cut from cursor to end of line
     Ctrl+U            cut from cursor to beginning
     Ctrl+W            cut previous word
     Ctrl+C            cancel current line
     Ctrl+L            clear screen
     Ctrl+D / Ctrl+Z   EOF on empty line

   HISTORY
     Up/Down           browse previous/next command
     Duplicate entries are suppressed when adjacent.

   API  (6 functions)
     cli_init          initialise CLI context
     cli_shutdown      restore terminal to normal mode
     cli_poll          non-blocking event tick (returns -1/0/1)
     cli_print         printf-like output, safe mid-edit
     cli_stop          request graceful exit from another thread
     cli_should_stop   query stop flag (1=stop requested)
*/
#ifndef CLI_H
#define CLI_H
#ifdef __cplusplus
extern "C" {
#endif
#ifndef CLI_DEF
#ifdef CLI_STATIC
#define CLI_DEF static
#else
#define CLI_DEF extern
#endif
#endif
typedef struct cli_cmd cli_cmd;
typedef struct cli_ctx cli_ctx;
typedef int (*cli_fn)(cli_ctx *,int,char **);
typedef void (*cli_post_fn)(cli_ctx *,const cli_cmd *,int,char **);
struct cli_cmd{
  const char *name;
  const char *help;
  cli_fn fn;
};
CLI_DEF int cli_init(cli_ctx *cli,const char *prompt,const cli_cmd *cmds,void *ctx,cli_post_fn post,char *linebuf,unsigned int linesz,char **argv,int maxargc);
CLI_DEF void cli_shutdown(cli_ctx *cli);
CLI_DEF int cli_poll(cli_ctx *cli);
CLI_DEF void cli_print(cli_ctx *cli,const char *fmt,...);
CLI_DEF void cli_stop(cli_ctx *cli);
CLI_DEF int cli_should_stop(cli_ctx *cli);
#ifdef __cplusplus
}
#endif
#endif
#if defined(CLI_IMPLEMENTATION)&&!defined(CLI_IMPLEMENTATION_ONCE)
#define CLI_IMPLEMENTATION_ONCE
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
struct cli_ctx{
  const char *prompt;
  const cli_cmd *cmds;
  void *ctx;
  cli_post_fn post;
  char *linebuf;
  unsigned int linesz;
  char **argv;
  int maxargc;
  unsigned int pos;
  unsigned int cursor;
  int stop;
  int prompt_shown;
  int need_redraw;
  int term_w;
  unsigned int view_off;
  char *hist_buf;
  int hist_max;
  int hist_len;
  int hist_count;
  int hist_head;
  int hist_idx;
  char hist_temp[256];
  char *hist_temp_buf;
  int tty;   /* 1 = real console/terminal (interactive line editor); 0 = scripted/redirected */
#ifdef _WIN32
  void *hstdin;
  void *hstdout;
  unsigned int old_in_mode;
  int raw;
#else
  int raw;
  struct termios old_termios;
  int old_flags;
#endif
};
typedef struct cli_abuf{
  char b[1024];
  int len;
} cli_abuf;
static void cli_clear_line(cli_ctx *cli){
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  HANDLE hout=(HANDLE)cli->hstdout;
  if(GetConsoleScreenBufferInfo(hout,&csbi)){
    COORD pos;
    DWORD nw;
    pos.X=0;
    pos.Y=csbi.dwCursorPosition.Y;
    FillConsoleOutputCharacterA(hout,' ',csbi.dwSize.X,pos,&nw);
    SetConsoleCursorPosition(hout,pos);
  }
#else
  if(cli->tty) write(STDOUT_FILENO,"\x1b[2K\x1b[0G",8);
#endif
}
static void cli_abuf_app(cli_abuf *ab,const char *s,int n){
  if(n>0&&ab->len+n<=(int)sizeof(ab->b)){
    memcpy(ab->b+ab->len,s,n);
    ab->len+=n;
  }
}
static void cli_abuf_flush(cli_abuf *ab){
  if(ab->len>0){
    fwrite(ab->b,1,ab->len,stdout);
    fflush(stdout);
    ab->len=0;
  }
}
static void cli_view_calc(cli_ctx *cli,unsigned int *out_off,unsigned int *out_len){
  unsigned int prompt_len,free_cols,off=0,len;
  prompt_len=cli->prompt?(unsigned int)strlen(cli->prompt):0;
  free_cols=cli->term_w>0&&(unsigned)cli->term_w>prompt_len+1?(unsigned)cli->term_w-prompt_len-1:0;
  len=cli->pos;
  if(free_cols&&len>free_cols){
    off=cli->view_off;
    if(off+free_cols>len) off=len-free_cols;
    if(cli->cursor<off) off=cli->cursor;
    if(cli->cursor>=off+free_cols) off=cli->cursor-free_cols+1;
    if(off+free_cols>len) off=len-free_cols;
    cli->view_off=off;
    len=free_cols;
    if(off+len>cli->pos) len=cli->pos-off;
  }
  *out_off=off;
  *out_len=len;
}
static void cli_redraw(cli_ctx *cli){
  const char *pr;
  cli_abuf ab;
  unsigned int view_off,view_len,prompt_len;
  if(!cli||!cli->prompt_shown||!cli->linebuf) return;
#ifndef _WIN32
  if(!cli->tty) return;
#endif
  cli_clear_line(cli);
  cli_view_calc(cli,&view_off,&view_len);
  prompt_len=cli->prompt?(unsigned int)strlen(cli->prompt):0;
  pr=cli->prompt?cli->prompt:"";
  ab.len=0;
  cli_abuf_app(&ab,pr,(int)strlen(pr));
  cli_abuf_app(&ab,cli->linebuf+view_off,(int)view_len);
#ifdef _WIN32
  cli_abuf_flush(&ab);
  {
    HANDLE hout=(HANDLE)cli->hstdout;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if(GetConsoleScreenBufferInfo(hout,&csbi)){
      COORD c;
      c.X=(SHORT)(prompt_len+cli->cursor-view_off);
      c.Y=csbi.dwCursorPosition.Y;
      SetConsoleCursorPosition(hout,c);
      if((int)csbi.dwSize.X!=cli->term_w) cli->term_w=(int)csbi.dwSize.X;
    }
  }
#else
  {
    char tmp[16];
    int ansi_len;
    struct winsize ws;
    if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)!=-1&&ws.ws_col>0) cli->term_w=(int)ws.ws_col;
    ansi_len=snprintf(tmp,sizeof(tmp),"\x1b[%uG",prompt_len+cli->cursor-view_off+1);
    if(ansi_len>(int)sizeof(tmp)-1) ansi_len=(int)sizeof(tmp)-1;
    cli_abuf_app(&ab,tmp,ansi_len);
  }
  cli_abuf_flush(&ab);
#endif
}
static void cli_hist_add(cli_ctx *cli){
  char *dst;
  int i,prev;
  if(!cli->hist_buf||cli->hist_max<=0||cli->hist_len<=0||cli->pos==0) return;
  if(cli->hist_count>0){
    prev=(cli->hist_head-1+cli->hist_max)%cli->hist_max;
    if(strcmp(&cli->hist_buf[prev*cli->hist_len],cli->linebuf)==0) return;
  }
  dst=&cli->hist_buf[cli->hist_head*cli->hist_len];
  for(i=0;i<(int)cli->pos&&i<cli->hist_len-1;i++) dst[i]=cli->linebuf[i];
  dst[i]='\0';
  cli->hist_head=(cli->hist_head+1)%cli->hist_max;
  if(cli->hist_count<cli->hist_max) cli->hist_count++;
}
static void cli_hist_nav(cli_ctx *cli,int dir){
  int newest,oldest;
  if(!cli->hist_buf||cli->hist_max<=0||cli->hist_len<=0||cli->hist_count==0) return;
  newest=(cli->hist_head-1+cli->hist_max)%cli->hist_max;
  oldest=(cli->hist_head-cli->hist_count+cli->hist_max)%cli->hist_max;
  if(cli->hist_idx==-1){
    char *tmp_buf;
    unsigned int tmp_max,n;
    if(dir<0) return;
    tmp_buf=cli->hist_temp_buf?cli->hist_temp_buf:cli->hist_temp;
    tmp_max=cli->hist_temp_buf?cli->linesz-1:(unsigned)sizeof(cli->hist_temp)-1;
    n=cli->pos<tmp_max?cli->pos:tmp_max;
    memcpy(tmp_buf,cli->linebuf,n);
    tmp_buf[n]='\0';
    cli->hist_idx=newest;
  }else if(dir>0){
    if(cli->hist_idx==oldest) return;
    cli->hist_idx=(cli->hist_idx-1+cli->hist_max)%cli->hist_max;
  }else if(cli->hist_idx==newest){
    cli->hist_idx=-1;
    strncpy(cli->linebuf,cli->hist_temp_buf?cli->hist_temp_buf:cli->hist_temp,cli->linesz-1);
    goto restore;
  }else{
    cli->hist_idx=(cli->hist_idx+1)%cli->hist_max;
  }
  strncpy(cli->linebuf,&cli->hist_buf[cli->hist_idx*cli->hist_len],cli->linesz-1);
restore:
  cli->linebuf[cli->linesz-1]='\0';
  cli->pos=(unsigned int)strlen(cli->linebuf);
  cli->cursor=cli->pos;
  cli_redraw(cli);
}
static void cli_hist_reset(cli_ctx *cli){
  cli->hist_idx=-1;
}
static void cli_shift_left(cli_ctx *cli,unsigned int from){
  if(cli->pos==0) return;
  if(from<cli->pos) memmove(&cli->linebuf[from],&cli->linebuf[from+1],cli->pos-from-1);
  cli->pos--;
  cli->linebuf[cli->pos]='\0';
}
static void cli_edit_backspace(cli_ctx *cli){
  cli_hist_reset(cli);
  if(cli->cursor>0){
    if(cli->cursor==cli->pos){
      cli->pos--;
      cli->cursor=cli->pos;
      if(cli->view_off==0){
#ifdef _WIN32
        DWORD nw;
        WriteConsoleA((HANDLE)cli->hstdout,"\b \b",3,&nw,0);
#else
        write(STDOUT_FILENO,"\b \b",3);
#endif
      }else cli_redraw(cli);
    }else{
      cli_shift_left(cli,cli->cursor-1);
      cli->cursor--;
      cli_redraw(cli);
    }
  }
}
static void cli_edit_insert(cli_ctx *cli,char ch){
  cli_hist_reset(cli);
  if(cli->pos+1>=cli->linesz) return;
  if(cli->cursor==cli->pos){
    cli->linebuf[cli->pos++]=ch;
    cli->cursor=cli->pos;
    cli->linebuf[cli->pos]='\0';
    if(cli->view_off==0){
#ifdef _WIN32
      DWORD nw;
      WriteConsoleA((HANDLE)cli->hstdout,&ch,1,&nw,0);
#else
      write(STDOUT_FILENO,&ch,1);
#endif
    }else cli_redraw(cli);
  }else{
    memmove(&cli->linebuf[cli->cursor+1],&cli->linebuf[cli->cursor],cli->pos-cli->cursor);
    cli->linebuf[cli->cursor++]=ch;
    cli->pos++;
    cli->linebuf[cli->pos]='\0';
    cli_redraw(cli);
  }
}
static void cli_edit_delete(cli_ctx *cli){
  cli_hist_reset(cli);
  if(cli->cursor<cli->pos){
    cli_shift_left(cli,cli->cursor);
    cli_redraw(cli);
  }
}
static void cli_edit_ctrl_k(cli_ctx *cli){
  cli_hist_reset(cli);
  cli->pos=cli->cursor;
  cli->linebuf[cli->pos]='\0';
  cli_redraw(cli);
}
static void cli_edit_ctrl_u(cli_ctx *cli){
  cli_hist_reset(cli);
  if(cli->cursor>0){
    memmove(cli->linebuf,&cli->linebuf[cli->cursor],cli->pos-cli->cursor+1);
    cli->pos-=cli->cursor;
    cli->cursor=0;
    cli_redraw(cli);
  }
}
static void cli_edit_ctrl_w(cli_ctx *cli){
  cli_hist_reset(cli);
  if(cli->cursor>0){
    unsigned int start=cli->cursor;
    while(start>0&&(cli->linebuf[start-1]==' '||cli->linebuf[start-1]=='\t')) start--;
    while(start>0&&(cli->linebuf[start-1]!=' '&&cli->linebuf[start-1]!='\t')) start--;
    if(start<cli->cursor){
      memmove(&cli->linebuf[start],&cli->linebuf[cli->cursor],cli->pos-cli->cursor+1);
      cli->pos-=cli->cursor-start;
      cli->cursor=start;
      cli_redraw(cli);
    }
  }
}
static void cli_edit_ctrl_c(cli_ctx *cli){
  cli_hist_reset(cli);
  printf("^C\r\n");
  cli->pos=0;
  cli->cursor=0;
  cli->linebuf[0]='\0';
  cli->prompt_shown=0;
}
static void cli_edit_enter(cli_ctx *cli){
  cli->cursor=cli->pos;
  printf("\r\n");
}
static void cli_edit_clear_screen(cli_ctx *cli){
#ifdef _WIN32
  HANDLE hout=(HANDLE)cli->hstdout;
  COORD zero={0,0};
  DWORD nw;
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if(GetConsoleScreenBufferInfo(hout,&csbi)){
    FillConsoleOutputCharacterA(hout,' ',csbi.dwSize.X*csbi.dwSize.Y,zero,&nw);
    SetConsoleCursorPosition(hout,zero);
  }
#else
  write(STDOUT_FILENO,"\x1b[H\x1b[2J",7);
#endif
}
static void cli_edit_left(cli_ctx *cli){
  if(cli->cursor>0){
    cli->cursor--;
    cli_redraw(cli);
  }
}
static void cli_edit_right(cli_ctx *cli){
  if(cli->cursor<cli->pos){
    cli->cursor++;
    cli_redraw(cli);
  }
}
static void cli_edit_home(cli_ctx *cli){
  cli->cursor=0;
  cli_redraw(cli);
}
static void cli_edit_end(cli_ctx *cli){
  cli->cursor=cli->pos;
  cli_redraw(cli);
}
#ifdef _WIN32
static int cli_raw_init(cli_ctx *cli){
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  DWORD mode;
  HANDLE hin=(HANDLE)GetStdHandle(STD_INPUT_HANDLE);
  HANDLE hout=(HANDLE)GetStdHandle(STD_OUTPUT_HANDLE);
  if(hin==INVALID_HANDLE_VALUE||hout==INVALID_HANDLE_VALUE) return -1;
  cli->old_in_mode=0;
  if(!GetConsoleMode(hin,&mode)) return -1;
  cli->old_in_mode=mode;
  cli->hstdin=hin;
  cli->hstdout=hout;
  mode&=~((DWORD)(ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT|ENABLE_PROCESSED_INPUT));
  mode|=ENABLE_WINDOW_INPUT;
  if(!SetConsoleMode(hin,mode)) return -1;
  cli->raw=1;
  cli->tty=1;   /* a usable console: the interactive editor may run */
  if(GetConsoleScreenBufferInfo(hout,&csbi)) cli->term_w=csbi.dwSize.X;
  return 0;
}
static void cli_raw_shutdown(cli_ctx *cli){
  if(!cli->raw) return;
  SetConsoleMode((HANDLE)cli->hstdin,(DWORD)cli->old_in_mode);
  cli->raw=0;
}
static int cli_kbhit(cli_ctx *cli){
  DWORD total,got,i;
  INPUT_RECORD buf[8];
  if(!GetNumberOfConsoleInputEvents((HANDLE)cli->hstdin,&total)) return 0;
  while(total){
    DWORD n=total>8?8:total;
    if(!PeekConsoleInputA((HANDLE)cli->hstdin,buf,n,&got)) return 0;
    for(i=0;i<got;i++){
      if(buf[i].EventType==KEY_EVENT&&buf[i].Event.KeyEvent.bKeyDown) return 1;
      else if(buf[i].EventType==WINDOW_BUFFER_SIZE_EVENT) cli->need_redraw=1;
    }
    if(!ReadConsoleInputA((HANDLE)cli->hstdin,buf,got,&got)) break;
    if(got==0) break;
    total-=got;
  }
  return 0;
}
static int cli_readone(cli_ctx *cli){
  INPUT_RECORD rec;
  DWORD nread;
  if(!ReadConsoleInputA((HANDLE)cli->hstdin,&rec,1,&nread)||nread!=1) return -1;
  if(rec.EventType==WINDOW_BUFFER_SIZE_EVENT||rec.EventType==FOCUS_EVENT) cli->need_redraw=1;
  else if(rec.EventType==KEY_EVENT&&rec.Event.KeyEvent.bKeyDown){
    WORD vk=rec.Event.KeyEvent.wVirtualKeyCode;
    char ch=rec.Event.KeyEvent.uChar.AsciiChar;
    if(vk==VK_RETURN) { cli_edit_enter(cli);return 1; }
    else if(vk==VK_BACK) cli_edit_backspace(cli);
    else if(vk==VK_DELETE) cli_edit_delete(cli);
    else if(vk==VK_LEFT) cli_edit_left(cli);
    else if(vk==VK_RIGHT) cli_edit_right(cli);
    else if(vk==VK_HOME) cli_edit_home(cli);
    else if(vk==VK_END) cli_edit_end(cli);
    else if(vk==VK_UP) cli_hist_nav(cli,1);
    else if(vk==VK_DOWN) cli_hist_nav(cli,-1);
    else if(ch==0x01) cli_edit_home(cli);
    else if(ch==0x05) cli_edit_end(cli);
    else if(ch==0x0B) cli_edit_ctrl_k(cli);
    else if(ch==0x15) cli_edit_ctrl_u(cli);
    else if(ch==0x17) cli_edit_ctrl_w(cli);
    else if(ch==0x03) cli_edit_ctrl_c(cli);
    else if(ch==0x0C){ cli_edit_clear_screen(cli);cli_redraw(cli); }
    else if(ch>=0x20&&ch<=0x7E) cli_edit_insert(cli,ch);
    else if(ch==0x1A&&cli->pos==0){ printf("^Z\r\n");return -1; }
    else if(ch==0x04&&cli->pos==0){ printf("^D\r\n");return -1; }
  }
  return 0;
}
#else
static int cli_raw_init(cli_ctx *cli){
  struct termios *old;
  struct winsize ws;
  int flags=fcntl(STDIN_FILENO,F_GETFL,0);
  if(flags==-1) return -1;
  cli->old_flags=flags;
  if(fcntl(STDIN_FILENO,F_SETFL,flags|O_NONBLOCK)==-1) return -1;
  if(!isatty(STDIN_FILENO)){ cli->tty=0;return -1; }
  cli->tty=1;
  old=&cli->old_termios;
  if(tcgetattr(STDIN_FILENO,old)!=0) return -1;
  {
    struct termios raw=*old;
    raw.c_iflag&=~((tcflag_t)(BRKINT|ICRNL|INPCK|ISTRIP|IXANY|IXON|PARMRK));
    raw.c_oflag&=~((tcflag_t)OPOST);
    raw.c_lflag&=~((tcflag_t)(ECHO|ICANON|IEXTEN|ISIG));
    raw.c_cflag&=~((tcflag_t)(CSIZE|PARENB));
    raw.c_cflag|=CS8;
    raw.c_cc[VMIN]=1;
    raw.c_cc[VTIME]=0;
    if(tcsetattr(STDIN_FILENO,TCSAFLUSH,&raw)!=0){
      fcntl(STDIN_FILENO,F_SETFL,cli->old_flags);
      return -1;
    }
  }
  cli->raw=1;
  if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)!=-1) cli->term_w=ws.ws_col;
  return 0;
}
static void cli_raw_shutdown(cli_ctx *cli){
  if(cli->raw){
    tcsetattr(STDIN_FILENO,TCSAFLUSH,&cli->old_termios);
    cli->raw=0;
  }
  fcntl(STDIN_FILENO,F_SETFL,cli->old_flags);
}
static int cli_kbhit(cli_ctx *cli){
  int r;
  do{
    fd_set fds;
    struct timeval tv={0,0};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO,&fds);
    r=select(STDIN_FILENO+1,&fds,0,0,&tv);
  }while(r==-1&&errno==EINTR);
  return r>0;
}
static int cli_read_byte(unsigned char *out,int timeout_ms){
  ssize_t n;
  int sr;
  if(timeout_ms>0){
    do{
      fd_set fds;
      struct timeval tv={timeout_ms/1000,(timeout_ms%1000)*1000};
      FD_ZERO(&fds);
      FD_SET(STDIN_FILENO,&fds);
      sr=select(STDIN_FILENO+1,&fds,0,0,&tv);
    }while(sr==-1&&errno==EINTR);
    if(sr<=0) return 0;
  }
  do{
    n=read(STDIN_FILENO,out,1);
  }while(n==-1&&errno==EINTR);
  return n==1;
}
static int cli_parse_esc(cli_ctx *cli){
  unsigned char b,d;
  if(!cli_read_byte(&b,5)) return 0;
  if(b!='['&&b!='O') return 0;
  if(!cli_read_byte(&b,5)) return 1;
  if(b=='A') cli_hist_nav(cli,1);
  else if(b=='B') cli_hist_nav(cli,-1);
  else if(b=='C') cli_edit_right(cli);
  else if(b=='D') cli_edit_left(cli);
  else if(b=='H') cli_edit_home(cli);
  else if(b=='F') cli_edit_end(cli);
  else if(b=='3'){ if(cli_read_byte(&b,5)&&b=='~') cli_edit_delete(cli); }
  else if(b=='1'){ if(cli_read_byte(&b,5)&&b=='~') cli_edit_home(cli); }
  else if(b=='4'){ if(cli_read_byte(&b,5)&&b=='~') cli_edit_end(cli); }
  else while(cli_read_byte(&d,5)){}
  return 1;
}
static int cli_readone(cli_ctx *cli){
  unsigned char ch;
  ssize_t n;
  do{
    n=read(STDIN_FILENO,&ch,1);
  }while(n==-1&&errno==EINTR);
  if(n<0){
    if(errno==EAGAIN||errno==EWOULDBLOCK) return 0;
    return -1;
  }
  if(n==0) return -1;
  if(ch==0x1B){ cli_parse_esc(cli);return 0; }
  if(ch=='\r'||ch=='\n') { cli_edit_enter(cli);return 1; }
  else if(ch=='\b'||ch==0x7F) cli_edit_backspace(cli);
  else if(ch==0x01) cli_edit_home(cli);
  else if(ch==0x05) cli_edit_end(cli);
  else if(ch==0x0B) cli_edit_ctrl_k(cli);
  else if(ch==0x15) cli_edit_ctrl_u(cli);
  else if(ch==0x17) cli_edit_ctrl_w(cli);
  else if(ch==0x03) cli_edit_ctrl_c(cli);
  else if(ch==0x0C){ cli_edit_clear_screen(cli);cli_redraw(cli); }
  else if(ch==0x04&&cli->pos==0){ printf("^D\r\n");return -1; }
  else if(ch==0x1A&&cli->pos==0){ printf("^Z\r\n");return -1; }
  else if(ch>=0x20&&ch<=0x7E) cli_edit_insert(cli,(char)ch);
  return 0;
}
#endif
CLI_DEF int cli_init(cli_ctx *cli,const char *prompt,const cli_cmd *cmds,void *ctx,cli_post_fn post,char *linebuf,unsigned int linesz,char **argv,int maxargc){
  char *save_hist_buf,*save_hist_temp_buf;
  int save_hist_max,save_hist_len;
  if(!cli||!linebuf||!argv||linesz<2||maxargc<=0) return -1;
  save_hist_buf=cli->hist_buf;
  save_hist_max=cli->hist_max;
  save_hist_len=cli->hist_len;
  save_hist_temp_buf=cli->hist_temp_buf;
  memset(cli,0,sizeof(*cli));
  cli->hist_buf=save_hist_buf;
  cli->hist_max=save_hist_max;
  cli->hist_len=save_hist_len;
  cli->hist_temp_buf=save_hist_temp_buf;
  cli->hist_idx=-1;
  cli->prompt=prompt;
  cli->cmds=cmds;
  cli->ctx=ctx;
  cli->post=post;
  cli->linebuf=linebuf;
  cli->linesz=linesz;
  cli->argv=argv;
  cli->maxargc=maxargc;
  if(cli_raw_init(cli)!=0){
    /* No console (stdout/stdin redirected, a pipe, or a service): degrade to a
       NON-interactive session instead of refusing to start.  cli_print still writes via
       printf+fflush and cli_exec_line still dispatches, so scripted use works; without
       this an operator gets a silent process that exits before even connecting. */
    cli->tty=0;
    cli->raw=0;
    cli->hstdin=0;
    cli->hstdout=0;
    cli->prompt_shown=0;
  }
  return 0;
}
CLI_DEF void cli_shutdown(cli_ctx *cli){
  if(cli) cli_raw_shutdown(cli);
}
static int cli_tokenize(char *line,char **argv,int maxargc){
  int argc=0;
  char *p=line;
  for(;;){
    while(*p==' '||*p=='\t') p++;
    if(!*p) break;
    if(argc==maxargc) return -2;
    argv[argc++]=p;
    if(*p=='"'){
      char *dst=++p;
      argv[argc-1]=dst;
      while(*p&&*p!='"'){
        if(*p=='\\'){
          if(!p[1]) return -1;
          p++;
        }
        *dst++=*p++;
      }
      if(*p!='"') return -1;
      *dst='\0';
      p++;
      if(*p&&*p!=' '&&*p!='\t') return -1;
    }else{
      while(*p&&*p!=' '&&*p!='\t'){
        if(*p=='"') return -1;
        p++;
      }
      if(*p) *p++='\0';
    }
  }
  return argc;
}
static const cli_cmd *cli_find_cmd(const cli_cmd *cmds,const char *name){
  while(cmds&&cmds->name){
    const char *s1=cmds->name,*s2=name;
    while(*s1&&*s2&&((*s1>='A'&&*s1<='Z'?*s1+32:*s1)==(*s2>='A'&&*s2<='Z'?*s2+32:*s2))){ s1++;s2++; }
    if(*s1==*s2) return cmds;
    cmds++;
  }
  return 0;
}
static int cli_dispatch(cli_ctx *cli){
  int argc;
  cli->prompt_shown=0;
  cli->linebuf[cli->pos]='\0';
  argc=cli_tokenize(cli->linebuf,cli->argv,cli->maxargc);
  if(argc==-1){
    printf("Error: invalid quote syntax\r\n");
    return 0;
  }
  if(argc==-2){
    printf("Error: too many arguments (max=%d)\r\n",cli->maxargc);
    return 0;
  }
  if(argc>0){
    const cli_cmd *cmd=(cli->argv[0]&&cli->argv[0][0])?cli_find_cmd(cli->cmds,cli->argv[0]):0;
    if(cmd){
      if(cli->post){
        cli->post(cli,cmd,argc,cli->argv);
        return 1;
      }else if(cmd->fn){
        if(cmd->fn(cli,argc,cli->argv)) return -1;
        return 1;
      }else printf("Command has no callback: %s\r\n",cli->argv[0]);
    }else printf("Unknown command: %s\r\n",cli->argv[0]);
  }
  return 0;
}
/* Run ONE command line supplied by the caller (no keyboard/tty involvement).  Returns
   cli_dispatch's result: -1 = quit, 1 = handled, 0 = nothing/unknown.  This is what makes
   the CLI usable from scripts and from automation, where a prompt-driven REPL is useless. */
CLI_DEF int cli_exec_line(cli_ctx *cli,const char *line){
  size_t len;
  int rc;
  if(!cli||!cli->linebuf||!cli->argv||!line) return 0;
  len=strlen(line);
  if(len>=(size_t)cli->linesz) len=(size_t)cli->linesz-1u;
  memcpy(cli->linebuf,line,len);
  cli->linebuf[len]='\0';
  cli->pos=(int)len;
  cli->cursor=(int)len;
  cli->prompt_shown=1;
  rc=cli_dispatch(cli);
  cli->pos=0;
  cli->cursor=0;
  cli->view_off=0;
  return rc;
}
static void cli_prompt_show(cli_ctx *cli){
  if(!cli->tty){ cli->prompt_shown=1; return; }   /* script mode: never draw a prompt */
  printf("%s",cli->prompt?cli->prompt:"");
  fflush(stdout);
  cli->prompt_shown=1;
}
CLI_DEF int cli_poll(cli_ctx *cli){
  int n=0;
  if(!cli||!cli->linebuf||!cli->argv||cli->linesz<2||cli->maxargc<=0||cli->stop) return -1;
#ifndef _WIN32
  if(cli->tty){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)!=-1&&ws.ws_col>0&&(int)ws.ws_col!=cli->term_w){
      cli->term_w=(int)ws.ws_col;
      cli->need_redraw=1;
    }
  }
#endif
  if(!cli->prompt_shown) cli_prompt_show(cli);
#ifndef CLI_MAX_EVENTS_PER_POLL
#define CLI_MAX_EVENTS_PER_POLL 64
#endif
  while(cli->tty&&n<CLI_MAX_EVENTS_PER_POLL&&cli_kbhit(cli)){
    int r=cli_readone(cli);
    n++;
    if(r==1){
      int rc;
      cli->linebuf[cli->pos]='\0';
      cli_hist_add(cli);
      rc=cli_dispatch(cli);
      cli->pos=0;
      cli->cursor=0;
      cli->view_off=0;
      cli_hist_reset(cli);
      if(rc==-1||cli->stop){
        cli->stop=1;
        return -1;
      }
      cli_prompt_show(cli);
      return rc;
    }
    if(r==-1){
      cli->stop=1;
      return -1;
    }
  }
  if(cli->need_redraw){
    cli->need_redraw=0;
    cli_redraw(cli);
  }
  return 0;
}
#ifndef CLI_PRINT_BUFSIZE
#define CLI_PRINT_BUFSIZE 1024
#endif
#if defined(_MSC_VER)
#define vsnprintf _vsnprintf
#endif
CLI_DEF void cli_print(cli_ctx *cli,const char *fmt,...){
  char msgbuf[CLI_PRINT_BUFSIZE];
  va_list args;
  va_start(args,fmt);
  vsnprintf(msgbuf,sizeof(msgbuf),fmt,args);
  va_end(args);
  msgbuf[sizeof(msgbuf)-1]='\0';
  if(cli&&cli->tty&&cli->prompt_shown){
    cli_clear_line(cli);
    printf("%s\r\n",msgbuf);
    cli_redraw(cli);
  }else{
    printf("%s\r\n",msgbuf);
  }
  fflush(stdout);
}
CLI_DEF void cli_stop(cli_ctx *cli){
  if(cli) cli->stop=1;
}
CLI_DEF int cli_should_stop(cli_ctx *cli){
  return cli?cli->stop:1;
}
#endif
