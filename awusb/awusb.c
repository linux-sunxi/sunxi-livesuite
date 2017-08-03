/* -*- linux-c -*- */

/* 
 * Driver for AW USB which is for downloading firmware
 *
 * Cesc 
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 * Changelog:
 *           
 * */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/hardirq.h>     //<linux/smp_lock.h>
#include <linux/errno.h>
#include <linux/random.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/wait.h>

/* by Cesc */
#include <linux/ioctl.h>
#include <linux/mutex.h> //?

#include <linux/sched/signal.h>
/*
 * Version Information
 */
#define DRIVER_VERSION "v0.5"
#define DRIVER_AUTHOR "Jojo"
#define DRIVER_DESC "AW USB driver"

#define AW_MINOR	64

/* stall/wait timeout for AWUSB */
#define NAK_TIMEOUT (HZ)

/* Size of the AW buffer */
#define OBUF_SIZE 0x10000
#define IBUF_SIZE 0x500
#define MAX_TIME_WAIT 6000000

#define USB_AW_VENDOR_ID	0x1f3a
#define USB_AW_PRODUCT_ID	0xefe8

struct allwinner_usb {
	struct usb_device *aw_dev;     /* init: probe_aw */
	struct usb_interface *aw_intf;	//store interface to get endpoint
	unsigned int ifnum;             /* Interface number of the USB device */
	int isopen;                     /* nz if open */
	int present;                    /* Device is present on the bus */
	char *obuf, *ibuf;              /* transfer buffers */
	size_t	bulk_in_size;			/* the size of the receive buffer */
	unsigned char bulk_in_endpointAddr; /* Endpoint assignments */
	unsigned char bulk_out_endpointAddr; /* Endpoint assignments */
	wait_queue_head_t wait_q;       /* for timeouts */
	struct mutex lock;          /* general race avoidance */
};

/* by Cesc */
struct usb_param {
	unsigned		test_num;	
	unsigned		p1;    /* parameter 1 */
	unsigned		p2;
	unsigned		p3;
};

struct aw_command {
	int value;
	int length;
	void __user *buffer; //? by Cesc
};

#define AWUSB_IOC_MAGIC 's'

#define AWUSB_IOCRESET _IO(AWUSB_IOC_MAGIC, 0)
#define AWUSB_IOCSET   _IOW(AWUSB_IOC_MAGIC, 1, struct usb_param)
#define AWUSB_IOCGET   _IOR(AWUSB_IOC_MAGIC, 2, struct usb_param)
#define AWUSB_IOCSEND  _IOW(AWUSB_IOC_MAGIC, 3, struct aw_command)
#define AWUSB_IOCRECV  _IOR(AWUSB_IOC_MAGIC, 4, struct aw_command)
#define AWUSB_IOCSEND_RECV _IOWR(AWUSB_IOC_MAGIC, 5, struct aw_command) //how to implement it?

/* table of devices that work with this driver */
static const struct usb_device_id aw_table[] = {
	{ USB_DEVICE(USB_AW_VENDOR_ID, USB_AW_PRODUCT_ID) },
	{ }					/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, aw_table);

static int probe_aw(struct usb_interface *intf,
		const struct usb_device_id *id);
static void disconnect_aw(struct usb_interface *intf);

static struct usb_driver aw_driver = {
	.name =		"allwinner",
	.probe =	probe_aw,
	.disconnect =	disconnect_aw,
	.id_table =	aw_table,
};

static struct allwinner_usb aw_instance;

static int open_aw(struct inode *inode, struct file *file)
{
	struct allwinner_usb *aw = &aw_instance;
	struct usb_interface* intf;// = aw->aw_intf; 
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	int subminor;
	int i;

	if (aw->isopen || !aw->present) {
		mutex_unlock(&(aw->lock));
		return -EBUSY;
	}

	subminor = iminor(inode);
	intf = usb_find_interface(&aw_driver, subminor);;
	if(intf == NULL)
		return -ENODEV;

	iface_desc = intf->cur_altsetting;
	aw->bulk_in_endpointAddr = 0;
	aw->bulk_out_endpointAddr = 0;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;

		if ( !aw->bulk_in_endpointAddr && usb_endpoint_is_bulk_in(endpoint)) {
			/* we found a bulk in endpoint */
			aw->bulk_in_endpointAddr = endpoint->bEndpointAddress;
			printk(KERN_DEBUG"bulk_in_endpointAddr = %d\n", endpoint->bEndpointAddress);
		}

		if ( !aw->bulk_out_endpointAddr && usb_endpoint_is_bulk_out(endpoint)) {
			/* we found a bulk out endpoint */
			aw->bulk_out_endpointAddr = endpoint->bEndpointAddress;
			printk(KERN_DEBUG"bulk_out_endpointAddr = %d\n", endpoint->bEndpointAddress);
		}
	}

	if (!(aw->bulk_in_endpointAddr && aw->bulk_out_endpointAddr)) {
		printk(KERN_ERR"Could not find both bulk-in and bulk-out endpoints");
		return -EPIPE;
	}

	aw->isopen = 1;
	init_waitqueue_head(&aw->wait_q);

	return 0;
}

