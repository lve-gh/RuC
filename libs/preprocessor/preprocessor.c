/*
 *	Copyright 2018 Andrey Terekhov, Egor Anikin
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

#include "preprocessor.h"
#include "constants.h"
#include "context_var.h"
#include "file.h"
#include "include.h"
#include "uniio.h"
#include "uniprinter.h"
#include "preprocessor_error.h"
#include "preprocessor_utils.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MACRODEBUG 1

#define H_FILE 1
#define C_FILE 0


/*void to_reprtab(char str[], int num, preprocess_context *context)
{
	int i;
	int oldrepr = context->rp;
	int hash = 0;
	unsigned char firstchar;
	unsigned char secondchar;
	int p;
	int c = 0;
	context->rp += 2;

	for (i = 0; str[i] != 0; i++)
	{
		sscanf(&str[i], "%c%n", &firstchar, &p);

		if ((firstchar & /*0b11100000* 0xE0) == /*0b11000000* 0xC0)
		{
			++i;
			sscanf(&str[i], "%c%n", &secondchar, &p);
			c = ((int)(firstchar & /*0b11111* 0x1F)) << 6 | (secondchar & /*0b111111* 0x3F);
		}
		else
		{
			c = firstchar;
		}

		hash += c
}*/

void to_reprtab_full(char str1[], char str2[], char str3[], char str4[], int num, preprocess_context *context)
{
	con_repr_add(&context->repr, str1, num);
	con_repr_add(&context->repr, str2, num);
	con_repr_add(&context->repr, str3, num);
	con_repr_add(&context->repr, str4, num);
}

void add_keywods(preprocess_context *context)
{
	to_reprtab_full("MAIN", "main", "ГЛАВНАЯ", "главная", SH_MAIN, context);
	to_reprtab_full("#DEFINE", "#define", "#ОПРЕД", "#опред", SH_DEFINE, context);
	to_reprtab_full("#IFDEF", "#ifdef", "#ЕСЛИБЫЛ", "#еслибыл", SH_IFDEF, context);
	to_reprtab_full("#IFNDEF", "#ifndef", "#ЕСЛИНЕБЫЛ", "#еслинебыл", SH_IFNDEF, context);
	to_reprtab_full("#IF", "#if", "#ЕСЛИ", "#если", SH_IF, context);
	to_reprtab_full("#ELIF", "#elif", "#ИНЕСЛИ", "#инесли", SH_ELIF, context);
	to_reprtab_full("#ENDIF", "#endif", "#КОНЕЦЕСЛИ", "#конецесли", SH_ENDIF, context);
	to_reprtab_full("#ELSE", "#else", "#ИНАЧЕ", "#иначе", SH_ELSE, context);
	to_reprtab_full("#MACRO", "#macro", "#МАКРО", "#макро", SH_MACRO, context);
	to_reprtab_full("#ENDM", "#endm", "#КОНЕЦМ", "#конецм", SH_ENDM, context);
	to_reprtab_full("#WHILE", "#while", "#ПОКА", "#пока", SH_WHILE, context);
	to_reprtab_full("#ENDW", "#endw", "#КОНЕЦП", "#конецп", SH_ENDW, context);
	to_reprtab_full("#SET", "#set", "#ПЕРЕОПРЕД", "#переопред", SH_SET, context);
	to_reprtab_full("#UNDEF", "#undef", "#ОТМЕНАОПРЕД", "#отменаопред", SH_UNDEF, context);
	to_reprtab_full("#FOR", "#for", "#ДЛЯ", "#для", SH_FOR, context);
	to_reprtab_full("#ENDF", "#endf", "#КОНЕЦД", "#конецд", SH_ENDF, context);
	to_reprtab_full("#EVAL", "#eval", "#ВЫЧИСЛЕНИЕ", "#вычисление", SH_EVAL, context);
	to_reprtab_full("#INCLUDE", "#include", "#ДОБАВИТЬ", "#добавить", SH_INCLUDE, context);
}


void add_c_file_siple(preprocess_context *context)
{
	context->temp_output = 0;

	while (context->curchar != EOF && context->fs.main_faile == -1)
	{
		char32_t str[STRIGSIZE];
		collect_mident(context, str);
		context->cur = con_repr_find(&context->repr, str);
		if (context->cur == SH_MAIN)
		{
			con_file_it_is_main(&context->fs);
		}
		m_nextch(context);
	}

	while (context->curchar != EOF)
	{
		m_nextch(context);
	}
}

