// Theme toggle, mobile navigation and search. No dependencies.
(function () {
	var root = document.documentElement;
	var BASE = "{{base}}";

	// Theme: follows the system until the toggle is used, then remembered.
	var dark = matchMedia("(prefers-color-scheme: dark)");
	function current() { return root.dataset.theme || (dark.matches ? "dark" : "light"); }
	var toggle = document.querySelector(".theme-toggle");
	function label() { toggle.setAttribute("aria-label", "Switch to " + (current() === "dark" ? "light" : "dark") + " theme"); toggle.title = toggle.getAttribute("aria-label"); }
	toggle.addEventListener("click", function () {
		var next = current() === "dark" ? "light" : "dark";
		root.dataset.theme = next;
		try { localStorage.setItem("theme", next); } catch (e) {}
		label();
	});
	dark.addEventListener("change", label);
	label();

	// Navigation drawer on narrow screens.
	var navBtn = document.querySelector(".nav-toggle");
	function setNav(open) { document.body.classList.toggle("nav-open", open); navBtn.setAttribute("aria-expanded", String(open)); }
	navBtn.addEventListener("click", function () { setNav(!document.body.classList.contains("nav-open")); });
	document.addEventListener("keydown", function (e) {
		if (e.key !== "Escape" || !document.body.classList.contains("nav-open")) return;
		var inside = document.getElementById("sidebar").contains(document.activeElement);
		setNav(false);
		if (inside) navBtn.focus();
	});
	var active = document.querySelector('.sidebar [aria-current="page"]');
	if (active) active.scrollIntoView({ block: "nearest" });

	// Search over page titles and headings (search.json, loaded on first use).
	var input = document.getElementById("search");
	var list = document.getElementById("search-results");
	var index = null, results = [], sel = -1;
	function load() {
		if (index) return;
		index = [];
		fetch(BASE + "search.json").then(function (r) { return r.json(); }).then(function (d) { index = d; run(); });
	}
	function esc(s) { return s.replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; }); }
	function run() {
		var q = input.value.trim().toLowerCase();
		var terms = q.split(/\s+/).filter(Boolean);
		if (!terms.length) { close(); return; }
		results = index.map(function (e) {
			var t = e.t.toLowerCase(), hay = t + " " + e.p.toLowerCase(), s = 0;
			for (var i = 0; i < terms.length; i++) { if (hay.indexOf(terms[i]) < 0) return null; }
			if (t === q) s += 100; else if (t.indexOf(q) === 0) s += 50; else if (t.indexOf(q) >= 0) s += 20;
			if (!e.p) s += 10;
			return { e: e, s: s - t.length / 100 };
		}).filter(Boolean).sort(function (a, b) { return b.s - a.s; }).slice(0, 12);
		sel = results.length ? 0 : -1;
		list.innerHTML = results.length
			? results.map(function (r, i) {
				return '<li role="option" aria-selected="' + (i === sel) + '"><a href="' + esc(r.e.u) + '">' + esc(r.e.t) + (r.e.p ? "<small>" + esc(r.e.p) + "</small>" : "") + "</a></li>";
			}).join("")
			: '<li class="empty">No matches</li>';
		list.hidden = false;
		input.setAttribute("aria-expanded", "true");
	}
	function close() { list.hidden = true; input.setAttribute("aria-expanded", "false"); }
	function move(d) {
		if (!results.length) return;
		sel = (sel + d + results.length) % results.length;
		[].forEach.call(list.children, function (li, i) { li.setAttribute("aria-selected", String(i === sel)); });
		list.children[sel].scrollIntoView({ block: "nearest" });
	}
	input.addEventListener("focus", load);
	input.addEventListener("input", function () { load(); run(); });
	input.addEventListener("keydown", function (e) {
		if (e.key === "ArrowDown") { e.preventDefault(); move(1); }
		else if (e.key === "ArrowUp") { e.preventDefault(); move(-1); }
		else if (e.key === "Enter" && sel >= 0) { location.href = results[sel].e.u; close(); }
		else if (e.key === "Escape") { input.value = ""; close(); input.blur(); }
	});
	// Keep focus in the input while a result is clicked.
	list.addEventListener("mousedown", function (e) { e.preventDefault(); });
	input.addEventListener("blur", close);
	document.addEventListener("keydown", function (e) {
		if (e.key === "/" && document.activeElement !== input && !/INPUT|TEXTAREA/.test(document.activeElement.tagName)) { e.preventDefault(); input.focus(); }
	});
})();

