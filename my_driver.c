//============================================================================
// author: charles <yinding.tsai@gmail.com>
//============================================================================

//============================================================================
//    includes
//============================================================================

#include <linux/module.h>
#include <linux/init.h>

#include <linux/fs.h>   //chrdev
#include <linux/cdev.h> //cdev_add() / cdev_del()
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/gfp.h>

#include <asm/uaccess.h> //copy_*_user()
#include <asm/io.h>

#include "my_ioctl.h"



//============================================================================
//    definations
//============================================================================

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Charles Tsai");
#define EN_DEBOUNCE
#define MAX_STR_SIZE 64
#define DEV_BUFSIZE    1024
#define NR_PORTS  3    /* use 3 ports by default */
//LED is connected to this GPIO
#define GPIO_INT  (25)
#if LINUX_VERSION_CODE <= KERNEL_VERSION (2, 6, 16)
#define my_access_ok(type, addr, size) \
    access_ok (type, addr, size)
#else
#define my_access_ok(type, addr, size) \
    access_ok (addr, size)
#endif
extern unsigned long volatile jiffies;
unsigned long old_jiffie = 0;



//============================================================================
//    types
//============================================================================



//============================================================================
//    functions
//============================================================================

//--- public method ----------------------------------------------------------
//--- private method ---------------------------------------------------------

/**
 * dev_open will be Called when user open the device file
 */
static int dev_open (
    struct inode* inode,
    struct file* filp
);


/**
 * dev_release will be Called when user close the device file
 */
static int dev_release (
    struct inode* inode,
    struct file* filp
);


/**
 * the callback to handle ioctl
 */
#if LINUX_VERSION_CODE <= KERNEL_VERSION (2, 6, 16)
static int my_ioctl (
    struct inode*,
    struct file*,
    unsigned int,
    unsigned long
);


#else
static long my_unlocked_ioctl (
    struct file* filp,
    unsigned int cmd,
    unsigned long args
);
#endif


/**
 * send messages to user
 */
static ssize_t dev_read (
    struct file* filp,
    char __user* buf,
    size_t count,
    loff_t* f_pos
);


/**
 * receive messages from user
 */
static ssize_t dev_write (
    struct file* filp,
    const char __user* buf,
    size_t count,
    loff_t* f_pos
);


/**
 * interrupt service routine
 */
irqreturn_t dev_isr (
    int irq,
    void* dev_id
);


/**
 *
 */
static int __init init_modules (void);


/**
 *
 */
static void __exit exit_modules (void);



//============================================================================
//    variables
//============================================================================

struct task_struct* ktrd;
static int dev_major;
static int dev_minor;
static dev_t dev;         // Global variable for the first device number
static struct cdev c_dev; // Global variable for the character device structure
struct class* cl;         // Global variable for the device class

// mainly io control between user and kernel
// args are pointed to user space's buffer
static int val = 0;
static int irq_no = -1;
static char str[MAX_STR_SIZE] = "hello from kernel!";
unsigned long short_buffer = 0;
unsigned long volatile short_head;
volatile unsigned long short_tail;
DECLARE_WAIT_QUEUE_HEAD (short_queue);

//Functions on the right side are the handler we just defined
struct file_operations dev_fops = {
    .owner          = THIS_MODULE,
    .open           = dev_open,
    .release        = dev_release,
#if LINUX_VERSION_CODE <= KERNEL_VERSION (2, 6, 16)
    .ioctl          = my_ioctl,
#else
    .unlocked_ioctl = my_unlocked_ioctl,
#endif
    .read           = dev_read,
    .write          = dev_write
};



//============================================================================
//    implementation
//============================================================================

//--- public method ----------------------------------------------------------
//--- private method ---------------------------------------------------------

/**
 * Atomicly increment an index into short_buffer
 */
static inline void short_incr_bp (
    volatile unsigned long *index,
    int delta
) {
	unsigned long new = *index + delta;
	barrier();  /* Don't optimize these two together */
	*index = (new >= (short_buffer + PAGE_SIZE)) ? short_buffer : new;
}


static int dev_open (
    struct inode* inode,
    struct file* filp
) {
    printk ("%s():\n", __FUNCTION__);
    return 0;
}


