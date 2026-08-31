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
| YUView.js | `build-wasm/YUViewApp/YUView.js` | ~546KB |
| YUView.wasm | `build-wasm/YUViewApp/YUView.wasm` | ~28MB |
| YUView.wasm.map | `build-wasm/YUViewApp/YUView.wasm.map` | ~17MB（调试用） |

> **Source Map**：`build-wasm.sh` 已启用 `-g`（DWARF 调试信息）和 `-gsource-map=inline`（内嵌源码的 source map）。浏览器 DevTools 打开 `YUView.html` 后，WASM 调用栈会显示可读的 C++ 源码。`YUView.wasm.map` 仅用于调试，生产部署可删除以减小体积。

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

## 五、AFBC 服务端解码

Wasm 版不能启动本地 `afbcDecoder` 二进制，但可将单帧 AFBC 数据发送至内网解码服务。服务端实现和部署说明位于 [tools/afbcDecoderServer/README.md](../tools/afbcDecoderServer/README.md)。

启动服务后，在 YUView 的 **Settings → RK Tools → AFBC Decoder Service URL** 中填写服务根地址，例如：

```text
https://afbc-decoder.example.internal:8080
```

客户端会异步请求 `<服务根地址>/v1/afbc/decode`，并在响应返回后刷新当前帧。因此播放或预取 AFBC 内容时，首次显示一个帧会受网络和服务端解码延迟影响。

当 Wasm 页面和解码服务不属于同一 origin 时，服务端应设置 `AFBC_ALLOWED_ORIGIN` 为 Wasm 页面 origin。服务端已经为该 origin 返回需要的 CORS 响应头；不要使用通配符 origin。

### 局域网（LAN）访问必须使用 HTTPS

Qt 多线程 WASM 构建依赖 `SharedArrayBuffer`，而浏览器**只在安全上下文（secure context）中启用 `SharedArrayBuffer`**。`http://localhost` 和 `http://127.0.0.1` 即使走明文 HTTP 也算安全上下文，但 **LAN IP 走明文 `http://` 不是安全上下文**——此时即使服务端正确返回了 COOP/COEP 头，浏览器也会忽略它们，页面无法启动。

因此从内网其他机器访问时，必须使用 `https://<server-ip>:<port>/`。`package_release.sh` 会自动生成自签名证书（`certs/server.crt`、`certs/server.key`），`start.sh` 检测到证书后自动启用 HTTPS。首次访问时浏览器会提示自签名证书不受信任，接受该提示（或安装证书为受信任）即可。

> **同一端口同时支持 HTTP 和 HTTPS**：服务端会预览连接的首字节自动识别协议。HTTPS 连接正常提供页面和解码服务；若用户仍用明文 `http://<server-ip>:8080/` 访问，会返回一个友好的提示页（"YUView requires HTTPS"）并给出可点击的 HTTPS 链接，而不是连接失败或在日志里出现乱码。明文 HTTP 的 `/v1/afbc/decode` 请求会返回 `426 Upgrade Required`。

生产环境建议用正式证书替换自签名证书，设置 `AFBC_TLS_CERT` 和 `AFBC_TLS_KEY` 后运行 `start.sh`。

也可以一键构建包含 Wasm 页面和服务端的本地发布包：

```bash
./tools/afbcDecoderServer/package_release.sh
```

该脚本会生成 `tools/afbcDecoderServer/Release/`，并将项目根目录的 `afbcdec_linux_static` 一起打包。直接运行其中的 `start.sh`，同一服务将同时提供页面和 `/v1/afbc/decode`。设置中的默认 URL 为 `http://127.0.0.1:8080`；从其他主机访问时，将其改为实际服务 origin。

## 六、Wasm 平台代码适配

YUView 部分代码在 Wasm 平台不可用，已通过 `#ifndef Q_OS_WASM` 保护：

| 文件 | 原因 |
|------|------|
| `YUViewLib/src/handler/UpdateHandler.cpp` | Qt for Wasm 不支持 `QNetworkAccessManager::sslErrors` 和 `connectToHostEncrypted` |
| `YUViewLib/src/video/rgb/videoHandlerRGB.cpp` | 桌面端使用 `QProcess`；Wasm 端改为异步调用 AFBC 解码服务 |
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

### 5. 打开页面后 CPU 全核心占满

**现象**：页面打开后所有 CPU 核心被占满，调用栈显示所有线程都在 `QThread::exec()` → `QEventLoop::exec()`。

**原因**：Qt for WebAssembly 的 `QEventDispatcherWasm::secondaryThreadWait` 使用 `std::condition_variable::wait_for`，在 WASM 上这个等待**退化为忙等**（不真正阻塞），导致 `QThread::exec()` 事件循环在无事件时高频旋转。YUView 的每个 QThread（交互线程 + 缓存线程）都会占满一个 CPU 核心。

**修复**（已实施）：
1. `functions::getOptimalThreadCount()` 在 WASM 返回 0，`VideoCache` 不创建后台缓存线程。
2. `LoadingThread` 在 WASM 上重写 `run()`，用 `QWaitCondition` 阻塞等待任务，替代 `QEventLoop::exec()` 忙等。交互线程空闲时真正休眠，不占 CPU。
3. `VideoCache` 在 WASM 上通过 `submitLoadingJob`/`submitCacheJob` 提交任务，替代 `invokeMethod`。

**验证**：CPU 占用从 199%（2 核）降到 0.1%，忙等线程完全消失，应用功能正常。

## 参考

- [Emscripten 官方文档 - Building to WebAssembly](https://emscripten.org/docs/compiling/WebAssembly.html)
- [Emscripten 官方文档 - Optimizing Code](https://emscripten.org/docs/optimizing/Optimizing-Code.html)
- [Qt for WebAssembly 官方文档](https://doc.qt.io/qt-6/wasm.html)
