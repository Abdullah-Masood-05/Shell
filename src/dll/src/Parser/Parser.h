#pragma once

namespace Nilesoft
{
	namespace Shell
	{
		using namespace Windows;
		using namespace Drawing;

		// A file a parse depended on, with its last-write time taken just
		// before it was read (0 if it did not exist). See Parser::Sources.
		struct SourceFile
		{
			string path;
			uint64_t time{};
		};

		// One problem found while parsing, for shell -config:check.
		struct ParseDiagnostic
		{
			bool error{};		// false: a warning, the parse went on
			string file;
			size_t line{}, column{};
			string message;
		};

		class Parser
		{
			static constexpr auto error_format0 = L"error %s";
			static constexpr auto error_format1 = L"error %s, %s, \"%s\"";
			static constexpr auto error_format2 = L"error line[%d] column[%d], %s \"%s\"";

		private:
			Lexer *l = nullptr;
			string m_path;

			size_t	prevCol = 0;
			size_t	start_col = 0;
			size_t	start_line = 0;

			bool	m_error = false;
			TokenError error_code = TokenError::None;

			std::vector<uint32_t> m_imports;
			std::vector<std::unique_ptr<Lexer>> _imports;

		private:

			bool peek_char(wchar_t c, bool singleLineComment);
			uint32_t peek_ident(const std::initializer_list<uint32_t> &ids, uint32_t *value = nullptr);

			ExpressionType make_error(TokenError tokenError, size_t column);
			bool error(TokenError tokenError);
			bool error(TokenError tokenError, const wchar_t *message);
			bool error(TokenError tokenError, size_t column);
			bool error(TokenError tokenError, size_t column, size_t line);

			void error_if(bool condition, TokenError tokenError);
			void error_if(bool condition, TokenError tokenError, size_t column);
			void error_if(bool condition, TokenError tokenError, size_t column, size_t line);

			bool skip_comment(bool singleLineComment = true);
			bool skip(bool singleLineComment = true, bool eat_whitespace = true);
			Parser &eat(int i = -1);

			bool expect(wchar_t c, bool singleLineComment = true);
			bool expect(const wchar_t *word, bool singleLineComment = true);

			bool expect(std::initializer_list<wchar_t> words, bool singleLineComment = true);
			uint32_t expect(std::initializer_list<const wchar_t *> words, bool singleLineComment = true);

			bool expect_assign(bool expected = false);
			bool expect_openParen();
			bool expect_closeParen();

			bool expect_openCurly();
			bool expect_closeCurly();
			bool parse_environment(string &value);
			auto parse_operator(OperatorType precedence = OperatorType::None);
			bool is_number() const;
			bool is_color() const;

			//OperatorType get_operator(wchar_t op1, wchar_t op2 = 0) const;

			// literal
			//Expression *parse_char();
			Expression *parse_color();
			Expression *parse_number();
			Expression* parse_char();
			Expression *parse_string();
			FuncExpression *parse_func(Expression *parent = nullptr);
			Expression *parse_identifier(Expression *parent, bool hasedot = false, bool inside_quotes = false, bool is_var = false);
			Expression *parse_for_statement(Expression *parent = nullptr);
			Expression *parse_statement(Expression *parent = nullptr);
			Expression *parse_array(Expression *parent = nullptr);
			Expression *parse_brackets(Expression *parent = nullptr);
			Expression *parse_interpolated_string(wchar_t close, Expression *parent = nullptr);
			Expression *parse_unary_expression(Expression *parent = nullptr);
			Expression *parse_term(Expression *parent = nullptr);

			Expression *parse_expression(OperatorType precedence, Expression *parent = nullptr);
			Expression *parse_expression(Expression* parent = nullptr);
			Expression *parse_root_expression(Expression *parent = nullptr);

			bool parse_properties_command(CommandProperty *cmd, Ident const &id, std::unique_ptr<Expression> *expr);
			bool parse_properties_commands(CommandProperty *cmd);
			bool parse_properties_commands(NativeMenu *menu);
			bool parse_properties(NativeMenu *menu);
			bool parse_property(FileSystemObjects &fso);
			bool parse_clsid(NativeMenu *item);

			Expression *parse_property(Ident &id, bool as_value);
			Expression *parse_property__();
			bool parse_property_ident(Ident &id, bool singleLineComment = true);

			
			bool parse_ident(Ident &id, bool allow_dash = false);
			uint32_t parse_ident(bool multiple = true);

