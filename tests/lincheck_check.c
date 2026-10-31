/* lincheck_check.c -- command-line wrapper around tests/lincheck.h.

   lincheck.h is a header that the fuzz harness includes, so the only way the checker reaches a build
   target of its own is through a driver.  This one reads a history file (the grammar below, written by
   tests/lincheck_parity.py), runs BOTH of lincheck.h's engines over every history and prints one verdict
   per history:

     V <memoized> <plain-oracle>

   which lets the Python side compare the production checker against the reference implementation in
   tests/lincheck.py and, for short histories, against an exhaustive permutation search.  The driver runs
   lincheck_selftest() first: a checker whose own regression cases fail must not be used as an oracle.

   Exit status: 0 = every history decided and both engines agreed; 1 = the selftest failed, the two engines
   disagreed, or the input was malformed (each is named on stderr); 2 = usage or an unreadable file.
   A history longer than LIN_MAX is NOT truncated: lin_linearizable() answers LIN_INCONCLUSIVE for it,
   which is exactly the documented difference from tests/lincheck.py, so the Python side can check that
   answer instead of trusting a silently shortened history.

   History-file grammar (one history per 'H' line; ops in file order):
     H <n> <init>            n = how many O lines follow, init = the register's value before the history
     O w <val> <inv> <resp>  a write of <val>
     O r <val> <inv> <resp>  a read that returned <val>
   LIN_ABSENT (-1) is the "key absent" value; inv/resp are the logical invocation/response timestamps.
   Blank lines and lines that start with another character are ignored. */
#include <stdio.h>
#include "lincheck.h"

#define LC_MAX_OPS 256   /* an input history may legitimately be longer than LIN_MAX's op mask */

static char lc_line[256];

static void lc_flush(const lin_op *ops,int n,int init,int *count,int *bad){
    int memo=lin_linearizable(ops,n,init);
    int oracle=lin_oracle(ops,n,init);
    if(memo!=oracle){
        fprintf(stderr,"lincheck_check: engine disagreement on history %d (n=%d): memo=%d oracle=%d\n",
                *count,n,memo,oracle);
        lin_dump(ops,n);
        *bad=1;
    }
    printf("V %d %d\n",memo,oracle);
    (*count)++;
}

int main(int argc,char **argv){
    lin_op ops[LC_MAX_OPS];
    FILE *f;
    int n=0,init=0,oi=0,have=0,count=0,bad=0,lineno=0,val=0,rc=0;
    char kind=0;
    long inv=0,resp=0;

    if(argc!=2){
        fprintf(stderr,"usage: lincheck_check <history-file>\n");
        return 2;
    }
    f=fopen(argv[1],"r");
    if(f==NULL){
        fprintf(stderr,"lincheck_check: cannot open %s\n",argv[1]);
        return 2;
    }
    if(lincheck_selftest()!=0){
        fprintf(stderr,"lincheck_check: lincheck_selftest FAILED - this checker is not fit to judge\n");
        fclose(f);
        return 1;
    }
    printf("selftest: PASS\n");

    while(fgets(lc_line,sizeof(lc_line),f)!=NULL){
        lineno++;
        if(lc_line[0]=='H'){
            if(have){
                if(oi!=n){
                    fprintf(stderr,"lincheck_check: %s:%d: history announced %d ops but carried %d\n",
                            argv[1],lineno,n,oi);
                    bad=1;
                }
                lc_flush(ops,n,init,&count,&bad);
            }
            if(sscanf(lc_line,"H %d %d",&n,&init)!=2||n<0||n>LC_MAX_OPS){
                fprintf(stderr,"lincheck_check: %s:%d: malformed history header\n",argv[1],lineno);
                bad=1;
                have=0; n=0; oi=0;
                continue;
            }
            have=1; oi=0;
        }else if(lc_line[0]=='O'){
            kind=0; val=0; inv=0; resp=0;
            if(!have||oi>=n){
                fprintf(stderr,"lincheck_check: %s:%d: op outside a history or past its announced length\n",
                        argv[1],lineno);
                bad=1;
                continue;
            }
            if(sscanf(lc_line,"O %c %d %ld %ld",&kind,&val,&inv,&resp)!=4||(kind!='w'&&kind!='r')){
                fprintf(stderr,"lincheck_check: %s:%d: malformed op line\n",argv[1],lineno);
                bad=1;
                continue;
            }
            ops[oi].is_write=(kind=='w')?1:0;
            ops[oi].val=val;
            ops[oi].inv=inv;
            ops[oi].resp=resp;
            oi++;
        }
    }
    if(have){
        if(oi!=n){
            fprintf(stderr,"lincheck_check: %s: final history announced %d ops but carried %d\n",
                    argv[1],n,oi);
            bad=1;
        }
        lc_flush(ops,n,init,&count,&bad);
    }
    fclose(f);
    printf("read: %d histories\n",count);
    if(count==0){
        fprintf(stderr,"lincheck_check: %s contained no history - nothing was decided\n",argv[1]);
        bad=1;
    }
    if(bad) rc=1;
    return rc;
}
