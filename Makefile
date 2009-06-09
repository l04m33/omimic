
g_omimic-objs := omimic.o usbstring.o config.o epautoconf.o
obj-m += g_omimic.o
KDIR:=/home/l_amee/books/kernel_source/linux-2.6.29.1

EXTRA_CFLAGS+= -DOMIMIC_DEBUG -g 

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	rm -vf *.o *.ko
	rm -vf *.mod.c
	rm -vf .*.cmd
	rm -vf *.symvers
	rm -vf *.order
	rm -vrf .tmp_versions
