// No devices, SDK, networking or media files: g++ host-only exact-source regressions.
const fs = require('fs');
const cp = require('child_process');
const path = require('path');
const os = require('os');
const repo = path.resolve(__dirname, '../..');
const out = process.env.CUCKOO_STOP_OUT || fs.mkdtempSync(path.join(os.tmpdir(), 'cuckoo-stop-'));
fs.mkdirSync(out, { recursive: true });
const generated = cp.spawnSync(process.execPath, [path.join(__dirname, 'prepare.cjs')], {
    env: { ...process.env, CUCKOO_STOP_OUT: out }, encoding: 'utf8'
});
if (generated.status !== 0) { console.error(generated.stdout, generated.stderr); process.exit(1); }
const distro = process.env.CUCKOO_WSL_DISTRO;
function compilerPath(p) {
    return distro ? p.replace(/^([A-Za-z]):[\\/]/, (_, drive) => '/mnt/' + drive.toLowerCase() + '/').replaceAll(String.fromCharCode(92), '/') : p;
}
function run(exe, args, timeout) {
    if (distro) { args = ['-d', distro, '--exec', exe, ...args]; exe = 'wsl.exe'; }
    return cp.spawnSync(exe, args, { encoding: 'utf8', timeout, maxBuffer: 16 * 1024 * 1024 });
}
const rows = [];
for (const [mode, flag] of [['baseline', 0], ['fixed', 1]]) {
    const source = compilerPath(path.join(out, 'host-' + mode + '.cc'));
    const binary = compilerPath(path.join(out, 'host-' + mode));
    const args = ['-std=c++20', '-pthread', '-Wall', '-Wextra', '-I' + compilerPath(path.join(repo, 'main/boards/cuckoo-clock')), '-DFIXED=' + flag, source, '-o', binary];
    const c = run(process.env.CXX || 'g++', args, 90000);
    fs.writeFileSync(path.join(out, mode + '-compile.log'), (c.stdout || '') + (c.stderr || ''));
    const row = { mode, compile_exit: c.status, compile_error: c.error?.message, command: [process.env.CXX || 'g++', ...args] };
    if (c.status === 0) {
        const result = run(binary, [], 30000);
        fs.writeFileSync(path.join(out, mode + '-run.log'), (result.stdout || '') + (result.stderr || ''));
        Object.assign(row, { run_exit: result.status, run_error: result.error?.message, stdout: result.stdout, stderr: result.stderr });
    }
    rows.push(row);
}
fs.writeFileSync(path.join(out, 'results.json'), JSON.stringify(rows, null, 2));
let ok = rows[0].compile_exit === 0 && rows[0].run_exit === 42 && rows[1].compile_exit === 0 && rows[1].run_exit === 0;
for (const [prepare, name] of [['prepare-extra.cjs','extra-mechanical'],['prepare-media.cjs','extra-media'],['prepare-audio-failure.cjs','extra-admission-failure'],['prepare-offline.cjs','real-offline'],['prepare-ducking.cjs','normal-ducking']]) {
    const generated = cp.spawnSync(process.execPath, [path.join(__dirname, prepare)], {env:{...process.env,CUCKOO_STOP_OUT:out},encoding:'utf8'});
    fs.writeFileSync(path.join(out,name+'-prepare.log'),(generated.stdout||'')+(generated.stderr||''));
    const row = {mode:name,prepare_exit:generated.status};
    if (generated.status === 0) {
        const args=['-std=c++20','-pthread','-Wall','-Wextra','-DFIXED=1','-I'+compilerPath(path.join(repo,'main/boards/cuckoo-clock')),compilerPath(path.join(out,name+'.cc')),'-o',compilerPath(path.join(out,name))];
        const c=run(process.env.CXX||'g++',args,90000);row.compile_exit=c.status;
        fs.writeFileSync(path.join(out,name+'-compile.log'),(c.stdout||'')+(c.stderr||''));
        if(c.status===0){const x=run(compilerPath(path.join(out,name)),[],30000);row.run_exit=x.status;row.stdout=x.stdout;fs.writeFileSync(path.join(out,name+'-run.log'),(x.stdout||'')+(x.stderr||''));}
    }
    ok=ok&&row.prepare_exit===0&&row.compile_exit===0&&row.run_exit===0;rows.push(row);
}
fs.writeFileSync(path.join(out,'results.json'),JSON.stringify(rows,null,2));
console.log(JSON.stringify({ passed: ok, results: out, cases: rows.map(x => ({ mode: x.mode, prepare: x.prepare_exit, compile: x.compile_exit, run: x.run_exit })) }, null, 2));
process.exit(ok ? 0 : 1);
