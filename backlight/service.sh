MODDIR=${0%/*}
CONF="$MODDIR/config.conf"
PROP="$MODDIR/module.prop"
LOG=/data/local/tmp/brightness_curve.log

log() {
  echo "[$(date +%H:%M:%S)] $1" >> "$LOG"
  [ "$(wc -c < "$LOG" 2>/dev/null)" -gt 32768 ] && {
    tail -c 16384 "$LOG" > "${LOG}.tmp"
    mv "${LOG}.tmp" "$LOG"
  }
}

echo -n > "$LOG"

log "===== 🌟 K70专用亮度增强 v1.7.6 服务启动 🌟 ====="

if [ -x "$MODDIR/brightness" ]; then
  sed -i 's/^description=.*/description=已运行;  控制方式: 开启自动亮度 → 模块暂停接管；关闭自动亮度 → 模块自动接管 /' "$PROP"
  log "===== 💎 C语言版启动 ====="
  sleep 3
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
  exec "$MODDIR/brightness" "$CONF"
fi
done