/*
	Static site build for the Nilesoft Shell docs.

	The HTML fragments in ../docs stay the source of truth (they are shared
	with upstream moudey/Shell). This script wraps each page listed in
	../docs/menu.json in one layout, rewrites /docs/... links and images to
	BASE, highlights .nss code blocks, and writes pretty URLs to dist/.

	Env:
	  BASE    URL path the site is served under (default "/Shell/")
	  SITE    origin used in sitemap.xml (default GitHub Pages origin)
	  REPO    GitHub repo for "Edit this page" links
	  BRANCH  branch for those links (default "dev")
	  STRICT_ANCHORS=1  also fail on missing #anchors (default: warn)

	The build fails on links to pages that do not exist and on missing images.
*/
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync, cpSync, readdirSync, statSync } from "node:fs";
import { dirname, join, relative } from "node:path";

const ROOT = join(import.meta.dir, "..");
const DOCS = join(ROOT, "docs");
const ASSETS = join(ROOT, "packages", "assets");
const SRC = join(import.meta.dir, "src");
const DIST = join(import.meta.dir, "dist");

const BASE = normalizeBase(process.env.BASE ?? "/Shell/");
const SITE = (process.env.SITE ?? "https://abdullah-masood-05.github.io").replace(/\/+$/, "");
const REPO = process.env.REPO ?? "Abdullah-Masood-05/Shell";
const BRANCH = process.env.BRANCH ?? "dev";
const STRICT_ANCHORS = process.env.STRICT_ANCHORS === "1";

/*
	Upstream fragments link to a few URLs that only exist on nilesoft.org.
	They are mapped here instead of editing the fragments, so upstream doc
	changes keep merging cleanly.
*/
const LINK_ALIASES: Record<string, string> = {
	// "Dynamic" items were renamed "New items"; the #menu/#item anchors match.
	"/docs/configuration/dynamic": "/docs/configuration/new-items",
	// The installer download page lives on nilesoft.org.
	"/download": "https://github.com/Abdullah-Masood-05/Shell/releases/latest",
};

function normalizeBase(b: string): string {
	if (!b.startsWith("/")) b = "/" + b;
	if (!b.endsWith("/")) b += "/";
	return b;
}

// ---------------------------------------------------------------- menu ---

type MenuItem = { title: string; link: string; items?: MenuItem[] };
type Page = {
	title: string;
	link: string;     // "/docs/configuration/settings"
	slug: string;     // "configuration/settings" ("" for the index)
	file: string;     // absolute path of the fragment
	parent?: string;  // slug of the menu group
};

const menu: MenuItem[] = JSON.parse(stripBom(readFileSync(join(DOCS, "menu.json"), "utf8")));
const pages: Page[] = [];

function slugOf(link: string): string {
	const s = link.replace(/^\/docs\/?/, "").replace(/\/index(\.html)?$/, "").replace(/\.html$/, "").replace(/\/+$/, "");
	return s === "index" ? "" : s;
}

function fragmentFor(slug: string): string | undefined {
	const candidates = slug === "" ? ["index.html"] : [`${slug}.html`, `${slug}/index.html`];
	for (const c of candidates) {
		const p = join(DOCS, c);
		if (existsSync(p) && statSync(p).isFile()) return p;
	}
}

function walk(items: MenuItem[], parent?: string) {
	for (const it of items) {
		const slug = slugOf(it.link);
		const file = fragmentFor(slug);
		if (!file) fail(`menu.json: no fragment for ${it.link}`);
		pages.push({ title: it.title, link: it.link, slug, file: file!, parent });
		if (it.items) walk(it.items, slug);
	}
}

const errors: string[] = [];
const warnings: string[] = [];
function fail(msg: string) { errors.push(msg); }
function warn(msg: string) { warnings.push(msg); }
function stripBom(s: string) { return s.replace(/^﻿/, ""); }

walk(menu);
const bySlug = new Map(pages.map(p => [p.slug, p]));

// --------------------------------------------------------------- utils ---

