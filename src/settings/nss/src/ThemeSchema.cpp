#include "nss/ThemeSchema.h"
#include "nss/Text.h"

#include <cmath>
#include <unordered_map>

namespace nss
{
	namespace
	{
		constexpr double inf = std::numeric_limits<double>::infinity();

		struct Table
		{
			std::vector<KeyInfo> keys;
			std::unordered_map<std::string, size_t> index;
			std::unordered_map<std::string, std::string> aliases;

			void add(std::string key, ValueType type, double min = -inf, double max = inf, bool integer = false)
			{
				KeyInfo k;
				k.key = std::move(key);
				k.type = type;
				k.min = min;
				k.max = max;
				k.integer = integer;
				index[k.key] = keys.size();
				keys.push_back(std::move(k));
			}

			void addEnum(std::string key, std::vector<std::string> values)
			{
				add(std::move(key), ValueType::Enum);
				keys.back().values = std::move(values);
			}

			// key, key.disabled, key.normal, key.normal.disabled, key.select, key.select.disabled
			void addStates(const std::string &key, bool withSelf = true)
			{
				if(withSelf)
					add(key, ValueType::Color);
				add(key + ".normal", ValueType::Color);
				add(key + ".normal.disabled", ValueType::Color);
				add(key + ".select", ValueType::Color);
				add(key + ".select.disabled", ValueType::Color);
			}

			void addBox(const std::string &key)
			{
				add(key, ValueType::Box, 0);
				for(auto side : { ".left", ".top", ".right", ".bottom" })
					add(key + side, ValueType::Number);
			}

			void alias(std::string from, std::string to) { aliases[std::move(from)] = std::move(to); }

			Table()
			{
				// theme (Parser::parse_theme)
				addEnum("theme.name", { "auto", "classic", "white", "black", "modern" });
				add("theme.dark", ValueType::Bool);
				addEnum("theme.view", { "auto", "compact", "small", "medium", "large", "wide" });
				add("theme.background.color", ValueType::Color);
				add("theme.background.opacity", ValueType::Number, 0, 100);
				add("theme.background.effect", ValueType::Effect);
				add("theme.background.tintcolor", ValueType::Color);
				add("theme.background.image", ValueType::Any);
				add("theme.background.gradient.enabled", ValueType::Bool);
				add("theme.background.gradient.linear", ValueType::Any);
				add("theme.background.gradient.radial", ValueType::Any);
				add("theme.background.gradient.stop", ValueType::Any);
				add("theme.font", ValueType::Any);
				add("theme.font.size", ValueType::Number, 6);
				add("theme.font.name", ValueType::String);
				add("theme.font.weight", ValueType::Number, 1, 9, true);
				add("theme.font.italic", ValueType::Bool);
				add("theme.font.quality", ValueType::Number, 0, 6, true);
				add("theme.item.text", ValueType::Color);
				addStates("theme.item.text", false);
				add("theme.item.text.align", ValueType::Number);
				addStates("theme.item.back");
				addStates("theme.item.border");
				add("theme.item.opacity", ValueType::Number, 0, 100);
				add("theme.item.radius", ValueType::Number, 0, 4, true);
				add("theme.item.prefix", ValueType::Number, 0, 2, true);
				addBox("theme.item.padding");
				addBox("theme.item.margin");
				add("theme.shadow.enabled", ValueType::Bool);
				add("theme.shadow.size", ValueType::Number, 0, 30);
				add("theme.shadow.color", ValueType::Color);
				add("theme.shadow.opacity", ValueType::Number, 0, 100);
				add("theme.shadow.offset", ValueType::Number, 0, 30);
				add("theme.border.enabled", ValueType::Bool);
				add("theme.border.size", ValueType::Number, 0, 10);
				add("theme.border.color", ValueType::Color);
				add("theme.border.opacity", ValueType::Number, 0, 100);
				add("theme.border.radius", ValueType::Number, 0);
				addBox("theme.border.padding");
				add("theme.separator.color", ValueType::Color);
				add("theme.separator.opacity", ValueType::Number, 0, 100);
				add("theme.separator.size", ValueType::Number, 0, 40);
				addBox("theme.separator.margin");
				addStates("theme.symbol");
				addStates("theme.symbol.checkmark");
				addStates("theme.symbol.bullet");
				addStates("theme.symbol.chevron");
				add("theme.layout.width", ValueType::Number, 0);
				add("theme.layout.rtl", ValueType::Bool);
				add("theme.layout.popup.align", ValueType::Number, -20, 20);
				add("theme.image.enabled", ValueType::Bool);
				add("theme.image.color", ValueType::ColorList);
				add("theme.image.gap", ValueType::Number, 0);
				add("theme.image.size", ValueType::Number, 0);
				add("theme.image.glyph", ValueType::String);
				add("theme.image.scale", ValueType::Bool);
				add("theme.image.display", ValueType::Number, 0, 2, true);

				// settings (Parser::parse_settings)
				add("settings.priority", ValueType::Number);
				add("settings.showdelay", ValueType::Number, 0, 4000);
				add("settings.screenshot.enabled", ValueType::Bool);
				add("settings.screenshot.directory", ValueType::String);
				add("settings.exclude.where", ValueType::Any);
				add("settings.exclude.window", ValueType::Any);
				add("settings.exclude.process", ValueType::Any);
				add("settings.tip.enabled", ValueType::Bool);
				for(auto style : { "default", "primary", "info", "success", "warning", "danger" })
					add(std::string("settings.tip.") + style, ValueType::ColorPair);
				add("settings.tip.border", ValueType::Color);
				add("settings.tip.width", ValueType::Number, 200, 2000);
				add("settings.tip.opacity", ValueType::Number, 0, 100);
				add("settings.tip.radius", ValueType::Number, 0, 3, true);
				add("settings.tip.time", ValueType::Number, 0);
				addBox("settings.tip.padding");
				add("settings.modify.enabled", ValueType::Bool);
				add("settings.modify.image", ValueType::Number, 0, 2, true);
				add("settings.modify.title", ValueType::Bool);
				add("settings.modify.visibility", ValueType::Bool);
				add("settings.modify.parent", ValueType::Bool);
				add("settings.modify.position", ValueType::Bool);
				add("settings.modify.separator", ValueType::Bool);
				add("settings.modify.auto", ValueType::Bool);
				add("settings.modify.remove.duplicate", ValueType::Bool);
				add("settings.modify.remove.disabled", ValueType::Bool);
				add("settings.modify.remove.separator", ValueType::Bool);
				add("settings.new.enabled", ValueType::Bool);
				add("settings.new.image", ValueType::Bool);

				// A block's own value (SETTING::value) is one of its children.
				alias("theme", "theme.name");
				alias("theme.background", "theme.background.color");
				alias("theme.shadow", "theme.shadow.enabled");
				alias("theme.border", "theme.border.size");
				alias("theme.separator", "theme.separator.color");
				alias("theme.layout.popup", "theme.layout.popup.align");
				alias("theme.image", "theme.image.enabled");
				alias("theme.image.align", "theme.image.display");
				alias("settings", "settings.priority");
				alias("settings.screenshot", "settings.screenshot.enabled");
				alias("settings.exclude", "settings.exclude.where");
				alias("settings.tip", "settings.tip.enabled");
				alias("settings.modify", "settings.modify.enabled");
				alias("settings.new", "settings.new.enabled");
			}
		};

