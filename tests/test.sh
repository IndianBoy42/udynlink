python3 test_driver.py $1

find . -name *.o -delete
find . -name *.elf -delete
find . -name *.bin -delete
rm -rf build/