static int dev_release (
    struct inode* inode,
    struct file* filp
) {
    printk ("%s():\n", __FUNCTION__);
    return 0;
}


#if LINUX_VERSION_CODE <= KERNEL_VERSION (2, 6, 16)
static int my_ioctl (
    struct inode* inode,
    struct file* filp,
    unsigned int cmd,
    unsigned long args
) {
    int ret = 0;
#else
static long my_unlocked_ioctl (
    struct file* filp,
    unsigned int cmd,
    unsigned long args
) {
    long ret = 0;
#endif
    int tmp, err = 0;

    if (_IOC_TYPE (cmd) != IOC_MAGIC) {
        ret = -ENOTTY;
        goto fail;
    }

    if (_IOC_NR (cmd) > IOC_MAXNR) {
        ret = -ENOTTY;
        goto fail;
    }

    if (_IOC_DIR (cmd) & _IOC_READ) {
        err = !my_access_ok (VERIFY_WRITE, (void __user*) args, _IOC_SIZE (cmd));
    } else if (_IOC_DIR (cmd) & (_IOC_WRITE)) {
        err = !my_access_ok (VERIFY_READ, (void __user*) args, _IOC_SIZE (cmd));
    }
    if (err) {
        ret = -EFAULT;
        goto fail;
    }

    switch (cmd) {
    case SETNUM:
        //copy data from args(user) to val(kernel)
        if (copy_from_user (&val, (int __user*) args, sizeof (int))) {
            ret = -EFAULT;
            goto fail;
        }

        printk ("%s():get val from user = %d\n", __FUNCTION__, val);
        break;

    case GETNUM:
        //copy data from val to args
        if (copy_to_user ((int __user*) args, &val, sizeof (int))) {
            ret = -EFAULT;
            goto fail;
        }

        printk ("%s():set val to %d\n", __FUNCTION__, val);
        break;

    case XNUM:
        // exchange data passed by user
        tmp = val;
        if (copy_from_user (&val, (int __user*) args, sizeof (int))) {
            ret = -EFAULT;
            goto fail;
        }

        val *= 2; // multiply by 2

        if (copy_to_user ((int __user*) args, &val, sizeof (int))) {
            ret = -EFAULT;
            goto fail;
        }

        printk ("%s():change val from %d to %d\n", __FUNCTION__, tmp, val);
        break;

    default: /* redundant. as cmd was checked against MAXNR */
        ret = -ENOTTY;
        break;
    }

fail:

    return ret;
}


ssize_t dev_read (
    struct file* filp,
    char __user* buf,
    size_t count,
    loff_t* f_pos
) {
	DEFINE_WAIT(wait);
	unsigned long* value;
	int count0;

    printk ("%s():\n", __FUNCTION__);

	while (short_head == short_tail) {
		prepare_to_wait(&short_queue, &wait, TASK_INTERRUPTIBLE);
		if (short_head == short_tail)
			schedule();
		finish_wait(&short_queue, &wait);
		if (signal_pending (current))  /* a signal arrived */
			return -ERESTARTSYS; /* tell the fs layer to handle it */
	} 

	// get latest jiffies
	value = (unsigned long*) (short_tail - sizeof (unsigned long));

	sprintf (str, "%016lu\n", *value);
	if (strlen (str) < count) count = strlen (str);
	if (copy_to_user(buf, (char*) str, count)) {
		count = -EFAULT;
    }

	/* count0 is the number of readable data bytes */
	count0 = short_head - short_tail;
	if (count0 < 0) /* wrapped */
		count0 = short_buffer + PAGE_SIZE - short_tail;
	short_incr_bp (&short_tail, count0);

	return count;
}


ssize_t dev_write (
    struct file* filp,
    const char __user* buf,
    size_t count,
    loff_t* f_pos
) {
    ssize_t result = count;

    printk ("%s():\n", __FUNCTION__);

    if (count > MAX_STR_SIZE) {
        count = MAX_STR_SIZE;
    }

    if (copy_from_user (str, buf, count)) {
        return -EFAULT;
    }

    return result;
}


irqreturn_t dev_isr (
    int irq,
    void* dev_id
) {
    int written;
    static unsigned long flags = 0;

#ifdef EN_DEBOUNCE
    unsigned long diff = jiffies - old_jiffie;
    if (diff < 20) {
        return IRQ_HANDLED;
    }
    old_jiffie = jiffies;
#endif  

    local_irq_save (flags);

    /* Write a 16 byte record. Assume PAGE_SIZE is a multiple of 16 */
    written = sizeof (unsigned long);
    memcpy ((void*) short_head, (void*) &jiffies, written);
    short_incr_bp (&short_head, written);
    wake_up_interruptible (&short_queue); /* awake any reading process */

    local_irq_restore (flags);

    return IRQ_HANDLED;
}


int __init init_modules (void) {
    int ret;

    // get major number dynamically
    ret = alloc_chrdev_region (&dev, 0, 1, "mydev");
    if (ret < 0) {
        printk ("can't alloc chrdev\n");
        goto fail_alloc_region;
    }
    dev_major = MAJOR (dev);
    dev_minor = MINOR (dev);
    printk ("register chrdev(%d,%d)\n", dev_major, dev_minor);

    // create class
    if ((cl = class_create (THIS_MODULE, "myclass")) == NULL) {
        printk ("fail to call class_create()\n");
        goto fail_create_class;
    }

    // create device
    if (device_create (cl, NULL, dev, NULL, "mydev0") == NULL) {
        printk ("fail to call device_create()\n");
        goto fail_create_device;
    }

    // system call handler
    cdev_init (&c_dev, &dev_fops);
    c_dev.owner = THIS_MODULE;

    // register my device to kernel
    ret = cdev_add (&c_dev, dev, 1);
    if (ret < 0) {
        printk ("add chr c_dev failed\n");
        goto fail_add_device;
    }

    short_buffer = (unsigned long) kmalloc (PAGE_SIZE, GFP_KERNEL);
	short_head = short_tail = short_buffer;

    // request interrupt
    //Input GPIO configuratioin
    //Checking the GPIO is valid or not
    if(gpio_is_valid (GPIO_INT) == false){
        printk("GPIO %d is not valid\n", GPIO_INT);
        goto fail_alloc_mem;
    }

    //Requesting the GPIO
    if(gpio_request(GPIO_INT,"GPIO_INT") < 0){
        printk("ERROR: GPIO %d request\n", GPIO_INT);
        goto fail_alloc_mem;
    }

    //configure the GPIO as input
    gpio_direction_input(GPIO_INT);

    /*
     ** I have commented the below few lines, as gpio_set_debounce is not supported 
     ** in the Raspberry pi. So we are using EN_DEBOUNCE to handle this in this driver.
     */ 
#ifndef EN_DEBOUNCE
    //Debounce the button with a delay of 200ms
    if(gpio_set_debounce(GPIO_INT, 200) < 0){
        printk("ERROR: gpio_set_debounce - %d\n", GPIO_INT);
    }
#endif

    //Get the IRQ number for our GPIO
    irq_no = gpio_to_irq (GPIO_INT);
    printk ("GPIO_irqNumber = %d\n", irq_no);
  
    ret = request_irq (
        irq_no,               // IRQ number
        (void*) dev_isr,      // IRQ handler
        IRQF_TRIGGER_RISING,  // Handler will be called in raising edge
        "mydev0",             // used to identify the device name using this IRQ
        (void*) &dev          // should be the same while call free_irq()
    );
    if (ret) {
        printk (
            KERN_INFO "can't get assigned irq %i\n",
            irq_no
        );
        irq_no = -1;
	    goto fail_gpio_config; 
    }

    return 0;

fail_gpio_config :

    gpio_free (GPIO_INT);

fail_alloc_mem :

    kfree ((const void*) short_buffer);
    short_buffer = 0;

fail_add_device :

    device_destroy (cl, dev);

fail_create_device :

    class_destroy (cl);

fail_create_class :

    unregister_chrdev_region (dev, 1);

fail_alloc_region :


    return -ENODEV;
}


void __exit exit_modules (void) {
    if (irq_no > 0) {
        free_irq (irq_no, (void*) &dev);
    }
    cdev_del (&c_dev);
    device_destroy (cl, dev);
    class_destroy (cl);
    unregister_chrdev_region (dev, 1);
    if (short_buffer) {
        kfree ((const void*) short_buffer);
    }
    printk ("unregister chrdev\n");
}


module_init (init_modules);
module_exit (exit_modules);
