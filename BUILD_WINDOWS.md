# YUView 构建指南 (Windows + Qt 6.10.3 + MSVC 2022)

## 环境要求

| 组件 | 路径/版本 |
|------|-----------|
| Qt | `K:\YUView\6.10.3\msvc2022_64` (Qt 6.10.3, MSVC 2022 64-bit) |
| Visual Studio | `C:\Program Files\Microsoft Visual Studio\2022\Community` |
| jom | `K:\YUView\jom\jom.exe` (Qt 官方并行构建工具, 替代 nmake) |
| 源码 | `K:\YUView\YUView` |

## 一键构建

在 PowerShell 或命令提示符中执行：

```cmd
cd /d K:\YUView\YUView\build && "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64 && "K:\YUView\jom\jom.exe" /J 8 /F Makefile
```

### 参数说明

| 参数 | 含义 |
|------|------|
| `vcvarsall.bat amd64` | 初始化 MSVC 64 位编译环境 |
| `jom /J 8` | 使用 8 个并行进程编译（根据 CPU 核心数调整） |
| `/F Makefile` | 指定 Makefile 文件 |

## 分步构建

### 1. 初始化 MSVC 环境

```cmd
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64
```

### 2. 生成 Makefile（仅首次或 .pro 文件变更后需要）

```cmd
cd /d K:\YUView\YUView\build
K:\YUView\6.10.3\msvc2022_64\bin\qmake.exe ..\YUView.pro
```

### 3. 编译

```cmd
K:\YUView\jom\jom.exe /J 8 /F Makefile
```

## 构建产物

| 文件 | 路径 |
|------|------|
| YUViewLib 静态库 | `build\YUViewLib\YUViewLib.lib` |
| YUView 可执行文件 | `build\YUViewApp\YUView.exe` |

## 清理与重新构建

```cmd
cd /d K:\YUView\YUView\build
rmdir /s /q YUViewLib YUViewApp
del /q Makefile*
K:\YUView\6.10.3\msvc2022_64\bin\qmake.exe ..\YUView.pro
K:\YUView\jom\jom.exe /J 8 /F Makefile
```

## 关于 jom

`jom` 是 Qt 官方的 nmake 替代品，支持 `-j` 并行编译。本项目使用的 Qt 6.10.3 预编译包不自带 jom，需单独下载：

- 下载地址：https://download.qt.io/official_releases/jom/jom.zip
- 解压到 `K:\YUView\jom\` 即可

## 注意事项

- 必须使用 **x64 Native Tools Command Prompt** 或先运行 `vcvarsall.bat amd64`，否则 `nmake`/`jom` 找不到 MSVC 编译器
- `/J` 参数建议设置为 CPU 逻辑核心数，例如 8 核 CPU 用 `/J 8`
- 如果修改了 `.pro` 或 `.ui` 文件，需要重新运行 `qmake` 生成 Makefile