// Download links. The pages name a fixed release, so they still work when GitHub can't be reached;
// this points them at the newest one.
//   a[data-latest-beta="file"]   the file in the newest beta (empty: the beta's release page)
//   a[data-latest-beta-text]     its text becomes the beta's version
//   [data-latest-beta-version]   its text becomes the beta's version
//   [data-latest-stable-version="Version {v}"]  its text becomes this, with {v} the stable version
//   .beta-offer                  hidden when the newest stable is newer than the newest beta
(function () {
	var sel = "a[data-latest-beta], [data-latest-beta-version], [data-latest-stable-version], .beta-offer";
	if (!document.querySelector(sel) || !window.fetch) return;
	fetch("https://api.github.com/repos/Abdullah-Masood-05/Shell/releases?per_page=20")
		.then(function (r) { return r.ok ? r.json() : []; })
		.then(function (list) {
			var beta = null, stable = null, stableFirst = false;
			for (var i = 0; i < list.length; i++) {
				var r = list[i];
				if (r.draft) continue;
				if (r.prerelease) { if (!beta) beta = r; }
				else if (!stable) { stable = r; if (!beta) stableFirst = true; }
			}
			var each = function (q, f) { [].forEach.call(document.querySelectorAll(q), f); };
			var version = function (r) { return r.tag_name.replace(/^v/, ""); };
			if (stable) each("[data-latest-stable-version]", function (el) {
				el.textContent = (el.getAttribute("data-latest-stable-version") || "{v}").replace("{v}", version(stable));
			});
			if (stableFirst) each(".beta-offer", function (el) { el.hidden = true; });
			if (!beta) return;
			each("[data-latest-beta-version]", function (el) { el.textContent = version(beta); });
			each("a[data-latest-beta]", function (a) {
				var file = a.getAttribute("data-latest-beta");
				if (!file) {
					a.href = beta.html_url;
					if (a.hasAttribute("data-latest-beta-text")) a.textContent = version(beta);
					return;
				}
				for (var j = 0; j < beta.assets.length; j++)
					if (beta.assets[j].name === file) { a.href = beta.assets[j].browser_download_url; return; }
			});
		})
		.catch(function () {});
})();

// 404 page: offer the page whose address matches the one asked for, such as an old
// /docs/... link or one ending in .html.
(function () {
	var box = document.querySelector(".suggest");
	if (!box || !window.fetch) return;
	var BASE = "{{base}}";
	var path = decodeURIComponent(location.pathname);
	if (path.indexOf(BASE) === 0) path = path.slice(BASE.length);
	var want = path.replace(/^\/+/, "").replace(/^docs(\/|$)/, "").replace(/(\/index)?(\.html?)?\/*$/, "").toLowerCase();
	var last = want.split("/").pop();
	fetch(BASE + "search.json").then(function (r) { return r.json(); }).then(function (index) {
		var pages = index.filter(function (e) { return !e.p; });
		var slug = function (e) { return e.u.slice(BASE.length).replace(/\/$/, "").toLowerCase(); };
		var hit = pages.filter(function (e) { return slug(e) === want; })[0]
			|| (last && pages.filter(function (e) { return slug(e).split("/").pop() === last; })[0]);
		if (!hit) return;
		var a = document.createElement("a");
		a.href = hit.u; a.textContent = hit.t;
		box.textContent = "Were you looking for ";
		box.appendChild(a);
		box.appendChild(document.createTextNode("?"));
		box.hidden = false;
	}).catch(function () {});
})();
