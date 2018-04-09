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
#include <linux/device.h>

MODULE_AUTHOR("Francesco Segala - francesco.segala10@gmial.com");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("this project deals with implementing within Linux services similar to those that are offered by Windows mail slots");
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
#define MAX_MINOR_NUM 255
#define MAX_MESSAGE_SIZE 512
#define INIT_MESSAGE_SIZE 256
#define MAX_SLOT_SIZE 128
#define NO 0
#define YES 1
#define NON_BLOCKING 0
#define BLOCKING 1
#define DEBUG 1

//mudule error codes
#define SUCCESS 0
#define FAILURE -1
#define MSOPEN_ERROR -1
#define MSWRITE_ERROR -2
#define MSREAD_ERROR -3
#define MSPUSH_ERROR -4
#define NOT_ENOUGH_SPACE_ERROR -5

//IOCTL param
#define CHANGE_MESSAGE_SIZE 100
#define CHANGE_BLOCKING_MODE 110
#define GET_SLOT_SIZE 111


//message
typedef struct Message{
  char* payload;
  size_t size;
  struct Message* next;
} message;


//list elem , this entry of the list represent a process waiting on that WQ
typedef struct List_Elem{
  struct List_Elem* prev;
  struct List_Elem* next;
  struct task_struct *task;	//pointer to the thread PCB that inserted this process in the WQ
  int awake;
  int already_hit;
}list_elem;


//wrapper for list elem collections
typedef struct List{
  list_elem* head;
  list_elem* tail;
}list;

//mailslot element
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

//
static int major_number = 0;
//the mailslots list
static slot_elem* mailslots[MAX_MINOR_NUM];

//functions declaration
static int lms_open(struct inode *inode , struct file *file);
static int lms_release(struct inode *inode, struct file *file);
static ssize_t lms_write(struct file *filp, const char *buff, size_t len, loff_t *off);
static ssize_t lms_read(struct file *filp, char *buff, size_t len, loff_t *off);
static long lms_ioctl( struct file *, unsigned int , unsigned long );
static ssize_t push_message(slot_elem* elem,const char* payload, ssize_t len, struct file * filp);
static void pop_message(slot_elem* elem, char* out_buff);




static int lms_open(struct inode *inode, struct file *file){
  const int MINOR_CURRENT = iminor(inode);
  if (MINOR_CURRENT<0 || MINOR_CURRENT > MAX_MINOR_NUM ){
    printk( KERN_INFO "%s: Cannot open the device, minor number not allowed", MODNAME);
    return MSOPEN_ERROR;
  }

  printk(KERN_INFO "%s: Device opened and new LMS instance created with minor %d\n", MODNAME, MINOR_CURRENT);
  return SUCCESS;
}


static int lms_release(struct inode *inode, struct file *file){
  const int MINOR_CURRENT = iminor(inode);
  printk(KERN_INFO "%s: Device closing...closed a LMS instance with minor %d", MODNAME, MINOR_CURRENT );
  return SUCCESS;
}



