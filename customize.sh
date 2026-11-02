#!/system/bin/sh
# KernelSU / Magisk 通用安装脚本

SKIPUNZIP=0
umask 022

ui_print "- Q20 runtime"

if [ -f "$MODPATH/core.bin" ]; then
  SZ=$(stat -c %s "$MODPATH/core.bin" 2>/dev/null)
  ui_print "- image ${SZ} bytes"
else
  abort "! core.bin 缺失，安装包不完整"
fi

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/zygisk/arm64-v8a.so" 0 0 0644
set_perm "$MODPATH/core.bin" 0 0 0644
set_perm "$MODPATH/customize.sh" 0 0 0755

ui_print "- 装完必须重启"
