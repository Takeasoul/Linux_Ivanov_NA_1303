# SimpleFS

Файловая система для Linux kernel `6.12.x` поверх блочного устройства.

## Сборка

```bash
make
```

## Загрузка и монтирование

```bash
dd if=/dev/zero of=disk.img bs=1M count=16
sudo losetup -fP disk.img
sudo insmod simplefs.ko device_name=loop0 sb_primary_sector=0 sb_backup_sector=8 max_filename_len=32 max_file_sectors=4
sudo mount -t simplefs /dev/loop0 /mnt
./tools/simplefsctl fill /mnt
./tools/simplefsctl hashes /mnt
./tools/simplefsctl map /mnt file000
sudo umount /mnt
sudo rmmod simplefs
sudo losetup -d /dev/loop0
```

## Параметры модуля

- `device_name` - имя блочного устройства (`loop0` или `/dev/loop0`)
- `sb_primary_sector` - сектор первой копии superblock
- `sb_backup_sector` - сектор второй копии superblock
- `max_filename_len` - максимальная длина имени файла
- `max_file_sectors` - максимальный размер файла в секторах

Для размещения первой копии superblock в начале диска используется `sb_primary_sector=0`.

## Размещение

- сектор имеет размер `512` байт
- два сектора заняты копиями superblock
- остальные сектора распределяются между файлами
- все файлы создаются при инициализации
- все файлы имеют одинаковый размер от `1` до `max_file_sectors` секторов
- имена файлов имеют вид `file000`, `file001`, ...

## CLI

```bash
./tools/simplefsctl fill /mnt
./tools/simplefsctl zero /mnt
./tools/simplefsctl wipe /mnt
./tools/simplefsctl hashes /mnt
./tools/simplefsctl map /mnt file000
```

Команды:

- `fill` - записать случайное число в каждый файл и прочитать его обратно
- `zero` - обнулить все файлы через ioctl
- `wipe` - стереть ФС через ioctl
- `hashes` - получить метаинформацию и crc32 по всем файлам через ioctl
- `map` - получить список физических секторов файла через ioctl

## Проверка

Smoke test требует root-доступ:

```bash
sudo ./scripts/smoke_test.sh
```
