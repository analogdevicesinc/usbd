#!/bin/sh

mount configfs -t configfs /sys/kernel/config
mkdir /sys/kernel/config/usb_gadget/ffs

# Real ADI idVendor
echo 0x0456 > /sys/kernel/config/usb_gadget/ffs/idVendor

# Dummy idProduct
echo 0xa4a4 > /sys/kernel/config/usb_gadget/ffs/idProduct

mkdir /sys/kernel/config/usb_gadget/ffs/strings/0x409
echo "Analog Devices Inc." > /sys/kernel/config/usb_gadget/ffs/strings/0x409/manufacturer
echo "M2K" > /sys/kernel/config/usb_gadget/ffs/strings/0x409/product
echo 00000000 > /sys/kernel/config/usb_gadget/ffs/strings/0x409/serialnumber

mkdir /sys/kernel/config/usb_gadget/ffs/functions/ffs.m2k_ffs
mkdir /sys/kernel/config/usb_gadget/ffs/configs/c.1
mkdir /sys/kernel/config/usb_gadget/ffs/configs/c.1/strings/0x409
echo "M2K IIO" > /sys/kernel/config/usb_gadget/ffs/configs/c.1/strings/0x409/configuration

ln -s /sys/kernel/config/usb_gadget/ffs/functions/ffs.m2k_ffs /sys/kernel/config/usb_gadget/ffs/configs/c.1/ffs.m2k_ffs

mkdir /dev/m2k_ffs
mount m2k_ffs -t functionfs /dev/m2k_ffs

echo "Start: echo ci_hdrc.0 > /sys/kernel/config/usb_gadget/ffs/UDC"
