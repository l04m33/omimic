/*
 * =====================================================================================
 *
 *       Filename:  omimic.c
 *
 *    Description:  a driver to mimic input devices, using the old API
 *
 *        Version:  0x0001
 *        Created:  01/23/2009 11:15:54 PM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  l_amee (l_amee), l04m33@gmail.com
 *        Company:  SYSU
 *
 * =====================================================================================
 */


#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/device.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/hid.h>
#include <linux/ctype.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/ioctl.h>
#include <linux/list.h>
#include <asm/uaccess.h>


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kay Zheng");


#define USB_BUFSIZE 256
#define KBD_BUFSIZE 8
#define MOUSE_BUFSIZE 4
#define NR_REQ 10


#ifdef OMIMIC_DEBUG
#define PDBG(fmt, args...) printk(KERN_DEBUG "omimic: " fmt, ## args)
#else
#define PDBG(fmt, args...) /* empty debug slot */
#endif

#define OMIMIC_PERR(fmt, args...) printk(KERN_NOTICE "omimic: " fmt, ## args)
#define OMIMIC_PINFO(fmt, args...) printk(KERN_NOTICE "omimic: " fmt, ## args)

/************* types **************/
struct omimic_dev {
    struct usb_request *ctrl_req;
    struct list_head idle_list; /* list for idle requests */
    struct list_head busy_list; /* list for busy requests */
    struct usb_ep *kbd_ep;
    struct usb_ep *mouse_ep;
    //const char *kbd_ep_name;
    //const char *mouse_ep_name;
    spinlock_t lock;
    u8 cur_config;
    dev_t devno;
    struct cdev cdev;
};

struct omimic_req {
    struct usb_request *req;
    struct list_head list;
};


/************* declarations **************/

static int  omimic_bind(struct usb_gadget *);
static void omimic_unbind(struct usb_gadget *);
static int  omimic_setup(struct usb_gadget *, const struct usb_ctrlrequest *);
static void omimic_setup_complete(struct usb_ep *ep, struct usb_request *req);
static void omimic_disconnect(struct usb_gadget *);
static void omimic_suspend(struct usb_gadget *);
static void omimic_resume(struct usb_gadget *);
static int  omimic_set_config(struct usb_gadget*, unsigned, unsigned);
static void omimic_reset_config(struct usb_gadget*);
static void __free_ep_req(struct usb_ep *ep, struct usb_request *req);
static int config_buf(struct usb_gadget *, u8 *, u8, unsigned);
static int set_km_config(struct usb_gadget *, unsigned);
static void intr_complete(struct usb_ep *, struct usb_request *);
static struct usb_request *alloc_ep_req(struct usb_ep*, unsigned);

static int omimic_open(struct inode *, struct file *);
static int omimic_release(struct inode *, struct file *);
static ssize_t omimic_write(struct file *, const char __user *, size_t, loff_t *);

static int populate_req_list(struct list_head *, struct usb_ep *, void *, int, int);


int  __init omimic_init(void);
void __exit omimic_exit(void);
module_init(omimic_init);
module_exit(omimic_exit);



/************* other globals **************/
static struct file_operations omimic_fops = {
    .open    = omimic_open,
    .release = omimic_release,
    .write   = omimic_write,
    .owner   = THIS_MODULE,
};

__u8 kbd_report_desc[] = {
    0x05, 0x01,     /* Usage Page (Generic Desktop) */
    0x09, 0x06,     /* Usage (Keyboard) */
    0xa1, 0x01,     /* Collection (Application) */

    /* 8 bits for modifier keys */
    0x05, 0x07,     /*      Usage Page (Key Codes) */
    0x19, 0xe0,     /*      Usage Minimum (224) */
    0x29, 0xe7,     /*      Usage Maximum (231) */
    0x15, 0x00,     /*      Logical Minimum (0) */
    0x25, 0x01,     /*      Logical Maximum (1) */
    0x75, 0x01,     /*      Report Size (1)     */
    0x95, 0x08,     /*      Report Count (8)    */
    0x81, 0x02,     /*      Input (Data, Variable, Absolute) */

    /* 8 reserved bits */
    0x95, 0x01,     /*      Report Count (1)    */
    0x75, 0x08,     /*      Report Size (8)     */
    0x81, 0x01,     /*      Input (Constant)    */

    /* 5-bit output for led states */
    0x95, 0x05,     /*      Report Count (5)    */
    0x75, 0x01,     /*      Report Size (1)     */
    0x05, 0x08,     /*      Usage Page (Page# for LEDs) */
    0x19, 0x01,     /*      Usage Minimum (1)   */
    0x29, 0x05,     /*      Usage Maximum (5)   */
    0x91, 0x02,     /*      Output (Data, Variable, Absolute) */

    /* 3 padding bits */
    0x95, 0x01,     /*      Report Count (1)    */
    0x75, 0x03,     /*      Report Size (3)     */
    0x91, 0x01,     /*      Output (Constant)   */

    /* key code input */
    0x95, 0x06,     /*      Report Count (6)    */
    0x75, 0x08,     /*      Report Size (8)     */
    0x15, 0x00,     /*      Logical Minimum (0) */
    0x25, 0x65,     /*      Logical Maximum (101) */
    0x05, 0x07,     /*      Usage Page (Key Codes) */
    0x19, 0x00,     /*      Usage Minimum (0)   */
    0x29, 0x65,     /*      Usage Maximum (101) */
    0x81, 0x00,     /*      Input (Data, Array) */

    0xc0,           /* End Collection */
};

