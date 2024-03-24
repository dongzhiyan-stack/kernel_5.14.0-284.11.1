// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2014 Intel Corp.
 * Author: Jiang Liu <jiang.liu@linux.intel.com>
 *
 * This file is licensed under GPLv2.
 *
 * This file contains common code to support Message Signaled Interrupts for
 * PCI compatible and non PCI compatible devices.
 */
#include <linux/types.h>
#include <linux/device.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/msi.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/pci.h>

#include "internals.h"

/**
 * alloc_msi_entry - Allocate an initialized msi_desc
 * @dev:	Pointer to the device for which this is allocated
 * @nvec:	The number of vectors used in this entry
 * @affinity:	Optional pointer to an affinity mask array size of @nvec
 *
 * If @affinity is not %NULL then an affinity array[@nvec] is allocated
 * and the affinity masks and flags from @affinity are copied.
 *
 * Return: pointer to allocated &msi_desc on success or %NULL on failure
 */
struct msi_desc *alloc_msi_entry(struct device *dev, int nvec,
				 const struct irq_affinity_desc *affinity)
{
	struct msi_desc *desc;

	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return NULL;

	INIT_LIST_HEAD(&desc->list);
	desc->dev = dev;
	desc->nvec_used = nvec;
	if (affinity) {
		desc->affinity = kmemdup(affinity,
			nvec * sizeof(*desc->affinity), GFP_KERNEL);
		if (!desc->affinity) {
			kfree(desc);
			return NULL;
		}
	}

	return desc;
}

void free_msi_entry(struct msi_desc *entry)
{
	kfree(entry->affinity);
	kfree(entry);
}

void __get_cached_msi_msg(struct msi_desc *entry, struct msi_msg *msg)
{
	*msg = entry->msg;
}

void get_cached_msi_msg(unsigned int irq, struct msi_msg *msg)
{
	struct msi_desc *entry = irq_get_msi_desc(irq);

	__get_cached_msi_msg(entry, msg);
}
EXPORT_SYMBOL_GPL(get_cached_msi_msg);

#ifdef CONFIG_SYSFS
static ssize_t msi_mode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct msi_desc *entry;
	bool is_msix = false;
	unsigned long irq;
	int retval;

	retval = kstrtoul(attr->attr.name, 10, &irq);
	if (retval)
		return retval;

	entry = irq_get_msi_desc(irq);
	if (!entry)
		return -ENODEV;

	if (dev_is_pci(dev))
		is_msix = entry->pci.msi_attrib.is_msix;

	return sysfs_emit(buf, "%s\n", is_msix ? "msix" : "msi");
}

/**
 * msi_populate_sysfs - Populate msi_irqs sysfs entries for devices
 * @dev:	The device(PCI, platform etc) who will get sysfs entries
 *
 * Return attribute_group ** so that specific bus MSI can save it to
 * somewhere during initilizing msi irqs. If devices has no MSI irq,
 * return NULL; if it fails to populate sysfs, return ERR_PTR
 */
