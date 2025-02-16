#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_pci.h>
#include <linux/msi.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>

#define MAX_IRQ_NUMBER 512
/*
 * here we assume all plic hwirq and pic(PCIe Interrupt
 * Controller) hwirq should be contiguous.
 * pcie_intc hwirq is index of bitmap (both software and
 * hardware), and starts from 0.
 * so we use pic hwirq as index to get plic hwirq and its
 * irq data.
 * when used as a msi parent, pic hwirq is written to Top
 * reg for triggering irq by a PCIe device.
 *
 * now we pre-requested plic interrupt, but may try request
 * plic interrupt when needed, like gicp_irq_domain_alloc.
 */
struct pcie_intc_data {
	struct platform_device *pdev;
	int irq_num;
	struct irq_domain *domain;
	struct irq_chip *chip;
	int reg_bitwidth;

	DECLARE_BITMAP(irq_bitmap, MAX_IRQ_NUMBER);
	spinlock_t lock;

	void __iomem *reg_set;
	void __iomem *reg_clr;

	phys_addr_t reg_set_phys;

	irq_hw_number_t		plic_hwirqs[MAX_IRQ_NUMBER];
	int			plic_irqs[MAX_IRQ_NUMBER];
	struct irq_data		*plic_irq_datas[MAX_IRQ_NUMBER];
	int			pic_to_plic[MAX_IRQ_NUMBER]; // mapping from tic hwirq to plic hwirq
};

// workaround for using in other modules
struct pcie_intc_data *pic_data;

struct irq_domain *sophgo_dw_pcie_get_parent_irq_domain(void)
{
	if (pic_data)
		return pic_data->domain;
	else
		return NULL;
}

static int pcie_intc_domain_translate(struct irq_domain *d,
				    struct irq_fwspec *fwspec,
				    unsigned long *hwirq,
				    unsigned int *type)
{
	struct pcie_intc_data *data = d->host_data;

	if (fwspec->param_count != 2)
		return -EINVAL;
	if (fwspec->param[1] >= data->irq_num)
		return -EINVAL;

	*hwirq = fwspec->param[0];
	*type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;
	pr_debug("%s hwirq %d, flag %d\n", __func__, fwspec->param[0], fwspec->param[1]);
	return 0;
}

static int pcie_intc_domain_alloc(struct irq_domain *domain,
				unsigned int virq, unsigned int nr_irqs,
				void *args)
{
	unsigned long flags;
	irq_hw_number_t hwirq;
	int i, ret = -1;
	struct pcie_intc_data *data = domain->host_data;

	// dynamically alloc hwirq
	spin_lock_irqsave(&data->lock, flags);
	ret = bitmap_find_free_region(data->irq_bitmap, data->irq_num,
				      order_base_2(nr_irqs));
	spin_unlock_irqrestore(&data->lock, flags);

	if (ret < 0) {
		pr_err("%s failed to alloc irq %d, total %d\n", __func__, virq, nr_irqs);
		return -ENOSPC;
	}

	hwirq = ret;
	for (i = 0; i < nr_irqs; i++) {
		irq_domain_set_info(domain, virq + i, hwirq + i,
				    data->chip,
				    data, handle_edge_irq,
				    NULL, NULL);
		data->pic_to_plic[hwirq + i] = data->plic_hwirqs[hwirq + i];
	}

	pr_debug("%s hwirq %ld, irq %d, plic irq %d, total %d\n", __func__,
		hwirq, virq, data->plic_irqs[hwirq], nr_irqs);
	return 0;
}

static void pcie_intc_domain_free(struct irq_domain *domain,
				    unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct pcie_intc_data *data = irq_data_get_irq_chip_data(d);
	unsigned long flags;

	pr_debug("%s hwirq %ld, irq %d, total %d\n", __func__, d->hwirq, virq, nr_irqs);

	spin_lock_irqsave(&data->lock, flags);
	bitmap_release_region(data->irq_bitmap, d->hwirq,
				order_base_2(nr_irqs));
	spin_unlock_irqrestore(&data->lock, flags);
}

static const struct irq_domain_ops pcie_intc_domain_ops = {
	.translate = pcie_intc_domain_translate,
	.alloc	= pcie_intc_domain_alloc,
	.free	= pcie_intc_domain_free,
};