const esc = (s: string) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
const decode = (s: string) => s
	.replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&quot;/g, '"')
	.replace(/&#39;|&apos;/g, "'").replace(/&nbsp;/g, " ")
	.replace(/&#(\d+);/g, (_, n) => String.fromCodePoint(+n))
	.replace(/&amp;/g, "&");
const textOf = (html: string) => decode(html.replace(/<[^>]*>/g, "")).replace(/\s+/g, " ").trim();
const pageUrl = (slug: string) => BASE + (slug ? slug + "/" : "");

// ------------------------------------------------------ nss highlighter ---

const KEYWORDS = new Set([
	"shell", "settings", "theme", "item", "menu", "separator", "sep", "modify", "remove",
	"import", "dynamic", "static", "if", "else", "for", "while", "break", "continue",
	"return", "true", "false", "null", "nil", "and", "or", "not",
]);

function highlightNss(src: string): string {
	let out = "";
	let i = 0;
	const n = src.length;
	const span = (cls: string, s: string) => `<span class="tok-${cls}">${esc(s)}</span>`;
	while (i < n) {
		const c = src[i];
		const rest = src.slice(i);
		let m: RegExpMatchArray | null;
		if (rest.startsWith("//")) {
			const end = src.indexOf("\n", i);
			const s = end < 0 ? rest : src.slice(i, end);
			out += span("c", s); i += s.length;
		} else if (rest.startsWith("/*")) {
			const end = src.indexOf("*/", i + 2);
			const s = end < 0 ? rest : src.slice(i, end + 2);
			out += span("c", s); i += s.length;
		} else if (c === "'" || c === '"') {
			let j = i + 1;
			while (j < n && src[j] !== c) { if (c === '"' && src[j] === "\\") j++; j++; }
			const s = src.slice(i, Math.min(j + 1, n));
			out += span("s", s); i += s.length;
		} else if ((m = rest.match(/^#[0-9a-fA-F]{3,8}\b/))) {
			out += span("n", m[0]); i += m[0].length;
		} else if ((m = rest.match(/^\d+(\.\d+)?/))) {
			out += span("n", m[0]); i += m[0].length;
		} else if ((m = rest.match(/^[$@][A-Za-z_][\w]*(?:[.\-][A-Za-z_]\w*)*/))) {
			out += span("v", m[0]); i += m[0].length;
		} else if ((m = rest.match(/^[A-Za-z_]\w*(?:[.\-][A-Za-z_]\w*)*/))) {
			const w = m[0];
			const after = src.slice(i + w.length);
			if (KEYWORDS.has(w.toLowerCase())) out += span("k", w);
			else if (/^\s*\(/.test(after)) out += span("f", w);
			else if (/^\s*=(?!=)/.test(after)) out += span("p", w);
			else out += esc(w);
			i += w.length;
		} else {
			out += esc(c); i++;
		}
	}
	return out;
}

let highlighted = 0, skippedHighlight = 0;
function highlightBlocks(html: string, where: string): string {
	return html.replace(/<code class="lang-shell">([\s\S]*?)<\/code>/g, (whole, body: string) => {
		// Blocks that already carry markup (e.g. <cite> comments) are left as written.
		if (/<[a-zA-Z\/]/.test(body)) { skippedHighlight++; return whole; }
		highlighted++;
		return `<code class="lang-shell">${highlightNss(decode(body))}</code>`;
	});
}

// --------------------------------------------------- link rewriting -----

const imagesUsed = new Set<string>();
type Ref = { from: string; target: string; anchor: string };
const anchorRefs: Ref[] = [];
const idsBySlug = new Map<string, Set<string>>();

function resolveDocsLink(raw: string, from: Page): string {
	let href = raw.trim();
	if (href === "" || /^(https?:|mailto:|javascript:|data:)/i.test(href)) return raw;

	if (href.startsWith("#")) {
		anchorRefs.push({ from: from.slug, target: from.slug, anchor: href.slice(1) });
		return raw;
	}
	let [path, anchor = ""] = href.split("#", 2) as [string, string?];
	if (LINK_ALIASES[path] !== undefined) path = LINK_ALIASES[path];
	if (/^https?:/i.test(path)) return path + (anchor ? "#" + anchor : "");

	if (path === "/docs" || path.startsWith("/docs/")) {
		if (path.startsWith("/docs/images/")) {
			const rel = path.slice("/docs/".length);
			imagesUsed.add(rel);
			if (!existsSync(join(DOCS, rel))) fail(`${from.slug || "index"}: missing image ${path}`);
			return BASE + rel;
		}
		const slug = slugOf(path);
		if (!bySlug.has(slug)) {
			fail(`${from.slug || "index"}: broken link ${raw}`);
			return raw;
		}
		if (anchor) anchorRefs.push({ from: from.slug, target: slug, anchor });
		return pageUrl(slug) + (anchor ? "#" + anchor : "");
	}
	if (path.startsWith("/")) fail(`${from.slug || "index"}: link outside /docs: ${raw}`);
	else if (path) warn(`${from.slug || "index"}: relative link ${raw}`);
	return raw;
}

function rewriteLinks(html: string, page: Page): string {
	return html.replace(/(\s(?:href|src))\s*=\s*("([^"]*)"|'([^']*)'|([^\s>"']+))/g,
		(_, attr: string, _q, d?: string, s?: string, b?: string) => {
			const v = d ?? s ?? b ?? "";
			return `${attr}="${esc(resolveDocsLink(decode(v), page))}"`;
		});
}

/*
	images/dark/X and images/light/X: emit both and let CSS show the one
	matching the active theme (system preference or the toggle).
*/
function themeImages(html: string): string {
	return html.replace(/<img\b([^>]*?)\ssrc="\/docs\/images\/(dark|light)\/([^"]+)"([^>]*)>/g,
		(whole, pre: string, mode: string, name: string, post: string) => {
			const other = mode === "dark" ? "light" : "dark";
			if (!existsSync(join(DOCS, "images", other, name))) return whole;
			const img = (m: string) => {
				let attrs = `${pre} src="/docs/images/${m}/${name}"${post}`;
				attrs = /\sclass="/.test(attrs)
					? attrs.replace(/\sclass="([^"]*)"/, ` class="$1 only-${m}"`)
					: attrs + ` class="only-${m}"`;
				return `<img${attrs}>`;
			};
			return img("light") + img("dark");
		});
}

function lazyImages(html: string): string {
	return html.replace(/<img\b(?![^>]*\sloading=)/g, '<img loading="lazy" decoding="async"');
}

// ------------------------------------------------------ search index ----

type Entry = { t: string; p: string; u: string };
const search: Entry[] = [];

/*
	Give headings without an id one derived from their text, so they can be
	linked to (e.g. /docs/expressions/string#single-quotes). Existing ids win.
*/
function headingIds(html: string): string {
	const taken = collectIds(html);
	return html.replace(/<h([3-6])((?:\s[^>]*)?)>([\s\S]*?)<\/h\1>/g, (whole, lvl: string, attrs: string, inner: string) => {
		if (/\sid\s*=/.test(attrs)) return whole;
		const base = textOf(inner).toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "");
		if (!base) return whole;
		let id = base, n = 2;
		while (taken.has(id)) id = `${base}-${n++}`;
		taken.add(id);
		return `<h${lvl} id="${id}"${attrs}>${inner}</h${lvl}>`;
	});
}