const struct attribute_group **msi_populate_sysfs(struct device *dev)
{
	const struct attribute_group **msi_irq_groups;
	struct attribute **msi_attrs, *msi_attr;
	struct device_attribute *msi_dev_attr;
	struct attribute_group *msi_irq_group;
	struct msi_desc *entry;
	int ret = -ENOMEM;
	int num_msi = 0;
	int count = 0;
	int i;

	/* Determine how many msi entries we have */
	for_each_msi_entry(entry, dev)
		num_msi += entry->nvec_used;
	if (!num_msi)
		return NULL;

	/* Dynamically create the MSI attributes for the device */
	msi_attrs = kcalloc(num_msi + 1, sizeof(void *), GFP_KERNEL);
	if (!msi_attrs)
		return ERR_PTR(-ENOMEM);

	for_each_msi_entry(entry, dev) {
		for (i = 0; i < entry->nvec_used; i++) {
			msi_dev_attr = kzalloc(sizeof(*msi_dev_attr), GFP_KERNEL);
			if (!msi_dev_attr)
				goto error_attrs;
			msi_attrs[count] = &msi_dev_attr->attr;

			sysfs_attr_init(&msi_dev_attr->attr);
			msi_dev_attr->attr.name = kasprintf(GFP_KERNEL, "%d",
							    entry->irq + i);
			if (!msi_dev_attr->attr.name)
				goto error_attrs;
			msi_dev_attr->attr.mode = 0444;
			msi_dev_attr->show = msi_mode_show;
			++count;
		}
	}

	msi_irq_group = kzalloc(sizeof(*msi_irq_group), GFP_KERNEL);
	if (!msi_irq_group)
		goto error_attrs;
	msi_irq_group->name = "msi_irqs";
	msi_irq_group->attrs = msi_attrs;

	msi_irq_groups = kcalloc(2, sizeof(void *), GFP_KERNEL);
	if (!msi_irq_groups)
		goto error_irq_group;
	msi_irq_groups[0] = msi_irq_group;

	ret = sysfs_create_groups(&dev->kobj, msi_irq_groups);
	if (ret)
		goto error_irq_groups;

	return msi_irq_groups;

error_irq_groups:
	kfree(msi_irq_groups);
error_irq_group:
	kfree(msi_irq_group);
error_attrs:
	count = 0;
	msi_attr = msi_attrs[count];
	while (msi_attr) {
		msi_dev_attr = container_of(msi_attr, struct device_attribute, attr);
		kfree(msi_attr->name);
		kfree(msi_dev_attr);
		++count;
		msi_attr = msi_attrs[count];
	}
	kfree(msi_attrs);
	return ERR_PTR(ret);
}

/**
 * msi_destroy_sysfs - Destroy msi_irqs sysfs entries for devices
 * @dev:		The device(PCI, platform etc) who will remove sysfs entries
 * @msi_irq_groups:	attribute_group for device msi_irqs entries
 */
void msi_destroy_sysfs(struct device *dev, const struct attribute_group **msi_irq_groups)
{
	struct device_attribute *dev_attr;
	struct attribute **msi_attrs;
	int count = 0;

	if (msi_irq_groups) {
		sysfs_remove_groups(&dev->kobj, msi_irq_groups);
		msi_attrs = msi_irq_groups[0]->attrs;
		while (msi_attrs[count]) {
			dev_attr = container_of(msi_attrs[count],
					struct device_attribute, attr);
			kfree(dev_attr->attr.name);
			kfree(dev_attr);
			++count;
		}
		kfree(msi_attrs);
		kfree(msi_irq_groups[0]);
		kfree(msi_irq_groups);
	}
}
#endif

#ifdef CONFIG_GENERIC_MSI_IRQ_DOMAIN
static inline void irq_chip_write_msi_msg(struct irq_data *data,
					  struct msi_msg *msg)
{
	data->chip->irq_write_msi_msg(data, msg);
}

static void msi_check_level(struct irq_domain *domain, struct msi_msg *msg)
{
	struct msi_domain_info *info = domain->host_data;

	/*
	 * If the MSI provider has messed with the second message and
	 * not advertized that it is level-capable, signal the breakage.
	 */
	WARN_ON(!((info->flags & MSI_FLAG_LEVEL_CAPABLE) &&
		  (info->chip->flags & IRQCHIP_SUPPORTS_LEVEL_MSI)) &&
		(msg[1].address_lo || msg[1].address_hi || msg[1].data));
}

/**
 * msi_domain_set_affinity - Generic affinity setter function for MSI domains
 * @irq_data:	The irq data associated to the interrupt
 * @mask:	The affinity mask to set
 * @force:	Flag to enforce setting (disable online checks)
 *
 * Intended to be used by MSI interrupt controllers which are
 * implemented with hierarchical domains.
 *
 * Return: IRQ_SET_MASK_* result code
 */
