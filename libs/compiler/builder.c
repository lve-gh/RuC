/*
 *	Copyright 2021 Andrey Terekhov, Ilya Andreev
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

#include "builder.h"
#include "AST.h"


static inline node node_create(syntax *const sx, operation_t type)
{
	return node_add_child(&sx->nd, type);
}

static inline void node_set_child(const node *const parent, const node *const child)
{
	node temp = node_add_child(parent, OP_NOP);
	node_swap(child, &temp);
	node_remove(&temp);
}

/**
 *	Emit a semantic error
 *
 *	@param	sx			Syntax structure
 *	@param	loc			Error location
 *	@param	num			Error code
 */
static void semantic_error(syntax *const sx, const location loc, error_t num, ...)
{
	va_list args;
	va_start(args, num);

	const size_t prev_loc = in_get_position(sx->io);
	in_set_position(sx->io, loc.begin);

	verror(sx->io, num, args);
	sx->was_error = true;

	in_set_position(sx->io, prev_loc);

	va_end(args);
}

static item_t usual_arithmetic_conversions(const item_t left_type, const item_t right_type)
{
	return type_is_integer(left_type) && type_is_integer(right_type)
		? TYPE_INTEGER
		: TYPE_FLOATING;
}

static node fold_unary_expression(syntax *const sx, const item_t type, const category_t ctg
	, node *const expr, const unary_t op, const location loc)
{
	if (expression_get_class(expr) != EXPR_LITERAL)
	{
		return expression_unary(sx, type, ctg, expr, op, loc);
	}

	if (type_is_null_pointer(type))
	{
		// Это может быть только UN_LOGNOT
		node_remove(expr);
		return build_integer_literal_expression(sx, true, loc);
	}
	else if (type_is_integer(type))
	{
		const item_t value = expression_literal_get_integer(expr);
		node_remove(expr);

		switch (op)
		{
			case UN_MINUS:
				return build_integer_literal_expression(sx, -value, loc);
			case UN_NOT:
				return build_integer_literal_expression(sx, ~value, loc);
			case UN_LOGNOT:
				return build_integer_literal_expression(sx, value == 0 ? 1 : 0, loc);
			case UN_ABS:
				return build_integer_literal_expression(sx, value >= 0 ? value : -value, loc);
			default:
				return node_broken();
		}
	}
	else // if (type_is_double(type))
	{
		const double value = expression_literal_get_floating(expr);
		node_remove(expr);

		switch (op)
		{
			case UN_MINUS:
				return build_floating_literal_expression(sx, -value, loc);
			case UN_ABS:
				return build_floating_literal_expression(sx, value >= 0 ? value : -value, loc);
			default:
				return node_broken();
		}
	}
}

