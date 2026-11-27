/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * File: rknpu_debugger.c
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Modified by: DrDitto with Google AI assitance
 *
 * Target: RK3566 / RK3568 (Single-Core NPU) | DRM GEM + IOMMU | Kernel >= 7.3.0
 */

#include <linux/clk.h>
#include <linux/compiler.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/devfreq.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <asm/div64.h>

#include "rknpu_debugger.h"
#include "rknpu_drv.h"
#include "rknpu_reset.h"

#define RKNPU_DEBUGGER_ROOT_NAME "rknpu"

#if defined(CONFIG_ROCKCHIP_RKNPU_DEBUG_FS) || \
	defined(CONFIG_ROCKCHIP_RKNPU_PROC_FS)

static __always_inline struct rknpu_device *
rknpu_debugger_dev_from_node(const struct rknpu_debugger_node *node)
{
	if (unlikely(!node || !node->debugger))
		return NULL;

	return container_of(node->debugger, struct rknpu_device, debugger);
}

static int rknpu_copy_user_cmd(const char __user *ubuf, size_t len, char *buf,
			       size_t buf_sz)
{
	if (unlikely(!ubuf || !buf || buf_sz < 2 || len == 0 || len >= buf_sz))
		return -EINVAL;

	if (unlikely(copy_from_user(buf, ubuf, len)))
		return -EFAULT;

	buf[len] = '\0';
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	return 0;
}

static int rknpu_version_show(struct seq_file *m, void *data)
{
	(void)data;

	if (unlikely(!m))
		return -EINVAL;

	seq_printf(m, "%s: v%d.%d.%d\n", DRIVER_DESC, DRIVER_MAJOR,
		   DRIVER_MINOR, DRIVER_PATCHLEVEL);
	return 0;
}

static int rknpu_load_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node;
	struct rknpu_device *rknpu_dev;
	unsigned long flags;
	u64 total_busy_time;
	u64 div_value;
	u32 load;

	(void)data;

	if (unlikely(!m || !m->private))
		return -EINVAL;

	node = m->private;
	rknpu_dev = rknpu_debugger_dev_from_node(node);
	if (unlikely(!rknpu_dev))
		return -ENODEV;

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	total_busy_time = rknpu_dev->subcore_data.timer.total_busy_time;
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	div_value = max_t(u64, 1ULL, (u64)RKNPU_LOAD_INTERVAL / 100ULL);
	do_div(total_busy_time, div_value);
	load = total_busy_time > 100ULL ? 100U : (u32)total_busy_time;

	seq_printf(m, "NPU load: %2u%%\n", load);
	return 0;
}

static int rknpu_power_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node;
	struct rknpu_device *rknpu_dev;

	(void)data;

	if (unlikely(!m || !m->private))
		return -EINVAL;

	node = m->private;
	rknpu_dev = rknpu_debugger_dev_from_node(node);
	if (unlikely(!rknpu_dev))
		return -ENODEV;

	seq_puts(m, atomic_read(&rknpu_dev->power_refcount) > 0 ? "on\n" :
								     "off\n");
	return 0;
}

static ssize_t rknpu_power_set(struct file *file, const char __user *ubuf,
			       size_t len, loff_t *offp)
{
	struct seq_file *priv;
	struct rknpu_debugger_node *node;
	struct rknpu_device *rknpu_dev;
	char buf[16];
	int ret;

	(void)offp;

	if (unlikely(!file || !file->private_data))
		return -EINVAL;

	priv = file->private_data;
	node = priv->private;
	rknpu_dev = rknpu_debugger_dev_from_node(node);
	if (unlikely(!rknpu_dev))
		return -ENODEV;

	ret = rknpu_copy_user_cmd(ubuf, len, buf, sizeof(buf));
	if (unlikely(ret))
		return ret;

	if (strcmp(buf, "on") == 0) {
		atomic_inc(&rknpu_dev->cmdline_power_refcount);
		ret = rknpu_power_get(rknpu_dev);
		if (unlikely(ret)) {
			atomic_dec_if_positive(&rknpu_dev->cmdline_power_refcount);
			return ret;
		}
	} else if (strcmp(buf, "off") == 0) {
		mutex_lock(&rknpu_dev->power_lock);
		if (atomic_read(&rknpu_dev->power_refcount) > 0 &&
		    atomic_dec_if_positive(&rknpu_dev->cmdline_power_refcount) >= 0) {
			int extra = atomic_read(&rknpu_dev->cmdline_power_refcount);

			if (extra > 0)
				atomic_sub(extra, &rknpu_dev->power_refcount);
			atomic_set(&rknpu_dev->cmdline_power_refcount, 0);
			mutex_unlock(&rknpu_dev->power_lock);
			rknpu_power_put(rknpu_dev);
		} else {
			mutex_unlock(&rknpu_dev->power_lock);
		}
	} else {
		LOG_ERROR("invalid rknpu power param: %s\n", buf);
		return -EINVAL;
	}

	return (ssize_t)len;
}