int msi_domain_set_affinity(struct irq_data *irq_data,
			    const struct cpumask *mask, bool force)
{
	struct irq_data *parent = irq_data->parent_data;
	struct msi_msg msg[2] = { [1] = { }, };
	int ret;

	ret = parent->chip->irq_set_affinity(parent, mask, force);
	if (ret >= 0 && ret != IRQ_SET_MASK_OK_DONE) {
		BUG_ON(irq_chip_compose_msi_msg(irq_data, msg));
		msi_check_level(irq_data->domain, msg);
		irq_chip_write_msi_msg(irq_data, msg);
	}

	return ret;
}

static int msi_domain_activate(struct irq_domain *domain,
			       struct irq_data *irq_data, bool early)
{
	struct msi_msg msg[2] = { [1] = { }, };

	BUG_ON(irq_chip_compose_msi_msg(irq_data, msg));
	msi_check_level(irq_data->domain, msg);
	irq_chip_write_msi_msg(irq_data, msg);
	return 0;
}

static void msi_domain_deactivate(struct irq_domain *domain,
				  struct irq_data *irq_data)
{
	struct msi_msg msg[2];

	memset(msg, 0, sizeof(msg));
	irq_chip_write_msi_msg(irq_data, msg);
}

static int msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
			    unsigned int nr_irqs, void *arg)
{
	struct msi_domain_info *info = domain->host_data;
	struct msi_domain_ops *ops = info->ops;
	irq_hw_number_t hwirq = ops->get_hwirq(info, arg);
	int i, ret;

	if (irq_find_mapping(domain, hwirq) > 0)
		return -EEXIST;

	if (domain->parent) {
		ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, arg);
		if (ret < 0)
			return ret;
	}

	for (i = 0; i < nr_irqs; i++) {
		ret = ops->msi_init(domain, info, virq + i, hwirq + i, arg);
		if (ret < 0) {
			if (ops->msi_free) {
				for (i--; i > 0; i--)
					ops->msi_free(domain, info, virq + i);
			}
			irq_domain_free_irqs_top(domain, virq, nr_irqs);
			return ret;
		}
	}

	return 0;
}

static void msi_domain_free(struct irq_domain *domain, unsigned int virq,
			    unsigned int nr_irqs)
{
	struct msi_domain_info *info = domain->host_data;
	int i;

	if (info->ops->msi_free) {
		for (i = 0; i < nr_irqs; i++)
			info->ops->msi_free(domain, info, virq + i);
	}
	irq_domain_free_irqs_top(domain, virq, nr_irqs);
}

static const struct irq_domain_ops msi_domain_ops = {
	.alloc		= msi_domain_alloc,
	.free		= msi_domain_free,
	.activate	= msi_domain_activate,
	.deactivate	= msi_domain_deactivate,
};

static irq_hw_number_t msi_domain_ops_get_hwirq(struct msi_domain_info *info,
						msi_alloc_info_t *arg)
{
	return arg->hwirq;
}

static int msi_domain_ops_prepare(struct irq_domain *domain, struct device *dev,
				  int nvec, msi_alloc_info_t *arg)
{
	memset(arg, 0, sizeof(*arg));
	return 0;
}

static void msi_domain_ops_set_desc(msi_alloc_info_t *arg,
				    struct msi_desc *desc)
{
	arg->desc = desc;
}

static int msi_domain_ops_init(struct irq_domain *domain,
			       struct msi_domain_info *info,
			       unsigned int virq, irq_hw_number_t hwirq,
			       msi_alloc_info_t *arg)
{
	irq_domain_set_hwirq_and_chip(domain, virq, hwirq, info->chip,
				      info->chip_data);
	if (info->handler && info->handler_name) {
		__irq_set_handler(virq, info->handler, 0, info->handler_name);
		if (info->handler_data)
			irq_set_handler_data(virq, info->handler_data);
	}
	return 0;
}

