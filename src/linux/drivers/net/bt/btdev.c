// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 */

#include "btdev.h"

static struct bt_drv *bt_drv;

static int bt_seq_show(struct seq_file *m, void *v)
{
	struct bt_virnet *vnet = NULL;

	pr_devel("bt seq_show");
	seq_printf(m, "Total device: %d (bitmap: 0x%X) Ring size: %d\n",
		   bt_get_total_device(bt_drv), bt_drv->bitmap,
		   BT_RING_BUFFER_SIZE);

	list_for_each_entry(vnet, &bt_drv->devices_table->head, virnet_entry) {
		seq_printf(m, "dev: %12s, interface: %5s, state: %12s, MTU: %4d\n",
			   bt_virnet_get_cdev_name(vnet), bt_virnet_get_ndev_name(vnet),
			   bt_virnet_get_state_rep(vnet), vnet->ndev->mtu);
		seq_printf(m, "ring head: %4d, ring tail: %4d, packets num: %4d\n",
			   vnet->tx_ring->head, vnet->tx_ring->tail,
			   bt_virnet_get_ring_packets(vnet));
	}

	return OK;
}

static int bt_proc_open(struct inode *inode, struct file *file)
{
	pr_devel("bt proc_open");
	return single_open(file, bt_seq_show, PDE_DATA(inode));
}

static struct proc_ops bt_proc_fops = {
	.proc_open = bt_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release};

static int bt_io_file_open(struct inode *node, struct file *filp)
{
	struct bt_virnet *vnet = NULL;
	int ret = OK;

	pr_devel("bt io file open called");

	list_for_each_entry(vnet, &bt_drv->devices_table->head, virnet_entry) {
		if (bt_virnet_get_cdev(vnet) == node->i_cdev) {
			struct net_device *ndev;

			if ((filp->f_flags & O_ACCMODE) == O_RDONLY) {
				if (unlikely(!atomic_dec_and_test(&vnet->io_file
							 ->read_open_limit))) {
					atomic_inc(&vnet->io_file->read_open_limit);
					pr_err("file %s has been opened for read twice already",
					       bt_virnet_get_cdev_name(vnet));
					return -EBUSY;
				}
			} else if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
				if (unlikely(!atomic_dec_and_test(&vnet->io_file
							 ->write_open_limit))) {
					atomic_inc(&vnet->io_file->write_open_limit);
					pr_err("file %s has been opened for write twice already",
					       bt_virnet_get_cdev_name(vnet));
					return -EBUSY;
				}
			} else if ((filp->f_flags & O_ACCMODE) == O_RDWR) {
				if (unlikely(!atomic_dec_and_test(&vnet->io_file
							 ->read_open_limit))) {
					atomic_inc(&vnet->io_file->read_open_limit);
					pr_err("file %s has been opened for read twice already",
					       bt_virnet_get_cdev_name(vnet));
					return -EBUSY;
				}

				if (unlikely(!atomic_dec_and_test(&vnet->io_file
							 ->write_open_limit))) {
					atomic_inc(&vnet->io_file->write_open_limit);
					pr_err("file %s has been opened for write twice already",
					       bt_virnet_get_cdev_name(vnet));
					return -EBUSY;
				}
			}

			rtnl_lock();
			ndev = vnet->ndev;
			if (unlikely(!(ndev->flags & IFF_UP))) {
				ret = dev_change_flags(ndev, ndev->flags | IFF_UP, NULL);
				if (unlikely(ret < 0)) {
					rtnl_unlock();
					pr_err("bt dev_change_flags error: ret=%d", ret);
					return -EBUSY;
				}
			}
			rtnl_unlock();

			SET_STATE(vnet, BT_VIRNET_STATE_CONNECTED);
			filp->private_data = vnet;
			return OK;
		}
	}

	return -EIO;
}

static int bt_io_file_release(struct inode *node, struct file *filp)
{
	struct bt_virnet *vnet = filp->private_data;

	pr_devel("bt io file release called");

	if ((filp->f_flags & O_ACCMODE) == O_RDONLY) {
		atomic_inc(&vnet->io_file->read_open_limit);
	} else if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		atomic_inc(&vnet->io_file->write_open_limit);
	} else if ((filp->f_flags & O_ACCMODE) == O_RDWR) {
		atomic_inc(&vnet->io_file->read_open_limit);
		atomic_inc(&vnet->io_file->write_open_limit);
	}

	SET_STATE(vnet, BT_VIRNET_STATE_DISCONNECTED);

	return OK;
}

