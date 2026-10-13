
#include <pch.h>
#include <tuple>
#include "Expression\Variable.h"
#include "Include\Theme.h"
#include "ConfigLocation.h"

// Windows State (Hidden | Normal | Minimized | Maximized)
namespace Nilesoft
{
	namespace Shell
	{
		extern Logger &_log = Logger::Instance();

		Parser::Parser(const wchar_t *root)
		{
			context.Application = &Initializer::instance->application;

			context.Cache = Initializer::instance->cache;
			context.variables.global = &context.Cache->variables.global;
			context.variables.runtime = &context.Cache->variables.runtime;
			context.Selections = nullptr;

			if(root && *root)
			{
				m_path = root;
				add_source(m_path);
				if(!ConfigLocation::IsFile(m_path))
				{
					error_code = TokenError::CannotFoundConfigFile;
					m_error = true;
					diagnose(true, m_path, 0, 0, L"config file not found");
					return;
				}

				_imports.emplace_back(new Lexer);
				l = _imports.front().get();
				if(!l->load_File(m_path.c_str()) || l->length < 5)
				{
					if(l->error != TokenError::None)
					{
						error_code = l->error;
						m_error = true;
						diagnose(true, m_path, 0, 0, ParserException::errortostr(l->error));
					}
					else
					{
						diagnose(false, m_path, 0, 0, L"config file is empty");
					}
				}
				return;
			}

			// HKCU "config" value, then %APPDATA%\Nilesoft\Shell\shell.nss, then
			// the install folder's shell.nss; see ConfigLocation. Shared with
			// shell.exe so -status/-config:path report this same file. The DLL
			// only ever reads it.
			auto location = ConfigLocation::Resolve(context.Application->Dirctory);

			// Watch the locations that would take precedence over the one in
			// use (they do not exist now), so creating one, e.g. with
			// shell -config:init, is picked up like an edit.
			if(location.source != ConfigLocation::Source::registry && !location.configured.empty())
				add_source(location.configured);
			if(location.source == ConfigLocation::Source::install || location.source == ConfigLocation::Source::none)
				add_source(ConfigLocation::UserConfig());

			if(location.source == ConfigLocation::Source::none)
			{
				add_source(location.path);
				string ss = L"config file ";
				if(!location.configured.empty())
				{
					ss += L"'";
					ss += location.configured;
					ss += L"' ";
				}
				ss += L"not found.";
				_log.warning(ss);
				return;
			}

			m_path = location.path.move();
			context.Application->Config = m_path;

			_imports.emplace_back(new Lexer);
			l = _imports.front().get();

			add_source(m_path);
			if(!l->load_File(m_path.c_str()) || l->length < 5)
			{
				if(HasError())
				{
					error(l->length < 5 ? TokenError::InvalidConfigFile : l->error, m_path.c_str());
				}
			}
		}

		Parser::~Parser() { }

		void Parser::diagnose(bool is_error, const wchar_t *file, size_t line, size_t column, const string &message)
		{
			if(auto list = Diagnostics)
				list->push_back({ is_error, file ? file : L"", line, column, message });
		}

		void Parser::add_source(const wchar_t *path)
		{
			if(!path || !*path)
				return;
			for(auto &source : Sources)
			{
				if(source.path.equals(path, true))
					return;
			}
			Sources.push_back({ path, Initializer::last_write_time(path) });
		}
		size_t Parser::Line() const { return l ? l->line : 0; };
		size_t Parser::Column() const { return l ? l->column : 0; };
		const wchar_t *Parser::Path() const { return l ? l->path.c_str() : nullptr; };

		bool Parser::HasError() const
		{
			return error_code != TokenError::None;
		}

		TokenError Parser::Error() const
		{
			return error_code;
		}

		bool Parser::error(TokenError tokenError)
		{
			if(!m_error)
			{
				error_code = tokenError;
				m_error = true;
			}

			// Every call aborts the parse (it throws), so it is reported each time.
			diagnose(true, l->path, l->line, l->column, ParserException::errortostr(tokenError));

			bool log = true;
			auto le = &Initializer::LastError;
			if(le->code != TokenError::None)
				log = !(le->code == error_code && le->line == l->line && le->col == l->column);

			throw ParserException(tokenError, l->line, l->column, Path::Name(l->path).c_str(), log);
		}

		bool Parser::error(TokenError tokenError, const wchar_t *message)
		{
			if(!m_error)
			{
				error_code = tokenError;
				m_error = true;

				string text = ParserException::errortostr(tokenError);
				if(message)
				{
					text += L", ";
					text += message;
				}
				diagnose(true, l ? l->path.c_str() : m_path.c_str(), l ? l->line : 0, l ? l->column : 0, text);

				auto le = &Initializer::LastError;
				if(le->code != TokenError::None && (le->code == error_code && le->line == l->line && le->col == l->column))
					return false;

				if(message)
					Logger::Error(error_format1, ParserException::errortostr(error_code), message, Path::Name(l->path).c_str());
				else
					Logger::Error(error_format0, ParserException::errortostr(error_code));
				//Logger::Error(L"error %s, %s file", ParserException::errortostr(error_code), message);
			}
			return false;
		}

		bool Parser::error(TokenError tokenError, size_t column)
		{
			l->column = column;
			return error(tokenError);
		}

		bool Parser::error(TokenError tokenError, size_t column, size_t line)
		{
			l->column = column;
			l->line = line;
			return error(tokenError);
		}

		void Parser::error_if(bool condition, TokenError tokenError)
		{
			if(condition) error(tokenError);
		}

		void Parser::error_if(bool condition, TokenError tokenError, size_t column)
		{
			if(condition) error(tokenError, column);
		}

		void Parser::error_if(bool condition, TokenError tokenError, size_t column, size_t line)
		{
			if(condition) error(tokenError, column, line);
		}