__u8 mouse_report_desc[] = {
    0x05, 0x01,     /* Usage Page (Generic Desktop)     */
    0x09, 0x02,     /* Usage (Mouse)                    */
    0xa1, 0x01,     /* Collection (Application)         */
    0x09, 0x01,     /*      Usage (Pointer)             */
    0xa1, 0x00,     /*      Collection (Physical)       */

    /* 3 bits for 3 buttons */
    0x05, 0x09,     /*          Usage Page (Button)     */
    0x19, 0x01,     /*          Usage Minimum (1)       */
    0x29, 0x03,     /*          Usage Maximum (3)       */
    0x15, 0x00,     /*          Logical Minimum (0)     */
    0x25, 0x01,     /*          Logical Maximum (1)     */
    0x95, 0x03,     /*          Report Count (3)        */
    0x75, 0x01,     /*          Report Size (1)         */
    0x81, 0x02,     /*          Input (Data, Variable, Absolute) */

    /* 5 padding bits */
    0x95, 0x01,     /*          Report Count (1)        */
    0x75, 0x05,     /*          Report Size (5)         */
    0x81, 0x01,     /*          Input (Constant)        */

    /* 24 bits for 2 axes and the wheel */
    0x05, 0x01,     /*          Usage Page (Generic Desktop) */
    0x09, 0x30,     /*          Usage (X)               */
    0x09, 0x31,     /*          Usage (Y)               */
    0x09, 0x38,     /*          Usage (Wheel)           */
    0x15, 0x81,     /*          Logical Minimum (-127)  */
    0x25, 0x7f,     /*          Logical Maximum (127)   */
    0x75, 0x08,     /*          Report Size (8)         */
    0x95, 0x03,     /*          Report Count (3)        */
    0x81, 0x06,     /*          Input (Data, Variable, Relative) */

    0xc0,           /*      End Collection              */
    0xc0,           /* End Collection                   */
};


/************* USB strings **************/

#define STRIDX_MANUFACTURER 9
#define STRIDX_PRODUCT 11
#define STRIDX_SERIAL 129
#define STRIDX_KBD 249
#define STRIDX_MOUSE 251

static const char SHORT_NAME[] = "omimic";
static const char LONG_NAME[]  = "Gadget OMimic";
static const char STRING_KBD[] = "mimic the keyboard";
static const char STRING_MOUSE[] = "mimic the mouse";
static char STRING_MANUFACTURER[40] = "MadGods Studio";
static char STRING_SERIAL[40] = "0123456789.0123456789.0123456789";

static struct usb_string omimic_strings[] = {
    { STRIDX_MANUFACTURER, STRING_MANUFACTURER },
    { STRIDX_PRODUCT, LONG_NAME },
    { STRIDX_SERIAL, STRING_SERIAL }, // XXX: is this mandatory?
    { STRIDX_KBD, STRING_KBD },
    { STRIDX_MOUSE, STRING_MOUSE },
    { },
};

static struct usb_gadget_strings omimic_strtab = {
    .language = 0x0409, /* en_US */
    .strings  = omimic_strings,
};


/************* descriptors **************/

#define OMIMIC_VENDOR_NUM    0x2929
#define OMIMIC_PRODUCT_NUM   0x2929

