#include "CodeDocument.h"

#include <algorithm>
#include <chrono>

namespace code {

namespace {

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

size_t utf8Length(char32_t c) { return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; }

std::string utf8(char32_t c)
{
    std::string out;
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
    return out;
}

bool at(std::string_view s, size_t pos, std::string_view what)
{
    return pos <= s.size() && s.substr(pos, what.size()) == what;
}

bool oneCodePoint(std::string_view s)
{
    if (s.empty())
        return false;
    size_t n = 1;
    while (n < s.size() && continuation(s[n]))
        ++n;
    return n == s.size();
}

bool blank(char c) { return c == ' ' || c == '\t'; }

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

int64_t steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Typed characters and Backspace/Delete runs merge while they come this close together.
constexpr int64_t kMergeMs = 2000;
constexpr size_t kMaxUndo = 2000;

// One replacement at a line start, for mapping offsets through an indent or outdent.
struct LineEdit
{
    size_t start;   // old offset
    size_t removed; // bytes removed at `start`
    size_t added;   // bytes inserted at `start`
};

size_t mapOffset(size_t o, const std::vector<LineEdit> &edits)
{
    long long delta = 0;
    for (const auto &e : edits) {
        if (o <= e.start)
            break;
        if (o >= e.start + e.removed) {
            delta += (long long)e.added - (long long)e.removed;
        } else {
            // Inside removed text: to the end of what replaced it.
            return size_t((long long)e.start + delta + (long long)e.added);
        }
    }
    return size_t((long long)o + delta);
}

} // namespace

std::string normalizeNewlines(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
        } else {
            out += text[i];
        }
    }
    return out;
}

Document::Document(std::string text, int tabWidth) : tabWidth_(std::max(1, tabWidth)) { reset(std::move(text)); }

void Document::reset(std::string text)
{
    text_ = std::move(text);
    glyphs_ = hl::pickTabGlyphs(text_);
    detectIndent();
    refreshDisplay();
    undo_.clear();
    redo_.clear();
    view = {};
}

void Document::refreshDisplay() { display_ = hl::tabsToDisplay(text_, glyphs_, tabWidth_); }

void Document::detectIndent()
{
    // Tabs unless more lines are indented with spaces; then 2 or 4 of them.
    size_t tabs = 0, spaces = 0, two = 0;
    for (size_t ls = 0; ls < text_.size();) {
        size_t le = text_.find('\n', ls);
        if (le == std::string::npos)
            le = text_.size();
        if (ls < le && text_[ls] == '\t') {
            ++tabs;
        } else if (ls < le && text_[ls] == ' ') {
            size_t n = 0;
            while (ls + n < le && text_[ls + n] == ' ')
                ++n;
            if (ls + n < le && text_[ls + n] != '\t') {
                ++spaces;
                if (n % 4 != 0 && n % 2 == 0)
                    ++two;
            }
        }
        ls = le + 1;
    }
    if (spaces > tabs)
        unit_ = std::string(two > 0 ? 2 : 4, ' ');
    else
        unit_ = "\t";
}

size_t Document::toDisplay(size_t t) const
{
    t = std::min(t, text_.size());
    if (!glyphs_.valid())
        return t;
    const size_t lead = utf8Length(glyphs_.lead), pad = utf8Length(glyphs_.pad);
    size_t extra = 0;
    int column = 0;
    for (size_t i = 0; i < t; ++i) {
        char c = text_[i];
        if (c == '\t') {
            int n = tabWidth_ - column % tabWidth_;
            extra += lead + size_t(n - 1) * pad - 1;
            column += n;
        } else if (c == '\n') {
            column = 0;
        } else if (!continuation(c)) {
            ++column;
        }
    }
    return t + extra;
}

size_t Document::toText(size_t d) const
{
    if (!glyphs_.valid())
        return std::min(d, text_.size());
    const size_t lead = utf8Length(glyphs_.lead), pad = utf8Length(glyphs_.pad);
    size_t pos = 0;
    int column = 0;
    for (size_t i = 0; i < text_.size(); ++i) {
        if (pos >= d)
            return i;
        char c = text_[i];
        if (c == '\t') {
            int n = tabWidth_ - column % tabWidth_;
            size_t width = lead + size_t(n - 1) * pad;
            if (d < pos + width)
                return i + 1; // inside the tab's glyphs
            pos += width;
            column += n;
        } else {
            ++pos;
            if (c == '\n')
                column = 0;
            else if (!continuation(c))
                ++column;
        }
    }
    return text_.size();
}