		bool Parser::skip_comment(bool singleLineComment)
		{
			auto col = start_col, line = start_line;

			if(l->eof) return false;

			auto single = (l->tok == L'/' && l->peek == L'/');	// start single-line comment
			auto multi = (l->tok == L'/' && l->peek == L'*');	// start multi-line comment
					
			if(single)
			{
				if(!singleLineComment) return false;
				//error_if(singleLineComment == 2, TokenError::CommentSinglelineUnexpected);
			}

			if(single || multi)
			{
				if(multi)
				{
					col = l->column, line = l->line;
				}

				//skip start comment and continue to end
				l->next(2);
				for(;;)
				{
					if(l->eof)
					{
						//error(TokenError::CloseCommentExpected, col, line);
						//return false;
						//break;
						//if(single) return false;
						error_if(multi, TokenError::CloseCommentExpected, col, line);
						return false;
					}
					// End single-line comment
					else if(l->tok == L'\n' && single) break;
					// End multi-line comment
					else if((l->tok == L'*' && l->peek == L'/') && multi) { l->next(); break; }
					l->next();
				}
				l->next(); //skip end comment '\n' or '/'
				return true;
			}
			return false;
		}

		bool Parser::skip(bool singleLineComment, bool eat_whitespace)
		{
			for(; ;)
			{
				if(l->eof) return false;
				if(l->is_space() && eat_whitespace) l->next();
				else if(l->tok == L'/' && l->is({ L'/', L'*' }, 1))
				{
					if(!skip_comment(singleLineComment))
						break;
				}
				else
				{
					break;
				}
			}
			return true;
		}

		Parser &Parser::eat(int i)
		{
			skip();
			if(i >= 0)
			{
				l->next(i);
				skip();
			}
			return *this;
		}

		bool Parser::peek_char(wchar_t c, bool singleLineComment)
		{
			auto p = l->index;
			skip(singleLineComment);
			if(l->next_is(c))
				return true;
			l->previous(p);
			return false;
		}

		uint32_t Parser::peek_ident(const std::initializer_list<uint32_t> &ids, uint32_t *value)
		{
			auto s = l->buf();
			if(std::iswalpha(*s))
			{
				uint32_t count = 0;
				Hash h;
				h.hash(*s);
				do
				{
					h.hash(*s++); count++;
				} while(std::iswalnum(*s) || (*s == L'_'));

				for(auto &id : ids)
				{
					if(h == id)
					{
						if(value) *value = h;
						return count;
					}
				}
			}
			if(value) *value = 0;
			return 0;
		}

		bool Parser::expect(wchar_t c, bool singleLineComment)
		{
			skip(singleLineComment);
			if(l->next_is(c))
				return true;
			return false;
		}

		bool Parser::expect(std::initializer_list<wchar_t> chs, bool singleLineComment)
		{
			for(auto c : chs)
			{
				if(l->eof) return false;
				if(expect(c, singleLineComment)) return true;
			}
			return false;
		}


		uint32_t Parser::expect(std::initializer_list<const wchar_t *> words, bool singleLineComment)
		{
			for(auto word : words)
			{
				if(l->eof) return false;
				if(expect(word, singleLineComment)) return true;
			}
			return false;
		}

		bool Parser::expect(const wchar_t *word, bool singleLineComment)
		{
			skip(singleLineComment);

			bool ret = true;
			auto last_index = l->index;

			size_t i = 0;
			while(*word)
			{
				if(l->eof) return false;
				if(++i >= l->length) break;

				if(!l->is(*word++))
				{
					ret = false;
					break;
				}

				l->next();
			}

			if(ret && !l->is_alpha())
				return true;

			l->previous(last_index);
			return false;
		}

		bool Parser::expect_assign(bool expected)
		{
			skip();
			if(l->next_if(l->is_assign())) return true;
			error_if(expected, TokenError::AssignExpected);
			return false;
		}

		bool Parser::expect_openParen()
		{
			return expect(L'(') ? true : error(TokenError::OpenParenExpected);
		}

		bool Parser::expect_closeParen()
		{
			return expect(L')') ? true : error(TokenError::CloseParenExpected);
		}

		bool Parser::expect_openCurly()
		{
			return expect(L'{') ? true : error(TokenError::OpenCurlyExpected);
		}

		bool Parser::expect_closeCurly()
		{
			return expect(L'}') ? true : error(TokenError::CloseCurlyExpected);
		}

		// system environment variable
		//	::= %var%
		bool Parser::parse_environment(string &value)
		{
			if(l->tok != L'%') return false;
			wchar_t percent = l->next();// skip start '%'
			if(l->tok != L'%')
			{
				bool ret = false;
				for(;;)
				{
					error_if(l->eof, TokenError::StringUnterminated);

					if(l->next_is(percent))// skip end %
					{
						ret = true;
						break;
					}
					else if(!(l->is_iddigit() || l->tok == L'(' || l->tok == L')'))
						break;
					value.append(l->next());
				}

				if(ret)
				{
					string var = Environment::Variable(value).move();
					if(!var.empty())
					{
						value = var.move();
						return true;
					}
					value.append(percent);
				}
			}
			value = (percent + value).move();
			return true;
		}

		uint32_t Parser::parse_ident(bool multiple)
		{
			skip();
			prevCol = l->column;
			Hash ident;
			if(multiple)
				while(l->is_iddigit() || l->is({ L'-', L'.' })) ident.hash(l->next());
			else
				while(l->is_iddigit()) ident.hash(l->next());
			//error_if(eof, TokenError::IdentifierExpected);
			return ident.value();
		}

		void Parser::parse_menu(NativeMenu *menu, bool imported)
		{
			if(!imported)
				expect_openCurly();

			while(true)
			{
				skip();

				if(l->eof || l->tok == L'}')
					break;

				// local variables definition
				if(parse_variable(&menu->variables))
					continue;

				// local image definition
				if(parse_image())
					continue;

				auto type = parse_ident(false);
				
				if(type == CONFIG_IMPORT)
				{
					auto ret = load_import(l->line, l->column, true, false);
					if(ret > 0)
					{
						skip();
						prevCol = l->column;
						parse_menu(menu, true);
						skip();
						pop_import();
					}
					continue;
				}
				else if(type == CONFIG_MODIFY || type == IDENT_REMOVE)
				{
					parse_modify_items(type);
					continue;
				}

				std::unique_ptr<NativeMenu> sub(new NativeMenu(menu));
				if(parse_menu_item(sub.get(), type))
				{
					menu->items.push_back(sub.release());
					TotalMenuCount++;
				}
			//	else if(!imported)
			//		break;
			}

			if(!imported && !l->eof)
				expect_closeCurly();
		}