static struct usb_device_descriptor omimic_dev_desc = {
	.bLength         =  sizeof(omimic_dev_desc),
	.bDescriptorType =  USB_DT_DEVICE,
	.bcdUSB          =  __constant_cpu_to_le16(0x0110),
    .bcdDevice       =  __constant_cpu_to_le16(0x0212), /* the driver is for s3c24xx */
	.bDeviceClass    =  0,                              /* the class is decided by the interfaces */
	.idVendor        =  __constant_cpu_to_le16(OMIMIC_VENDOR_NUM),
	.idProduct       =  __constant_cpu_to_le16(OMIMIC_PRODUCT_NUM),
	.bNumConfigurations =  1,
};

static struct usb_qualifier_descriptor omimic_dev_qualifier = {
    .bLength = sizeof(omimic_dev_qualifier),
    .bDescriptorType = USB_DT_DEVICE_QUALIFIER,
    .bcdUSB = __constant_cpu_to_le16(0x0110),
    .bDeviceClass = 0,
    .bNumConfigurations = 1,
};

#define KBD_INTF_NUM 0
#define MOUSE_INTF_NUM 1

/*--------------- kbd descriptors -----------------*/

static struct usb_interface_descriptor kbd_intf = {
    .bLength = sizeof(kbd_intf),
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = KBD_INTF_NUM,    /* first interface */
    .bNumEndpoints = 1,
    .bInterfaceClass = USB_CLASS_HID,
    .bInterfaceSubClass = 1,  /* 'boot interface' */
    .bInterfaceProtocol = 1,  /* keyboard protocol */
    .iInterface = STRIDX_KBD,
};

static struct hid_descriptor kbd_hid_desc = {
    .bLength = sizeof(kbd_hid_desc),
    .bDescriptorType = 33, /* hid descriptor */
    .bcdHID = __constant_cpu_to_le16(0x0110),
    .bCountryCode = 0,
    .bNumDescriptors = 1,
    .desc = {
        [0] = {
            .bDescriptorType = 34, /* report descriptor */
            .wDescriptorLength = __constant_cpu_to_le16(sizeof(kbd_report_desc)),
        },
    },
};

static struct usb_endpoint_descriptor kbd_ep_desc = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_IN,
    .bmAttributes = USB_ENDPOINT_XFER_INT,
    .bInterval = 10,
    .wMaxPacketSize = __constant_cpu_to_le16(KBD_BUFSIZE),
};

/*--------------- mouse descriptors -----------------*/

static struct usb_interface_descriptor mouse_intf = {
    .bLength = sizeof(mouse_intf),
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = MOUSE_INTF_NUM,    /* second interface */
    .bNumEndpoints = 1,
    .bInterfaceClass = USB_CLASS_HID,
    .bInterfaceSubClass = 1,  /* 'boot interface' */
    .bInterfaceProtocol = 2,  /* mouse protocol */
    .iInterface = STRIDX_MOUSE,
};

static struct hid_descriptor mouse_hid_desc = {
    .bLength = sizeof(mouse_hid_desc),
    .bDescriptorType = 33, /* hid descriptor */
    .bcdHID = __constant_cpu_to_le16(0x0110),
    .bCountryCode = 0,
    .bNumDescriptors = 1,
    .desc = {
        [0] = {
            .bDescriptorType = 34, /* report descriptor */
            .wDescriptorLength = __constant_cpu_to_le16(sizeof(mouse_report_desc)),
        },
    },
};

static struct usb_endpoint_descriptor mouse_ep_desc = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_IN,
    .bmAttributes = USB_ENDPOINT_XFER_INT,
    .bInterval = 10,
    .wMaxPacketSize = __constant_cpu_to_le16(MOUSE_BUFSIZE),
};

const static struct usb_descriptor_header *km_func[] = {
    (struct usb_descriptor_header *) &kbd_intf,
    (struct usb_descriptor_header *) &kbd_hid_desc,
    (struct usb_descriptor_header *) &kbd_ep_desc,
    (struct usb_descriptor_header *) &mouse_intf,
    (struct usb_descriptor_header *) &mouse_hid_desc,
    (struct usb_descriptor_header *) &mouse_ep_desc,
    NULL,
};

#define KM_CONF_VAL 2

static struct usb_config_descriptor km_config = {
    .bLength = sizeof(km_config),
    .bDescriptorType = USB_DT_CONFIG,
    .bNumInterfaces = 2,
    .bConfigurationValue = KM_CONF_VAL,
    .iConfiguration = STRIDX_KBD,
    .bmAttributes = USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
    .bMaxPower = 1,
};




