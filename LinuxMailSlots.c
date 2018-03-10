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
#define MESSAGE_SIZE 256
#define SUCCESS 0
#define FAILURE -1
#define NO 0
#define YES 1

//mudule error codes
#define MSOPEN_ERROR -1
#define MSWRITE_ERROR -2
#define MSREAD_ERROR -3
#define MSPUSH_ERROR -4


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

typedef struct Slot_elem{
  list_elem* w_queue;
  list_elem* r_queue;
  message* head;
  message* tail;
  int free_mem;
  spinlock_t queue_lock;
  int blocking;
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

  if (sizeof(payload) > MAX_MESSAGE_SIZE ){ //max message size check
    printk("Error, payload message exceed maximum message size");
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
  copy_to_user(out_buff, elem->head->message->payload, elem->head->message->len); //put the message into the buffer (to,from.len)
  message* head_aux = elem->head;
  elem->head = elem->head->next; //pop the readed message
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

  //lock the mailslot elem
  spin_lock(mailslots[MINOR_CURRENT]->queue_lock);

  while( mailslots[MINOR_CURRENT]->free_mem < len ){
    //not enough free space

    //if in blocking mode --> wait
    //else exit
    /*
    if(blk_mode[calling_device]==NON_BLOCKING_MODE){
        spin_unlock(&lock[calling_device]);
        kfree(tmp);
        kfree(new->payload);
        kfree(new);
        return -EAGAIN;
    }
    */
    aux = mailslots[MINOR_CURRENT]->w_queue->tail;
    /*check on the regularity of the queue*/
    /*
    if(aux->prev == NULL){
        spin_unlock(&lock[calling_device]);
        printk("%s: malformed sleep-list - service damaged\n",MODNAME);
        kfree(tmp);
        kfree(new->payload);
        kfree(new);
        return -1;
    }
    */

    //then the process put himself in the tail of the writing queue
    aux->prev->next = &me;
    me.prev = aux->prev;
    aux->prev = &me;
    me.next = aux;

    spin_unlock(mailslots[MINOR_CURRENT]->queue_lock );

    int ret = wait_event_interruptible(the_queue, mailslots[MINOR_CURRENT]->free_mem >= len);
    /*insert return code checl*/

    //now the writer has to delete himself from the queue
    spin_lock(mailslots[MINOR_CURRENT]->queue_lock );

    me->prev->next = me->next;
    me->next->prev = me->prev;

  }
  //push the message to the message queue and decrease the slot capacity
  push_message(mailslots[MINOR_CURRENT], buff , len);
  mailslots[MINOR_CURRENT]->free_mem -= len;
  //awake a reader process that is waiting
  aux = mailslots[MINOR_CURRENT]->r_queue;
  //check on validity if the queue :::::::::
  while ( aux->next != NULL ){
    if ( aux->already_hit == NO ){
      aux->already_hit = YES ;
      aux->awake = YES ;
      wake_up_process(aux->task);
      break;
    }
    aux= aux->next;
  }
  spin_unlock(mailslots[MINOR_CURRENT]->queue_lock);
  return len;
}

static ssize_t lms_read(struct file *filp, const char *buff, size_t len, loff_t *off){

}






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