		bool Parser::parse_menu_item(NativeMenu *menu, uint32_t type)
		{
			switch(type)
			{
				case MENU_TYPE_MENU:
					menu->type = NativeMenuType::Menu;
					break;
				case MENU_TYPE_ITEM:
					menu->type = NativeMenuType::Item;
					break;
				case MENU_TYPE_SEP:
				case MENU_TYPE_SEPARATOR:
					menu->type = NativeMenuType::Separator;
					break;
				default:
					return error(TokenError::MenuTypeExpected, prevCol);
			}

			auto col_after_open_paren = l->column;

			// parse menu properties
			if(!parse_properties(menu))
				return false;

			if(menu->properties == 0 && menu->is_separator())
				return true;

			if(!menu->is_separator())
			{
				error_if(menu->properties <= 0, TokenError::PropertyExpected, l->column - 1);
				
				if(!menu->title && !menu->image.defined)
					error(TokenError::PropertyTitleOrImageExpected, col_after_open_paren);

				if(menu->is_item())
				{
					skip();
					if(l->tok == L'{')
					{
						// parse menu commands property
						if(!parse_properties_commands(menu))
							return false;
					}
					menu->properties += !menu->commands.empty();
				}

				/*
				// fixme
				if(!menu->cmd->admin.expr && menu->cmd->admin.value == Privileges::None)
				{
					if(menu->owner->cmd->admin.value != Privileges::None)
						menu->cmd->admin.value = menu->owner->cmd->admin.value;
					else
					{
						menu->cmd->admin.inherit = true;
						menu->cmd->admin.expr = menu->owner->cmd->admin.expr;
					}
				}*/

				if(menu->is_menu())
					parse_menu(menu, false);
			}
			return true;
		}

		void Parser::parse_modify_items(uint32_t action)
		{
			auto cache = Initializer::instance->cache;
			std::unique_ptr<NativeMenu> item(new NativeMenu(true));
			if(eat().parse_modify_properties(item.get(), action))
				cache->statics.push_back(item.release());
		}

		struct SETTING
		{
			uint32_t id = 0;
			auto_expr *value = nullptr;
			std::vector<SETTING> children;

			SETTING *find(const Ident &ident,  uint32_t i = 1)
			{
				SETTING *ret{};
				for(auto &setting : children)
				{
					if(setting.id == ident[i])
					{
						ret = (i == (ident.length() - 1)) ? &setting : setting.find(ident, ++i);
						break;
					}
				}
				return ret;
			}
		};

		void Parser::parse_settings(std::vector<SETTING> *settings, const Ident &id, bool imported)
		{
			if(!settings)
				return;

			auto ln = l->line;
			auto col = l->column;

			skip();

			Ident ident;
			if(id != 0)
				ident = id;
			else if(!parse_ident(ident, true))
				return;

			auto has_bracket = eat().l->next_is(L'{');
			
			if(!has_bracket && !l->is_assign())
			{
				if(l->eof && imported)
					return;
				error(TokenError::AssignExpected);
			}

			for(auto &setting : *settings)
			{
				if(ident == setting.id)
				{
					auto _setting = &setting;
					if(ident.length() > 1)
					{
						_setting = _setting->find(ident);
						if(!_setting) goto error_undefined;
					}

					if(has_bracket)
					{
						parse_settings(_setting, ident, imported);
						return;
					}

					if(!eat().l->next_if(l->is_assign()))
					{
						if(l->eof && imported)
							return;
						error(TokenError::AssignExpected);
					}

					*_setting->value = parse_expression();
					return;
				}
			}

		error_undefined:
			error(TokenError::IdentifierUndefined, col, ln);
		}

		void Parser::parse_settings(SETTING *setting, const Ident &id, bool imported)
		{
			auto cache = Initializer::instance->cache;

			if(!setting)
				return;

			skip();

			auto ln = l->line;
			auto col = l->column;
			Ident _ident;
			prevCol = l->column;
			if(id != 0)
				_ident = id;
			else if(!parse_ident(_ident, true))
				return;
			
			skip();

			col = l->column;
			auto has_bracket = false;
			if(l->next_if(l->is_assign()))
			{
				if(_ident == setting->id)
				{
					*setting->value = parse_expression();
					return;
				}

				if(l->eof && imported)
					return;
				goto error_undefined;
			}
			else if(l->tok == L'{')
			{
				l->next();
				has_bracket = true;
			}
			
			while(true)
			{
				skip();

				if(l->eof || l->tok == L'}')
					break;

				skip();
				col = l->column;
				Ident ident;
				if(!parse_ident(ident, true))
				{
					if(l->next_is(L'{'))
						continue;

					if(l->is(L'}'))
						break;

					if(parse_variable(&cache->variables.global))
						continue;

					if(l->eof && imported)
						break;
					
					goto error_undefined;
				}
				
				if(ident[0] == IDENT_IMPORT)
				{
					if(ident.length() > 1)
						goto error_undefined;

					if(auto ret = load_import(l->line, l->column, true, false); ret > 0)
					{
						while(!l->eof) parse_settings(&setting->children, 0, true);
						pop_import();
						if(!has_bracket)
							return;
					}
					continue;
				}

				if(!ident.empty() && l->tok == L'}')
					break;
				parse_settings(&setting->children, ident);
			}

			expect_closeCurly();

			return;

		error_undefined:
			error(TokenError::IdentifierUndefined, col, ln);
		}