static ssize_t bt_io_file_read(struct file *filp,
			       char __user *buffer,
			       size_t size, loff_t *off)
{
	struct bt_virnet *vnet = filp->private_data;
	ssize_t out_sz;
	struct sk_buff *skb = NULL;

	pr_devel("bt io file read called");

	while (unlikely(bt_ring_is_empty(vnet->tx_ring))) {
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(vnet->rx_queue,
					     !bt_ring_is_empty(vnet->tx_ring)))
			return -ERESTARTSYS;
	}

	skb = bt_ring_current(vnet->tx_ring);
	out_sz = skb->len - MACADDR_LEN;
	if (unlikely(out_sz > size)) {
		pr_err("io file read: buffer too small: skb's len=%ld buffer's len=%ld",
		       (long)out_sz, (long)size);
		return -EINVAL;
	}

	bt_ring_consume(vnet->tx_ring);
	if (copy_to_user(buffer, skb->data + MACADDR_LEN, out_sz)) {
		pr_err("io file read: copy_to_user failed");
		return -EIO;
	}

	dev_kfree_skb(skb);
	skb = NULL;

	if (unlikely(netif_queue_stopped(vnet->ndev))) {
		pr_devel("consume data: wake the queue");
		netif_wake_queue(vnet->ndev);
	}

	return out_sz;
}

static ssize_t bt_io_file_write(struct file *filp,
				const char __user *buffer,
				size_t size, loff_t *off)
{
	struct bt_virnet *vnet = filp->private_data;
	struct sk_buff *skb = NULL;
	int ret;
	int len;
	ssize_t in_sz;

	pr_devel("bt io file write called: %lu bytes", size);
	in_sz = size + MACADDR_LEN;

	skb = netdev_alloc_skb(bt_virnet_get_ndev(vnet), in_sz + 2);
	if (unlikely(!skb))
		return -ENOMEM;

	skb_reserve(skb, 2);
	skb_put(skb, in_sz);

	memset(skb->data, 0, MACADDR_LEN);
	if (copy_from_user(skb->data + MACADDR_LEN, buffer, size))
		return -EIO;

	len = skb->len;
	skb->dev = bt_virnet_get_ndev(vnet);
	skb->protocol = eth_type_trans(skb, bt_virnet_get_ndev(vnet));
	ret = netif_rx_ni(skb);

	if (ret == NET_RX_SUCCESS) {
		vnet->ndev->stats.rx_packets++;
		vnet->ndev->stats.rx_bytes += len;
	} else {
		vnet->ndev->stats.rx_errors++;
		vnet->ndev->stats.rx_dropped++;
	}

	return size;
}

static int bt_virnet_change_mtu(struct net_device *dev, int mtu)
{
	pr_devel("bt virnet change mtu called");
	dev->mtu = mtu;
	return OK;
}

static int bt_set_mtu(struct net_device *dev, int mtu)
{
	int err = OK;

	pr_devel("bt set_mtu called");
	rtnl_lock();
	err = dev_set_mtu(dev, mtu);
	if (err < 0)
		pr_err("bt set_mtu failed to changed MTU to %d, err:%d", mtu, err);

	rtnl_unlock();

	return err;
}

static int bt_cmd_enable_virnet(struct bt_virnet *vnet, unsigned long arg)
{
	int ret;

	WARN_ON(!vnet);

	if (unlikely(vnet->state != BT_VIRNET_STATE_DISABLED)) {
		pr_err("bt enable can only be set at DISABLED state");
		return -EINVAL; // enable failed
	}

	rtnl_lock();
	ret = dev_change_flags(vnet->ndev, vnet->ndev->flags | IFF_UP, NULL);
	if (unlikely(ret < 0)) {
		rtnl_unlock();
		pr_err("bt cmd enable virnet: dev_change_flags error: ret=%d", ret);
		return -EIO;
	}
	rtnl_unlock();

	SET_STATE(vnet, BT_VIRNET_STATE_CONNECTED);
	return OK;
}

static int bt_cmd_disable_virnet(struct bt_virnet *vnet, unsigned long arg)
{
	int ret;

	WARN_ON(!vnet);
	if (unlikely(vnet->state != BT_VIRNET_STATE_CONNECTED)) {
		pr_err("bt disable can only be set at CONNECTED state");
		return -EINVAL;
	}

	rtnl_lock();
	ret = dev_change_flags(vnet->ndev, vnet->ndev->flags & ~IFF_UP, NULL);
	if (unlikely(ret < 0)) {
		rtnl_unlock();
		pr_err("bt cmd disable virnet: dev_change_flags error: ret=%d", ret);
		return -EIO;
	}
	rtnl_unlock();

	SET_STATE(vnet, BT_VIRNET_STATE_DISABLED);
	return OK;
}

