#ifndef USB__GADGET__CONFIGFS__H
#define USB__GADGET__CONFIGFS__H

#include <linux/configfs.h>

void unregister_gadget_item(struct config_item *item);

struct config_group *usb_os_desc_prepare_interf_dir(
		struct config_group *parent,
		int n_interf,
		struct usb_os_desc **desc,
		char **names,
		struct module *owner);

static inline struct usb_os_desc *to_usb_os_desc(struct config_item *item)
{
	return container_of(to_config_group(item), struct usb_os_desc, group);
}

//added by tommy 2020-01-31 Begin
#ifndef mdelay
#define mdelay(n) (\
	(__builtin_constant_p(n) && (n)<=MAX_UDELAY_MS) ? udelay((n)*1000) : \
	({unsigned long __ms=(n); while (__ms--) udelay(1000);}))
#endif

#define gadget_init_subsys_ffs_delay_msec(n) mdelay(n)
//added by tommy 2020-01-31 End

#endif /*  USB__GADGET__CONFIGFS__H */