static void pcie_intc_ack_irq(struct irq_data *d)
{
	struct pcie_intc_data *data  = irq_data_get_irq_chip_data(d);
	int reg_off = 0;
	struct irq_data *plic_irq_data = data->plic_irq_datas[d->hwirq];

	reg_off = d->hwirq;
	writel(0, (unsigned int *)data->reg_clr + reg_off);

	pr_debug("%s %ld, parent %s/%ld\n", __func__, d->hwirq,
		plic_irq_data->domain->name, plic_irq_data->hwirq);
	if (plic_irq_data->chip->irq_ack)
		plic_irq_data->chip->irq_ack(plic_irq_data);
}

static void pcie_intc_mask_irq(struct irq_data *d)
{
	struct pcie_intc_data *data  = irq_data_get_irq_chip_data(d);
	struct irq_data *plic_irq_data = data->plic_irq_datas[d->hwirq];

	pr_debug("%s %ld, parent %s/%ld\n", __func__, d->hwirq,
		plic_irq_data->domain->name, plic_irq_data->hwirq);
	plic_irq_data->chip->irq_mask(plic_irq_data);
}

static void pcie_intc_unmask_irq(struct irq_data *d)
{
	struct pcie_intc_data *data  = irq_data_get_irq_chip_data(d);
	struct irq_data *plic_irq_data = data->plic_irq_datas[d->hwirq];

	pr_debug("%s %ld, parent %s/%ld\n", __func__, d->hwirq,
		plic_irq_data->domain->name, plic_irq_data->hwirq);
	plic_irq_data->chip->irq_unmask(plic_irq_data);
}

static void pcie_intc_setup_msi_msg(struct irq_data *d, struct msi_msg *msg)
{
	struct pcie_intc_data *data  = irq_data_get_irq_chip_data(d);

	msg->address_lo = lower_32_bits(data->reg_set_phys) + 4 * (d->hwirq / 32);
	msg->address_hi = upper_32_bits(data->reg_set_phys);
	msg->data = d->hwirq % 32;

	pr_debug("%s msi#%d: address_hi %#x, address_lo %#x, data %#x\n", __func__,
		(int)d->hwirq, msg->address_hi, msg->address_lo, msg->data);
}

static int pcie_intc_set_affinity(struct irq_data *d,
				 const struct cpumask *mask, bool force)
{
	struct pcie_intc_data *data  = irq_data_get_irq_chip_data(d);
	struct irq_data *plic_irq_data = data->plic_irq_datas[d->hwirq];

	irq_data_update_effective_affinity(d, mask);
	if (plic_irq_data->chip->irq_set_affinity)
		return plic_irq_data->chip->irq_set_affinity(plic_irq_data, mask, force);
	else
		return -EINVAL;
}

static int pcie_intc_set_type(struct irq_data *d, u32 type)
{
	/*
	 * dummy function, so __irq_set_trigger can continue to set
	 * correct trigger type.
	 */
	return 0;
}

static struct irq_chip pcie_intc_irq_chip = {
	.name = "pcie-intc",
	.irq_ack = pcie_intc_ack_irq,
	.irq_mask = pcie_intc_mask_irq,
	.irq_unmask = pcie_intc_unmask_irq,
	.irq_compose_msi_msg = pcie_intc_setup_msi_msg,
	.irq_set_affinity = pcie_intc_set_affinity,
	.irq_set_type = pcie_intc_set_type,
};

static void pcie_intc_irq_handler(struct irq_desc *plic_desc)
{
	struct irq_chip *plic_chip = irq_desc_get_chip(plic_desc);
	struct pcie_intc_data *data = irq_desc_get_handler_data(plic_desc);
	irq_hw_number_t plic_hwirq = irq_desc_get_irq_data(plic_desc)->hwirq;
	irq_hw_number_t pcie_intc_hwirq;
	int pcie_intc_irq, i, ret;

	chained_irq_enter(plic_chip, plic_desc);

	for (i = 0; i < data->irq_num; i++) {
		if (data->pic_to_plic[i] == plic_hwirq)
			break;
	}
	if (i < data->irq_num) {
		pcie_intc_hwirq = i;
		pcie_intc_irq = irq_find_mapping(data->domain, pcie_intc_hwirq);
		pr_debug("%s plic hwirq %ld, tic hwirq %ld, tic irq %d\n", __func__,
				plic_hwirq, pcie_intc_hwirq, pcie_intc_irq);
		if (pcie_intc_irq)
			ret = generic_handle_irq(pcie_intc_irq);
		pr_debug("%s handled pic irq %d, %d\n", __func__, pcie_intc_irq, ret);
	} else {
		pr_debug("%s not found tic hwirq for plic hwirq %ld\n", __func__, plic_hwirq);
		// workaround, ack unexpected(unregistered) interrupt
		writel(1 << (plic_hwirq - data->plic_hwirqs[0]), data->reg_clr);
	}

	chained_irq_exit(plic_chip, plic_desc);
}