static int bt_cmd_change_mtu(struct bt_virnet *vnet, unsigned long arg)
{
	int mtu;
	int ret;

	WARN_ON(!vnet);

	if (unlikely(get_user(mtu, (int __user *)arg))) {
		pr_err("get_user failed");
		return -EIO;
	}

	ret = bt_set_mtu(vnet->ndev, mtu);

	if (unlikely(ret < 0)) {
		pr_err("bt_dev_ioctl: changed mtu failed");
		return -EIO;
	}
	return OK;
}

static int bt_cmd_peek_packet(struct bt_virnet *vnet, unsigned long arg)
{
	struct sk_buff *skb = NULL;

	pr_devel("bt peek packet called");

	if (unlikely(bt_ring_is_empty(vnet->tx_ring))) {
		pr_err("bt peek packet ring is empty");
		return -EAGAIN;
	}

	skb = bt_ring_current(vnet->tx_ring);
	if (unlikely(put_user(skb->len - MACADDR_LEN, (int __user *)arg))) {
		pr_err("put_user failed");
		return -EIO;
	}

	return OK;
}

static long bt_io_file_ioctl(struct file *filep,
			     unsigned int cmd,
			     unsigned long arg)
{
	long ret;

	struct bt_virnet *vnet = filep->private_data;

	pr_devel("bt io file ioctl called");
	switch (cmd) {
	case BT_IOC_CHANGE_MTU:
		ret = bt_cmd_change_mtu(vnet, arg);
		break;
	case BT_IOC_ENABLE:
		ret = bt_cmd_enable_virnet(vnet, arg);
		break;
	case BT_IOC_DISABLE:
		ret = bt_cmd_disable_virnet(vnet, arg);
		break;
	case BT_IOC_PEEK_PACKET:
		ret = bt_cmd_peek_packet(vnet, arg);
		break;
	default:
		pr_err("not a valid cmd");
		return -ENOIOCTLCMD;
	}

	return ret;
}

static unsigned int bt_io_file_poll(struct file *filp, poll_table *wait)
{
	struct bt_virnet *vnet = filp->private_data;
	unsigned int mask = 0;

	poll_wait(filp, &vnet->rx_queue, wait);
	poll_wait(filp, &vnet->tx_queue, wait);

	if (!bt_ring_is_empty(vnet->tx_ring)) // readable
		mask |= POLLIN | POLLRDNORM;

	if (!bt_ring_is_full(vnet->tx_ring)) // writable
		mask |= POLLOUT | POLLWRNORM;

	return mask;
}

static const struct file_operations bt_io_file_ops = {
	.owner = THIS_MODULE,
	.open = bt_io_file_open,
	.release = bt_io_file_release,
	.read = bt_io_file_read,
	.write = bt_io_file_write,
	.poll = bt_io_file_poll,
	.unlocked_ioctl = bt_io_file_ioctl,
	.compat_ioctl = bt_io_file_ioctl};

static int bt_mng_file_open(struct inode *node, struct file *filp)
{
	pr_devel("bt mng file open called");

	if (unlikely(!atomic_dec_and_test(&bt_drv->mng_file->open_limit))) {
		atomic_inc(&bt_drv->mng_file->open_limit);
		pr_err("file %s has been opened already",
		       bt_drv->mng_file->bt_cdev->dev_filename);
		return -EBUSY;
	}
	filp->private_data = bt_drv;
	return OK;
}

static int bt_mng_file_release(struct inode *node, struct file *filp)
{
	struct bt_drv *drv = filp->private_data;

	pr_devel("bt mng file release called");

	atomic_inc(&drv->mng_file->open_limit);
	return OK;
}