		void Parser::parse_theme()
		{
			auto st = &context.Cache->settings.theme;

			SETTING _item = { IDENT_ITEM, nullptr, {
				{ IDENT_TEXT, &st->item.text.color.value, {
					{ IDENT_NORMAL, &st->item.text.color.normal, {
						{ IDENT_DISABLED, &st->item.text.color.normal_disabled }
					}},
					{ IDENT_SELECT, &st->item.text.color.select, {
						{ IDENT_DISABLED, &st->item.text.color.select_disabled }
					}},
					{ IDENT_ALIGN, &st->item.text.align }
				}},
				{ IDENT_BACK, &st->item.back.value, {
					{ IDENT_NORMAL, &st->item.back.normal, {
					{ IDENT_DISABLED, &st->item.back.normal_disabled }
					}},
					{ IDENT_SELECT, &st->item.back.select, {
					{ IDENT_DISABLED, &st->item.back.select_disabled }
					}}
				}},
				{ IDENT_BORDER, &st->item.border.value, {
					{ IDENT_NORMAL, &st->item.border.normal, {
						{ IDENT_DISABLED, &st->item.border.normal_disabled }
					}},
					{ IDENT_SELECT, &st->item.border.select, {
						{ IDENT_DISABLED, &st->item.border.select_disabled }
					}}
				}},
				{ IDENT_OPACITY, &st->item.opacity},
				{ IDENT_RADIUS, &st->item.radius},
				{ IDENT_PREFIX, &st->item.prefix},
				{ IDENT_PADDING, &st->item.padding.value, {
					{ IDENT_LEFT, &st->item.padding.left },
					{ IDENT_TOP, &st->item.padding.top },
					{ IDENT_RIGHT, &st->item.padding.right},
					{ IDENT_BOTTOM, &st->item.padding.bottom }
				}},
				{ IDENT_MARGIN, &st->item.margin.value, {
					{ IDENT_LEFT, &st->item.margin.left },
					{ IDENT_TOP, &st->item.margin.top },
					{ IDENT_RIGHT, &st->item.margin.right},
					{ IDENT_BOTTOM, &st->item.margin.bottom }
				}}
			} };

			SETTING _shadow = { IDENT_SHADOW, &st->shadow.enabled, {
				{IDENT_ENABLED, &st->shadow.enabled},
				{IDENT_SIZE , &st->shadow.size},
				{IDENT_COLOR , &st->shadow.color},
				{IDENT_OPACITY, &st->shadow.opacity},
				{IDENT_OFFSET , &st->shadow.offset}
			} };

			SETTING _border = { IDENT_BORDER, &st->border.size, {
				{IDENT_ENABLED, &st->border.enabled},
				{IDENT_SIZE, &st->border.size},
				{IDENT_COLOR, &st->border.color},
				{IDENT_OPACITY, &st->border.opacity},
				{IDENT_RADIUS, &st->border.radius},
				{IDENT_PADDING, &st->border.padding.value, {
					{ IDENT_LEFT, &st->border.padding.left },
					{ IDENT_TOP, &st->border.padding.top },
					{ IDENT_RIGHT, &st->border.padding.right},
					{ IDENT_BOTTOM, &st->border.padding.bottom }
				}},
			} };

			SETTING _separator = { IDENT_SEPARATOR, &st->separator.color, {
				{IDENT_COLOR, &st->separator.color},
				{IDENT_OPACITY, &st->separator.opacity},
				{IDENT_SIZE, &st->separator.size},
				{IDENT_MARGIN, &st->separator.margin.value, {
					{ IDENT_LEFT, &st->separator.margin.left },
					{ IDENT_TOP, &st->separator.margin.top },
					{ IDENT_RIGHT, &st->separator.margin.right},
					{ IDENT_BOTTOM, &st->separator.margin.bottom }
				}}
			} };

			SETTING _symbol = { IDENT_SYMBOL, &st->symbol.color.value, {
				{IDENT_NORMAL, &st->symbol.color.normal, {
					{ IDENT_DISABLED, &st->symbol.color.normal_disabled }
				}},
				{IDENT_SELECT, &st->symbol.color.select, {
					{ IDENT_DISABLED, &st->symbol.color.select_disabled }
				}},
				{IDENT_CHECKMARK, &st->symbol.checkmark.value, {
					{ IDENT_NORMAL, &st->symbol.checkmark.normal, {
						{ IDENT_DISABLED, &st->symbol.checkmark.normal_disabled }
					}},
					{ IDENT_SELECT, &st->symbol.checkmark.select, {
						{ IDENT_DISABLED, &st->symbol.checkmark.select_disabled }
					}}
				}},
				{IDENT_BULLET, &st->symbol.bullet.value, {
					{ IDENT_NORMAL, &st->symbol.bullet.normal, {
					{ IDENT_DISABLED, &st->symbol.bullet.normal_disabled }
				}},
					{ IDENT_SELECT, &st->symbol.bullet.select, {
						{ IDENT_DISABLED, &st->symbol.bullet.select_disabled }
					}},
				}},
				{IDENT_CHEVRON, &st->symbol.chevron.value, {
					{ IDENT_NORMAL, &st->symbol.chevron.normal, {
					{ IDENT_DISABLED, &st->symbol.chevron.normal_disabled }
				}},
					{ IDENT_SELECT, &st->symbol.chevron.select, {
						{ IDENT_DISABLED, &st->symbol.chevron.select_disabled }
					}},
				}}
			} };

			SETTING _image = { IDENT_IMAGE, &st->image.enabled, {
				{IDENT_ENABLED, &st->image.enabled},
				{IDENT_COLOR, &st->image.color},
				{IDENT_GAP, &st->image.gap},
				{IDENT_SIZE, &st->image.size},
				{IDENT_GLYPH, &st->image.glyph},
				{IDENT_SCALE, &st->image.scale},
				{IDENT_ALIGN, &st->image.display},
				{IDENT_DISPLAY, &st->image.display}
			} };

			SETTING  _layout = { IDENT_LAYOUT, nullptr, {
				{IDENT_WIDTH, &st->layout.width},
				{IDENT_RTL, &st->layout.rtl},
				{IDENT_POPUP, &st->layout.popup.align, {
					{IDENT_ALIGN, &st->layout.popup.align}
				}}
			} };

			SETTING _gradient = { IDENT_GRADIENT, nullptr, {
				{IDENT_ENABLED, &st->gradient.enabled},
				{IDENT_GRADIENT_LINEAR, &st->gradient.linear},
				{IDENT_GRADIENT_RADIAL, &st->gradient.radial},
				{IDENT_GRADIENT_STOP, &st->gradient.stop},
			} };

			SETTING _font = { IDENT_FONT, &st->font.value, {
				{ IDENT_SIZE, &st->font.size },
				{ IDENT_NAME, &st->font.name },
				{ IDENT_FONT_WEIGHT, &st->font.weight },
				{ IDENT_FONT_ITALIC, &st->font.italic }
			} };

			auto sets = &context.Cache->settings;
			SETTING _tip = { IDENT_TIP, &sets->tip.enabled, {
				{IDENT_ENABLED, &sets->tip.enabled},
				{IDENT_DEFAULT, &sets->tip.normal},
				{IDENT_PRIMARY, &sets->tip.primary},
				{IDENT_INFO, &sets->tip.info},
				{IDENT_SUCCESS, &sets->tip.success},
				{IDENT_WARNING, &sets->tip.warning},
				{IDENT_DANGER, &sets->tip.danger},
				{IDENT_BORDER, &sets->tip.border},
				{IDENT_WIDTH, &sets->tip.width},
				{IDENT_OPACITY, &sets->tip.opacity},
				{IDENT_RADIUS, &sets->tip.radius},
				{IDENT_TIME, &sets->tip.time},
				{IDENT_PADDING, &sets->tip.padding.value, {
					{ IDENT_LEFT, &sets->tip.padding.left },
					{ IDENT_TOP, &sets->tip.padding.top },
					{ IDENT_RIGHT, &sets->tip.padding.right},
					{ IDENT_BOTTOM, &sets->tip.padding.bottom }
				}}
			} };

			SETTING _theme = { IDENT_THEME, &st->name, {
				{IDENT_NAME, &st->name},
				{IDENT_DARK, &st->dark},
				{IDENT_VIEW, &st->view},
				{IDENT_BACKGROUND, &st->background.color, {
					{IDENT_COLOR, &st->background.color},
					{IDENT_OPACITY, &st->background.opacity},
					{IDENT_EFFECT, &st->background.effect},
					{IDENT_TINTCOLOR, &st->background.tintcolor},
					{IDENT_IMAGE, &st->background.image},
					_gradient
				}},
				_font, _item, _shadow, _border, _separator, _symbol, _layout, _image, _tip
			} };

			auto _setting = &_theme;
			Ident ident;
			parse_ident(ident, true);

			if(ident.length() > 1)
			{
				_setting = _setting->find(ident);
				ident = ident.back();
			}
			parse_settings(_setting, ident, false);
		}

