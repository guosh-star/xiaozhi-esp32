# QQ 音乐 Cookie 同步工具

用途：在 Windows 上用 Edge 扫码登录 y.qq.com，读取登录 Cookie 并上传到
`qqmusic-proxy` 服务器，供 `/opt/qqmusic-proxy/server.py` 获取 VIP 播放地址。

## 依赖

- Node.js 18+
- Microsoft Edge
- OpenSSH（`scp`、`ssh` 在 PATH 中）
- 在仓库根目录安装 Playwright 运行时：

```bat
npm install playwright-core
```

## 配置

设置环境变量（不要把服务器地址、私钥提交到仓库）：

```bat
set QQ_COOKIE_SSH_HOST=root@your-server
set QQ_COOKIE_SSH_KEY=%USERPROFILE%\.ssh\id_ed25519
set QQ_COOKIE_REMOTE=/opt/qqmusic-proxy/qq_cookie.txt
```

或在运行前直接修改 `tools/qq_cookie_sync.js` 顶部的默认值。

## 运行

```bat
node tools/qq_cookie_sync.js
```

也可以双击 `tools/UpdateQQMusicCookie.bat`。

- Cookie 有效：几秒内自动上传
- Cookie 失效：自动弹出 Edge，用手机 QQ 扫码登录后自动上传
- 日志：`tools/qq_cookie_sync.log`
- 登录配置目录：仓库根目录 `.qq-edge-profile/`（不会提交）

## 安全

- `qq_cookie.txt`、`.qq-edge-profile/`、日志文件都不应提交
- 服务器端 Cookie 文件权限应为 `600`
- Cookie 有效期约 3 天；过期后重新运行本工具即可