void add_c_file(preprocess_context *context)
{
	m_nextch(context);
	while (context->curchar != EOF)
	{
		switch (context->curchar)
		{
			case EOF:
			{
				return;
			}
			case ' ':
			case '\n':
			case '\t':
			{
				m_nextch(context);
				break;
			}
			case '#':
			{
				char32_t str[STRIGSIZE];//char32_t *str = malloc(100 * sizeof(char *));
				collect_mident(context, str);
				
				context->cur = con_repr_find(&context->repr, str);
				if (context->cur == SH_INCLUDE)
				{
					include_relis(context);
					break;
				}
			}
			default:
			{
				add_c_file_siple(context);
				return;
			}
		}
	}
}

void open_files(preprocess_context *context, const workspace *const ws)
{
	const char **ways = context->include_ways;
	int i = 0;
	const char *temp = ws_get_dir(ws, i);
	while (temp != NULL)
	{
		ways[i++] = temp;
		temp = ws_get_dir(ws, i);
	}
	context->iwp = i;
	
	i = 0;
	temp = ws_get_file(ws, i++);
	while (temp != NULL)
	{
		int l = strlen(temp);
		if (temp[l - 1] == 'h')
		{
			temp = ws_get_file(ws, i++);
			continue;
		}

		if (con_repr_add(&context->repr, temp, SH_FILE) != 0)
		{
			con_files_add_parametrs(&context->fs, temp);
			con_file_open_next(&context->fs, context, C_FILE);

			get_next_char(context);

			if (context->nextchar != EOF)
			{
				add_c_file(context);
			}
			con_file_close_cur(context);
		}
		temp = ws_get_file(ws, i++);
	}
	con_file_it_is_end_h(&context->fs);
}

void preprocess_h_file(preprocess_context *context)
{
	context->h_flag = 1;
	context->include_type = 1;

	if(con_file_open_hedrs(&context->fs, context) != 0)
	{
		file_read(context);

		while (con_file_open_next(&context->fs, context, C_FILE)!= 0)
		{
			file_read(context);
		}
	}
}

void preprocess_c_file(preprocess_context *context)
{
	context->include_type = 2;
	context->h_flag = 0;
	if(con_file_open_sorse(&context->fs, context) != 0)
	{
		file_read(context);

		while (con_file_open_next(&context->fs, context, C_FILE) != 0)
		{
			file_read(context);
		}
	}

	if(con_file_open_main(&context->fs, context) != 0)
	{
		file_read(context);
	}
}

char *macro(workspace *const ws)
{
	preprocess_context context;
	con_init(&context);
	out_set_buffer(&context.io, 1024);

	add_keywods(&context);

	open_files(&context, ws);
	preprocess_h_file(&context);
	preprocess_c_file(&context);

	con_files_free(&context.fs);

	char *macro_processed = out_extract_buffer(&context.io);

	
#if MACRODEBUG
	printf("\n\n");
	printf("Текст после препроцессирования:\n>\n%s<\n", macro_processed);
#endif
	return macro_processed;
}


/*
	printf("cur = %d, %c; next = %d, %c;\n",context->curchar, context->curchar, context->nextchar, context->nextchar);

	for (int k = 0; k < fsp; k++)
	{
		printf("str[%d] = %d,%c.\n", k, fstring[k], fstring[k]);
	}

	printf("!!!!!!!!!!!!!!1\n");

	for (int k = 0; k < context->mp; k++)
	{
		printf("context->macrotext[%d] = %d,%c.\n", k, context->macrotext[k], context->macrotext[k]);
	}
	
	for (int k = 0; k < cp; k++)
	{
		printf(" fchange[%d] = %d,%c.\n", k, fchange[k], fchange[k]);
	}

	/Egor/test_eval1.c
	/Egor/calculator/test1.c
	/Fadeev/import.c
	/Egor/Macro/includ/cofig.txt
*/

int macro_to_file(workspace *const ws, const char *const path)
{
	char *buffer = macro(ws);
	if (buffer == NULL)
	{
		return -1;
	}

	universal_io io = io_create();
	if (out_set_file(&io, path))
	{
		free(buffer);
		return -1;
	}
	
	uni_printf(&io, "%s", buffer);
	io_erase(&io);

	free(buffer);
	return 0;
}


char *auto_macro(const int argc, const char *const *const argv)
{
	workspace ws = ws_parse_args(argc, argv);
	if (!ws_is_correct(&ws))
	{
		return NULL;
	}

	return macro(&ws);
}

int auto_macro_to_file(const int argc, const char *const *const argv, const char *const path)
{
	workspace ws = ws_parse_args(argc, argv);
	if (!ws_is_correct(&ws))
	{
		return -1;
	}

	return macro_to_file(&ws, path);
}
