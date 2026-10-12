#include "pawn_instrumenter.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace memguard::instrument {

namespace {

	enum class Tok
	{
		Ident,
		Number,
		String,
		Char,
		Punct,
		Directive
	};

	struct Token
	{
		Tok type;
		std::size_t begin; // byte offset in source
		std::size_t end;
		int line;
		std::string_view text;
	};

	bool identStart(char c)
	{
		return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '@';
	}

	bool identChar(char c)
	{
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '@';
	}

	struct Lexer
	{
		std::string_view src;
		std::size_t pos = 0;
		int line = 1;
		bool atLineStart = true;
		char escape = '\\';
		std::vector<Token> tokens;
		std::string error;
		int errorLine = 0;

		bool fail(const std::string& message, int at)
		{
			error = message;
			errorLine = at;
			return false;
		}

		void newline()
		{
			++line;
			atLineStart = true;
		}

		bool run()
		{
			while (pos < src.size())
			{
				const char c = src[pos];
				if (c == '\n')
				{
					newline();
					++pos;
					continue;
				}
				if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v')
				{
					++pos;
					continue;
				}
				if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/')
				{
					while (pos < src.size() && src[pos] != '\n')
					{
						++pos;
					}
					continue;
				}
				if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '*')
				{
					const int startLine = line;
					pos += 2;
					while (pos + 1 < src.size() && !(src[pos] == '*' && src[pos + 1] == '/'))
					{
						if (src[pos] == '\n')
						{
							++line;
						}
						++pos;
					}
					if (pos + 1 >= src.size())
					{
						return fail("unterminated /* comment", startLine);
					}
					pos += 2;
					continue;
				}
				if (c == '#' && atLineStart)
				{
					if (!directive())
					{
						return false;
					}
					continue;
				}
				atLineStart = false;
				const std::size_t start = pos;
				const int startLine = line;
				if (identStart(c))
				{
					while (pos < src.size() && identChar(src[pos]))
					{
						++pos;
					}
					push(Tok::Ident, start, startLine);
					continue;
				}
				if (std::isdigit(static_cast<unsigned char>(c)))
				{
					while (pos < src.size() && (identChar(src[pos]) || src[pos] == '.'))
					{
						// 1.5e-3 : allow a sign right after an exponent marker
						if ((src[pos] == 'e' || src[pos] == 'E') && pos + 1 < src.size() && (src[pos + 1] == '-' || src[pos + 1] == '+'))
						{
							++pos;
						}
						++pos;
					}
					push(Tok::Number, start, startLine);
					continue;
				}
				if (c == '"' || (c == '!' && pos + 1 < src.size() && src[pos + 1] == '"'))
				{
					if (c == '!')
					{
						++pos;
					}
					if (!quoted('"', false, startLine))
					{
						return false;
					}
					push(Tok::String, start, startLine);
					continue;
				}
				if ((c == '\\' || c == escape) && pos + 1 < src.size() && (src[pos + 1] == '"' || (src[pos + 1] == '!' && pos + 2 < src.size() && src[pos + 2] == '"')))
				{
					// raw string: \"..." or \!"..." - no escape processing
					++pos;
					if (src[pos] == '!')
					{
						++pos;
					}
					if (!quoted('"', true, startLine))
					{
						return false;
					}
					push(Tok::String, start, startLine);
					continue;
				}
				if (c == '\'')
				{
					if (!quoted('\'', false, startLine))
					{
						return false;
					}
					push(Tok::Char, start, startLine);
					continue;
				}
				++pos;
				push(Tok::Punct, start, startLine);
			}
			return true;
		}

		void push(Tok type, std::size_t start, int startLine)
		{
			tokens.push_back(Token { type, start, pos, startLine, src.substr(start, pos - start) });
		}

		bool quoted(char quote, bool raw, int startLine)
		{
			++pos; // opening quote
			while (pos < src.size())
			{
				const char c = src[pos];
				if (c == '\n')
				{
					return fail(std::string("unterminated ") + (quote == '"' ? "string" : "character") + " literal", startLine);
				}
				if (!raw && c == escape && pos + 1 < src.size())
				{
					if (src[pos + 1] == '\n')
					{
						++line; // line continuation inside a string
					}
					pos += 2;
					continue;
				}
				++pos;
				if (c == quote)
				{
					return true;
				}
			}
			return fail("unterminated literal", startLine);
		}

