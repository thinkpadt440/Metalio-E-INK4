# 自定义 UI 离线检查

在 WSL/Linux 中，使用项目已有的 LVGL、实际页面源码及 Flash 字体包渲染 480×800 页面。
不需要设备、显示服务器或额外下载依赖；需要 CMake 和主机 C/C++ 编译器。

```bash
cmake -S tools/ui_preview -B build/ui-preview-host
cmake --build build/ui-preview-host -j 8
cd build/ui-preview-host
./preview
```

成功时输出 `PASS`，并生成 dashboard、助手空页面和三页演示回复的 PPM 图片。
PPM 可用 Pillow 或图片查看器打开。模拟器使用 32 位软件渲染，因此不验证墨水屏波形、
真实局刷、触摸硬件、充电、网络连接和休眠唤醒。预览状态栏为固定占位值。

检查包括触摸按钮入口、BOOT 回调、设置返回、分页边界、待机中的分页拦截、
60 次页面往返的定时器清理，以及标签是否超出父容器。HOME 的默认分发和
音量键路由仍需结合真机检查。

`stubs/` 只替代硬件、Flash 映射与 RTOS 接口；字体加载及页面实现直接编译项目源码。
主机渲染文件不会参与 ESP-IDF 固件构建。
