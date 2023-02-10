/*
 *	Copyright 2023 Andrey Terekhov, Victor Y. Fadeev
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


#include "computer.h"
#include "error.h"


#define MAX_NUMBER_SIZE 21


static const size_t MAX_EXPRESSION_DAPTH = 64;


/**
 *	Emit an error from computer
 *
 *	@param	prs			Сomputer structure
 *	@param	num			Error code
 */
static void computer_error(computer *const comp, const item_t pos, error_t num, ...)
{
	if (in_is_file(comp->io) && pos != ITEM_MAX)
	{
		const size_t origin = in_get_position(comp->io);
		in_set_position(comp->io, (size_t)pos);
		loc_search_from(&comp->loc);
		in_set_position(comp->io, origin);
	}

	if (loc_is_correct(&comp->loc))
	{
		va_list args;
		va_start(args, num);

		macro_verror(&comp->loc, num, args);
		comp->loc = loc_copy(NULL);

		va_end(args);
	}
}

static inline int computer_token_to_string(char *const buffer, const token_t tk)
{
	switch(tk)
	{
		case TK_L_BOUND:
			return sprintf(buffer, "(");
		case TK_R_BOUND:
			return sprintf(buffer, ")");
		case TK_COMPL:
			return sprintf(buffer, "~");
		case TK_NOT:
			return sprintf(buffer, "!");
		case TK_U_NEGATION:
		case TK_SUB:
			return sprintf(buffer, "-");
		case TK_U_PLUS:
		case TK_ADD:
			return sprintf(buffer, "+");
		case TK_MULT:
			return sprintf(buffer, "*");
		case TK_DIV:
			return sprintf(buffer, "/");
		case TK_MOD:
			return sprintf(buffer, "%%");
		case TK_L_SHIFT:
			return sprintf(buffer, "<<");
		case TK_R_SHIFT:
			return sprintf(buffer, ">>");
		case TK_LESS:
			return sprintf(buffer, "<");
		case TK_GREATER:
			return sprintf(buffer, ">");
		case TK_LESS_EQ:
			return sprintf(buffer, "<=");
		case TK_GREATER_EQ:
			return sprintf(buffer, ">=");
		case TK_EQ:
			return sprintf(buffer, "==");
		case TK_NOT_EQ:
			return sprintf(buffer, "!=");
		case TK_BIT_AND:
			return sprintf(buffer, "&");
		case TK_XOR:
			return sprintf(buffer, "^");
		case TK_BIT_OR:
			return sprintf(buffer, "|");
		case TK_AND:
			return sprintf(buffer, "&&");
		case TK_OR:
			return sprintf(buffer, "||");

		default:
			return 0;
	}
}

static inline int computer_token_without_number(computer *const comp, const item_t pos, const token_t tk)
{
	const item_t previous = stack_peek(&comp->operators);
	switch (tk)
	{
		case TK_L_BOUND:
			stack_push(&comp->operators, pos);
			stack_push(&comp->operators, tk);
			return 0;
		case TK_COMPL:
			stack_push(&comp->operators, pos);
			stack_push(&comp->operators, tk);
			return 0;
		case TK_NOT:
			stack_push(&comp->operators, pos);
			stack_push(&comp->operators, tk);
			return 0;
		case TK_ADD:
			stack_push(&comp->operators, pos);
			stack_push(&comp->operators, TK_U_NEGATION);
			return 0;
		case TK_SUB:
			stack_push(&comp->operators, pos);
			stack_push(&comp->operators, TK_U_NEGATION);
			return 0;

		case TK_R_BOUND:
			switch (previous)
			{
				case TK_L_BOUND:
					stack_pop(&comp->operators);
					computer_error(comp, stack_pop(&comp->operators), EXPR_MISSING_BETWEEN);
					return -1;
				case ITEM_MAX:
					computer_error(comp, pos, EXPR_MISSING_BRACKET, '(');
					return -1;
			}

		default:
			if (previous != TK_L_BOUND && previous != ITEM_MAX)
			{
				char token[3];
				computer_token_to_string(token, stack_pop(&comp->operators));
				computer_error(comp, stack_pop(&comp->operators), EXPR_NO_RIGHT_OPERAND, token);
			}
			else
			{
				char token[3];
				computer_token_to_string(token, tk);
				computer_error(comp, pos, EXPR_NO_LEFT_OPERAND, token);
			}
			return -1;
	}
}

static int computer_select_three(computer *const comp)
{
	const token_t operator = stack_pop(&comp->operators);
	stack_pop(&comp->operators);

	const item_t second = stack_pop(&comp->numbers);
	stack_pop(&comp->numbers);
	const item_t first = stack_pop(&comp->numbers);
	stack_pop(&comp->numbers);

	stack_push(&comp->numbers, ITEM_MAX);
	switch(operator)
	{
		case TK_MULT:
			return stack_push(&comp->numbers, first * second);
		case TK_DIV:
			return stack_push(&comp->numbers, first / second);
		case TK_MOD:
			return stack_push(&comp->numbers, first % second);
		case TK_ADD:
			return stack_push(&comp->numbers, first + second);
		case TK_SUB:
			return stack_push(&comp->numbers, first - second);
		case TK_L_SHIFT:
			return stack_push(&comp->numbers, first << second);
		case TK_R_SHIFT:
			return stack_push(&comp->numbers, first >> second);
		case TK_LESS:
			return stack_push(&comp->numbers, first < second ? 1 : 0);
		case TK_GREATER:
			return stack_push(&comp->numbers, first > second ? 1 : 0);
		case TK_LESS_EQ:
			return stack_push(&comp->numbers, first <= second ? 1 : 0);
		case TK_GREATER_EQ:
			return stack_push(&comp->numbers, first >= second ? 1 : 0);
		case TK_EQ:
			return stack_push(&comp->numbers, first == second ? 1 : 0);
		case TK_NOT_EQ:
			return stack_push(&comp->numbers, first != second ? 1 : 0);
		case TK_BIT_AND:
			return stack_push(&comp->numbers, first & second);
		case TK_XOR:
			return stack_push(&comp->numbers, first ^ second);
		case TK_BIT_OR:
			return stack_push(&comp->numbers, first | second);
		case TK_AND:
			return stack_push(&comp->numbers, first && second ? 1 : 0);
		case TK_OR:
			return stack_push(&comp->numbers, first || second ? 1 : 0);

		default:
			return -1;
	}
}