		bool directive()
		{
			const std::size_t start = pos;
			const int startLine = line;
			while (pos < src.size())
			{
				const char c = src[pos];
				if (c == '\n')
				{
					// continuation: backslash (optionally followed by \r) right before the newline
					std::size_t back = pos;
					if (back > start && src[back - 1] == '\r')
					{
						--back;
					}
					if (back > start && src[back - 1] == '\\')
					{
						++line;
						++pos;
						continue;
					}
					break;
				}
				if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '*')
				{
					// a block comment inside a directive may span lines
					pos += 2;
					while (pos + 1 < src.size() && !(src[pos] == '*' && src[pos + 1] == '/'))
					{
						if (src[pos] == '\n')
						{
							++line;
						}
						++pos;
					}
					pos = std::min(pos + 2, src.size());
					continue;
				}
				++pos;
			}
			tokens.push_back(Token { Tok::Directive, start, pos, startLine, src.substr(start, pos - start) });
			handlePragmaCtrlchar(src.substr(start, pos - start));
			return true;
		}

		void handlePragmaCtrlchar(std::string_view d)
		{
			// #pragma ctrlchar '$'  (changes the escape character)
			const std::size_t p = d.find("ctrlchar");
			if (p == std::string_view::npos || d.find("pragma") == std::string_view::npos)
			{
				return;
			}
			const std::size_t q = d.find('\'', p);
			if (q != std::string_view::npos && q + 2 < d.size() && d[q + 2] == '\'')
			{
				escape = d[q + 1];
			}
		}
	};

	std::string_view directiveName(std::string_view d)
	{
		std::size_t i = 1; // skip '#'
		while (i < d.size() && (d[i] == ' ' || d[i] == '\t'))
		{
			++i;
		}
		std::size_t j = i;
		while (j < d.size() && identChar(d[j]))
		{
			++j;
		}
		return d.substr(i, j - i);
	}

	bool isPunct(const Token& t, char c)
	{
		return t.type == Tok::Punct && t.text.size() == 1 && t.text[0] == c;
	}

	bool isIdent(const Token& t, std::string_view s)
	{
		return t.type == Tok::Ident && t.text == s;
	}

	// Tags that only describe a value type; dropped from zone names ("Float:GetSpeed" -> "GetSpeed").
	bool isValueTag(std::string_view tag)
	{
		static const char* const tags[] = { "Float", "bool", "_", "File", "Text", "Text3D", "PlayerText", "PlayerText3D",
			"DB", "DBResult", "Menu", "Fixed", "WEAPON", "VEHICLE_PANEL_STATUS", "Timer", "Iterator", "char", "E_STREAMER" };
		for (const char* t : tags)
		{
			if (tag == t)
			{
				return true;
			}
		}
		return false;
	}

	bool nameMatches(const std::vector<std::string>& patterns, std::string_view name, std::string_view zone)
	{
		for (const std::string& p : patterns)
		{
			if (wildcardMatch(p, name) || wildcardMatch(p, zone))
			{
				return true;
			}
		}
		return false;
	}

} // namespace

bool wildcardMatch(std::string_view pattern, std::string_view text)
{
	std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
	while (t < text.size())
	{
		if (p < pattern.size() && pattern[p] == '*')
		{
			star = p++;
			mark = t;
		}
		else if (p < pattern.size() && pattern[p] == text[t])
		{
			++p;
			++t;
		}
		else if (star != std::string_view::npos)
		{
			p = star + 1;
			t = ++mark;
		}
		else
		{
			return false;
		}
	}
	while (p < pattern.size() && pattern[p] == '*')
	{
		++p;
	}
	return p == pattern.size();
}

int Result::instrumentedCount() const
{
	return static_cast<int>(std::count_if(functions.begin(), functions.end(), [](const FunctionInfo& f) { return f.instrumented; }));
}