function collectIds(html: string): Set<string> {
	const ids = new Set<string>();
	for (const m of html.matchAll(/\sid\s*=\s*["']([^"']+)["']/g)) ids.add(m[1]);
	return ids;
}

function collectHeadings(html: string, page: Page) {
	search.push({ t: page.title, p: "", u: pageUrl(page.slug) });
	const seen = new Set<string>([page.title.toLowerCase()]);
	// Headings with ids, and <section id> blocks whose first heading names them.
	const re = /<section\b[^>]*\sid="([^"]+)"[^>]*>\s*<h[3-6][^>]*>([\s\S]*?)<\/h[3-6]>|<h([3-6])\b[^>]*\sid="([^"]+)"[^>]*>([\s\S]*?)<\/h\3>/g;
	for (const m of html.matchAll(re)) {
		if (m.index === 0) continue; // the page title
		const id = m[1] ?? m[4];
		const t = textOf(m[2] ?? m[5] ?? "");
		if (!t || /^(syntax|example|examples|properties|parameters)$/i.test(t)) continue;
		const key = t.toLowerCase();
		if (seen.has(key)) continue;
		seen.add(key);
		search.push({ t, p: page.title, u: pageUrl(page.slug) + "#" + id });
	}
}

function description(html: string): string {
	const m = html.match(/<p\b[^>]*>([\s\S]*?)<\/p>/);
	const t = m ? textOf(m[1]) : "";
	return t.length > 160 ? t.slice(0, 157).replace(/\s+\S*$/, "") + "..." : t;
}

// -------------------------------------------------------------- layout ---

const template = readFileSync(join(SRC, "layout.html"), "utf8");

function sidebar(active: Page): string {
	const activeGroup = active.parent ?? active.slug;
	const li = (it: MenuItem, depth: number): string => {
		const slug = slugOf(it.link);
		const cur = slug === active.slug ? ' aria-current="page"' : "";
		let sub = "";
		if (it.items && (slug === activeGroup)) {
			sub = `<ul>${it.items.map(c => li(c, depth + 1)).join("")}</ul>`;
		}
		return `<li><a href="${pageUrl(slug)}"${cur}>${esc(it.title)}</a>${sub}</li>`;
	};
	return `<ul>${menu.map(it => li(it, 0)).join("")}</ul>`;
}

function pager(i: number): string {
	const prev = pages[i - 1], next = pages[i + 1];
	const a = (p: Page, rel: string, label: string) =>
		`<a class="pager-${rel}" rel="${rel}" href="${pageUrl(p.slug)}"><span>${label}</span>${esc(p.title)}</a>`;
	return (prev ? a(prev, "prev", "Previous") : "<span></span>") + (next ? a(next, "next", "Next") : "<span></span>");
}

function render(vars: Record<string, string>): string {
	return template.replace(/\{\{(\w+)\}\}/g, (_, k) => {
		if (!(k in vars)) throw new Error(`layout.html: unknown placeholder {{${k}}}`);
		return vars[k];
	});
}

function pageVars(p: Partial<Record<string, string>>): Record<string, string> {
	return {
		base: BASE,
		title: "",
		description: "",
		canonical: "",
		sidebar: "",
		content: "",
		pager: "",
		edit: "",
		...p,
	} as Record<string, string>;
}

// --------------------------------------------------------------- build ---

rmSync(DIST, { recursive: true, force: true });
mkdirSync(DIST, { recursive: true });

const outputs: { slug: string; html: string }[] = [];

for (const [i, page] of pages.entries()) {
	let html = headingIds(stripBom(readFileSync(page.file, "utf8")));
	idsBySlug.set(page.slug, collectIds(html));
	collectHeadings(html, page);
	html = highlightBlocks(html, page.slug);
	html = themeImages(html);
	html = rewriteLinks(html, page);
	html = lazyImages(html);

	const source = relative(ROOT, page.file).replace(/\\/g, "/");
	const out = render(pageVars({
		title: `${esc(page.title)} · Nilesoft Shell`,
		description: esc(description(html)),
		canonical: SITE + pageUrl(page.slug),
		sidebar: sidebar(page),
		content: html,
		pager: pager(i),
		edit: `<a href="https://github.com/${REPO}/edit/${BRANCH}/${source}">Edit this page</a>`,
	}));
	outputs.push({ slug: page.slug, html: out });
}

for (const r of anchorRefs) {
	const ids = idsBySlug.get(r.target);
	if (ids && !ids.has(r.anchor)) {
		const msg = `${r.from || "index"}: missing anchor #${r.anchor}` + (r.target !== r.from ? ` on ${r.target || "index"}` : "");
		STRICT_ANCHORS ? fail(msg) : warn(msg);
	}
}

// Fragments not reachable from menu.json are not published.
for (const f of listFiles(DOCS, ".html")) {
	const slug = slugOf("/docs/" + relative(DOCS, f).replace(/\\/g, "/"));
	if (!bySlug.has(slug)) warn(`not in menu.json, skipped: docs/${relative(DOCS, f).replace(/\\/g, "/")}`);
}

if (errors.length) {
	for (const e of errors) console.error("error: " + e);
	for (const w of warnings) console.warn("warning: " + w);
	console.error(`\nbuild failed: ${errors.length} error(s)`);
	process.exit(1);
}

for (const o of outputs) {
	const dir = join(DIST, o.slug);
	mkdirSync(dir, { recursive: true });
	writeFileSync(join(dir, "index.html"), o.html);
}

// 404 page: links are absolute, so it works at any depth.
writeFileSync(join(DIST, "404.html"), render(pageVars({
	title: "Page not found · Nilesoft Shell",
	description: "Page not found.",
	canonical: SITE + BASE,
	sidebar: sidebar({ title: "", link: "", slug: "\u0000", file: "" }),
	content: `<h4>Page not found</h4><p>There is no page at this address. It may have moved when the documentation was reorganised.</p><p>Try the search above, or start from the <a href="${BASE}">introduction</a>.</p>`,
})));

// Static files.
cpSync(join(DOCS, "images"), join(DIST, "images"), { recursive: true });
mkdirSync(join(DIST, "assets"), { recursive: true });
for (const f of ["logo-32.png", "logo-128.png", "logo-256.png"]) cpSync(join(ASSETS, f), join(DIST, "assets", f));
for (const f of readdirSync(join(SRC, "assets"))) {
	let s = readFileSync(join(SRC, "assets", f), "utf8");
	writeFileSync(join(DIST, "assets", f), s.replaceAll("{{base}}", BASE));
}
writeFileSync(join(DIST, "search.json"), JSON.stringify(search));
writeFileSync(join(DIST, ".nojekyll"), "");

const sitemap = `<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n` +
	pages.map(p => `  <url><loc>${SITE}${pageUrl(p.slug)}</loc></url>`).join("\n") + `\n</urlset>\n`;
writeFileSync(join(DIST, "sitemap.xml"), sitemap);
writeFileSync(join(DIST, "robots.txt"), `User-agent: *\nAllow: /\nSitemap: ${SITE}${BASE}sitemap.xml\n`);

for (const w of warnings) console.warn("warning: " + w);
console.log(`built ${pages.length} pages, ${search.length} search entries, ${imagesUsed.size} images referenced, ` +
	`${highlighted} code blocks highlighted (${skippedHighlight} with inline markup left as is), ` +
	`${warnings.length} warning(s) -> ${relative(process.cwd(), DIST) || "."} (BASE=${BASE})`);

function listFiles(dir: string, ext: string): string[] {
	const out: string[] = [];
	for (const e of readdirSync(dir, { withFileTypes: true })) {
		const p = join(dir, e.name);
		if (e.isDirectory()) out.push(...listFiles(p, ext));
		else if (e.name.endsWith(ext)) out.push(p);
	}
	return out;
}
