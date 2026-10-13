#pragma once

// A minimal JSON reader/writer for snapshot manifests and -config:check
// output. Internal to nsscore.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nss::json
{
	struct Value
	{
		enum class Type { Null, Bool, Number, String, Array, Object };
		Type type = Type::Null;
		bool boolean = false;
		double number = 0;
		std::string string;
		std::vector<Value> array;
		std::vector<std::pair<std::string, Value>> object;

		const Value *get(std::string_view key) const
		{
			for(auto &kv : object)
				if(kv.first == key) return &kv.second;
			return nullptr;
		}
		std::string str(std::string_view key) const
		{
			auto v = get(key);
			return v && v->type == Type::String ? v->string : std::string();
		}
		bool flag(std::string_view key) const
		{
			auto v = get(key);
			return v && v->type == Type::Bool && v->boolean;
		}
	};

	std::optional<Value> parse(std::string_view text);
	std::string quote(std::string_view s);
}
