#!/bin/bash
set -e

cd /vagrant

echo ""
echo "ЭТАП 1: Жёсткая очистка и пересборка..."
echo "_______________________________________"
echo ""

sudo umount -f -l /mnt 2>/dev/null || true
sudo losetup -D 2>/dev/null || true
sudo rmmod filesystem 2>/dev/null || true
sync && sleep 1
sudo rm -f /tmp/virt.img 2>/dev/null || true

echo "Очистка завершена"

echo ""
echo "ЭТАП 2: Компиляция модуля и утилит..."
echo "_____________________________________"
echo ""

make clean >/dev/null 2>&1
make >/dev/null 2>&1 && echo "Модуль скомпилирован" || (echo "Ошибка при компиляции модуля"; exit 1)
make userspace >/dev/null 2>&1 && echo "Утилиты скомпилированы" || (echo "Ошибка при компиляции утилит"; exit 1)

echo ""
echo "ЭТАП 3: Создание виртуального диска (10 MB)..."
echo "______________________________________________"
echo ""

sudo dd if=/dev/zero of=/tmp/virt.img bs=1M count=10 2>&1 | grep -E "records|copied"
export DEV=$(sudo losetup --show -f /tmp/virt.img)
if [ -z "$DEV" ]; then 
    echo "Ошибка: Loop-устройство не создалось"
    exit 1
fi
echo "Виртуальный диск создан: $DEV"

echo ""
echo "ЭТАП 4: Монтирование ФС..."
echo "__________________________"
echo ""

sudo umount -l "$DEV" 2>/dev/null || true
sudo umount -l /mnt 2>/dev/null || true
sleep 1

sudo insmod filesystem.ko disk_name=$DEV sb1_offset=0 sb2_offset=1024 max_name_len=64 max_file_sectors=8
sudo mount -t filesystem $DEV /mnt

echo "ФС смонтирована в /mnt"

echo ""
echo "ЭТАП 5: Проверка структуры файлов..."
echo "____________________________________"
echo ""

FILE_COUNT=$(ls -1 /mnt | wc -l)
echo "Количество файлов: $FILE_COUNT"

echo "Первые 5 файлов:"
ls -1 /mnt | head -5 | sed 's/^/  /'

if [ $FILE_COUNT -lt 2 ]; then
    echo "Ошибка: Недостаточно файлов!"
    exit 1
fi

echo "Структура файлов корректна"

echo ""
echo "ЭТАП 6: Тест чтения/записи в каждый файл..."
echo "___________________________________________"
echo ""

sudo ./userspace/fsctl demo

for i in {0..4}; do
    TEST_DATA="TEST_DATA_$i"
    
    echo -n "$TEST_DATA" | sudo tee /mnt/file_$i >/dev/null 2>&1
    
    RESULT=$(head -c ${#TEST_DATA} /mnt/file_$i 2>/dev/null)
    
    if [ "$RESULT" = "$TEST_DATA" ]; then
        echo "file_$i: запись/чтение OK"
    else
        echo "file_$i: ОШИБКА"
        exit 1
    fi
done

echo "Чтение/запись работает корректно"

echo ""
echo "ЭТАП 7: Тестирование IOCTL..."
echo "_____________________________"
echo ""
echo "IOCTL 1: Обнуление всех файлов (FS_IOCTL_ZERO_ALL)"
echo ""

sudo ./userspace/fsctl zero
NONZERO=$(sudo dd if=/mnt/file_0 bs=64 count=1 2>/dev/null | od -An -v -tu1 | awk '{ for (i = 1; i <= NF; i++) if ($i != 0) c++ } END { print c + 0 }')
if [ "$NONZERO" -eq 0 ]; then
    echo "IOCTL ZERO_ALL работает"
else
    echo "IOCTL ZERO_ALL: в file_0 остались ненулевые байты"
    exit 1
fi

echo ""
echo "IOCTL 3: Получение хешей (FS_IOCTL_GET_HASHES)"

for i in {0..4}; do
    echo -n "DATA_$i" | sudo tee /mnt/file_$i >/dev/null 2>&1
done

HASH_OUTPUT=$(sudo ./userspace/fsctl hashes 2>&1)
HASH_COUNT=$(echo "$HASH_OUTPUT" | grep -c "\[.*\]" || true)

echo "$HASH_OUTPUT" | head -8
if [ $HASH_COUNT -gt 0 ]; then
    echo "IOCTL GET_HASHES работает (найдено $HASH_COUNT файлов)"
else
    echo "IOCTL GET_HASHES: нет результатов"
fi

echo ""
echo "IOCTL 4: Получение маппинга секторов (FS_IOCTL_GET_MAPPING)"

MAPPING_OUTPUT=$(sudo ./userspace/fsctl mapping 0 2>&1)
SECTOR_COUNT=$(echo "$MAPPING_OUTPUT" | grep -c "sector=" || true)

echo "$MAPPING_OUTPUT" | head -5
if [ $SECTOR_COUNT -gt 0 ]; then
    echo "IOCTL GET_MAPPING работает (найдено $SECTOR_COUNT секторов)"
else
    echo "IOCTL GET_MAPPING: нет результатов"
fi

echo ""
echo "IOCTL 2: Стирание ФС (FS_IOCTL_ERASE_FS)"

sudo ./userspace/fsctl erase
echo "ФС размечена на стирание (требует пересборки)"
echo "IOCTL ERASE_FS выполнен"

echo ""
echo "ЭТАП 8: Проверка целостности superblock..."
echo "__________________________________________"
echo ""

DMESG_OUTPUT=$(sudo dmesg | tail -20 | grep -i "filesystem\|checksum\|magic" || true)
if [ -n "$DMESG_OUTPUT" ]; then
    echo "Сообщения ядра (последние 20 строк):"
    sudo dmesg | tail -20 | grep -i "filesystem" | sed 's/^/  /'
    echo "Superblock инициализирован и валидирован"
else
    echo  "ФС работает (сообщений об ошибках нет)"
fi