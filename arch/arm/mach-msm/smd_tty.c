/* arch/arm/mach-msm/smd_tty.c
 *
 * Copyright (C) 2007 Google, Inc.
 * Author: Brian Swetland <swetland@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/wakelock.h>

#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>

#include <mach/msm_smd.h>

#define MAX_SMD_TTYS 32

static DEFINE_MUTEX(smd_tty_lock);

struct smd_tty_info {
	smd_channel_t *ch;
	struct tty_struct *tty;
	struct wake_lock wake_lock;
	int open_count;
	struct work_struct tty_work;
};

static struct smd_tty_info smd_tty[MAX_SMD_TTYS];
static struct workqueue_struct *smd_tty_wq;

static const struct smd_tty_channel_desc smd_default_tty_channels[] = {
        { .id = 0, .name = "SMD_DS" },
        { .id = 1, .name = "SMD_DIAG" },
#if defined(CONFIG_MACH_DESIREC)
        { .id = 9, .name = "SMD_DATA4" },
#else
        { .id = 9, .name = "SMD_DATA9" },
#endif
#ifdef CONFIG_BUILD_OMA_DM
        /* MASD requested OMA_DM AT-channel */
        { .id = 19, .name = "SMD_DATA3" },
#endif
#ifdef CONFIG_BUILD_CIQ
        /* CIQ Master/Slaver Bridge */
        { .id = 26, .name = "SMD_DATA20" },
#endif
        { .id = 27, .name = "SMD_GPSNMEA" },
};

static const struct smd_tty_channel_desc *smd_tty_channels =
		smd_default_tty_channels;
static int smd_tty_channels_len = ARRAY_SIZE(smd_default_tty_channels);

int smd_set_channel_list(const struct smd_tty_channel_desc *channels, int len)
{
	smd_tty_channels = channels;
	smd_tty_channels_len = len;
	return 0;
}

// static void smd_tty_notify(void *priv, unsigned event)
static void smd_tty_work_func(struct work_struct *work)
{
	unsigned char *ptr;
	int avail;
	struct smd_tty_info *info = container_of(work,
                                               struct smd_tty_info,
                                               tty_work);//priv;
	struct tty_struct *tty = info->tty;

	if (!tty)
		return;

//	if (event != SMD_EVENT_DATA)
//		return;

	mutex_lock(&smd_tty_lock);
	for (;;) {
		if (test_bit(TTY_THROTTLED, &tty->flags)) break;

		if (info->ch == 0) {
			printk(KERN_ERR "smd_tty_work_func: info->ch null\n");
                       break;
		}

		avail = smd_read_avail(info->ch);

		if (avail == 0) {
			tty->low_latency = 0;
			tty_flip_buffer_push(tty);
			break;
		}

		avail = tty_prepare_flip_string(tty, &ptr, avail);
		if (avail && ptr) {
			if (smd_read(info->ch, ptr, avail) != avail) {
				/* shouldn't be possible since we're in interrupt
				** context here and nobody else could 'steal' our
				** characters.
				*/
				printk(KERN_ERR "OOPS - smd_tty_buffer mismatch?!");
			}

			wake_lock_timeout(&info->wake_lock, HZ / 2);
			tty->low_latency = 1;
			tty_flip_buffer_push(tty);
		} else
			printk(KERN_ERR "smd_tty_work_func: tty_prepare_flip_string fail\n");
	}

	mutex_unlock(&smd_tty_lock);
	/* XXX only when writable and necessary */
	tty_wakeup(tty);
}

static void smd_tty_notify(void *priv, unsigned event)
{
       struct smd_tty_info *info = priv;

       if (event != SMD_EVENT_DATA)
               return;

       queue_work(smd_tty_wq, &info->tty_work);
}