static int msi_domain_ops_check(struct irq_domain *domain,
				struct msi_domain_info *info,
				struct device *dev)
{
	return 0;
}

static struct msi_domain_ops msi_domain_ops_default = {
	.get_hwirq		= msi_domain_ops_get_hwirq,
	.msi_init		= msi_domain_ops_init,
	.msi_check		= msi_domain_ops_check,
	.msi_prepare		= msi_domain_ops_prepare,
	.set_desc		= msi_domain_ops_set_desc,
	.domain_alloc_irqs	= __msi_domain_alloc_irqs,
	.domain_free_irqs	= __msi_domain_free_irqs,
};

static void msi_domain_update_dom_ops(struct msi_domain_info *info)
{
	struct msi_domain_ops *ops = info->ops;

	if (ops == NULL) {
		info->ops = &msi_domain_ops_default;
		return;
	}

	if (ops->domain_alloc_irqs == NULL)
		ops->domain_alloc_irqs = msi_domain_ops_default.domain_alloc_irqs;
	if (ops->domain_free_irqs == NULL)
		ops->domain_free_irqs = msi_domain_ops_default.domain_free_irqs;

	if (!(info->flags & MSI_FLAG_USE_DEF_DOM_OPS))
		return;

	if (ops->get_hwirq == NULL)
		ops->get_hwirq = msi_domain_ops_default.get_hwirq;
	if (ops->msi_init == NULL)
		ops->msi_init = msi_domain_ops_default.msi_init;
	if (ops->msi_check == NULL)
		ops->msi_check = msi_domain_ops_default.msi_check;
	if (ops->msi_prepare == NULL)
		ops->msi_prepare = msi_domain_ops_default.msi_prepare;
	if (ops->set_desc == NULL)
		ops->set_desc = msi_domain_ops_default.set_desc;
}

static void msi_domain_update_chip_ops(struct msi_domain_info *info)
{
	struct irq_chip *chip = info->chip;

	BUG_ON(!chip || !chip->irq_mask || !chip->irq_unmask);
	if (!chip->irq_set_affinity)
		chip->irq_set_affinity = msi_domain_set_affinity;
}

/**
 * msi_create_irq_domain - Create an MSI interrupt domain
 * @fwnode:	Optional fwnode of the interrupt controller
 * @info:	MSI domain info
 * @parent:	Parent irq domain
 *
 * Return: pointer to the created &struct irq_domain or %NULL on failure
 */
struct irq_domain *msi_create_irq_domain(struct fwnode_handle *fwnode,
					 struct msi_domain_info *info,
					 struct irq_domain *parent)
{
	struct irq_domain *domain;

	msi_domain_update_dom_ops(info);
	if (info->flags & MSI_FLAG_USE_DEF_CHIP_OPS)
		msi_domain_update_chip_ops(info);

	domain = irq_domain_create_hierarchy(parent, IRQ_DOMAIN_FLAG_MSI, 0,
					     fwnode, &msi_domain_ops, info);

	if (domain && !domain->name && info->chip)
		domain->name = info->chip->name;

	return domain;
}

int msi_domain_prepare_irqs(struct irq_domain *domain, struct device *dev,
			    int nvec, msi_alloc_info_t *arg)
{
	struct msi_domain_info *info = domain->host_data;
	struct msi_domain_ops *ops = info->ops;
	int ret;

	ret = ops->msi_check(domain, info, dev);
	if (ret == 0)
		ret = ops->msi_prepare(domain, dev, nvec, arg);

	return ret;
}