/************* implementations **************/

static int  omimic_bind(struct usb_gadget *gadget)
{
    int ret;
    struct omimic_dev *odev;

    odev = (struct omimic_dev *)kmalloc(sizeof(*odev), GFP_KERNEL);
    memset(odev, 0, sizeof(*odev));
    if(!odev){
        OMIMIC_PERR("can't allocate omimic_dev structure, abort\n");
        return -ENOMEM;
    }
    set_gadget_data(gadget, odev);

    spin_lock_init(&odev->lock);
    INIT_LIST_HEAD(&odev->idle_list);
    INIT_LIST_HEAD(&odev->busy_list);

    usb_ep_autoconfig_reset(gadget);
    /* kbd endpoint */
    odev->kbd_ep = usb_ep_autoconfig(gadget, &kbd_ep_desc);
    if(!odev->kbd_ep){
        OMIMIC_PERR("can't automatically config gadget: %s\n", gadget->name);
        omimic_unbind(gadget);
        return -ENODEV;
    }
    PDBG("ep configured: %s\n", odev->kbd_ep->name);
    odev->kbd_ep->driver_data = odev; /* claiming the endpoint */
    //odev->kbd_ep_name = odev->kbd_ep->name; /* XXX: ??? */
    /* mouse endpoint */
    odev->mouse_ep = usb_ep_autoconfig(gadget, &mouse_ep_desc);
    if(!odev->mouse_ep){
        OMIMIC_PERR("can't automatically config gadget: %s\n", gadget->name);
        omimic_unbind(gadget);
        return -ENODEV;
    }
    PDBG("ep configured: %s\n", odev->mouse_ep->name);
    odev->mouse_ep->driver_data = odev;
    //odev->mouse_ep_name = odev->mouse_ep->name;

    odev->ctrl_req = usb_ep_alloc_request(gadget->ep0, GFP_KERNEL);
    if(!odev->ctrl_req){
        omimic_unbind(gadget);
        return -ENOMEM;
    }
    odev->ctrl_req->buf = kmalloc(USB_BUFSIZE, GFP_KERNEL);
    if(!odev->ctrl_req->buf){
        omimic_unbind(gadget);
        return -ENOMEM;
    }
    odev->ctrl_req->complete = omimic_setup_complete;
    PDBG("ep0 standby\n");

    // XXX: kbd & mouse use the same idle_list, but the list is allocated for kbd_ep
    //      in fact, s3c2410_alloc_buffer() doesn't use the 'ep' at all 
    //      and s3c2410_alloc_request() only use the '_ep' to print debug info (see s3c2410_udc.c)
    ret = populate_req_list(&odev->idle_list, odev->kbd_ep, intr_complete, KBD_BUFSIZE, NR_REQ);
    if(ret){
        omimic_unbind(gadget);
        return ret;
    }
    PDBG("request buffers standby\n");

    gadget->ep0->driver_data = odev;  /* claiming the control ep */
    omimic_dev_desc.bMaxPacketSize0 = gadget->ep0->maxpacket;
    omimic_dev_qualifier.bMaxPacketSize0 = omimic_dev_desc.bMaxPacketSize0;
    
    /* XXX: ignore OTG devices, and don't support auto resume */

    usb_gadget_set_selfpowered(gadget);

    /* initialize the char dev */
    PDBG("going to allocate char dev region\n");
    ret = alloc_chrdev_region(&odev->devno, 0, 1, "omimic");
    if(ret){
        OMIMIC_PERR("error allocating char device region, abort\n");
        omimic_unbind(gadget);
        return ret;
    }

    OMIMIC_PINFO("using devno: major=%d, minor=%d\n", MAJOR(odev->devno), MINOR(odev->devno));

    cdev_init(&odev->cdev, &omimic_fops);
    odev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&odev->cdev, odev->devno, 1);
    if(ret){
        OMIMIC_PERR("error adding char device, abort\n");
        omimic_unbind(gadget);
        return ret;
    }

    return 0;
}

