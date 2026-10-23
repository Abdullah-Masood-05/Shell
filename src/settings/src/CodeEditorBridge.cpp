#include "CodeEditorBridge.h"

#include "nss/Text.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace codeui {

namespace {

slint::SharedString ss(std::string_view s) { return slint::SharedString(s); }

} // namespace

code::Selection selection(int anchor, int cursor)
{
    return { size_t(std::max(0, anchor)), size_t(std::max(0, cursor)) };
}

EditResult toSlint(const code::Result &r)
{
    return EditResult{ r.select, int(r.selection.anchor), int(r.selection.cursor) };
}

std::optional<std::string> readClipboard()
{
    if (!OpenClipboard(nullptr))
        return std::nullopt;
    std::optional<std::string> out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
        if (auto *w = static_cast<const wchar_t *>(GlobalLock(h))) {
            out = nss::narrow(w);
            GlobalUnlock(h);
        }
    CloseClipboard();
    return out;
}

bool writeClipboard(std::string_view text)
{
    std::string crlf;
    crlf.reserve(text.size() + text.size() / 16);
    for (char c : text) {
        if (c == '\n')
            crlf += '\r';
        crlf += c;
    }
    auto wide = nss::widen(crlf);
    if (!OpenClipboard(nullptr))
        return false;
    bool ok = false;
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t))) {
        if (auto *dst = static_cast<wchar_t *>(GlobalLock(mem))) {
            memcpy(dst, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            EmptyClipboard();
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok)
            GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

code::Result command(code::Document &doc, std::string_view name, int anchor, int cursor, bool readOnly)
{
    const auto sel = selection(anchor, cursor);
    if (name == "copy" || name == "cut") {
        if (sel.empty())
            return {};
        writeClipboard(doc.selectedText(sel));
        return name == "cut" && !readOnly ? doc.cut(sel) : code::Result{};
    }
    if (readOnly)
        return {};
    if (name == "paste") {
        auto clip = readClipboard();
        return clip && !clip->empty() ? doc.paste(sel, *clip) : code::Result{};
    }
    if (name == "undo")
        return doc.undo();
    if (name == "redo")
        return doc.redo();
    if (name == "tab")
        return doc.tab(sel);
    if (name == "outdent")
        return doc.outdent(sel);
    if (name == "newline")
        return doc.newline(sel);
    return {};
}

FindResult find(const code::Document &doc, std::string_view query, int anchor, int cursor, bool backwards,
                bool incremental)
{
    auto m = doc.find(query, selection(anchor, cursor), backwards, incremental);
    if (!m)
        return FindResult{ false, anchor, cursor, 0, 0 };
    return FindResult{ true, int(m->selection.anchor), int(m->selection.cursor), m->index, m->count };
}

std::string findSeed(const code::Document &doc, int anchor, int cursor)
{
    auto s = doc.selectedText(selection(anchor, cursor));
    return s.find('\n') == std::string::npos && s.size() <= 200 ? s : std::string{};
}

Rows::Rows() : model_(std::make_shared<slint::VectorModel<CodeLine>>()) { }

void Rows::clear() { update({}, {}); }

// Each span is drawn as the line up to its end, clipped to the span, so every
// glyph sits where the TextInput (one run per line) puts it.
CodeLine Rows::row(size_t i) const
{
    auto spans = std::make_shared<slint::VectorModel<CodeSpan>>();
    std::string prefix;
    const auto &ls = lines_[i].spans;
    for (size_t k = 0; k < ls.size(); ++k) {
        spans->push_back(CodeSpan{ ss(ls[k].text), ls[k].kind, ss(prefix), k + 1 == ls.size() });
        prefix += ls[k].text;
    }
    return CodeLine{ 0, spans, marks_[i] }; // the page numbers edit rows by index
}

void Rows::update(std::vector<hl::Line> lines, std::vector<int> marks)
{
    marks.resize(lines.size(), 0);
    const size_t o = lines_.size(), n = lines.size();
    size_t pre = 0, suf = 0;
    while (pre < o && pre < n && lines[pre] == lines_[pre] && marks[pre] == marks_[pre])
        ++pre;
    while (suf < o - pre && suf < n - pre && lines[n - 1 - suf] == lines_[o - 1 - suf] &&
           marks[n - 1 - suf] == marks_[o - 1 - suf])
        ++suf;
    const size_t oldMid = o - pre - suf, newMid = n - pre - suf, common = std::min(oldMid, newMid);
    lines_ = std::move(lines);
    marks_ = std::move(marks);
    for (size_t k = 0; k < common; ++k)
        model_->set_row_data(pre + k, row(pre + k));
    for (size_t k = common; k < newMid; ++k)
        model_->insert(pre + k, row(pre + k));
    for (size_t k = oldMid; k > common; --k)
        model_->erase(pre + k - 1);
}

void Rows::patch(hl::LinePatch p)
{
    if (p.first > lines_.size())
        return;
    const size_t oldCount = std::min(p.oldCount, lines_.size() - p.first), newCount = p.lines.size();
    const size_t common = std::min(oldCount, newCount);
    // Model rows first erased/inserted, then the vectors follow the same shape.
    std::vector<int> marks(newCount, 0);
    for (size_t k = 0; k < common; ++k)
        marks[k] = marks_[p.first + k];
    lines_.erase(lines_.begin() + ptrdiff_t(p.first), lines_.begin() + ptrdiff_t(p.first + oldCount));
    lines_.insert(lines_.begin() + ptrdiff_t(p.first), std::make_move_iterator(p.lines.begin()),
                  std::make_move_iterator(p.lines.end()));
    marks_.erase(marks_.begin() + ptrdiff_t(p.first), marks_.begin() + ptrdiff_t(p.first + oldCount));
    marks_.insert(marks_.begin() + ptrdiff_t(p.first), marks.begin(), marks.end());
    for (size_t k = 0; k < common; ++k)
        model_->set_row_data(p.first + k, row(p.first + k));
    for (size_t k = common; k < newCount; ++k)
        model_->insert(p.first + k, row(p.first + k));
    for (size_t k = oldCount; k > common; --k)
        model_->erase(p.first + k - 1);
}

} // namespace codeui
