/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Cornelia Schulz");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");

    // ---
    // handle open
    filp->private_data = container_of(inode->i_cdev, struct aesd_dev, cdev);
    // ---

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");

    // ---
    // handle release
    // ---

    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

    // ---
    // handle read

    // get AESD buffer struct
    struct aesd_dev *dev = filp->private_data;
    if (!dev)
        goto end_read;

    // lock circular buffer mutex
    int rc = mutex_lock_interruptible(&dev->mutex);
    if (rc != 0) {              // rc = -EINTR if signal received while waiting, rc = 0 in case of success
        retval = -ERESTARTSYS;
        goto end_read;
    }

    // fetch the data from the circular buffer
    size_t offset = 0;
    struct aesd_buffer_entry *entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *fpos, &offset);

    // and copy it into userspace
    if (entry)
    {
        // returned offset is < size of entry and >= 0, so we need to read at max the rest of the entry
        // however, count could be even smaller, so we need the minimum of those two
        size_t read_count = min((entry->size - offset), count);

        // actually copy data into userspace
        int bytes_not_copied = copy_to_user(/* to */buf, /* from */entry->buffptr + offset, /* number of bytes */read_count);

        // update the return value with the number of read bytes
        retval = (read - bytes_not_copied);

        // and increase the f_pos by that amount
        *f_pos += (read - bytes_not_copied);
    }

unlock_write:
    // unlock circular buffer mutex
    mutex_unlock(&aesd_device.mutex);
end_write:
    // ---

    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    // ---
    // handle write

    // get AESD buffer struct
    struct aesd_dev *dev = filp->private_data;
    if (!dev)
        goto end_write;

    // lock circular buffer mutex
    int rc = mutex_lock_interruptible(&dev->mutex);
    if (rc != 0) {              // rc = -EINTR if signal received while waiting, rc = 0 in case of success
        retval = -ERESTARTSYS;
        goto end_write;
    }

    // input needs to be buffered until a '\n' is received
    // we do this inside dev with an additional 'entry' object, where we append the newly written data

    // for appending data, first reallocate the entry with the new size (old size + buffer size)...
    dev->tmp.buffptr = krealloc(dev->tmp.buffptr, dev->tmp.size + count, GFP_KERNEL);
    if (!dev->tmp.buffptr) {    // returns address
        retval = -ENOMEM;
        goto unlock_write;
    }

    // ... and then copy the data from userspace into the newly allocated part of the entry object
    int bytes_not_copied = copy_from_user(/* to */dev->tmp.buffptr + dev->tmp.size, /* from */buf, /* number of bytes */count);

    // also update the size of the entry object
    dev->tmp.size += (count - bytes_not_copied);

    // update return value s.t. the number of written bytes is returned
    // (at least they were written to the entry object already)
    retval = (count - bytes_not_copied);

    // if a '\n' is received, insert entry into circular buffer and clear the entry object
    // TODO: can we assume that '\n' occurs only at the end of buf / the entry object?
    if (dev->tmp.buffptr[dev->tmp.size -1] == '\n') {

        // insert entry into circular buffer
        const char* old_entry = aesd_circular_buffer_add_entry(&dev->buffer, &dev->tmp);

        // clear entry object; don't free it, because circular buffer now holds the pointer
        dev->tmp.buffptr = NULL;
        dev->tmp.size = 0;

        // we might need to free the memory of the value we have overwritten
        if (old_entry)
            kfree(old_entry);
    }

unlock_write:
    // unlock circular buffer mutex
    mutex_unlock(&aesd_device.mutex);
end_write:
    // ---

    return retval;
}
struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    // ---
    // initialize the AESD specific portion of the device

    // initialize the circular buffer of size 10
    aesd_circular_buffer_init(&aesd_device.buffer);

    // the input buffer and the mutex do not need explicit initialization
    // the input buffer will be reallocated in 'write'

    // ---

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    // ---
    // cleanup AESD specific positions here as necessary

    // cleanup the circular buffer of size 10
    uint8_t index;
    struct aesd_buffer_entry *entryptr;
    AESD_CIRCULAR_BUFFER_FOREACH(entryptr, &aesd_device.buffer, index) {
        if (entryptr->buffptr)
            kfree(entryptr->buffptr);
    };

    // cleanup the input buffer
    if (aesd_device.tmp.buffptr)
        kfree(aesd_device.tmp.buffptr);

    // ---

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