static node fold_binary_expression(syntax *const sx, const item_t type
   , node *const LHS, node *const RHS, const binary_t op, const location loc)
{
	if (expression_get_class(LHS) != EXPR_LITERAL || expression_get_class(RHS) != EXPR_LITERAL)
	{
		return expression_binary(sx, type, LHS, RHS, op, loc);
	}

	if (op == BIN_COMMA)
	{
		node_remove(LHS);
		return *RHS;
	}

	const item_t left_type = expression_get_type(LHS);
	const item_t right_type = expression_get_type(RHS);
	if (type_is_integer(type))
	{
		if (type_is_integer(left_type) && type_is_integer(right_type))
		{
			const item_t left_value = expression_literal_get_integer(LHS);
			const item_t right_value = expression_literal_get_integer(RHS);
			node_remove(LHS);
			node_remove(RHS);

			switch (op)
			{
				case BIN_MUL:
					return build_integer_literal_expression(sx, left_value * right_value, loc);
				case BIN_DIV:
					return build_integer_literal_expression(sx, left_value / right_value, loc);
				case BIN_REM:
					return build_integer_literal_expression(sx, left_value % right_value, loc);
				case BIN_ADD:
					return build_integer_literal_expression(sx, left_value + right_value, loc);
				case BIN_SUB:
					return build_integer_literal_expression(sx, left_value - right_value, loc);
				case BIN_SHL:
					return build_integer_literal_expression(sx, left_value << right_value, loc);
				case BIN_SHR:
					return build_integer_literal_expression(sx, left_value >> right_value, loc);
				case BIN_LT:
					return build_integer_literal_expression(sx, left_value < right_value, loc);
				case BIN_GT:
					return build_integer_literal_expression(sx, left_value > right_value, loc);
				case BIN_LE:
					return build_integer_literal_expression(sx, left_value <= right_value, loc);
				case BIN_GE:
					return build_integer_literal_expression(sx, left_value >= right_value, loc);
				case BIN_EQ:
					return build_integer_literal_expression(sx, left_value == right_value, loc);
				case BIN_NE:
					return build_integer_literal_expression(sx, left_value != right_value, loc);
				case BIN_AND:
					return build_integer_literal_expression(sx, left_value & right_value, loc);
				case BIN_XOR:
					return build_integer_literal_expression(sx, left_value ^ right_value, loc);
				case BIN_OR:
					return build_integer_literal_expression(sx, left_value | right_value, loc);
				case BIN_LOG_AND:
					return build_integer_literal_expression(sx, left_value && right_value, loc);
				case BIN_LOG_OR:
					return build_integer_literal_expression(sx, left_value || right_value, loc);
				default:
					return node_broken();
			}
		}
		else
		{
			const double left_value = type_is_integer(left_type)
				? expression_literal_get_integer(LHS)
				: expression_literal_get_floating(LHS);

			const double right_value = type_is_integer(right_type)
				? expression_literal_get_integer(RHS)
				: expression_literal_get_floating(RHS);

			node_remove(LHS);
			node_remove(RHS);

			switch (op)
			{
				case BIN_LT:
					return build_integer_literal_expression(sx, left_value < right_value, loc);
				case BIN_GT:
					return build_integer_literal_expression(sx, left_value > right_value, loc);
				case BIN_LE:
					return build_integer_literal_expression(sx, left_value <= right_value, loc);
				case BIN_GE:
					return build_integer_literal_expression(sx, left_value >= right_value, loc);
				case BIN_EQ:
					return build_integer_literal_expression(sx, left_value == right_value, loc);
				case BIN_NE:
					return build_integer_literal_expression(sx, left_value != right_value, loc);
				default:
					return node_broken();
			}
		}
	}
	else // if (type_is_floating(type))
	{
		const double left_value = type_is_integer(left_type)
			? expression_literal_get_integer(LHS)
			: expression_literal_get_floating(LHS);

		const double right_value = type_is_integer(right_type)
			? expression_literal_get_integer(RHS)
			: expression_literal_get_floating(RHS);

		node_remove(LHS);
		node_remove(RHS);

		switch (op)
		{
			case BIN_MUL:
				return build_floating_literal_expression(sx, left_value * right_value, loc);
			case BIN_DIV:
				return build_floating_literal_expression(sx, left_value / right_value, loc);
			case BIN_ADD:
				return build_floating_literal_expression(sx, left_value + right_value, loc);
			case BIN_SUB:
				return build_floating_literal_expression(sx, left_value - right_value, loc);
			default:
				return node_broken();
		}
	}
}

static node fold_ternary_expression(syntax *const sx, const item_t type, node *const cond
	, node *const LHS, node *const RHS, const location loc)
{
	if (expression_get_class(cond) != EXPR_LITERAL)
	{
		return expression_ternary(sx, type, cond, LHS, RHS, loc);
	}

	const item_t cond_type = expression_get_type(cond);
	if (type_is_null_pointer(cond_type))
	{
		node_remove(cond);
		node_remove(LHS);
		return *RHS;
	}
	else if (type_is_integer(cond_type))
	{
		const item_t value = expression_literal_get_integer(cond);
		node_remove(cond);
		if (value != 0)
		{
			node_remove(RHS);
			return *LHS;
		}
		else
		{
			node_remove(LHS);
			return *RHS;
		}
	}
	else // if (type_is_double(cond_type)
	{
		const double value = expression_literal_get_floating(cond);
		node_remove(cond);
		if (value != 0)
		{
			node_remove(RHS);
			return *LHS;
		}
		else
		{
			node_remove(LHS);
			return *RHS;
		}
	}
}