int msi_domain_populate_irqs(struct irq_domain *domain, struct device *dev,
			     int virq, int nvec, msi_alloc_info_t *arg)
{
	struct msi_domain_info *info = domain->host_data;
	struct msi_domain_ops *ops = info->ops;
	struct msi_desc *desc;
	int ret = 0;

	for_each_msi_entry(desc, dev) {
		/* Don't even try the multi-MSI brain damage. */
		if (WARN_ON(!desc->irq || desc->nvec_used != 1)) {
			ret = -EINVAL;
			break;
		}

		if (!(desc->irq >= virq && desc->irq < (virq + nvec)))
			continue;

		ops->set_desc(arg, desc);
		/* Assumes the domain mutex is held! */
		ret = irq_domain_alloc_irqs_hierarchy(domain, desc->irq, 1,
						      arg);
		if (ret)
			break;

		irq_set_msi_desc_off(desc->irq, 0, desc);
	}

	if (ret) {
		/* Mop up the damage */
		for_each_msi_entry(desc, dev) {
			if (!(desc->irq >= virq && desc->irq < (virq + nvec)))
				continue;

			irq_domain_free_irqs_common(domain, desc->irq, 1);
		}
	}

	return ret;
}

/*
 * Carefully check whether the device can use reservation mode. If
 * reservation mode is enabled then the early activation will assign a
 * dummy vector to the device. If the PCI/MSI device does not support
 * masking of the entry then this can result in spurious interrupts when
 * the device driver is not absolutely careful. But even then a malfunction
 * of the hardware could result in a spurious interrupt on the dummy vector
 * and render the device unusable. If the entry can be masked then the core
 * logic will prevent the spurious interrupt and reservation mode can be
 * used. For now reservation mode is restricted to PCI/MSI.
 */
static bool msi_check_reservation_mode(struct irq_domain *domain,
				       struct msi_domain_info *info,
				       struct device *dev)
{
	struct msi_desc *desc;

	switch(domain->bus_token) {
	case DOMAIN_BUS_PCI_MSI:
	case DOMAIN_BUS_VMD_MSI:
		break;
	default:
		return false;
	}

	if (!(info->flags & MSI_FLAG_MUST_REACTIVATE))
		return false;

	if (IS_ENABLED(CONFIG_PCI_MSI) && pci_msi_ignore_mask)
		return false;

	/*
	 * Checking the first MSI descriptor is sufficient. MSIX supports
	 * masking and MSI does so when the can_mask attribute is set.
	 */
	desc = first_msi_entry(dev);
	return desc->pci.msi_attrib.is_msix || desc->pci.msi_attrib.can_mask;
}

static int msi_handle_pci_fail(struct irq_domain *domain, struct msi_desc *desc,
			       int allocated)
{
	switch(domain->bus_token) {
	case DOMAIN_BUS_PCI_MSI:
	case DOMAIN_BUS_VMD_MSI:
		if (IS_ENABLED(CONFIG_PCI_MSI))
			break;
		fallthrough;
	default:
		return -ENOSPC;
	}

	/* Let a failed PCI multi MSI allocation retry */
	if (desc->nvec_used > 1)
		return 1;

	/* If there was a successful allocation let the caller know */
	return allocated ? allocated : -ENOSPC;
}