static int pcie_intc_probe(struct platform_device *pdev)
{
	struct pcie_intc_data *data;
	struct resource *res;
	struct fwnode_handle *fwnode = of_node_to_fwnode(pdev->dev.of_node);
	int ret = 0, i;

	// alloc private data
	data = kzalloc(sizeof(struct pcie_intc_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	platform_set_drvdata(pdev, data);
	data->pdev = pdev;
	spin_lock_init(&data->lock);

	if (device_property_read_u32(&pdev->dev, "reg-bitwidth", &data->reg_bitwidth))
		data->reg_bitwidth = 32;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "set");
	data->reg_set = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(data->reg_set)) {
		dev_err(&pdev->dev, "failed map set register\n");
		ret = PTR_ERR(data->reg_set);
		goto out;
	}
	data->reg_set_phys = res->start;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "clr");
	data->reg_clr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(data->reg_clr)) {
		dev_err(&pdev->dev, "failed map clear register\n");
		ret = PTR_ERR(data->reg_clr);
		goto out;
	}

	// get irq numbers
	for (i = 0; i < ARRAY_SIZE(data->plic_hwirqs); i++) {
		char name[8];
		int irq;

		snprintf(name, ARRAY_SIZE(name), "msi%d", i);
		irq = platform_get_irq_byname(pdev, name);
		if (irq < 0)
			break;

		data->plic_irqs[i] = irq;
		data->plic_irq_datas[i] = irq_get_irq_data(irq);
		data->plic_hwirqs[i] = data->plic_irq_datas[i]->hwirq;
		dev_dbg(&pdev->dev, "%s: plic hwirq %ld, plic irq %d\n", name,
				data->plic_hwirqs[i], data->plic_irqs[i]);
	}
	data->irq_num = i;
	dev_dbg(&pdev->dev, "got %d plic irqs\n", data->irq_num);

	// create IRQ domain
	data->domain = irq_domain_create_linear(fwnode, data->irq_num,
						&pcie_intc_domain_ops, data);
	if (!data->domain) {
		dev_err(&pdev->dev, "create linear irq doamin failed\n");
		ret = -ENODEV;
		goto out;
	}
	data->chip = &pcie_intc_irq_chip;

	/*
	 * workaround to deal with IRQ conflict with TPU driver,
	 * skip the firt IRQ and mark it as used.
	 */
	//bitmap_allocate_region(data->irq_bitmap, 0, order_base_2(1));
	for (i = 0; i < data->irq_num; i++)
		irq_set_chained_handler_and_data(data->plic_irqs[i],
							pcie_intc_irq_handler, data);

	irq_domain_update_bus_token(data->domain, DOMAIN_BUS_NEXUS);
	if (pic_data)
		dev_err(&pdev->dev, "pic_data is not empty, %s\n",
			dev_name(&pic_data->pdev->dev));
	pic_data = data;

	return ret;

out:
	if (data->reg_set)
		iounmap(data->reg_set);
	if (data->reg_clr)
		iounmap(data->reg_clr);
	kfree(data);
	return ret;
}

static const struct of_device_id pcie_intc_of_match[] = {
	{
		.compatible = "sophgo,pcie-intc",
	},
	{},
};

static struct platform_driver pcie_intc_driver = {
	.driver = {
		.name = "sophgo,pcie-intc",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(pcie_intc_of_match),
	},
	.probe = pcie_intc_probe,
};

static int __init pcie_intc_init(void)
{
	return platform_driver_register(&pcie_intc_driver);
}

arch_initcall(pcie_intc_init);