/*
 *	 __     __   __     ______   ______     ______     ______   ______     ______     ______
 *	/\ \   /\ "-.\ \   /\__  _\ /\  ___\   /\  == \   /\  ___\ /\  __ \   /\  ___\   /\  ___\
 *	\ \ \  \ \ \-.  \  \/_/\ \/ \ \  __\   \ \  __<   \ \  __\ \ \  __ \  \ \ \____  \ \  __\
 *	 \ \_\  \ \_\\"\_\    \ \_\  \ \_____\  \ \_\ \_\  \ \_\    \ \_\ \_\  \ \_____\  \ \_____\
 *	  \/_/   \/_/ \/_/     \/_/   \/_____/   \/_/ /_/   \/_/     \/_/\/_/   \/_____/   \/_____/
 */


bool check_assignment_operands(syntax *const sx, const item_t expected_type, const node *const nd_init)
{
	if (expression_get_class(nd_init) == EXPR_LIST)
	{
		const size_t actual_inits = expression_list_get_size(nd_init);
		if (type_is_structure(sx, expected_type))
		{
			const size_t expected_inits = type_structure_get_member_amount(sx, expected_type);

			if (expected_inits != actual_inits)
			{
				semantic_error(sx, expression_get_location(nd_init), wrong_init_in_actparam, expected_inits, actual_inits);
				return false;
			}

			for (size_t i = 0; i < actual_inits; i++)
			{
				const node nd_initializer = expression_list_get_subexpr(nd_init, i);
				if (!node_is_correct(&nd_initializer))
				{
					return false;
				}

				const item_t type = type_structure_get_member_type(sx, expected_type, i);
				if (!check_assignment_operands(sx, type, &nd_initializer))
				{
					return false;
				}
			}
			expression_list_set_type(nd_init, expected_type);
			return true;
		}
		else if (type_is_array(sx, expected_type))
		{
			const item_t type = type_array_get_element_type(sx, expected_type);
			for (size_t i = 0; i < actual_inits; i++)
			{
				const node nd_initializer = expression_list_get_subexpr(nd_init, i);
				if (!node_is_correct(&nd_initializer))
				{
					return false;
				}

				if (!check_assignment_operands(sx, type, &nd_initializer))
				{
					return false;
				}
			}
			expression_list_set_type(nd_init, expected_type);
			return true;
		}
		else
		{
			semantic_error(sx, expression_get_location(nd_init), wrong_init);
			return false;
		}
	}

	const item_t actual_type = expression_get_type(nd_init);

	if (type_is_floating(expected_type) && type_is_integer(actual_type))
	{
		return true;
	}

	if (type_is_pointer(sx, expected_type) && type_is_null_pointer(actual_type))
	{
		return true;
	}

	if (expected_type == actual_type)
	{
		return true;
	}

	semantic_error(sx, expression_get_location(nd_init), wrong_init);
	return false;
}

node build_identifier_expression(syntax *const sx, const size_t name, const location loc)
{
	const size_t identifier = (size_t)repr_get_reference(sx, name);

	if ((item_t)identifier == ITEM_MAX)
	{
		semantic_error(sx, loc, undeclared_var_use, repr_get_name(sx, name));
		return node_broken();
	}

	const item_t type = ident_get_type(sx, identifier);
	const category_t category = type_is_function(sx, type) ? RVALUE : LVALUE;

	return expression_identifier(sx, type, category, identifier, loc);
}

node build_integer_literal_expression(syntax *const sx, const item_t value, const location loc)
{
	node nd = node_create(sx, OP_LITERAL);
	node_add_arg(&nd, TYPE_INTEGER);				// Тип значения литерала
	node_add_arg(&nd, RVALUE);						// Категория значения литерала
	node_add_arg(&nd, value);						// Значение литерала
	node_add_arg(&nd, (item_t)loc.begin);			// Начальная позиция литерала
	node_add_arg(&nd, (item_t)loc.end);				// Конечная позиция литерала

	return nd;
}