static ssize_t push_message(slot_elem* elem,const char* payload, ssize_t len, struct file * filp){

  const int MINOR_CURRENT = iminor(filp->f_path.dentry->d_inode);
  message* pushed_msg = kmalloc( sizeof( message), GFP_KERNEL);//memory allocation
  pushed_msg->payload = kmalloc( len*sizeof(char), GFP_KERNEL);

  if (pushed_msg == NULL || pushed_msg->payload == NULL){ //error in allocation check
    printk(KERN_INFO"%s: Error while allocating memory for pushing a new message for the entry %d" , MODNAME, MINOR_CURRENT);
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
  printk(KERN_INFO"%s: message pushed ",MODNAME);
  return SUCCESS;
}



static void pop_message(slot_elem* elem, char* out_buff){

  message* head_aux;
  copy_to_user(out_buff, elem->head->payload, elem->head->size); //put the message into the buffer (to,from.len)
  head_aux = elem->head;
  elem->head = elem->head->next; //pop the readed message
  kfree(head_aux->payload); //release the message head memory
  kfree(head_aux);

}



static ssize_t lms_write(struct file *filp, const char *buff, size_t len, loff_t *off){

  list_elem *aux;
  int ret;
  const int MINOR_CURRENT = iminor(filp->f_path.dentry->d_inode);
  DECLARE_WAIT_QUEUE_HEAD(the_queue);//here we use a private queue - wakeup is selective via wake_up_process
  list_elem me;
  me.next = NULL;
  me.prev = NULL;
  me.task = current;
	me.awake = NO;
	me.already_hit = NO;

  if ( len > mailslots[MINOR_CURRENT]->curr_size  || len <= 0 ){
    printk(KERN_INFO"%s: lms_write error, len to write not compliant with the spec. \n " , MODNAME);
    return FAILURE;
  }
  if (DEBUG) printk(KERN_INFO"%s: freemem = %d \n " , MODNAME, mailslots[MINOR_CURRENT]->free_mem );

  //lock the mailslot elem
  spin_lock( &(mailslots[MINOR_CURRENT]->queue_lock) );

  while( mailslots[MINOR_CURRENT]->free_mem < len ){
    //not enough free space
    //if in blocking mode then wait else exit
    if ( DEBUG ) printk(KERN_INFO"%s: lms_write func in while\n" , MODNAME);
    if ( mailslots[MINOR_CURRENT]->blocking == NON_BLOCKING ){
      spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock) );
      return NOT_ENOUGH_SPACE_ERROR;
    }
    /*
    //the reader process has to put himself in the reader queue
    aux = mailslots[MINOR_CURRENT]->r_queue->head;

    */
    aux = mailslots[MINOR_CURRENT]->w_queue->head;
    // if the queue is empty initialize a new queue : head and tail
    if ( aux == NULL ){
        mailslots[MINOR_CURRENT]->w_queue->head = &me;
        mailslots[MINOR_CURRENT]->w_queue->tail = &me;
    }
    else {
      aux = mailslots[MINOR_CURRENT]->w_queue->tail;
      //otherwise put it on the tail and update
      if ( aux->prev == NULL ){
        spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock) );
        printk(KERN_INFO"%s: malformed write queue, aborted", MODNAME);
        return FAILURE;
      }
      aux->next = &me ;
      me.prev = aux ;
      me.next = NULL;
      mailslots[MINOR_CURRENT]->w_queue->tail = &me;
    }

    //release the lock and wait for the event
    spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock)  );

    ret = wait_event_interruptible(the_queue, mailslots[MINOR_CURRENT]->free_mem >= len);
    if ( ret != 0 ){
      /*the function will return -ERESTARTSYS if it was interrupted by a signal and 0 if condition evaluated to true.*/
      printk(KERN_INFO"%s: The process %d has been awaken by a signal\n", MODNAME , current->pid);
      return FAILURE;
    }

    //now the writer has to delete himself from the w_queue
    spin_lock( &(mailslots[MINOR_CURRENT]->queue_lock)  );

    me.prev->next = me.next;
    me.next->prev = me.prev;
    //done
  }

  //once you know you can write your message because there is enough space
  //push the message to the message queue and decrease the slot capacity
  //but before check is the len policy is has changed by IOCTL
  if ( len > mailslots[MINOR_CURRENT]->curr_size  || len <= 0 ){
    printk(KERN_INFO"%s: lms_write error, len to write not compliant with the spec. \n " , MODNAME);
    return FAILURE;
  }
  //TODO number of byte to push is len or curr size?
  /*
  in case of curr size you have to add one more check:
  if ( len < mailslots[MINOR_CURRENT]->curr_size ) len = mailslots[MINOR_CURRENT]->curr_size;
  */
  push_message( mailslots[MINOR_CURRENT], buff , len, filp );
  if ( DEBUG ) printk(KERN_INFO "%s: updating free memory pre is %d\n", MODNAME, mailslots[MINOR_CURRENT]->free_mem );
  mailslots[MINOR_CURRENT]->free_mem -= len;
  if ( DEBUG ) printk(KERN_INFO "%s: free memory availabe is %d\n", MODNAME, mailslots[MINOR_CURRENT]->free_mem );

  //awake a reader process that is waiting
  if ( DEBUG ) printk( KERN_INFO "%s: awaking a reader process that is waiting \n" ,MODNAME);
  aux = mailslots[MINOR_CURRENT]->r_queue->head;
  if ( aux == NULL ){
    spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock) );
    if(DEBUG) printk( KERN_INFO "%s: write done, written %ld bytes no processes waiting for read ",MODNAME, len);
    return len;
  }
  while ( aux != NULL ){
    if ( aux->already_hit == NO ){
      aux->already_hit = YES ;
      aux->awake = YES ;
      wake_up_process(aux->task);
      break;
    aux= aux->next;
    }
  }
  //then release the lock and return the number of byte written
  spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock) );
  if(DEBUG) printk( KERN_INFO "%s: write done, written %ld bytes! \n ",MODNAME, len);
  return len;
}