static void omimic_unbind(struct usb_gadget *gadget)
{
    struct omimic_dev *odev = get_gadget_data(gadget);
    struct omimic_req *oreq, *tmp_oreq;

    if(odev->cdev.dev) cdev_del(&odev->cdev);
    if(odev->devno) unregister_chrdev_region(odev->devno, 1);

    if(odev->kbd_ep) odev->kbd_ep->driver_data = NULL;
    if(odev->mouse_ep) odev->mouse_ep->driver_data = NULL;

    if(odev->ctrl_req)
        __free_ep_req(gadget->ep0, odev->ctrl_req);

    list_for_each_entry_safe(oreq, tmp_oreq, &odev->idle_list, list){
        list_del(&oreq->list);
        __free_ep_req(odev->kbd_ep, oreq->req);
        kfree(oreq);
    }

    list_for_each_entry_safe(oreq, tmp_oreq, &odev->busy_list, list){
        list_del(&oreq->list);
        __free_ep_req(odev->kbd_ep, oreq->req);
        kfree(oreq);
    }

    set_gadget_data(gadget, NULL);
    kfree(odev);
    return;
}

static int  omimic_setup(struct usb_gadget *gadget, const struct usb_ctrlrequest *ctrl)
{
    struct omimic_dev *odev = get_gadget_data(gadget);
    struct usb_request *req = odev->ctrl_req;
    int ret = -EOPNOTSUPP;
    u16 w_index = le16_to_cpu(ctrl->wIndex);
    u16 w_value = le16_to_cpu(ctrl->wValue);
    u16 w_length = le16_to_cpu(ctrl->wLength);

    PDBG("omimic_setup --> w_index:%u, w_value:%u, w_length:%u\n", w_index, w_value, w_length);

    req->zero = 0;
    switch(ctrl->bRequest){
    case USB_REQ_GET_DESCRIPTOR:
        PDBG("USB_REQ_GET_DESCRIPTOR: ctrl->bRequestType: %x\n", ctrl->bRequestType);
        if(!(ctrl->bRequestType & USB_DIR_IN))
            goto unknown;
        switch(w_value >> 8){
        case USB_DT_DEVICE:
            PDBG("    USB_DT_DEVICE\n");
            ret = min(w_length, (u16)sizeof(omimic_dev_desc));
            memcpy(req->buf, &omimic_dev_desc, ret);
            break;
        case USB_DT_DEVICE_QUALIFIER:
            PDBG("    USB_DT_DEVICE_QUALIFIER\n");
            if(!gadget->is_dualspeed)
                break;
            ret = min(w_length, (u16)sizeof(omimic_dev_qualifier));
            memcpy(req->buf, &omimic_dev_qualifier, ret);
            break;
        case USB_DT_OTHER_SPEED_CONFIG:
            PDBG("    USB_DT_OTHER_SPEED_CONFIG\n");
            if(!gadget->is_dualspeed)
                break;
            /* fall through */
        case USB_DT_CONFIG:
            PDBG("    USB_DT_CONFIG\n");
            ret = config_buf(gadget, req->buf, w_value>>8, w_value & 0xff);
            if(ret >= 0)
                ret = min(w_length, (u16)ret);
            break;
        case USB_DT_STRING:
            PDBG("    USB_DT_STRING\n");
            ret = usb_gadget_get_string(&omimic_strtab, w_value & 0xff, req->buf);
            if(ret >= 0)
                ret = min(w_length, (u16)ret);
            break;
        case USB_DT_CS_CONFIG:  /* report descriptor */
            PDBG("    USB_DT_CS_CONFIG\n");
            switch(w_index){
            case KBD_INTF_NUM:
                PDBG("        KBD_INTF_NUM\n");
                ret = min(w_length, (u16)sizeof(kbd_report_desc));
                memcpy(req->buf, kbd_report_desc, ret);
                break;
            case MOUSE_INTF_NUM:
                PDBG("        MOUSE_INTF_NUM\n");
                ret = min(w_length, (u16)sizeof(mouse_report_desc));
                memcpy(req->buf, mouse_report_desc, ret);
                break;
            default:
                OMIMIC_PERR("unknown interface number: %d\n", w_index);
                ret = -EINVAL;
            }
            break;
        default:
            PDBG("    unknown descriptor type: %d\n", w_value);
        }
        break;
    case USB_REQ_SET_CONFIGURATION: /* XXX: this value duplicates the SET_REPORT request */
        // XXX: there will be a non-zero status code from the usb core, 
        //      resulting from 'nuke()' in s3c2410_udc.c, but won't stall the setup process
        PDBG("USB_REQ_SET_CONFIGURATION: ctrl->bRequestType: %x\n", ctrl->bRequestType);
//        if(ctrl->bRequestType != 0)
//            goto unknown;
        if(ctrl->bRequestType == (USB_RECIP_INTERFACE | USB_TYPE_CLASS)){
            // XXX: handle SET_REPORT request
            goto unknown;
        }else if(ctrl->bRequestType != 0)
            goto unknown;

        // ???
        //if(gadget->a_hnp_support)
        //    PDBG("    HNP available\n");
        //else if(gadget->a_alt_hnp_support)
        //    PDBG("    HNP needs a different root port\n");
        //else
        //    PDBG("    HNP inavtive\n");

        spin_lock(&odev->lock);
        ret = omimic_set_config(gadget, w_value, GFP_ATOMIC);
        spin_unlock(&odev->lock);
        break;
    case USB_REQ_GET_CONFIGURATION:
        PDBG("USB_REQ_GET_CONFIGURATION: ctrl->bRequestType: %x\n", ctrl->bRequestType);
        if(ctrl->bRequestType != USB_DIR_IN)
            goto unknown;
        *(u8*)req->buf = odev->cur_config;
        ret = min(w_length, (u16)1);
        break;
    case USB_REQ_SET_INTERFACE:
        PDBG("USB_REQ_SET_INTERFACE: ctrl->bRequestType: %x\n", ctrl->bRequestType);
        if(!(ctrl->bRequestType & USB_RECIP_INTERFACE))
            goto unknown;
        spin_lock(&odev->lock);
        if(odev->cur_config && w_index == 0 && w_value == 0){
            u8 config = odev->cur_config;
            omimic_reset_config(gadget);
            omimic_set_config(gadget, config, GFP_ATOMIC);
            ret = 0;
        }
        spin_unlock(&odev->lock);
        break;
    case USB_REQ_GET_INTERFACE: /* XXX: this value duplicates the SET_IDLE request */
        PDBG("USB_REQ_GET_INTERFACE: ctrl->bRequestType: %x\n", ctrl->bRequestType);
        //if(ctrl->bRequestType != (USB_DIR_IN | USB_RECIP_INTERFACE))
        if(!(ctrl->bRequestType & USB_RECIP_INTERFACE))
            goto unknown;
        if(!(ctrl->bRequestType & USB_DIR_IN)){
            // XXX: handle SET_IDLE
            goto unknown;
        }
        if(!odev->cur_config) break;
        if(w_index != 0){
            ret = -EDOM; // ???
            break;
        }
        *(u8*)req->buf = 0;
        ret = min(w_length, (u16)1);
        break;
    case USB_REQ_GET_STATUS:
        // XXX: to be written (seems to be optional)
        PDBG("USB_REQ_GET_STATUS: ctrl->bRequestType: %x\n", ctrl->bRequestType);
        break;

    /* class specific requests */
    case 0x02: /* GET_IDLE */
        // XXX: to be written
        PDBG("GET_IDLE: ctrl->bRequestType: %x\n", ctrl->bRequestType);
        break;

    /* ignore vendor-specific requests... */
    default:
unknown:
        PDBG("unknown control req%02x.%02x v%04x i%04x l%d\n",
                ctrl->bRequestType, ctrl->bRequest, w_value, 
                w_index, w_length);
    }
    if(ret >= 0){
        PDBG("omimic_setup --> ret:%d\n", ret);
        req->length = ret;
        req->zero = ret < w_length; /* indicate if there are pending zero's */
        ret = usb_ep_queue(gadget->ep0, req, GFP_ATOMIC);
        PDBG("usb_ep_queue --> ret:%d\n", ret);
        if(ret < 0){
            PDBG("    ep queue --> %d\n", ret);
            req->status = 0;
            omimic_setup_complete(gadget->ep0, req); /* call it myself to clean up the mess */
        }
    }

    return ret;
}

