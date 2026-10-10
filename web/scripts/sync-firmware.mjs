// SPDX-License-Identifier: GPL-3.0-only
// Copies the firmware package built by tools/build.py into the app (public/firmware/), so the Flash tab can install it with one
// click. Run from web/ after tools/build.py:  npm run firmware
import { createHash } from 'node:crypto';
import { copyFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';

const src = new URL('../../build/modular-1.fwsc', import.meta.url);
if (!existsSync(src)) {
  console.error('build/modular-1.fwsc is missing: run tools/build.py first (inside nix develop)');
  process.exit(1);
}
const dir = new URL('../public/firmware/', import.meta.url);
mkdirSync(dir, { recursive: true });
copyFileSync(src, new URL('modular-1.fwsc', dir));
const bytes = readFileSync(src);
const version = /#define FELUCCA_VERSION "([^"]+)"/.exec(readFileSync(new URL('../../firmware/src/modular.c', import.meta.url), 'utf8'))?.[1] ?? '?';
const product = /#define FELUCCA_ID "([^"]+)"/.exec(readFileSync(new URL('../../firmware/src/modular.c', import.meta.url), 'utf8'))?.[1] ?? 'FM-1_800';
writeFileSync(new URL('modular-1.json', dir), JSON.stringify({ version, product, size: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex') }));
// the package carries three Apache-2.0 SDK files and GPL-3.0 firmware: ship both licence texts with it
copyFileSync(new URL('../../LICENSE', import.meta.url), new URL('LICENSE-GPL-3.0.txt', dir));
copyFileSync(new URL('../../LICENSES/Apache-2.0.txt', import.meta.url), new URL('LICENSE-Apache-2.0.txt', dir));
console.log(`public/firmware/modular-1.fwsc: Modular-1 ${version}, ${product}, ${bytes.length} bytes`);
