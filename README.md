## SimpleFS

Учебная файловая система для задания Linux Kernel: блочное устройство,
две копии superblock, CRC32-проверка superblock, VFS-представление файлов,
чтение/запись и ioctl-команды.

### Запуск в Vagrant

```bash
vagrant up
vagrant ssh
cd /vagrant
make
sudo ./test.sh
```

Vagrant использует `cloud-image/debian-13`; внутри проверено ядро
`6.12.88+deb13-amd64`.

### Ручной запуск

```bash
make
sudo dd if=/dev/zero of=/tmp/virt.img bs=1M count=10
DEV=$(sudo losetup --show -f /tmp/virt.img)
sudo insmod filesystem.ko disk_name=$DEV sb1_offset=0 sb2_offset=1024 max_name_len=64 max_file_sectors=8
sudo mount -t filesystem $DEV /mnt

sudo ./userspace/fsctl demo
sudo ./userspace/fsctl hashes
sudo ./userspace/fsctl mapping 0
sudo ./userspace/fsctl zero
sudo ./userspace/fsctl erase
```
