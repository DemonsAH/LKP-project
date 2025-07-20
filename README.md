# LKP-project
This repository is to store and update codes between team members for RWTH SS2025 LKP project(private use)
# How to use
懒得写英文了不装了反正是private仓库
clone在和share平行的文件夹。如：
```
share
|---project
        |---code.c
        |---code.h
        |--- ...
LKP-project
|--- update.sh
|--- code.c
|--- code.h
|--- backup
        |--- code_backup.c
        |--- ...
|--- ...
```
注意：clone命令在share的父文件夹中执行，会自动创建文件夹LKP-project
# How to update code
运行```update.sh```会完成以下工作：
- 将share/project/ 中的代码复制到backup
- 将LKP-project/下的代码复制到share/project
- 重新make
- git commit --amend
  - 此处建议把之前lab的commit清空 
- 发送邮件给maintainer（需要手动确认）