static int bt_cmd_create_virnet(struct bt_drv *bt_mng, unsigned long arg)
{
	int id;
	int ret;
	struct bt_virnet *vnet = NULL;
	struct bt_uioc_args vp;
	unsigned long size;

	WARN_ON(!bt_mng);

	mutex_lock(&bt_mng->bitmap_lock);
	id = bt_get_unused_id(&bt_mng->bitmap);
	pr_devel("create io_file: get unused bit: %d", id);

	if (unlikely(bt_mng->devices_table->num == BT_VIRNET_MAX_NUM)) {
		pr_err("reach the limit of max virnets");
		mutex_unlock(&bt_mng->bitmap_lock);
		return -EIO;
	}
	vnet = bt_virnet_create(bt_mng, id);
	if (unlikely(!vnet)) {
		pr_err("bt virnet create failed");
		mutex_unlock(&bt_mng->bitmap_lock);
		return -EIO;
	}

	vnet->bt_table_head = bt_mng->devices_table;
	ret = bt_table_add_device(bt_mng->devices_table, vnet);
	if (unlikely(ret < 0)) {
		pr_err("bt table add device failed: ret=%d", ret);
		bt_virnet_destroy(vnet);
		mutex_unlock(&bt_mng->bitmap_lock);
		return -EIO; // failed to create
	}

	bt_set_bit(&bt_mng->bitmap, id);
	mutex_unlock(&bt_mng->bitmap_lock);

	memcpy(vp.ifa_name, bt_virnet_get_ndev_name(vnet),
	       sizeof(vp.ifa_name));
	memcpy(vp.cfile_name, bt_virnet_get_cdev_name(vnet),
	       sizeof(vp.cfile_name));

	mdelay(DELAY_100_MS);

	size = copy_to_user((void __user *)arg, &vp, sizeof(struct bt_uioc_args));
	if (unlikely(size)) {
		pr_err("copy_to_user failed: left size=%lu", size);
		return -EIO;
	}
	return OK;
}

static int bt_cmd_delete_virnet(struct bt_drv *bt_mng, unsigned long arg)
{
	int id;
	struct bt_virnet *vnet = NULL;
	struct bt_uioc_args vp;
	unsigned long size;

	WARN_ON(!bt_mng);

	size = copy_from_user(&vp, (void __user *)arg,
			      sizeof(struct bt_uioc_args));
	if (unlikely(size)) {
		pr_err("copy_from_user failed: left size=%lu", size);
		return -EIO;
	}

	vnet = bt_table_find(bt_mng->devices_table, vp.ifa_name);
	if (unlikely(!vnet)) {
		pr_err("virnet: %s cannot be found in bt table", vp.ifa_name);
		return -EIO; // not found
	}

	mutex_lock(&bt_mng->bitmap_lock);
	id = MINOR(bt_virnet_get_cdev_number(vnet));
	bt_table_remove_device(bt_mng->devices_table, vnet);
	bt_virnet_destroy(vnet);
	bt_clear_bit(&bt_mng->bitmap, id);
	mutex_unlock(&bt_mng->bitmap_lock);
	return OK;
}

static int bt_cmd_query_all_virnets(struct bt_drv *bt_mng, unsigned long arg)
{
	WARN_ON(!bt_mng);
	if (unlikely(put_user(bt_mng->bitmap, (u32 *)arg))) {
		pr_err("put_user failed");
		return -EIO;
	}
	return OK;
}

static int bt_cmd_delete_all_virnets(struct bt_drv *bt_mng, unsigned long arg)
{
	WARN_ON(!bt_mng);
	bt_table_delete_all(bt_mng);
	return OK;
}

static long bt_mng_file_ioctl(struct file *filep,
			      unsigned int cmd,
			      unsigned long arg)
{
	int ret;

	struct bt_drv *bt_mng = filep->private_data;

	pr_devel("bt mng file ioctl called");
	switch (cmd) {
	case BT_IOC_CREATE:
		ret = bt_cmd_create_virnet(bt_mng, arg);
		break;
	case BT_IOC_DELETE:
		ret = bt_cmd_delete_virnet(bt_mng, arg);
		break;
	case BT_IOC_QUERY_ALL:
		ret = bt_cmd_query_all_virnets(bt_mng, arg);
		break;
	case BT_IOC_DELETE_ALL:
		ret = bt_cmd_delete_all_virnets(bt_mng, arg);
		break;
	default:
		pr_err("not a valid command");
		return -ENOIOCTLCMD;
	}
	return ret;
}

static const struct file_operations bt_mng_file_ops = {
	.owner = THIS_MODULE,
	.open = bt_mng_file_open,
	.release = bt_mng_file_release,
	.unlocked_ioctl = bt_mng_file_ioctl,
	.compat_ioctl = bt_mng_file_ioctl};

static netdev_tx_t bt_virnet_xmit(struct sk_buff *skb,
				  struct net_device *dev)
{
	int ret;
	struct bt_virnet *vnet = NULL;
	int len = skb->len;

	pr_alert("alert: bt virnet_xmit: called");
	vnet = bt_table_find(bt_drv->devices_table, dev->name);
	WARN_ON(!vnet);

