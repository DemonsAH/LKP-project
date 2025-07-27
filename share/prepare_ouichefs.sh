OUICHE_DIR=~/linux/ouichefs
sh update_lkp_codes.sh
cd "$OUICHE_DIR"
umount /mnt/ouichefs # unmount
losetup -d /dev/loop0
rmmod ouichefs              # 可选，只有你要更新模块时才需要
cd ../
make M=./ouichefs
cd ouichefs
insmod ouichefs.ko

losetup -fP test.img
losetup -a # show sth like /dev/loop0
mount -t ouichefs /dev/loop0 /mnt/ouichefs


#gcc -o testbench testbench.c
#./testbench
