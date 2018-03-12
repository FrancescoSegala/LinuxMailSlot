#define EXPORT_SYMTAB
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/version.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

MODULE_AUTHOR("Francesco Segala - francesco.segala10@gmial.com")
MODULE_LICENSE("GPL")
MODULE_DESCRIPTION("this project deals with implementing within Linux services similar to those that are offered by Windows mail slots")
/*spec*/
/*
This specification related the implementation of a special device file that is accessible according to FIFO style semantic
(via open/close/read/write services), but offering an execution semantic of read/write services such that any segment that is
 posted to the stream associated with the file is seen as an independent data unit (a message), thus being posted and delivered
 atomically (all or nothing) and in data separation (with respect to other segments) to the reading threads.
The device file needs to be multi-instance (by having the possibility to manage at least 256 different instances) so that mutiple
 FIFO style streams (characterized by the above semantic) can be concurrently accessed by active processes/threads.

The device file needs to also support ioctl commands in order to define the run time behavior of any I/O session
 targeting it (such as whether read and/or write operations on a session need to be performed according to blocking or non-blocking rules).

Parameters that are left to the designer, which should be selected via reasonable policies, are:

the maximum size of managed data-units (this might also be made tunable via ioctl up to an absolute upper limit)
the maximum storage that can be (dynamically) reserved for any individual mail slot
the range of device file minor numbers supported by the driver (it could be the interval [0-255] or not)
*/
#define MODNAME "LINUXMAILSLOT"
#define DEVICE_NAME "mail_slot"

//module tunable parameters and const
#define MINOR_CURRENT MINOR(filp->f_dentry->d_inode->i_rdev);
#define MAX_MINOR_NUM 255
#define MAX_MESSAGE_SIZE 512
#define INIT_MESSAGE_SIZE 256
#define NO 0
#define YES 1
#define NON_BLOCKING 0
#define BLOCKING 1

//mudule error codes
#define SUCCESS 0
#define FAILURE -1
#define MSOPEN_ERROR -1
#define MSWRITE_ERROR -2
#define MSREAD_ERROR -3
#define MSPUSH_ERROR -4
#define NOT_ENOUGH_SPACE_ERROR -5

//IOCTL param
#define CHANGE_MESSAGE_SIZE 10
#define CHANGE_BLOCKING_MODE 11


//message
typedef struct Message{
  char* payload;
  size_t size;
  message* next;
} message;

//process list elem, every list elem is a mail slot, it contains reference to 3 lists : message list, wait read process list,
// wait write process list
typedef struct List_Elem{
  list_elem* prev;
  list_elem* next;
  struct task_struct *task;	//puntatore al pcb del thread che ha insertito questo record nella WQ
  int awake; //flag che va ad indicare se la condizione di risveglio è verificata 0 o 1
  int already_hit;
}list_elem;


//wrapper for list elem coolections
typedef struct List{
  list_elem* head;
  list_elem* queue;
}list;


typedef struct Slot_elem{
  list* w_queue;
  list* r_queue;
  message* head;
  message* tail;
  int free_mem;
  spinlock_t queue_lock;
  int blocking;
  ssize_t curr_size;
} slot_elem;


//list
static list_elem* mailslots[MAX_MINOR_NUM];

//functions declaration
static int lms_open(struct inode *inode , struct file *file);
static int lms_release(struct inode *inode, struct file *file);
static ssize_t lms_write(struct file *filp, const char *buff, size_t len, loff_t *off);
static ssize_t lms_read(struct file *filp, const char *buff, size_t len, loff_t *off);
static long lms_ioctl(struct inode *, struct file *, unsigned int param, unsigned long value);
static ssize_t push_message(slot_elem* elem, char* payload, ssize_t len);
static ssize_t pop_message(slot_elem* elem, char* out_buff);




static int lms_open(struct inode *inode, struct file *file){

  if (MINOR_CURRENT<0 || MINOR_CURRENT > MAX_MINOR_NUM ){
    printk("Cannot open the device, minor number not allowed");
    return MSOPEN_ERROR;
  }

  printk("Device opened and new LMS instance created with minor %d\n", MINOR_CURRENT);
  try_module_get(THIS_MODULE);
  return 0;
}

static int lms_release(struct inode *inode, struct file *file){
  printk("Device closing...closed a LMS instance with minor %d",MINOR_CURRENT);
  module_put(THIS_MODULE);
  return 0;
}



