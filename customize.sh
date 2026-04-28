#!/system/bin/sh
SKIPMOUNT=false
PROPFILE=true
POSTFSDATA=false
LATESTARTSERVICE=true

ui_print ""
ui_print "  🌟🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉🌟"
ui_print "  🌟                                              🌟"
ui_print "  🌟  K70专用亮度增强          🌟"
ui_print "  🌟              v1.7.6                       🌟"
ui_print "  🌟                                              🌟"
ui_print "  🌟🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉🌟"
ui_print ""
ui_print "  📱 适配机型"
ui_print "     ✅ 红米 K70"
ui_print ""
ui_print "  🔧 核心特性"
ui_print "     🌙 自定义亮度曲线"
ui_print ""

ui_print "  ════════════════════════════════════"
ui_print "  🔨 正在检测编译环境..."
ui_print "  ════════════════════════════════════"
ui_print ""

CC=""
for c in \
  /data/data/com.termux/files/usr/bin/clang \
  /data/data/com.termux/files/usr/bin/gcc \
  /data/data/com.termux/files/usr/bin/aarch64-linux-android-clang \
  /data/data/com.termux/files/usr/bin/aarch64-linux-android-gcc \
  /system/bin/clang \
  /system/bin/gcc; do
  [ -f "$c" ] && { CC="$c"; break; }
done

if [ -n "$CC" ]; then
  ui_print "  🔧 找到编译器: $CC"
  ui_print ""

  # 💎 编译C语言版主程序
  ui_print "  ⏳ 正在编译C语言版主程序..."
  $CC -O2 -o "$MODPATH/brightness" "$MODPATH/main.c" \
      -landroid -DLOG=1 > "$MODPATH/compile.log" 2>&1
  if [ -f "$MODPATH/brightness" ]; then
    chmod 755 "$MODPATH/brightness"
    BIN_SIZE=$(du -h "$MODPATH/brightness" | cut -f1)
    rm -f "$MODPATH/compile.log"
    ui_print "  ✅ C语言版编译成功! 🎉 (大小: $BIN_SIZE)"
  else
    rm -f "$MODPATH/brightness"
    ui_print "  ⚠️ 编译失败"
    [ -f "$MODPATH/compile.log" ] && cat "$MODPATH/compile.log" | while read l; do ui_print "  $l"; done
    rm -f "$MODPATH/compile.log"
  fi

  ui_print ""
else
  rm -f "$MODPATH/brightness"
  ui_print "  ℹ️ 未找到编译器"
fi

ui_print ""
ui_print "  🔐 正在设置权限..."
set_perm_recursive $MODPATH 0 0 0755 0644
[ -f "$MODPATH/brightness" ] && set_perm $MODPATH/brightness 0 0 0755
ui_print "  ✅ 权限设置完成"

ui_print ""
ui_print "  🌟🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉🌟"
ui_print "  🌟                                              🌟"
ui_print "  🌟        🎉 安装完成 重启生效 🎉              🌟"
ui_print "  🌟                                              🌟"
ui_print "  🌟  📋 配置文件:                               🌟"
ui_print "  🌟  /data/adb/modules/                          🌟"
ui_print "  🌟    custom_brightness_K70/                 🌟"
ui_print "  🌟    config.conf                               🌟"
ui_print "  🌟                                              🌟"
ui_print "  🌟  📋 日志文件:                                🌟"
ui_print "  🌟  /data/local/tmp/                            🌟"
ui_print "  🌟    brightness_curve.log                      🌟"
ui_print "  🌟                                              🌟"
ui_print "  🌟  🎚️ 控制方式:                                🌟"
ui_print "  🌟  开启自动亮度开关 → 模块暂停接管                🌟"
ui_print "  🌟  关闭自动亮度开关 → 模块自动接管                🌟"
ui_print "  🌟                                              🌟"
ui_print "  🌟🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉✨🎉🌟"
ui_print ""

