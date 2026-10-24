/**
 * QQ 音乐 Cookie 同步脚本（Windows 端）
 * - 首次运行：弹出 Edge，扫码登录 y.qq.com
 * - 之后运行：无头模式读取 Cookie，scp 上传到服务器
 * - 服务器端默认：/opt/qqmusic-proxy/qq_cookie.txt（600）
 *
 * 环境变量：
 *   QQ_COOKIE_SSH_HOST  服务器 SSH 地址（必填，例如 root@1.2.3.4）
 *   QQ_COOKIE_SSH_KEY   SSH 私钥路径（默认 ~/.ssh/id_ed25519）
 *   QQ_COOKIE_REMOTE    远端 cookie 文件路径（默认 /opt/qqmusic-proxy/qq_cookie.txt）
 *
 * 用法：node tools/qq_cookie_sync.js
 */
const { chromium } = require('playwright-core');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');

const PROFILE = path.join(__dirname, '..', '.qq-edge-profile');
const LOG = path.join(__dirname, 'qq_cookie_sync.log');
const SERVER = process.env.QQ_COOKIE_SSH_HOST || '';
const KEY = process.env.QQ_COOKIE_SSH_KEY || path.join(os.homedir(), '.ssh', 'id_ed25519');
const REMOTE = process.env.QQ_COOKIE_REMOTE || '/opt/qqmusic-proxy/qq_cookie.txt';

function log(msg) {
    const line = `[${new Date().toISOString()}] ${msg}`;
    console.log(line);
    try { fs.appendFileSync(LOG, line + '\n'); } catch (e) {}
}

async function grab(headless, waitMs) {
    const ctx = await chromium.launchPersistentContext(PROFILE, {
        channel: 'msedge',
        headless,
        viewport: { width: 1280, height: 860 },
        args: ['--no-first-run', '--disable-blink-features=AutomationControlled'],
    });
    let page = ctx.pages()[0];
    if (!page) page = await ctx.newPage();
    try {
        await page.goto('https://y.qq.com/', { waitUntil: 'domcontentloaded', timeout: 45000 });
    } catch (e) {
        log('打开 y.qq.com 出错: ' + e.message);
    }
    if (!headless) {
        try {
            await page.getByText('登录', { exact: true }).first().click({ timeout: 5000 });
            log('已点击登录按钮，请扫码');
        } catch (e) {
            log('未找到登录按钮（可能已登录或页面结构变化）');
        }
    }
    const deadline = Date.now() + waitMs;
    let lastLog = 0;
    while (Date.now() < deadline) {
        let all;
        try {
            all = await ctx.cookies();
        } catch (e) {
            log('浏览器已关闭，停止等待');
            return { ctx, header: null };
        }
        const qq = all.filter(c => (c.domain || '').includes('qq.com'));
        const map = {};
        for (const c of qq) map[c.name] = c.value;
        const loginId = map.uin || map.wxuin || map.euin || map.qlogin_uid || '';
        if (loginId && (map.qm_keyst || map.qqmusic_key)) {
            return {
                ctx,
                header: qq.map(c => `${c.name}=${c.value}`).join('; '),
                uin: loginId,
                expires: (qq.find(c => c.name === 'qm_keyst') || {}).expires || 0,
            };
        }
        if (headless) break;
        if (Date.now() - lastLog > 10000) {
            log(`页面: ${page.url()} | cookies: ${Object.keys(map).slice(0, 40).join(',')}`);
            lastLog = Date.now();
        }
        try {
            await page.waitForTimeout(2000);
        } catch (e) {
            log('页面已关闭，停止等待');
            return { ctx, header: null };
        }
    }
    return { ctx, header: null };
}

(async () => {
    log('=== QQ Cookie 同步开始 ===');
    if (!SERVER) {
        log('未设置 QQ_COOKIE_SSH_HOST，退出');
        process.exit(1);
    }
    let r;
    try {
        r = await grab(true, 4000);
    } catch (e) {
        log('无头启动失败，改用有头模式: ' + e.message);
        r = { ctx: null, header: null };
    }

    if (!r.header) {
        if (r.ctx) { try { await r.ctx.close(); } catch (e) {} }
        await new Promise(res => setTimeout(res, 1200));
        log('未检测到登录，打开 Edge 窗口，请用手机 QQ 扫码登录（最多等 10 分钟）');
        r = await grab(false, 600000);
    }

    if (!r.header) {
        log('登录超时：没有拿到 qm_keyst/uin');
        if (r.ctx) { try { await r.ctx.close(); } catch (e) {} }
        process.exit(2);
    }

    log(`Cookie 已获取：uin=${r.uin}，长度=${r.header.length}`);
    if (r.expires) {
        const left = ((r.expires * 1000 - Date.now()) / 86400000).toFixed(1);
        log(`Cookie 到期：${new Date(r.expires * 1000).toLocaleString()}（剩余 ${left} 天）`);
    }
    const tmp = path.join(os.tmpdir(), `qq_cookie_${Date.now()}.txt`);
    fs.writeFileSync(tmp, r.header, 'utf8');

    try {
        execFileSync('scp', ['-i', KEY, '-o', 'StrictHostKeyChecking=no', tmp, `${SERVER}:${REMOTE}`], { stdio: 'pipe' });
        execFileSync('ssh', ['-i', KEY, '-o', 'StrictHostKeyChecking=no', SERVER, `chmod 600 ${REMOTE}`], { stdio: 'pipe' });
        log('已上传到服务器并设置 600 权限');
    } catch (e) {
        const err = e.stderr ? e.stderr.toString() : e.message;
        log('上传失败: ' + err);
        process.exitCode = 3;
    } finally {
        try { fs.unlinkSync(tmp); } catch (e) {}
        await r.ctx.close();
    }
    log('=== QQ Cookie 同步结束 ===');
})();