static int smd_tty_open(struct tty_struct *tty, struct file *f)
{
	int res = 0;
	int n = tty->index;
	struct smd_tty_info *info;
	const char *name = NULL;
	int i;

	for (i = 0; i < smd_tty_channels_len; i++) {
		if (smd_tty_channels[i].id == n) {
			name = smd_tty_channels[i].name;
			break;
		}
	}
	if (!name)
		return -ENODEV;

	info = smd_tty + n;

	mutex_lock(&smd_tty_lock);
	wake_lock_init(&info->wake_lock, WAKE_LOCK_SUSPEND, name);
	tty->driver_data = info;

	if (info->open_count++ == 0) {
		info->tty = tty;
		if (info->ch) {
			smd_kick(info->ch);
		} else {
			res = smd_open(name, &info->ch, info, smd_tty_notify);
		}
	}
	mutex_unlock(&smd_tty_lock);

	return res;
}

static void smd_tty_close(struct tty_struct *tty, struct file *f)
{
	struct smd_tty_info *info = tty->driver_data;

	if (info == 0)
		return;

	mutex_lock(&smd_tty_lock);
	if (--info->open_count == 0) {
		info->tty = 0;
		tty->driver_data = 0;
		wake_lock_destroy(&info->wake_lock);
		if (info->ch) {
			smd_close(info->ch);
			info->ch = 0;
		}
	}
	mutex_unlock(&smd_tty_lock);
}

static int smd_tty_write(struct tty_struct *tty, const unsigned char *buf, int len)
{
	struct smd_tty_info *info = tty->driver_data;
	int avail;
	int ret;

	/* if we're writing to a packet channel we will
	** never be able to write more data than there
	** is currently space for
	*/
	mutex_lock(&smd_tty_lock);
	avail = smd_write_avail(info->ch);
	if (len > avail)
		len = avail;
	ret = smd_write(info->ch, buf, len);
	mutex_unlock(&smd_tty_lock);

	return ret;
}

static int smd_tty_write_room(struct tty_struct *tty)
{
	struct smd_tty_info *info = tty->driver_data;
	return smd_write_avail(info->ch);
}

static int smd_tty_chars_in_buffer(struct tty_struct *tty)
{
	struct smd_tty_info *info = tty->driver_data;
	return smd_read_avail(info->ch);
}

static void smd_tty_unthrottle(struct tty_struct *tty)
{
	struct smd_tty_info *info = tty->driver_data;
	queue_work(smd_tty_wq, &info->tty_work);
	return;
}

static struct tty_operations smd_tty_ops = {
	.open = smd_tty_open,
	.close = smd_tty_close,
	.write = smd_tty_write,
	.write_room = smd_tty_write_room,
	.chars_in_buffer = smd_tty_chars_in_buffer,
	.unthrottle = smd_tty_unthrottle,
};

static struct tty_driver *smd_tty_driver;

static int __init smd_tty_init(void)
{
	int ret, i;

	smd_tty_wq = create_singlethread_workqueue("smd_tty");
        if (smd_tty_wq == 0)
	        return -ENOMEM;

	smd_tty_driver = alloc_tty_driver(MAX_SMD_TTYS);
	if (smd_tty_driver == 0){
		destroy_workqueue(smd_tty_wq);
		return -ENOMEM;
	}	

	smd_tty_driver->owner = THIS_MODULE;
	smd_tty_driver->driver_name = "smd_tty_driver";
	smd_tty_driver->name = "smd";
	smd_tty_driver->major = 0;
	smd_tty_driver->minor_start = 0;
	smd_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	smd_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	smd_tty_driver->init_termios = tty_std_termios;
	smd_tty_driver->init_termios.c_iflag = 0;
	smd_tty_driver->init_termios.c_oflag = 0;
//	smd_tty_driver->init_termios.c_cflag = B38400 | CS8 | CREAD;
	smd_tty_driver->init_termios.c_cflag = B115200 | CS8 | CREAD;
	smd_tty_driver->init_termios.c_lflag = 0;
	smd_tty_driver->flags = TTY_DRIVER_RESET_TERMIOS |
		TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV;
	tty_set_operations(smd_tty_driver, &smd_tty_ops);

	ret = tty_register_driver(smd_tty_driver);
	if (ret) return ret;

	for (i = 0; i < smd_tty_channels_len; i++) {
		tty_register_device(smd_tty_driver, smd_tty_channels[i].id, 0);
		INIT_WORK(&smd_tty[smd_tty_channels[i].id].tty_work, smd_tty_work_func);
	}

	return 0;
}

module_init(smd_tty_init);