		const Table &table()
		{
			static const Table t;
			return t;
		}

		bool isKeyword(const Literal &v, std::initializer_list<const char *> words)
		{
			if(v.kind != LiteralKind::Keyword) return false;
			for(auto w : words)
				if(v.string == w) return true;
			return false;
		}

		bool isColor(const Literal &v)
		{
			return v.kind == LiteralKind::Color || (v.kind == LiteralKind::Name && v.string.rfind("color.", 0) == 0) ||
				isKeyword(v, { "auto", "default", "none" });
		}

		std::string fmtRange(const KeyInfo &k)
		{
			if(std::isinf(k.max))
				return ">= " + formatNumber(k.min);
			if(std::isinf(k.min))
				return "<= " + formatNumber(k.max);
			return formatNumber(k.min) + ".." + formatNumber(k.max);
		}

		void checkNumber(const KeyInfo &k, const Literal &v, std::vector<std::string> &errors, const std::string &what)
		{
			if(v.kind != LiteralKind::Number)
			{
				errors.push_back(what + ": a number is expected");
				return;
			}
			if(v.number < k.min || v.number > k.max)
				errors.push_back(what + ": " + formatNumber(v.number) + " is out of range (" + fmtRange(k) + ")");
			else if(k.integer && v.number != std::floor(v.number))
				errors.push_back(what + ": a whole number is expected");
		}
	}

	const std::vector<KeyInfo> &ThemeSchema::keys()
	{
		return table().keys;
	}

	std::string ThemeSchema::canonical(std::string_view key)
	{
		auto k = toLower(trim(key));
		if(k == "theme.tip" || k.rfind("theme.tip.", 0) == 0)
			k = "settings" + k.substr(5);
		auto &aliases = table().aliases;
		if(auto it = aliases.find(k); it != aliases.end())
			return it->second;
		return k;
	}

	const KeyInfo *ThemeSchema::find(std::string_view key)
	{
		auto &t = table();
		auto it = t.index.find(canonical(key));
		return it == t.index.end() ? nullptr : &t.keys[it->second];
	}

