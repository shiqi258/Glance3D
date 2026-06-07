# 开发常用命令

本文记录开发中高频使用的命令。下面的命令以 Windows PowerShell 为准，默认已经位于项目根目录，复制后可直接执行。

## 网页查看器


### 构建 WebAssembly 包

```powershell
npm run build
```


### 启动网页查看器开发服务器

```powershell
npm --prefix "examples\libf3d\web" run dev -- --host 127.0.0.1 --port 5175
```

## 网页查看器日志

网页查看器的浏览器控制台日志默认写入：

```text
logs\browser-console.jsonl
```

### 清空网页查看器日志

```powershell
npm --prefix "examples\libf3d\web" run clear:browser-logs
```


### 用 Edge 启动远程调试并打开网页查看器

```powershell
Start-Process "msedge" -ArgumentList "--remote-debugging-address=127.0.0.1", "--remote-debugging-port=9222", "--user-data-dir=$env:TEMP\glance3d-edge-cdp", "http://127.0.0.1:5175"
```

### 采集浏览器 DevTools 日志

```powershell
npm --prefix "examples\libf3d\web" run collect:browser-logs -- --host=127.0.0.1 --port=9222 --url=127.0.0.1:5175
```

### 采集浏览器 DevTools 日志到指定文件

```powershell
npm --prefix "examples\libf3d\web" run collect:browser-logs -- --host=127.0.0.1 --port=9222 --url=127.0.0.1:5175 --log-file="../../../logs/browser-console.jsonl"
```