		void Parser::parse_settings()
		{
			auto sets = &context.Cache->settings;

			SETTING _modify = { IDENT_MODIFY, &sets->modify_items.enabled, {
				{IDENT_ENABLED, &sets->modify_items.enabled},
				{MENU_IMAGE, &sets->modify_items.image},
				{MENU_TITLE, &sets->modify_items.title},
				{MENU_VISIBILITY, &sets->modify_items.visibility},
				{MENU_PARENT, &sets->modify_items.parent},
				{MENU_POSITION, &sets->modify_items.position},
				{MENU_SEPARATOR, &sets->modify_items.separator},
				{IDENT_AUTO, &sets->modify_items.auto_image_group},
				{IDENT_REMOVE, nullptr, {
					{IDENT_DUPLICATE, &sets->modify_items.remove.duplicate },
					{IDENT_DISABLED, &sets->modify_items.remove.disabled },
					{IDENT_SEPARATOR, &sets->modify_items.remove.separator }
				}}
			} };

			SETTING _new = { CONFIG_NEW, &sets->new_items.enabled, {
				{IDENT_ENABLED, &sets->new_items.enabled},
				{MENU_IMAGE, &sets->new_items.image},
			} };

			SETTING _tip = { IDENT_TIP, &sets->tip.enabled, {
				{IDENT_ENABLED, &sets->tip.enabled},
				{IDENT_DEFAULT, &sets->tip.normal},
				{IDENT_PRIMARY, &sets->tip.primary},
				{IDENT_INFO, &sets->tip.info},
				{IDENT_SUCCESS, &sets->tip.success},
				{IDENT_WARNING, &sets->tip.warning},
				{IDENT_DANGER, &sets->tip.danger},
				{IDENT_BORDER, &sets->tip.border},
				{IDENT_WIDTH, &sets->tip.width},
				{IDENT_OPACITY, &sets->tip.opacity},
				{IDENT_RADIUS, &sets->tip.radius},
				{IDENT_TIME, &sets->tip.time},
				{IDENT_PADDING, &sets->tip.padding.value, {
					{ IDENT_LEFT, &sets->tip.padding.left },
					{ IDENT_TOP, &sets->tip.padding.top },
					{ IDENT_RIGHT, &sets->tip.padding.right},
					{ IDENT_BOTTOM, &sets->tip.padding.bottom }
				}}
			}};

			// winx = true is short for winx.replace = true
			SETTING _winx = { IDENT_WINX, &sets->winx.replace, {
				{IDENT_REPLACE, &sets->winx.replace},
				{IDENT_DEBUG, &sets->winx.debug},
			} };

			SETTING _settings = { CONFIG_SETTINGS, &sets->priority, {
				{ IDENT_PRIORITY, &sets->priority },
				{ IDENT_SHOWDELAY, &sets->showdelay },
				{ IDENT_SCREENSHOT, &sets->screenshot.enabled,{
					{ IDENT_ENABLED, &sets->screenshot.enabled },
					{ IDENT_DIRECTORY, &sets->screenshot.directory }
				}},
				{ IDENT_EXCLUDE, &sets->exclude.value,{
					{ IDENT_WHERE, &sets->exclude.value },
					{ IDENT_WINDOW, &sets->exclude.window },
					{ IDENT_PROCESS, &sets->exclude.process }
				}},
				_tip, _modify, _new, _winx,
			} };

			auto _setting = &_settings;
			Ident ident;
			parse_ident(ident, true);
			if(ident.length() > 1)
			{
				_setting = _setting->find(ident);
				ident = ident.back();
			}
			parse_settings(_setting, ident, false);
		}