	size_t ThemeSchema::order(std::string_view key)
	{
		auto &t = table();
		auto it = t.index.find(canonical(key));
		return it == t.index.end() ? t.keys.size() : it->second;
	}

	const char *ThemeSchema::typeName(ValueType type)
	{
		switch(type)
		{
			case ValueType::Bool: return "bool";
			case ValueType::Number: return "number";
			case ValueType::Color: return "color";
			case ValueType::String: return "string";
			case ValueType::Enum: return "enum";
			case ValueType::Effect: return "effect";
			case ValueType::Box: return "box";
			case ValueType::ColorPair: return "color pair";
			case ValueType::ColorList: return "color list";
			default: return "any";
		}
	}

	std::vector<std::string> ThemeSchema::validate(std::string_view key, std::string_view value)
	{
		return validate(key, Literal::parse(value));
	}

	std::vector<std::string> ThemeSchema::validate(std::string_view key, const Literal &v)
	{
		std::vector<std::string> errors;
		auto info = find(key);
		std::string name = canonical(key);
		if(!info)
		{
			errors.push_back(name + ": unknown key");
			return errors;
		}
		if(v.kind == LiteralKind::Expression)
		{
			if(trim(v.text).empty())
				errors.push_back(name + ": a value is expected");
			return errors;
		}
		// The core treats auto/default as "use the built-in value" almost everywhere.
		if(isKeyword(v, { "auto", "default" }) && info->type != ValueType::Bool)
			return errors;

		switch(info->type)
		{
			case ValueType::Bool:
				if(!v.isBool() && !isKeyword(v, { "auto", "default" }))
					errors.push_back(name + ": true or false is expected");
				break;
			case ValueType::Number:
				checkNumber(*info, v, errors, name);
				break;
			case ValueType::Color:
				if(!isColor(v))
					errors.push_back(name + ": a color is expected");
				break;
			case ValueType::String:
				if(v.kind != LiteralKind::String)
					errors.push_back(name + ": a string is expected");
				break;
			case ValueType::Enum:
			{
				std::string word;
				if(v.kind == LiteralKind::String || v.kind == LiteralKind::Keyword)
					word = toLower(v.string);
				else if(v.kind == LiteralKind::Name)
				{
					auto dot = v.string.find('.');
					auto ns = v.string.substr(0, dot);
					// theme.view takes view.compact; theme.name takes strings.
					if(name == "theme.view" && ns == "view")
						word = v.string.substr(dot + 1);
				}
				bool ok = false;
				for(auto &allowed : info->values)
					if(word == allowed) ok = true;
				if(!ok)
				{
					std::string list;
					for(auto &allowed : info->values)
						list += (list.empty() ? "" : ", ") + (name == "theme.view" && allowed != "auto" ? "view." + allowed : allowed);
					errors.push_back(name + ": expected one of " + list);
				}
				break;
			}
			case ValueType::Effect:
			{
				KeyInfo mode;
				mode.min = 0;
				mode.max = 3;
				mode.integer = true;
				if(v.kind == LiteralKind::Array)
				{
					if(v.items.empty() || v.items.size() > 3)
						errors.push_back(name + ": [effect, tint, opacity] is expected");
					else
					{
						checkNumber(mode, v.items[0], errors, name + " effect");
						if(v.items.size() > 1 && !isColor(v.items[1]))
							errors.push_back(name + ": the tint must be a color");
						if(v.items.size() > 2)
						{
							KeyInfo opacity;
							opacity.min = 0;
							opacity.max = 100;
							checkNumber(opacity, v.items[2], errors, name + " opacity");
						}
					}
				}
				else
					checkNumber(mode, v, errors, name);
				break;
			}
			case ValueType::Box:
			{
				if(v.kind == LiteralKind::Array)
				{
					auto n = v.items.size();
					if(n != 1 && n != 2 && n != 4)
						errors.push_back(name + ": 1, 2 or 4 values are expected");
					KeyInfo side;
					for(auto &item : v.items)
						checkNumber(side, item, errors, name);
				}
				else
					checkNumber(*info, v, errors, name);
				break;
			}
			case ValueType::ColorPair:
			case ValueType::ColorList:
			{
				size_t max = info->type == ValueType::ColorPair ? 2 : 3;
				if(v.kind == LiteralKind::Array)
				{
					if(v.items.empty() || v.items.size() > max)
						errors.push_back(name + ": up to " + std::to_string(max) + " colors are expected");
					for(auto &item : v.items)
						if(!isColor(item))
							errors.push_back(name + ": " + item.text + " is not a color");
				}
				else if(!isColor(v))
					errors.push_back(name + ": a color is expected");
				break;
			}
			default:
				break;
		}
		return errors;
	}
}