void Document::applyRaw(size_t pos, size_t removeLen, std::string_view insert)
{
    pos = std::min(pos, text_.size());
    removeLen = std::min(removeLen, text_.size() - pos);
    bool repick = false;
    if (glyphs_.valid())
        repick = insert.find(utf8(glyphs_.lead)) != std::string_view::npos ||
                 insert.find(utf8(glyphs_.pad)) != std::string_view::npos;
    if (repick) {
        // The new text has the characters that show tabs: show them with another pair.
        text_.replace(pos, removeLen, insert);
        glyphs_ = hl::pickTabGlyphs(text_);
        refreshDisplay();
        return;
    }
    // Only the lines the change touches are shown differently.
    size_t ls = pos == 0 ? 0 : text_.rfind('\n', pos - 1);
    ls = ls == std::string::npos ? 0 : (pos == 0 ? 0 : ls + 1);
    size_t le = text_.find('\n', pos + removeLen);
    if (le == std::string::npos)
        le = text_.size();
    const size_t dls = toDisplay(ls);
    const size_t dlen = hl::tabsToDisplay(std::string_view(text_).substr(ls, le - ls), glyphs_, tabWidth_).size();
    text_.replace(pos, removeLen, insert);
    const size_t newLe = le - removeLen + insert.size();
    display_.replace(dls, dlen, hl::tabsToDisplay(std::string_view(text_).substr(ls, newLe - ls), glyphs_, tabWidth_));
}

Result Document::commit(Change c, int64_t nowMs, const std::string *shown)
{
    const int64_t now = nowMs ? nowMs : steadyMs();
    applyRaw(c.pos, c.removed.size(), c.inserted);
    c.time = now;
    redo_.clear();

    bool merged = false;
    if (!undo_.empty() && c.kind != Kind::Other) {
        auto &last = undo_.back();
        const bool recent = last.open && now - last.time >= 0 && now - last.time < kMergeMs;
        if (recent && last.kind == Kind::Typing && c.kind == Kind::Typing && c.removed.empty() &&
            c.pos == last.pos + last.inserted.size()) {
            // A new step starts at each word: "foo bar" undoes as "bar", then "foo ".
            const bool wordStart = !last.inserted.empty() && blank(last.inserted.back()) && !blank(c.inserted[0]);
            if (!wordStart) {
                last.inserted += c.inserted;
                last.after = c.after;
                last.time = now;
                merged = true;
            }
        } else if (recent && last.kind == Kind::Delete && c.kind == Kind::Delete) {
            if (c.pos + c.removed.size() == last.pos) { // Backspace
                last.removed = c.removed + last.removed;
                last.pos = c.pos;
                last.after = c.after;
                last.time = now;
                merged = true;
            } else if (c.pos == last.pos) { // Delete
                last.removed += c.removed;
                last.after = c.after;
                last.time = now;
                merged = true;
            }
        }
    }
    if (!merged) {
        closeGroup();
        c.open = c.kind != Kind::Other;
        undo_.push_back(std::move(c));
        if (undo_.size() > kMaxUndo)
            undo_.erase(undo_.begin());
    }

    Result r;
    r.edited = true;
    r.display = !shown || *shown != display_;
    r.select = r.display;
    r.selection = toDisplay(undo_.back().after);
    return r;
}

void Document::closeGroup()
{
    if (!undo_.empty())
        undo_.back().open = false;
}