static inline int computer_compare_priority(const token_t fst, const token_t snd)
{
	switch(fst)
	{
		case ITEM_MAX:
			return -1;
		case TK_L_BOUND:
			return TK_R_BOUND == snd ? 0 : -1;

		case TK_MULT:
		case TK_DIV:
		case TK_MOD:
			return TK_MOD < snd ? 1 : TK_MULT <= snd ? 0 : -1;

		case TK_ADD:
		case TK_SUB:
			return TK_SUB < snd ? 1 : TK_ADD <= snd ? 0 : -1;

		case TK_L_SHIFT:
		case TK_R_SHIFT:
			return TK_R_SHIFT < snd ? 1 : TK_L_SHIFT <= snd ? 0 : -1;

		case TK_LESS:
		case TK_GREATER:
		case TK_LESS_EQ:
		case TK_GREATER_EQ:
			return TK_GREATER_EQ < snd ? 1 : TK_LESS <= snd ? 0 : -1;

		case TK_EQ:
		case TK_NOT_EQ:
			return TK_NOT_EQ < snd ? 1 : TK_EQ <= snd ? 0 : -1;

		default:
			return fst < snd ? 1 : fst == snd ? 0 : -1;
	}
}

static inline int computer_const_number(computer *const comp, const item_t pos
	, const item_t num, const char *const name)
{
	return -1;
}


/*
 *	 __     __   __     ______   ______     ______     ______   ______     ______     ______
 *	/\ \   /\ "-.\ \   /\__  _\ /\  ___\   /\  == \   /\  ___\ /\  __ \   /\  ___\   /\  ___\
 *	\ \ \  \ \ \-.  \  \/_/\ \/ \ \  __\   \ \  __<   \ \  __\ \ \  __ \  \ \ \____  \ \  __\
 *	 \ \_\  \ \_\\"\_\    \ \_\  \ \_____\  \ \_\ \_\  \ \_\    \ \_\ \_\  \ \_____\  \ \_____\
 *	  \/_/   \/_/ \/_/     \/_/   \/_____/   \/_/ /_/   \/_/     \/_/\/_/   \/_____/   \/_____/
 */


computer computer_create(location *const loc, universal_io *const io)
{
	return (computer) { .loc = loc_copy(loc), .io = io
		, .numbers = stack_create(MAX_EXPRESSION_DAPTH)
		, .operators = stack_create(MAX_EXPRESSION_DAPTH)
		, .was_number = false };
}


int computer_push_token(computer *const comp, const size_t pos, const token_t tk)
{
	if (!computer_is_correct(comp))
	{
		return -1;
	}

	if (!comp->was_number)
	{
		return computer_token_without_number(comp, (item_t)pos, tk);
	}

	const item_t previous = stack_peek(&comp->operators);
	if (previous == ITEM_MAX && tk == TK_R_BOUND)
	{
		computer_error(comp, (item_t)pos, EXPR_MISSING_BRACKET, '(');
		return -1;
	}

	if (previous == TK_L_BOUND && tk == TK_R_BOUND)
	{
		stack_pop(&comp->operators);
		stack_pop(&comp->operators);
		comp->was_number = false;
		return 0;
	}

	const int priority = computer_compare_priority(previous, tk);
	if (priority > 0)
	{
		computer_select_three(comp);
		return computer_push_token(comp, pos, tk);
	}

	if (priority == 0)
	{
		computer_select_three(comp);
	}

	stack_push(&comp->operators, (item_t)pos);
	stack_push(&comp->operators, tk);
	comp->was_number = false;
	return 0;
}

int computer_push_number(computer *const comp, const size_t pos, const item_t num)
{
	if (!computer_is_correct(comp))
	{
		return -1;
	}

	char value[MAX_NUMBER_SIZE];
	sprintf(value, "%" PRIitem, num);
	return computer_const_number(comp, (item_t)pos, num, value);
}

int computer_push_const(computer *const comp, const size_t pos, const char32_t ch, const char *const name)
{
	if (!computer_is_correct(comp))
	{
		return -1;
	}

	return computer_const_number(comp, (item_t)pos, (item_t)ch, name);
}


item_t computer_pop_result(computer *const comp)
{
	if (!computer_is_correct(comp))
	{
		return 0;
	}

	return stack_pop(&comp->numbers);
}

bool computer_is_correct(const computer *const comp)
{
	return comp != NULL && stack_is_correct(&comp->numbers) && stack_is_correct(&comp->operators);
}


int computer_clear(computer *const comp)
{
	if (!computer_is_correct(comp))
	{
		return -1;
	}

	stack_clear(&comp->numbers);
	stack_clear(&comp->operators);

	return 0;
}