		uint32_t Parser::parse_image_ident()
		{
			prevCol = l->column - 1;
			Hash h;
			
			auto isq = l->is_quote();
			wchar_t c = 0;

			auto is_normalize = [](const wchar_t &c)->bool
			{
				return !std::iswpunct(c) and !std::iswcntrl(c) and !iswblank(c);
			};

			if(isq) 
			{
				bool last_punct = false;
				auto q = l->tok;
				l->next(); // eat quote
				while(l->tok != q)
				{
					c = l->next();

					if(l->tok == 0)
						break;

					if(c == L'&')
					{
						if(l->tok != L'&') continue;
						l->next();
					}

					if(c == L'_')
					{
						if(last_punct)
							continue;
						last_punct = true;
						h.hash(c);
					}
					else if(is_normalize(c))
					{
						h.hash(c);
						last_punct = false;
					}
					else if(!last_punct)
					{
						if(!l->peek_is(q))
							h.hash(L'_');
						last_punct = true;
					}
				}
				error_if(!l->next_is(q), TokenError::CloseQuoteExpected);
			}
			else if(l->is_alpha())
			{
				while(l->is_iddigit())
				{
					h.hash(l->next());
				}
			}
			if(h.zero()) l->column = prevCol;
			return h;
		}

		//predefined constant variable
		bool Parser::parse_variable(Scope *variables, bool has_sign)
		{
			skip();

			if(has_sign)
			{
				if(l->tok != L'$')
					return false;

				skip();
				prevCol = l->column;
				l->next(); // skip $
			}

			error_if(!l->is_ident(0), TokenError::VariableExpected, prevCol);
			Ident id;
			id.push_back(parse_ident(true));
			prevCol = l->column;
			expect_assign(true);
			prevCol = l->column;
			
			variables->set(id, parse_expression());

			return true;
		}

		bool Parser::parse_image()
		{
			auto cache = Initializer::instance->cache;

			skip();

			if(l->tok != L'@')
				return false;
			
			l->next(); // skip @
			skip();

			prevCol = l->column;

			std::vector<uint32_t> ids;
			int i = 0;
			while(l->is_alpha() || l->is_quote())
			{
				if(auto ident = parse_image_ident(); ident)
				{
					i++;
					ids.push_back(ident);
				}
				skip();
				if(l->next_is(L',')) 
				{
					skip();
					if(l->tok == L'@')
					{
						l->next(); // skip @
						skip();
					}
				}
			}

			if(i == 0)
			{
				error_if(!l->eof, TokenError::IdentifierExpected, prevCol);
				return false;
			}

			expect_assign(true);
			cache->add_image(std::move(ids), parse_root_expression());
			return true;
		}

		// ... load data from disk and populate
		int Parser::load_import(size_t line, size_t col, bool ignore_failed, bool parse_import)
		{
			skip();
			
			if(parse_import)
			{
				if(!l->skip_import())
					return -1;
			}

			std::unique_ptr<Expression> epath(parse_root_expression());
			error_if(!epath, TokenError::ImportPathExpected, prevCol);

			Object obj = context.Eval(epath.get()).move();

			if(obj.is_null())
				return 0;

			string path = obj.to_string().trim().move();

			if(path.empty())
				return 0;
			
			// Root every relative import against the importing file's own
			// directory. The old length > 2 guard let short names such as "a"
			// through unrooted, to be resolved against the process working
			// directory, which is not ours to rely on.
			if(!Path::IsAbsolute(path))
				path = Path::Combine(l->location, path).move();

			path = Path::FixSeparator(path).move();

			// Legacy alias: the Ukrainian translation used to ship as "ua.nss"
			// (country code); it is now "uk.nss" (ISO 639-1 language code).
			// Keep existing configs that still import "ua.nss" working.
			if(path.ends_with(L"\\ua.nss") && !Path::IsFileExists(path))
			{
				auto alias = (path.substr(0, path.length() - 6) + L"uk.nss").move();
				if(Path::IsFileExists(alias))
					path = alias.move();
			}

			auto hash = path.hash();
			
			if(hash)
			{
				for(auto &h : m_imports)
				{
					if(h == hash)
					{
						//if (!ignore_duplicated)
						{
							__trace(L"line[%d] column[%d] already imported '%s'",
											line, col, path.c_str());
							//return false;
						}
						break;
					}
				}
				m_imports.push_back(hash);
			}

			if(_imports.size() >= MaxImportDepth)
			{
				Logger::Error(L"line[%d] column[%d] import nesting deeper than %d, refusing '%s'",
							  line, col, static_cast<int>(MaxImportDepth), path.c_str());
				string text;
				text.format(L"import nesting deeper than %d, refusing '%s'", static_cast<int>(MaxImportDepth), path.c_str());
				diagnose(false, l->path, line, col, text);
				return 0;
			}

			auto lex = import_push();

			if(hash)
				add_source(path);
			if(hash && lex->load_File(path, ignore_failed))
			{
				__trace(L"import '%s'", path.c_str());

				l = lex;
				skip();
				prevCol = l->column;
				return 1;
			}

			// Not fatal: the import is skipped and the parse goes on.
			string text;
			if(lex->error != TokenError::None)
			{
				Logger::Error(L"line[%d] column[%d], %s '%s'",
							  line, col, ParserException::errortostr(lex->error), l->path.c_str());
				text.format(L"import '%s': %s", path.c_str(), ParserException::errortostr(lex->error));
			}
			else
			{
				Logger::Warning(L"line[%d] column[%d] invalid import file, '%s'",
								line, col, path.c_str());
				text.format(L"invalid import file '%s'", path.c_str());
			}
			diagnose(false, l->path, line, col, text);

			pop_import();

			return 0;
		}

		void Parser::parse_loc(bool has_curly, Scope *into)
		{
			if(!into)
				into = &context.Cache->variables.loc;
			if(has_curly)
				expect_openCurly();
			while(!l->eof)
			{
				if(has_curly && l->tok == L'}')
					break;
				parse_variable(into, false);
			}
			if(has_curly)
				expect_closeCurly();
		}

