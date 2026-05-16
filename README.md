# SimpleFS

SimpleFS - учебная файловая система для Linux kernel 6.12.x. ФС
регистрируется в VFS и работает поверх указанного блочного устройства.

## Что реализовано

- две копии superblock в секторах, заданных параметрами модуля;
- проверка целостности superblock через CRC32;
- автоматическая инициализация набора файлов при первом монтировании;
- одинаковый размер всех файлов, от 1 до `max_file_sectors` секторов;
- чтение и запись обычных файлов через VFS;
- запрет на создание, удаление и переименование файлов;
- ioctl для обнуления данных, стирания ФС, получения хэшей и sector map;
- userspace-утилита `simplefsctl` для проверки работы ФС.

## Сборка

Нужны заголовки ядра для текущей версии 6.12.x.

```bash
make
```

В результате собираются:

- `simplefs.ko` - модуль ядра;
- `tools/simplefsctl` - userspace-утилита.

## Параметры модуля

- `device_name` - имя блочного устройства, например `loop14` или `/dev/loop14`;
- `sb_primary_sector` - сектор первой копии superblock;
- `sb_backup_sector` - сектор второй копии superblock;
- `max_filename_len` - максимальная длина имени файла;
- `max_file_sectors` - максимальный размер файла в секторах.

Пример параметров:

```bash
device_name=loop14 sb_primary_sector=0 sb_backup_sector=8 max_filename_len=32 max_file_sectors=4
```

Те же параметры используются ниже в примере загрузки модуля.

## Запуск вручную

Создать файл-диск и подключить его как loop device:

```bash
dd if=/dev/zero of=disk.img bs=1M count=16
sudo losetup -fP disk.img
losetup -a
```

В выводе нужно найти устройство, которое указывает на `disk.img`, например
`/dev/loop14`. Дальше удобно сохранить имя в переменную:

```bash
LOOP=/dev/loop14
LOOP_NAME=$(basename "$LOOP")
```

Загрузить модуль и смонтировать ФС:

```bash
sudo insmod simplefs.ko \
  device_name="$LOOP_NAME" \
  sb_primary_sector=0 \
  sb_backup_sector=8 \
  max_filename_len=32 \
  max_file_sectors=4

sudo mkdir -p /mnt
sudo mount -t simplefs "$LOOP" /mnt
```

Проверить, что параметры применились:

```bash
cat /sys/module/simplefs/parameters/sb_primary_sector
cat /sys/module/simplefs/parameters/sb_backup_sector
cat /sys/module/simplefs/parameters/max_file_sectors
```

## Проверка файлов

Посмотреть созданные файлы:

```bash
ls /mnt | head
```

Имена генерируются автоматически: `file00000`, `file00001` и т.д. Количество
цифр зависит от размера диска и числа файлов.

Проверить чтение и запись:

```bash
FIRST=$(ls /mnt | head -n 1)
echo 12345 | sudo tee "/mnt/$FIRST"
cat "/mnt/$FIRST"
```

Проверить, что новые файлы не создаются:

```bash
echo test | sudo tee /mnt/newfile
```

Ожидается ошибка доступа, так как операции создания файлов в ФС не
поддерживаются.

## Проверка userspace-утилиты

Записать случайное число в каждый файл и сразу прочитать его обратно:

```bash
./tools/simplefsctl fill /mnt
```

Получить хэши файлов:

```bash
./tools/simplefsctl hashes /mnt | head
```

Получить маппинг секторов для первого файла:

```bash
FIRST=$(ls /mnt | head -n 1)
./tools/simplefsctl map /mnt "$FIRST"
```

Если superblock-и находятся в секторах `0` и `8`, эти сектора не должны
попадать в mapping файлов.

Обнулить все файлы:

```bash
sudo ./tools/simplefsctl zero /mnt
xxd -l 32 "/mnt/$FIRST"
```

Стереть ФС:

```bash
sudo ./tools/simplefsctl wipe /mnt
```

Команды `zero` и `wipe` требуют прав администратора.

## Проверка superblock

До `wipe` оба сектора superblock должны быть ненулевыми:

```bash
sudo dd if="$LOOP" bs=512 skip=0 count=1 status=none | hexdump -C | head
sudo dd if="$LOOP" bs=512 skip=8 count=1 status=none | hexdump -C | head
```

После `wipe` оба сектора должны быть заполнены нулями:

```bash
sudo dd if="$LOOP" bs=512 skip=0 count=1 status=none | hexdump -C | head
sudo dd if="$LOOP" bs=512 skip=8 count=1 status=none | hexdump -C | head
```

## Автоматическая проверка

Smoke test собирает проект, создает loop device, загружает модуль,
монтирует ФС и проверяет основные ioctl:

```bash
sudo ./scripts/smoke_test.sh
```

При успешном прохождении в конце будет:

```text
simplefs smoke test passed
```

## Завершение работы

```bash
sudo umount /mnt
sudo rmmod simplefs
sudo losetup -d "$LOOP"
```

Если модуль уже загружен и `insmod` возвращает `File exists`, нужно сначала
выполнить `sudo rmmod simplefs`.
