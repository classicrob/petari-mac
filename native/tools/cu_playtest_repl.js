// Paste this script into cua_repl after its first-use documentation.
// The REPL disallows eval; use its direct JavaScript input.
// await playtest.bind(); await playtest.capture('title');
// Use observed coordinates only. Inspect fresh state after every action batch.
var playtest = {
    // The repository root: PETARI_REPO, else the REPL's working directory.
    root: (typeof process !== 'undefined' && process.env.PETARI_REPO) || (typeof process !== 'undefined' ? process.cwd() : '.'),
    app: null,
    async init() {
        this.fs = await import('node:fs/promises');
        this.out = this.root + '/build/cu-playtest';
    },
    async alive() {
        const active = JSON.parse(await this.fs.readFile(this.out + '/active-session.json', 'utf8'));
        return active;
    },
    async record(action, data = {}) {
        await this.fs.appendFile(this.out + '/actions.jsonl', JSON.stringify({utc:new Date().toISOString(), action, ...data}) + '\n');
    },
    async bind() {
        await this.init();
        const active = await this.alive();
        this.app = await cua.getApp(active.app || this.root + '/build/macos-gx/native/app/Petari.app');
        await this.record('bind', {pid:active.pid});
    },
    async tap(key, count = 1) {
        await this.alive();
        if (!Number.isInteger(count) || count < 1 || count > 40) throw new Error('Use 1..40 observed key pulses per batch');
        await this.record('key-batch-start', {key,count});
        for (let i=0;i<count;i++) await this.app.pressKey(key);
        await this.app.getAXState();
        await this.record('key-batch-end', {key,count});
    },
    async click(target, mouseButton = 'left') {
        await this.alive();
        await this.record('click', {target,mouseButton});
        await this.app.click(target,{mouseButton});
        await this.app.getAXState();
    },
    async drag(from, to) {
        await this.alive();
        await this.record('drag', {from,to});
        await this.app.drag(from,to);
        await this.app.getAXState();
    },
    async capture(label) {
        await this.alive();
        if (!/^[a-z0-9-]+$/.test(label)) throw new Error('Use a simple screenshot label');
        const state = await this.app.getAXState({emit:false});
        const png = await this.app.getScreenshot({emit:false});
        const file = new Date().toISOString().replace(/[:.]/g,'-') + '-' + label;
        await this.fs.writeFile(this.out + '/' + file + '.png',png);
        await this.fs.writeFile(this.out + '/' + file + '.txt',state);
        await this.record('screenshot',{label,file:file+'.png',state});
        await nodeRepl.emitImage(png);
        nodeRepl.write(file + '.png');
    },
    async note(text) { await this.record('observation',{text}); }
};