static ssize_t lms_read(struct file *filp, char *buff, size_t len, loff_t *off){



  list_elem me;
  list_elem *aux;
  const int MINOR_CURRENT = iminor(filp->f_path.dentry->d_inode);
  int ret;
  DECLARE_WAIT_QUEUE_HEAD(the_queue);//here we use a private queue - wakeup is selective via wake_up_process
  me.next = NULL;
  me.prev = NULL;
  me.task = current;
	me.awake = NO;
	me.already_hit = NO;
  //check on len : has to be equal to the size of the message
  if (*off < 0) return FAILURE;


  if ( len <= 0  ){
    printk(KERN_INFO"%s: called a read with negative buffer len \n",MODNAME);
    return FAILURE;
  }
  else if (mailslots[MINOR_CURRENT]->head == NULL){
    if (DEBUG) printk(KERN_INFO "%s:no message in the mailslot, len assigned to default \n", MODNAME );
    len = INIT_MESSAGE_SIZE;
  }
  else if (len < mailslots[MINOR_CURRENT]->head->size  ){
    printk(KERN_INFO "%s: called a read with a len not compliant with the message size, the read hs to be all or nothing \n", MODNAME );
    return FAILURE;
  }
  else {
    len = mailslots[MINOR_CURRENT]->head->size;
  }

  if ( DEBUG ) printk(KERN_INFO"%s: valid lenght\n",MODNAME);

  spin_lock( &(mailslots[MINOR_CURRENT]->queue_lock) );
  //acquire the lock in order to read the message slot

  while( mailslots[MINOR_CURRENT]->head == NULL ){
    //no messages to read!
    if ( mailslots[MINOR_CURRENT]->blocking == NON_BLOCKING ){
      //quit
      printk(KERN_INFO"%s: No messages to read in this mailslot, exiting...\n",MODNAME);
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
      if ( aux->prev == NULL && aux != mailslots[MINOR_CURRENT]->r_queue->head ){
        //if there is just one process in the queue i can have aux->prev == NULL
        spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock) );
        printk(KERN_INFO"%s: malformed read queue, aborted", MODNAME);
        return FAILURE;
      }
      aux->next = &me ;
      me.prev = aux ;
      me.next = NULL;
      mailslots[MINOR_CURRENT]->r_queue->tail = &me;
    }
    spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock) );
    ret = wait_event_interruptible(the_queue, mailslots[MINOR_CURRENT]->head != NULL );

    if ( ret != 0 ){
      /*the function will return -ERESTARTSYS if it was interrupted by a signal and 0 if condition evaluated to true.*/

      printk(KERN_INFO"%s: The process %d has been awaken by a signal\n", MODNAME , current->pid);
      return FAILURE;
    }

    //once waked up the reader delete himself from the waitqueue , then the loop is over
    //and the process can pop a message from the queue in CS
    spin_lock( &(mailslots[MINOR_CURRENT])->queue_lock );
    if ( me.prev != NULL ) me.prev->next = me.next;
    if ( me.next != NULL ) me.next->prev = me.prev;
  }

  //check again the len to read after the lock releasing because can be changed
  if ( len < mailslots[MINOR_CURRENT]->head->size  ){
    spin_unlock( &(mailslots[MINOR_CURRENT])->queue_lock );
    printk(KERN_INFO "%s: called a read with a len not compliant with the message size, the read hs to be all or nothing ", MODNAME );
    return FAILURE;
  }
  else {
    len = mailslots[MINOR_CURRENT]->head->size;
  }
  //poping the message from the mailslot
  pop_message(mailslots[MINOR_CURRENT], buff);
  //now the reader has to signal to the writers waiting that there is a new slot ready
  //then is his duty to remove himself from the w_queue
  aux = mailslots[MINOR_CURRENT]->w_queue->head;
  while ( aux != NULL ){
    if ( aux->already_hit == NO ){
      aux->awake = YES;
      aux->already_hit = YES;
      wake_up_process(aux->task);
      break;
    }
    aux = aux->next;
  }
  spin_unlock( &(mailslots[MINOR_CURRENT])->queue_lock );
  if (DEBUG) printk(KERN_INFO "%s: read performed, read %ld bytes\n",MODNAME, len);
  return len;
}



