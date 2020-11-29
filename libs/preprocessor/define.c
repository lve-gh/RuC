/*
 *	Copyright 2020 Andrey Terekhov, Egor Anikin
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

#include "define.h"
#include "calculator.h"
#include "constants.h"
#include "context_var.h"
#include "file.h"
#include "preprocessor_error.h"
#include "preprocessor_utils.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void define_get_from_macrotext(int r, preprocess_context *context);


int m_equal(preprocess_context *context, char32_t* s)
{
	int i = 1;
	int n = 1;
	int j = 0;

	while (j < context->csp)
	{
		while (s[i] == context->cstring[j])
		{
			i++;
			j++;
			
			if (s[i] == 0 && context->cstring[j] == 0)
			{
				return n;
			}
		}

		n++;
		i = 1;
		if (context->cstring[j++] != 0)
		{
			while (context->cstring[j++] != 0)
			{
				;
			}
		}
	}
	return 0;
}

// define c параметрами (function)
void function_scob_collect(int t, int num, preprocess_context *context)
{
	int i;

	while (context->curchar != EOF)
	{
		if (is_letter(context) != 0)
		{
			char32_t str[STRIGSIZE];
			collect_mident(context, str);
			int r = con_repr_find(&context->repr, str);

			if (r != 0)
			{
				int oldcp1 = context->cp;
				int oldlsp = context->lsp;
				int locfchange[STRIGSIZE];
				int lcp = 0;
				int ldip;

				context->lsp += num;
				define_get_from_macrotext(r, context);
				ldip = get_dipp(context);

				if (context->nextch_type == FTYPE)
				{
					ldip--;
				}

				while (get_dipp(context) >= ldip) // 1 переход потому что есть префиксная замена
				{
					locfchange[lcp++] = context->curchar;
					m_nextch(context);
				}

				context->lsp = oldlsp;
				context->cp = oldcp1;

				for (i = 0; i < lcp; i++)
				{
					context->fchange[context->cp++] = locfchange[i];
				}
			}
			else
			{
				int i = 1;
				while (str[i] != '\0')
				{
					context->fchange[context->cp++] = str[i++];
				}
			}
		}
		else if (context->curchar == '(')
		{
			context->fchange[context->cp++] = context->curchar;
			m_nextch(context);
			function_scob_collect(0, num, context);
		}
		else if (context->curchar == ')' || (t == 1 && context->curchar == ','))
		{
			if (t == 0)
			{
				context->fchange[context->cp++] = context->curchar;
				m_nextch(context);
			}

			return;
		}
		else if (context->curchar == '#')
		{
			char32_t str[STRIGSIZE];
			collect_mident(context, str);
			if (con_repr_find(&context->repr, str) == SH_EVAL && context->curchar == '(')
			{
				calculator(0, context);
				for (i = 0; i < context->csp; i++)
				{
					context->fchange[context->cp++] = context->cstring[i];
				}
			}
			else
			{
				int i = 1; 
				while (str[i] != '\0')
				{
					context->fchange[context->cp++] = str[i++];
				}
			}
		}
		else
		{
			context->fchange[context->cp++] = context->curchar;
			m_nextch(context);
		}
	}
	m_error(scob_not_clous, context);
}

void function_stack_create(int n, preprocess_context *context)
{
	int num = 0;

	m_nextch(context);
	// printf("function_stack_create n = %d\n", n);
	context->localstack[num + context->lsp] = context->cp;

	if (context->curchar == ')')
	{
		m_error(stalpe, context);
	}

	while (context->curchar != ')')
	{
		function_scob_collect(1, num, context);
		context->fchange[context->cp++] = CANGEEND;

		if (context->curchar == ',')
		{
			num++;
			context->localstack[num + context->lsp] = context->cp;

			if (num > n)
			{
				m_error(not_enough_param, context);
			}
			m_nextch(context);

			if (context->curchar == ' ')
			{
				m_nextch(context);
			}
		}
		else if (context->curchar == ')')
		{
			if (num != n)
			{
				m_error(not_enough_param2, context);
			}
			m_nextch(context);

			context->cp = context->localstack[context->lsp];
			return;
		}
	}

	m_error(scob_not_clous, context);
}

void funktionleter(int flag_macro, preprocess_context *context)
{
	int n = 0;
	int i = 0;

	char32_t str[STRIGSIZE];
	collect_mident(context, str);
	int r = con_repr_find(&context->repr, str);

	// printf("funktionleter\n");

	if ((n = m_equal(context, str)) != 0)
	{
		context->macrotext[context->mp++] = MACROCANGE;
		context->macrotext[context->mp++] = n - 1;
	}
	else if (flag_macro == 0 && r)
	{
		define_get_from_macrotext(r, context);	
	}
	else
	{
		int i = 1; 
		while (str[i] != '\0')
		{
			context->macrotext[context->mp++] = str[i++];
		}
	}
}

int to_functionident(preprocess_context *context)
{
	int num = 0;
	context->csp = 0;

	// printf("to_functionident\n");

	while (context->curchar != ')')
	{
		if (is_letter(context) != 0)
		{
			while (is_letter(context) != 0 || is_digit(context->curchar) != 0)
			{
				context->cstring[context->csp++] = context->curchar;
				m_nextch(context);
			}
			context->cstring[context->csp++] = 0;
		}
		else
		{
			m_error(functionid_begins_with_letters, context);
		}

		
		if (context->curchar == ',')
		{
			m_nextch(context);
			space_skip(context);
			num++;
		}
		else if (context->curchar != ')')
		{
			m_error(after_functionid_must_be_comma, context);
		}
	}

	// printf("-to_functionident = %d\n", num);
	m_nextch(context);
	return num;
}

void function_add_to_macrotext(preprocess_context *context)
{
	int j;
	int flag_macro = 0;
	int empty = 0;

	// printf("function_add_to_macrotext\n");

	if (context->cur == SH_MACRO)
	{
		flag_macro = 1;
	}

	context->macrotext[context->mp++] = MACROFUNCTION;

	if (context->curchar == ')')
	{
		context->macrotext[context->mp++] = -1;
		empty = 1;
		m_nextch(context);
	}
	else
	{
		context->macrotext[context->mp++] = to_functionident(context);
	}
	space_skip(context);

	while (context->curchar != '\n' || flag_macro && context->curchar != EOF)
	{
		if (is_letter(context) != 0 && empty == 0)
		{
			funktionleter(flag_macro, context);
		}
		else if (context->curchar == '#')
		{
			
			char32_t str[STRIGSIZE];
			collect_mident(context, str);
			context->cur = con_repr_find(&context->repr, str);

			if (flag_macro == 0 && context->cur == SH_EVAL && context->curchar == '(')
			{
				calculator(0, context);
				for (j = 0; j < context->csp; j++)
				{
					context->macrotext[context->mp++] = context->cstring[j];
				}
			}
			else if (flag_macro != 0 && context->cur == SH_ENDM)
			{
				m_nextch(context);
				context->macrotext[context->mp++] = MACROEND;
				return;
			}
			else
			{
				context->cur = 0;
				int i = 1; 
				while (str[i] != '\0')
				{
					context->macrotext[context->mp++] = str[i++];
				}
			}
		}
		else
		{
			context->macrotext[context->mp++] = context->curchar;
			m_nextch(context);
		}

		if (context->curchar == EOF)
		{
			m_error(not_end_fail_define, context);
		}

		if (context->curchar == '\\')
		{
			m_nextch(context);
			space_end_line(context);
		}
	}

	context->macrotext[context->mp++] = MACROEND;
}
//

// define
void define_get_from_macrotext(int r, preprocess_context *context)
{
	int t = r;

	if (r != 0)
	{
		
		if (context->macrotext[t] == MACROFUNCTION)
		{
			if (context->macrotext[++t] > -1)
			{
				function_stack_create(context->macrotext[t], context);
			}
		}

		// printf("--from_macrotext r = %d\n", t + 1);
		m_change_nextch_type(TEXTTYPE, t + 1, context);
		m_nextch(context);
	}
	else
	{
		m_error(ident_not_exist, context);
	}
}

void define_add_to_macrotext(preprocess_context *context)
{
	int j;

	context->macrotext[context->mp++] = MACRODEF;
	if (context->curchar != '\n')
	{
		while (context->curchar != '\n')
		{
			if (context->curchar == EOF)
			{
				m_error(not_end_fail_define, context);
			}
			else if (context->curchar == '#')
			{
				char32_t str[STRIGSIZE];
				collect_mident(context, str);
				context->cur = con_repr_find(&context->repr, str);
				if (context->cur == SH_EVAL)
				{
					if (context->curchar != '(')
					{
						m_error(after_eval_must_be_ckob, context);
					}

					calculator(0, context);

					for (j = 0; j < context->csp; j++)
					{
						context->macrotext[context->mp++] = context->cstring[j];
					}
				}
				else
				{
					int i = 1; 
					while (str[i] != '\0')
					{
						context->macrotext[context->mp++] = str[i++];
					}
				}
			}
			else if (context->curchar == '\\')
			{
				m_nextch(context);
				space_end_line(context);
			}
			else if (is_letter(context) != 0)
			{
				char32_t str[STRIGSIZE];
				collect_mident(context, str);
				int k = con_repr_find(&context->repr, str);
				if (k != 0)
				{
					define_get_from_macrotext(k, context);
				}
				else
				{
					int i = 1; 
					while (str[i] != '\0')
					{
						context->macrotext[context->mp++] = str[i++];
					}
				}
			}
			else
			{
				context->macrotext[context->mp++] = context->curchar;
				m_nextch(context);
			}
		}

		while (context->macrotext[context->mp - 1] == ' ' || context->macrotext[context->mp - 1] == '\t')
		{
			context->macrotext[context->mp - 1] = MACROEND;
			context->mp--;
		}
	}
	else
	{
		context->macrotext[context->mp++] = '0';
	}

	context->macrotext[context->mp++] = MACROEND;
}

void define_relis(preprocess_context *context)
{
	int r;
	if (is_letter(context) == 0)
	{
		m_error(ident_begins_with_letters1, context);
	}
	
	con_repr_add_ident(&context->repr, context);

	if (context->curchar == '(')
	{
		m_nextch(context);
		function_add_to_macrotext(context);
	}
	else if (context->curchar != ' ' && context->curchar != '\n' && context->curchar != '\t')
	{
		m_error(after_ident_must_be_space1, context);
	}
	else
	{
		space_skip(context);
		define_add_to_macrotext(context);
	}
	m_nextch(context);
}

void set_relis(preprocess_context *context)
{
	int j;

	space_skip(context);

	if (is_letter(context) == 0)
	{
		m_error(ident_begins_with_letters1, context);
	}

	con_repr_change(&context->repr, context);

	if (context->curchar != ' ')
	{
		m_error(after_ident_must_be_space1, context);
	}

	m_nextch(context);
	space_skip(context);

	define_add_to_macrotext(context);
}
//