Result Document::input(std::string_view shown, size_t cursor, Selection before, int64_t nowMs)
{
    const std::string_view o = display_, n = shown;
    if (o == n)
        return {};
    cursor = std::min(cursor, n.size());

    // The changed range: old [a, b) became new [a, n.size() - s). When that is
    // ambiguous ("a" typed after "a"), it ends at the caret.
    size_t s = 0;
    const size_t maxS = std::min({ o.size(), n.size(), n.size() - cursor });
    while (s < maxS && o[o.size() - 1 - s] == n[n.size() - 1 - s])
        ++s;
    while (s > 0 && continuation(o[o.size() - s]))
        --s;
    size_t p = 0;
    const size_t maxP = std::min(o.size(), n.size()) - s;
    while (p < maxP && o[p] == n[p])
        ++p;
    while (p > 0 && ((p < o.size() && continuation(o[p])) || (p < n.size() && continuation(n[p]))))
        --p;
    size_t a = p, b = o.size() - s;
    const std::string_view inserted = n.substr(p, n.size() - s - p);

    if (glyphs_.valid() && b > a) {
        // Removing part of a tab's glyphs removes the whole tab.
        const auto lead = utf8(glyphs_.lead), pad = utf8(glyphs_.pad);
        if (at(o, a, pad)) {
            while (a >= pad.size() && at(o, a - pad.size(), pad))
                a -= pad.size();
            if (a >= lead.size() && at(o, a - lead.size(), lead))
                a -= lead.size();
        }
        if ((b >= lead.size() && at(o, b - lead.size(), lead)) || (b >= pad.size() && at(o, b - pad.size(), pad)))
            while (b < o.size() && at(o, b, pad))
                b += pad.size();
    }

    const size_t ta = toText(a), tb = std::max(ta, toText(b));
    Change c;
    c.pos = ta;
    c.removed = text_.substr(ta, tb - ta);
    c.inserted = hl::tabsFromDisplay(inserted, glyphs_);
    if (c.removed.empty() && c.inserted.empty()) {
        // Only glyphs inside a tab moved (a pad typed over): show the canonical text again.
        Result r;
        r.display = true;
        r.select = true;
        r.selection = { toDisplay(ta), toDisplay(ta) };
        return r;
    }

    // The selection before the change, for undo; checked against the change.
    const Selection bt = toText(before);
    const size_t rl = c.removed.size();
    if ((bt.begin() == ta && bt.end() == ta + rl && !bt.empty()) || (bt.empty() && (bt.cursor == ta || bt.cursor == ta + rl)))
        c.before = bt;
    else if (rl > 0 && !c.inserted.empty())
        c.before = { ta, ta + rl };
    else
        c.before = { ta + rl, ta + rl };
    c.after = { ta + c.inserted.size(), ta + c.inserted.size() };

    if (oneCodePoint(c.inserted) && c.inserted != "\n")
        c.kind = Kind::Typing;
    else if (c.inserted.empty() && oneCodePoint(c.removed))
        c.kind = Kind::Delete;
    else
        c.kind = Kind::Other;

    const std::string shownCopy(shown);
    return commit(std::move(c), nowMs, &shownCopy);
}

Result Document::replace(Selection sel, std::string_view text)
{
    const Selection t = toText(sel);
    Change c;
    c.pos = t.begin();
    c.removed = text_.substr(t.begin(), t.end() - t.begin());
    c.inserted = std::string(text);
    if (c.removed.empty() && c.inserted.empty())
        return {};
    c.before = t;
    c.after = { c.pos + c.inserted.size(), c.pos + c.inserted.size() };
    c.kind = Kind::Other;
    return commit(std::move(c), 0, nullptr);
}

Result Document::paste(Selection sel, std::string_view clipboard) { return replace(sel, normalizeNewlines(clipboard)); }

Result Document::cut(Selection sel)
{
    if (sel.empty())
        return {};
    return replace(sel, {});
}

Result Document::newline(Selection sel)
{
    const Selection t = toText(sel);
    const size_t b = t.begin(), e = t.end();
    size_t ls = b == 0 ? 0 : text_.rfind('\n', b - 1);
    ls = (b == 0 || ls == std::string::npos) ? 0 : ls + 1;
    size_t k = ls;
    while (k < b && blank(text_[k]))
        ++k;
    const std::string indent = text_.substr(ls, k - ls);

    std::string ins = "\n" + indent;
    // After an opening brace, one level deeper; "{|}" puts the brace on its own line.
    size_t q = b;
    while (q > ls && blank(text_[q - 1]))
        --q;
    size_t caret;
    if (q > ls && text_[q - 1] == '{') {
        ins += unit_;
        caret = b + ins.size();
        if (e < text_.size() && text_[e] == '}')
            ins += "\n" + indent;
    } else {
        caret = b + ins.size();
    }
    Change c;
    c.pos = b;
    c.removed = text_.substr(b, e - b);
    c.inserted = std::move(ins);
    c.before = t;
    c.after = { caret, caret };
    c.kind = Kind::Other;
    return commit(std::move(c), 0, nullptr);
}

std::pair<size_t, size_t> Document::lineSpan(Selection t) const
{
    const size_t b = std::min(t.begin(), text_.size());
    size_t end = std::min(t.end(), text_.size());
    size_t ls = b == 0 ? 0 : text_.rfind('\n', b - 1);
    ls = (b == 0 || ls == std::string::npos) ? 0 : ls + 1;
    // A selection ending at the start of a line doesn't take that line.
    if (end > b && text_[end - 1] == '\n')
        --end;
    size_t le = text_.find('\n', end);
    if (le == std::string::npos)
        le = text_.size();
    return { ls, std::max(ls, le) };
}