static long lms_ioctl( struct file * filp, unsigned int param, unsigned long value){

  int status = SUCCESS ;
  const int MINOR_CURRENT = iminor(filp->f_path.dentry->d_inode);
  //since this function has not to be queued we try to get the lock and if is busy we quit otherwise we lock the mailslot
  if ( spin_trylock( &(mailslots[MINOR_CURRENT]->queue_lock) )  == 0 ){
    if ( mailslots[MINOR_CURRENT]->blocking == NON_BLOCKING ){
      printk(KERN_INFO"%s: trying to acquire a lock in a mailslot already locked. Error.",MODNAME);
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
        printk(KERN_INFO"%s: Error, change blocking mode parameter value not found!\n",MODNAME);
        status = FAILURE;
      }
      break ;

    case CHANGE_MESSAGE_SIZE:
      if ( value <= MAX_MESSAGE_SIZE && value > 0){
        mailslots[MINOR_CURRENT]->curr_size = value;
        status = SUCCESS;
      }
      else {
        printk(KERN_INFO"%s: Error, change slot size parameter value not compliant with the spec (MAX %d) ", MODNAME, MAX_MESSAGE_SIZE );
        status = FAILURE;
      }
      break;

    case GET_SLOT_SIZE:
      printk(KERN_INFO"%s: current slot size of entry with minor %d is %ld ", MODNAME, MINOR_CURRENT, mailslots[MINOR_CURRENT]->curr_size);
      break;

    default:
      printk(KERN_INFO"%s: command not found", MODNAME);
      break;
  }
  spin_unlock( &(mailslots[MINOR_CURRENT]->queue_lock) );
  return status;
}



static struct file_operations fops = {
  .owner = THIS_MODULE,
  .open =  lms_open,
  .write = lms_write,
  .read = lms_read,
  .unlocked_ioctl = lms_ioctl,
  .release = lms_release
};

int init_module(void) {
  //register the chardevice and store the result in major_number
  int i ;
  major_number = register_chrdev(0, DEVICE_NAME, &fops);
  if ( major_number < 0 ){
    printk(KERN_INFO"%s: cannot register a chardevice , failed ", MODNAME);
    return major_number;
  }
  //then initialize the all data structures
  for (i = 0 ; i < MAX_MINOR_NUM ; i++){
    mailslots[i] = kmalloc( sizeof( slot_elem ) , GFP_KERNEL);
    mailslots[i]->w_queue = kmalloc(sizeof(list) , GFP_KERNEL );
    mailslots[i]->r_queue = kmalloc(sizeof(list) , GFP_KERNEL );
    mailslots[i]->w_queue->head = NULL;
    mailslots[i]->w_queue->tail = NULL;
    mailslots[i]->r_queue->head = NULL;
    mailslots[i]->r_queue->tail = NULL;
    mailslots[i]->head = NULL;
    mailslots[i]->tail = NULL;
    mailslots[i]->free_mem = INIT_MESSAGE_SIZE*MAX_SLOT_SIZE; //so there are at most MAX_SLOT_SIZE slot for each specific mailslot
    mailslots[i]->curr_size = INIT_MESSAGE_SIZE;
    mailslots[i]->blocking = BLOCKING;
    spin_lock_init( &(mailslots[i]->queue_lock) );
  }
  printk(KERN_INFO "%s: Device registered, it is assigned major number %d\n", MODNAME, major_number);
	return SUCCESS;
}

void cleanup_module(void){
  //TODO cleanup memory usage
  int i;
  if ( major_number <= 0 ){
  		printk(KERN_INFO "%s: No device registered!\n", MODNAME);
  		return;
  }
  for (i = 0 ; i < MAX_MINOR_NUM ; i++){
    message* iterate = mailslots[i]->head;
    message* aux;
    while( iterate != NULL ){
      aux = iterate;
      iterate = iterate->next;
      kfree(aux->payload);
      kfree(aux);
    }
  }

  unregister_chrdev(major_number, DEVICE_NAME);
  printk(KERN_INFO "%s:Device unregistered!\n", MODNAME);
  return;
}