static ssize_t push_message(slot_elem* elem, char* payload, ssize_t len){

  struct message* pushed_msg = kmalloc(sizeof(struct message), GFP_KERNEL);//memory allocation

  if (pushed_msg == NULL){ //error in allocation check
    printk("Error while allocating memory for pushing a new message for the entry %d", MINOR_CURRENT);
    return MSPUSH_ERROR;
  }

  memset(pushed_msg->payload, 0, len );   //cleaning memory before using
  copy_from_user(pushed_msg->payload, payload,len ); //Copy a block of data from user space memory to kernel memory (to,from,len)
  pushed_msg->size = len;
  pushed_msg->next = NULL;    //completing the message data structure
  //and updating it
  if( elem->head == NULL ) {
    //empty message queue
    elem->head = pushed_msg;
    elem->tail = pushed_msg;
  }
  else {
    //push the message to the tail of the message queue
    elem->tail->next = pushed_msg;
    elem->tail = pushed_msg;
  }
  return SUCCESS;
}



static void pop_message(slot_elem* elem, char* out_buff){
  copy_to_user(out_buff, elem->head->payload, elem->head->size); //put the message into the buffer (to,from.len)
  message* head_aux = elem->head;
  elem->head = elem->head->next; //pop the readed message
  elem->free_mem += head_aux->size; //update the free memory of the slot
  kfree(head_aux->payload); //release the message head memory
  kfree(head_aux);
}

static ssize_t lms_write(struct file *filp, const char *buff, size_t len, loff_t *off){

  volatile list_elem me;
  list_elem *aux;
  DECLARE_WAIT_QUEUE_HEAD(the_queue);//here we use a private queue - wakeup is selective via wake_up_process
  me.next = NULL;
  me.prev = NULL;
  me.task = current;
	me.awake = NO;
	me.already_hit = NO;

  if ( len > mailslots[MINOR_CURRENT]->curr_size  || len <= 0 ){
    printk("%s: lms_write error, len to write not compliant with the spec. \n " , MODNAME);
    return FAILURE;
  }

  //lock the mailslot elem
  spin_lock( &(mailslots[MINOR_CURRENT]->queue_lock) );

  while( mailslots[MINOR_CURRENT]->free_mem < len ){
    //not enough free space
    //if in blocking mode then wait else exit

    if ( mailslots[MINOR_CURRENT]->blocking == NON_BLOCKING ){
      spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock) );
      return NOT_ENOUGH_SPACE_ERROR;
    }

    aux = mailslots[MINOR_CURRENT]->w_queue->tail;
    /*check on the regularity of the queue*/

    if(aux->prev == NULL){
        spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock) );
        printk("%s: malformed sleep-list - service damaged\n",MODNAME);
        return FAILURE;
    }

    if (aux == NULL ){
      //empty queue but cannot write? error somewhere
      spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock) );
      printk("%s: lms_write error! writing queue empty but cannot write\n",MODNAME);
      return FAILURE;
    }

    //then the process put himself in the tail of the writing queue and we save a pointer to our position
    //remember : AUX is the tail
    aux->next = &me;
    me.prev = aux;
    me.next = NULL ;
    mailslots[MINOR_CURRENT]->w_queue->tail = &me;

    //release the lock and wait for the event
    spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock)  );

    int ret = wait_event_interruptible(the_queue, mailslots[MINOR_CURRENT]->free_mem >= len);
    /*TODO insert return code check*/

    //now the writer has to delete himself from the w_queue
    spin_lock( &(mailslots[MINOR_CURRENT]->queue_lock)  );

    me->prev->next = me->next;
    me->next->prev = me->prev;
    //done
  }

  //once you know you can write your message because there is enough space
  //push the message to the message queue and decrease the slot capacity
  push_message(mailslots[MINOR_CURRENT], buff , len);
  mailslots[MINOR_CURRENT]->free_mem -= len;

  //awake a reader process that is waiting
  aux = mailslots[MINOR_CURRENT]->r_queue->head;
  while ( aux->next != NULL ){
    if ( aux->already_hit == NO ){
      aux->already_hit = YES ;
      aux->awake = YES ;
      wake_up_process(aux->task);
      break;
    }
    aux= aux->next;
  }
  //then release the lock and return the number of byte written
  spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock) );
  return len;
}


/*



*/