	ret = bt_virnet_produce_data(vnet, (void *)skb);

	if (unlikely(ret < 0)) {
		pr_devel("virnet xmit: produce data failed: ring is full, need to stop queue");
		netif_stop_queue(vnet->ndev);
		return NETDEV_TX_BUSY;
	}

	vnet->ndev->stats.tx_packets++;
	vnet->ndev->stats.tx_bytes += len;

	return NETDEV_TX_OK;
}

static const struct net_device_ops bt_virnet_ops = {
	.ndo_start_xmit = bt_virnet_xmit,
	.ndo_change_mtu = bt_virnet_change_mtu};

static struct bt_table *bt_table_init(void)
{
	struct bt_table *tbl = kmalloc(sizeof(*tbl), GFP_KERNEL);

	if (unlikely(!tbl)) {
		pr_err("alloc struct bt_table failed: oom");
		return NULL;
	}

	INIT_LIST_HEAD(&tbl->head);
	mutex_init(&tbl->tbl_lock);
	tbl->num = 0;
	return tbl;
}

static int bt_table_add_device(struct bt_table *tbl, struct bt_virnet *vn)
{
	struct bt_virnet *vnet = NULL;

	WARN_ON(!tbl);
	WARN_ON(!vn);

	vnet = bt_table_find(tbl, bt_virnet_get_ndev_name(vn));
	if (unlikely(vnet)) {
		pr_err("found duplicated device");
		return -ENOIOCTLCMD; // duplicated
	}

	mutex_lock(&tbl->tbl_lock);
	list_add_tail(&vn->virnet_entry, &tbl->head);
	++tbl->num;
	mutex_unlock(&tbl->tbl_lock);

	return OK;
}

static void bt_table_remove_device(struct bt_table *tbl, struct bt_virnet *vn)
{
	WARN_ON(!tbl);
	WARN_ON(!vn);
	mutex_lock(&tbl->tbl_lock);
	list_del(&vn->virnet_entry);
	--tbl->num;
	mutex_unlock(&tbl->tbl_lock);
}

static struct bt_virnet *bt_table_find(struct bt_table *tbl, const char *ifa_name)
{
	struct bt_virnet *vnet = NULL;

	WARN_ON(!tbl);

	if (unlikely(!ifa_name))
		return NULL;

	list_for_each_entry(vnet, &tbl->head, virnet_entry) {
		if (!strcmp(bt_virnet_get_ndev_name(vnet), ifa_name))
			return vnet;
	}
	return NULL;
}

static void __bt_table_delete_all(struct bt_drv *drv)
{
	u32 id;
	struct bt_virnet *vnet = NULL, *tmp_vnet = NULL;

	WARN_ON(!drv);
	list_for_each_entry_safe(vnet,
				 tmp_vnet,
				 &drv->devices_table->head,
				 virnet_entry) {
		id = MINOR(bt_virnet_get_cdev_number(vnet));
		list_del(&vnet->virnet_entry);
		bt_clear_bit(&drv->bitmap, id);
		bt_virnet_destroy(vnet);
	}
	drv->devices_table->num = 0;
}

static void bt_table_delete_all(struct bt_drv *bt_drv)
{
	WARN_ON(!bt_drv);
	mutex_lock(&bt_drv->bitmap_lock);
	mutex_lock(&bt_drv->devices_table->tbl_lock);

	__bt_table_delete_all(bt_drv);

	mutex_unlock(&bt_drv->devices_table->tbl_lock);
	mutex_unlock(&bt_drv->bitmap_lock);
}

static void bt_table_destroy(struct bt_drv *bt_drv)
{
	WARN_ON(!bt_drv);
	__bt_table_delete_all(bt_drv);
	kfree(bt_drv->devices_table);
	bt_drv->devices_table = NULL;
}

static struct bt_ring *__bt_ring_create(int size)
{
	struct bt_ring *ring = kmalloc(sizeof(*ring), GFP_KERNEL);

	if (unlikely(!ring)) {
		pr_err("ring create alloc failed: oom");
		return NULL;
	}

	if (unlikely(size < 0))
		return NULL;

	ring->head = 0;
	ring->tail = 0;
	ring->data = kmalloc_array(size, sizeof(void *), GFP_KERNEL);
	if (unlikely(!ring->data)) {
		pr_err("ring create alloc data failed: oom");
		kfree(ring);
		return NULL;
	}
	ring->size = size;

	return ring;
}

static struct bt_ring *bt_ring_create(void)
{
	return __bt_ring_create(BT_RING_BUFFER_SIZE);
}

