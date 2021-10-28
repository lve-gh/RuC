/*
 *	Copyright 2021 Andrey Terekhov, Egor Anikin
 *
 *	Licensed under the Apache License, Version 2.0 (the "License");
 *	you may not use this file except in compliance with the License.
 *	You may obtain a copy of the License at
 *
 *		http://www.apache.org/licenses/LICENSE-2.0
 *
 *	Unless required by applicable law or agreed to in writing, software
 *	distributed under the License is distributed on an "AS IS" BASIS,
 *	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *	See the License for the specific language governing permissions and
 *	limitations under the License.
 */

#include "parser.h"
#include <string.h>
#include "error.h"
#include "keywords.h"
#include "uniio.h"
#include "uniprinter.h"
#include "uniscanner.h"


const size_t FST_LINE_INDEX =		1;
const size_t FST_CHARACTER_INDEX =	0;


/**
 *	Checks if сharacter is separator
 *
 *	@param	symbol	UTF-8 сharacter
 *
 *	@return	@c 1 on true, @c 0 on false
 */
static bool utf8_is_separator(const char32_t symbol)
{
	return  symbol == U' ' || symbol == U'\t';
}

/**
 *	Checks if сharacter is line_breaker
 *
 *	@param	symbol	UTF-8 сharacter
 *
 *	@return	@c 1 on true, @c 0 on false
 */
static bool utf8_is_line_breaker(const char32_t symbol)
{
	return  symbol == U'\r' || symbol == U'\n';
}


/**
 *	Увеличивает значение line и сбрасывает значение position
 */
static inline void parser_next_line(parser *const prs)
{
	prs->line_position = in_get_position(prs->in);
	prs->line++;
	prs->position = FST_CHARACTER_INDEX;
	strings_clear(&prs->string);
	prs->string = strings_create(256);
}

/**
 *	Печатает в prs.out строку кода
 */
static inline void parser_print(parser *const prs)
{
	const size_t size = strings_size(&prs->string);
	for(size_t i = 0; i < size; i++)
	{
		uni_printf(prs->out, "%s", strings_get(&prs->string, i));
	}

	parser_next_line(prs);
}

/**
 *	Добавляет str в string
 */
static inline size_t parser_add_string(parser *const prs, const char *const str)
{
	if (str == NULL)
	{
		return 0;
	}

	strings_add(&prs->string, str);
	return strlen(str);
}

/**
 *	Добавляет символ в string и увеличивает значение position
 */
static inline void parser_add_char(parser *const prs, const char32_t cur)
{
	char buffer[9];
	utf8_to_string(buffer, cur);

	strings_add(&prs->string, buffer);
	prs->position++;
}

/**
 *	Сохраняет считанный код
 */
static inline size_t parser_add_to_buffer(char *const buffer, const char *const str)
{
	if (str == NULL)
	{
		return 0;
	}

	strcat(buffer, str);
	return strlen(str);
}

/**
 *	Сохраняет считанный символ
 */
static inline void parser_add_char_to_buffer(char *const buffer, const char32_t ch)
{
	utf8_to_string(&buffer[strlen(buffer)], ch);
}

/**
 *	Заполняет string до конца строки
 */
/*static inline char parser_fill_string(parser *const prs)
{
	char str[256];
	str[0] = '\0';

	char32_t cur = uni_scan_char(prs->in);
	while (!utf8_is_line_breaker(cur) && cur != (char32_t)EOF)
	{
		utf8_to_string(&str[strlen(str)], cur);
		cur = uni_scan_char(prs->in);
	}

	return str;
}*/


/**
 *	Emit an error from parser
 *
 *	@param	prs			Parser structure
 *	@param	num			Error code
 */
static void parser_macro_error(parser *const prs, const error_t num)
{
	if (parser_is_correct(prs) && !prs->is_recovery_disabled)
	{
		size_t position = in_get_position(prs->in);
		in_set_position(prs->in, prs->line_position);

		char str[256];
		str[0] = '\0';

		char32_t cur = uni_scan_char(prs->in);
		while (!utf8_is_line_breaker(cur) && cur != (char32_t)EOF)
		{
			utf8_to_string(&str[strlen(str)], cur);
			cur = uni_scan_char(prs->in);
		}

		prs->was_error = true;
		macro_error(linker_current_path(prs->lk), str, prs->line, prs->position, num);
		in_set_position(prs->in, position);
	}
}

/**
 *	Emit an warning from parser
 *
 *	@param	prs			Parser structure
 *	@param	num			Error code
 */