node build_floating_literal_expression(syntax *const sx, const double value, const location loc)
{
	node nd = node_create(sx, OP_LITERAL);
	node_add_arg(&nd, TYPE_FLOATING);				// Тип значения литерала
	node_add_arg(&nd, RVALUE);						// Категория значения литерала
	node_add_arg_double(&nd, value);				// Значение литерала
	node_add_arg(&nd, (item_t)loc.begin);			// Начальная позиция литерала
	node_add_arg(&nd, (item_t)loc.end);				// Конечная позиция литерала

	return nd;
}

node build_string_literal_expression(syntax *const sx, const size_t index, const location loc)
{
	const item_t type = type_array(sx, TYPE_INTEGER);

	node nd = node_create(sx, OP_LITERAL);
	node_add_arg(&nd, type);						// Тип значения литерала
	node_add_arg(&nd, LVALUE);						// Категория значения литерала
	node_add_arg(&nd, (item_t)index);				// Индекс в списке строк
	node_add_arg(&nd, (item_t)loc.begin);			// Начальная позиция литерала
	node_add_arg(&nd, (item_t)loc.end);				// Конечная позиция строки

	return nd;
}

node build_null_pointer_literal_expression(syntax *const sx, const location loc)
{
	node nd = node_create(sx, OP_LITERAL);
	node_add_arg(&nd, TYPE_NULL_POINTER);			// Тип значения литерала
	node_add_arg(&nd, RVALUE);						// Категория значения литерала
	node_add_arg(&nd, (item_t)loc.begin);			// Начальная позиция литерала
	node_add_arg(&nd, (item_t)loc.end);				// Конечная позиция литерала

	return nd;
}

node build_subscript_expression(syntax *const sx, const node *const base, const node *const index
	, const location l_loc, const location r_loc)
{
	if (!node_is_correct(base) || !node_is_correct(index))
	{
		return node_broken();
	}

	const item_t base_type = expression_get_type(base);
	if (!type_is_array(sx, base_type))
	{
		semantic_error(sx, l_loc, typecheck_subscript_value);
		return node_broken();
	}

	const item_t index_type = expression_get_type(index);
	if (!type_is_integer(index_type))
	{
		semantic_error(sx, expression_get_location(index), typecheck_subscript_not_integer);
		return node_broken();
	}

	const item_t element_type = type_array_get_element_type(sx, base_type);

	const location loc = { expression_get_location(base).begin, r_loc.end };
	return expression_subscript(sx, element_type, base, index, loc);
}

node build_call_expression(syntax *const sx, const node *const callee
	, node_vector *const args, const location l_loc, const location r_loc)
{
	if (!node_is_correct(callee))
	{
		node_vector_clear(args);
		return node_broken();
	}

	const item_t callee_type = expression_get_type(callee);
	if (!type_is_function(sx, callee_type))
	{
		semantic_error(sx, l_loc, typecheck_call_not_function);
		node_vector_clear(args);
		return node_broken();
	}

	const size_t expected_args = type_function_get_parameter_amount(sx, callee_type);
	const size_t actual_args = args != NULL ? node_vector_size(args) : 0;

	if (expected_args != actual_args)
	{
		semantic_error(sx, r_loc, wrong_number_of_params, expected_args, actual_args);
		node_vector_clear(args);
		return node_broken();
	}

	for (size_t i = 0; i < actual_args; i++)
	{
		const node argument = node_vector_get(args, i);
		if (!node_is_correct(&argument))
		{
			node_vector_clear(args);
			return node_broken();
		}

		const item_t expected_type = type_function_get_parameter_type(sx, callee_type, i);
		if (!check_assignment_operands(sx, expected_type, &argument))
		{
			node_vector_clear(args);
			return node_broken();
		}
	}

	const item_t return_type = type_function_get_return_type(sx, callee_type);
	const location loc = { expression_get_location(callee).begin, r_loc.end };
	return expression_call(sx, return_type, callee, args, loc);
}