static int rknpu_power_put_delay_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node;
	struct rknpu_device *rknpu_dev;

	(void)data;

	if (unlikely(!m || !m->private))
		return -EINVAL;

	node = m->private;
	rknpu_dev = rknpu_debugger_dev_from_node(node);
	if (unlikely(!rknpu_dev))
		return -ENODEV;

	seq_printf(m, "%lu\n", rknpu_dev->power_put_delay);
	return 0;
}

static ssize_t rknpu_power_put_delay_set(struct file *file,
					 const char __user *ubuf, size_t len,
					 loff_t *offp)
{
	struct seq_file *priv;
	struct rknpu_debugger_node *node;
	struct rknpu_device *rknpu_dev;
	char buf[16];
	unsigned long power_put_delay;
	int ret;

	(void)offp;

	if (unlikely(!file || !file->private_data))
		return -EINVAL;

	priv = file->private_data;
	node = priv->private;
	rknpu_dev = rknpu_debugger_dev_from_node(node);
	if (unlikely(!rknpu_dev))
		return -ENODEV;

	ret = rknpu_copy_user_cmd(ubuf, len, buf, sizeof(buf));
	if (unlikely(ret))
		return ret;

	if (unlikely(kstrtoul(buf, 10, &power_put_delay)))
		return -EINVAL;

	rknpu_dev->power_put_delay = power_put_delay;
	return (ssize_t)len;
}

static int rknpu_freq_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node;
	struct rknpu_device *rknpu_dev;
	unsigned long current_freq;
	int ret;

	(void)data;

	if (unlikely(!m || !m->private))
		return -EINVAL;

	node = m->private;
	rknpu_dev = rknpu_debugger_dev_from_node(node);
	if (unlikely(!rknpu_dev))
		return -ENODEV;

	if (unlikely(!rknpu_dev->clks || rknpu_dev->num_clks < 1 ||
		     !rknpu_dev->clks[0].clk))
		return -ENODEV;

	ret = rknpu_power_get(rknpu_dev);
	if (unlikely(ret))
		return ret;

	current_freq = clk_get_rate(rknpu_dev->clks[0].clk);
	rknpu_power_put(rknpu_dev);

	seq_printf(m, "%lu\n", current_freq);
	return 0;
}

#ifdef CONFIG_PM_DEVFREQ
static ssize_t rknpu_freq_set(struct file *file, const char __user *ubuf,
			      size_t len, loff_t *offp)
{
	struct seq_file *priv;
	struct rknpu_debugger_node *node;
	struct rknpu_device *rknpu_dev;
	unsigned long current_freq, freq;
	char buf[16];
	int ret;

	(void)offp;

	if (unlikely(!file || !file->private_data))
		return -EINVAL;

	priv = file->private_data;
	node = priv->private;
	rknpu_dev = rknpu_debugger_dev_from_node(node);
	if (unlikely(!rknpu_dev || !rknpu_dev->devfreq))
		return -ENODEV;

	if (unlikely(!rknpu_dev->clks || rknpu_dev->num_clks < 1 ||
		     !rknpu_dev->clks[0].clk))
		return -ENODEV;

	ret = rknpu_copy_user_cmd(ubuf, len, buf, sizeof(buf));
	if (unlikely(ret))
		return ret;

	if (unlikely(kstrtoul(buf, 10, &freq)))
		return -EINVAL;

	ret = rknpu_power_get(rknpu_dev);
	if (unlikely(ret))
		return ret;

	current_freq = clk_get_rate(rknpu_dev->clks[0].clk);
	if (freq != current_freq) {
		rknpu_dev->ondemand_freq = freq;
		mutex_lock(&rknpu_dev->devfreq->lock);
		update_devfreq(rknpu_dev->devfreq);
		mutex_unlock(&rknpu_dev->devfreq->lock);
	}
	rknpu_power_put(rknpu_dev);

	return (ssize_t)len;
}
#else
static ssize_t rknpu_freq_set(struct file *file, const char __user *ubuf,
			      size_t len, loff_t *offp)
{
	(void)file;
	(void)ubuf;
	(void)len;
	(void)offp;
	return -EOPNOTSUPP;
}
#endif

