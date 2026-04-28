MODDIR=${0%/*}
CONF="$MODDIR/config.conf"
PROP="$MODDIR/module.prop"

if [ -x "$MODDIR/brightness" ]; then
  # 更精准的杀进程逻辑（只杀当前MODDIR下的brightness）
  PROCESS_PID=$(pgrep -f "$MODDIR/brightness")
  if [ -n "$PROCESS_PID" ]; then
    log "检测到已有brightness进程(PID: $PROCESS_PID)运行，正在终止..."
    kill "$PROCESS_PID" > /dev/null 2>&1
    sleep 1
    # 若进程没正常退出，强制杀掉
    if ps -p "$PROCESS_PID" > /dev/null; then
      kill -9 "$PROCESS_PID" > /dev/null 2>&1
      log "强制终止进程(PID: $PROCESS_PID)"
    fi
  fi
fi

rm -rf /data/adb/modules/custom_brightness_K70 2>/dev/null
rm -f /data/local/tmp/brightness_curve.log 2>/dev/null
rm -f /data/local/tmp/brightness_curve.log.old 2>/dev/null