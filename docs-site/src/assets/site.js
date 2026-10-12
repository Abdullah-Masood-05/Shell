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
	document.addEventListener("keydown", function (e) { if (e.key === "Escape") setNav(false); });
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
