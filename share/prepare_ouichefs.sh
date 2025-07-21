cd ../linux/ouichefs

sudo rmmod ouichefs              # 可选，只有你要更新模块时才需要
make
sudo insmod ouichefs.ko

sudo losetup -fP test.img
losetup -a
sudo mount -t ouichefs /dev/loop0 /mnt/ouichefs

#gcc -o testbench testbench.c
#./testbench