static void omimic_setup_complete(struct usb_ep *ep, struct usb_request *req)
{
    if(req->status || req->actual != req->length){
        PDBG("setup complete --> status:%d, actual:%d, length:%d\n",
                req->status, req->actual, req->length);
        req->status = 0;
    }
}

static void omimic_disconnect(struct usb_gadget *gadget)
{
    unsigned long flags;
    struct omimic_dev *odev = get_gadget_data(gadget);
    spin_lock_irqsave(&odev->lock, flags);
    omimic_reset_config(gadget);
    spin_unlock_irqrestore(&odev->lock, flags);
    return;
}

static void omimic_suspend(struct usb_gadget *gadget)
{
    return;
}

static void omimic_resume(struct usb_gadget *gadget)
{
    return;
}

static struct usb_gadget_driver omimic_driver = {
//#ifdef CONFIG_USB_GADGET_DUALSPEED
//    .speed = USB_SPEED_HIGH,
//#else
//    .speed = USB_SPEED_FULL,   /* we are always running at full speed */
//#endif
    .speed = USB_SPEED_FULL,
    .function = (char *)LONG_NAME,
    .bind = omimic_bind,
    .unbind = omimic_unbind,
    .setup = omimic_setup,
    .disconnect = omimic_disconnect,
    .suspend = omimic_suspend,
    .resume = omimic_resume,
    .driver = {
        .name = SHORT_NAME,
    },
};