static ssize_t lms_read(struct file *filp, const char *buff, size_t len, loff_t *off){

  volatile list_elem me;
  list_elem *aux;
  DECLARE_WAIT_QUEUE_HEAD(the_queue);//here we use a private queue - wakeup is selective via wake_up_process
  me.next = NULL;
  me.prev = NULL;
  me.task = current;
	me.awake = NO;
	me.already_hit = NO;

  //check on len : has to be equal to the size of the message
  if ( len <= 0  ){
    printk("%s: called a read with negative buffer len \n",MODNAME);
    return FAILURE;
  }
  else if ( len < mailslots[MINOR_CURRENT]->head->size  ){
    printk( "%s: called a read with a len not compliant with the message size, the read hs to be all or nothing ", MODNAME );
    return FAILURE;
  }
  else {
    len = mailslots[MINOR_CURRENT]->head->size;
  }

  spin_lock( &(mailslots[MINOR_CURRENT]->queue_lock) );
  //acquire the lock in order to read the message slot

  while( mailslots[MINOR_CURRENT]->head == NULL ){
    //no messages to read!
    if ( mailslots[MINOR_CURRENT]->blocking == NON_BLOCKING_MODE ){
      //quit
      printk("%s: No messages to read in this mailslot, exiting...\n",MODNAME);
      spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock) );
      return FAILURE;
    }
    //the reader process has to put himself in the reader queue
    aux = mailslots[MINOR_CURRENT]->r_queue->head;
    // if the queue is empty initialize a new queue : head and tail
    if ( aux == NULL ){
        mailslots[MINOR_CURRENT]->r_queue->head = &me;
        mailslots[MINOR_CURRENT]->r_queue->tail = &me;
    }
    else {
      aux = mailslots[MINOR_CURRENT]->r_queue->tail;
      //otherwise put it on the tail and update
      aux->next = &me ;
      (&me)->prev = aux ;
      mailslots[MINOR_CURRENT]->r_queue->tail = mailslots[MINOR_CURRENT]->r_queue->tail->next;
    }
    spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock) );
    int ret = wait_event_interruptible(the_queue, mailslots[MINOR_CURRENT]->head != NULL );

    if ( ret != 0 ){
      /*the function will return -ERESTARTSYS if it was interrupted by a signal and 0 if condition evaluated to true.*/
      printk("%s: The process %d has been awaken by a signal\n", MODNAME , current->pid);
      return FAILURE;
    }

    //once waked up the reader delete himself from the waitqueue , then the loop is over
    //and the process can pop a message from the queue in CS
    spin_lock( &(mailslots[MINOR_CURRENT])->queue_lock );
    me.prev->next = me.next;
    me.next->prev = me.prev;
  }

  //poping the message from the mailslot
  pop_message(mailslots[MINOR_CURRENT], buff);
  //now the reader has to signal to the writers waiting that there is a new slot ready
  //then is his duty to remove himself from the w_queue
  aux = mailslots[MINOR_CURRENT]->w_list->head;
  while ( aux != NULL ){
    if ( aux->already_hit == NO ){
      aux->awake = YES;
      aux->already_hit = NO;
      wake_up_process(aux->task);
      break;
    }
    aux = aux->next;
  }
  spin_unlock( &(mailslots[MINOR_CURRENT])->queue_lock );
  return len;
}

/*





*/
static long lms_ioctl(struct inode *, struct file *, unsigned int param, unsigned long value){
  //since this function has not to be queued we try to get the lock
  int status = SUCCESS ;
  if ( spin_trylock( &(mailslots[MINOR_CURRENT]->queue_lock) )  == 0 ){
    if ( mailslots[MINOR_CURRENT]->blocking == NON_BLOCKING ){
      printk("%s: trying to acquire a lock in a mailslot already locked. Error.",MODNAME);
      return FAILURE;
    }
    else {
      spin_lock(&(mailslots[MINOR_CURRENT]->queue_lock));
    }
  }
  switch (param) {

    case CHANGE_BLOCKING_MODE:
      if (value == BLOCKING || value == NON_BLOCKING){
        mailslots[MINOR_CURRENT]->blocking = value;
        status = SUCCESS;
      }
      else {
        printk("%s: Error, change blocking mode parameter value not found!\n",MODNAME);
        status = FAILURE;
      }
      break ;

    case CHANGE_MESSAGE_SIZE:
      if ( value <= MAX_MESSAGE_SIZE && value > 0){
        mailslots[MINOR_CURRENT]->curr_size = value;
        status = SUCCESS;
      }
      else {
        printk("%s: Error, change slot size parameter value not compliant with the spec (MAX %d) ", MODNAME, MAX_MESSAGE_SIZE );
        status = FAILURE;
      }
      break;

    case GET_SLOT_SIZE:
      printk("%s: current slot size of entry with minor %d is %d ", MODNAME, MINOR_CURRENT, mailslots[MINOR_CURRENT]->curr_size);
      break;

    default:
      printk("%s: command not found", MODNAME);
      break;
  }
  spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock) );
  return status;
}

/*


*/

static struct file_operations fops = {
  .owner= THIS_MODULE,
  .open =  lms_open,
  .write = lms_write,
  .read = lms_read,
  .unlocked_ioctl= lms_ioctl,
  .release= lms_release
};

int init_module(void) {}

void cleanup_module(void){}