static int close_aw(struct inode *inode, struct file *file)
{
	struct allwinner_usb *aw = &aw_instance;

	aw->isopen = 0;

	return 0;
}

static long ioctl_aw(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct allwinner_usb *aw = &aw_instance;
	int retval=0;
	struct usb_param param_tmp;

	struct aw_command aw_cmd;
	void __user *data;
	int value = 0;
	int buffer_len = 0;

	int result = 0;
	int actual_len = 0;
	unsigned int nPipe = 0;

	mutex_lock(&(aw->lock));
	if (aw->present == 0 || aw->aw_dev == NULL) {
		retval = -ENODEV;
		goto err_out;
	}

	param_tmp.test_num = 0;
	param_tmp.p1 = 0;
	param_tmp.p2 = 0;
	param_tmp.p3 = 0;


	switch (cmd) {
		case AWUSB_IOCRESET:
			printk(KERN_DEBUG"ioctl_aw--AWUSB_IOCRESET\n");
			break;

		case AWUSB_IOCSET:    

			if (copy_from_user(&param_tmp, (void __user *)arg, sizeof(param_tmp)))
				retval= -EFAULT;

			printk(KERN_DEBUG"param_tmp.test_num = %d\n", param_tmp.test_num);
			printk(KERN_DEBUG"param_tmp.p1 = %d\n", param_tmp.p1);
			printk(KERN_DEBUG"param_tmp.p2 = %d\n", param_tmp.p2);
			printk(KERN_DEBUG"param_tmp.p3 = %d\n", param_tmp.p3);

			break;

		case AWUSB_IOCGET:

			param_tmp.test_num = 3;
			param_tmp.p1 = 4;
			param_tmp.p2 = 5;
			param_tmp.p3 = 6;

			if (copy_to_user((void __user *)arg, &param_tmp, sizeof(param_tmp)))
				retval= -EFAULT;

			break;

		case AWUSB_IOCSEND:

			data = (void __user *) arg;
			if (data == NULL)
				break;

			if (copy_from_user(&aw_cmd, data, sizeof(struct aw_command))) {
				retval = -EFAULT;
				goto err_out;
			}

			buffer_len = aw_cmd.length;
			value = aw_cmd.value;

			if (buffer_len > OBUF_SIZE) {
				retval = - EINVAL;
				goto err_out;
			}

			// stage 1, get data from app
			if (copy_from_user(aw->obuf, aw_cmd.buffer, aw_cmd.length)) {
				retval = -EFAULT;
				goto err_out;
			}

			//stage 2, send data to usb device
			nPipe = usb_sndbulkpipe(aw->aw_dev, aw->bulk_out_endpointAddr);
			result = usb_bulk_msg(aw->aw_dev, nPipe,
					aw->obuf, buffer_len, &actual_len, MAX_TIME_WAIT);  
			if (result) {
				printk(KERN_ERR"Write Whoops - %d", (int)result);
				printk(KERN_ERR"send pipe - %u", nPipe);
				retval = result;
				goto err_out;
			}

			break;

		case AWUSB_IOCRECV:

			data = (void __user *) arg;
			if (data == NULL)
				break;
			if (copy_from_user(&aw_cmd,data, sizeof(struct aw_command))) {
				retval = -EFAULT;
				goto err_out;
			}
			if (aw_cmd.length < 0) {
				retval = - EINVAL;
				goto err_out;
			}
			buffer_len = aw_cmd.length;
			value = aw_cmd.value;

			if (buffer_len > aw->bulk_in_size) {
				retval = - EINVAL;
				goto err_out;
			}

			memset(aw->ibuf, 0x00, aw->bulk_in_size);

			// stage 1, get data from usb device
			nPipe = usb_rcvbulkpipe(aw->aw_dev, aw->bulk_in_endpointAddr);
			result = usb_bulk_msg(aw->aw_dev, nPipe,
					aw->ibuf, buffer_len, &actual_len, MAX_TIME_WAIT);

			if (result) {
				printk(KERN_ERR"Read Whoops - %d", result);
				printk(KERN_ERR"Receive pipe - %u", nPipe);
				retval = result;
				goto err_out;
			}

			// stage 2, copy data to app in user space
			if (copy_to_user(aw_cmd.buffer, aw->ibuf, aw_cmd.length)) {
				retval = - EFAULT;
				goto err_out;
			}
			break;

		case AWUSB_IOCSEND_RECV:
			printk(KERN_DEBUG"ioctl_aw--AWUSB_IOCSEND_RECV\n");

			break;

		default:
			retval = -ENOTTY;
			break;
	}

err_out:
	mutex_unlock(&(aw->lock));
	return retval;

}

	static ssize_t