		void Parser::note_loc(uint32_t key, const string &name, size_t line, size_t column)
		{
			if(m_loc_refs.contains(key) || !l)
				return;

			LocRef ref;
			ref.name = name;
			ref.file = l->path;
			ref.line = line;
			ref.column = column;
			ref.order = m_loc_refs.size();
			m_loc_refs.emplace(key, std::move(ref));
		}

		// A string with nothing to evaluate: "text", or 'text' (parsed as an
		// interpolated string) with no @ or % parts.
		static bool is_literal(const Expression *e)
		{
			if(!e)
				return false;
			if(e->Type() == ExpressionType::String)
				return true;
			if(e->Type() != ExpressionType::Statement)
				return false;
			auto stmt = static_cast<const StatementExpression *>(e);
			if(!stmt->HasReturn)
				return false;
			for(auto part : stmt->Body)
			{
				if(!part || part->Type() != ExpressionType::String)
					return false;
			}
			return true;
		}

		void Parser::record_title(const Expression *e, size_t line, size_t column)
		{
			// Only -config:check (which collects diagnostics) reports titles.
			if(!e || !Diagnostics)
				return;

			TitleRef ref;
			ref.file = l ? l->path : m_path;
			ref.line = line;
			ref.column = column;
			if(e == m_last_ident.expr)
			{
				ref.root = m_last_ident.root;
				ref.key = m_last_ident.key;
				ref.name = m_last_ident.name;
			}
			else
			{
				ref.literal = is_literal(e);
			}
			// Keyed by address: a later title at a reused address replaces it.
			m_titles.insert_or_assign(e, std::move(ref));
		}

		// "file_manage" -> "File manage", for a loc name defined nowhere.
		static string humanize_loc_name(const string &name)
		{
			std::wstring text;
			for(auto c : std::wstring_view(name.c_str(), name.length()))
				text += (c == L'_' || c == L'-') ? L' ' : c;
			if(!text.empty())
				text[0] = static_cast<wchar_t>(std::towupper(text[0]));
			return string(text);
		}

		bool Parser::load_loc_file(const string &path, Scope &into)
		{
			// A broken fallback file must not fail the configuration that only
			// borrowed it: its errors are rolled back and it is skipped.
			auto saved_error = m_error;
			auto saved_code = error_code;
			auto saved_diagnostics = Diagnostics ? Diagnostics->size() : 0;

			bool ok = false;
			auto lex = import_push();
			add_source(path);
			try
			{
				if(lex->load_File(path, true))
				{
					l = lex;
					skip();
					parse_loc(false, &into);
					ok = !m_error;
				}
			}
			catch(...)
			{
				ok = false;
			}
			pop_import();

			if(!ok)
			{
				m_error = saved_error;
				error_code = saved_code;
				if(Diagnostics && Diagnostics->size() > saved_diagnostics)
					Diagnostics->resize(saved_diagnostics);
				into.clear(true);
			}
			return ok;
		}

		void Parser::resolve_loc()
		{
			auto &loc = context.Cache->variables.loc;

			std::vector<std::pair<uint32_t, const LocRef *>> missing;
			for(auto &[key, ref] : m_loc_refs)
			{
				if(!loc.exists(key))
					missing.emplace_back(key, &ref);
			}
			if(missing.empty())
				return;

			std::sort(missing.begin(), missing.end(),
					  [](auto &a, auto &b) { return a.second->order < b.second->order; });

			// The configuration imports no language file (an older shell.nss
			// with newer imports), or one that lacks these names: English, from
			// imports\lang\en.nss next to the root config, then (for names an
			// older copy there lacks) the one in the install folder. Read here,
			// once per load, into this load's cache: nothing is read when a menu
			// opens, and a reload starts again from a new cache.
			string english_path;
			const LocRef *first_english = nullptr;
			uint32_t from_english = 0;
			string dirs[] = { location, context.Application ? string(context.Application->Dirctory) : string() };
			for(size_t d = 0; d < std::size(dirs) && !missing.empty(); d++)
			{
				if(dirs[d].empty() || (d > 0 && dirs[d].equals(dirs[0], true)))
					continue;
				auto path = Path::Combine(dirs[d], L"imports\\lang\\en.nss").move();
				Scope english;
				if(!Path::IsFileExists(path) || !load_loc_file(path, english))
					continue;

				for(auto it = missing.begin(); it != missing.end();)
				{
					if(auto e = english.detach(it->first))
					{
						loc.set(it->first, e);
						if(!first_english)
						{
							first_english = it->second;
							english_path = path;
						}
						from_english++;
						it = missing.erase(it);
					}
					else
						++it;
				}
			}

			// Defined nowhere: a readable form of the name, never a blank title.
			for(auto &[key, ref] : missing)
			{
				auto text = humanize_loc_name(ref->name).move();
				loc.set(key, text);

				string message;
				message.format(L"loc.%s is not defined in any language file; it shows as '%s'",
							   ref->name.c_str(), text.c_str());
				Logger::Warning(L"line[%d] column[%d] %s", static_cast<int>(ref->line), static_cast<int>(ref->column), message.c_str());
				diagnose(false, ref->file, ref->line, ref->column, message);
			}

			if(from_english)
			{
				string message;
				message.format(L"no imported language file defines %u of the loc.* names used (such as loc.%s); "
							   L"the English text from '%s' is used. Add the localization block (import lang ...) "
							   L"from the default shell.nss to get translated titles",
							   from_english, first_english->name.c_str(), english_path.c_str());
				Logger::Info(L"%s", message.c_str());
				diagnose(false, _imports.front()->path, 0, 0, message);
			}
		}