node build_member_expression(syntax *const sx, const node *const base, const size_t name
	, const bool is_arrow, const location op_loc, const location id_loc)
{
	if (!node_is_correct(base))
	{
		return node_broken();
	}

	const item_t base_type = expression_get_type(base);
	item_t struct_type;
	category_t category;

	if (!is_arrow)
	{
		if (!type_is_structure(sx, base_type))
		{
			semantic_error(sx, op_loc, typecheck_member_reference_struct);
			return node_broken();
		}

		struct_type = base_type;
		category = expression_is_lvalue(base) ? LVALUE : RVALUE;
	}
	else
	{
		if (!type_is_struct_pointer(sx, base_type))
		{
			semantic_error(sx, op_loc, typecheck_member_reference_arrow);
			return node_broken();
		}

		struct_type = type_pointer_get_element_type(sx, base_type);
		category = LVALUE;
	}

	const size_t member_amount = type_structure_get_member_amount(sx, struct_type);
	for (size_t i = 0; i < member_amount; i++)
	{
		if (name == type_structure_get_member_name(sx, struct_type, i))
		{
			const item_t type = type_structure_get_member_type(sx, struct_type, i);
			const location loc = { expression_get_location(base).begin, id_loc.end };
			
			return expression_member(sx, type, category, i, is_arrow, base, loc);
		}
	}

	semantic_error(sx, id_loc, no_member, repr_get_name(sx, name));
	return node_broken();
}

node build_upb_expression(syntax *const sx, const node *const nd_fst, const node *const nd_snd)
{
	if (!node_is_correct(nd_fst) || !node_is_correct(nd_snd))
	{
		return node_broken();
	}

	const item_t fst_type = expression_get_type(nd_fst);
	if (!type_is_integer(fst_type))
	{
		semantic_error(sx, expression_get_location(nd_fst), not_int_in_stanfunc);
		return node_broken();
	}

	const item_t snd_type = expression_get_type(nd_snd);
	if (!type_is_array(sx, snd_type))
	{
		semantic_error(sx, expression_get_location(nd_snd), not_array_in_stanfunc);
		return node_broken();
	}

	node nd = node_create(sx, OP_UPB);
	node_add_arg(&nd, TYPE_INTEGER);				// Тип значения поля
	node_add_arg(&nd, RVALUE);						// Категория значения поля
	node_add_arg(&nd, (item_t)expression_get_location(nd_fst).begin);
	node_add_arg(&nd, (item_t)expression_get_location(nd_snd).begin);
	node_set_child(&nd, nd_fst);					// Выражение-операнд
	node_set_child(&nd, nd_snd);					// Выражение-операнд

	return nd;
}

