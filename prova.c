#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#define DEBUG 1

void do_work_child(char* path, int len);
static char *rand_string(size_t len);


void create_n_process(int n , int len , char* file){
  int pids[n];
  int i;
  /* Start children. */
  for (i = 0; i < n; ++i) {
    if ((pids[i] = fork()) < 0) {
      return;
    } else if (pids[i] == 0) {
      //Do Work In Child
      do_work_child(file,len);
      exit(0);
    }
  }
  int status;
  int pid;
  while (n > 0) {
    pid = wait(&status);
    n--;
  }
}

static char *rand_string(size_t len){
    char* str = malloc(len*sizeof(char));
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJK...";
    if (len) {
        --len;
        for (size_t n = 0; n < len; n++) {
            int key = rand(time(NULL)) % (int) (sizeof charset - 1);
            str[n] = charset[key];
        }
        str[len] = '\0';
    }
    return str;
}

void do_work_child(char* path, int len){
  FILE* fp;
  char* buff;
  if (DEBUG) printf("in child process with pid %d, mode = %d : %s\n",getpid(),getpid()%2, getpid()%2?"read":"write" );
  //len should be the len of the mailslot
  int mode = getpid()%2; //binary random value dependent to the pid of the process
  if ( mode ){ //mode 1 is reading
    buff = malloc(len*sizeof(char));
    fp = fopen(path, "r");
    if ( fp ){
      memset( buff , 0 , len );
      buff = fgets( buff, len, fp);
      if ( buff == NULL ) printf("void buffer , cannot read\n");
      printf("process PID %d tried a read : result %s of len %d\n", getpid() , buff , strlen(buff));
      fclose(fp);
    }
  }
  else{
    fp = fopen(path, "w");
    if ( fp ){
      memset(buff , 0 , len );
      buff = rand_string(len);
      int ret = fprintf(fp, buff );
      printf("process PID %d tried a write : result %d\n",getpid() , ret );
      fclose(fp);
    }
  }
}



int main(int argc, char const *argv[]) {

create_n_process(5, 20 , "aux.txt");

}
