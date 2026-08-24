# YUView 构建指南 (WebAssembly + Qt 6.11.1 multithread)

本文档说明如何将 YUView 交叉编译为 WebAssembly (Wasm) 版本，使其可以在浏览器中运行。

## 背景

YUView 是一个重度依赖多线程的 Qt 应用（`QThread`、`QThreadPool`、`QtConcurrent`），因此必须使用 **Qt for WebAssembly multithread 版本** 进行编译。singlethread 版本无法编译 YUView 中所有线程相关代码。

## 环境要求

| 组件 | 说明 |
|------|------|
| Docker | 用于运行构建容器 |
| 基础镜像 | `anaselgarhy/qt-wasm-builder:multithread` (Qt 6.11.1 + Emscripten 4.0.7) |
| 自定义镜像 | `yuv-wasm-builder` (基于上述镜像，补充宿主 Qt 工具) |
| 源码 | `<repo-root>`（YUView 仓库根目录） |

## 一、准备构建镜像

### 1. 拉取基础镜像

```bash
docker pull anaselgarhy/qt-wasm-builder:multithread
```

> 如果拉取失败，需要为 Docker daemon 配置代理（见文末"常见问题"）。

### 2. 构建自定义镜像

项目根目录已提供 `Dockerfile.wasm`，它解决了两个关键问题：

1. **宿主 Qt 工具缺失**：基础镜像只包含 wasm 目标 Qt，但 wasm 的 qmake 包装脚本需要宿主平台的 `qmake6`、`moc`、`uic`、`rcc`。Dockerfile 通过 Alpine 包管理器安装 `qt6-qtbase-dev` 和 `qt6-qttools-dev`，并建立符号链接。
2. **qmake 包装脚本修复**：Alpine 的 qmake6 不支持 `target_qt.conf` 中的 `TargetSpec` 指令，Dockerfile 重写了 qmake 包装脚本，显式传入 `-spec wasm-emscripten`。

```bash
docker build -f Dockerfile.wasm -t yuv-wasm-builder .
```

## 二、构建 Wasm 版本

### 1. 生成 Makefile

```bash
docker run --rm -v $(pwd):/app yuv-wasm-builder \
  sh -c "cd /app/build-wasm && \
         /opt/Qt/6.11.1/wasm_multithread/bin/qmake /app/YUView.pro \
           'QMAKE_LFLAGS_RELEASE = -O0'"
```

> **重要**：`QMAKE_LFLAGS_RELEASE = -O0` 是必须的。它只降低**链接阶段**的优化级别，跳过 `wasm-opt` 优化步骤。原因是 Emscripten 的 `wasm-opt` 在处理大型 wasm 文件（YUView 约 23MB）时存在已知的 SIGSEGV bug（见 [emscripten-core/emscripten#15340](https://github.com/emscripten-core/emscripten/issues/15340)）。编译阶段仍使用 `-O2`，不影响代码性能。

### 2. 编译

```bash
docker run --rm -v $(pwd):/app yuv-wasm-builder \
  sh -c "cd /app/build-wasm && make -j\$(nproc)"
```

### 3. 构建产物

| 文件 | 路径 | 大小 |
|------|------|------|
| YUView.js | `build-wasm/YUViewApp/YUView.js` | ~541KB |
| YUView.wasm | `build-wasm/YUViewApp/YUView.wasm` | ~23MB |

## 三、部署到浏览器

### 1. 生成 HTML 加载器

使用 Qt 的 `wasmdeployqt` 工具生成 HTML 页面和辅助文件：

```bash
docker run --rm -v $(pwd):/app yuv-wasm-builder \
  sh -c "cd /app/build-wasm/YUViewApp && \
         /usr/lib/qt6/bin/wasmdeployqt YUView.js"
```

### 2. 配置 HTTP 服务器

multithread 版本依赖 `SharedArrayBuffer`，服务器必须返回以下响应头：

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

使用 Python 快速启动一个带正确响应头的服务器：

```python
# serve.py
import http.server

class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()

http.server.HTTPServer(('', 8000), Handler).serve_forever()
```

```bash
cd build-wasm/YUViewApp && python3 serve.py
```

然后浏览器访问 `http://localhost:8000/YUView.html`。

## 四、增量编译

`build-wasm` 目录位于宿主机（通过 `-v $(pwd):/app` 挂载），构建产物会持久化。修改源码后只需重新运行 `make`，会自动增量编译：

```bash
docker run --rm -v $(pwd):/app yuv-wasm-builder \
  sh -c "cd /app/build-wasm && make -j\$(nproc)"
```

> **注意**：不要在容器内执行 `rm -rf /app/build-wasm`，这会删除宿主机上的构建产物，导致全量重编译。

## 五、Wasm 平台代码适配

YUView 部分代码在 Wasm 平台不可用，已通过 `#ifndef Q_OS_WASM` 保护：

| 文件 | 原因 |
|------|------|
| `YUViewLib/src/handler/UpdateHandler.cpp` | Qt for Wasm 不支持 `QNetworkAccessManager::sslErrors` 和 `connectToHostEncrypted` |
| `YUViewLib/src/video/rgb/videoHandlerRGB.cpp` | AFBC 解码依赖外部进程 `QProcess`，Wasm 不支持 |
| `YUViewLib/src/video/yuv/videoHandlerYUV.cpp` | 同上 |

## 常见问题

### 1. Docker 拉取镜像失败

如果拉取失败，请为 Docker daemon 配置代理（`docker pull` 由 daemon 执行，不是 CLI），然后重启 Docker。

### 2. `wasm-opt` 报 SIGSEGV

这是 binaryen 的已知 bug，处理大 wasm 文件时崩溃。解决方案是链接时用 `-O0`（见上文 qmake 参数），不要试图绕过或替换 `wasm-opt`。

### 3. 浏览器无法加载（SharedArrayBuffer 未定义）

确认服务器返回了 COOP/COEP 响应头，且浏览器为现代版本（Chrome 92+、Firefox 79+、Edge 92+）。

### 4. `fatal: detected dubious ownership in repository at '/app'`

容器内 git 对挂载目录的权限检查。在容器内执行：

```bash
git config --global --add safe.directory /app
```

## 参考

- [Emscripten 官方文档 - Building to WebAssembly](https://emscripten.org/docs/compiling/WebAssembly.html)
- [Emscripten 官方文档 - Optimizing Code](https://emscripten.org/docs/optimizing/Optimizing-Code.html)
- [Qt for WebAssembly 官方文档](https://doc.qt.io/qt-6/wasm.html)