static int bt_ring_is_empty(const struct bt_ring *ring)
{
	WARN_ON(!ring);
	return ring->head == ring->tail;
}

static int bt_ring_is_full(const struct bt_ring *ring)
{
	WARN_ON(!ring);
	return (ring->head + 1) % ring->size == ring->tail;
}

static void bt_ring_produce(struct bt_ring *ring, void *data)
{
	WARN_ON(!ring);
	WARN_ON(!data);

	smp_mb(); // Make sure the read and write order is correct
	ring->data[ring->head] = data;
	ring->head = (ring->head + 1) % ring->size;
	smp_wmb(); // Make sure the write order is correct
}

static void *bt_ring_current(struct bt_ring *ring)
{
	void *data = NULL;

	WARN_ON(!ring);
	data = ring->data[ring->tail];
	return data;
}

static void bt_ring_consume(struct bt_ring *ring)
{
	WARN_ON(!ring);

	smp_rmb(); // Make sure the read order is correct
	ring->tail = (ring->tail + 1) % ring->size;
	smp_mb(); // Make sure the read and write order is correct
}

static void bt_ring_destroy(struct bt_ring *ring)
{
	WARN_ON(!ring);
	kfree(ring->data);
	kfree(ring);
}

static int bt_virnet_produce_data(struct bt_virnet *dev, void *data)
{
	WARN_ON(!dev);
	WARN_ON(!data);
	if (unlikely(bt_ring_is_full(dev->tx_ring))) {
		pr_devel("ring is full");
		return -ENFILE;
	}

	smp_wmb(); // Make sure the write order is correct
	bt_ring_produce(dev->tx_ring, data);
	smp_wmb(); // Make sure twrite order is correct

	wake_up(&dev->rx_queue);
	return OK;
}

/**
 * register all the region
 */
static int bt_cdev_region_init(int major, int count)
{
	return register_chrdev_region(MKDEV(major, 0), count, "bt");
}

static struct class *bt_dev_class_create(void)
{
	struct class *cls = class_create(THIS_MODULE, "bt");

	if (IS_ERR(cls)) {
		pr_err("create struct class failed");
		return NULL;
	}
	return cls;
}

static void bt_dev_class_destroy(struct class *cls)
{
	WARN_ON(!cls);
	class_destroy(cls);
}

static int bt_cdev_device_create(struct bt_cdev *dev,
				 struct class *cls,
				 u32 id)
{
	struct device *device = NULL;
	dev_t devno = MKDEV(BT_DEV_MAJOR, id);

	WARN_ON(!dev);
	WARN_ON(!cls);

	pr_devel("bt cdev_device_create: id=%d", id);

	dev->bt_class = cls;

	device = device_create(cls, NULL, devno, NULL, "%s%u", BT_DEV_NAME_PREFIX, id);
	if (IS_ERR(device)) {
		pr_err("create device failed");
		return -EIO;
	}
	snprintf(dev->dev_filename, sizeof(dev->dev_filename), "%s%u", BT_DEV_PATH_PREFIX, id);
	return OK;
}

static void bt_cdev_device_destroy(struct bt_cdev *dev)
{
	WARN_ON(!dev);
	device_destroy(dev->bt_class, dev->cdev->dev);
}

static struct bt_cdev *bt_cdev_create(const struct file_operations *ops,
				      u32 id)
{
	int ret;
	int minor = id;
	struct bt_cdev *dev = NULL;
	struct cdev *chrdev = NULL;

	WARN_ON(!ops);

	pr_devel("bt cdev create called");

	dev = kmalloc(sizeof(*dev), GFP_KERNEL);
	if (unlikely(!dev)) {
		pr_err("bt cdev_create alloc failed: oom");
		goto err1;
	}

	chrdev = cdev_alloc();
	if (unlikely(!chrdev)) {
		pr_err("bt cdev_create: cdev_alloc() failed: oom");
		goto err2;
	}

	cdev_init(chrdev, ops);
	dev->cdev = chrdev;

	ret = cdev_add(chrdev, MKDEV(BT_DEV_MAJOR, minor), 1);
	if (unlikely(ret < 0)) {
		pr_err("cdev add failed");
		goto err3;
	}

	if (unlikely(bt_cdev_device_create(dev, bt_drv->bt_class, minor) < 0)) {
		pr_err("bt cdev_device_create failed");
		goto err3;
	}
	return dev;

err3:
	cdev_del(chrdev);

err2:
	kfree(dev);

err1:
	return NULL;
}

/**
 * delete one char device
 */