		bool Parser::ReportTitles(std::vector<TitleInfo> &out, const wchar_t *where)
		{
			// The selection a right-click in `where` makes, as Selections
			// describes it; only type= is tested (where= and vis= need the real
			// selection and are not evaluated).
			int32_t types[FSO_MAX]{};
			bool background = false;
			size_t count = 1;
			bool filter = where && *where;
			if(filter)
			{
				string name = where;
				if(name.equals(L"file", true))
					types[FSO_FILE] = 1;
				else if(name.equals(L"folder", true) || name.equals(L"dir", true))
					types[FSO_DIRECTORY] = 1;
				else if(name.equals(L"back", true) || name.equals(L"background", true))
				{
					background = true;
					types[FSO_BACK_DIRECTORY] = 1;
				}
				else if(name.equals(L"desktop", true))
				{
					background = true;
					types[FSO_DESKTOP] = 1;
				}
				else if(name.equals(L"taskbar", true))
				{
					types[FSO_TASKBAR] = 1;
					count = 0;
				}
				else
					return false;

				types[FSO_BACK] = background;
				for(int32_t i = 0; i < FSO_BACK; i++)
					types[FSO_COUNT] += types[i];
			}

			auto &loc = context.Cache->variables.loc;

			auto report = [&](NativeMenu *item, uint32_t depth, TitleInfo::Kind kind)
			{
				auto e = item->title.get();
				if(!e)
					return;

				TitleInfo info;
				info.state = TitleInfo::Dynamic;
				info.kind = kind;
				info.depth = depth;
				if(auto it = m_titles.find(e); it != m_titles.end())
				{
					auto &ref = it->second;
					info.file = ref.file;
					info.line = ref.line;
					info.column = ref.column;
					if(ref.root)
						info.source.format(L"%s.%s", ref.root == IDENT_LOC ? L"loc" : L"title", ref.name.c_str());

					Expression *value = nullptr;
					if(ref.literal)
						value = e;
					else if(ref.root == IDENT_LOC)
					{
						// Undefined (null) evaluates to an empty title.
						value = loc.at(ref.key);
						if(!value)
							info.state = TitleInfo::Empty;
						else if(!is_literal(value))
							value = nullptr;	// e.g. 'Copy @sel.count paths'
					}
					else if(ref.root == IDENT_TITLE)
					{
						info.state = TitleInfo::Empty;
						if(auto uid = Initializer::get_muid(ref.key); uid)
						{
							info.text = uid->get_title().move();
							if(!info.text.empty())
								info.state = TitleInfo::Text;
						}
					}

					if(value)
					{
						info.text = context.Eval(value).to_string().move();
						info.state = info.text.empty() ? TitleInfo::Empty : TitleInfo::Text;
					}
				}
				out.push_back(std::move(info));
			};

			// A menu that does not show in `where` hides everything in it.
			std::function<void(std::vector<NativeMenu *> &, uint32_t)> walk = [&](std::vector<NativeMenu *> &items, uint32_t depth)
			{
				for(auto item : items)
				{
					if(!item)
						continue;
					if(filter && !Selections::match_types(item->fso, types, background, count))
						continue;
					report(item, depth, item->type == NativeMenuType::Menu ? TitleInfo::Menu : TitleInfo::Item);
					walk(item->items, depth + 1);
				}
			};

			walk(context.Cache->dynamic.items, 0);
			for(auto item : context.Cache->statics)
			{
				if(!item)
					continue;
				if(filter && !Selections::match_types(item->fso, types, background, count))
					continue;
				report(item, 0, TitleInfo::Modify);
			}
			return true;
		}

		// load data from disk and populate
		void Parser::parse_config()
		{
			auto cache = context.Cache;
			while(!l->eof)
			{
				skip();
				prevCol = l->column;
				
				if(l->peek_ident(IDENT_THEME))
				{
					parse_theme();
					continue;
				}
				else if(l->peek_ident(CONFIG_SETTINGS))
				{
					parse_settings();
					continue;
				}

				Hash id = parse_ident();
				switch(id)
				{
					case IDENT_THEME:
						parse_theme();
						break;
					case CONFIG_SETTINGS:
						parse_settings();
						break;
					case CONFIG_SET:
					case CONFIG_MODIFY:
					case CONFIG_UPDATE:
					case CONFIG_CHANGE:
					case IDENT_REMOVE:
						parse_modify_items(id);
						break;
					case CONFIG_MENU:
					case CONFIG_ITEM:
					case CONFIG_SEP:
					case CONFIG_SEPARATOR:
					{
						std::unique_ptr<NativeMenu> item(new NativeMenu(&cache->dynamic));
						if(parse_menu_item(item.get(), id))
						{
							cache->dynamic.items.push_back(item.release());
							TotalMenuCount++;
						}
						break;
					}
					case IDENT_IMPORT:
					{
						skip();
						bool bloc = false;
						if(l->peek_ident(IDENT_LOC))
						{
							bloc = true;
							l->next(3);
						}
						else if(l->peek_ident(IDENT_LANG))
						{
							bloc = true;
							l->next(4);
						}

						auto ret = load_import(l->line, l->column, true, false);
						if(ret == 1)
						{
							if(bloc)
								parse_loc(false);
							else
								parse_config();
							pop_import();
						}
						break;
					}
					case IDENT_LANG:
					case IDENT_LOC:
					{
						parse_loc(true);
						break;
					}
					default:
					{
						if(id == 0)
						{
							if(l->eof)
								break;
							if(parse_variable(&cache->variables.global))
								break;
							if(parse_image())
								break;
						}
						error(TokenError::IdentifierConfigUnexpected, prevCol);
						break;
					}
				}
			}
		}

		//expected a ')'
		//params
		bool Parser::Load()
		{
			bool result = false;
			try
			{
				_theme = { 
					0, IDENT_BACKGROUND, {
						{ 0, IDENT_OPACITY },
						{ 0, IDENT_EFFECT }
					}
				};

				if(_imports.empty() || (l->length == 0 && !m_error))
					return true;
			
				context.Runtime = false;

				auto cache = context.Cache;
				cache->dynamic.type = NativeMenuType::Menu;
				cache->dynamic.fso.set(TRUE);

				location = Path::Parent(l->path);
				parse_config();
				// Before the cache goes live: no title may be blank.
				if(!m_error)
					resolve_loc();

				result = /*parse_config() && */!m_error;
				if(result)
				{
				}
			}
			catch(const ParserException&)
			{
			}
			catch(...)
			{
#ifdef _DEBUG
				Logger::Exception(__func__);
#endif
			}
			return result;
		}
	}
}