static int rknpu_volt_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node;
	struct rknpu_device *rknpu_dev;
	unsigned long current_volt;

	(void)data;

	if (unlikely(!m || !m->private))
		return -EINVAL;

	node = m->private;
	rknpu_dev = rknpu_debugger_dev_from_node(node);
	if (unlikely(!rknpu_dev))
		return -ENODEV;

	current_volt = rknpu_dev->vdd ?
			       (unsigned long)regulator_get_voltage(rknpu_dev->vdd) :
			       0UL;

	seq_printf(m, "%lu\n", current_volt);
	return 0;
}

static int rknpu_reset_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node;
	struct rknpu_device *rknpu_dev;

	(void)data;

	if (unlikely(!m || !m->private))
		return -EINVAL;

	node = m->private;
	rknpu_dev = rknpu_debugger_dev_from_node(node);
	if (unlikely(!rknpu_dev))
		return -ENODEV;

	seq_puts(m, !rknpu_dev->bypass_soft_reset ? "on\n" : "off\n");
	return 0;
}

static ssize_t rknpu_reset_set(struct file *file, const char __user *ubuf,
			       size_t len, loff_t *offp)
{
	struct seq_file *priv;
	struct rknpu_debugger_node *node;
	struct rknpu_device *rknpu_dev;
	char buf[16];
	int ret;

	(void)offp;

	if (unlikely(!file || !file->private_data))
		return -EINVAL;

	priv = file->private_data;
	node = priv->private;
	rknpu_dev = rknpu_debugger_dev_from_node(node);
	if (unlikely(!rknpu_dev))
		return -ENODEV;

	ret = rknpu_copy_user_cmd(ubuf, len, buf, sizeof(buf));
	if (unlikely(ret))
		return ret;

	if (strcmp(buf, "1") == 0 && atomic_read(&rknpu_dev->power_refcount) > 0)
		rknpu_soft_reset(rknpu_dev);
	else if (strcmp(buf, "on") == 0)
		rknpu_dev->bypass_soft_reset = false;
	else if (strcmp(buf, "off") == 0)
		rknpu_dev->bypass_soft_reset = true;
	else
		return -EINVAL;

	return (ssize_t)len;
}

static const struct rknpu_debugger_list rknpu_debugger_root_list[] = {
	{ "version", rknpu_version_show, NULL, NULL },
	{ "load", rknpu_load_show, NULL, NULL },
	{ "power", rknpu_power_show, rknpu_power_set, NULL },
	{ "freq", rknpu_freq_show, rknpu_freq_set, NULL },
	{ "volt", rknpu_volt_show, NULL, NULL },
	{ "delayms", rknpu_power_put_delay_show, rknpu_power_put_delay_set,
	  NULL },
	{ "reset", rknpu_reset_show, rknpu_reset_set, NULL },
};

static ssize_t rknpu_debugger_write(struct file *file, const char __user *ubuf,
				    size_t len, loff_t *offp)
{
	struct seq_file *priv;
	struct rknpu_debugger_node *node;

	if (unlikely(!file || !file->private_data))
		return -EINVAL;

	priv = file->private_data;
	node = priv->private;
	if (unlikely(!node || !node->info_ent))
		return -EINVAL;

	return node->info_ent->write ?
		       node->info_ent->write(file, ubuf, len, offp) :
		       (ssize_t)len;
}