int __msi_domain_alloc_irqs(struct irq_domain *domain, struct device *dev,
			    int nvec)
{
	struct msi_domain_info *info = domain->host_data;
	struct msi_domain_ops *ops = info->ops;
	struct irq_data *irq_data;
	struct msi_desc *desc;
	msi_alloc_info_t arg = { };
	int allocated = 0;
	int i, ret, virq;
	bool can_reserve;

	ret = msi_domain_prepare_irqs(domain, dev, nvec, &arg);
	if (ret)
		return ret;

	for_each_msi_entry(desc, dev) {
		ops->set_desc(&arg, desc);

		virq = __irq_domain_alloc_irqs(domain, -1, desc->nvec_used,
					       dev_to_node(dev), &arg, false,
					       desc->affinity);
		if (virq < 0) {
			ret = msi_handle_pci_fail(domain, desc, allocated);
			goto cleanup;
		}

		for (i = 0; i < desc->nvec_used; i++) {
			irq_set_msi_desc_off(virq, i, desc);
			irq_debugfs_copy_devname(virq + i, dev);
		}
		allocated++;
	}

	can_reserve = msi_check_reservation_mode(domain, info, dev);

	/*
	 * This flag is set by the PCI layer as we need to activate
	 * the MSI entries before the PCI layer enables MSI in the
	 * card. Otherwise the card latches a random msi message.
	 */
	if (!(info->flags & MSI_FLAG_ACTIVATE_EARLY))
		goto skip_activate;

	for_each_msi_vector(desc, i, dev) {
		if (desc->irq == i) {
			virq = desc->irq;
			dev_dbg(dev, "irq [%d-%d] for MSI\n",
				virq, virq + desc->nvec_used - 1);
		}

		irq_data = irq_domain_get_irq_data(domain, i);
		if (!can_reserve) {
			irqd_clr_can_reserve(irq_data);
			if (domain->flags & IRQ_DOMAIN_MSI_NOMASK_QUIRK)
				irqd_set_msi_nomask_quirk(irq_data);
		}
		ret = irq_domain_activate_irq(irq_data, can_reserve);
		if (ret)
			goto cleanup;
	}

skip_activate:
	/*
	 * If these interrupts use reservation mode, clear the activated bit
	 * so request_irq() will assign the final vector.
	 */
	if (can_reserve) {
		for_each_msi_vector(desc, i, dev) {
			irq_data = irq_domain_get_irq_data(domain, i);
			irqd_clr_activated(irq_data);
		}
	}
	return 0;

cleanup:
	msi_domain_free_irqs(domain, dev);
	return ret;
}

/**
 * msi_domain_alloc_irqs - Allocate interrupts from a MSI interrupt domain
 * @domain:	The domain to allocate from
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are allocated
 * @nvec:	The number of interrupts to allocate
 *
 * Return: %0 on success or an error code.
 */
int msi_domain_alloc_irqs(struct irq_domain *domain, struct device *dev,
			  int nvec)
{
	struct msi_domain_info *info = domain->host_data;
	struct msi_domain_ops *ops = info->ops;

	return ops->domain_alloc_irqs(domain, dev, nvec);
}

void __msi_domain_free_irqs(struct irq_domain *domain, struct device *dev)
{
	struct irq_data *irq_data;
	struct msi_desc *desc;
	int i;

	for_each_msi_vector(desc, i, dev) {
		irq_data = irq_domain_get_irq_data(domain, i);
		if (irqd_is_activated(irq_data))
			irq_domain_deactivate_irq(irq_data);
	}

	for_each_msi_entry(desc, dev) {
		/*
		 * We might have failed to allocate an MSI early
		 * enough that there is no IRQ associated to this
		 * entry. If that's the case, don't do anything.
		 */
		if (desc->irq) {
			irq_domain_free_irqs(desc->irq, desc->nvec_used);
			desc->irq = 0;
		}
	}
}

/**
 * msi_domain_free_irqs - Free interrupts from a MSI interrupt @domain associated to @dev
 * @domain:	The domain to managing the interrupts
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are free
 */
void msi_domain_free_irqs(struct irq_domain *domain, struct device *dev)
{
	struct msi_domain_info *info = domain->host_data;
	struct msi_domain_ops *ops = info->ops;

	return ops->domain_free_irqs(domain, dev);
}

/**
 * msi_get_domain_info - Get the MSI interrupt domain info for @domain
 * @domain:	The interrupt domain to retrieve data from
 *
 * Return: the pointer to the msi_domain_info stored in @domain->host_data.
 */
struct msi_domain_info *msi_get_domain_info(struct irq_domain *domain)
{
	return (struct msi_domain_info *)domain->host_data;
}

#endif /* CONFIG_GENERIC_MSI_IRQ_DOMAIN */