int __init omimic_init(void)
{
    int ret;

    ret = usb_gadget_register_driver(&omimic_driver);
    if(ret){
        OMIMIC_PERR("error registering usb gadget driver, abort\n");
        return ret;
    }

    return 0;
}

void __exit omimic_exit(void)
{
    usb_gadget_unregister_driver(&omimic_driver);
}

static void __free_ep_req(struct usb_ep *ep, struct usb_request *req)
{
    if(req->buf)
        kfree(req->buf);
    usb_ep_free_request(ep, req);
}

static int  omimic_set_config(struct usb_gadget *gadget, unsigned number, unsigned gfp_flags)
{
    int res = 0;
    struct omimic_dev *odev = get_gadget_data(gadget);
    PDBG("omimic_set_config: %u\n", number);

    if(number == odev->cur_config) return 0;

    omimic_reset_config(gadget);
    switch(number){
    case KM_CONF_VAL:
        res = set_km_config(gadget, gfp_flags);
        PDBG("set_km_config --> res:%d\n", res);
        break;
    default:
        res = -EINVAL;
    case 0:
        return res;
    }
    if(!res && !odev->kbd_ep) res = -ENODEV;
    if(res) omimic_reset_config(gadget);
    else{
        char *speed;
        switch(gadget->speed){
        case USB_SPEED_LOW: speed = "low"; break;
        case USB_SPEED_FULL: speed = "full"; break;
        case USB_SPEED_HIGH: speed = "high"; break;
        default: speed = "?"; break;
        }
        odev->cur_config = number;
        OMIMIC_PINFO("%s speed config #%d\n", speed, number);
    }

    return res;
}

static void omimic_reset_config(struct usb_gadget *gadget)
{
    struct omimic_dev *odev = get_gadget_data(gadget);
    if(odev->cur_config == 0) return;

    PDBG("omimic_reset_config\n");

    if(odev->kbd_ep){
        usb_ep_disable(odev->kbd_ep);
        odev->kbd_ep = NULL;
    }
    if(odev->mouse_ep){
        usb_ep_disable(odev->mouse_ep);
        odev->mouse_ep = NULL;
    }
    odev->cur_config = 0;
}

static int config_buf(struct usb_gadget *gadget, u8 *buf, u8 type, unsigned index)
{
    int len;

    PDBG("config_buf --> type:%d, index:%d\n", type, index);

    if(index > 0) return -EINVAL; /* currently there's only one conf */
    len = usb_gadget_config_buf(&km_config, buf, USB_BUFSIZE, km_func);
    if(len < 0) return len;
    ((struct usb_config_descriptor *)buf)->bDescriptorType = type;
    return len;
}

static int set_km_config(struct usb_gadget *gadget, unsigned gfp_flags)
{
    int res;
    struct omimic_dev *odev = get_gadget_data(gadget);
//    struct usb_ep *ep;
//    const struct usb_endpoint_descriptor *d;

    PDBG("set_km_config\n");

//    gadget_for_each_ep(ep, gadget){
//        if(strcmp(ep->name, odev->kbd_ep_name) == 0){
//            res = usb_ep_enable(ep, &kbd_ep_desc);
//            if(res == 0){
//                PDBG("ep enabled: %s\n", ep->name);
//                ep->driver_data = odev;
//                odev->kbd_ep = ep;
//            }
//        }else if(strcmp(ep->name, odev->kbd_ep_name) == 0){
//            res = usb_ep_enable(ep, &mouse_ep_desc);
//            if(res == 0){
//                PDBG("ep enabled: %s\n", ep->name);
//                ep->driver_data = odev;
//                odev->mouse_ep = ep;
//            }
//        }
//    }

    res = usb_ep_enable(odev->kbd_ep, &kbd_ep_desc);
    if(res == 0){
        PDBG("ep enabled: %s\n", odev->kbd_ep->name);
        odev->kbd_ep->driver_data = odev;
    }else{
fail_enable:
        PDBG("ep can't be enabled: %s\n", odev->kbd_ep->name);
        return res;
    }

    res = usb_ep_enable(odev->mouse_ep, &mouse_ep_desc);
    if(res == 0){
        PDBG("ep enabled: %s\n", odev->mouse_ep->name);
        odev->mouse_ep->driver_data = odev;
    }else
        goto fail_enable;

    return 0;
}