static int rknpu_debugfs_open(struct inode *inode, struct file *file)
{
	struct rknpu_debugger_node *node;

	if (unlikely(!inode || !inode->i_private))
		return -EINVAL;

	node = inode->i_private;
	if (unlikely(!node->info_ent || !node->info_ent->show))
		return -EINVAL;

	return single_open(file, node->info_ent->show, node);
}

static const struct file_operations rknpu_debugfs_fops = {
	.owner = THIS_MODULE,
	.open = rknpu_debugfs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = rknpu_debugger_write,
};
#endif

#ifdef CONFIG_ROCKCHIP_RKNPU_DEBUG_FS
static int rknpu_debugfs_remove_files(struct rknpu_debugger *debugger)
{
	struct rknpu_debugger_node *pos, *q;
	struct dentry *dir;

	if (unlikely(!debugger))
		return -EINVAL;

	mutex_lock(&debugger->debugfs_lock);
	dir = debugger->debugfs_dir;
	debugger->debugfs_dir = NULL;
	if (dir && !IS_ERR(dir))
		debugfs_remove_recursive(dir);

	list_for_each_entry_safe(pos, q, &debugger->debugfs_entry_list, list) {
		list_del(&pos->list);
		pos->dent = NULL;
		kfree(pos);
	}
	mutex_unlock(&debugger->debugfs_lock);
	return 0;
}

static int rknpu_debugfs_create_files(const struct rknpu_debugger_list *files,
				      int count, struct dentry *root,
				      struct rknpu_debugger *debugger)
{
	int i;
	struct dentry *ent;
	struct rknpu_debugger_node *tmp;
	umode_t mode;

	if (unlikely(!files || count <= 0 || !root || !debugger))
		return -EINVAL;

	for (i = 0; i < count; i++) {
		tmp = kzalloc(sizeof(*tmp), GFP_KERNEL);
		if (unlikely(!tmp))
			goto err_alloc;

		tmp->info_ent = &files[i];
		tmp->debugger = debugger;
		mode = files[i].write ? 0644 : 0444;

		ent = debugfs_create_file(files[i].name, mode, root, tmp,
					  &rknpu_debugfs_fops);
		if (unlikely(IS_ERR_OR_NULL(ent))) {
			kfree(tmp);
			goto err_alloc;
		}

		tmp->dent = ent;
		mutex_lock(&debugger->debugfs_lock);
		list_add_tail(&tmp->list, &debugger->debugfs_entry_list);
		mutex_unlock(&debugger->debugfs_lock);
	}
	return 0;

err_alloc:
	rknpu_debugfs_remove_files(debugger);
	return -ENOMEM;
}

static int rknpu_debugfs_remove(struct rknpu_debugger *debugger)
{
	return rknpu_debugfs_remove_files(debugger);
}

static int rknpu_debugfs_init(struct rknpu_debugger *debugger)
{
	int ret;

	if (unlikely(!debugger))
		return -EINVAL;

	debugger->debugfs_dir =
		debugfs_create_dir(RKNPU_DEBUGGER_ROOT_NAME, NULL);
	if (unlikely(IS_ERR_OR_NULL(debugger->debugfs_dir))) {
		debugger->debugfs_dir = NULL;
		return -EIO;
	}

	ret = rknpu_debugfs_create_files(rknpu_debugger_root_list,
					 ARRAY_SIZE(rknpu_debugger_root_list),
					 debugger->debugfs_dir, debugger);
	if (unlikely(ret)) {
		rknpu_debugfs_remove_files(debugger);
		return ret;
	}
	return 0;
}
#endif

#ifdef CONFIG_ROCKCHIP_RKNPU_PROC_FS
static int rknpu_procfs_open(struct inode *inode, struct file *file)
{
	struct rknpu_debugger_node *node;

	if (unlikely(!inode || !pde_data(inode)))
		return -EINVAL;

	node = pde_data(inode);
	if (unlikely(!node->info_ent || !node->info_ent->show))
		return -EINVAL;

	return single_open(file, node->info_ent->show, node);
}

static const struct proc_ops rknpu_procfs_fops = {
	.proc_open = rknpu_procfs_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = rknpu_debugger_write,
};