static void bt_cdev_delete(struct bt_cdev *bt_cdev)
{
	dev_t devno;

	WARN_ON(!bt_cdev);
	if (unlikely(bt_cdev)) {
		devno = bt_cdev->cdev->dev;

		unregister_chrdev(MAJOR(devno), bt_cdev->dev_filename + 5);
		bt_cdev_device_destroy(bt_cdev);

		cdev_del(bt_cdev->cdev);
	} else {
		pr_err("bt cdev_delete: cdev is null");
		return;
	}
}

/**
 * create and add data char device
 */
static struct bt_io_file *bt_create_io_file(u32 id)
{
	struct bt_io_file *file = kmalloc(sizeof(*file), GFP_KERNEL);

	if (unlikely(!file)) {
		pr_err("bt create_io_file alloc failed: oom");
		return NULL;
	}
	file->bt_cdev = bt_cdev_create(&bt_io_file_ops, id);
	if (unlikely(!file->bt_cdev)) {
		pr_err("bt create_io_file: create cdev failed");
		kfree(file);
		return NULL;
	}
	atomic_set(&file->read_open_limit, 1);
	atomic_set(&file->write_open_limit, 1);
	return file;
}

static struct bt_io_file **bt_create_io_files(void)
{
	int i;
	struct bt_io_file **all_files = kmalloc(BT_VIRNET_MAX_NUM * sizeof(struct bt_io_file *),
						GFP_KERNEL);

	if (unlikely(!all_files)) {
		pr_err("bt create_io_files alloc failed: oom");
		return NULL;
	}
	for (i = 0; i < BT_VIRNET_MAX_NUM; ++i)
		all_files[i] = bt_create_io_file(i + 1);

	return all_files;
}

static void bt_delete_io_file(struct bt_io_file *file)
{
	if (unlikely(!file))
		return;

	bt_cdev_delete(file->bt_cdev);
	kfree(file);
}

static void bt_delete_io_files(struct bt_drv *bt_mng)
{
	int i;

	for (i = 0; i < BT_VIRNET_MAX_NUM; ++i)
		bt_delete_io_file(bt_mng->io_files[i]);

	kfree(bt_mng->io_files);
	bt_mng->io_files = NULL;
}

/**
 * create and add management char device
 */
static struct bt_mng_file *bt_create_mng_file(int id)
{
	struct bt_mng_file *file = kmalloc(sizeof(*file), GFP_KERNEL);

	if (unlikely(!file)) {
		pr_err("bt create_mng_file: oom");
		return NULL;
	}

	file->bt_cdev = bt_cdev_create(&bt_mng_file_ops, id);
	if (unlikely(!file->bt_cdev)) {
		pr_err("bt create_mng_file: create cdev failed");
		kfree(file);
		return NULL;
	}

	atomic_set(&file->open_limit, 1);

	return file;
}

static void bt_delete_mng_file(struct bt_mng_file *file)
{
	if (unlikely(!file))
		return;

	bt_cdev_delete(file->bt_cdev);
	kfree(file);
}

/**
 * unregister the region
 */
static void bt_cdev_region_destroy(int major, int count)
{
	return unregister_chrdev_region(MKDEV(major, 0), count);
}

/**
 * create one net device
 */
static struct net_device *bt_net_device_create(u32 id)
{
	struct net_device *ndev = NULL;
	int err;
	char ifa_name[IFNAMSIZ];

	snprintf(ifa_name, sizeof(ifa_name), "%s%d", BT_VIRNET_NAME_PREFIX, id);
	ndev = alloc_netdev(0, ifa_name, NET_NAME_UNKNOWN, ether_setup);
	if (unlikely(!ndev)) {
		pr_err("alloc_netdev failed");
		return NULL;
	}

	ndev->netdev_ops = &bt_virnet_ops;
	ndev->flags |= IFF_NOARP;
	ndev->flags &= ~IFF_BROADCAST & ~IFF_MULTICAST;
	ndev->min_mtu = 1;
	ndev->max_mtu = ETH_MAX_MTU;

	err = register_netdev(ndev);
	if (unlikely(err)) {
		pr_err("create net_device failed");
		free_netdev(ndev);
		return NULL;
	}

	return ndev;
}

/**
 * destroy one net device
 */
static void bt_net_device_destroy(struct net_device *dev)
{
	WARN_ON(!dev);
	unregister_netdev(dev);
	free_netdev(dev);
}

static struct bt_io_file *bt_get_io_file(struct bt_drv *drv, int id)
{
	WARN_ON(id < 1);
	WARN_ON(id > BT_VIRNET_MAX_NUM);
	return drv->io_files[id - 1];
}