static void intr_complete(struct usb_ep *ep, struct usb_request *req)
{
    int status = req->status;
    struct omimic_req *oreq = req->context;
    struct omimic_dev *odev = ep->driver_data;

    PDBG("intr_complete\n");

    switch(status){
    case 0: /* normal completion */
        PDBG("intr_complete: success\n");
        /* move the request to the idle list */
        spin_lock(&odev->lock);
        list_del(&oreq->list);
        list_add(&oreq->list, &odev->idle_list);
        spin_unlock(&odev->lock);
        break;
    default: /* error occurs*/
        OMIMIC_PERR("%s kbd intr complete --> status:%d, actual:%d, length:%d\n",
                ep->name, status, req->actual, req->length);
    case -ECONNABORTED:
    case -ECONNRESET:
    case -ESHUTDOWN:
//        __free_ep_req(ep, req);
        return;
    }
}

static struct usb_request *alloc_ep_req(struct usb_ep* ep, 
        unsigned length)
{
    struct usb_request *req;
    req = usb_ep_alloc_request(ep, GFP_ATOMIC);
    if(req){
        req->length = length;
        req->buf = kmalloc(length, GFP_ATOMIC);
        if(!req->buf){
            usb_ep_free_request(ep, req);
            req = NULL;
        }
    }
    return req;
}


static int omimic_open(struct inode *inode, struct file *file)
{
    struct omimic_dev *odev = container_of(inode->i_cdev, struct omimic_dev, cdev);
    file->private_data = odev;
    return 0;
}

static int omimic_release(struct inode *inode, struct file *file)
{
    file->private_data = NULL;
    return 0;
}

static ssize_t omimic_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
    struct omimic_dev *odev = file->private_data;
    struct omimic_req *oreq;
    struct usb_ep *ep;

    switch(count){
    case KBD_BUFSIZE:
        ep = odev->kbd_ep; break;
    case MOUSE_BUFSIZE:
        ep = odev->mouse_ep; break;
    default:
        ep = NULL;
    }

    if(!ep) return -EINVAL;
    
    spin_lock(&odev->lock);
    if(list_empty(&odev->idle_list)){
        spin_unlock(&odev->lock);
        return -EBUSY;
    }
    oreq = list_entry(odev->idle_list.next, struct omimic_req, list);
    list_del(&oreq->list);
    list_add(&oreq->list, &odev->busy_list);
    spin_unlock(&odev->lock);
    if(copy_from_user(oreq->req->buf, buf, count)){  /* XXX: a few bytes a time may lag the system */
        OMIMIC_PERR("can't copy from user space, abort.\n");
        return -EFAULT;
    }
    oreq->req->status = 0; /* asuring */
    oreq->req->length = count;
    oreq->req->zero = 0;
    usb_ep_queue(ep, oreq->req, GFP_KERNEL);

    return count;
}

static int populate_req_list(struct list_head *head, struct usb_ep *ep, void *complete, int size, int nr)
{
    int i;
    struct omimic_req *oreq;

    for(i=0; i<nr; i++){
        oreq = kmalloc(sizeof(*oreq), GFP_KERNEL);
        if(!oreq)
            goto check_list;
        oreq->req = alloc_ep_req(ep, size);
        if(!oreq->req){
            kfree(oreq);
check_list:  
            if(list_empty(head)){
                OMIMIC_PERR("can't allocate any request buffer, abort\n");
                return -ENOMEM;
            }else{
                OMIMIC_PINFO("can't allocate more request buffers\n");
                break;
            }
        }
        oreq->req->complete = complete;
        oreq->req->context = oreq;
        oreq->req->length = size;
        oreq->req->zero = 0;
        list_add(&oreq->list, head);
        PDBG("request buffer added to idle list\n");
    }

    return 0;
}