static void parser_macro_warning(parser *const prs, const warning_t num)
{
	if (parser_is_correct(prs))
	{
		size_t position = in_get_position(prs->in);
		in_set_position(prs->in, prs->line_position);

		char str[256];
		str[0] = '\0';

		char32_t cur = uni_scan_char(prs->in);
		while (!utf8_is_line_breaker(cur) && cur != (char32_t)EOF)
		{
			utf8_to_string(&str[strlen(str)], cur);
			cur = uni_scan_char(prs->in);
		}

		macro_warning(linker_current_path(prs->lk), str, prs->line, prs->position, num);
		in_set_position(prs->in, position);
	}
}


/**
 *	Считывает символы до конца строковой константы и буфферизирует текущую строку кода
 */
static void parser_skip_string(parser *const prs, const char32_t ch)
{
	const size_t position = prs->position;		// Позиция начала строковой константы

	bool was_slash = false;
	parser_add_char(prs, ch);					// Вывод символа начала строковой константы

	char32_t cur = uni_scan_char(prs->in);
	while (cur != (char32_t)EOF)
	{
		if (cur == ch)
		{
			parser_add_char(prs, cur);
			if (was_slash)
			{
				was_slash = cur == U'\\' ? true : false;
			}
			else
			{
				return;							// Строка считана, выход из функции
			}
		}
		else if (utf8_is_line_breaker(cur))		// Ошибка из-за наличия переноса строки
		{
			prs->position = position;
			parser_macro_error(prs, PARSER_STRING_NOT_ENDED);

			if (prs->is_recovery_disabled)		// Добавление '\"' в конец незаконченной строковой константы
			{
				parser_add_char(prs, ch);
			}

			if (cur == U'\r')					// Обработка переноса строки
			{
				uni_scan_char(prs->in);
			}

			parser_print(prs);
			uni_print_char(prs->out, U'\n');
			return;
		}
		else									// Независимо от корректности строки выводит ее в out
		{
			was_slash = cur == U'\\' ? true : false;
			parser_add_char(prs, cur);
		}

		cur = uni_scan_char(prs->in);
	}

	parser_macro_error(prs, PARSER_UNEXPECTED_EOF);

	if (prs->is_recovery_disabled)				// Добавление "\";" в конец незаконченной строковой константы
	{
		parser_add_char(prs, ch);
		parser_add_char(prs, U';');
	}

	parser_print(prs);
}

/**
 *	Пропускает символы до конца комментария ('\n', '\r' или EOF)
 */
static void parser_skip_short_comment(parser *const prs)
{
	char32_t cur = uni_scan_char(prs->in);
	while(!utf8_is_line_breaker(cur) && cur != (char32_t)EOF)
	{
		cur = uni_scan_char(prs->in);
	}

	if (cur == U'\r')
	{
		uni_scan_char(prs->in);
	}

	parser_print(prs);
	uni_print_char(prs->out, U'\n');
}

/**
 *	Считывает символы до конца длинного комментария и буфферизирует текущую строку кода
 */
static void parser_skip_long_comment(parser *const prs, char32_t *const last)
{
	const size_t line_position = prs->line_position;		// Позиция начала строки с началом комментария
	const size_t line = prs->line;							// Номер строки с началом комментария
	const size_t position = prs->position - 1;				// Позиция начала комментария в строке
	const size_t comment_text_position = in_get_position(prs->in);	// Позиция после символа комментария

	prs->position++;										// '*' был считан снаружи

	bool was_star = false;

	char32_t cur = U'*';//uni_scan_char(prs->in);
	while (cur != (char32_t)EOF)
	{
		cur = uni_scan_char(prs->in);
		prs->position++;

		switch (cur)
		{
			case U'\r':
				uni_scan_char(prs->in);
			case U'\n':
				parser_print(prs);
					break;

			case U'/':
				if (was_star)
				{
					return;
				}
			default:
				was_star = cur == U'*' ? true : false;
				break;
		}
	}

	prs->line_position = line_position;
	prs->line = line;
	prs->position = position;

	*last = (char32_t)EOF;									// Необходимо для корректной работы снаружи
	parser_macro_error(prs, PARSER_COMM_NOT_ENDED);

	if (prs->is_recovery_disabled)							// Пропускает начало комментария
	{
		*last = U'/';
		prs->line_position = line_position;
		prs->line = line;
		prs->position = position + 2;
		in_set_position(prs->in, comment_text_position);
	}
}

/**
 *	Пропускает строку c текущего символа
 */