write_aw(struct file *file, const char __user *buffer,
		size_t count, loff_t * ppos)
{
	DEFINE_WAIT(wait);
	struct allwinner_usb *aw = &aw_instance;

	unsigned long copy_size;
	unsigned long bytes_written = 0;
	unsigned int partial;

	int result = 0;
	int maxretry;
	int errn = 0;
	int intr;

	intr = mutex_lock_interruptible(&(aw->lock));
	if (intr)
		return -EINTR;
	/* Sanity check to make sure aw is connected, powered, etc */
	if (aw->present == 0 || aw->aw_dev == NULL) {
		mutex_unlock(&(aw->lock));
		return -ENODEV;
	}



	do {
		unsigned long thistime;
		char *obuf = aw->obuf;

		thistime = copy_size =
			(count >= OBUF_SIZE) ? OBUF_SIZE : count;
		if (copy_from_user(aw->obuf, buffer, copy_size)) {
			errn = -EFAULT;
			goto error;
		}
		maxretry = 5;
		while (thistime) {
			if (!aw->aw_dev) {
				errn = -ENODEV;
				goto error;
			}
			if (signal_pending(current)) {
				mutex_unlock(&(aw->lock));
				return bytes_written ? bytes_written : -EINTR;
			}

			result = usb_bulk_msg(aw->aw_dev,
					usb_sndbulkpipe(aw->aw_dev, aw->bulk_out_endpointAddr),
					obuf, thistime, &partial, MAX_TIME_WAIT);

			printk(KERN_DEBUG"write stats: result:%d thistime:%lu partial:%u",
					result, thistime, partial);

			if (result == -ETIMEDOUT) {	/* NAK - so hold for a while */
				if (!maxretry--) {
					errn = -ETIME;
					goto error;
				}
				prepare_to_wait(&aw->wait_q, &wait, TASK_INTERRUPTIBLE);
				schedule_timeout(NAK_TIMEOUT);
				finish_wait(&aw->wait_q, &wait);
				continue;
			} else if (!result && partial) {
				obuf += partial;
				thistime -= partial;
			} else
				break;
		};
		if (result) {
			printk(KERN_ERR"Write Whoops - %x", result);
			errn = -EIO;
			goto error;
		}
		bytes_written += copy_size;
		count -= copy_size;
		buffer += copy_size;
	} while (count > 0);

	mutex_unlock(&(aw->lock));

	return bytes_written ? bytes_written : -EIO;

error:
	mutex_unlock(&(aw->lock));
	return errn;
}

static ssize_t
read_aw(struct file *file, char __user *buffer, size_t count, loff_t * ppos)
{
	DEFINE_WAIT(wait);
	struct allwinner_usb *aw = &aw_instance;
	ssize_t read_count;
	unsigned int partial;
	int this_read;
	int result;
	int maxretry = 10;
	char *ibuf;
	int intr;

	intr = mutex_lock_interruptible(&(aw->lock));
	if (intr)
		return -EINTR;
	/* Sanity check to make sure aw is connected, powered, etc */
	if (aw->present == 0 || aw->aw_dev == NULL) {
		mutex_unlock(&(aw->lock));
		return -ENODEV;
	}

	ibuf = aw->ibuf;

	read_count = 0;


	while (count > 0) {
		if (signal_pending(current)) {
			mutex_unlock(&(aw->lock));
			return read_count ? read_count : -EINTR;
		}
		if (!aw->aw_dev) {
			mutex_unlock(&(aw->lock));
			return -ENODEV;
		}
		this_read = (count >= aw->bulk_in_size) ? aw->bulk_in_size : count;

		result = usb_bulk_msg(aw->aw_dev,
				usb_rcvbulkpipe(aw->aw_dev, aw->bulk_in_endpointAddr),
				ibuf, this_read, &partial,
				MAX_TIME_WAIT);

		printk(KERN_DEBUG"read stats: result:%d this_read:%u partial:%u",
				result, this_read, partial);

		if (partial) {
			count = this_read = partial;
		} else if (result == -ETIMEDOUT || result == 15) {	/* FIXME: 15 ??? */
			if (!maxretry--) {
				mutex_unlock(&(aw->lock));
				printk(KERN_ERR"read_aw: maxretry timeout");
				return -ETIME;
			}
			prepare_to_wait(&aw->wait_q, &wait, TASK_INTERRUPTIBLE);
			schedule_timeout(NAK_TIMEOUT);
			finish_wait(&aw->wait_q, &wait);
			continue;
		} else if (result != -EREMOTEIO) {
			mutex_unlock(&(aw->lock));
			printk(KERN_ERR"Read Whoops - result:%u partial:%u this_read:%u",
					result, partial, this_read);
			return -EIO;
		} else {
			mutex_unlock(&(aw->lock));
			return (0);
		}

		if (this_read) {
			if (copy_to_user(buffer, ibuf, this_read)) {
				mutex_unlock(&(aw->lock));
				return -EFAULT;
			}
			count -= this_read;
			read_count += this_read;
			buffer += this_read;
		}
	}
	mutex_unlock(&(aw->lock));
	return read_count;
}