Result Document::tab(Selection sel)
{
    const Selection t = toText(sel);
    const bool lines = !t.empty() && text_.find('\n', t.begin()) < t.end();
    if (!lines) {
        std::string ins = unit_;
        if (unit_ != "\t") {
            // Spaces up to the next indent stop.
            size_t ls = t.begin() == 0 ? 0 : text_.rfind('\n', t.begin() - 1);
            ls = (t.begin() == 0 || ls == std::string::npos) ? 0 : ls + 1;
            int column = 0;
            for (size_t i = ls; i < t.begin(); ++i)
                if (text_[i] == '\t')
                    column += tabWidth_ - column % tabWidth_;
                else if (!continuation(text_[i]))
                    ++column;
            const int w = int(unit_.size());
            ins = std::string(size_t(w - column % w), ' ');
        }
        return replace(sel, ins);
    }

    auto [lb, le] = lineSpan(t);
    std::string block;
    std::vector<LineEdit> edits;
    for (size_t ls = lb; ls <= le;) {
        size_t e = text_.find('\n', ls);
        if (e == std::string::npos || e > le)
            e = le;
        if (e > ls) { // empty lines stay empty
            block += unit_;
            edits.push_back({ ls, 0, unit_.size() });
        }
        block.append(text_, ls, e - ls);
        if (e >= le)
            break;
        block += '\n';
        ls = e + 1;
    }
    if (edits.empty())
        return {};
    Change c;
    c.pos = lb;
    c.removed = text_.substr(lb, le - lb);
    c.inserted = std::move(block);
    c.before = t;
    c.after = { mapOffset(t.anchor, edits), mapOffset(t.cursor, edits) };
    c.kind = Kind::Other;
    return commit(std::move(c), 0, nullptr);
}

Result Document::outdent(Selection sel)
{
    const Selection t = toText(sel);
    auto [lb, le] = lineSpan(t);
    std::string block;
    std::vector<LineEdit> edits;
    for (size_t ls = lb; ls <= le;) {
        size_t e = text_.find('\n', ls);
        if (e == std::string::npos || e > le)
            e = le;
        size_t r = 0;
        if (ls < e && text_[ls] == '\t') {
            r = 1;
        } else {
            while (ls + r < e && r < size_t(tabWidth_) && text_[ls + r] == ' ')
                ++r;
            if (r > 0 && r < size_t(tabWidth_) && ls + r < e && text_[ls + r] == '\t')
                ++r; // "  \t": the spaces and the tab are one level
        }
        if (r > 0)
            edits.push_back({ ls, r, 0 });
        block.append(text_, ls + r, e - ls - r);
        if (e >= le)
            break;
        block += '\n';
        ls = e + 1;
    }
    if (edits.empty())
        return {};
    Change c;
    c.pos = lb;
    c.removed = text_.substr(lb, le - lb);
    c.inserted = std::move(block);
    c.before = t;
    c.after = { mapOffset(t.anchor, edits), mapOffset(t.cursor, edits) };
    c.kind = Kind::Other;
    return commit(std::move(c), 0, nullptr);
}

Result Document::selectText(Selection textSel) const
{
    Result r;
    r.edited = true;
    r.display = true;
    r.select = true;
    r.selection = toDisplay(textSel);
    return r;
}

Result Document::undo()
{
    if (undo_.empty())
        return {};
    Change c = std::move(undo_.back());
    undo_.pop_back();
    applyRaw(c.pos, c.inserted.size(), c.removed);
    c.open = false;
    auto r = selectText(c.before);
    redo_.push_back(std::move(c));
    return r;
}

Result Document::redo()
{
    if (redo_.empty())
        return {};
    Change c = std::move(redo_.back());
    redo_.pop_back();
    applyRaw(c.pos, c.removed.size(), c.inserted);
    c.open = false;
    auto r = selectText(c.after);
    undo_.push_back(std::move(c));
    return r;
}

std::string Document::selectedText(Selection sel) const
{
    const Selection t = toText(sel);
    return text_.substr(t.begin(), t.end() - t.begin());
}

std::optional<Document::Match> Document::find(std::string_view query, Selection from, bool backwards,
                                               bool incremental) const
{
    if (query.empty() || query.size() > text_.size())
        return std::nullopt;
    std::vector<size_t> hits;
    for (size_t i = 0; i + query.size() <= text_.size(); ++i) {
        size_t k = 0;
        while (k < query.size() && lower(text_[i + k]) == lower(query[k]))
            ++k;
        if (k == query.size())
            hits.push_back(i);
    }
    if (hits.empty())
        return std::nullopt;
    const Selection t = toText(from);
    size_t pick = 0;
    if (!backwards) {
        const size_t start = incremental ? t.begin() : t.end();
        auto it = std::lower_bound(hits.begin(), hits.end(), start);
        pick = it == hits.end() ? 0 : size_t(it - hits.begin());
    } else {
        auto it = std::lower_bound(hits.begin(), hits.end(), t.begin());
        pick = it == hits.begin() ? hits.size() - 1 : size_t(it - hits.begin()) - 1;
    }
    Match m;
    m.selection = { toDisplay(hits[pick]), toDisplay(hits[pick] + query.size()) };
    m.index = int(pick) + 1;
    m.count = int(hits.size());
    return m;
}

} // namespace code