static int rknpu_procfs_remove_files(struct rknpu_debugger *debugger)
{
	struct rknpu_debugger_node *pos, *q;
	struct proc_dir_entry *dir;

	if (unlikely(!debugger))
		return -EINVAL;

	mutex_lock(&debugger->procfs_lock);
	dir = debugger->procfs_dir;
	debugger->procfs_dir = NULL;
	if (dir)
		proc_remove(dir);

	list_for_each_entry_safe(pos, q, &debugger->procfs_entry_list, list) {
		list_del(&pos->list);
		pos->pent = NULL;
		kfree(pos);
	}
	mutex_unlock(&debugger->procfs_lock);
	return 0;
}

static int rknpu_procfs_create_files(const struct rknpu_debugger_list *files,
				     int count, struct proc_dir_entry *root,
				     struct rknpu_debugger *debugger)
{
	int i;
	struct proc_dir_entry *ent;
	struct rknpu_debugger_node *tmp;
	umode_t mode;

	if (unlikely(!files || count <= 0 || !root || !debugger))
		return -EINVAL;

	for (i = 0; i < count; i++) {
		tmp = kzalloc(sizeof(*tmp), GFP_KERNEL);
		if (unlikely(!tmp))
			goto err_alloc;

		tmp->info_ent = &files[i];
		tmp->debugger = debugger;
		mode = files[i].write ? 0644 : 0444;

		ent = proc_create_data(files[i].name, mode, root,
				       &rknpu_procfs_fops, tmp);
		if (unlikely(!ent)) {
			kfree(tmp);
			goto err_alloc;
		}

		tmp->pent = ent;
		mutex_lock(&debugger->procfs_lock);
		list_add_tail(&tmp->list, &debugger->procfs_entry_list);
		mutex_unlock(&debugger->procfs_lock);
	}
	return 0;

err_alloc:
	rknpu_procfs_remove_files(debugger);
	return -ENOMEM;
}

static int rknpu_procfs_remove(struct rknpu_debugger *debugger)
{
	return rknpu_procfs_remove_files(debugger);
}

static int rknpu_procfs_init(struct rknpu_debugger *debugger)
{
	int ret;

	if (unlikely(!debugger))
		return -EINVAL;

	debugger->procfs_dir = proc_mkdir(RKNPU_DEBUGGER_ROOT_NAME, NULL);
	if (unlikely(IS_ERR_OR_NULL(debugger->procfs_dir))) {
		debugger->procfs_dir = NULL;
		return -EIO;
	}

	ret = rknpu_procfs_create_files(rknpu_debugger_root_list,
					ARRAY_SIZE(rknpu_debugger_root_list),
					debugger->procfs_dir, debugger);
	if (unlikely(ret)) {
		rknpu_procfs_remove_files(debugger);
		return ret;
	}
	return 0;
}
#endif

int rknpu_debugger_init(struct rknpu_device *rknpu_dev)
{
	int ret = 0;

	if (unlikely(!rknpu_dev))
		return -EINVAL;

#ifdef CONFIG_ROCKCHIP_RKNPU_DEBUG_FS
	mutex_init(&rknpu_dev->debugger.debugfs_lock);
	INIT_LIST_HEAD(&rknpu_dev->debugger.debugfs_entry_list);
	ret = rknpu_debugfs_init(&rknpu_dev->debugger);
	if (unlikely(ret))
		return ret;
#endif
#ifdef CONFIG_ROCKCHIP_RKNPU_PROC_FS
	mutex_init(&rknpu_dev->debugger.procfs_lock);
	INIT_LIST_HEAD(&rknpu_dev->debugger.procfs_entry_list);
	ret = rknpu_procfs_init(&rknpu_dev->debugger);
	if (unlikely(ret)) {
#ifdef CONFIG_ROCKCHIP_RKNPU_DEBUG_FS
		rknpu_debugfs_remove(&rknpu_dev->debugger);
#endif
		return ret;
	}
#endif
	return 0;
}

int rknpu_debugger_remove(struct rknpu_device *rknpu_dev)
{
	if (unlikely(!rknpu_dev))
		return -EINVAL;

#ifdef CONFIG_ROCKCHIP_RKNPU_DEBUG_FS
	rknpu_debugfs_remove(&rknpu_dev->debugger);
#endif
#ifdef CONFIG_ROCKCHIP_RKNPU_PROC_FS
	rknpu_procfs_remove(&rknpu_dev->debugger);
#endif
	return 0;
}
