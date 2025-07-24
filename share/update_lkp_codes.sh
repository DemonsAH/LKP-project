#!/bin/bash
# 设置路径变量
SRC=~/share/lab6/*
DST=~/linux/lkp/
LKPDIR=~/linux

# 拷贝文件
cp -r project/* ~/linux/ouichefs/

# 进入内核源目录
#cd "$LKPDIR" || exit 1

# 添加文件到 git
#git add lkp
#git add fs
#git add include

# 使用 --amend 修改上一次提交（进入编辑器，可根据需要修改提交信息）
#git commit --amend --no-edit

# 发送邮件补丁（基于 HEAD~1）
#git send-email --no-cc --to lkp-maintainers@os.rwth-aachen.de HEAD~1