			bool parse_variable(Scope *variables, bool has_sign = true);
			int load_import(size_t line, size_t col, bool ignore_failed = true, bool parse_import = false);

			void parse_modify_items(uint32_t action = 0);

			uint32_t parse_image_ident();
			bool parse_image();

			bool parse_menu_item(NativeMenu *menu, uint32_t type);
			void parse_menu(NativeMenu *menu, bool has_curly = true);
			bool parse_modify_properties(NativeMenu *item, uint32_t action = 0);

			// into: the scope to fill; the configuration's loc strings if null.
			void parse_loc(bool has_curly = false, Scope *into = nullptr);

			// loc.<name> names used by the configuration, by name hash: the
			// name as written and where it was first used. See resolve_loc().
			struct LocRef
			{
				string name;
				string file;
				size_t line{}, column{};
				size_t order{};
			};
			std::unordered_map<uint32_t, LocRef> m_loc_refs;
			void note_loc(uint32_t key, const string &name, size_t line, size_t column);

			// The last loc.<name> or title.<name> built by parse_identifier
			// with nothing after it (no arguments, index or member), so a title
			// that is exactly one of them can be recognised. See record_title().
			struct
			{
				const Expression *expr{};
				uint32_t root{}, key{};
				string name;		// as written: "file_manage"
			} m_last_ident;

			// Where each menu/item title expression was written, and what it is
			// when that is known without running it. For ReportTitles().
			struct TitleRef
			{
				string file;
				size_t line{}, column{};
				uint32_t root{};	// IDENT_LOC, IDENT_TITLE, or 0
				uint32_t key{};
				string name;		// the <name> of loc.<name> / title.<name>
				bool literal{};		// a plain string
			};
			std::unordered_map<const Expression *, TitleRef> m_titles;
			void record_title(const Expression *e, size_t line, size_t column);

			// Where each menu/item/separator/modify/remove was declared (the
			// keyword), for ReportTree(). Like m_titles, only while a check
			// collects diagnostics.
			struct ItemRef
			{
				string file;
				size_t line{}, column{};
			};
			std::unordered_map<const NativeMenu *, ItemRef> m_items;
			void record_item(const NativeMenu *item, size_t line, size_t column);

			// Every loc.<name> the configuration uses but no imported language
			// file defines gets the English string from imports\lang\en.nss
			// (next to the root config, else in the install folder), or failing
			// that a readable form of the name, so a title is never blank.
			void resolve_loc();
			bool load_loc_file(const string &path, Scope &into);

			void parse_settings(std::vector<struct SETTING> *settings, const Ident &id, bool imported = false);
			void parse_settings(struct SETTING *setting, const Ident &id = 0, bool imported = false);
			void parse_theme();
			void parse_settings();
			void parse_config();

			bool is_lexer() const { return l == _imports.front().get(); };

			ExpressionType verify_ident(const Ident &id, const size_t argc, bool hasdot = false);
			int verify_string_ident(uint32_t id, const size_t argc);

			TokenError verify_command(const Ident &id, bool signer);
			TokenError verify(const Ident &id, bool signer);
			TokenError verify(const NativeMenu *menu, const Ident &id, bool _signed);

			// Ceiling on import nesting. _imports is the stack of files currently
			// open, so its size is the nesting depth. Nothing else bounds the
			// recursion: the duplicate check in load_import only traces, and it
			// consults every file ever imported rather than the ones still open,
			// so it cannot tell a cycle from a snippet imported from two places.
			// Without this a config that imports itself recursed until the stack
			// was exhausted, inside Explorer.
			static constexpr size_t MaxImportDepth = 32;

			void add_source(const wchar_t *path);
			void diagnose(bool is_error, const wchar_t *file, size_t line, size_t column, const string &message);

			Lexer *import_push()
			{
				_imports.emplace_back(new Lexer);
				return _imports.back().get();
			}