Result instrumentSource(std::string_view source, const Options& options)
{
	Result result;
	Lexer lex;
	lex.src = source;
	if (!lex.run())
	{
		result.error = lex.error;
		result.errorLine = lex.errorLine;
		return result;
	}
	const std::vector<Token>& toks = lex.tokens;

	for (const Token& t : toks)
	{
		if (t.type == Tok::Directive && directiveName(t.text) == "include" && t.text.find("memguard") != std::string_view::npos)
		{
			result.includesMemguard = true;
		}
	}

	struct Insertion
	{
		std::size_t offset;
		std::string text;
	};
	std::vector<Insertion> insertions;

	// Only file-scope tokens are examined here: every brace block found at file scope is skipped
	// as a whole (function body, enum, array initialiser), so this loop never runs inside one.
	int parenDepth = 0;
	std::size_t stmtStart = 0;
	for (std::size_t i = 0; i < toks.size(); ++i)
	{
		const Token& t = toks[i];
		if (t.type == Tok::Directive)
		{
			continue;
		}
		{
			if (isPunct(t, '('))
			{
				++parenDepth;
				continue;
			}
			if (isPunct(t, ')'))
			{
				if (--parenDepth < 0)
				{
					result.error = "unbalanced ')'";
					result.errorLine = t.line;
					return result;
				}
				continue;
			}
			if (parenDepth == 0 && isPunct(t, ';'))
			{
				stmtStart = i + 1;
				continue;
			}
			if (isPunct(t, '}'))
			{
				result.error = "unbalanced '}' at top level";
				result.errorLine = t.line;
				return result;
			}
			if (!isPunct(t, '{') || parenDepth != 0)
			{
				continue;
			}

			// '{' at file scope: decide whether it opens a function body.
			std::vector<std::size_t> hdr; // non-directive token indices of the statement
			for (std::size_t k = stmtStart; k < i; ++k)
			{
				if (toks[k].type != Tok::Directive)
				{
					hdr.push_back(k);
				}
			}

			// Find the end of this brace block.
			int d = 0;
			std::size_t close = i;
			bool hasEmit = false;
			bool alreadyInstrumented = false;
			for (std::size_t k = i; k < toks.size(); ++k)
			{
				const Token& b = toks[k];
				if (b.type == Tok::Directive)
				{
					if (directiveName(b.text) == "emit")
					{
						hasEmit = true;
					}
					continue;
				}
				if (isIdent(b, "__emit"))
				{
					hasEmit = true;
				}
				if (b.type == Tok::Ident && b.text == options.macro)
				{
					alreadyInstrumented = true;
				}
				if (isPunct(b, '{'))
				{
					++d;
				}
				else if (isPunct(b, '}') && --d == 0)
				{
					close = k;
					break;
				}
			}
			if (close == i)
			{
				result.error = "unbalanced '{' (no matching '}')";
				result.errorLine = t.line;
				return result;
			}

			bool isFunction = false;
			std::string name;
			std::string zone;
			bool isPublic = false;
			std::string notFunctionReason;
			if (!hdr.empty())
			{
				const Token& first = toks[hdr.front()];
				bool hasAssign = false;
				for (std::size_t k : hdr)
				{
					if (isPunct(toks[k], '='))
					{
						hasAssign = true;
					}
				}
				const bool declKeyword = isIdent(first, "enum") || isIdent(first, "native") || isIdent(first, "forward") || isIdent(first, "new");
				if (!hasAssign && !declKeyword)
				{
					// Optional state specification "<...>" after the parameter list.
					std::size_t e = hdr.size();
					if (e > 0 && isPunct(toks[hdr[e - 1]], '>'))
					{
						std::size_t k = e - 1;
						while (k > 0 && !isPunct(toks[hdr[k]], '<'))
						{
							--k;
						}
						e = k;
					}
					if (e > 0 && isPunct(toks[hdr[e - 1]], ')'))
					{
						// matching '('
						int pd = 0;
						std::size_t k = e - 1;
						for (;; --k)
						{
							if (isPunct(toks[hdr[k]], ')'))
							{
								++pd;
							}
							else if (isPunct(toks[hdr[k]], '(') && --pd == 0)
							{
								break;
							}
							if (k == 0)
							{
								break;
							}
						}
						if (pd == 0 && k > 0 && toks[hdr[k - 1]].type == Tok::Ident)
						{
							isFunction = true;
							const std::size_t nameIdx = k - 1;
							name = std::string(toks[hdr[nameIdx]].text);
							bool isOperator = false;
							std::string prefixWords;
							std::string tag;
							for (std::size_t m = 0; m < nameIdx; ++m)
							{
								const Token& w = toks[hdr[m]];
								if (isIdent(w, "operator"))
								{
									isOperator = true;
								}
								else if (isIdent(w, "public"))
								{
									isPublic = true;
								}
								else if (w.type == Tok::Ident && m + 1 < nameIdx + 1 && isPunct(toks[hdr[m + 1]], ':'))
								{
									tag = std::string(w.text);
								}
								else if (w.type == Tok::Ident && !isIdent(w, "stock") && !isIdent(w, "static") && !isIdent(w, "const"))
								{
									prefixWords += std::string(w.text) + " ";
								}
							}
							if (isOperator)
							{
								notFunctionReason = "operator function";
							}
							zone = prefixWords;
							if (!tag.empty() && !isValueTag(tag))
							{
								zone += tag + ":";
							}
							zone += name;
							// state functions: "Foo<state>"
							if (e != hdr.size())
							{
								std::string states;
								for (std::size_t m = e; m < hdr.size(); ++m)
								{
									states += std::string(toks[hdr[m]].text);
								}
								zone += states;
							}
						}
					}
				}
			}

			if (isFunction)
			{
				FunctionInfo info;
				info.name = name;
				info.zone = options.prefix + zone;
				info.line = t.line;
				info.isPublic = isPublic;
				if (!notFunctionReason.empty())
				{
					info.skipReason = notFunctionReason;
				}
				else if (info.name.rfind("MemGuard_", 0) == 0 || info.name.rfind("__mgx", 0) == 0)
				{
					info.skipReason = "MemGuard X API function";
				}
				else if (alreadyInstrumented)
				{
					info.skipReason = "already contains " + options.macro;
				}
				else if (hasEmit && !options.allowEmit)
				{
					info.skipReason = "contains #emit/__emit (use --allow-emit to override)";
				}
				else if (options.onlyPublic && !isPublic)
				{
					info.skipReason = "not public (--only-public)";
				}
				else if (!options.include.empty() && !nameMatches(options.include, info.name, info.zone))
				{
					info.skipReason = "not matched by --only";
				}
				else if (nameMatches(options.exclude, info.name, info.zone))
				{
					info.skipReason = "excluded";
				}
				else if (info.zone.find('"') != std::string::npos || info.zone.find('\\') != std::string::npos)
				{
					info.skipReason = "zone name contains quote/backslash";
				}
				else
				{
					info.instrumented = true;
					// Insert in front of the first statement when it starts on a later line, so the
					// statement keeps that line's indentation (no "loose indentation" warning) and
					// line numbers are unchanged. If a preprocessor line comes first, the first
					// statement may be conditional, so insert right after the brace instead.
					std::size_t at = t.end;
					std::string text = " " + options.macro + "(\"" + info.zone + "\");";
					std::size_t k = i + 1;
					const bool directiveFirst = k < toks.size() && toks[k].type == Tok::Directive;
					if (!directiveFirst && k < toks.size() && toks[k].line > t.line)
					{
						at = toks[k].begin;
						text = options.macro + "(\"" + info.zone + "\"); ";
					}
					insertions.push_back(Insertion { at, text });
				}
				result.functions.push_back(std::move(info));
			}

			// Continue after the block.
			i = close;
			stmtStart = close + 1;
			continue;
		}
	}
	if (parenDepth != 0)
	{
		result.error = "unbalanced '(' at end of file";
		return result;
	}

	result.output.reserve(source.size() + insertions.size() * 40);
	std::size_t last = 0;
	for (const Insertion& ins : insertions)
	{
		result.output.append(source.substr(last, ins.offset - last));
		result.output.append(ins.text);
		last = ins.offset;
	}
	result.output.append(source.substr(last));
	result.ok = true;
	return result;
}

} // namespace memguard::instrument