/**
 * create an virtual net_device
 */
static struct bt_virnet *bt_virnet_create(struct bt_drv *bt_mng, u32 id)
{
	struct bt_virnet *vnet = kmalloc(sizeof(*vnet), GFP_KERNEL);

	if (unlikely(!vnet)) {
		pr_err("error: bt_virnet init failed");
		goto failure1;
	}

	vnet->tx_ring = bt_ring_create();
	if (unlikely(!vnet->tx_ring)) {
		pr_err("create ring failed");
		goto failure2;
	}

	vnet->ndev = bt_net_device_create(id);
	if (unlikely(!vnet->ndev)) {
		pr_err("create net device failed");
		goto failure3;
	}

	vnet->io_file = bt_get_io_file(bt_mng, id);
	if (unlikely(!vnet->io_file)) {
		pr_err("create cdev failed");
		goto failure4;
	}

	init_waitqueue_head(&vnet->rx_queue);
	init_waitqueue_head(&vnet->tx_queue);

	SET_STATE(vnet, BT_VIRNET_STATE_CREATED);
	return vnet;

failure4:
	bt_net_device_destroy(vnet->ndev);

failure3:
	bt_ring_destroy(vnet->tx_ring);

failure2:
	kfree(vnet);

failure1:
	return NULL;
}

static void bt_virnet_destroy(struct bt_virnet *vnet)
{
	WARN_ON(!vnet);
	bt_ring_destroy(vnet->tx_ring);
	bt_net_device_destroy(vnet->ndev);

	SET_STATE(vnet, BT_VIRNET_STATE_DELETED);

	kfree(vnet);
}

static void bt_module_release(void)
{
	bt_table_destroy(bt_drv);
	bt_delete_io_files(bt_drv);
	bt_delete_mng_file(bt_drv->mng_file);
	bt_dev_class_destroy(bt_drv->bt_class);
	bt_cdev_region_destroy(BT_DEV_MAJOR, BT_VIRNET_MAX_NUM);

	kfree(bt_drv);
	bt_drv = NULL;
	remove_proc_entry("bt_info_proc", NULL);
}

/**
 *  module init function
 */
static int __init bt_module_init(void)
{
	int mid;
	struct proc_dir_entry *entry = NULL;

	pr_devel("bt module_init called");
	bt_drv = kmalloc(sizeof(*bt_drv), GFP_KERNEL);
	if (unlikely(!bt_drv)) {
		pr_err("module init: alloc struct bt_drv failed: oom");
		goto failure1;
	}

	if (unlikely(bt_cdev_region_init(BT_DEV_MAJOR, BT_VIRNET_MAX_NUM) < 0)) {
		pr_err("bt_cdev_region_init: failed");
		goto failure2;
	}

	bt_drv->devices_table = bt_table_init();
	if (unlikely(!bt_drv->devices_table)) {
		pr_err("bt_table_init(): failed");
		goto failure2;
	}

	bt_drv->bt_class = bt_dev_class_create();
	if (IS_ERR(bt_drv->bt_class)) {
		pr_err("class create failed");
		goto failure3;
	}

	bt_drv->io_files = bt_create_io_files();

	mutex_init(&bt_drv->bitmap_lock);
	bt_drv->bitmap = 0;

	mutex_lock(&bt_drv->bitmap_lock);
	mid = bt_get_unused_id(&bt_drv->bitmap);
	pr_devel("create mng_file: get unused bit: %d", mid);

	bt_drv->mng_file = bt_create_mng_file(mid);
	if (unlikely(!bt_drv->mng_file)) {
		pr_err("bt_ctrl_cdev_init failed");
		mutex_unlock(&bt_drv->bitmap_lock);
		goto failure4;
	}
	bt_set_bit(&bt_drv->bitmap, mid);
	mutex_unlock(&bt_drv->bitmap_lock);

	entry = proc_create_data("bt_info_proc", 0, NULL, &bt_proc_fops, NULL);
	if (unlikely(!entry)) {
		pr_err("create proc data failed");
		goto failure5;
	}

	return OK;

failure5:
	bt_delete_mng_file(bt_drv->mng_file);

failure4:
	bt_dev_class_destroy(bt_drv->bt_class);

failure3:
	bt_table_destroy(bt_drv);

failure2:
	kfree(bt_drv);

failure1:
	return -1;
}

module_init(bt_module_init);
module_exit(bt_module_release);
MODULE_LICENSE("GPL");
