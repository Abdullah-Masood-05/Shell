/*
	Local preview of dist/ under the same BASE path GitHub Pages uses.
	  bun run serve.ts [--watch]    (PORT, default 4000)
	With --watch, the site is rebuilt when docs/ or docs-site/src changes.
*/
import { existsSync, statSync, watch } from "node:fs";
import { join, normalize } from "node:path";

const DIST = join(import.meta.dir, "dist");
let BASE = process.env.BASE ?? "/Shell/";
if (!BASE.startsWith("/")) BASE = "/" + BASE;
if (!BASE.endsWith("/")) BASE += "/";
const PORT = Number(process.env.PORT ?? 4000);

function file(rel: string): string | undefined {
	const p = normalize(join(DIST, rel));
	if (!p.startsWith(DIST)) return;
	if (existsSync(p) && statSync(p).isFile()) return p;
	const idx = join(p, "index.html");
	if (existsSync(idx)) return idx;
}

const server = Bun.serve({
	port: PORT,
	hostname: "127.0.0.1",
	fetch(req) {
		const url = new URL(req.url);
		const path = decodeURIComponent(url.pathname);
		if (path === "/" || path + "/" === BASE) return Response.redirect(BASE, 302);
		if (path.startsWith(BASE)) {
			const rel = path.slice(BASE.length);
			const f = file(rel);
			// Directories without a trailing slash redirect, as GitHub Pages does.
			if (f && rel && !rel.endsWith("/") && f.endsWith("index.html") && !rel.endsWith("index.html"))
				return Response.redirect(BASE + rel + "/" + url.search, 301);
			if (f) return new Response(Bun.file(f));
		}
		return new Response(Bun.file(join(DIST, "404.html")), { status: 404, headers: { "content-type": "text/html; charset=utf-8" } });
	},
});
console.log(`serving ${DIST} at http://127.0.0.1:${server.port}${BASE}`);

if (process.argv.includes("--watch")) {
	let timer: Timer | undefined;
	const rebuild = () => {
		clearTimeout(timer);
		timer = setTimeout(() => {
			const r = Bun.spawnSync(["bun", "run", join(import.meta.dir, "build.ts")], { stdout: "inherit", stderr: "inherit", env: process.env });
			if (r.exitCode !== 0) console.error("build failed; serving the previous output");
		}, 200);
	};
	for (const dir of [join(import.meta.dir, "..", "docs"), join(import.meta.dir, "src")])
		watch(dir, { recursive: true }, rebuild);
	console.log("watching docs/ and docs-site/src for changes");
}