static const struct file_operations usb_aw_fops = {
	.owner =	THIS_MODULE,
	.read =		read_aw,
	.write =	write_aw,
	.unlocked_ioctl = ioctl_aw,
	.open =		open_aw,
	.release =	close_aw,
};

static struct usb_class_driver usb_aw_class = {
	.name =		"aw_efex%d",
	.fops =		&usb_aw_fops,
	.minor_base =	AW_MINOR,
};

static int probe_aw(struct usb_interface *intf,
		const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct allwinner_usb *aw = &aw_instance;
	int retval;
#if 0
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	size_t buffer_size;
	int i;

	iface_desc = intf->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (!aw->bulk_in_endpointAddr &&
				usb_endpoint_is_bulk_in(endpoint)) {
			/* we found a bulk in endpoint */
			buffer_size = le16_to_cpu(endpoint->wMaxPacketSize);
			aw->bulk_in_size = IBUF_SIZE;
			aw->bulk_in_endpointAddr = endpoint->bEndpointAddress;
			printk(KERN_DEBUG"bulk_in_endpointAddr = %d\n", endpoint->bEndpointAddress);
		}

		if (!aw->bulk_out_endpointAddr &&
				usb_endpoint_is_bulk_out(endpoint)) {
			/* we found a bulk out endpoint */
			aw->bulk_out_endpointAddr = endpoint->bEndpointAddress;
			printk(KERN_DEBUG"bulk_out_endpointAddr = %d\n", endpoint->bEndpointAddress);
		}
	}
	if (!(aw->bulk_in_endpointAddr && aw->bulk_out_endpointAddr)) {
		err("Could not find both bulk-in and bulk-out endpoints");
		return -EPIPE;
	}

#endif
	retval = usb_register_dev(intf, &usb_aw_class);
	if (retval) {
		printk(KERN_ERR"Not able to get a minor for this device.");
		return -ENOMEM;
	}

	aw->aw_dev = dev;
	aw->aw_intf = intf;
	aw->bulk_in_size = IBUF_SIZE;

	if (!(aw->obuf = kmalloc(OBUF_SIZE, GFP_KERNEL))) {
		printk(KERN_ERR"probe_aw: Not enough memory for the output buffer");
		usb_deregister_dev(intf, &usb_aw_class);
		return -ENOMEM;
	}

	if (!(aw->ibuf = kmalloc(aw->bulk_in_size, GFP_KERNEL))) {
		printk(KERN_ERR"probe_aw: Not enough memory for the input buffer");
		usb_deregister_dev(intf, &usb_aw_class);
		kfree(aw->obuf);
		return -ENOMEM;
	}

	mutex_init(&(aw->lock));

	usb_set_intfdata (intf, aw);
	aw->present = 1;

	return 0;
}

static void disconnect_aw(struct usb_interface *intf)
{
	struct allwinner_usb *aw = usb_get_intfdata (intf);

	usb_set_intfdata (intf, NULL);
	if (aw) {
		usb_deregister_dev(intf, &usb_aw_class);

		mutex_lock(&(aw->lock));
		if (aw->isopen) {
			aw->isopen = 0;
			/* better let it finish - the release will do whats needed */
			aw->aw_dev = NULL;
			mutex_unlock(&(aw->lock));
			return;
		}
		kfree(aw->ibuf);
		kfree(aw->obuf);

		dev_info(&intf->dev, "USB aw disconnected.\n");

		aw->present = 0;
		mutex_unlock(&(aw->lock));
	}
}

static int __init usb_aw_init(void)
{
	int retval;
	retval = usb_register(&aw_driver);
	if (retval)
		goto out;

	printk(KERN_INFO KBUILD_MODNAME ": " DRIVER_VERSION ":"
			DRIVER_DESC "\n");

out:
	return retval;
}


static void __exit usb_aw_cleanup(void)
{
	struct allwinner_usb *aw = &aw_instance;

	aw->present = 0;
	usb_deregister(&aw_driver);


}

module_init(usb_aw_init);
module_exit(usb_aw_cleanup);

MODULE_AUTHOR( DRIVER_AUTHOR );
MODULE_DESCRIPTION( DRIVER_DESC );
MODULE_LICENSE("GPL");

