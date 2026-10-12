/*
	Link checker for the built site. Reads every HTML file in dist/ and
	verifies that each href/src under BASE points at a file in dist/ and
	that #anchors on internal pages exist. Exits 1 on any broken link.
	  bun run check.ts            (run after build, same BASE)
	  STRICT_ANCHORS=0 to report missing anchors without failing.
*/
import { existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import { join, relative } from "node:path";

const DIST = join(import.meta.dir, "dist");
let BASE = process.env.BASE ?? "/Shell/";
if (!BASE.startsWith("/")) BASE = "/" + BASE;
if (!BASE.endsWith("/")) BASE += "/";
const STRICT_ANCHORS = process.env.STRICT_ANCHORS !== "0";

if (!existsSync(DIST)) { console.error("dist/ not found; run `bun run build` first"); process.exit(1); }

const files = (function list(d: string): string[] {
	return readdirSync(d, { withFileTypes: true }).flatMap(e =>
		e.isDirectory() ? list(join(d, e.name)) : e.name.endsWith(".html") ? [join(d, e.name)] : []);
})(DIST);

const idCache = new Map<string, Set<string>>();
function ids(file: string) {
	let s = idCache.get(file);
	if (!s) {
		s = new Set([...readFileSync(file, "utf8").matchAll(/\sid="([^"]+)"/g)].map(m => m[1]));
		idCache.set(file, s);
	}
	return s;
}
function target(path: string): string | undefined {
	const rel = decodeURIComponent(path.slice(BASE.length));
	const p = join(DIST, rel);
	if (existsSync(p) && statSync(p).isFile()) return p;
	const idx = join(p, "index.html");
	if (existsSync(idx)) return idx;
}

let links = 0, broken = 0, anchors = 0;
const problems: string[] = [];
for (const f of files) {
	const html = readFileSync(f, "utf8");
	const page = relative(DIST, f).replace(/\\/g, "/");
	for (const m of html.matchAll(/\s(?:href|src|srcset)="([^"]*)"/g)) {
		for (const raw of m[1].split(",").map(s => s.trim().split(/\s+/)[0])) {
			if (!raw || /^(https?:|mailto:|data:|javascript:)/i.test(raw)) continue;
			links++;
			const [path, anchor] = raw.split("#", 2);
			let file: string | undefined = f;
			if (path) {
				if (!path.startsWith(BASE)) { problems.push(`${page}: link outside BASE: ${raw}`); broken++; continue; }
				file = target(path);
				if (!file) { problems.push(`${page}: broken link ${raw}`); broken++; continue; }
			}
			if (anchor && file.endsWith(".html") && !ids(file).has(anchor)) {
				anchors++;
				problems.push(`${page}: missing anchor ${raw}`);
			}
		}
	}
}
for (const p of problems) console.log(p);
console.log(`checked ${files.length} pages, ${links} internal links: ${broken} broken, ${anchors} missing anchors`);
process.exit(broken || (STRICT_ANCHORS && anchors) ? 1 : 0);
