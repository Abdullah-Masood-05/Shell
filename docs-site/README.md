# Docs site

Builds the GitHub Pages site (https://abdullah-masood-05.github.io/Shell/) from the HTML fragments in `../docs`.
The fragments and `../docs/menu.json` stay the source of truth, shared with upstream; this folder only adds the layout,
styles and build.

Needs [bun](https://bun.sh) 1.4 or later.

```sh
cd docs-site
bun install
bun run build     # writes dist/ (fails on broken internal links or missing images)
bun run serve     # http://127.0.0.1:4000/Shell/
bun run dev       # build, serve, and rebuild when docs/ or src/ change
bun run check     # build, then check every link and #anchor in dist/
```

- `BASE` sets the path the site is served under (default `/Shell/`). Use `BASE=/` for a root deployment. `serve` and
  `check` read the same variable.
- `PORT` changes the preview port. `SITE`, `REPO` and `BRANCH` set the sitemap origin and the "Edit this page" links.
- To add a page, write a fragment in `../docs` in the same style as the others and add it to `../docs/menu.json`.
  Pages not in the menu are not published.
- Links to `/docs/...` and images under `/docs/images/...` are rewritten to `BASE`. A screenshot in `images/dark/` with
  a namesake in `images/light/` is shown according to the reader's theme.
- Code blocks marked `<code class="lang-shell">` are highlighted at build time.

Files: `build.ts` (the build), `src/layout.html` (page template), `src/assets/` (CSS and JS), `serve.ts` (local
preview), `check.ts` (link checker). `.github/workflows/docs.yml` builds and deploys on pushes to `dev` and `main`.