static inline void parser_skip_line(parser *const prs)
{
	char32_t cur = uni_scan_char(prs->in);
	while (!utf8_is_line_breaker(cur) && cur != (char32_t)EOF)
	{
		prs->position++;
		cur = uni_scan_char(prs->in);
	}

	if (cur == U'\r')
	{
		uni_scan_char(prs->in);
	}

	parser_print(prs);
	uni_print_char(prs->out, U'\n');
}


/**
 *	Проверяет наличие лексем перед директивой препроцессора
 */
static inline int parser_check_kw_position(parser *const prs, const bool was_lexeme)
{
	if (was_lexeme)
	{
		parser_macro_error(prs, PARSER_UNEXPECTED_GRID);
		return 0;
	}

	return -1;
}

/**
 *	Считывает путь к файлу и выполняет его обработку
 */
static void parser_include(parser *const prs)
{
	const size_t position = prs->position;

	char32_t cur = U'\0';
	storage_search(prs->stg, prs->in, &cur);
	
	// Пропуск разделителей и комментариев
	while (utf8_is_separator(cur) || cur == U'/')
	{
		if (cur == U'/')
		{
			char32_t next = uni_scan_char(prs->in);
			switch (next)
			{
				case U'*':
					parser_skip_long_comment(prs, &cur);
					break;
				default:
					prs->position = position;
					parser_macro_error(prs, PARSER_INCLUDE_NEED_FILENAME);
					parser_skip_line(prs);
					return;
			}
		}

		cur = uni_scan_char(prs->in);
		prs->position++;
	}
	if (utf8_is_line_breaker(cur) || cur == (char32_t)EOF || cur != U'\"')
	{
		prs->position = position;
		parser_macro_error(prs, PARSER_INCLUDE_NEED_FILENAME);
		parser_skip_line(prs);
		return;
	}

	// ОБработка пути
	char buffer[MAX_ARG_SIZE] = "\0";
	storage_search(prs->stg, prs->in, &cur);
	prs->position += parser_add_to_buffer(buffer, storage_last_read(prs->stg));

	while (cur != U'\"' && !utf8_is_line_breaker(cur) && cur != (char32_t)EOF)
	{
		prs->position++;

		storage_search(prs->stg, prs->in, &cur);
		prs->position += parser_add_to_buffer(buffer, storage_last_read(prs->stg));
	}

	prs->position++;
	if (cur != U'\"')
	{
		prs->position = position;
		parser_macro_error(prs, PARSER_INCLUDE_NEED_FILENAME);
	parser_skip_line(prs);
	return;
	}

	// Обработка символов за путем
	while (utf8_is_separator(cur) || cur == U'/')
	{
		if (cur == U'/')
		{
			char32_t next = uni_scan_char(prs->in);
			switch (next)
			{
				case U'/':
					parser_skip_short_comment(prs);
					return;
				case U'*':
					parser_skip_long_comment(prs, &cur);
					break;
				default:
					parser_macro_error(prs, PARSER_UNEXPECTED_LEXEME);
					parser_skip_line(prs);
			}
		}

		prs->position++;
		cur = uni_scan_char(prs->in);
	}

	if (!utf8_is_line_breaker(cur) && cur != (char32_t)EOF)
	{
		parser_macro_error(prs, PARSER_UNEXPECTED_LEXEME);
		parser_skip_line(prs);
	}

	// Необходимо подключить файл и вызвать parser_preprocess
	printf("\"%s\"\n", buffer);
}

/**
 *	Считывает имя идентификатора, его значение, добавляет в stg
 *
 *	@param	prs			Структура парсера
 *	@param	mode		Режим работы функции
 *						@c KW_DEFINE #define
 *						@c KW_SET	 #set
 *						@c KW_UNDEF	 #undef
 */
