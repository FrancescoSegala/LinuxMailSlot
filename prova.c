#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>




#define DEBUG if( 1 )
#define READ  1
#define WRITE  0

void do_work_child(char* path, int len, int mode);
static char *rand_string(size_t len);

void open_close(char* path){
  int fd = open(path , O_RDWR);
  printf("open done %d \n" ,fd);
  int ret = close(fd);
  printf("close done %d\n",ret );
  int fs = open("Node2" , O_RDWR);
  printf("open 2 done %d \n" ,fs);
  int res = close(fs);
  printf("close done %d\n",res );

}


void test_ioctl(char* path,int param , int value){
  int fd = open(path , O_RDWR);
  if (fd ) {

    int rc = ioctl(fd, param , value);//GET_SLOT_SIZE 111
  }
}

void create_n_process(int n , int len , char* file){
  int pids[n];
  int i;
  /* Start children. */
  for (i = 0; i < n; ++i) {
    if ((pids[i] = fork()) < 0) {
      return;
    } else if (pids[i] == 0) {
      //Do Work In Child
      do_work_child(file,len,(rand()+ getpid())%2);
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
            int key = rand() % (int) (sizeof charset - 1);
            str[n] = charset[key];
        }
        str[len] = '\0';
    }
    return str;
}

void do_work_child(char* path, int len , int mode ){
  int fp;
  char* buff = malloc(len*sizeof(char));
  DEBUG printf("in child process with pid %d, mode = %d : %s\n",getpid(),getpid()%2, getpid()%2?"write":"read" );
  //len should be the len of the mailslot
  if ( mode ){ //mode 1 is reading
    fp = open(path, O_RDWR);
    if ( fp ){
      memset( buff , 0 , len );
      int ret = read( fp, buff, len);
      if ( ret == -1 ) printf("process %d cannot read\n", getpid());
      DEBUG printf("process PID %d tried a read : result of len %ld\n\n", getpid() , strlen(buff));
      printf("%s\n",buff );
      close(fp);
    }
  }
  else{
    DEBUG printf("[write mode]\n" );
    fp = open(path, O_RDWR);
    if ( fp ){
      memset(buff , 0 , len );
      buff = rand_string(len);
      int ret = write(fp, buff ,len);
      if ( ret != len ) printf("error in writing process %d written %d len %d\n",getpid(),ret,len );
      DEBUG printf("process PID %d tried a write : result %d\n\n",getpid() , ret );
      close(fp);
    }
    else printf("cannot open file %s\n",path  );
  }
}



int main(int argc, char const *argv[]) {

  create_n_process(5, 256 ,"testNode" );
  //do_work_child("testNode", 256 , WRITE);
  //do_work_child("testNode", 256 , READ);
  test_ioctl("testNode",111,0);
  return 0;
}
