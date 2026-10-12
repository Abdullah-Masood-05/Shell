#include "nss/OverridesWriter.h"
#include "nss/ConfigGraph.h"
#include "nss/ThemeSchema.h"

#include <algorithm>
#include <vector>

namespace nss
{
	namespace
	{
		std::string normalizePath(std::string_view p)
		{
			auto s = toLower(trim(p));
			std::replace(s.begin(), s.end(), '/', '\\');
			while(s.rfind(".\\", 0) == 0)
				s.erase(0, 2);
			return s;
		}
	}

	bool OverridesWriter::samePath(std::string_view importValue, std::string_view relPath)
	{
		auto a = normalizePath(importValue), b = normalizePath(relPath);
		if(a == b)
			return true;
		return a.size() > b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0 && a[a.size() - b.size() - 1] == '\\';
	}

	std::string OverridesWriter::generate(const Overrides &overrides, Eol eol)
	{
		auto nl = std::string(eolString(eol));
		std::vector<std::pair<std::string, std::string>> sorted(overrides.begin(), overrides.end());
		for(auto &kv : sorted)
			kv.first = ThemeSchema::canonical(kv.first);
		std::stable_sort(sorted.begin(), sorted.end(), [](auto &a, auto &b)
		{
			auto oa = ThemeSchema::order(a.first), ob = ThemeSchema::order(b.first);
			return oa != ob ? oa < ob : a.first < b.first;
		});

		std::string out(header);
		out += nl;
		for(std::string section : { "theme", "settings" })
		{
			std::string body;
			for(auto &[key, value] : sorted)
			{
				if(key.size() <= section.size() + 1 || key.compare(0, section.size() + 1, section + ".") != 0)
					continue;
				body += "\t" + key.substr(section.size() + 1) + " = " + std::string(trim(value)) + nl;
			}
			if(body.empty())
				continue;
			out += nl + section + nl + "{" + nl + body + "}" + nl;
		}
		return out;
	}

	NssDocument OverridesWriter::generateDocument(const Overrides &overrides, const NssDocument *existing)
	{
		auto format = existing ? existing->format() : defaultFormat();
		return NssDocument::fromText(generate(overrides, format.eol), format);
	}

	Overrides OverridesWriter::read(const NssDocument &doc)
	{
		Overrides out;
		for(auto n : doc.assigns())
			out[ThemeSchema::canonical(n->path)] = std::string(trim(doc.slice(n->valueSpan)));
		return out;
	}

	bool OverridesWriter::ensureImportedLast(NssDocument &root, std::string_view relPath)
	{
		auto matchesAt = [&]()
		{
			std::vector<size_t> found;
			auto imps = root.imports();
			for(size_t k = 0; k < imps.size(); k++)
			{
				if(imps[k]->importKind != ImportKind::Config)
					continue;
				auto v = ConfigGraph::evaluate(root.slice(imps[k]->valueSpan), {});
				if(v && samePath(*v, relPath))
					found.push_back(k);
			}
			return found;
		};

		auto found = matchesAt();
		const Node *last = nullptr;
		for(auto it = root.nodes().rbegin(); it != root.nodes().rend(); ++it)
		{
			if(it->kind == NodeKind::Import && !it->enabled)
				continue;
			last = &*it;
			break;
		}
		if(found.size() == 1 && last && last->kind == NodeKind::Import && last->enabled)
		{
			auto imps = root.imports();
			if(imps[found[0]] == last)
				return false;
		}

		for(auto it = found.rbegin(); it != found.rend(); ++it)
		{
			if(!root.removeImport(*it))
			{
				auto n = root.imports()[*it];
				root.replace(n->span, "");
			}
		}
		root.appendImport(relPath);
		return true;
	}
}