node build_unary_expression(syntax *const sx, node *const nd_operand, const unary_t op_kind, const location op_loc)
{
	if (!node_is_correct(nd_operand))
	{
		return node_broken();
	}

	const item_t operand_type = expression_get_type(nd_operand);

	const location loc = op_kind == UN_POSTINC || op_kind == UN_POSTDEC
		? (location){ op_loc.begin, expression_get_location(nd_operand).end }
		: (location){ expression_get_location(nd_operand).begin, op_loc.end };

	switch (op_kind)
	{
		case UN_POSTINC:
		case UN_POSTDEC:
		case UN_PREINC:
		case UN_PREDEC:
		{
			if (!type_is_arithmetic(operand_type))
			{
				semantic_error(sx, op_loc, typecheck_illegal_increment, op_kind);
				return node_broken();
			}

			if (!expression_is_lvalue(nd_operand))
			{
				semantic_error(sx, op_loc, typecheck_expression_not_lvalue);
				return node_broken();
			}

			return expression_unary(sx, operand_type, RVALUE, nd_operand, op_kind, loc);
		}

		case UN_ADDRESS:
		{
			if (!expression_is_lvalue(nd_operand))
			{
				semantic_error(sx, op_loc, typecheck_invalid_lvalue_addrof);
				return node_broken();
			}

			const item_t result_type = type_pointer(sx, operand_type);
			return fold_unary_expression(sx, result_type, RVALUE, nd_operand, op_kind, loc);
		}

		case UN_INDIRECTION:
		{
			if (!type_is_pointer(sx, operand_type))
			{
				semantic_error(sx, op_loc, typecheck_indirection_requires_pointer);
				return node_broken();
			}

			const item_t type = type_pointer_get_element_type(sx, operand_type);
			return fold_unary_expression(sx, type, LVALUE, nd_operand, op_kind, loc);
		}

		case UN_ABS:
		case UN_PLUS:
		case UN_MINUS:
		{
			if (!type_is_arithmetic(operand_type))
			{
				semantic_error(sx, op_loc, typecheck_unary_expr, operand_type);
				return node_broken();
			}

			return fold_unary_expression(sx, operand_type, RVALUE, nd_operand, op_kind, loc);
		}

		case UN_NOT:
		{
			if (!type_is_integer(operand_type))
			{
				semantic_error(sx, op_loc, typecheck_unary_expr, operand_type);
				return node_broken();
			}

			return fold_unary_expression(sx, TYPE_INTEGER, RVALUE, nd_operand, op_kind, loc);
		}

		case UN_LOGNOT:
		{
			if (!type_is_scalar(sx, operand_type))
			{
				semantic_error(sx, op_loc, typecheck_unary_expr, operand_type);
				return node_broken();
			}

			return fold_unary_expression(sx, TYPE_INTEGER, RVALUE, nd_operand, op_kind, loc);
		}

		default:
			// Unknown unary operator
			return node_broken();
	}
}

node build_binary_expression(syntax *const sx, node *const LHS, node *const RHS
	, const binary_t op_kind, const location op_loc)
{
	if (!node_is_correct(LHS) || !node_is_correct(RHS))
	{
		return node_broken();
	}

	const item_t left_type = expression_get_type(LHS);
	const item_t right_type = expression_get_type(RHS);

	if (operation_is_assignment(op_kind))
	{
		if (!expression_is_lvalue(LHS))
		{
			semantic_error(sx, op_loc, unassignable);
			return node_broken();
		}

		if (!check_assignment_operands(sx, left_type, RHS))
		{
			return node_broken();
		}
	}

	const location loc = { expression_get_location(LHS).begin, expression_get_location(RHS).end };

	switch (op_kind)
	{
		case BIN_REM:
		case BIN_SHL:
		case BIN_SHR:
		case BIN_AND:
		case BIN_XOR:
		case BIN_OR:
		{
			if (!type_is_integer(left_type) || !type_is_integer(right_type))
			{
				semantic_error(sx, op_loc, typecheck_binary_expr);
				return node_broken();
			}

			return fold_binary_expression(sx, TYPE_INTEGER, LHS, RHS, op_kind, loc);
		}

		case BIN_MUL:
		case BIN_DIV:
		case BIN_ADD:
		case BIN_SUB:
		{
			if (!type_is_arithmetic(left_type) || !type_is_arithmetic(right_type))
			{
				semantic_error(sx, op_loc, typecheck_binary_expr);
				return node_broken();
			}

			const item_t type = usual_arithmetic_conversions(left_type, right_type);
			return fold_binary_expression(sx, type, LHS, RHS, op_kind, loc);
		}

		case BIN_LT:
		case BIN_GT:
		case BIN_LE:
		case BIN_GE:
		{
			if (!type_is_arithmetic(left_type) || !type_is_arithmetic(right_type))
			{
				semantic_error(sx, op_loc, typecheck_binary_expr);
				return node_broken();
			}

			return fold_binary_expression(sx, TYPE_INTEGER, LHS, RHS, op_kind, loc);
		}

		case BIN_LOG_AND:
		case BIN_LOG_OR:
		{
			if (!type_is_scalar(sx, left_type) || !type_is_scalar(sx, right_type))
			{
				semantic_error(sx, op_loc, typecheck_binary_expr);
				return node_broken();
			}

			return fold_binary_expression(sx, TYPE_INTEGER, LHS, RHS, op_kind, loc);
		}

		case BIN_EQ:
		case BIN_NE:
		{
			if (type_is_floating(left_type) || type_is_floating(right_type))
			{
				warning(sx->io, variable_deviation);
			}

			if ((type_is_arithmetic(left_type) && type_is_arithmetic(right_type))
				|| (type_is_pointer(sx, left_type) && type_is_null_pointer(right_type))
				|| (type_is_null_pointer(left_type) && type_is_pointer(sx, right_type))
				|| left_type == right_type)
			{
				return expression_binary(sx, TYPE_INTEGER, LHS, RHS, op_kind, loc);
			}

			semantic_error(sx, op_loc, typecheck_binary_expr);
			return node_broken();
		}

		case BIN_ASSIGN:
			return fold_binary_expression(sx, left_type, LHS, RHS, op_kind, loc);

		case BIN_REM_ASSIGN:
		case BIN_SHL_ASSIGN:
		case BIN_SHR_ASSIGN:
		case BIN_AND_ASSIGN:
		case BIN_XOR_ASSIGN:
		case BIN_OR_ASSIGN:
		{
			if (!type_is_integer(left_type) || !type_is_integer(right_type))
			{
				semantic_error(sx, op_loc, typecheck_binary_expr);
				return node_broken();
			}

			return fold_binary_expression(sx, left_type, LHS, RHS, op_kind, loc);
		}

		case BIN_MUL_ASSIGN:
		case BIN_DIV_ASSIGN:
		case BIN_ADD_ASSIGN:
		case BIN_SUB_ASSIGN:
		{
			if (!type_is_arithmetic(left_type) || !type_is_arithmetic(right_type))
			{
				semantic_error(sx, op_loc, typecheck_binary_expr);
				return node_broken();
			}

			return fold_binary_expression(sx, left_type, LHS, RHS, op_kind, loc);
		}

		case BIN_COMMA:
			return fold_binary_expression(sx, right_type, LHS, RHS, op_kind, loc);

		default:
			// Unknown binary operator
			return node_broken();
	}
}