static void parser_define(parser *const prs, char32_t cur, const keyword_t mode)
{
	prs->position += strlen(storage_last_read(prs->stg)) + 1;	// Учитывается разделитель после директивы

	// Пропуск разделителей и комментариев
	while (utf8_is_separator(cur) || cur == U'/')
	{
		if (cur == U'/')
		{
			char32_t next = uni_scan_char(prs->in);
			switch (next)
			{
				case U'*':
					parser_skip_long_comment(prs, &cur);
					break;
				default:
					parser_macro_error(prs, PARSER_INCORRECT_IDENT_NAME);
					parser_skip_line(prs);
					return;
			}
		}

		cur = uni_scan_char(prs->in);
		prs->position++;
	}

	if (utf8_is_line_breaker(cur) || cur == (char32_t)EOF)
	{
		parser_macro_error(prs, mode == KW_DEFINE
								? PARSER_DEFINE_NEED_IDENT
								: mode == KW_SET
									? PARSER_SET_NEED_IDENT
									: PARSER_UNDEF_NEED_IDENT);
		parser_skip_line(prs);
		return;
	}
	else if (!utf8_is_letter(cur))
	{
		parser_macro_error(prs, PARSER_INCORRECT_IDENT_NAME);
		parser_skip_line(prs);
		return;
	}
	else
	{
		char32_t id[1024];
		id[0] = U'\0';
		char32_t value[1024];
		value[0] = U'\0';

		// Запись идентификатора
		const size_t position = prs->position - 1;	// Позиция начала идентификатора

		size_t i = 0;
		while (!utf8_is_separator(cur) && cur != U'/' && !utf8_is_line_breaker(cur) && cur != (char32_t)EOF)
		{
			if (utf8_is_letter(cur) || utf8_is_digit(cur))
			{
				id[i++] = cur;
			}
			else
			{
				in_set_position(prs->in, position);
				parser_macro_error(prs, PARSER_INCORRECT_IDENT_NAME);
				parser_skip_line(prs);
				return;
			}

			prs->position++;
			cur = uni_scan_char(prs->in);
		}
		id[i] = U'\0';

		// Проверка существования
		const size_t temp = in_get_position(prs->in);
		if (mode == KW_DEFINE && storage_add(prs->stg, id, value) == SIZE_MAX)
		{
			in_set_position(prs->in, position);
			parser_macro_warning(prs, PARSER_DEFINE_EXIST_IDENT);
			in_set_position(prs->in, temp);
		}
		else if (mode == KW_SET && storage_add(prs->stg, id, value) != SIZE_MAX)
		{
			in_set_position(prs->in, position);
			parser_macro_warning(prs, PARSER_SET_NOT_EXIST_IDENT);
			in_set_position(prs->in, temp);
		}
		else if (mode == KW_UNDEF)
		{
			storage_remove(prs->stg, id);

			// Проверка последующего кода для #undef
			while (utf8_is_separator(cur) || cur == U'/')
			{
				if (cur == U'/')
				{
					char32_t next = uni_scan_char(prs->in);
					switch (next)
					{
						case U'/':
							parser_skip_short_comment(prs);
							return;
						case U'*':
							parser_skip_long_comment(prs, &cur);
							break;
						default:
							parser_macro_error(prs, PARSER_UNEXPECTED_LEXEME);
							parser_skip_line(prs);
							return;
					}
				}

				prs->position++;
				cur = uni_scan_char(prs->in);
			}

			if (!utf8_is_line_breaker(cur) && cur != (char32_t)EOF)
			{
				parser_macro_error(prs, PARSER_UNEXPECTED_LEXEME);
			}

			parser_skip_line(prs);
			return;
		}

		// Запись значения
		size_t j = 0;
		while (!utf8_is_line_breaker(cur) && cur != (char32_t)EOF)
		{
			if (cur == U'/')
			{
				char32_t next = uni_scan_char(prs->in);
				switch (next)
				{
					case U'/':
						parser_skip_short_comment(prs);
						value[j] = U'\0';
						storage_set(prs->stg, id, value);
						return;
					case U'*':
						parser_skip_long_comment(prs, &cur);
						break;
					default:
						value[j++] = cur;
						uni_unscan_char(prs->in, next);	// next будет считан и обработан в цикле
				}
			}
			else
			{
				value[j++] = cur;
			}

			prs->position++;
			cur = uni_scan_char(prs->in);
		}
		value[j] = U'\0';

		storage_set(prs->stg, id, value);
		if (cur == U'\r')
		{
			uni_scan_char(prs->in);
		}
		parser_next_line(prs);
		uni_print_char(prs->out, U'\n');
	}
}


/*
 *	 __     __   __     ______   ______     ______     ______   ______     ______     ______
 *	/\ \   /\ "-.\ \   /\__  _\ /\  ___\   /\  == \   /\  ___\ /\  __ \   /\  ___\   /\  ___\
 *	\ \ \  \ \ \-.  \  \/_/\ \/ \ \  __\   \ \  __<   \ \  __\ \ \  __ \  \ \ \____  \ \  __\
 *	 \ \_\  \ \_\\"\_\    \ \_\  \ \_____\  \ \_\ \_\  \ \_\    \ \_\ \_\  \ \_____\  \ \_____\
 *	  \/_/   \/_/ \/_/     \/_/   \/_____/   \/_/ /_/   \/_/     \/_/\/_/   \/_____/   \/_____/
 */


