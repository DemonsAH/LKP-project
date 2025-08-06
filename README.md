# LKP-project
This repository is to store and update codes between team members for RWTH SS2025 LKP project(private use)
# How to use
clone the repo parallel to share:
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
# How to update code
Here is what ```update.sh```will do:
- copy codes from ```LKP-project/``` to ```share/project```
Here is what ```prepare_ouichefs.sh```will do:
- update codes once
- make
- umount
- losetup -d
- rmmod
- insmod
- losetup -fP
- mount
- *send email to maintainer(need manual confirmation)

