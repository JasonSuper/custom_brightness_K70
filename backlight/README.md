# K70 Backlight (custom_brightness_K70)

适用于红米 K70 的亮度增强模块，使用 C 程序读取环境光并按自定义曲线调整背光。

## 功能简介

- 双传感器 lux 融合（前置 + 后置）
- 滑动窗口平滑 + 突变噪声过滤
- 趋势预测（线性回归）辅助亮度决策
- 非对称滞回（提亮更灵敏、变暗更稳）
- 系统自动亮度开启时自动让出控制权

## 项目结构

- `main.c`：核心亮度控制程序（运行时编译生成 `brightness`）
- `config.conf`：算法与曲线配置
- `customize.sh`：安装时脚本（检测编译器并编译 C 程序）
- `service.sh`：开机服务入口
- `uninstall.sh`：卸载清理脚本
- `module.prop`：模块元数据
- `webroot/index.html`：WebUI 页面入口

## 安装与运行

1. 将模块以 Magisk/KernelSU 模块方式安装。
2. 安装脚本会尝试查找可用编译器（如 `clang`/`gcc`）并编译 `main.c`。
3. 重启后，`service.sh` 启动 `brightness` 进程并读取 `config.conf`。

> 日志默认输出到：`/data/local/tmp/brightness_curve.log`

## 控制逻辑

- 自动亮度开启：模块暂停接管
- 自动亮度关闭：模块恢复接管

这意味着模块与系统自动亮度不会同时抢占背光控制。

## 配置说明（`config.conf`）

常用项如下：

- `hysteresis_up` / `hysteresis_down`：提亮/变暗滞回系数
- `window_size`：滑动窗口长度
- `spurious_threshold`：突变噪声过滤阈值
- `sensor_sample_rate_ms`：传感器采样周期（毫秒）
- `poll_interval_ms`：主循环阻塞轮询周期（毫秒）
- `segments.N`：lux 到亮度的分段映射

建议先小步调整，观察日志后再继续微调。

## 卸载

`uninstall.sh` 会清理：

- 模块目录：`/data/adb/modules/custom_brightness_K70`
- 日志文件：`/data/local/tmp/brightness_curve.log`
- 历史日志：`/data/local/tmp/brightness_curve.log.old`

## 注意事项

- 仅保证对 K70 场景进行优化，其他机型请自行验证。
- 若安装时未找到可用编译器，`brightness` 可能不会生成。
- 调参过激可能导致亮度频繁波动，建议先调整滞回和分段映射。
