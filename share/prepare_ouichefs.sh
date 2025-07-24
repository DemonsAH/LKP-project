OUICHE_DIR=~/linux/ouichefs

cd "OUICHE_DIR"
sudo umount /mnt/ouichefs # unmount
sudo losetup -d /dev/loop0
sudo rmmod ouichefs              # 可选，只有你要更新模块时才需要
make
sudo insmod ouichefs.ko

sudo losetup -fP test.img
losetup -a # show sth like /dev/loop0
sudo mount -t ouichefs /dev/loop0 /mnt/ouichefs


#gcc -o testbench testbench.c
#./testbench