			void pop_import(bool chk = true)
			{
				if(chk && _imports.size() >= 2)
				{
					_imports.pop_back();
				}
				l = _imports.back().get();
			}
/*
			bool log(TokenError err, size_t line, size_t column)
			{
				if(!m_error)
				{
					error_code = err;
					m_error = true;
					Logger::Error(error_format2, err, line, column,
								  Path::Name(context.Application->Config).c_str());
				}
				return false;
			}

			bool log(TokenError err, const wchar_t *message)
			{
				if(!m_error)
				{
					error_code = err;
					m_error = true;
					if(message)
						Logger::Error(error_format1, error_code, message,
									  Path::Name(context.Application->Config).c_str());
					else
					{
						Logger::Error(error_format0, error_code, message,
									  Path::Name(context.Application->Config).c_str());
					}
				}
				return false;
			}


			bool log(TokenError err, size_t column) { return log(err, Line, column); }
			bool log(TokenError err) { return log(err, Line, Column); }
			bool log() { return log(error_code, Line, Column); }
*/

		public:
			// root: parse this file instead of resolving the config location
			// (ConfigLocation), and leave Application::Config alone. Used by
			// shell -config:check.
			explicit Parser(const wchar_t *root = nullptr);
			~Parser();

			// When set, errors and warnings are also collected here. Per thread,
			// so a check never sees another thread's parse.
			inline static thread_local std::vector<ParseDiagnostic> *Diagnostics = nullptr;


			bool			Load();
			bool			HasError() const;

			// shell -config:check -titles: every menu/item title in the loaded
			// configuration, in menu order, evaluated where that needs no
			// selection (a string, loc.<name>, title.<name>).
			struct TitleInfo
			{
				enum State { Text, Empty, Dynamic } state{};
				enum Kind { Menu, Item, Modify } kind{};
				uint32_t depth{};	// 0: top level
				string file;
				size_t line{}, column{};
				string source;		// "loc.<name>" / "title.<name>", else empty
				string text;
			};
			// where: null for every title, else only those whose type= lets
			// them show in "file", "folder", "back" (a folder's background),
			// "desktop" or "taskbar"; false if `where` is none of these.
			bool ReportTitles(std::vector<TitleInfo> &out, const wchar_t *where = nullptr);

			// shell -config:tree: the menu for a right-click in `where` (as
			// ReportTitles, plus "drive"), as JSON: Shell's own items with the
			// file and line of each, the Windows menu built off-screen for
			// `sample` (no window is shown), and the modify/remove rules with
			// the Windows items each one reaches. See src/dll/src/MenuTree.cpp.
			bool ReportTree(std::wstring &json, const wchar_t *where, const wchar_t *sample);

			// The selection a right-click in `where` makes, as Selections
			// describes it (types, background, item count). False for an
			// unknown `where`; an empty one leaves `filter` false.
			static bool WhereTypes(const wchar_t *where, int32_t (&types)[FSO_MAX], bool &background,
								   size_t &count, bool &filter);
			// What ReportTitles says about one item's title.
			TitleInfo DescribeTitle(NativeMenu *item, uint32_t depth, TitleInfo::Kind kind);
			TokenError		Error() const;

			size_t			Line() const;
			size_t			Column() const;
			const wchar_t*	Path() const;
			uint32_t		TotalMenuCount = 0;
			string			ConfigVersion;
			Context			context;
			string			location;

			// Every file this parse read or tried to read: the root config, each
			// import (missing ones too, with time 0), and the higher-priority
			// config locations that did not exist, so creating one is noticed.
			// Each is stamped before it is read: a save racing the read can only
			// cause one extra reload, never a missed one.
			std::vector<SourceFile> Sources;

			private:
				struct FUNC
				{
					uint32_t argc{};
					uint32_t id{};
					std::vector<FUNC> idents;

					FUNC *scope(const Ident &ident, uint32_t i = 1)
					{
						FUNC *ret{};
						for(auto &func : idents)
						{
							if(func.id == ident[i])
							{
								if(i == (ident.length() - 1))
									ret = &func;
								else
									ret = func.scope(ident, ++i);
								break;
							}
						}
						return ret;
					}

					int isf(const Ident &ident, uint32_t arg = 0 , uint32_t i = 1) const
					{
						if(ident.length() == 1)
						{
							if(id == ident[0])
								return argc == arg;
						}
						else 
						{
							for(auto &func : idents)
							{
								if(func.id == ident[i])
								{
									if(i == (ident.length() - 1))
										return true;
									else
										return func.isf(ident, ++i);
									break;
								}
							}
						}
						return 0;
					}

					static bool is(const FUNC& f, const Ident &ident)
					{
						return f.isf(ident);
					}
				};

				FUNC _theme;
		};
	}
}
