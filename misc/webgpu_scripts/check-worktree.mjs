// Apply the existing stack's file boundaries to edits not committed yet.
// check-stack.sh remains the authority for numbered commits and [shared] tags.
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../../', import.meta.url));
const policy = fs.readFileSync(new URL('check-stack.sh', import.meta.url), 'utf8');
const rule = name => {
    const match = policy.match(new RegExp(`^${name}='([^']+)'`, 'm'));
    if (!match) throw Error(`Missing ${name} in check-stack.sh`);
    return new RegExp(match[1]);
};
const allowed = rule('ALLOWED');
const shared = rule('SHARED_OK');
const git = args => execFileSync('git', ['-c', 'core.fsmonitor=false', ...args], { cwd: root, encoding: 'utf8' });
const files = [...new Set([
    ...git(['diff', '--name-only', 'HEAD', '-z']).split('\0'),
    ...git(['ls-files', '--others', '--exclude-standard', '-z']).split('\0'),
].filter(Boolean))];
const forbidden = files.filter(file => !allowed.test(file) && !shared.test(file));
const sharedFiles = files.filter(file => !allowed.test(file) && shared.test(file));
git(['diff', '--check']);
console.log(JSON.stringify({ passed: forbidden.length === 0, files: files.length, forbidden, requires_shared_commit: sharedFiles }, null, 2));
process.exit(forbidden.length ? 1 : 0);
