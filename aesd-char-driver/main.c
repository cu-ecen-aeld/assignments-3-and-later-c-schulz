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
#include <linux/slab.h> // krealloc
#include "aesdchar.h"
#include "aesd_ioctl.h"

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
    filp->private_data = NULL;
    // ---

    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    // ---
    // ISO C90 requires all declarations at one place in the beginning
    struct aesd_dev *dev;
    const struct aesd_buffer_entry *entry;
    size_t offset = 0;
    size_t read_count = 0;
    int bytes_not_copied = 0;
    // ---

    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

    // ---
    // handle read

    // get AESD device struct
    if (!(dev = filp->private_data))
        goto end_read;

    // lock circular buffer mutex
    if (mutex_lock_interruptible(&dev->mutex) != 0)     // rc = -EINTR if signal received while waiting, rc = 0 in case of success
        goto end_read;

    // fetch the data from the circular buffer
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &offset);

    // if any data exists, copy it into userspace
    if (entry) {
        // returned offset is < size of entry and >= 0, so we need to read at max the rest of the entry
        // however, count could be even smaller, so we need the minimum of those two
        read_count = min((entry->size - offset), count);

        // actually copy data into userspace
        bytes_not_copied = copy_to_user(/* to */buf, /* from */entry->buffptr + offset, /* number of bytes */read_count);

        // update the return value with the number of read bytes
        retval = (read_count - bytes_not_copied);

        // and increase the f_pos by that amount
        // assignment 9) "must set *fpos to *fpos + retcount where retcount is the number of bytes read"
        *f_pos += retval;
    }

    // unlock circular buffer mutex
    mutex_unlock(&dev->mutex);
end_read:
    // ---

    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    // ---
    // ISO C90 requires all declarations at one place in the beginning
    struct aesd_dev *dev;
    int bytes_not_copied = 0;
    const char* old_entry = NULL;
    // ---

    ssize_t retval = -ENOMEM;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    // ---
    // handle write

    // get AESD device struct
    if (!(dev = filp->private_data))
        goto end_write;

    // lock circular buffer mutex
    if (mutex_lock_interruptible(&dev->mutex) != 0) {   // rc = -EINTR if signal received while waiting, rc = 0 in case of success
        retval = -ERESTARTSYS;
        goto end_write;
    }

    // input needs to be buffered until a '\n' is received
    // we do this inside dev with an additional 'entry' object, where we append the newly written data

    // for appending data, first reallocate the entry with the new size (old size + buffer size)...
    if (!(dev->tmp.buffptr = krealloc(dev->tmp.buffptr, dev->tmp.size + count, GFP_KERNEL)))
        goto unlock_write;

    // ... and then copy the data from userspace into the newly allocated part of the entry object
    bytes_not_copied = copy_from_user(/* to */(void*)(dev->tmp.buffptr + dev->tmp.size), /* from */buf, /* number of bytes */count);

    // also update the size of the entry object
    dev->tmp.size += (count - bytes_not_copied);

    // update return value s.t. the number of written bytes is returned
    // (at least they were written to the entry object already)
    retval = (count - bytes_not_copied);

    // assignment 9) "must set *fpos to *fpos + retcount where retcount is the number of bytes written"
    *f_pos += retval;

    // if a '\n' is received, insert entry into circular buffer and clear the entry object
    // TODO: can we assume that '\n' occurs only at the end of buf / the entry object?
    if (dev->tmp.buffptr[dev->tmp.size -1] == '\n') {

        // insert entry into circular buffer
        old_entry = aesd_circular_buffer_add_entry(&dev->buffer, &dev->tmp);

        // clear entry object; don't free it, because circular buffer now holds the pointer
        dev->tmp.buffptr = NULL;
        dev->tmp.size = 0;

        // we might need to free the memory of the value we have overwritten
        if (old_entry)
            kfree(old_entry);
    }

unlock_write:
    // unlock circular buffer mutex
    mutex_unlock(&dev->mutex);
end_write:
    // ---

    return retval;
}

static loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    // ---
    // ISO C90 requires all declarations at one place in the beginning
    struct aesd_dev *dev;
    int device_size = 0;
    loff_t retval = 0;
    uint8_t index;
    struct aesd_buffer_entry *entryptr;

    // get AESD device struct
    if (!(dev = filp->private_data)) {
        retval = -EFAULT;
        goto end_llseek;
    }

    // lock circular buffer mutex
    if (mutex_lock_interruptible(&dev->mutex) != 0) {   // rc = -EINTR if signal received while waiting, rc = 0 in case of success
        retval = -ERESTARTSYS;
        goto end_llseek;
    }

    // calculate total size of circular buffer by accumulating the size of its entries
    AESD_CIRCULAR_BUFFER_FOREACH(entryptr, &dev->buffer, index) {
        if (entryptr->buffptr)  // otherwise, size should be zero, but let's be sure to only add the size if the value exists
            device_size += entryptr->size;
    };

    // unlock circular buffer mutex
    mutex_unlock(&dev->mutex);

    // let fixed_size_llseek set filp->f_pos
    retval = fixed_size_llseek(filp, offset, whence, device_size);

    // alternatively, set f_pos directly:
    //
    // SEEK_SET: new_pos = offset;
    // SEEK_CUR: new_pos += offset;
    // SEEK_END: new_pos = device_size + offset;
    // default:  return -EINVAL
    //
    // if (new_pos < 0 || new_pos >= device_size): return -EINVAL
    // filp->f_pos = new_pos;
    //
    // retval = new_pos;

end_llseek:
    return retval;
    // ---
}

static long aesd_adjust_file_offset(struct file *filp, unsigned int write_cmd, unsigned int write_cmd_offset)
{
    // ---
    // ISO C90 requires all declarations at one place in the beginning
    struct aesd_dev *dev;
    long retval = 0;    // function returns 0 in case of no error
    uint8_t offs;
    uint8_t max_offs;
    long new_pos = 0;
    int i = 0;

    // get AESD device struct
    if (!(dev = filp->private_data)) {
        retval = -EFAULT;
        goto end_adjust;
    }

    // lock circular buffer mutex
    if (mutex_lock_interruptible(&dev->mutex) != 0) {   // rc = -EINTR if signal received while waiting, rc = 0 in case of success
        retval = -ERESTARTSYS;
        goto end_adjust;
    }

    // calculate location in the buffer
    offs = (dev->buffer.out_offs + write_cmd) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    max_offs = (dev->buffer.in_offs - dev->buffer.out_offs + AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    // check if input values are out of bounds
    if ((offs >= max_offs) || (write_cmd_offset >= dev->buffer.entry[offs].size)) {
        retval = -EINVAL;
        goto unlock_adjust;
    }

    // calculate new f_pos:
    // 1. add size of <write_cmd> entries
    for (i = 0; i < write_cmd; i++)
        new_pos += dev->buffer.entry[(dev->buffer.out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED].size;

    // 2. add <write_cmd_offset>
    new_pos += write_cmd_offset;
    filp->f_pos = new_pos;

unlock_adjust:
    // unlock circular buffer mutex
    mutex_unlock(&dev->mutex);

end_adjust:
    return retval;
    // ---
}

static long aesd_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    // ---
    // ISO C90 requires all declarations at one place in the beginning
    long retval = 0;

    switch (cmd) {
        case AESDCHAR_IOCSEEKTO:
        {
            struct aesd_seekto seekto;
            if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)) != 0)
                retval = -EFAULT;
            else
                retval = aesd_adjust_file_offset(filp,seekto.write_cmd,seekto.write_cmd_offset);
            break;
        }
        default:
            retval = -EINVAL;
    }

    return retval;
    // ---
}

struct file_operations aesd_fops = {
    .owner =          THIS_MODULE,
    .read =           aesd_read,
    .write =          aesd_write,
    .open =           aesd_open,
    .release =        aesd_release,
    .llseek =         aesd_llseek,
    .unlocked_ioctl = aesd_unlocked_ioctl,
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

    // initialize input buffer as empty
    aesd_device.tmp.buffptr = NULL;
    aesd_device.tmp.size = 0;

    // ---

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    // ---
    // ISO C90 requires all declarations at one place in the beginning
    uint8_t index;
    struct aesd_buffer_entry *entryptr;
    // ---

    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    // ---
    // cleanup AESD specific positions here as necessary

    // cleanup the circular buffer of size 10
    AESD_CIRCULAR_BUFFER_FOREACH(entryptr, &aesd_device.buffer, index) {
        if (entryptr->buffptr)
            kfree(entryptr->buffptr);
    };

    // cleanup the input buffer
    if (aesd_device.tmp.buffptr)
        kfree(aesd_device.tmp.buffptr);
    aesd_device.tmp.size = 0;

    // ---

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
