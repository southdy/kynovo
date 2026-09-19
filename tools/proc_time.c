/* proc_time.c -- read-only helper: print cumulative CPU time (ms) of a process.
   Used by the performance harness to split wall time into server-side CPU and
   the rest (client CPU + round trips).  Windows XP compatible: uses only
   GetProcessTimes / OpenProcess from kernel32.  Build:
     gcc -std=c89 -O2 -Wall -o build/proc_time.exe tools/proc_time.c
*/
#define _WIN32_WINNT 0x0501
#if defined(_WIN32)
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>

int main(int argc,char **argv){
  HANDLE handle;
  FILETIME create,exit,ktime,utime;
  ULARGE_INTEGER total_user,total_kernel;
  double user_ms,kernel_ms;
  if(argc<2){
    fprintf(stderr,"usage: proc_time <pid>\n");
    return 2;
  }
  handle=OpenProcess(PROCESS_QUERY_INFORMATION,FALSE,(DWORD)atoi(argv[1]));
  if(handle==NULL){
    handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,(DWORD)atoi(argv[1]));
  }
  if(handle==NULL){
    fprintf(stderr,"proc_time: OpenProcess(pid=%s) failed err=%lu\n",argv[1],(unsigned long)GetLastError());
    return 1;
  }
  if(!GetProcessTimes(handle,&create,&exit,&ktime,&utime)){
    fprintf(stderr,"proc_time: GetProcessTimes failed err=%lu\n",(unsigned long)GetLastError());
    CloseHandle(handle);
    return 1;
  }
  CloseHandle(handle);
  total_user.LowPart=utime.dwLowDateTime;
  total_user.HighPart=utime.dwHighDateTime;
  total_kernel.LowPart=ktime.dwLowDateTime;
  total_kernel.HighPart=ktime.dwHighDateTime;
  user_ms=(double)total_user.QuadPart/10000.0;
  kernel_ms=(double)total_kernel.QuadPart/10000.0;
  printf("user_ms=%.1f kernel_ms=%.1f total_ms=%.1f\n",user_ms,kernel_ms,user_ms+kernel_ms);
  return 0;
}