node build_ternary_expression(syntax *const sx, node *const nd_left, node *const nd_middle, node *const nd_right
	, const location op_loc)
{
	if (!node_is_correct(nd_left) || !node_is_correct(nd_middle) || !node_is_correct(nd_right))
	{
		return node_broken();
	}

	const item_t left_type = expression_get_type(nd_left);
	const item_t middle_type = expression_get_type(nd_middle);
	const item_t right_type = expression_get_type(nd_right);

	if (!type_is_scalar(sx, left_type))
	{
		semantic_error(sx, expression_get_location(nd_left), typecheck_statement_requires_scalar);
		return node_broken();
	}

	const location loc = { expression_get_location(nd_left).begin, expression_get_location(nd_right).end };

	if (type_is_arithmetic(middle_type) && type_is_arithmetic(right_type))
	{
		const item_t type = usual_arithmetic_conversions(middle_type, right_type);
		return fold_ternary_expression(sx, type, nd_left, nd_middle, nd_right, loc);
	}

	if (type_is_pointer(sx, middle_type) && type_is_null_pointer(right_type))
	{
		return fold_ternary_expression(sx, middle_type, nd_left, nd_middle, nd_right, loc);
	}

	if (type_is_null_pointer(middle_type) && type_is_pointer(sx, right_type))
	{
		return fold_ternary_expression(sx, right_type, nd_left, nd_middle, nd_right, loc);
	}

	if (middle_type == right_type)
	{
		return fold_ternary_expression(sx, right_type, nd_left, nd_middle, nd_right, loc);
	}

	semantic_error(sx, op_loc, typecheck_cond_incompatible_operands);
	return node_broken();
}

node build_init_list_expression(syntax *const sx, node_vector *const vec
	, const location l_loc, const location r_loc)
{
	const size_t actual_inits = node_vector_size(vec);
	if (actual_inits == 0)
	{
		semantic_error(sx, l_loc, empty_init);
		node_vector_clear(vec);
		return node_broken();
	}

	const location loc = { l_loc.begin, r_loc.end };
	return expression_list(sx, vec, loc);
}
