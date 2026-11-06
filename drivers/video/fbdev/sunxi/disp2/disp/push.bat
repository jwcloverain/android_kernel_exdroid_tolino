adb remount
adb push disp.ko /vendor/modules/
adb shell chmod 644 /vendor/modules/disp.ko
adb shell reboot
pause