parser parser_create(linker *const lk, storage *const stg, universal_io *const out)
{
	parser prs; 
	if (!linker_is_correct(lk) || !out_is_correct(out) || !storage_is_correct(stg))
	{
		prs.lk = NULL;
		return prs;
	}

	prs.lk = lk;
	prs.stg = stg;
	
	prs.in = NULL;
	prs.out = out;

	prs.line_position = 0;
	prs.line = FST_LINE_INDEX;
	prs.position = FST_CHARACTER_INDEX;
	prs.string = strings_create(256);

	prs.is_recovery_disabled = false;
	prs.was_error = false;

	return prs;
} 


int parser_preprocess(parser *const prs, universal_io *const in)
{
	if (!parser_is_correct(prs)|| !in_is_correct(in))
	{
		return -1;
	}

	prs->in = in;
	// comment_create

	char32_t cur = U'\0';
	size_t index = 0;
	bool was_slash = false;
	bool was_lexeme = false;

	while (cur != (char32_t)EOF)
	{
		index = storage_search(prs->stg, prs->in, &cur);
		switch (index)
		{
			case KW_INCLUDE:
				parser_check_kw_position(prs, was_lexeme);
				parser_include(prs);
				was_lexeme = false;
				break;
		
			case KW_DEFINE:
			case KW_SET:
			case KW_UNDEF:
				parser_check_kw_position(prs, was_lexeme);
				parser_define(prs, cur, index);
				was_lexeme = false;
				break;

			case KW_MACRO:
			case KW_ENDM:

			case KW_IFDEF:
			case KW_IFNDEF:
			case KW_IF:
			case KW_ELIF:
			case KW_ELSE:
			case KW_ENDIF:

			case KW_EVAL:

			case KW_WHILE:
			case KW_ENDW:

			default:
				if ((!utf8_is_separator(cur) && cur != U'/' && cur != U'*')	// Перед '#' могут быть разделители
					|| storage_last_read(prs->stg) != NULL)					// и длинные однострочные комментарии
				{
					was_lexeme = true;
				}

				if (storage_last_read(prs->stg) != NULL)
				{
					was_slash = false;

					if (storage_last_read(prs->stg)[0] == '#')
					{
						if (parser_check_kw_position(prs, was_lexeme))	// Перед '#' есть лексемы -> '#' не на месте
																		// Перед '#' нет лексем   -> неправильная директива
						{
							parser_macro_error(prs, PARSER_UNIDETIFIED_KEYWORD);
						}
						parser_skip_line(prs);
						break;
					}

					if (index != SIZE_MAX)
					{
						// Макроподстановка
						prs->position += parser_add_string(prs, storage_get_by_index(prs->stg, index));
					}
					else
					{
						prs->position += parser_add_string(prs, storage_last_read(prs->stg));
					}
				}

				switch (cur)
				{
					case U'#':
						was_slash = false;
						parser_macro_error(prs, PARSER_UNEXPECTED_GRID);
						parser_skip_line(prs);
						break;

					case U'\'':
						was_slash = false;
						parser_skip_string(prs, U'\'');
						break;
					case U'\"':
						was_slash = false;
						parser_skip_string(prs, U'\"');
						break;

					case U'\r':
						cur = uni_scan_char(prs->in);
					case U'\n':
					case (char32_t)EOF:
						was_slash = false;
						was_lexeme = false;
						parser_print(prs);
						uni_print_char(prs->out, U'\n');
						break;

					case U'/':
						if (was_slash)
						{
							was_slash = false;
							strings_remove(&prs->string);
							parser_skip_short_comment(prs);
						}
						else
						{
							was_slash = true;
							parser_add_char(prs, cur);
						}
						break;
					case U'*':
						if (was_slash)
						{
							was_slash = false;
							strings_remove(&prs->string);
							parser_skip_long_comment(prs, &cur);
							break;
						}
						else
						{
							parser_add_char(prs, cur);
						}
						break;

					default:
						was_slash = false;
						parser_add_char(prs, cur);
				}
		}
	}

	return !prs->was_error ? 0 : -1;
}


int parser_disable_recovery(parser *const prs)
{
	if (!parser_is_correct(prs))
	{
		return -1;
	}

	prs->is_recovery_disabled = true;
	return 0;
}


bool parser_is_correct(const parser *const prs)
{
	return prs != NULL && linker_is_correct(prs->lk) && storage_is_correct(prs->stg) && out_is_correct(prs->out);
}


int parser_clear(parser *const prs)
{
	return prs != NULL && linker_clear(prs->lk) && storage_clear(prs->stg) &&  strings_clear(&prs->string) && out_clear(prs->out);
}
