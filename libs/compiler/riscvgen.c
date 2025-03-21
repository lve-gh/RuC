/*
 *	Copyright 2021 Andrey Terekhov, Ivan S. Arkhipov
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

#include "riscvgen.h"
#include "AST.h"
#include "hash.h"
#include "operations.h"
#include "tree.h"
#include "uniprinter.h"


#ifndef max
	#define max(a, b) ((a) > (b) ? (a) : (b))
#endif


static const size_t BUFFER_SIZE = 65536; /**< Размер буфера для тела функции */
static const size_t HASH_TABLE_SIZE = 1024; /**< Размер хеш-таблицы для смещений и регистров */
static const bool IS_ON_STACK = true; /**< Хранится ли переменная на стеке */

static const size_t WORD_LENGTH = 4;	  /**< Длина слова данных */
static const size_t HALF_WORD_LENGTH = 2; /**< Длина половины слова данных */

static const size_t LOW_DYN_BORDER = 0x10010000; /**< Нижняя граница динамической памяти */
static const size_t HEAP_DISPL = 8000; /**< Смещение кучи относительно глобальной памяти */

static const size_t SP_SIZE = 4; /**< Размер регистра sp для его сохранения */
static const size_t RA_SIZE = 4; /**< Размер регистра ra для его сохранения */

static const size_t TEMP_FP_REG_AMOUNT = 12; /**< Количество временных регистров для чисел с плавающей точкой */
static const size_t TEMP_REG_AMOUNT = 7; /**< Количество обычных временных регистров */
static const size_t ARG_REG_AMOUNT = 8; /**< Количество регистров-аргументов для функций (и int, и float) */

static const size_t PRESERVED_REG_AMOUNT = 12; /**< Количество сохраняемых регистров общего назначения */
static const size_t PRESERVED_FP_REG_AMOUNT = 12; /**< Количество сохраняемых регистров с плавающей точкой */

static const bool FROM_LVALUE = 1; /**< Получен ли rvalue из lvalue */

/**< Смещение в стеке для сохранения оберегаемых регистров, без учёта оптимизаций */
static const size_t FUNC_DISPL_PRESEREVED = /* за sp */ 4 + /* за ra */ 4 +
											/* fs0-fs11 (двойная точность): */ 12 * 8 + /* s0-s11: */ 12 * 4;

int array_sizes[10000]; // храним здесь размеры массивов

int array_declaration_sizes[10000]; 

//item_t true_loc[10000];

int current_memory_location = 0; // на каком в месте памяти стоим

int is_declaration = false;

bool emit_literal = true;

bool null_registers = false;

int link_on_true_location[1000];

int prev_size = 1;
int prev_declaration_size = 1;
item_t lvalue_location = 0;
int for_strncpy_counter = 0;
	// Назначение регистров взято из документации SYSTEM V APPLICATION BINARY INTERFACE RISCV RISC Processor, 3rd Edition
typedef enum RISCV_REGISTER
{
	R_ZERO, /**< Always has the value 0 */

	R_A0,
	R_A1, /**< Used for expression evaluations and to hold the integer
			  and pointer type function return values */

	R_A2,
	R_A3,
	R_A4,
	R_A5,
	R_A6,
	R_A7, /**< Used for passing arguments to functions; values are not
			  preserved across function calls */

	R_T0,
	R_T1,
	R_T2,
	R_T3,
	R_T4,
	R_T5,
	R_T6, /**< Temporary registers used for expression evaluation;
			  values are not preserved across function calls */


	R_S0,
	R_S1,
	R_S2,
	R_S3,
	R_S4,
	R_S5,
	R_S6,
	R_S7,
	R_S8,
	R_S9,
	R_S10,
	R_S11, /**< Saved registers; values are preserved across function calls */

	R_GP, /**< Global pointer and context pointer */
	R_SP, /**< Stack pointer */
	R_FP, /**< Saved register (like s0-s7) or frame pointer */
	R_RA, /**< Return address. The return address is the location to
			  which a function should return control */

	// Регистры для работы с числами с плавающей точкой
	// Для чисел с двойной точностью используется пара регистров:
	// - регистр с чётным номером содержит младшие 32 бита числа;
	// - регистр с нечётным номером содержит старшие 32 бита числа.
	R_FA0,
	R_FA1, /**< Used to hold floating-point type function results;
			   single-precision uses f0 and double-precision uses
			   the register pair f0..f1 */

	R_FA2,
	R_FA3,
	R_FA4,
	R_FA5,
	R_FA6,
	R_FA7, /**< Used for passing arguments to functions */

	R_FT0,
	R_FT1,
	R_FT2,
	R_FT3,
	R_FT4,
	R_FT5,
	R_FT6,
	R_FT7,
	R_FT8,
	R_FT9,
	R_FT10,
	R_FT11, /**< Temporary registers */

	R_FS0,
	R_FS1,
	R_FS2,
	R_FS3,
	R_FS4,
	R_FS5,
	R_FS6,
	R_FS7,
	R_FS8,
	R_FS9,
	R_FS10,
	R_FS11 /**< Saved registers; their values are preserved across function calls */
} riscv_register_t;

// Назначение команд
typedef enum INSTRUCTION
{
	IC_RISCV_MOVE, /**< RISCV Pseudo-Instruction. Move the contents of one register to another */
	IC_RISCV_LI,   /**< RISCV Pseudo-Instruction. Load a constant into a register */
	IC_RISCV_NOT,  /**< RISCV Pseudo-Instruction. Flips the bits of the source register and
					   stores them in the destination register (не из вышеуказанной книги) */

	IC_RISCV_ADDI, /**< To add a constant to a 3	2-bit integer. If overflow occurs, then trap */
	IC_RISCV_SLLI, /**< To left-shift a word by a fixed number of bits */
	IC_RISCV_SRAI, /**< To execute an arithmetic right-shift of a word by a fixed number of bits */
	IC_RISCV_ANDI, /**< To do a bitwise logical AND with a constant */
	IC_RISCV_XORI, /**< To do a bitwise logical Exclusive OR with a constant */
	IC_RISCV_ORI,  /**< To do a bitwise logical OR with a constant */

	IC_RISCV_ADD, /**< To add 32-bit integers. If an overflow occurs, then trap */
	IC_RISCV_SUB, /**< To subtract 32-bit integers. If overflow occurs, then trap */
	IC_RISCV_MUL, /**< To multiply two words and write the result to a GPR */
	IC_RISCV_DIV, /**< DIV performs a signed 32-bit integer division, and places
					  the 32-bit quotient result in the destination register */
	IC_RISCV_REM, /**< MOD performs a signed 32-bit integer division, and places
					  the 32-bit remainder result in the destination register.
					  The remainder result has the same sign as the dividend */
	IC_RISCV_SLL, /**< To left-shift a word by a variable number of bits */
	IC_RISCV_SRA, /**< To execute an arithmetic right-shift of a word by a variable number of bits */
	IC_RISCV_AND, /**< To do a bitwise logical AND */
	IC_RISCV_XOR, /**< To do a bitwise logical Exclusive OR */
	IC_RISCV_OR,  /**< To do a bitwise logical OR */

	IC_RISCV_SW, /**< To store a word to memory */
	IC_RISCV_LW, /**< To load a word from memory as a signed value */

	IC_RISCV_JR,  /**< To execute a branch to an instruction address in a register */
	IC_RISCV_JAL, /**< To execute a procedure call within the current 256MB-aligned region */
	IC_RISCV_J,	  /**< To branch within the current 256 MB-aligned region */

	IC_RISCV_BLT, /**< Branch on Less Than Zero.
					  To test a GPR then do a PC-relative conditional branch */
	IC_RISCV_BGE, /**< Branch on Greater Than or Equal to Zero.
					  To test a GPR then do a PC-relative conditional branch */
	IC_RISCV_BEQ, /**< Branch on Equal.
					  To compare GPRs then do a PC-relative conditional branch */
	IC_RISCV_BNE, /**< Branch on Not Equal.
					  To compare GPRs then do a PC-relative conditional branch */
	IC_RISCV_FEQ, /**< Equal comparison between floating-point registers rs1 and rs2
					   and record the Boolean result in integer register rd:
					   x[rd] = f[rs1] == f[rs2] */
	IC_RISCV_FLT, /**< x[rd] = f[rs1] < f[rs2] */
	IC_RISCV_FLE, /**< x[rd] = f[rs1] <= f[rs2] */

	IC_RISCV_LA, /**< Load the address of a named memory
				 location into a register (не из вышеуказанной книги)*/

	IC_RISCV_SLTIU, /**< Set on Less Than Immediate Unsigned.
						To record the result of an unsigned less-than comparison with a constant. */

	IC_RISCV_NOP, /**< To perform no operation */

	/** Floating point operations. Single precision. */
	// TODO: use all this instructions in real operations, now they are only declared
	IC_RISCV_FADD, /**< To add FP values. */
	IC_RISCV_FSUB, /**< To subtract FP values. */
	IC_RISCV_FMUL, /**< To multiply FP values. */
	IC_RISCV_FDIV, /**< To divide FP values. */
	IC_RISCV_FMVI, /**< To move data from int register to float register. */

	IC_RISCV_FABS, /**< Floating Point Absolute Value Single Precision*/
	IC_RISCV_DABS, /**< Floating Point Absolute Value Double Precition*/

	// TODO: here is no abs for integers in risc-v
	IC_RISCV_ABS, /**< GPR absolute value (не из вышеуказанной книги). RISCV Pseudo-Instruction. */

	// TODO: remove this instruction, they are not presented in risc-v

	IC_RISCV_FSD, /**< RISCV Pseudo instruction. To store a doubleword from an FPR to memory. */
	IC_RISCV_FLD, /**< RISCV Pseudo instruction. To load a doubleword from memory to an FPR. */

	IC_RISCV_FMOV, /**< The value in first FPR is placed into second FPR. */

	IC_RISCV_MFC_1,	 /**< Move word from Floating Point.
						 To copy a word from an FPU (CP1) general register to a GPR. */
	IC_RISCV_MFHC_1, /**< To copy a word from the high half of an FPU (CP1)
					 general register to a GPR. */

	IC_RISCV_CVT_D_S, /**< To convert an FP value to double FP. */
	IC_RISCV_CVT_S_W, /**< To convert fixed point value to single FP. */
	IC_RISCV_CVT_W_S, /**< To convert single FP to fixed point value */
	IC_RISCV_FMV_D_X,
	IC_RISCV_FCVT_D_W
} riscv_instruction_t;


typedef enum LABEL
{
	L_MAIN,			 /**< Тип метки -- главная функция */
	L_FUNC,			 /**< Тип метки -- вход в функцию */
	L_NEXT,			 /**< Тип метки -- следующая функция */
	L_FUNCEND,		 /**< Тип метки -- выход из функции */
	L_STRING,		 /**< Тип метки -- строка */
	L_THEN,			 /**< Тип метки -- переход по then */
	L_ELSE,			 /**< Тип метки -- переход по else */
	L_END,			 /**< Тип метки -- переход в конец конструкции */
	L_BEGIN_CYCLE,	 /**< Тип метки -- переход в начало цикла */
	L_CASE,			 /**< Тип метки -- переход по case */
	L_CASE_CONDITION, /**< Тип метки -- условие для case */
	L_DEFAULT
} riscv_label_t;

typedef struct label
{
	riscv_label_t kind;
	size_t num;
} label;


/** Kinds of lvalue */
typedef enum LVALUE_KIND
{
	LVALUE_KIND_STACK,
	LVALUE_KIND_REGISTER,
} lvalue_kind_t;

typedef struct lvalue
{
	lvalue_kind_t kind;		   /**< Value kind */
	riscv_register_t base_reg; /**< Base register */
	union					   /**< Value location */
	{
		item_t reg_num; /**< Register where the value is stored */
		item_t displ;	/**< Stack displacement where the value is stored */
	} loc;
	item_t type; /**< Value type */
} lvalue;


/** Kinds of rvalue */
typedef enum RVALUE_KIND
{
	RVALUE_KIND_CONST, // Значит, запомнили константу и потом обработали её
	RVALUE_KIND_REGISTER,
	RVALUE_KIND_VOID,
} rvalue_kind_t;

typedef struct rvalue
{
	rvalue_kind_t kind; /**< Value kind */
	item_t type;		/**< Value type */
	bool from_lvalue;	/**< Was the rvalue instance formed from lvalue */
	union
	{
		item_t reg_num;	  /**< Where the value is stored */
		item_t int_val;	  /**< Value of integer (character, boolean) literal */
		double float_val; /**< Value of floating literal */
		item_t str_index; /**< Index of pre-declared string */
	} val;
} rvalue;


typedef struct encoder
{
	syntax *sx; /**< Структура syntax с таблицами */

	size_t max_displ;	 /**< Максимальное смещение от sp */
	size_t global_displ; /**< Смещение от gp */

	hash displacements; /**< Хеш таблица с информацией о расположении идентификаторов:
							@c key		- ссылка на таблицу идентификаторов
							@c value[0]	- флаг, лежит ли переменная на стеке или в регистре
							@c value[1]	- смещение или номер регистра */

	riscv_register_t next_register; /**< Следующий обычный регистр для выделения */
	riscv_register_t next_float_register; /**< Следующий регистр с плавающей точкой для выделения */

	size_t label_num;			/**< Номер метки */
	size_t case_label_num;		/**< Номер метки-перехода по case */
	label label_else;			/**< Метка перехода на else */
	label label_continue;		/**< Метка continue */
	label label_if_true;		/**< Метка перехода по then */
	label label_if_false;		/**< Метка перехода по else */
	label label_break;			/**< Метка break */
	label label_end_counter;
	size_t curr_function_ident; /**< Идентификатор текущей функций */

	bool registers[22]; /**< Информация о занятых регистрах */

	size_t scope_displ; /**< Смещение */
} encoder;


static const rvalue RVALUE_ONE = { .kind = RVALUE_KIND_CONST, .type = TYPE_INTEGER, .val.int_val = 1 };
static const rvalue RVALUE_NEGATIVE_ONE = { .kind = RVALUE_KIND_CONST, .type = TYPE_INTEGER, .val.int_val = -1 };
static const rvalue RVALUE_ZERO = { .kind = RVALUE_KIND_CONST, .type = TYPE_INTEGER, .val.int_val = 0 };
static const rvalue RVALUE_VOID = { .kind = RVALUE_KIND_CONST };


static lvalue emit_lvalue(encoder *const enc, const node *const nd);
static void emit_binary_operation(encoder *const enc, const rvalue *const dest, const rvalue *const first_operand,
								  const rvalue *const second_operand, const binary_t operator);
static rvalue emit_expression(encoder *const enc, const node *const nd);
static rvalue emit_void_expression(encoder *const enc, const node *const nd);
static void emit_structure_init(encoder *const enc, const lvalue *const target, const node *const initializer);
static void emit_statement(encoder *const enc, const node *const nd);


static size_t riscv_type_size(const syntax *const sx, const item_t type)
{
	if (type_is_structure(sx, type))
	{
		size_t size = 0;
		const size_t amount = type_structure_get_member_amount(sx, type);
		for (size_t i = 0; i < amount; i++)
		{
			const item_t member_type = type_structure_get_member_type(sx, type, i);
			size += riscv_type_size(sx, member_type);
		}
		return size;
	}
	else
	{
		return (type_is_floating(sx, type) ? 2 : 1) * WORD_LENGTH;
	}
}

/**
 *	Locks certain register
 *
 *	@param	enc					Encoder
 *	@param	reg					Register to lock
 */
static void lock_register(encoder *const enc, const riscv_register_t reg)
{
	switch (reg)
	{
		case R_T0:
		case R_T1:
		case R_T2:
		case R_T3:
		case R_T4:
		case R_T5:
		case R_T6:
			if (!enc->registers[reg - R_T0])
			{
				// Регистр занят => освобождаем
				enc->registers[reg - R_T0] = true;
			}
			return;

		case R_FT0:
		case R_FT1:
		case R_FT2:
		case R_FT3:
		case R_FT4:
		case R_FT5:
		case R_FT6:
		case R_FT7:
		case R_FT8:
		case R_FT9:
		case R_FT10:
		case R_FT11:
			if (!enc->registers[reg - R_FT0 + /* индекс R_FT0 в enc->registers */ TEMP_REG_AMOUNT])
			{
				enc->registers[reg - R_FT0 + TEMP_REG_AMOUNT] = true;
			}
			return;

		default: // Не временный регистр и пришли сюда => и так захвачен
			return;
	}
}

/**
 *	Takes the first free register
 *
 *	@param	enc					Encoder
 *
 *	@return	General purpose register
 */
static riscv_register_t get_register(encoder *const enc)
{
	// Ищем первый свободный регистр
	size_t i = 0;
	while ((i < TEMP_REG_AMOUNT) && (enc->registers[i]))
	{
		i++;
	}

	//assert(i != TEMP_REG_AMOUNT);

	// Занимаем его
	enc->registers[i] = true;

	return i + R_T0;
}

/**
 *	Takes the first free floating point register
 *
 *	@param	enc					Encoder
 *
 *	@return	Register			Floating point register
 */
static riscv_register_t get_float_register(encoder *const enc)
{
	// Ищем первый свободный регистр
	size_t i = TEMP_REG_AMOUNT;
	while ((i < TEMP_FP_REG_AMOUNT + TEMP_REG_AMOUNT) && (enc->registers[i]))
	{
		i += 2; /* т.к. операции с одинарной точностью */
	}

	assert(i != TEMP_FP_REG_AMOUNT + TEMP_REG_AMOUNT);

	// Занимаем его
	enc->registers[i] = true;

	return i + R_FT0 - /* за индекс R_FT0 в enc->registers */ TEMP_REG_AMOUNT;
}

/**
 *	Free register
 *
 *	@param	enc					Encoder
 *	@param	reg					Register to set as free
 */
static void free_register(encoder *const enc, const riscv_register_t reg)
{
	switch (reg)
	{
		case R_T0:
		case R_T1:
		case R_T2:
		case R_T3:
		case R_T4:
		case R_T5:
		case R_T6:
			if (enc->registers[reg - R_T0])
			{
				// Регистр занят => освобождаем
				enc->registers[reg - R_T0] = false;
			}
			return;

		case R_FT0:
		case R_FT1:
		case R_FT2:
		case R_FT3:
		case R_FT4:
		case R_FT5:
		case R_FT6:
		case R_FT7:
		case R_FT8:
		case R_FT9:
		case R_FT10:
		case R_FT11:
			if (enc->registers[reg - R_FT0 + /* индекс R_FT0 в enc->registers */ TEMP_REG_AMOUNT])
			{
				// Регистр занят => освобождаем
				enc->registers[reg - R_FT0 + TEMP_REG_AMOUNT] = false;
			}
			return;

		default: // Не временный регистр => освобождать не надо
			return;
	}
}

/**
 *	Free register occupied by rvalue
 *
 *	@param	enc					Encoder
 *	@param	rval				Rvalue to be freed
 */
static void free_rvalue(encoder *const enc, const rvalue *const rval)
{
	if ((rval->kind == RVALUE_KIND_REGISTER) && (!rval->from_lvalue))
	{
		free_register(enc, rval->val.reg_num);
	}
}

/**	Get RISCV assembler binary instruction from binary_t type
 *
 *	@param	operation_type		Type of operation in AST
 *	@param	is_imm				@c True if the instruction is immediate, @c False otherwise
 *	@param	is_floating		    @c True if the operands are float, @c False otherwise
 *
 *	@return	RISCV binary instruction
 */
static riscv_instruction_t get_bin_instruction(const binary_t operation_type, const bool is_imm, const bool is_floating)
{
	switch (operation_type)
	{
		case BIN_ADD_ASSIGN:
		case BIN_ADD:
			return (is_floating) ? IC_RISCV_FADD : (is_imm) ? IC_RISCV_ADDI : IC_RISCV_ADD;

		case BIN_SUB_ASSIGN:
		case BIN_SUB:
			return (is_floating) ? IC_RISCV_FSUB : (is_imm) ? IC_RISCV_ADDI : IC_RISCV_SUB;

		case BIN_MUL_ASSIGN:
		case BIN_MUL:
			return (is_floating) ? IC_RISCV_FMUL : IC_RISCV_MUL;

		case BIN_DIV_ASSIGN:
		case BIN_DIV:
			return (is_floating) ? IC_RISCV_FDIV : IC_RISCV_DIV;

		case BIN_REM_ASSIGN:
		case BIN_REM:
			return IC_RISCV_REM;

		case BIN_SHL_ASSIGN:
		case BIN_SHL:
			return (is_imm) ? IC_RISCV_SLLI : IC_RISCV_SLL;

		case BIN_SHR_ASSIGN:
		case BIN_SHR:
			return (is_imm) ? IC_RISCV_SRAI : IC_RISCV_SRA;

		case BIN_AND_ASSIGN:
		case BIN_AND:
			return (is_imm) ? IC_RISCV_ANDI : IC_RISCV_AND;

		case BIN_XOR_ASSIGN:
		case BIN_XOR:
			return (is_imm) ? IC_RISCV_XORI : IC_RISCV_XOR;

		case BIN_OR_ASSIGN:
		case BIN_OR:
			return (is_imm) ? IC_RISCV_ORI : IC_RISCV_OR;

		case BIN_EQ:
			return is_floating ? IC_RISCV_FEQ : IC_RISCV_BEQ;
		case BIN_NE:
			return IC_RISCV_BNE;
		case BIN_GT:
		case BIN_LT:
			return is_floating ? IC_RISCV_FLT : IC_RISCV_BLT;
		case BIN_GE:
		case BIN_LE:
			return is_floating ? IC_RISCV_FLE : IC_RISCV_BGE;

		default:
			return IC_RISCV_NOP;
	}
}

static void riscv_register_to_io(universal_io *const io, const riscv_register_t reg)
{
	switch (reg)
	{
		case R_ZERO:
			uni_printf(io, "x0");
			break;

		case R_A0:
			uni_printf(io, "a0");
			break;
		case R_A1:
			uni_printf(io, "a1");
			break;
		case R_A2:
			uni_printf(io, "a2");
			break;
		case R_A3:
			uni_printf(io, "a3");
			break;
		case R_A4:
			uni_printf(io, "a4");
			break;
		case R_A5:
			uni_printf(io, "a5");
			break;
		case R_A6:
			uni_printf(io, "a6");
			break;
		case R_A7:
			uni_printf(io, "a7");
			break;
		case R_T0:
			uni_printf(io, "t0");
			break;
		case R_T1:
			uni_printf(io, "t1");
			break;
		case R_T2:
			uni_printf(io, "t2");
			break;
		case R_T3:
			uni_printf(io, "t3");
			break;
		case R_T4:
			uni_printf(io, "t4");
			break;
		case R_T5:
			uni_printf(io, "t5");
			break;
		case R_T6:
			uni_printf(io, "t6");
			break;

		case R_S0:
			uni_printf(io, "s0");
			break;
		case R_S1:
			uni_printf(io, "s1");
			break;
		case R_S2:
			uni_printf(io, "s2");
			break;
		case R_S3:
			uni_printf(io, "s3");
			break;
		case R_S4:
			uni_printf(io, "s4");
			break;
		case R_S5:
			uni_printf(io, "s5");
			break;
		case R_S6:
			uni_printf(io, "s6");
			break;
		case R_S7:
			uni_printf(io, "s7");
			break;
		case R_S8:
			uni_printf(io, "s8");
			break;
		case R_S9:
			uni_printf(io, "s9");
			break;
		case R_S10:
			uni_printf(io, "s10");
			break;
		case R_S11:
			uni_printf(io, "s11");
			break;

		case R_GP:
			uni_printf(io, "gp");
			break;
		case R_SP:
			uni_printf(io, "sp");
			break;
		case R_FP:
			uni_printf(io, "fp");
			break;
		case R_RA:
			uni_printf(io, "ra");
			break;

		case R_FT0:
			uni_printf(io, "f0");
			break;
		case R_FT1:
			uni_printf(io, "f1");
			break;
		case R_FT2:
			uni_printf(io, "f2");
			break;
		case R_FT3:
			uni_printf(io, "f3");
			break;
		case R_FT4:
			uni_printf(io, "f4");
			break;
		case R_FT5:
			uni_printf(io, "f5");
			break;
		case R_FT6:
			uni_printf(io, "f6");
			break;
		case R_FT7:
			uni_printf(io, "f7");
			break;

		case R_FS0:
			uni_printf(io, "f8");
			break;
		case R_FS1:
			uni_printf(io, "f9");
			break;

		case R_FA0:
			uni_printf(io, "f10");
			break;
		case R_FA1:
			uni_printf(io, "f11");
			break;
		case R_FA2:
			uni_printf(io, "f12");
			break;
		case R_FA3:
			uni_printf(io, "f13");
			break;
		case R_FA4:
			uni_printf(io, "f14");
			break;
		case R_FA5:
			uni_printf(io, "f15");
			break;
		case R_FA6:
			uni_printf(io, "f16");
			break;
		case R_FA7:
			uni_printf(io, "f17");
			break;

		case R_FS2:
			uni_printf(io, "f18");
			break;
		case R_FS3:
			uni_printf(io, "f19");
			break;
		case R_FS4:
			uni_printf(io, "f20");
			break;
		case R_FS5:
			uni_printf(io, "f21");
			break;
		case R_FS6:
			uni_printf(io, "f22");
			break;
		case R_FS7:
			uni_printf(io, "f23");
			break;
		case R_FS8:
			uni_printf(io, "f24");
			break;
		case R_FS9:
			uni_printf(io, "f25");
			break;
		case R_FS10:
			uni_printf(io, "f26");
			break;
		case R_FS11:
			uni_printf(io, "f27");
			break;

		case R_FT8:
			uni_printf(io, "f28");
			break;
		case R_FT9:
			uni_printf(io, "f29");
			break;
		case R_FT10:
			uni_printf(io, "f30");
			break;
		case R_FT11:
			uni_printf(io, "f31");
			break;
	}
}

static void instruction_to_io(universal_io *const io, const riscv_instruction_t instruction)
{
	switch (instruction)
	{
		case IC_RISCV_MOVE:
			uni_printf(io, "mv");
			break;
		case IC_RISCV_LI:
			uni_printf(io, "li");
			break;
		case IC_RISCV_LA:
			uni_printf(io, "la");
			break;
		case IC_RISCV_NOT:
			uni_printf(io, "not");
			break;

		case IC_RISCV_ADDI:
			uni_printf(io, "addi");
			break;
		case IC_RISCV_SLLI:
			uni_printf(io, "slli");
			break;
		case IC_RISCV_SRAI:
			uni_printf(io, "srai");
			break;
		case IC_RISCV_ANDI:
			uni_printf(io, "andi");
			break;
		case IC_RISCV_XORI:
			uni_printf(io, "xori");
			break;
		case IC_RISCV_ORI:
			uni_printf(io, "ori");
			break;

		case IC_RISCV_ADD:
			uni_printf(io, "add");
			break;
		case IC_RISCV_SUB:
			uni_printf(io, "sub");
			break;
		case IC_RISCV_MUL:
			uni_printf(io, "mul");
			break;
		case IC_RISCV_DIV:
			uni_printf(io, "div");
			break;
		case IC_RISCV_REM:
			uni_printf(io, "rem");
			break;
		case IC_RISCV_SLL:
			uni_printf(io, "sll");
			break;
		case IC_RISCV_SRA:
			uni_printf(io, "sra");
			break;
		case IC_RISCV_AND:
			uni_printf(io, "and");
			break;
		case IC_RISCV_XOR:
			uni_printf(io, "xor");
			break;
		case IC_RISCV_OR:
			uni_printf(io, "or");
			break;

		case IC_RISCV_SW:
			uni_printf(io, "sw");
			break;
		case IC_RISCV_LW:
			uni_printf(io, "lw");
			break;

		case IC_RISCV_JR:
			uni_printf(io, "jr");
			break;
		case IC_RISCV_JAL:
			uni_printf(io, "jal");
			break;
		case IC_RISCV_J:
			uni_printf(io, "j");
			break;


		case IC_RISCV_BLT:
			uni_printf(io, "blt");
			break;
		case IC_RISCV_BGE:
			uni_printf(io, "bge");
			break;
		case IC_RISCV_BEQ:
			uni_printf(io, "beq");
			break;
		case IC_RISCV_BNE:
			uni_printf(io, "bne");
			break;
		case IC_RISCV_FEQ:
			uni_printf(io, "feq.d");
			break;
		case IC_RISCV_FLT:
			uni_printf(io, "flt.d");
			break;
		case IC_RISCV_FLE:
			uni_printf(io, "fle.d");
			break;

		case IC_RISCV_SLTIU:
			uni_printf(io, "sltiu");
			break;

		case IC_RISCV_NOP:
			uni_printf(io, "nop");
			break;

		case IC_RISCV_FADD:
			uni_printf(io, "fadd.d");
			break;
		case IC_RISCV_FSUB:
			uni_printf(io, "fsub.d");
			break;
		case IC_RISCV_FMUL:
			uni_printf(io, "fmul.d");
			break;
		case IC_RISCV_FDIV:
			uni_printf(io, "fdiv.d");
			break;
		case IC_RISCV_FMVI:
			uni_printf(io, "fmv.w.x");
			break;

		case IC_RISCV_FABS:
			uni_printf(io, "fabs.s");
			break;
		case IC_RISCV_DABS:
			uni_printf(io, "fabs.d");
			break;

		// TODO: remove later
		case IC_RISCV_ABS:
			uni_printf(io, "abs");
			break;

		case IC_RISCV_FSD:
			uni_printf(io, "fsd");
			break;
		case IC_RISCV_FLD:
			uni_printf(io, "fld");
			break;

		case IC_RISCV_FMOV:
			uni_printf(io, "mov");
			break;

		case IC_RISCV_MFC_1:
			uni_printf(io, "mfc1");
			break;
		case IC_RISCV_MFHC_1:
			uni_printf(io, "mfhc1");
			break;

		case IC_RISCV_CVT_D_S:
			uni_printf(io, "cvt.d.s");
			break;
		case IC_RISCV_CVT_S_W:
			uni_printf(io, "cvt.s.w");
			break;
		case IC_RISCV_CVT_W_S:
			uni_printf(io, "cvt.w.s");
		case IC_RISCV_FMV_D_X:
			uni_printf(io, "fmv.d.x");
		case IC_RISCV_FCVT_D_W:
			uni_printf(io, "fcvt.d.w");
			break;
	}
}

// Начало инструкции:	instr reg,
static void to_code_begin(universal_io *const io, const riscv_instruction_t instruction, const riscv_register_t reg)
{
	uni_printf(io, "\t");
	instruction_to_io(io, instruction);
	uni_printf(io, " ");
	riscv_register_to_io(io, reg);
	uni_printf(io, ", ");
}

// Вид инструкции:	instr	fst_reg, snd_reg, thd_reg
static void to_code_3R(universal_io *const io, const riscv_instruction_t instruction, const riscv_register_t fst_reg,
					   const riscv_register_t snd_reg, const riscv_register_t thd_reg)
{
	// printf("here\n");
	to_code_begin(io, instruction, fst_reg);
	riscv_register_to_io(io, snd_reg);
	uni_printf(io, ", ");
	riscv_register_to_io(io, thd_reg);
	uni_printf(io, "\n");
}

// Вид инструкции:	instr	fst_reg, snd_reg
static void to_code_2R(universal_io *const io, const riscv_instruction_t instruction, const riscv_register_t fst_reg,
					   const riscv_register_t snd_reg)
{
	// printf("here\n");
	to_code_begin(io, instruction, fst_reg);
	riscv_register_to_io(io, snd_reg);
	// printf();
	uni_printf(io, "\n");
}

// Вид инструкции:	instr	fst_reg, snd_reg, imm
static void to_code_2R_I(universal_io *const io, const riscv_instruction_t instruction, const riscv_register_t fst_reg,
						 const riscv_register_t snd_reg, const item_t imm)
{
	// printf("here\n");
	to_code_begin(io, instruction, fst_reg);
	riscv_register_to_io(io, snd_reg);
	uni_printf(io, ", %" PRIitem "\n", imm);
}

// Вид инструкции:	instr	fst_reg, imm(snd_reg)
static void to_code_R_I_R(universal_io *const io, const riscv_instruction_t instruction, const riscv_register_t fst_reg,
						  const item_t imm, const riscv_register_t snd_reg)
{
	// printf("here\n");
	to_code_begin(io, instruction, fst_reg);
	uni_printf(io, "%" PRIitem "(", imm);
	riscv_register_to_io(io, snd_reg);
	uni_printf(io, ")\n");
}

// Вид инструкции:	instr	reg, imm
static void to_code_R_I(universal_io *const io, const riscv_instruction_t instruction, const riscv_register_t reg,
						const double imm)
{
	printf("!%f !\n", imm);
	// тут манипуляции как минимум с переменными
	// printf("here\n");
	to_code_begin(io, instruction, reg);
	// printf("%" PRId64, imm);
	// uni_printf(io, "%" PRIitem "\n", imm);
	uni_printf(io,
			   "%f"
			   "\n",
			   imm);
}

/**
 *	Writes @c val field of rvalue structure to io
 *
 *	@param	io					Universal io structure
 *	@param	rval				Rvalue whose value is to be printed
 */
static void rvalue_const_to_io(universal_io *const io, const rvalue *const rval, syntax *const sx)
{
	switch (rval->type)
	{
		case TYPE_BOOLEAN:
		case TYPE_CHARACTER:
		case TYPE_INTEGER:
			uni_printf(io, "%" PRIitem, rval->val.int_val);
			current_memory_location = rval->val.int_val;
			break;
		//case TYPE_ARRAY:
			//current_memory_location = rval->val.int_val - 4 * prev_declaration_size + 4;
			break;
		case TYPE_FLOATING:
			uni_printf(io, "%f", rval->val.float_val);
			break;
		case 10:
			uni_printf(io, "%s%i", "STRING", rval->val.str_index);
			//uni_printf(io, "%i", strlen(string_get(sx, rval->val.str_index)));
			uni_printf(io, "\n\tli a2, %i", strlen(string_get(sx, rval->val.str_index)));
		default:
			system_error(node_unexpected);
			break;
	}
}

/**
 *	Writes rvalue to io
 *
 *	@param	enc					Encoder
 *	@param	rval				Rvalue to write
 */
static void rvalue_to_io(encoder *const enc, const rvalue *const rval)
{
	assert(rval->kind != RVALUE_KIND_VOID);

	if (rval->kind == RVALUE_KIND_CONST)
	{
		rvalue_const_to_io(enc->sx->io, rval, enc->sx);
	}
	else
	{
		riscv_register_to_io(enc->sx->io, rval->val.reg_num);
	}
}

/**
 *	Writes lvalue to io
 *
 *	@param	enc					Encoder
 *	@param	lvalue				Lvalue
 */
item_t displ_counter = 0;
item_t true_loc[10000];
static void lvalue_to_io(encoder *const enc, lvalue * value)
{
	if (value->kind == LVALUE_KIND_REGISTER)
	{
		riscv_register_to_io(enc->sx->io, value->loc.reg_num);
	}
	else
	{
		if (is_declaration)
		{
			//printf("%i\n", value->loc.displ);
			displ_counter -= 4;
			//true_loc[-value->base_reg] = displ_counter;
			true_loc[-value->loc.displ] = displ_counter;
			//printf("%i %i\n", value->loc.displ, true_loc[-value->loc.displ]);
		}
		uni_printf(enc->sx->io, "%" PRIitem "(", true_loc[-value->loc.displ]);
		riscv_register_to_io(enc->sx->io, value->base_reg);
		uni_printf(enc->sx->io, ")\n");
	}
}

/**
 *	Add new identifier to displacements table
 *
 *	@param	enc					Encoder
 *	@param	identifier			Identifier for adding to the table
 *
 *	@return	Identifier lvalue
 */
static lvalue displacements_add(encoder *const enc, const size_t identifier, const bool is_register)
{
	// TODO: выдача сохраняемых регистров
	assert(is_register == false);
	const bool is_local = ident_is_local(enc->sx, identifier);
	const riscv_register_t base_reg = is_local ? R_FP : R_GP;
	const item_t type = ident_get_type(enc->sx, identifier);
	if (is_local && !is_register)
	{
		enc->scope_displ += riscv_type_size(enc->sx, type);
		enc->max_displ = max(enc->scope_displ, enc->max_displ);
	}
	const item_t location = is_local ? -(item_t)enc->scope_displ : (item_t)enc->global_displ;

	if ((!is_local) && (is_register)) // Запрет на глобальные регистровые переменные
	{
		// TODO: кидать соответствующую ошибку
		system_error(node_unexpected);
	}

	const size_t index = hash_add(&enc->displacements, identifier, 3);
	hash_set_by_index(&enc->displacements, index, 0, (is_register) ? 1 : 0);
	hash_set_by_index(&enc->displacements, index, 1, location);
	hash_set_by_index(&enc->displacements, index, 2, base_reg);

	if (!is_local)
	{
		enc->global_displ += riscv_type_size(enc->sx, type);
	}

	return (lvalue){ .kind = is_register ? LVALUE_KIND_REGISTER : LVALUE_KIND_STACK,
					 .base_reg = base_reg,
					 .loc.displ = location,
					 .type = type };
}

/**
 *	Add identifier to displacements table with known location
 *
 *	@param	enc					Encoder
 *	@param	identifier			Identifier for adding to the table
 *	@param	value				Lvalue for adding to the table
 */
static void displacements_set(encoder *const enc, const size_t identifier, const lvalue *const value)
{
	const size_t index = hash_add(&enc->displacements, identifier, 3);
	hash_set_by_index(&enc->displacements, index, 0, (value->kind == LVALUE_KIND_REGISTER) ? 1 : 0);
	hash_set_by_index(&enc->displacements, index, 1, value->loc.displ);
	hash_set_by_index(&enc->displacements, index, 2, value->base_reg);
}

/**
 *	Return lvalue for the given identifier
 *
 *	@param	enc					Encoder
 *	@param	identifier			Identifier in the table
 *
 *	@return	Identifier lvalue
 */
static lvalue displacements_get(encoder *const enc, const size_t identifier)
{
	const bool is_register = (hash_get(&enc->displacements, identifier, 0) == 1);
	const size_t displacement = (size_t)hash_get(&enc->displacements, identifier, 1);
	const riscv_register_t base_reg = hash_get(&enc->displacements, identifier, 2);
	const item_t type = ident_get_type(enc->sx, identifier);

	const lvalue_kind_t kind = (is_register) ? LVALUE_KIND_REGISTER : LVALUE_KIND_STACK;

	return (lvalue){ .kind = kind, .base_reg = base_reg, .loc.displ = displacement, .type = type };
}

/**
 *	Emit label
 *
 *	@param	enc					Encoder
 *	@param	label				Label for emitting
 */
static void emit_label(encoder *const enc, const label *const lbl)
{
	//printf("%i\n", lbl->kind);
	universal_io *const io = enc->sx->io;
	switch (lbl->kind)
	{
		case L_MAIN:
			uni_printf(io, "main");
			break;
		case L_FUNC:
			uni_printf(io, "FUNC");
			break;
		case L_NEXT:
			uni_printf(io, "NEXT");
			break;
		case L_FUNCEND:
			uni_printf(io, "FUNCEND");
			break;
		case L_STRING:
			uni_printf(io, "STRING");
			break;
		case L_THEN:
			uni_printf(io, "THEN");
			break;
		case L_ELSE:
			uni_printf(io, "ELSE");
			break;
		case L_END:
			uni_printf(io, "END");
			break;
		case L_BEGIN_CYCLE:
			uni_printf(io, "BEGIN_CYCLE");
			break;
		case L_CASE:
			uni_printf(io, "CASE");
			break;
		case L_CASE_CONDITION:
			uni_printf(io, "CASE_CONDITION");
			break;
		case L_DEFAULT:
			uni_printf(io, "DEFAULT");
			break;
	}
	uni_printf(io, "%zu", lbl->num);
}

/**
 *	Emit label declaration
 *
 *	@param	enc					Encoder
 *	@param	label				Declared label
 */
static void emit_label_declaration(encoder *const enc, const label *const lbl)
{
	emit_label(enc, lbl);
	uni_printf(enc->sx->io, ":\n");
}

/**
 *	Emit unconditional branch
 *
 *	@param	enc					Encoder
 *	@param	label				Label for unconditional jump
* 
 */
static void emit_unconditional_branch(encoder *const enc, const riscv_instruction_t instruction, const label *const lbl)
{
	assert(instruction == IC_RISCV_J || instruction == IC_RISCV_JAL);	
	uni_printf(enc->sx->io, "\t");
	instruction_to_io(enc->sx->io, instruction);
	uni_printf(enc->sx->io, " ");
	emit_label(enc, lbl);
	uni_printf(enc->sx->io, "\n");
	
}

/**
 *	Emit conditional branch
 *
 *	@param	enc					Encoder
 *	@param	label				Label for conditional jump
 */
static void emit_conditional_branch_old(encoder *const enc, const riscv_instruction_t instruction,
										const rvalue *const value, const label *const lbl)
{
	if (value->kind == RVALUE_KIND_CONST)
	{
		if (value->val.int_val == 0)
		{
			emit_unconditional_branch(enc, IC_RISCV_J, lbl);
		}
	}
	else
	{
		uni_printf(enc->sx->io, "\t");
		instruction_to_io(enc->sx->io, instruction);
		uni_printf(enc->sx->io, " ");
		rvalue_to_io(enc, value);
		uni_printf(enc->sx->io, ", ");
		riscv_register_to_io(enc->sx->io, R_ZERO);
		uni_printf(enc->sx->io, ", ");
		emit_label(enc, lbl);
		uni_printf(enc->sx->io, "\n");
	}
}

/**
 *	Emit conditional branch
 *
 *	@param	enc					Encoder
 *	@param	label				Label for conditional jump
 */
static void emit_conditional_branch(encoder *const enc, const riscv_instruction_t instruction,
									const rvalue *const first_operand, const rvalue *const second_operand,
									const label *const lbl)
{
	// TODO: Handle constant
	//if (lbl->num == 14757395258967641292)
		//uni_printf(enc->sx->io, "\tli t2, 0\n");
	//printf("%i\n", lbl->kind);
	uni_printf(enc->sx->io, "\t");
	instruction_to_io(enc->sx->io, instruction);
	uni_printf(enc->sx->io, " ");
	rvalue_to_io(enc, first_operand);
	uni_printf(enc->sx->io, ", ");
	//uni_printf(enc->sx->io, "t1");
	rvalue_to_io(enc, second_operand);
	uni_printf(enc->sx->io, ", ");
	//if (lbl->num != 14757395258967641292)
	if (lbl->kind != L_END)
		emit_label(enc, lbl);
	else
		uni_printf(enc->sx->io, "%s%zu", "END", (lbl->num - 1));

	//uni_printf(enc->sx->io, "%zu", lbl->num);
	//else
		//uni_printf(enc->sx->io, "TRUE_CONDITION");
	uni_printf(enc->sx->io, "\n");
}

/**
 *	Emit branching with register
 *
 *	@param	enc					Encoder
 *	@param	reg					Register
 */
static void emit_register_branch(encoder *const enc, const riscv_instruction_t instruction, const riscv_register_t reg)
{
	assert(instruction == IC_RISCV_JR);

	uni_printf(enc->sx->io, "\t");
	instruction_to_io(enc->sx->io, instruction);
	uni_printf(enc->sx->io, " ");
	riscv_register_to_io(enc->sx->io, reg);
	uni_printf(enc->sx->io, "\n");
}

char *doubleToHex(double d)
{
	unsigned long long *longLongPtr = (unsigned long long *)&d; // Получаем доступ к битовому представлению
	static char hexString[19]; // Достаточно места для 64-битного числа + нуль-терминатор

	sprintf(hexString, "0x%016llx", *longLongPtr); // Форматируем в шестнадцатеричный формат

	return hexString;
}

/*
 *	 ______     __  __     ______   ______     ______     ______     ______     __     ______     __   __     ______
 *	/\  ___\   /\_\_\_\   /\  == \ /\  == \   /\  ___\   /\  ___\   /\  ___\   /\ \   /\  __ \   /\ "-.\ \   /\  ___\
 *	\ \  __\   \/_/\_\/_  \ \  _-/ \ \  __<   \ \  __\   \ \___  \  \ \___  \  \ \ \  \ \ \/\ \  \ \ \-.  \  \ \___  \
 *	 \ \_____\   /\_\/\_\  \ \_\    \ \_\ \_\  \ \_____\  \/\_____\  \/\_____\  \ \_\  \ \_____\  \ \_\\"\_\  \/\_____\
 *	  \/_____/   \/_/\/_/   \/_/     \/_/ /_/   \/_____/   \/_____/   \/_____/   \/_/   \/_____/   \/_/ \/_/   \/_____/
 */

/**
 *	Creates register kind rvalue and stores there constant kind rvalue
 *
 *	@param	enc					Encoder
 *	@param	value				Rvalue of constant kind
 *
 *	@return	Created rvalue
 */
static rvalue emit_load_of_immediate(encoder *const enc, const rvalue *const value)
{
	assert(value->kind == RVALUE_KIND_CONST);
	const bool is_floating = type_is_floating(enc->sx, value->type);
	riscv_register_t reg = get_register(enc);
	const riscv_instruction_t instruction = IC_RISCV_LI;

	if (is_floating)
	{
		// const uint64_t real_val = *(uint64_t*)&value->val.float_val;
		// const uint32_t hex_val1 = real_val >> 32;
		// const uint32_t hex_val2 = (real_val << 32) >> 32;
		const riscv_register_t float_reg = get_float_register(enc);
		// li t0, val1
		//printf("| %f |\n", value->val.float_val);

		// to_code_R_I(enc->sx->io, IC_RISCV_LI, R_T0, hex_val1);
		const float f = (float)value->val.float_val;
		//to_code_R_I(enc->sx->io, IC_RISCV_LI, R_T0, value->val.float_val);
		//to_code_R_I(enc->sx->io, IC_RISCV_LI, R_T0, *(unsigned int *)&f);

		//float_MY
		//
		//uni_printf(enc->sx->io, "\tli t0, %s\n", doubleToHex(f));
		uni_printf(enc->sx->io, "\tli ", doubleToHex(f));
		riscv_register_to_io(enc->sx->io, reg);
		//uni_printf(enc->sx->io, "\tli ", doubleToHex(f));
		uni_printf(enc->sx->io, ", %s\n", doubleToHex(f));
		//uni_printf(enc->sx->io, "\tli t0, 0x%x\n", *(unsigned int *)&f);
		uni_printf(enc->sx->io, "\tfmv.d.x ");
		riscv_register_to_io(enc->sx->io, float_reg);
		uni_printf(enc->sx->io, ", ");
		riscv_register_to_io(enc->sx->io, reg);
		uni_printf(enc->sx->io, " \n");


		// sw t0, -4(fp)
		//to_code_R_I_R(enc->sx->io, IC_RISCV_SW, R_FT0, -(item_t)WORD_LENGTH, R_FP);
		// li t0, val2
		//to_code_R_I(enc->sx->io, IC_RISCV_LI, R_T0, value->val.float_val);
		// to_code_R_I(enc->sx->io, IC_RISCV_LI, R_T0, hex_val2);
		//  sw t0, -8(fp)
		//to_code_R_I_R(enc->sx->io, IC_RISCV_SW, R_T0, -(item_t)(2 * WORD_LENGTH), R_FP);
		// fld fi, -8(fp)
		//to_code_R_I_R(enc->sx->io, IC_RISCV_FLD, float_reg, -(item_t)(2 * WORD_LENGTH), R_FP);
		reg = float_reg;
	}
	else
	{
		uni_printf(enc->sx->io, "\t");
		if (value->type == 10)
			instruction_to_io(enc->sx->io, IC_RISCV_LA);
		else
			instruction_to_io(enc->sx->io, instruction);
		uni_printf(enc->sx->io, " ");
		if (value->type == 10)
			riscv_register_to_io(enc->sx->io, R_A1);
		else
			riscv_register_to_io(enc->sx->io, reg);
		uni_printf(enc->sx->io, ", ");
		rvalue_to_io(enc, value);
		uni_printf(enc->sx->io, "\n");
	}

	return (
		rvalue){ .from_lvalue = !FROM_LVALUE, .kind = RVALUE_KIND_REGISTER, .val.reg_num = reg, .type = value->type };
}


/**
 *	Loads lvalue to register and forms rvalue. If lvalue kind is @c LVALUE_KIND_REGISTER,
 *	returns rvalue on the same register
 *
 *	@param	enc					Encoder
 *	@param	lval				Lvalue to load
 *
 *	@return	Formed rvalue
 */
static rvalue emit_load_of_lvalue(encoder *const enc, const lvalue *const lval)
{
	if (lval->kind == LVALUE_KIND_REGISTER)
	{
		return (rvalue){ .kind = RVALUE_KIND_REGISTER,
						 .val.reg_num = lval->loc.reg_num,
						 .from_lvalue = FROM_LVALUE,
						 .type = lval->type };
	}

	if (type_is_structure(enc->sx, lval->type) || type_is_array(enc->sx, lval->type))
	{
		//uni_printf(enc->sx->io, "\t#33333\n");
		// Грузим адрес первого элемента на регистр
		//enc->registers
		//enc->registers[0] = NULL;
		const rvalue tmp = { .kind = RVALUE_KIND_CONST, .val.int_val = lval->loc.displ, .type = TYPE_INTEGER };
		//uni_printf(enc->sx->io, "\n\t#");
		rvalue displ_rvalue = emit_load_of_immediate(enc, &tmp);
		//displ_rvalue.val.reg_num = 9;
		const rvalue base_reg_rvalue = { .kind = RVALUE_KIND_REGISTER,
										 .val.reg_num = lval->base_reg,	  
										 .type = TYPE_INTEGER };
		//uni_printf(enc->sx->io, "\t#");
		emit_binary_operation(enc, &displ_rvalue, &displ_rvalue, &base_reg_rvalue, BIN_ADD);
		//uni_printf(enc->sx->io, "\t#44444\n");
		
		return displ_rvalue;
	}

	const bool is_floating = type_is_floating(enc->sx, lval->type);
	const riscv_register_t reg = is_floating ? get_float_register(enc) : get_register(enc);
	const riscv_instruction_t instruction = is_floating ? IC_RISCV_FLD : IC_RISCV_LW;

	const rvalue result = {
		.kind = RVALUE_KIND_REGISTER,
		.val.reg_num = reg,
		.from_lvalue = !FROM_LVALUE,
		.type = lval->type,
	};

	
	uni_printf(enc->sx->io, "\t");
	instruction_to_io(enc->sx->io, instruction);
	uni_printf(enc->sx->io, " ");
	rvalue_to_io(enc, &result);
	uni_printf(enc->sx->io, ", %" PRIitem "(", true_loc[-lval->loc.displ]);
	riscv_register_to_io(enc->sx->io, lval->base_reg);
	uni_printf(enc->sx->io, ")\n");

	//// Для любых скалярных типов ничего не произойдёт,
	//// а для остальных освобождается base_reg, в котором хранилось смещение
	//free_register(enc, lval->base_reg);
	//uni_printf(enc->sx->io, "\t#88888888\n");
	return result;
}

/**
 *	Emit identifier lvalue
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Identifier lvalue
 */
static lvalue emit_identifier_lvalue(encoder *const enc, const node *const nd)
{
	//uni_printf(enc->sx->io, "\tEIV\n");
	return displacements_get(enc, expression_identifier_get_id(nd));
}

/**
 *	Emit subscript lvalue
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Subscript lvalue
 */
static lvalue emit_subscript_lvalue(encoder *const enc, const node *const nd)
{
	const item_t type = expression_get_type(nd);

	const node base = expression_subscript_get_base(nd);
	const rvalue base_value = emit_expression(enc, &base);

	const node index = expression_subscript_get_index(nd);
	const rvalue index_value = emit_expression(enc, &index);

	// base_value гарантированно имеет kind == RVALUE_KIND_REGISTER
	if (index_value.kind == RVALUE_KIND_CONST)
	{
		return (lvalue){ .kind = LVALUE_KIND_STACK,
						 .base_reg = base_value.val.reg_num,
						 .loc.displ = -(item_t)index_value.val.int_val * riscv_type_size(enc->sx, type),
						 .type = type };
	}

	const rvalue type_size_value = { // Можно было бы сделать отдельным конструктором
									 .from_lvalue = !FROM_LVALUE,
									 .kind = RVALUE_KIND_CONST,
									 .val.int_val = riscv_type_size(enc->sx, type),
									 .type = TYPE_INTEGER
	};
	const rvalue offset = { .from_lvalue = !FROM_LVALUE,
							.kind = RVALUE_KIND_REGISTER,
							.val.reg_num = get_register(enc),
							.type = TYPE_INTEGER };

	emit_binary_operation(enc, &offset, &index_value, &type_size_value, BIN_MUL);
	free_rvalue(enc, &index_value);

	emit_binary_operation(enc, &base_value, &base_value, &offset, BIN_SUB);
	free_rvalue(enc, &offset);

	return (lvalue){ .kind = LVALUE_KIND_STACK, .base_reg = base_value.val.reg_num, .loc.displ = 0, .type = type };
}

/**
 *	Emit member lvalue
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Created lvalue
 */
static lvalue emit_member_lvalue(encoder *const enc, const node *const nd)
{
	const node base = expression_member_get_base(nd);
	const item_t base_type = expression_get_type(&base);

	const bool is_arrow = expression_member_is_arrow(nd);
	const item_t struct_type = is_arrow ? type_pointer_get_element_type(enc->sx, base_type) : base_type;

	size_t member_displ = 0;
	const size_t member_index = expression_member_get_member_index(nd);
	for (size_t i = 0; i < member_index; i++)
	{
		const item_t member_type = type_structure_get_member_type(enc->sx, struct_type, i);
		member_displ += riscv_type_size(enc->sx, member_type);
	}

	const item_t type = expression_get_type(nd);

	if (is_arrow)
	{
		const rvalue struct_pointer = emit_expression(enc, &base);
		// FIXME: грузить константу на регистр в случае константных указателей
		return (lvalue){
			.kind = LVALUE_KIND_STACK, .base_reg = struct_pointer.val.reg_num, .loc.displ = member_displ, .type = type
		};
	}
	else
	{
		const lvalue base_lvalue = emit_lvalue(enc, &base);
		const size_t displ = (size_t)(base_lvalue.loc.displ + member_displ);
		return (
			lvalue){ .kind = LVALUE_KIND_STACK, .base_reg = base_lvalue.base_reg, .loc.displ = displ, .type = type };
	}
}

/**
 *	Emit indirection lvalue
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Indirected lvalue
 */
static lvalue emit_indirection_lvalue(encoder *const enc, const node *const nd)
{
	assert(expression_unary_get_operator(nd) == UN_INDIRECTION);

	const node operand = expression_unary_get_operand(nd);
	const rvalue base = emit_expression(enc, &operand);
	// FIXME: грузить константу на регистр в случае константных указателей
	const item_t type = expression_get_type(nd);

	return (lvalue){ .kind = LVALUE_KIND_STACK, .base_reg = base.val.reg_num, .loc.displ = 0, .type = type };
}

/**
 *	Emit lvalue expression
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Lvalue
 */
static lvalue emit_lvalue(encoder *const enc, const node *const nd)
{
	assert(expression_is_lvalue(nd));

	switch (expression_get_class(nd))
	{
		case EXPR_IDENTIFIER:
			//uni_printf(enc->sx->io, "\tID\n");
			return emit_identifier_lvalue(enc, nd);

		case EXPR_SUBSCRIPT:
			return emit_subscript_lvalue(enc, nd);

		case EXPR_MEMBER:
			return emit_member_lvalue(enc, nd);

		case EXPR_UNARY: // Только UN_INDIRECTION
			return emit_indirection_lvalue(enc, nd);

		default:
			// Не может быть lvalue
			system_error(node_unexpected, nd);
			return (lvalue){ .loc.displ = ITEM_MAX };
	}
}

/**
 *	Stores one rvalue of register kind to another
 *
 *	@param	enc					Encoder
 *	@param	target				Target register
 *	@param	value				Rvalue to store
 */
static void emit_move_rvalue_to_register(encoder *const enc, const riscv_register_t target, const rvalue *const value)
{
	if (value->kind == RVALUE_KIND_CONST)
	{
		// TODO: use li twice for move double
		const riscv_instruction_t instruction = IC_RISCV_LI;
		uni_printf(enc->sx->io, "\t");
		instruction_to_io(enc->sx->io, instruction);
		uni_printf(enc->sx->io, " ");
		riscv_register_to_io(enc->sx->io, target);
		uni_printf(enc->sx->io, ", ");
		rvalue_to_io(enc, value);
		uni_printf(enc->sx->io, "\n");
		return;
	}

	if (value->val.reg_num == target)
	{
		uni_printf(enc->sx->io, "\t# stays in register ");
		riscv_register_to_io(enc->sx->io, target);
		uni_printf(enc->sx->io, ":\n");
	}
	else
	{
		const riscv_instruction_t instruction = IC_RISCV_MOVE;
		uni_printf(enc->sx->io, "\t");
		instruction_to_io(enc->sx->io, instruction);
		uni_printf(enc->sx->io, " ");
		riscv_register_to_io(enc->sx->io, target);
		uni_printf(enc->sx->io, ", ");
		rvalue_to_io(enc, value);
		uni_printf(enc->sx->io, "\n");
	}
}

/**
 *	Stores rvalue to lvalue. Frees value parameter register for a pointer
 *
 *	@param	enc					Encoder
 *	@param	target				Target lvalue
 *	@param	value				Rvalue to store
 */
static void emit_store_of_rvalue(encoder *const enc, lvalue *target, const rvalue *const value)
{
	assert(value->kind != RVALUE_KIND_VOID);
	// assert(value->type == target->type);
	const rvalue reg_value = (value->kind == RVALUE_KIND_CONST) ? emit_load_of_immediate(enc, value) : *value;
	if (target->kind == LVALUE_KIND_REGISTER)
	{
		if (value->val.reg_num != target->loc.reg_num)
		{
			const riscv_instruction_t instruction =
				type_is_floating(enc->sx, value->type) ? IC_RISCV_FMOV : IC_RISCV_MOVE;
			uni_printf(enc->sx->io, "\t");
			instruction_to_io(enc->sx->io, instruction);
			uni_printf(enc->sx->io, " ");
			lvalue_to_io(enc, target);
			uni_printf(enc->sx->io, ", ");
			rvalue_to_io(enc, &reg_value);
			uni_printf(enc->sx->io, "\n");
		}
	}
	else
	{
		if ((!type_is_structure(enc->sx, target->type)) && (!type_is_array(enc->sx, target->type)))
		{
			//printf("&&&&%i&&&&\n", target->loc.displ);
			const riscv_instruction_t instruction = type_is_floating(enc->sx, value->type) ? IC_RISCV_FSD : IC_RISCV_SW;
			uni_printf(enc->sx->io, "\t");
			instruction_to_io(enc->sx->io, instruction);
			uni_printf(enc->sx->io, " ");
			rvalue_to_io(enc, &reg_value);
			uni_printf(enc->sx->io, ", ");
			lvalue_to_io(enc, target);	
			//
			//printf("&%i&", target->loc.displ);
			//printf("qwqwqw");
			uni_printf(enc->sx->io, "\n");

			// Освобождаем регистр только в том случае, если он был занят на этом уровне. Выше не лезем.
			if (value->kind == RVALUE_KIND_CONST)
			{
				free_rvalue(enc, &reg_value);
			}

			free_register(enc, target->base_reg);
		}
		else
		{
			if (type_is_array(enc->sx, target->type))
			{
				// Загружаем указатель на массив
				uni_printf(enc->sx->io, "\t");
				instruction_to_io(enc->sx->io, IC_RISCV_SW);
				uni_printf(enc->sx->io, " ");
				rvalue_to_io(enc, &reg_value);
				//printf("%llu\n", true_loc[-target->loc.displ]);
				//if (is_declaration)
				//{
				//	printf("%i", target->loc.displ);
				//	displ_counter -= 4;
				//	true_loc[-target->loc.displ] = displ_counter;
				//	target->loc.displ = true_loc[-target->loc.displ];
				//}
				uni_printf(enc->sx->io, ", %" PRIitem "(", target->loc.displ);
				riscv_register_to_io(enc->sx->io, target->base_reg);
				uni_printf(enc->sx->io, ")\n\n");
				return;
			}
			// else кусок должен быть не достижим
		}
	}
}

/**
 *	Gets original operands and loads them depending on the needs of operation
 *
 *	@param	enc					Encoder
 *	@param	operand 			Original operand rvalue
 *	@param  operation			Operation in which this operand used
 *	@param  is_first_operand    Bool which defines if the operand is first in binary expression
 *
 *	@return new operand rvalue
 */
static rvalue emit_operand_load(encoder *const enc, const rvalue *const operand, const binary_t operation,
								const bool is_first_operand)
{
	switch (operation)
	{
		case BIN_LT:
		case BIN_GT:
		case BIN_LE:
		case BIN_GE:
		case BIN_EQ:
		case BIN_NE:
		case BIN_SUB:
		case BIN_ADD:
		case BIN_DIV:
		case BIN_MUL:
		case BIN_REM:
			// Сравнение => Используется BIN_SUB, поэтому грузим на регистр
			// sub, div, mul, rem => Нет команд работающих с константными значениями, поэтому грузим его на регистр
			return (operand->kind == RVALUE_KIND_CONST) ? emit_load_of_immediate(enc, operand) : *operand;
		case BIN_SHL:
		case BIN_SHR:
			// Нет команд работающих с первым операндом в виде константы и операции не коммутативны, поэтому грузим его
			// на регистр
			return !is_first_operand || operand->kind != RVALUE_KIND_CONST ? *operand
																		   : emit_load_of_immediate(enc, operand);
		case BIN_AND:
		case BIN_OR:
		case BIN_XOR:
			// Операции коммутативны и есть операция работающая с одним значением на регистре и одним в виде константы
			// Логика замены операндов вне функции
			return *operand;
		default:
			// Не может быть других операций
			system_error(node_unexpected);
			return RVALUE_VOID;
	}
}

static void emit_bin_registers_cond_branching(encoder *const enc, const rvalue *const dest, const rvalue *first_operand,
											  const rvalue *second_operand, binary_t operator)
{
	const bool is_floating = type_is_floating(enc->sx, first_operand->type);
	if (operator== BIN_GT ||(is_floating &&operator== BIN_GE) || (!is_floating && BIN_LE))
	{
		const rvalue *tmp = first_operand;
		first_operand = second_operand;
		second_operand = tmp;
	}
	const riscv_instruction_t instruction = get_bin_instruction(is_floating ? BIN_NE : operator, false, false);
	if (!is_floating)
	{
		//printf("%i\n", enc->label_if_true.kind);
		emit_conditional_branch(enc, instruction, first_operand, second_operand, &enc->label_if_true);
		//uni_printf(enc->sx->io, "AAAAAAAAA\n");
	}
	else
	{
		to_code_3R(enc->sx->io, get_bin_instruction(operator, false, true), dest->val.reg_num,
				   first_operand->val.reg_num, second_operand->val.reg_num);
		emit_conditional_branch_old(enc, instruction, dest, &enc->label_if_true);
	}
	emit_unconditional_branch(enc, IC_RISCV_J, &enc->label_if_false);
}

/**
 *	Emit binary operation with two rvalues
 *
 *	@param	enc					Encoder
 *	@param	dest				Destination rvalue
 *	@param	first_operand		First rvalue operand
 *	@param	second_operand		Second rvalue operand
 *	@param	operator			Operator
 */
int labelNumGlobal = 0;
static void emit_binary_operation(encoder *const enc, const rvalue *const dest, const rvalue *const first_operand,
								  const rvalue *const second_operand, const binary_t operator)
{
	//printf("%llu\n",	val.reg_num);
	assert(operator!= BIN_LOG_AND);
	assert(operator!= BIN_LOG_OR);

	assert(dest->kind == RVALUE_KIND_REGISTER);
	assert(first_operand->kind != RVALUE_KIND_VOID);
	assert(second_operand->kind != RVALUE_KIND_VOID);

	const bool is_floating = type_is_floating(enc->sx, dest->type);

	if ((first_operand->kind == RVALUE_KIND_REGISTER) && (second_operand->kind == RVALUE_KIND_REGISTER))
	{

		switch (operator)
		{
			case BIN_LT:
			case BIN_GT:
			case BIN_LE:
			case BIN_GE:
			case BIN_EQ:
			case BIN_NE:
				const item_t curr_label_num = enc->label_num++;
				labelNumGlobal = (int)enc->label_num++;
				const label label_else = { .kind = L_END, .num = (size_t)curr_label_num };
				//printf("%zu", enc->label_num);
				uni_printf(enc->sx->io, "\t");
				instruction_to_io(enc->sx->io, IC_RISCV_LI);
				uni_printf(enc->sx->io, " t1, 1\n");

				uni_printf(enc->sx->io, "\t");
				uni_printf(enc->sx->io, "\n");

				const riscv_instruction_t instruction = get_bin_instruction(operator, false, false);
				emit_conditional_branch(enc, instruction, first_operand, second_operand, &label_else);
				//uni_printf(enc->sx->io, "\t");
				//instruction_to_io(enc->sx->io, IC_RISCV_LI);
				//uni_printf(enc->sx->io, " t1, 1\n");
				// rvalue_to_io(enc, dest);
				emit_label_declaration(enc, &label_else);

				uni_printf(enc->sx->io, "\n");
				//emit_bin_registers_cond_branching(enc, dest, first_operand, second_operand, operator);
				break;
			default:
			{
				uni_printf(enc->sx->io, "\t");
				instruction_to_io(enc->sx->io, get_bin_instruction(operator, false, is_floating
																   /* Два регистра => 0 в get_bin_instruction() -> */));
				uni_printf(enc->sx->io, " ");
				rvalue_to_io(enc, dest);
				uni_printf(enc->sx->io, ", ");
				rvalue_to_io(enc, first_operand);
				uni_printf(enc->sx->io, ", ");
				rvalue_to_io(enc, second_operand);
				uni_printf(enc->sx->io, "\n");
			}
			break;
		}
	}
	else
	{

		const rvalue real_first_operand = emit_operand_load(enc, first_operand, operator, true);
		const rvalue real_second_operand = emit_operand_load(enc, second_operand, operator, false);

		switch (operator)
		{
			case BIN_LT:
			case BIN_GT:
			case BIN_LE:
			case BIN_GE:
			case BIN_EQ:
			case BIN_NE:
				//printf("%zu", enc->label_num);
				const item_t curr_label_num = enc->label_num++;
				const label label_else = { .kind = L_ELSE, .num = (size_t)curr_label_num };
				// Записываем <значение из first_operand> - <значение из second_operand> в dest
				uni_printf(enc->sx->io, "\t");
				//instruction_to_io(enc->sx->io, IC_RISCV_SUB);
				//uni_printf(enc->sx->io, " ");
				//rvalue_to_io(enc, dest);
				//uni_printf(enc->sx->io, ", ");
				//rvalue_to_io(enc, &real_first_operand);
				//uni_printf(enc->sx->io, ", ");
				//rvalue_to_io(enc, &real_second_operand);
				uni_printf(enc->sx->io, "\n");
				const riscv_instruction_t instruction = get_bin_instruction(operator, false, false);
				emit_conditional_branch(enc, instruction, &real_first_operand, &real_second_operand, &label_else);

				uni_printf(enc->sx->io, "\t");
				instruction_to_io(enc->sx->io, IC_RISCV_LI);
				uni_printf(enc->sx->io, " ");
				rvalue_to_io(enc, dest);
				uni_printf(enc->sx->io, ", 1\n");

				emit_label_declaration(enc, &label_else);
				uni_printf(enc->sx->io, "\tli t1, 0\n");
				int temp = (int)(enc->label_num);
				if (-temp + labelNumGlobal + 3 != 0)
					uni_printf(enc->sx->io, "\tj END%i\n", (-temp + labelNumGlobal + 3));
				uni_printf(enc->sx->io, "\n");
				//emit_bin_registers_cond_branching(enc, dest, &real_first_operand, &real_second_operand, operator);

				break;
			default:
			{

				bool does_need_instruction_working_with_both_operands_in_registers =
					// Нет команд работающих с операндами в виде константы
					(operator== BIN_SUB) || (operator== BIN_DIV) || (operator== BIN_MUL) ||
					(operator== BIN_REM)
					// Нет команд работающих с первым операндом в виде константы и операция не коммутативна
					|| ((operator== BIN_SHL || operator== BIN_SHR) && first_operand->kind == RVALUE_KIND_CONST);

				// Все остальные операции коммутативны, поэтому используем инструкцию с константой, меняя порядок если
				// надо
				bool change_order =
					(operator== BIN_ADD || operator== BIN_OR || operator== BIN_XOR || operator== BIN_AND) &&
					first_operand->kind == RVALUE_KIND_CONST;

				// Выписываем операцию, её результат будет записан в result
				uni_printf(enc->sx->io, "\t");
				instruction_to_io(
					enc->sx->io,
					get_bin_instruction(operator,
										/* Один регистр => true в get_bin_instruction() -> */ false,
										is_floating)
				);
				uni_printf(enc->sx->io, " ");

				rvalue_to_io(enc, dest);
				uni_printf(enc->sx->io, ", ");
				if (change_order)
				{
					rvalue_to_io(enc, &real_second_operand);
					uni_printf(enc->sx->io, ", ");
					rvalue_to_io(enc, &real_first_operand);
				}
				else
				{
					rvalue_to_io(enc, &real_first_operand);
					uni_printf(enc->sx->io, ", ");
					rvalue_to_io(enc, &real_second_operand);
				}

				uni_printf(enc->sx->io, "\n");
			}
		}
	}
}

/**
 *	Emit literal expression
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Rvalue of literal expression
 */
static rvalue emit_literal_expression(encoder *const enc, const node *const nd)
{
	const item_t type = expression_get_type(nd);

	switch (type_get_class(enc->sx, type))
	{
		case TYPE_BOOLEAN:
			return (rvalue){ .kind = RVALUE_KIND_CONST,
							 .val.int_val = (expression_literal_get_boolean(nd)) ? 1 : 0,
							 .type = TYPE_INTEGER };

		case TYPE_CHARACTER:
			return (rvalue){ .kind = RVALUE_KIND_CONST,
							 .val.int_val = expression_literal_get_character(nd),
							 .type = TYPE_INTEGER };

		case TYPE_INTEGER:

			//uni_printf(enc->sx->io, "\t#66666666\n");
			return (rvalue){ .kind = RVALUE_KIND_CONST,
							 .val.int_val = expression_literal_get_integer(nd),
							 .type = TYPE_INTEGER };

		case TYPE_FLOATING:
			return (rvalue){ .kind = RVALUE_KIND_CONST,
							 .val.float_val = expression_literal_get_floating(nd),
							 .type = TYPE_FLOATING };

		case TYPE_ARRAY:
			assert(type_is_string(enc->sx, type));
			return (
				rvalue){ .kind = RVALUE_KIND_CONST, .val.str_index = expression_literal_get_string(nd), .type = type };

		default:
			return RVALUE_VOID;
	}
}

/**
 *	Emit printf expression
 *
 *	@param	enc					Encoder
 *	@param	nd					AST node
 *	@param	parameters_amount	Number of function parameters
 */
static rvalue emit_printf_expression(encoder *const enc, const node *const nd)
{

	//printf("\n%i\n", nd->index);
	//uni_printf(enc->sx->io, "\n\t------------printf expr------------------\n");
	const node string = expression_call_get_argument(nd, 0);
	const size_t index = expression_literal_get_string(&string);
	const size_t amount = strings_amount(enc->sx);
	const size_t parameters_amount = expression_call_get_arguments_amount(nd);

	//const lvalue variable = displacements_add(enc, 175, false);
	//const lvalue target = {
	//	.kind = variable.kind, .type = TYPE_INTEGER, .loc = variable.loc, .base_reg = variable.base_reg
	//};
	//printf("%i", target.loc.displ);
	
	//const size_t amount_smth = declaration_variable_get_bounds(nd);
	//printf("%lli", variable.loc.displ);
	
	int k = 0;
	int start = 0;
	for (size_t i = 1; i < parameters_amount; i++)
	{
		do
		{
			const node arg2 = expression_call_get_argument(nd, i);
			const lvalue lval2 = emit_lvalue(enc, &arg2);
			//start = lval2.loc.displ;
			start = link_on_true_location[-lval2.loc.displ];
			//printf("%i", start);
			//const int size1 = array_sizes[-lval1.loc.displ];
			k += 1;
			const node arg = expression_call_get_argument(nd, i);
			//uni_printf(enc->sx->io, "\n!!!!!!\n");
			const rvalue val = emit_expression(enc, &arg);
			//uni_printf(enc->sx->io, "\n!!!!!!\n");
			//if (start == 0)
				//start = current_memory_location;
			//printf("%i", array_sizes[-start]);
			//printf("%i\n", parameters_amount);
			//uni_printf(enc->sx->io, "%" PRIitem, val.val.int_val);
			//uni_printf(enc->sx->io, "\t!!!!!!!!!!!!\n");

			if (array_sizes[-start] != 0)
			{
				//uni_printf(enc->sx->io, "#\n");
				uni_printf(enc->sx->io, "\tli t0, %i\n",  - 4 * (k - 1) + start);
				uni_printf(enc->sx->io, "\tadd t0, t0, fp\n");
			}
			// for (int i = val.val.int_val; i <= val.val.int_val * array_sizes[val.val.int_val]; i += 4)
			const rvalue arg_rvalue = (val.kind == RVALUE_KIND_CONST) ? emit_load_of_immediate(enc, &val) : val;
			const item_t arg_rvalue_type = arg_rvalue.type;

			uni_printf(enc->sx->io, "\n");

			const lvalue a0_lval = { .base_reg = R_SP,
									 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
									 // второй на 2*WORD_LENGTH и т.д.
									 .loc.displ = 0,
									 .kind = LVALUE_KIND_STACK,
									 .type = arg_rvalue.type };
			const rvalue a0_rval = {
				.kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A0, .type = TYPE_INTEGER, .from_lvalue = !FROM_LVALUE
			};
			// emit_store_of_rvalue(enc, &a0_lval, &a0_rval);

			const lvalue a1_lval = { .base_reg = R_SP,
									 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
									 // второй на 2*WORD_LENGTH и т.д.
									 .loc.displ = WORD_LENGTH,
									 .kind = LVALUE_KIND_STACK,
									 .type = arg_rvalue.type };
			const rvalue a1_rval = {
				.kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A1, .type = TYPE_INTEGER, .from_lvalue = !FROM_LVALUE
			};
			// emit_store_of_rvalue(enc, &a1_lval, &a1_rval);

			if (!type_is_floating(enc->sx, arg_rvalue.type))
			{
				//printf("!");
				uni_printf(enc->sx->io, "\n");
				emit_move_rvalue_to_register(enc, R_A1, &arg_rvalue);

				uni_printf(enc->sx->io, "\tlui t1, %%hi(STRING%zu)\n", index + (i - 1) * amount);
				uni_printf(enc->sx->io, "\taddi a0, t1, %%lo(STRING%zu)\n", index + (i - 1) * amount);

				uni_printf(enc->sx->io, "\tjal printf\n");
				uni_printf(enc->sx->io, "\t");
				instruction_to_io(enc->sx->io, IC_RISCV_NOP);
				uni_printf(enc->sx->io, "\n");

				free_rvalue(enc, &arg_rvalue);

				uni_printf(enc->sx->io, "\n\t# data restoring:\n");
			}
			else
			{
				uni_printf(enc->sx->io, "\tfsd f0, (sp)\n");
				uni_printf(enc->sx->io, "\tlw a0, (sp)\n");
				uni_printf(enc->sx->io, "\tflw fa2, (sp)\n");
				const lvalue a2_lval = { .base_reg = R_SP,
										 // по call convention: первый на WORD_LENGTH выше предыдущего положения
										 // fp, второй на 2*WORD_LENGTH и т.д.
										 .loc.displ = 2 * WORD_LENGTH,
										 .type = TYPE_INTEGER,
										 .kind = LVALUE_KIND_STACK };
				const rvalue a2_rval = {
					.kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A2, .type = TYPE_INTEGER, .from_lvalue = !FROM_LVALUE
				};

				// emit_store_of_rvalue(enc, &a2_lval, &a2_rval);
				uni_printf(enc->sx->io, "\n");

				uni_printf(enc->sx->io, "\tfcvt.d.s fa2,fa2\n");
				uni_printf(enc->sx->io, "\tfmv.x.d a1,fa2\n");
				uni_printf(enc->sx->io, "\tlui a5, %%hi(STRING%zu)\n", index + (i - 1) * amount);
				uni_printf(enc->sx->io, "\taddi a0, a5, %%lo(STRING%zu)\n", index + (i - 1) * amount);
				uni_printf(enc->sx->io, "\tfmv.x.d a1, ft0\n");
				uni_printf(enc->sx->io, "\tcall printf\n\t", index + (i - 1) * amount);
				instruction_to_io(enc->sx->io, IC_RISCV_NOP);
				uni_printf(enc->sx->io, "\n");

				// Восстановление регистров-аргументов -- они могут понадобится в дальнейшем
				uni_printf(enc->sx->io, "\n\t# data restoring:\n");

				const rvalue a2_rval_to_copy = emit_load_of_lvalue(enc, &a2_lval);
				emit_move_rvalue_to_register(enc, R_A2, &a2_rval_to_copy);

				free_rvalue(enc, &a2_rval);
				free_rvalue(enc, &arg_rvalue);
				uni_printf(enc->sx->io, "\n");
			}
			//printf("%i", array_sizes[-start]);
		} while (k < array_declaration_sizes[-start]);
		//const rvalue a0_rval_to_copy = emit_load_of_lvalue(enc, &a0_lval);
		//emit_move_rvalue_to_register(enc, R_A0, &a0_rval_to_copy);

		//free_rvalue(enc, &a0_rval_to_copy);
		//uni_printf(enc->sx->io, "\n");

		//const rvalue a1_rval_to_copy = emit_load_of_lvalue(enc, &a1_lval);
		//emit_move_rvalue_to_register(enc, R_A1, &a1_rval_to_copy);

		//free_rvalue(enc, &a1_rval_to_copy);
		//uni_printf(enc->sx->io, "\n");

		//to_code_2R_I(enc->sx->io, IC_RISCV_ADDI, R_SP, R_SP,
		//			 (item_t)WORD_LENGTH *
		//				 (!type_is_floating(enc->sx, arg_rvalue_type) ? /* a0 и a1 */ 1 : /* a0, a1 и a2 */ 2));
		//uni_printf(enc->sx->io, "\n");
	}

	const lvalue a0_lval = { .base_reg = R_SP,
							 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
							 // второй на 2*WORD_LENGTH и т.д.
							 .loc.displ = 0,
							 .kind = LVALUE_KIND_STACK,
							 .type = TYPE_INTEGER };
	const rvalue a0_rval = {
		.from_lvalue = !FROM_LVALUE, .kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A0, .type = TYPE_INTEGER
	};

	emit_store_of_rvalue(enc, &a0_lval, &a0_rval);

	uni_printf(enc->sx->io, "\tlui a5, %%hi(STRING%zu)\n", index + (parameters_amount - 1) * amount);
	uni_printf(enc->sx->io, "\taddi a0, a5, %%lo(STRING%zu)\n", index + (parameters_amount - 1) * amount);
	uni_printf(enc->sx->io, "\tcall printf\n");
	uni_printf(enc->sx->io, "\t");
	instruction_to_io(enc->sx->io, IC_RISCV_NOP);
	uni_printf(enc->sx->io, "\n");

	uni_printf(enc->sx->io, "\n\t# data restoring:\n");
	const rvalue a0_rval_to_copy = emit_load_of_lvalue(enc, &a0_lval);
	emit_move_rvalue_to_register(enc, R_A0, &a0_rval_to_copy);

	free_rvalue(enc, &a0_rval_to_copy);
	// FIXME: Возвращает число распечатанных символов (включая '\0'?)
	return RVALUE_VOID;
}


static rvalue emit_printid_expression(encoder *const enc, const node *const nd)
{

	const size_t amount = strings_amount(enc->sx);
	const size_t parameters_amount = expression_call_get_arguments_amount(nd);
	int k = 0;
	int start = 0;
	for (size_t i = 0; i < parameters_amount; i++)
	{
		do
		{
			const node arg = expression_call_get_argument(nd, i);
			const size_t argv = expression_identifier_get_id(&arg);

			k += 1;

			// uni_printf(enc->sx->io, "\t!!!!!!!!!!!!\n");
			// uni_printf(enc->sx->io, "#");
			//const node arg = expression_call_get_argument(nd, i);
			const rvalue val = emit_expression(enc, &arg);

			if (start == 0)
				start = current_memory_location;
			//printf("%i", current_memory_location);
			// printf("%i\n", parameters_amount);
			// uni_printf(enc->sx->io, "%" PRIitem, val.val.int_val);
			// uni_printf(enc->sx->io, "\t!!!!!!!!!!!!\n");

			if (array_sizes[-start] != 0)
			{
				uni_printf(enc->sx->io, "\tli t0, %llu\n", argv);
				uni_printf(enc->sx->io, "\tadd t0, t0, fp\n");
			}
			// for (int i = val.val.int_val; i <= val.val.int_val * array_sizes[val.val.int_val]; i += 4)
			const rvalue arg_rvalue = (val.kind == RVALUE_KIND_CONST) ? emit_load_of_immediate(enc, &val) : val;
			const item_t arg_rvalue_type = arg_rvalue.type;

			uni_printf(enc->sx->io, "\n");

			const lvalue a0_lval = { .base_reg = R_SP,
									 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
									 // второй на 2*WORD_LENGTH и т.д.
									 .loc.displ = 0,
									 .kind = LVALUE_KIND_STACK,
									 .type = arg_rvalue.type };
			const rvalue a0_rval = {
				.kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A0, .type = TYPE_INTEGER, .from_lvalue = !FROM_LVALUE
			};
			// emit_store_of_rvalue(enc, &a0_lval, &a0_rval);

			const lvalue a1_lval = { .base_reg = R_SP,
									 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
									 // второй на 2*WORD_LENGTH и т.д.
									 .loc.displ = WORD_LENGTH,
									 .kind = LVALUE_KIND_STACK,
									 .type = arg_rvalue.type };
			const rvalue a1_rval = {
				.kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A1, .type = TYPE_INTEGER, .from_lvalue = !FROM_LVALUE
			};
			// emit_store_of_rvalue(enc, &a1_lval, &a1_rval);

				uni_printf(enc->sx->io, "\tli t0, %llu\n", argv);
				uni_printf(enc->sx->io, "\n");
				emit_move_rvalue_to_register(enc, R_A1, &arg_rvalue);

				uni_printf(enc->sx->io, "\tlui t1, %%hi(.printid)\n");
				uni_printf(enc->sx->io, "\taddi a0, t1, %%lo(.printid)\n");

				uni_printf(enc->sx->io, "\tjal printf\n");
				uni_printf(enc->sx->io, "\t");
				instruction_to_io(enc->sx->io, IC_RISCV_NOP);
				uni_printf(enc->sx->io, "\n");

				free_rvalue(enc, &arg_rvalue);

				uni_printf(enc->sx->io, "\n\t# data restoring:\n");
		} while (k < array_sizes[-start]);
	}

	const lvalue a0_lval = { .base_reg = R_SP,
							 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
							 // второй на 2*WORD_LENGTH и т.д.
							 .loc.displ = 0,
							 .kind = LVALUE_KIND_STACK,
							 .type = TYPE_INTEGER };
	const rvalue a0_rval = {
		.from_lvalue = !FROM_LVALUE, .kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A0, .type = TYPE_INTEGER
	};
	emit_store_of_rvalue(enc, &a0_lval, &a0_rval);


	uni_printf(enc->sx->io, "\n\t# data restoring:\n");
	const rvalue a0_rval_to_copy = emit_load_of_lvalue(enc, &a0_lval);
	emit_move_rvalue_to_register(enc, R_A0, &a0_rval_to_copy);

	free_rvalue(enc, &a0_rval_to_copy);
	// FIXME: Возвращает число распечатанных символов (включая '\0'?)
	return RVALUE_VOID;
}



static rvalue emit_printid_expression_old(encoder *const enc, const node *const nd)
{
	const size_t argc = expression_call_get_arguments_amount(nd);
	const node arg = expression_call_get_argument(nd, 0);
	const size_t argv = expression_identifier_get_id(&arg);
	printf("%llu", argv);
	// compress_ident(enc, expression_identifier_get_id(&arg)); // Ссылка в identtab

	// const size_t amount_smth = declaration_variable_get_bounds(nd);
	// printf("%llu", amount_smth);

	int k = 0;
	int start = 0;

		do
		{
			k += 1;

			// uni_printf(enc->sx->io, "\t!!!!!!!!!!!!\n");
			// uni_printf(enc->sx->io, "#");
			const node arg = expression_call_get_argument(nd, 0);
			const rvalue val = emit_expression(enc, &arg);

			if (start == 0)
				start = current_memory_location;
			// printf("%i\n", parameters_amount);
			// uni_printf(enc->sx->io, "%" PRIitem, val.val.int_val);
			// uni_printf(enc->sx->io, "\t!!!!!!!!!!!!\n");

			if (array_sizes[-start] != 0)
			{
				uni_printf(enc->sx->io, "\tli t0, %i\n", k * current_memory_location);
				uni_printf(enc->sx->io, "\tadd t0, t0, fp\n");
			}
			// for (int i = val.val.int_val; i <= val.val.int_val * array_sizes[val.val.int_val]; i += 4)
			const rvalue arg_rvalue = (val.kind == RVALUE_KIND_CONST) ? emit_load_of_immediate(enc, &val) : val;
			const item_t arg_rvalue_type = arg_rvalue.type;

			uni_printf(enc->sx->io, "\n");

			const lvalue a0_lval = { .base_reg = R_SP,
									 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
									 // второй на 2*WORD_LENGTH и т.д.
									 .loc.displ = 0,
									 .kind = LVALUE_KIND_STACK,
									 .type = arg_rvalue.type };
			const rvalue a0_rval = {
				.kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A0, .type = TYPE_INTEGER, .from_lvalue = !FROM_LVALUE
			};
			// emit_store_of_rvalue(enc, &a0_lval, &a0_rval);

			const lvalue a1_lval = { .base_reg = R_SP,
									 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
									 // второй на 2*WORD_LENGTH и т.д.
									 .loc.displ = WORD_LENGTH,
									 .kind = LVALUE_KIND_STACK,
									 .type = arg_rvalue.type };
			const rvalue a1_rval = {
				.kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A1, .type = TYPE_INTEGER, .from_lvalue = !FROM_LVALUE
			};
			// emit_store_of_rvalue(enc, &a1_lval, &a1_rval);

				uni_printf(enc->sx->io, "\tli t0, %llu\n", argv);
				uni_printf(enc->sx->io, "\n");
				emit_move_rvalue_to_register(enc, R_A1, &arg_rvalue);

				uni_printf(enc->sx->io, "\tlui t1, %%hi(.i)\n");
				uni_printf(enc->sx->io, "\taddi a0, t1, %%lo(.i)\n");

				uni_printf(enc->sx->io, "\tjal printf\n");
				uni_printf(enc->sx->io, "\t");
				instruction_to_io(enc->sx->io, IC_RISCV_NOP);
				uni_printf(enc->sx->io, "\n");

				free_rvalue(enc, &arg_rvalue);

				uni_printf(enc->sx->io, "\n\t# data restoring:\n");
		} while (k < array_sizes[-start]);
		// const rvalue a0_rval_to_copy = emit_load_of_lvalue(enc, &a0_lval);
		// emit_move_rvalue_to_register(enc, R_A0, &a0_rval_to_copy);

		// free_rvalue(enc, &a0_rval_to_copy);
		// uni_printf(enc->sx->io, "\n");

		// const rvalue a1_rval_to_copy = emit_load_of_lvalue(enc, &a1_lval);
		// emit_move_rvalue_to_register(enc, R_A1, &a1_rval_to_copy);

		// free_rvalue(enc, &a1_rval_to_copy);
		// uni_printf(enc->sx->io, "\n");

		// to_code_2R_I(enc->sx->io, IC_RISCV_ADDI, R_SP, R_SP,
		//			 (item_t)WORD_LENGTH *
		//				 (!type_is_floating(enc->sx, arg_rvalue_type) ? /* a0 и a1 */ 1 : /* a0, a1 и a2 */ 2));
		// uni_printf(enc->sx->io, "\n");

	const lvalue a0_lval = { .base_reg = R_SP,
							 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
							 // второй на 2*WORD_LENGTH и т.д.
							 .loc.displ = 0,
							 .kind = LVALUE_KIND_STACK,
							 .type = TYPE_INTEGER };
	const rvalue a0_rval = {
		.from_lvalue = !FROM_LVALUE, .kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A0, .type = TYPE_INTEGER
	};
	emit_store_of_rvalue(enc, &a0_lval, &a0_rval);

	uni_printf(enc->sx->io, "\n\t# data restoring:\n");
	const rvalue a0_rval_to_copy = emit_load_of_lvalue(enc, &a0_lval);
	emit_move_rvalue_to_register(enc, R_A0, &a0_rval_to_copy);

	free_rvalue(enc, &a0_rval_to_copy);
	// FIXME: Возвращает число распечатанных символов (включая '\0'?)
	return RVALUE_VOID;
}


static rvalue emit_print_expression(encoder *const enc, const node *const nd)
{
	const size_t amount = strings_amount(enc->sx);
	const size_t parameters_amount = expression_call_get_arguments_amount(nd);

	int k = 0;
	int start = 0;
	for (size_t i = 0; i < parameters_amount; i++)
	{
		do
		{
			const node arg = expression_call_get_argument(nd, i);
			//const size_t argv = expression_identifier_get_id(&arg);
			k += 1;

			// uni_printf(enc->sx->io, "\t!!!!!!!!!!!!\n");
			// uni_printf(enc->sx->io, "#");
			// const node arg = expression_call_get_argument(nd, i);
			const rvalue val = emit_expression(enc, &arg);

			if (start == 0)
				start = current_memory_location;
			// printf("%i\n", parameters_amount);
			// uni_printf(enc->sx->io, "%" PRIitem, val.val.int_val);
			// uni_printf(enc->sx->io, "\t!!!!!!!!!!!!\n");

			if (array_sizes[-start] != 0)
			{
				uni_printf(enc->sx->io, "\tli t0, %llu\n", k * current_memory_location);
				uni_printf(enc->sx->io, "\tadd t0, t0, fp\n");
			}
			// for (int i = val.val.int_val; i <= val.val.int_val * array_sizes[val.val.int_val]; i += 4)
			const rvalue arg_rvalue = (val.kind == RVALUE_KIND_CONST) ? emit_load_of_immediate(enc, &val) : val;
			const item_t arg_rvalue_type = arg_rvalue.type;

			uni_printf(enc->sx->io, "\n");

			const lvalue a0_lval = { .base_reg = R_SP,
									 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
									 // второй на 2*WORD_LENGTH и т.д.
									 .loc.displ = 0,
									 .kind = LVALUE_KIND_STACK,
									 .type = arg_rvalue.type };
			const rvalue a0_rval = {
				.kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A0, .type = TYPE_INTEGER, .from_lvalue = !FROM_LVALUE
			};
			// emit_store_of_rvalue(enc, &a0_lval, &a0_rval);

			const lvalue a1_lval = { .base_reg = R_SP,
									 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
									 // второй на 2*WORD_LENGTH и т.д.
									 .loc.displ = WORD_LENGTH,
									 .kind = LVALUE_KIND_STACK,
									 .type = arg_rvalue.type };
			const rvalue a1_rval = {
				.kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A1, .type = TYPE_INTEGER, .from_lvalue = !FROM_LVALUE
			};
			// emit_store_of_rvalue(enc, &a1_lval, &a1_rval);

			//где то тут реализовать разные типы
			if (type_is_integer(enc->sx, arg_rvalue.type))
			{
				uni_printf(enc->sx->io, "\n");
				emit_move_rvalue_to_register(enc, R_A1, &arg_rvalue);

				uni_printf(enc->sx->io, "\tlui t1, %%hi(.i)\n");
				uni_printf(enc->sx->io, "\taddi a0, t1, %%lo(.i)\n");

				uni_printf(enc->sx->io, "\tjal printf\n");
				uni_printf(enc->sx->io, "\t");
				instruction_to_io(enc->sx->io, IC_RISCV_NOP);
				uni_printf(enc->sx->io, "\n");

				free_rvalue(enc, &arg_rvalue);

				uni_printf(enc->sx->io, "\n\t# data restoring:\n");
			}

			else if (type_is_floating(enc->sx, arg_rvalue.type))
			{
				uni_printf(enc->sx->io, "\tfsd f0, (sp)\n");
				uni_printf(enc->sx->io, "\tlw a0, (sp)\n");
				uni_printf(enc->sx->io, "\tflw fa2, (sp)\n");
				const lvalue a2_lval = { .base_reg = R_SP,
										 // по call convention: первый на WORD_LENGTH выше предыдущего положения
										 // fp, второй на 2*WORD_LENGTH и т.д.
										 .loc.displ = 2 * WORD_LENGTH,
										 .type = TYPE_INTEGER,
										 .kind = LVALUE_KIND_STACK };
				const rvalue a2_rval = {
					.kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A2, .type = TYPE_INTEGER, .from_lvalue = !FROM_LVALUE
				};

				// emit_store_of_rvalue(enc, &a2_lval, &a2_rval);
				uni_printf(enc->sx->io, "\n");

				uni_printf(enc->sx->io, "\tfcvt.d.s fa2,fa2\n");
				uni_printf(enc->sx->io, "\tfmv.x.d a1,fa2\n");
				uni_printf(enc->sx->io, "\tlui a5, %%hi(.f)\n");
				uni_printf(enc->sx->io, "\taddi a0, a5, %%lo(.f)\n");
				uni_printf(enc->sx->io, "\tfmv.x.d a1, ft0\n");
				uni_printf(enc->sx->io, "\tcall printf\n\t");
				instruction_to_io(enc->sx->io, IC_RISCV_NOP);
				uni_printf(enc->sx->io, "\n");

				// Восстановление регистров-аргументов -- они могут понадобится в дальнейшем
				uni_printf(enc->sx->io, "\n\t# data restoring:\n");

				const rvalue a2_rval_to_copy = emit_load_of_lvalue(enc, &a2_lval);
				emit_move_rvalue_to_register(enc, R_A2, &a2_rval_to_copy);

				free_rvalue(enc, &a2_rval);
				free_rvalue(enc, &arg_rvalue);
				uni_printf(enc->sx->io, "\n");
			}
			else if (type_is_string(enc->sx, arg_rvalue.type)) {
					const node string = expression_call_get_argument(nd, i);
					const size_t index = expression_literal_get_string(&string);
					uni_printf(enc->sx->io, "\n");
					emit_move_rvalue_to_register(enc, R_A1, &arg_rvalue);

					uni_printf(enc->sx->io, "\tlui t1, %%hi(STRING%zu)\n", index);
					uni_printf(enc->sx->io, "\taddi a0, t1, %%lo(STRING%zu)\n", index);

					uni_printf(enc->sx->io, "\tjal printf\n");
					uni_printf(enc->sx->io, "\t");
					instruction_to_io(enc->sx->io, IC_RISCV_NOP);
					uni_printf(enc->sx->io, "\n");

					free_rvalue(enc, &arg_rvalue);

					uni_printf(enc->sx->io, "\n\t# data restoring:\n");
				//printf("!!!!!!!\n");
				//uni_printf(enc->sx->io, "\#!!!!!!!!!\n#!!!!!!!!!!!!\n#!!!!!!!\n");
			}
		} while (k < array_sizes[-start]);
	}

	const lvalue a0_lval = { .base_reg = R_SP,
							 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
							 // второй на 2*WORD_LENGTH и т.д.
							 .loc.displ = 0,
							 .kind = LVALUE_KIND_STACK,
							 .type = TYPE_INTEGER };
	const rvalue a0_rval = {
		.from_lvalue = !FROM_LVALUE, .kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A0, .type = TYPE_INTEGER
	};
	emit_store_of_rvalue(enc, &a0_lval, &a0_rval);


	uni_printf(enc->sx->io, "\n\t# data restoring:\n");
	const rvalue a0_rval_to_copy = emit_load_of_lvalue(enc, &a0_lval);
	emit_move_rvalue_to_register(enc, R_A0, &a0_rval_to_copy);

	free_rvalue(enc, &a0_rval_to_copy);
	// FIXME: Возвращает число распечатанных символов (включая '\0'?)
	return RVALUE_VOID;
}

static rvalue emit_print_expression_old(encoder *const enc, const node *const nd)
{
	// uni_printf(enc->sx->io, "\n\t------------printf expr------------------\n");
	//const node string = expression_call_get_argument(nd, 0);
	//const size_t index = expression_literal_get_string(&string);
	const size_t amount = strings_amount(enc->sx);
	const size_t parameters_amount = expression_call_get_arguments_amount(nd);

	// const size_t amount_smth = declaration_variable_get_bounds(nd);
	// printf("%llu", amount_smth);

	int k = 0;
	int start = 0;
	for (size_t i = 1; i <= parameters_amount; i++)
	{
		do
		{
			//printf("!!");
			k += 1;

			// uni_printf(enc->sx->io, "\t!!!!!!!!!!!!\n");
			// uni_printf(enc->sx->io, "#");
			const node arg = expression_call_get_argument(nd, i);
			const rvalue val = emit_expression(enc, &arg);

			if (start == 0)
				start = current_memory_location;
			// printf("%i\n", parameters_amount);
			// uni_printf(enc->sx->io, "%" PRIitem, val.val.int_val);
			// uni_printf(enc->sx->io, "\t!!!!!!!!!!!!\n");

			if (array_sizes[-start] != 0)
			{
				uni_printf(enc->sx->io, "\tli t0, %i\n", k * current_memory_location);
				uni_printf(enc->sx->io, "\tadd t0, t0, fp\n");
			}
			// for (int i = val.val.int_val; i <= val.val.int_val * array_sizes[val.val.int_val]; i += 4)
			const rvalue arg_rvalue = (val.kind == RVALUE_KIND_CONST) ? emit_load_of_immediate(enc, &val) : val;
			const item_t arg_rvalue_type = arg_rvalue.type;

			uni_printf(enc->sx->io, "\n");

			const lvalue a0_lval = { .base_reg = R_SP,
									 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
									 // второй на 2*WORD_LENGTH и т.д.
									 .loc.displ = 0,
									 .kind = LVALUE_KIND_STACK,
									 .type = arg_rvalue.type };
			const rvalue a0_rval = {
				.kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A0, .type = TYPE_INTEGER, .from_lvalue = !FROM_LVALUE
			};
			// emit_store_of_rvalue(enc, &a0_lval, &a0_rval);

			const lvalue a1_lval = { .base_reg = R_SP,
									 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
									 // второй на 2*WORD_LENGTH и т.д.
									 .loc.displ = WORD_LENGTH,
									 .kind = LVALUE_KIND_STACK,
									 .type = arg_rvalue.type };
			const rvalue a1_rval = {
				.kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A1, .type = TYPE_INTEGER, .from_lvalue = !FROM_LVALUE
			};
			// emit_store_of_rvalue(enc, &a1_lval, &a1_rval);
			if (type_is_integer(enc->sx, arg_rvalue.type))
			{
				uni_printf(enc->sx->io, "\n");
				emit_move_rvalue_to_register(enc, R_A1, &arg_rvalue);

				uni_printf(enc->sx->io, "\tlui t1, %%hi(.i)\n");
				uni_printf(enc->sx->io, "\taddi a0, t1, %%lo(.i)\n");

				uni_printf(enc->sx->io, "\tjal printf\n");
				uni_printf(enc->sx->io, "\t");
				instruction_to_io(enc->sx->io, IC_RISCV_NOP);
				uni_printf(enc->sx->io, "\n");

				free_rvalue(enc, &arg_rvalue);

				uni_printf(enc->sx->io, "\n\t# data restoring:\n");
			}
			else if (type_is_floating(enc->sx, arg_rvalue.type))
			{
				uni_printf(enc->sx->io, "\tfsd f0, (sp)\n");
				uni_printf(enc->sx->io, "\tlw a0, (sp)\n");
				uni_printf(enc->sx->io, "\tflw fa2, (sp)\n");
				const lvalue a2_lval = { .base_reg = R_SP,
										 // по call convention: первый на WORD_LENGTH выше предыдущего положения
										 // fp, второй на 2*WORD_LENGTH и т.д.
										 .loc.displ = 2 * WORD_LENGTH,
										 .type = TYPE_INTEGER,
										 .kind = LVALUE_KIND_STACK };
				const rvalue a2_rval = {
					.kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A2, .type = TYPE_INTEGER, .from_lvalue = !FROM_LVALUE
				};

				// emit_store_of_rvalue(enc, &a2_lval, &a2_rval);
				uni_printf(enc->sx->io, "\n");

				uni_printf(enc->sx->io, "\tfcvt.d.s fa2,fa2\n");
				uni_printf(enc->sx->io, "\tfmv.x.d a1,fa2\n");
				uni_printf(enc->sx->io, "\tlui a5, %%hi(.f)\n");
				uni_printf(enc->sx->io, "\taddi a0, a5, %%lo(.f)\n");
				uni_printf(enc->sx->io, "\tfmv.x.d a1, ft0\n");
				uni_printf(enc->sx->io, "\tcall printf\n\t");
				instruction_to_io(enc->sx->io, IC_RISCV_NOP);
				uni_printf(enc->sx->io, "\n");

				// Восстановление регистров-аргументов -- они могут понадобится в дальнейшем
				uni_printf(enc->sx->io, "\n\t# data restoring:\n");

				const rvalue a2_rval_to_copy = emit_load_of_lvalue(enc, &a2_lval);
				emit_move_rvalue_to_register(enc, R_A2, &a2_rval_to_copy);

				free_rvalue(enc, &a2_rval);
				free_rvalue(enc, &arg_rvalue);
				uni_printf(enc->sx->io, "\n");
			}
		} while (k < array_sizes[-start]);
		// const rvalue a0_rval_to_copy = emit_load_of_lvalue(enc, &a0_lval);
		// emit_move_rvalue_to_register(enc, R_A0, &a0_rval_to_copy);

		// free_rvalue(enc, &a0_rval_to_copy);
		// uni_printf(enc->sx->io, "\n");

		// const rvalue a1_rval_to_copy = emit_load_of_lvalue(enc, &a1_lval);
		// emit_move_rvalue_to_register(enc, R_A1, &a1_rval_to_copy);

		// free_rvalue(enc, &a1_rval_to_copy);
		// uni_printf(enc->sx->io, "\n");

		// to_code_2R_I(enc->sx->io, IC_RISCV_ADDI, R_SP, R_SP,
		//			 (item_t)WORD_LENGTH *
		//				 (!type_is_floating(enc->sx, arg_rvalue_type) ? /* a0 и a1 */ 1 : /* a0, a1 и a2 */ 2));
		// uni_printf(enc->sx->io, "\n");
	}

	const lvalue a0_lval = { .base_reg = R_SP,
							 // по call convention: первый на WORD_LENGTH выше предыдущего положения fp,
							 // второй на 2*WORD_LENGTH и т.д.
							 .loc.displ = 0,
							 .kind = LVALUE_KIND_STACK,
							 .type = TYPE_INTEGER };
	const rvalue a0_rval = {
		.from_lvalue = !FROM_LVALUE, .kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A0, .type = TYPE_INTEGER
	};
	emit_store_of_rvalue(enc, &a0_lval, &a0_rval);

	//uni_printf(enc->sx->io, "\tlui a5, %%hi(STRING%zu)\n", index + (parameters_amount - 1) * amount);
	//uni_printf(enc->sx->io, "\taddi a0, a5, %%lo(STRING%zu)\n", index + (parameters_amount - 1) * amount);
	//uni_printf(enc->sx->io, "\tcall printf\n");
	//uni_printf(enc->sx->io, "\t");
	//instruction_to_io(enc->sx->io, IC_RISCV_NOP);
	//uni_printf(enc->sx->io, "\n");

	uni_printf(enc->sx->io, "\n\t# data restoring:\n");
	const rvalue a0_rval_to_copy = emit_load_of_lvalue(enc, &a0_lval);
	emit_move_rvalue_to_register(enc, R_A0, &a0_rval_to_copy);

	free_rvalue(enc, &a0_rval_to_copy);
	// FIXME: Возвращает число распечатанных символов (включая '\0'?)
	return RVALUE_VOID;
}

static rvalue emit_strcat_expression(encoder *const enc, const node *const nd)
{
	const node arg1 = expression_call_get_argument(nd, 0);
	const node arg2 = expression_call_get_argument(nd, 1);


	const lvalue lval1 = emit_lvalue(enc, &arg1);
	const lvalue lval2 = emit_lvalue(enc, &arg2);

	const int size1 = array_sizes[-link_on_true_location[-lval1.loc.displ]];
	const int size2 = array_sizes[-link_on_true_location[-lval2.loc.displ]];
	//printf("%i", size1);
	//emit_expression(enc, &arg1);
	//uni_printf(enc->sx->io, "\tli t1, %lli\n", lval2.loc.displ);
	//uni_printf(enc->sx->io, "\tadd t1, t1, fp\n");

	//for (int i = 1; i <= size2; i++)
	//{
	//	uni_printf(enc->sx->io, "\tlw t2, %i(t1)\n ", (lval2.loc.displ * size2) - (4 * i));
	//	uni_printf(enc->sx->io, "\tsw t2, %i(t0)\n", (lval1.loc.displ * size1) - (4 * i));
	//}
	//printf("%i", link_on_true_location[-lval2.loc.displ]);
	for (int i = 0; i < size2; i++)
	{
		uni_printf(enc->sx->io, "\tlw t0, %i(fp)\n", (link_on_true_location[-lval2.loc.displ] - (4 * i)));
		uni_printf(enc->sx->io, "\tsw t0, %i(fp)\n",
				   link_on_true_location[-lval1.loc.displ] * array_sizes[-link_on_true_location[-lval1.loc.displ]] - 4 - 4 * i);
	}
	//const rvalue tmp = emit_expression(enc, &arg1);
	
	// 
	//const node bound = declaration_variable_get_bound(nd, 0);
	//uni_printf(enc->sx->io, "");
	//const rvalue tmp = emit_expression(enc, &arg2);
	//for (int i = 0; i < 1000; i++)

	//printf("%i\n", tmp.val.str_index);

	//const rvalue val1 = emit_expression(enc, &arg1);
	//const rvalue val2 = emit_expression(enc, &arg2);


	//size_t amount = expression_initializer_get_size(&arg2);
	//uni_printf(enc->sx->io, "!!!!!!, %" PRIitem "!!!!!!!!!!\n", -(item_t)amount);
	//size_t amount1 = expression_inline_get_size(arg1);
	//const size_t amount2 = expression_inline_get_size(arg2);
	//printf("%llu", array_sizes[4]);
	//uni_printf()
	free_register(enc, R_T0);
}

static rvalue emit_strncpy_expression(encoder *const enc, const node *const nd)
{
	//const node callee = expression_call_get_callee(nd);
	const node callee2 = expression_call_get_argument(nd, 0);
	const size_t func_ref = expression_identifier_get_id(&callee2);
	lvalue variable = displacements_add(enc, func_ref, false);
	printf("%i", variable.loc.displ);
	for (int i = 0; i < 20; i++)
	{
		//printf("%i  %i\n", -i, true_loc[i]);
	}
	const node arg1 = expression_call_get_argument(nd, 0);
	const node arg2 = expression_call_get_argument(nd, 1);
	const lvalue lval1 = emit_lvalue(enc, &arg1);
	const lvalue lval2 = emit_lvalue(enc, &arg2);
	printf("%i", link_on_true_location[-lval1.loc.displ]);
	//int s = arg2.tree->array[arg2.index];
	//printf("!%i!", s);
	const int size1 = array_sizes[-link_on_true_location[-lval1.loc.displ]];
	const int size2 = array_sizes[-link_on_true_location[-lval2.loc.displ]];
	//uni_printf(enc->sx->io, "!!!!!!!%llu!!!!!!", lvalue_location);
	//const rvalue tmp_rval = emit_load_of_lvalue(enc, &prev_arg_displ[]);
	//const node initializer = declaration_variable_get_initializer(nd);
	//const lvalue lval = emit_lvalue(enc, &initializer);
	//const lvalue lval = emit_lvalue(enc, nd);
	//expression_
	//const size_t identifier = declaration_variable_get_id(nd);
	//const lvalue variable = displacements_add(enc, identifier, false);
	//printf("%i", lval.type);

	for (int i = 0; i < size2; i++)
	{
		uni_printf(enc->sx->io, "\tlw t0, %i(fp)\n", (link_on_true_location[-lval2.loc.displ] - (4 * i)));
		uni_printf(enc->sx->io, "\tsw t0, %i(fp)\n", link_on_true_location[-lval1.loc.displ] - 4 * i);
	}
	free_register(enc, R_T0);
}
	/**
 *	Emit builtin function call
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Rvalue of builtin function call expression
 */
static rvalue emit_builtin_call(encoder *const enc, const node *const nd)
{
	const node callee = expression_call_get_callee(nd);
	const size_t func_ref = expression_identifier_get_id(&callee);
	switch (func_ref)
	{
		case BI_PRINTF:
			return emit_printf_expression(enc, nd);

		default:
			return RVALUE_VOID;
	}
}

/**
 *	Emit call expression
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Rvalue of the result of call expression
 */
static rvalue emit_call_expression(encoder *const enc, const node *const nd)
{
	const node callee = expression_call_get_callee(nd);
	const size_t func_ref = expression_identifier_get_id(&callee);
	const size_t params_amount = expression_call_get_arguments_amount(nd);
	//printf("%llu", params_amount);
	//assert(func_ref >= BEGIN_USER_FUNC); // поддерживаются только пользовательнские функции

	const item_t return_type = type_function_get_return_type(enc->sx, expression_get_type(&callee));
	uni_printf(enc->sx->io, "\t# \"%s\" function call:\n", ident_get_spelling(enc->sx, func_ref));

	rvalue ret = { .kind = RVALUE_KIND_REGISTER,
				   .type = return_type,
				   .val.reg_num = type_is_floating(enc->sx, return_type) ? R_FA0 : R_A0,
				   .from_lvalue = !FROM_LVALUE };

	// stack displacement: насколько нужно сместить стек, чтобы сохранить текущие значение регистров
	// на каждый аргумент отводится 8 байт (чтобы хранить double значения)
	size_t displ_for_parameters = (params_amount + 1) * WORD_LENGTH * 2 * 10;

	// previous arguments displacement: здесь сохраняем на какой позиции мы сохранили каждый регистр,
	// чтобы после возврата из функции  их восстановить
	//lvalue prev_arg_displ[params_amount];
	lvalue prev_arg_displ[10000];


	// сдвигаем стек на кол-сто аргументов. В стеке хранятся или забекапенные данные,
	// которые были в регистрах a0-a7, или аргументы, которые не поместились в регистры.
	// TODO: почему в riscvgen стек не сдвигается здесь, если аргумент один, но
	// 		 вместо этого в вызываемой функции sp сдвигается на слово?
	if (params_amount >= 1)
	{
		uni_printf(enc->sx->io, "\t # displacing stack for parameters\n");
		to_code_2R_I(enc->sx->io, IC_RISCV_ADDI, R_SP, R_SP, -(item_t)(displ_for_parameters));
	}

	uni_printf(enc->sx->io, "\n\t# passing %zu parameters \n", params_amount);

	lvalue saved_a0_lvalue;
	lvalue saved_f0_lvalue;

	size_t f_arg_counter = 0;
	size_t arg_counter = 0;

	for (size_t i = 0; i < params_amount; i++)
	{
		const node arg = expression_call_get_argument(nd, i);
		// транслируем аргумент, в объекте rvalue информация о его типе
		// TODO: что если аргумент - структура, которая сохранена на стеке
		// TODO: что если аргумент - структура или тип, который занимает несколько регистров?
		// TODO: возможно оптимизировать трансляцию указанного выше, меняя порядок аргументов
		emit_literal = false;

		const rvalue tmp = emit_expression(enc, &arg);
		emit_literal = true;
		const rvalue arg_rvalue = (tmp.kind == RVALUE_KIND_CONST) ? emit_load_of_immediate(enc, &tmp) : tmp;
		// assert(!type_is_floating(enc->sx, arg_rvalue.type));
		bool is_floating = type_is_floating(enc->sx, arg_rvalue.type);
		size_t curr_arg_counter = is_floating ? f_arg_counter++ : arg_counter++;

		riscv_register_t arg_register = (is_floating ? R_FA0 : R_A0) + curr_arg_counter;

		// tmp_arg_lvalue представляет место на стеке, куда сохраняем регистры a0-a7
		const lvalue tmp_arg_lvalue = {
			.base_reg = R_SP, .loc.displ = (i + 1) * WORD_LENGTH * 2, .kind = LVALUE_KIND_STACK, .type = arg_rvalue.type
		};

		// arg_saved_rvalue представляет значение регистра, которое мы будем сохранять на стек
		const rvalue arg_saved_rvalue = { .kind = RVALUE_KIND_REGISTER,
										  .val.reg_num = arg_register,
										  .type = arg_rvalue.type,
										  .from_lvalue = !FROM_LVALUE };

		if (curr_arg_counter == 0)
		{
			// значение, которое будет в a0 / fa0 сохраняем вначале на стек,
			// а в сам регистр загрузим в последнюю очередь, так как
			// вложенные вызовы могут затереть a0 и fa0
			const lvalue saved_reg = (lvalue){ .base_reg = R_SP,
											   .loc.displ = is_floating ? WORD_LENGTH * 2 : 0,
											   .kind = LVALUE_KIND_STACK,
											   .type = arg_rvalue.type };
			if (is_floating)
			{
				saved_f0_lvalue = saved_reg;
			}
			else
			{
				saved_a0_lvalue = saved_reg;
			}
			//emit_store_of_rvalue(enc, &saved_reg, &arg_rvalue);
		}
		else
		{
			// сохраняем a1..a7, f11..f17 на стек
			// невлезающие в регистры аргументы сохраняем на стек следом -- call convention
			//uni_printf(enc->sx->io, "\t#!!!!!!!!!!\n");
			//emit_store_of_rvalue(enc, &tmp_arg_lvalue,
			//					 curr_arg_counter < ARG_REG_AMOUNT ? &arg_saved_rvalue : &arg_rvalue);

			if (curr_arg_counter < ARG_REG_AMOUNT)
			{
				// теперь записываем в регистры a1-a7/fa1-fa7 аргументы
				emit_move_rvalue_to_register(enc, arg_register, &arg_rvalue);
			}
		}

		// Запоминаем lvalue объект, который представляет забекапенное значение
		prev_arg_displ[i] = tmp_arg_lvalue;

		free_rvalue(enc, &arg_rvalue);
	}

	// загружаем первые аргументы последним делом
	// со стека, где мы их ранее сохранили
	if (arg_counter)
	{
		//const rvalue tmp_rval = emit_load_of_lvalue(enc, &saved_a0_lvalue);
		//emit_move_rvalue_to_register(enc, R_A0, &tmp_rval);
	}
	if (f_arg_counter)
	{
		//const rvalue tmp_rval = emit_load_of_lvalue(enc, &saved_f0_lvalue);
		//emit_move_rvalue_to_register(enc, R_FA0, &tmp_rval);
	}

	const label label_func = { .kind = L_FUNC, .num = func_ref };
	// выполняем прыжок в функцию по относительному смещению (метке)
	if (label_func.num == 162)
	{
		emit_printid_expression(enc, nd);
	}
	else if (label_func.num == 158)
	{
		emit_print_expression(enc, nd);
	}
	else if (label_func.num == 46)
	{
		emit_strcat_expression(enc, nd);
	}
	else if (label_func.num == 42)
	{
		emit_strncpy_expression(enc, nd);
	}
	else if (label_func.num != 154 && label_func.num != 6 && label_func.num != 10 && label_func.num != 14 &&
		label_func.num != 18 && label_func.num != 22 && label_func.num != 26 && label_func.num != 30 &&
		label_func.num != 46)
	{
		emit_unconditional_branch(enc, IC_RISCV_JAL, &label_func);
	}
	else if (label_func.num != 10 && label_func.num != 14 && label_func.num != 18 && label_func.num != 22 &&
			 label_func.num != 26 && label_func.num != 30 && label_func.num != 46)
	{
		// uni_printf(enc->sx->io, "\t#$$$$$$$$$$$$$$$\n");
		emit_printf_expression(enc, nd);
		// uni_printf(enc->sx->io, "\t#&&&&&&&&&&&&&&\n");
	}
	uni_printf(enc->sx->io, "\n");
	if (label_func.num == 6)
	{
		uni_printf(enc->sx->io, "\tfmv.x.d a0, f0\n");
		uni_printf(enc->sx->io, "\tfmv.d.x fa0, a0 \n");
		uni_printf(enc->sx->io, "\tcall    asin\n");
		uni_printf(enc->sx->io, "\tfcvt.s.d        f10, fa0\n");
		uni_printf(enc->sx->io, "\tfcvt.d.s        f10, f10\n");
		// emit_store_of_rvalue(enc, &saved_f0_lvalue, &tmp);
		// emit_store_of_rvalue(enc, &saved_f0_lvalue, );
	}

	if (label_func.num == 10) {
		uni_printf(enc->sx->io, "\tfmv.x.d a0, f0\n");
		uni_printf(enc->sx->io, "\tfmv.d.x fa0, a0 \n");
		uni_printf(enc->sx->io, "\tcall    cos\n");
		uni_printf(enc->sx->io, "\tfcvt.s.d        f10, fa0\n");
		uni_printf(enc->sx->io, "\tfcvt.d.s        f10, f10\n");
		//emit_store_of_rvalue(enc, &saved_f0_lvalue, &tmp);
		//emit_store_of_rvalue(enc, &saved_f0_lvalue, );
	}

	if (label_func.num == 14)
	{
		uni_printf(enc->sx->io, "\tfmv.x.d a0, f0\n");
		uni_printf(enc->sx->io, "\tfmv.d.x fa0, a0 \n");
		uni_printf(enc->sx->io, "\tcall    sin\n");
		uni_printf(enc->sx->io, "\tfcvt.s.d        f10, fa0\n");
		uni_printf(enc->sx->io, "\tfcvt.d.s        f10, f10\n");
		// emit_store_of_rvalue(enc, &saved_f0_lvalue, &tmp);
		// emit_store_of_rvalue(enc, &saved_f0_lvalue, );
	}

	if (label_func.num == 18)
	{
		uni_printf(enc->sx->io, "\tfmv.x.d a0, f0\n");
		uni_printf(enc->sx->io, "\tfmv.d.x fa0, a0 \n");
		uni_printf(enc->sx->io, "\tcall    exp\n");
		uni_printf(enc->sx->io, "\tfcvt.s.d        f10, fa0\n");
		uni_printf(enc->sx->io, "\tfcvt.d.s        f10, f10\n");
		// emit_store_of_rvalue(enc, &saved_f0_lvalue, &tmp);
		// emit_store_of_rvalue(enc, &saved_f0_lvalue, );
	}

	if (label_func.num == 22)
	{
		uni_printf(enc->sx->io, "\tfmv.x.d a0, f0\n");
		uni_printf(enc->sx->io, "\tfmv.d.x fa0, a0 \n");
		uni_printf(enc->sx->io, "\tcall    log\n");
		uni_printf(enc->sx->io, "\tfcvt.s.d        f10, fa0\n");
		uni_printf(enc->sx->io, "\tfcvt.d.s        f10, f10\n");
		// emit_store_of_rvalue(enc, &saved_f0_lvalue, &tmp);
		// emit_store_of_rvalue(enc, &saved_f0_lvalue, );
	}

	if (label_func.num == 26)
	{
		uni_printf(enc->sx->io, "\tfmv.x.d a0, f0\n");
		uni_printf(enc->sx->io, "\tfmv.d.x fa0, a0 \n");
		uni_printf(enc->sx->io, "\tcall    log10\n");
		uni_printf(enc->sx->io, "\tfcvt.s.d        f10, fa0\n");
		uni_printf(enc->sx->io, "\tfcvt.d.s        f10, f10\n");
		// emit_store_of_rvalue(enc, &saved_f0_lvalue, &tmp);
		// emit_store_of_rvalue(enc, &saved_f0_lvalue, );
	}

	if (label_func.num == 30)
	{
		uni_printf(enc->sx->io, "\tfmv.x.d a0, f0\n");
		uni_printf(enc->sx->io, "\tfmv.d.x fa0, a0 \n");
		uni_printf(enc->sx->io, "\tcall    sqrt\n");
		uni_printf(enc->sx->io, "\tfcvt.s.d        f10, fa0\n");
		uni_printf(enc->sx->io, "\tfcvt.d.s        f10, f10\n");
		// emit_store_of_rvalue(enc, &saved_f0_lvalue, &tmp);
		// emit_store_of_rvalue(enc, &saved_f0_lvalue, );
	}

	if (params_amount > 0)
		uni_printf(enc->sx->io, "\n\t# register restoring:\n");

	f_arg_counter = arg_counter = 0;
	// восстановление значений регистров a1-a7 со стека
	for (size_t i = 1; i < params_amount; ++i)
	{
		//lvalue_location = 20;
		//printf("%llu\n", lvalue_location);
		uni_printf(enc->sx->io, "\n");
		// загружаем во временный регистр значение аргумента со стека
		const rvalue tmp_rval = emit_load_of_lvalue(enc, &prev_arg_displ[i]);
		bool is_floating = type_is_floating(enc->sx, prev_arg_displ[i].type);
		size_t curr_arg_counter = is_floating ? ++f_arg_counter : ++arg_counter;

		riscv_register_t arg_register = (is_floating ? R_FA0 : R_A0) + curr_arg_counter;

		// теперь возвращаем изначальное значение регистра a1-a7
		//emit_move_rvalue_to_register(enc, arg_register, &tmp_rval);
		// говорим, что больше не используем регистр, где записан tmp_rval
		free_rvalue(enc, &tmp_rval);
	}

	// возвращаем stack pointer в изначальное состояние
	if (params_amount >= 1)
	{
		to_code_2R_I(enc->sx->io, IC_RISCV_ADDI, R_SP, R_SP, (item_t)displ_for_parameters);
	}
	return ret;
}

/**
 *	Emit member expression
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Rvalue of member expression
 */
static rvalue emit_member_expression(encoder *const enc, const node *const nd)
{
	(void)enc;
	(void)nd;
	// FIXME: возврат структуры из функции. Указателя тут оказаться не может
	return RVALUE_VOID;
}

/**
 *	Emit cast expression
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Rvalue of cast expression
 */
static rvalue emit_cast_expression(encoder *const enc, const node *const nd)
{
	const node operand = expression_cast_get_operand(nd);

	const rvalue value = emit_expression(enc, &operand);

	const item_t target_type = expression_get_type(nd);
	const item_t source_type = expression_get_type(&operand);

	if (type_is_integer(enc->sx, source_type) && type_is_floating(enc->sx, target_type))
	{
		// int -> float
		const rvalue result = { .kind = RVALUE_KIND_REGISTER,
								.from_lvalue = !FROM_LVALUE,
								.val.reg_num = get_float_register(enc),
								.type = target_type };

		// FIXME: избавится от to_code функций
		//to_code_2R(enc->sx->io, IC_RISCV_MFC_1, value.val.reg_num, result.val.reg_num);
		//to_code_2R(enc->sx->io, IC_RISCV_CVT_S_W, result.val.reg_num, result.val.reg_num);

		to_code_2R(enc->sx->io, IC_RISCV_FCVT_D_W, result.val.reg_num, value.val.reg_num);

		free_rvalue(enc, &value);
		return result;
	}
	else
	{
		// char -> int пока не поддержано в билдере
		return (rvalue){
			.from_lvalue = value.from_lvalue, .kind = value.kind, .val.int_val = value.val.int_val, .type = TYPE_INTEGER
		};
	}
}

/**
 *	Emit increment or decrement expression
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Rvalue of the result of increment or decrement expression
 */
static rvalue emit_increment_expression(encoder *const enc, const node *const nd)
{
	const node operand = expression_unary_get_operand(nd);
	const lvalue operand_lvalue = emit_lvalue(enc, &operand);
	const rvalue operand_rvalue = emit_load_of_lvalue(enc, &operand_lvalue);

	const unary_t operator= expression_unary_get_operator(nd);
	const bool is_prefix = (operator== UN_PREDEC) || (operator== UN_PREINC);
	const rvalue imm_rvalue = { .from_lvalue = !FROM_LVALUE,
								.kind = RVALUE_KIND_CONST,
								.val.int_val = ((operator== UN_PREINC) || (operator== UN_POSTINC)) ? 1 : -1,
								.type = TYPE_INTEGER };

	if (is_prefix)
	{
		emit_binary_operation(enc, &operand_rvalue, &operand_rvalue, &imm_rvalue, BIN_ADD);
		emit_store_of_rvalue(enc, &operand_lvalue, &operand_rvalue);
	}
	else
	{
		const rvalue post_result_rvalue = { .from_lvalue = !FROM_LVALUE,
											.kind = RVALUE_KIND_REGISTER,
											.val.reg_num = get_register(enc),
											.type = operand_lvalue.type };

		emit_binary_operation(enc, &post_result_rvalue, &operand_rvalue, &imm_rvalue, BIN_ADD);
		emit_store_of_rvalue(enc, &operand_lvalue, &post_result_rvalue);
		free_rvalue(enc, &post_result_rvalue);
	}

	return operand_rvalue;
}

/**
 *	Emit unary expression
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Rvalue of the result of unary expression
 */
static rvalue emit_unary_expression(encoder *const enc, const node *const nd)
{
	const unary_t operator= expression_unary_get_operator(nd);
	assert(operator!= UN_INDIRECTION);

	switch (operator)
	{
		case UN_POSTINC:
		case UN_POSTDEC:
		case UN_PREINC:
		case UN_PREDEC:
			return emit_increment_expression(enc, nd);

		case UN_MINUS:
		case UN_NOT:
		{
			const node operand = expression_unary_get_operand(nd);
			const rvalue operand_rvalue = emit_expression(enc, &operand);
			const binary_t instruction = (operator== UN_MINUS) ? BIN_MUL : BIN_XOR;

			emit_binary_operation(enc, &operand_rvalue, &operand_rvalue, &RVALUE_NEGATIVE_ONE, instruction);
			return operand_rvalue;
		}

		case UN_LOGNOT:
		{
			const node operand = expression_unary_get_operand(nd);
			const rvalue value = emit_expression(enc, &operand);

			to_code_2R_I(enc->sx->io, IC_RISCV_SLTIU, value.val.reg_num, value.val.reg_num, 1);
			return value;
		}

		case UN_ABS:
		{
			const node operand = expression_unary_get_operand(nd);
			const rvalue operand_rvalue = emit_expression(enc, &operand);
			// TODO: use something else for integer abs
			const riscv_instruction_t instruction =
				type_is_floating(enc->sx, operand_rvalue.type) ? IC_RISCV_FABS : IC_RISCV_ABS;

			to_code_2R(enc->sx->io, instruction, operand_rvalue.val.reg_num, operand_rvalue.val.reg_num);
			return operand_rvalue;
		}

		case UN_ADDRESS:
		{
			const node operand = expression_unary_get_operand(nd);
			const lvalue operand_lvalue = emit_lvalue(enc, &operand);

			assert(operand_lvalue.kind != LVALUE_KIND_REGISTER);

			const rvalue result_rvalue = { .from_lvalue = !FROM_LVALUE,
										   .kind = RVALUE_KIND_REGISTER,
										   .val.reg_num = get_register(enc),
										   .type = TYPE_INTEGER };

			to_code_2R_I(enc->sx->io, IC_RISCV_ADDI, result_rvalue.val.reg_num, operand_lvalue.base_reg,
						 operand_lvalue.loc.displ);
			return result_rvalue;
		}

		case UN_UPB:
		{
			const node operand = expression_unary_get_operand(nd);
			const rvalue array_address = emit_expression(enc, &operand);
			const lvalue size_lvalue = { .base_reg = array_address.val.reg_num,
										 .kind = LVALUE_KIND_STACK,
										 .loc.displ = WORD_LENGTH,
										 .type = TYPE_INTEGER };
			return emit_load_of_lvalue(enc, &size_lvalue);
		}

		default:
			system_error(node_unexpected);
			return RVALUE_VOID;
	}
}

static bool is_binary_cond_operator(binary_t operator)
{
	return operator== BIN_LE || operator== BIN_LT || operator== BIN_GE || operator== BIN_GT || operator== BIN_NE ||
	operator== BIN_EQ;
}

/**
 *	Emit binary expression
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Rvalue of the result of binary expression
 */
static rvalue emit_binary_expression(encoder *const enc, const node *const nd)
{
	const binary_t operator= expression_binary_get_operator(nd);
	const node LHS = expression_binary_get_LHS(nd);
	const node RHS = expression_binary_get_RHS(nd);

	const label old_label_if_true = enc->label_if_true;
	const label old_label_if_false = enc->label_if_false;

	switch (operator)
	{
		case BIN_COMMA:
		{
			emit_void_expression(enc, &LHS);

			enc->label_if_true = old_label_if_true;
			enc->label_if_false = old_label_if_false;

			return emit_expression(enc, &RHS);
		}
		case BIN_LOG_OR:
		{
			const item_t curr_label_num = enc->label_num++;
			const label label_end = { .kind = L_END, .num = (size_t)curr_label_num };

			enc->label_if_true = old_label_if_true;
			enc->label_if_false = label_end;

			const rvalue lhs_rvalue = emit_expression(enc, &LHS);

			emit_label_declaration(enc, &label_end);

			free_rvalue(enc, &lhs_rvalue);

			enc->label_if_true = old_label_if_true;
			enc->label_if_false = old_label_if_false;

			const rvalue rhs_rvalue = emit_expression(enc, &RHS);
			//assert(lhs_rvalue.val.reg_num == rhs_rvalue.val.reg_num);

			enc->label_if_true = old_label_if_true;
			enc->label_if_false = old_label_if_false;

			return rhs_rvalue;
		}
		case BIN_LOG_AND:
		{
			const item_t curr_label_num = enc->label_num++;
			const label label_end = { .kind = L_END, .num = (size_t)curr_label_num };

			enc->label_if_true = label_end;
			enc->label_if_false = old_label_if_false;

			const rvalue lhs_rvalue = emit_expression(enc, &LHS);

			emit_label_declaration(enc, &label_end);

			free_rvalue(enc, &lhs_rvalue);

			enc->label_if_true = old_label_if_true;
			enc->label_if_false = old_label_if_false;

			const rvalue rhs_rvalue = emit_expression(enc, &RHS);
			//assert(lhs_rvalue.val.reg_num == rhs_rvalue.val.reg_num);

			enc->label_if_true = old_label_if_true;
			enc->label_if_false = old_label_if_false;

			return rhs_rvalue;
		}

		default:
		{
			const rvalue lhs_rvalue = emit_expression(enc, &LHS);
			const rvalue rhs_rvalue = emit_expression(enc, &RHS);
			rvalue assign_val = lhs_rvalue;
			bool clear_lhs = false, clear_rhs = true;
			// for assignment var = const + var;
			if (lhs_rvalue.kind == RVALUE_KIND_CONST)
			{
				assign_val = rhs_rvalue;
				clear_lhs = true;
				clear_rhs = false;
			}
			if (is_binary_cond_operator(operator))
			{
				assign_val.kind = RVALUE_KIND_REGISTER;
				assign_val.val.reg_num = get_register(enc);
				assign_val.type = TYPE_INTEGER;
				assign_val.from_lvalue = false;
				clear_lhs = clear_rhs = true;
			}
			//uni_printf(enc->sx->io, "AAAAA\n");
			//printf("%llu", enc->sx->cur_id);
			//printf("%i\n", enc->label_if_true.kind);
			emit_binary_operation(enc, &assign_val, &lhs_rvalue, &rhs_rvalue, operator);
			//uni_printf(enc->sx->io, "AAAAA\n");
			enc->label_if_true = old_label_if_true;
			enc->label_if_false = old_label_if_false;
			if (clear_lhs)
				free_rvalue(enc, &lhs_rvalue);
			if (clear_rhs)
				free_rvalue(enc, &rhs_rvalue);
			return assign_val;
		}
	}
}

/**
 *	Emit ternary expression
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Rvalue of the result of ternary expression
 */
static rvalue emit_ternary_expression(encoder *const enc, const node *const nd)
{
	const label old_label_if_true = enc->label_if_true;
	const label old_label_if_false = enc->label_if_false;

	const size_t label_num = enc->label_num++;
	const label label_end = { .kind = L_END, .num = label_num };
	const label label_else = { .kind = L_ELSE, .num = label_num };
	const label label_body = { .kind = L_THEN, .num = label_num };

	enc->label_if_true = label_body;
	enc->label_if_false = label_else;

	// const node condition = expression_ternary_get_condition(nd);
	// const rvalue value = emit_expression(enc, &condition);

	const rvalue result = { .kind = RVALUE_KIND_REGISTER,
							.val.reg_num = get_register(enc),
							.from_lvalue = !FROM_LVALUE,
							.type = expression_get_type(nd) };

	emit_label_declaration(enc, &label_body);

	const node LHS = expression_ternary_get_LHS(nd);
	const rvalue LHS_rvalue = emit_expression(enc, &LHS);
	emit_move_rvalue_to_register(enc, result.val.reg_num, &LHS_rvalue);
	free_rvalue(enc, &LHS_rvalue);

	emit_unconditional_branch(enc, IC_RISCV_J, &label_end);
	emit_label_declaration(enc, &label_else);

	const node RHS = expression_ternary_get_RHS(nd);
	const rvalue RHS_rvalue = emit_expression(enc, &RHS);
	emit_move_rvalue_to_register(enc, result.val.reg_num, &RHS_rvalue);
	free_rvalue(enc, &RHS_rvalue);

	emit_label_declaration(enc, &label_end);

	enc->label_if_true = old_label_if_true;
	enc->label_if_false = old_label_if_false;

	return result;
}

/**
 *	Emit structure assignment
 *
 *	@param	enc					Encoder
 *	@param	target				Target lvalue
 *	@param	value				Value to assign to target
 *
 *	@return	Rvalue of the result of assignment expression
 */
static rvalue emit_struct_assignment(encoder *const enc, const lvalue *const target, const node *const value)
{
	if (expression_get_class(value) == EXPR_INITIALIZER) // Присваивание списком
	{
		emit_structure_init(enc, target, value);
	}
	else // Присваивание другой структуры
	{
		// FIXME: возврат структуры из функции
		// FIXME: массив структур
		const size_t RHS_identifier = expression_identifier_get_id(value);
		const lvalue RHS_lvalue = displacements_get(enc, RHS_identifier);

		// Копирование всех данных из RHS
		const item_t type = expression_get_type(value);
		const size_t struct_size = riscv_type_size(enc->sx, type);
		for (size_t i = 0; i < struct_size; i += WORD_LENGTH)
		{
			// Грузим данные из RHS
			const lvalue value_word = { .base_reg = RHS_lvalue.base_reg,
										.loc.displ = RHS_lvalue.loc.displ + i,
										.kind = LVALUE_KIND_STACK,
										.type = TYPE_INTEGER };
			const rvalue proxy = emit_load_of_lvalue(enc, &value_word);

			// Отправляем их в variable
			const lvalue target_word = { .base_reg = target->base_reg,
										 .kind = LVALUE_KIND_STACK,
										 .loc.displ = target->loc.displ + i,
										 .type = TYPE_INTEGER };
			emit_store_of_rvalue(enc, &target_word, &proxy);

			free_rvalue(enc, &proxy);
		}
	}

	return emit_load_of_lvalue(enc, target);
}

/**
 *	Emit assignment expression
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Rvalue of the result of assignment expression
 */
static rvalue emit_assignment_expression(encoder *const enc, const node *const nd)
{
	const node LHS = expression_assignment_get_LHS(nd);
	const lvalue target = emit_lvalue(enc, &LHS);

	const node RHS = expression_assignment_get_RHS(nd);
	const item_t RHS_type = expression_get_type(&RHS);
	if (type_is_structure(enc->sx, RHS_type))
	{
		return emit_struct_assignment(enc, &target, &RHS);
	}

	const rvalue value = emit_expression(enc, &RHS);

	const binary_t operator= expression_assignment_get_operator(nd);
	if (operator== BIN_ASSIGN)
	{
		emit_store_of_rvalue(enc, &target, &value);
		return value;
	}

	// это "+=", "-=" и т.п.
	const rvalue target_value = emit_load_of_lvalue(enc, &target);
	binary_t correct_operation;
	switch (operator)
	{
		case BIN_ADD_ASSIGN:
			correct_operation = BIN_ADD;
			break;

		case BIN_SUB_ASSIGN:
			correct_operation = BIN_SUB;
			break;

		case BIN_MUL_ASSIGN:
			correct_operation = BIN_MUL;
			break;

		case BIN_DIV_ASSIGN:
			correct_operation = BIN_DIV;
			break;

		case BIN_SHL_ASSIGN:
			correct_operation = BIN_SHL;
			break;

		case BIN_SHR_ASSIGN:
			correct_operation = BIN_SHR;
			break;

		case BIN_AND_ASSIGN:
			correct_operation = BIN_AND;
			break;

		case BIN_XOR_ASSIGN:
			correct_operation = BIN_XOR;
			break;

		case BIN_OR_ASSIGN:
			correct_operation = BIN_OR;
			break;

		default:
			system_error(node_unexpected);
			return RVALUE_VOID;
	}
	emit_binary_operation(enc, &target_value, &target_value, &value, correct_operation);
	free_rvalue(enc, &value);

	emit_store_of_rvalue(enc, &target, &target_value);
	return target_value;
}

/**
 *	Emit inline expression
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Rvalue of inline expression
 */
// TODO: текущая ветка от feature, а туда inline_expression'ы пока не влили
/* static rvalue emit_inline_expression(encoder *const enc, const node *const nd)
{
	// FIXME: inline expression cannot return value at the moment
	const size_t amount = expression_inline_get_size(nd);

	for (size_t i = 0; i < amount; i++)
	{
		const node substmt = expression_inline_get_substmt(nd, i);
		emit_statement(enc, &substmt);
	}

	return RVALUE_VOID;
} */

/**
 *	Emit expression
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Rvalue of the expression
 */
//bool emit_literal = true;

static rvalue emit_expression(encoder *const enc, const node *const nd)
{
	if (expression_is_lvalue(nd))
	{
		const lvalue lval = emit_lvalue(enc, nd);
		uni_printf(enc->sx->io, "\t#7777777777\n");
		return emit_load_of_lvalue(enc, &lval);
	}
	// Иначе rvalue:
	switch (expression_get_class(nd))
	{
		case EXPR_LITERAL:
			//uni_printf(enc->sx->io, "\t#555555\n");
			//emit_literal = true;
			//if (emit_literal)
			return emit_literal_expression(enc, nd);
			//else
				//return RVALUE_VOID;

		case EXPR_CALL:
			//uni_printf(enc->sx->io, "\t#!!!!!!!!!\n");
			return emit_call_expression(enc, nd);

		case EXPR_MEMBER:
			return emit_member_expression(enc, nd);

		case EXPR_CAST:

			return emit_cast_expression(enc, nd);

		case EXPR_UNARY:
			return emit_unary_expression(enc, nd);

		case EXPR_BINARY:
			//uni_printf(enc->sx->io, "\tAAAAAAAAAAA\n");
			return emit_binary_expression(enc, nd);

		case EXPR_ASSIGNMENT:
			return emit_assignment_expression(enc, nd);

		case EXPR_TERNARY:
			return emit_ternary_expression(enc, nd);

			/*
			// TODO: текущая ветка от feature, а туда inline_expression'ы пока не влили
			case EXPR_INLINE:
				return emit_inline_expression(enc, nd);
			*/

		case EXPR_INITIALIZER:
			system_error(node_unexpected);
			return RVALUE_VOID;
		//LVE
		case EXPR_EMPTY_BOUND:
			system_error(node_unexpected);
			return RVALUE_VOID;

		default:
			system_error(node_unexpected);
			return RVALUE_VOID;
	}
}

/**
 *	Emit expression which will be evaluated as a void expression
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 *
 *	@return	Rvalue of void type
 */
static rvalue emit_void_expression(encoder *const enc, const node *const nd)
{
	if (expression_is_lvalue(nd))
	{
		emit_lvalue(enc, nd); // Либо регистровая переменная, либо на стеке => ничего освобождать не надо
	}
	else
	{
		const rvalue result = emit_expression(enc, nd);
		free_rvalue(enc, &result);
	}
	return RVALUE_VOID;
}


/*
 *	 _____     ______     ______     __         ______     ______     ______     ______   __     ______     __   __
 *______
 *	/\  __-.  /\  ___\   /\  ___\   /\ \       /\  __ \   /\  == \   /\  __ \   /\__  _\ /\ \   /\  __
 *\   /\ "-.\ \   /\  ___\
 *	\ \ \/\ \ \ \  __\   \ \ \____  \ \ \____  \ \  __ \  \ \  __<   \ \  __ \  \/_/\ \/ \ \ \  \ \ \/\ \  \ \ \-.
 *\  \ \___  \
 *	 \ \____-
 *\ \_____\  \ \_____\  \ \_____\  \ \_\ \_\  \ \_\ \_\  \ \_\ \_\    \ \_\  \ \_\  \ \_____\  \ \_\\"\_\  \/\_____\
 *	  \/____/   \/_____/   \/_____/   \/_____/   \/_/\/_/   \/_/ /_/   \/_/\/_/     \/_/   \/_/   \/_____/   \/_/ \/_/
 *\/_____/
 */


static void emit_array_init(encoder *const enc, const node *const nd, const size_t dimension, const node *const init,
							const rvalue *const addr)
{
	const size_t amount = expression_initializer_get_size(init);
	// Проверка на соответствие размеров массива и инициализатора
	uni_printf(enc->sx->io, "\n\t# Check for array and initializer sizes equality:\n");

	const node bound = declaration_variable_get_bound(nd, dimension);
	//uni_printf(enc->sx->io, "!!!!!!!!\t");
	const rvalue tmp = emit_expression(enc, &bound);
	const rvalue bound_rvalue = (tmp.kind == RVALUE_KIND_REGISTER) ? tmp : emit_load_of_immediate(enc, &tmp);
	//uni_printf(enc->sx->io, "!!!!!!!\t");
	// FIXME: через emit_binary_operation()
	uni_printf(enc->sx->io, "\t");
	instruction_to_io(enc->sx->io, IC_RISCV_ADDI);
	uni_printf(enc->sx->io, " ");
	rvalue_to_io(enc, &bound_rvalue);
	uni_printf(enc->sx->io, ", ");
	rvalue_to_io(enc, &bound_rvalue);
	//printf("%i ", tmp.val.int_val);
	
	//uni_printf(enc->sx->io, "\n!!!!!!!!!1\n");
	uni_printf(enc->sx->io, ", %" PRIitem "\n", -(item_t)amount);
	//uni_printf(enc->sx->io, "\n!!!!!!!!!1\n");

	uni_printf(enc->sx->io, "\t");
	//instruction_to_io(enc->sx->io, IC_RISCV_BNE);
	//uni_printf(enc->sx->io, " ");
	//rvalue_to_io(enc, &bound_rvalue);
	//uni_printf(enc->sx->io, ", ");
	//riscv_register_to_io(enc->sx->io, R_ZERO);
	//uni_printf(enc->sx->io, ", error\n"); // FIXME: error согласно RUNTIME'му

	free_rvalue(enc, &bound_rvalue);

	for (size_t i = 0; i < amount; i++)
	{
		//uni_printf(enc->sx->io, "\t#IIIII\n");
		const node subexpr = expression_initializer_get_subexpr(init, i);
		uni_printf(enc->sx->io, "\n");
		if (expression_get_class(&subexpr) == EXPR_INITIALIZER)
		{
			if (null_registers)
				enc->registers[0] = NULL;
			else
				null_registers = true;
			// Сдвиг адреса на размер массива + 1 (за размер следующего измерения)
			const riscv_register_t reg = get_register(enc);
			// FIXME: создать отдельные rvalue и lvalue и через emit_load_of_lvalue()
			to_code_R_I_R(enc->sx->io, IC_RISCV_LW, R_T1, 0, addr->val.reg_num); // адрес следующего измерения

			const rvalue next_addr = {
				.from_lvalue = !FROM_LVALUE, .kind = RVALUE_KIND_REGISTER, .val.reg_num = reg, .type = TYPE_INTEGER
			};

			emit_array_init(enc, nd, dimension + 1, &subexpr, &next_addr);

			// Сдвиг адреса
			bool riscv_saved_zero_reg = enc->registers[0];
			enc->registers[0] = NULL;
			to_code_2R_I(enc->sx->io, IC_RISCV_ADDI, addr->val.reg_num, addr->val.reg_num, -(item_t)WORD_LENGTH);
			enc->registers[0] = riscv_saved_zero_reg;
			uni_printf(enc->sx->io, "\n");
			free_register(enc, reg);


		}
		else
		{
			//bool save_first_reg = enc->registers[0];
			//enc->registers[0] = NULL;
			const rvalue subexpr_value = emit_expression(enc, &subexpr); // rvalue элемента
			
			const lvalue array_index_value = {
				.base_reg = addr->val.reg_num, .loc.displ = 0, .kind = LVALUE_KIND_STACK, .type = TYPE_INTEGER
			};
			emit_store_of_rvalue(enc, &array_index_value, &subexpr_value);
			lock_register(enc, addr->val.reg_num);
			//uni_printf(enc->sx->io, "\t#!!!!!!\n");
			if (i != amount - 1)
			{
				to_code_2R_I(enc->sx->io, IC_RISCV_ADDI, addr->val.reg_num, addr->val.reg_num, -4);
			}
			free_rvalue(enc, &subexpr_value);
			//enc->registers[0] = save_first_reg;
		}
		//uni_printf(enc->sx->io, "\t#JJJJJJJJ\n");
	}
}

/**
 *	Emit bound expression in array declaration
 *
 *	@param	enc					Encoder
 *	@param	bound				Bound node
 *	@param	nd					Declaration node
 *
 *	@return	Bound expression
 */
static rvalue emit_bound(encoder *const enc, const node *const bound, const node *const nd)
{
	// Если границы у массива не проставлены -- смотрим на инициализатор, чтобы их узнать
	node dim_size = *bound;
	const size_t dim = declaration_variable_get_bounds_amount(nd);
	if (expression_get_class(bound) == EXPR_EMPTY_BOUND)
	{
		const node init = declaration_variable_get_initializer(nd);
		dim_size = expression_initializer_get_subexpr(&init, dim - 1);
		const size_t size = expression_initializer_get_size(&init);
		const rvalue result = {
			.from_lvalue = !FROM_LVALUE, .kind = RVALUE_KIND_CONST, .val.int_val = (item_t)size, .type = TYPE_INTEGER
		};
		return emit_load_of_immediate(enc, &result);
	}
	return emit_expression(enc, &dim_size);
}

/**
 *	Emit array declaration
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */
//
//int prev_size = 1;
//int prev_declaration_size = 1;

int link_on_true_location[1000];
static void emit_array_declaration(encoder *const enc, const node *const nd)
{
	//printf("\n%i\n", nd->index);
	const size_t identifier = declaration_variable_get_id(nd);
	const bool has_init = declaration_variable_has_initializer(nd);

	const node bound = declaration_variable_get_bound(nd, 0);
	//uni_printf(enc->sx->io, "\n!!!!!!!!\n\t");
	const rvalue declaration_size = emit_expression(enc, &bound);

	//printf("%i!", tmp2.val.int_val);
	// Сдвигаем, чтобы размер первого измерения был перед массивом
	to_code_2R_I(enc->sx->io, IC_RISCV_ADDI, R_SP, R_SP, -4);
	lvalue variable = displacements_add(enc, identifier, false);
	const rvalue value = { .from_lvalue = !FROM_LVALUE,
						   .kind = RVALUE_KIND_REGISTER,
						   .val.reg_num = get_register(enc),
						   .type = TYPE_INTEGER };
	to_code_2R(enc->sx->io, IC_RISCV_MOVE, value.val.reg_num, R_SP);
	int save_old_displ = variable.loc.displ;
	//printf("%i", variable.loc.displ - 4 * prev_declaration_size + 4);
	variable.loc.displ = variable.loc.displ - 4 * prev_declaration_size + 4;
	link_on_true_location[-save_old_displ] = variable.loc.displ;
	const lvalue target = {
		.kind = variable.kind, .type = TYPE_INTEGER, .loc = variable.loc, .base_reg = variable.base_reg
	};
	//uni_printf(enc->sx->io, "!!!!!!!!!\n");
	//uni_printf(enc->sx->io, "%" PRIitem, value.val.int_val);
	//uni_printf(enc->sx->io, "sw t0, -%i(fp)", value.val.int_val);
	//emit_store_of_rvalue(enc, &target, &value);

	const node init = declaration_variable_get_initializer(nd);
	const size_t amount = expression_initializer_get_size(&init);
	//uni_printf(enc->sx->io, "\n!!!!!!!!\n\t");
	//const size_t amount_of_array = 
	uni_printf(enc->sx->io, "\tsw t0, ");
	uni_printf(enc->sx->io, "%" PRIitem "(", target.loc.displ);
	//printf("!!!!!!%i!!!!!", target.loc.displ);
	
	//На target.loc.displ - 4 * prev_size + 4  - начало массива, в array_sizes храним его размер
	array_sizes[-(target.loc.displ)] = amount;
	array_declaration_sizes[-(target.loc.displ)] = declaration_size.val.int_val;
	//printf("%i  %i\n", target.loc.displ - 4 * prev_size + 4, array_sizes[-(target.loc.displ - 4 * prev_size + 4)]);


	//prev_size = amount;
	riscv_register_to_io(enc->sx->io, target.base_reg);
	uni_printf(enc->sx->io, ")\n");

	//uni_printf(enc->sx->io, "!!!!!!!!!\n");
	free_rvalue(enc, &value);


	

	//// FIXME: Переделать регистры-аргументы
	//to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_S0, R_A0);
	//to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_S1, R_A1);
	//to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_S2, R_A2);
	//to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_S3, R_A3);

	//// Загрузка адреса в a0
	//to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_A0, R_SP);


	// Загрузка размера массива в a1
	const node dim_size = declaration_variable_get_bound(nd, 0);
	const rvalue tmp = emit_expression(enc, &dim_size);
	const rvalue second_arg_rvalue = (tmp.kind == RVALUE_KIND_CONST) ? emit_load_of_immediate(enc, &tmp) : tmp;
	emit_move_rvalue_to_register(enc, R_A1, &second_arg_rvalue);
	free_rvalue(enc, &second_arg_rvalue);

	const size_t dim = declaration_variable_get_bounds_amount(nd);
	//printf("%llu\n", dim);
	if (dim >= 2)
	{
		// Предварительно загрузим в a2 и a3 адрес первого элемента и размер соответственно
		to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_A2, R_A0);
		to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_A3, R_A1);
	}

	//uni_printf(enc->sx->io, "\tjal DEFARR1\n");

	for (size_t j = 1; j < dim; j++)
	{
		// Загрузка адреса в a0
		//to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_A0, R_A0);
		//// Загрузка размера массива в a1
		//const node try_dim_size = declaration_variable_get_bound(nd, j);
		//const rvalue bound = emit_bound(enc, &try_dim_size, nd);
		//emit_move_rvalue_to_register(enc, R_A1, &bound);
		//free_rvalue(enc, &bound);

		//to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_S5, R_A0);
		//to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_S6, R_A1);

		//uni_printf(enc->sx->io, "\tjal DEFARR2\n");

		if (j != dim - 1)
		{
			// Предварительно загрузим в a2 и a3 адрес первого элемента и размер соответственно
			to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_A2, R_T5);
			to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_A3, R_T6);
		}
	}

	if (has_init)
	{
		uni_printf(enc->sx->io, "\n");

		//uni_printf(enc->sx->io, "!!!!!!!!!\n");

		const rvalue tmp = { .kind = RVALUE_KIND_CONST, .val.int_val = variable.loc.displ, .type = TYPE_INTEGER };

		const node init = declaration_variable_get_initializer(nd);
		//uni_printf(enc->sx->io, "\tli t0, ");
		//uni_printf(enc->sx->io, "%" PRIitem, (tmp.val.int_val - 4 * prev_declaration_size + 4));


		//uni_printf(enc->sx->io, "\t#");
		const rvalue variable_value = emit_load_of_lvalue(enc, &variable);
		emit_array_init(enc, nd, 0, &init, &variable_value);
		//uni_printf(enc->sx->io, "!!!!!!!!!\n");
		//uni_printf(enc->sx->io, "\n\tadd t0, t0, fp\n");

		free_rvalue(enc, &variable_value);
	}
	prev_size = amount;
	prev_declaration_size = declaration_size.val.int_val;
	//to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_SP, R_A0);

	//to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_A0, R_S0);
	//to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_A1, R_S1);
	//to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_A2, R_S2);
	//to_code_2R(enc->sx->io, IC_RISCV_MOVE, R_A3, R_S3);
}

/**
 *	Emit struct initialization
 *
 *	@param	enc					Encoder
 *	@param	target				Lvalue to initialize
 *	@param	initializer			Initializer node in AST
 */
static void emit_structure_init(encoder *const enc, const lvalue *const target, const node *const initializer)
{
	assert(type_is_structure(enc->sx, target->type));

	size_t displ = 0;

	const size_t amount = type_structure_get_member_amount(enc->sx, target->type);
	for (size_t i = 0; i < amount; i++)
	{
		const item_t type = type_structure_get_member_type(enc->sx, target->type, i);
		const lvalue member_lvalue = {
			.base_reg = target->base_reg, .kind = target->kind, .loc.displ = target->loc.displ + displ, .type = type
		};
		displ += riscv_type_size(enc->sx, type);

		const node subexpr = expression_initializer_get_subexpr(initializer, i);
		if (expression_get_class(&subexpr) == EXPR_INITIALIZER)
		{
			emit_structure_init(enc, &member_lvalue, &subexpr);
			continue;
		}

		if (type_is_structure(enc->sx, expression_get_type(&subexpr)))
		{
			emit_struct_assignment(enc, &member_lvalue, &subexpr);
		}
		else
		{
			const rvalue subexpr_rvalue = emit_expression(enc, &subexpr);
			emit_store_of_rvalue(enc, &member_lvalue, &subexpr_rvalue);

			free_rvalue(enc, &subexpr_rvalue);
		}
	}
}

/**
 *	Emit variable declaration
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */
static void emit_variable_declaration(encoder *const enc, const node *const nd)
{
	is_declaration = true;
	const size_t identifier = declaration_variable_get_id(nd);
	uni_printf(enc->sx->io, "\t# \"%s\" variable declaration:\n", ident_get_spelling(enc->sx, identifier));

	const item_t type = ident_get_type(enc->sx, identifier);
	if (type_is_array(enc->sx, type))
	{
		emit_array_declaration(enc, nd);
	}
	else
	{

		const lvalue variable = displacements_add(enc, identifier, false);
		if (declaration_variable_has_initializer(nd))
		{
			const node initializer = declaration_variable_get_initializer(nd);

			if (type_is_structure(enc->sx, type))
			{
				const rvalue tmp = emit_struct_assignment(enc, &variable, &initializer);
				free_rvalue(enc, &tmp);
			}
			else
			{
				const rvalue value = emit_expression(enc, &initializer);
				emit_store_of_rvalue(enc, &variable, &value);
				free_rvalue(enc, &value);
			}
		}
		prev_size = 1;
		is_declaration = false; 
	}
}

/**
 *	Emit function definition
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */
static void emit_function_definition(encoder *const enc, const node *const nd)
{
	const size_t ref_ident = declaration_function_get_id(nd);
	const label func_label = { .kind = L_FUNC, .num = ref_ident };
	emit_label_declaration(enc, &func_label);

	const item_t func_type = ident_get_type(enc->sx, ref_ident);
	const size_t parameters = type_function_get_parameter_amount(enc->sx, func_type);

	if (ref_ident == enc->sx->ref_main)
	{
		// FIXME: пока тут будут две метки для функции main
		uni_printf(enc->sx->io, "main:\n");
	}

	uni_printf(enc->sx->io, "\t# \"%s\" function:\n", ident_get_spelling(enc->sx, ref_ident));

	enc->curr_function_ident = ref_ident;
	enc->max_displ = 0;
	enc->scope_displ = 0;

	// Сохранение оберегаемых регистров перед началом работы функции
	// FIXME: избавиться от функций to_code
	uni_printf(enc->sx->io, "\n\t# preserved registers:\n");
	to_code_R_I_R(enc->sx->io, IC_RISCV_SW, R_RA, -(item_t)RA_SIZE, R_SP);
	to_code_R_I_R(enc->sx->io, IC_RISCV_SW, R_FP, -(item_t)(RA_SIZE + SP_SIZE), R_SP);

	// Сохранение s0-s11
	for (size_t i = 0; i < PRESERVED_REG_AMOUNT; i++)
	{
		to_code_R_I_R(enc->sx->io, IC_RISCV_SW, R_S0 + i, -(item_t)(RA_SIZE + SP_SIZE + (i + 1) * WORD_LENGTH), R_SP);
	}

	uni_printf(enc->sx->io, "\n");

	// Сохранение fs0-fs11
	for (size_t i = 0; i < PRESERVED_FP_REG_AMOUNT; i++)
	{
		to_code_R_I_R(enc->sx->io, IC_RISCV_FSD, R_FS0 + i,
					  -(item_t)(RA_SIZE + SP_SIZE + (i + 1) * (WORD_LENGTH * 2) +
								PRESERVED_REG_AMOUNT * WORD_LENGTH /* за s0-s11 */),
					  R_SP);
	}

	// Выравнивание смещения на 8
	if (enc->max_displ % 8)
	{
		const size_t padding = 8 - (enc->max_displ % 8);
		enc->max_displ += padding;
		if (padding)
		{
			uni_printf(enc->sx->io, "\n\t# padding -- max displacement == %zu\n", enc->max_displ);
		}
	}

	// Создание буфера для тела функции
	universal_io *const old_io = enc->sx->io;
	universal_io new_io = io_create();
	out_set_buffer(&new_io, BUFFER_SIZE);
	enc->sx->io = &new_io;

	uni_printf(enc->sx->io, "\n\t# function parameters:\n");

	size_t register_arguments_amount = 0;
	size_t floating_register_arguments_amount = 0;

	for (size_t i = 0; i < parameters; i++)
	{
		const size_t id = declaration_function_get_parameter(nd, i);
		uni_printf(enc->sx->io, "\t# parameter \"%s\" ", ident_get_spelling(enc->sx, id));

		const bool argument_is_float = type_is_floating(enc->sx, ident_get_type(enc->sx, id));

		const bool argument_is_register = !argument_is_float ? register_arguments_amount < ARG_REG_AMOUNT
															 : floating_register_arguments_amount < ARG_REG_AMOUNT;

		if (argument_is_register)
		{
			// Рассматриваем их как регистровые переменные
			const item_t type = ident_get_type(enc->sx, id);
			const riscv_register_t curr_reg =
				argument_is_float ? R_FA0 + floating_register_arguments_amount++ : R_A0 + register_arguments_amount++;
			uni_printf(enc->sx->io, "is in register ");
			riscv_register_to_io(enc->sx->io, curr_reg);
			uni_printf(enc->sx->io, "\n");

			// Вносим переменную в таблицу символов
			const lvalue value = {
				.kind = LVALUE_KIND_REGISTER, .type = type, .loc.reg_num = curr_reg, .base_reg = R_FP
			};
			displacements_set(enc, id, &value);
		}
		else
		{
			const item_t type = ident_get_type(enc->sx, id);
			const size_t displ = i * WORD_LENGTH + FUNC_DISPL_PRESEREVED + WORD_LENGTH;
			uni_printf(enc->sx->io, "is on stack at offset %zu from fp\n", displ);

			const lvalue value = { .kind = LVALUE_KIND_STACK, .type = type, .loc.displ = displ, .base_reg = R_FP };
			displacements_set(enc, id, &value);
		}
	}

	uni_printf(enc->sx->io, "\n\t# function body:\n");
	node body = declaration_function_get_body(nd);
	emit_statement(enc, &body);

	// Извлечение буфера с телом функции в старый io
	char *buffer = out_extract_buffer(enc->sx->io);
	enc->sx->io = old_io;

	uni_printf(enc->sx->io, "\n\t# setting up fp:\n");
	// fp указывает на конец статики (которое в данный момент равно концу динамики)
	to_code_2R_I(enc->sx->io, IC_RISCV_ADDI, R_FP, R_SP, -(item_t)(FUNC_DISPL_PRESEREVED + WORD_LENGTH));

	uni_printf(enc->sx->io, "\n\t# setting up sp:\n");
	// sp указывает на конец динамики (которое в данный момент равно концу статики)
	// Смещаем sp ниже конца статики (чтобы он не совпадал с fp)
	to_code_2R_I(enc->sx->io, IC_RISCV_ADDI, R_SP, R_FP, -(item_t)(WORD_LENGTH + enc->max_displ));

	uni_printf(enc->sx->io, "%s", buffer);
	free(buffer);

	const label end_label = { .kind = L_FUNCEND, .num = ref_ident };
	emit_label_declaration(enc, &end_label);

	// Восстановление стека после работы функции
	uni_printf(enc->sx->io, "\n\t# data restoring:\n");

	// Ставим fp на его положение в предыдущей функции
	to_code_2R_I(enc->sx->io, IC_RISCV_ADDI, R_SP, R_FP, (item_t)(FUNC_DISPL_PRESEREVED + WORD_LENGTH));

	uni_printf(enc->sx->io, "\n");

	// Восстановление s0-s11
	for (size_t i = 0; i < PRESERVED_REG_AMOUNT; i++)
	{
		to_code_R_I_R(enc->sx->io, IC_RISCV_LW, R_S0 + i, -(item_t)(RA_SIZE + SP_SIZE + (i + 1) * WORD_LENGTH), R_SP);
	}

	uni_printf(enc->sx->io, "\n");

	// Восстановление fs0-fs11
	for (size_t i = 0; i < PRESERVED_FP_REG_AMOUNT; i++)
	{
		to_code_R_I_R(enc->sx->io, IC_RISCV_FLD, R_FS0 + i,
					  -(item_t)(RA_SIZE + SP_SIZE + (i + 1) * (WORD_LENGTH * 2) +
								/* за s0-s11 */ PRESERVED_REG_AMOUNT * WORD_LENGTH),
					  R_SP);
	}

	uni_printf(enc->sx->io, "\n");

	// Возвращаем sp его положение в предыдущей функции
	to_code_R_I_R(enc->sx->io, IC_RISCV_LW, R_FP, -(item_t)(RA_SIZE + SP_SIZE), R_SP);

	to_code_R_I_R(enc->sx->io, IC_RISCV_LW, R_RA, -(item_t)(RA_SIZE), R_SP);

	// Прыгаем далее
	emit_register_branch(enc, IC_RISCV_JR, R_RA);
}

static void emit_declaration(encoder *const enc, const node *const nd)
{
	switch (declaration_get_class(nd))
	{
		case DECL_VAR:
			emit_variable_declaration(enc, nd);
			break;

		case DECL_FUNC:
			emit_function_definition(enc, nd);
			break;

		default:
			// С объявлением типа ничего делать не нужно
			return;
	}

	uni_printf(enc->sx->io, "\n");
}


/*
 *	 ______     ______   ______     ______   ______     __    __     ______     __   __     ______   ______
 *	/\  ___\   /\__  _\ /\  __ \   /\__  _\ /\  ___\   /\ "-./  \   /\  ___\   /\ "-.\ \   /\__  _\ /\  ___\
 *	\ \___  \  \/_/\ \/ \ \  __ \  \/_/\ \/ \ \  __\   \ \ \-./\ \  \ \  __\   \ \ \-.  \  \/_/\ \/ \ \___  \
 *	 \/\_____\    \ \_\  \ \_\ \_\    \ \_\  \ \_____\  \ \_\ \ \_\  \ \_____\  \ \_\\"\_\    \ \_\  \/\_____\
 *	  \/_____/     \/_/   \/_/\/_/     \/_/   \/_____/   \/_/  \/_/   \/_____/   \/_/ \/_/     \/_/   \/_____/
 */


/**
 *	Emit declaration statement
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */
static void emit_declaration_statement(encoder *const enc, const node *const nd)
{
	const size_t size = statement_declaration_get_size(nd);
	for (size_t i = 0; i < size; i++)
	{
		const node decl = statement_declaration_get_declarator(nd, i);
		emit_declaration(enc, &decl);
	}
}

/**
 *	Emit case statement
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */
static void emit_case_statement(encoder *const enc, const node *const nd, const size_t label_num, int switch_counter)
{
	//const label label_case = { .kind = L_CASE, .num = label_num };
	uni_printf(enc->sx->io, "CASE");
	uni_printf(enc->sx->io, "%zu", label_num);
	//emit_label_declaration(enc, &label_case);
	uni_printf(enc->sx->io, "_");
	uni_printf(enc->sx->io, "%i", switch_counter);
	uni_printf(enc->sx->io, ":\n");

	const node substmt = statement_case_get_substmt(nd);
	emit_statement(enc, &substmt);
}

/**
 *	Emit default statement
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */
static void emit_default_statement(encoder *const enc, const node *const nd, const size_t label_num)
{
	const label label_default = { .kind = L_DEFAULT, .num = label_num };
	emit_label_declaration(enc, &label_default);

	const node substmt = statement_default_get_substmt(nd);
	emit_statement(enc, &substmt);
}

/**
 *	Emit compound statement
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */
static void emit_compound_statement(encoder *const enc, const node *const nd)
{
	const size_t scope_displacement = enc->scope_displ;

	const size_t size = statement_compound_get_size(nd);
	for (size_t i = 0; i < size; i++)
	{
		const node substmt = statement_compound_get_substmt(nd, i);
		emit_statement(enc, &substmt);
	}

	// for (int i = 0; i < nd->tree->size; i++)
	//	printf("%zx\n", nd->tree->array[i]);

	enc->max_displ = max(enc->scope_displ, enc->max_displ);
	enc->scope_displ = scope_displacement;
}

/**
 *	Emit if statement
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */

static void emit_if_statement(encoder *const enc, const node *const nd)
{
	const size_t label_num = enc->label_num++;
	const label label_then = { .kind = L_THEN, .num = label_num };
	const label label_else = { .kind = L_ELSE, .num = label_num };
	const label label_end = { .kind = L_END, .num = label_num };

	const label old_label_if_true = enc->label_if_true;
	const label old_label_if_false = enc->label_if_false;

	enc->label_if_true = label_then;
	enc->label_if_false = label_else;

	const node condition = statement_if_get_condition(nd);
	const rvalue value = emit_expression(enc, &condition);

	const bool has_else = statement_if_has_else_substmt(nd);
	free_rvalue(enc, &value);

	emit_label_declaration(enc, &label_then);

	const node then_substmt = statement_if_get_then_substmt(nd);
	emit_statement(enc, &then_substmt);
	emit_unconditional_branch(enc, IC_RISCV_J, &label_end);
	emit_label_declaration(enc, &label_else);
	if (has_else)
	{
		const node else_substmt = statement_if_get_else_substmt(nd);
		emit_statement(enc, &else_substmt);
	}

	emit_label_declaration(enc, &label_end);

	enc->label_if_true = old_label_if_true;
	enc->label_if_false = old_label_if_false;
}


/**
 *	Emit switch statement
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */
//int switch_counter = 0;

//enc->sx->cur_id++;
//enc->sx->cur_id--;
//nd->tree->size++;
//nd->tree->size--;
static void emit_switch_statement_old(encoder *const enc, const node *const nd)
{
	const size_t label_num = enc->label_num++;
	size_t curr_case_label_num = enc->case_label_num;

	const label old_label_if_true = enc->label_if_true;
	const label old_label_if_false = enc->label_if_false;

	const label old_label_break = enc->label_break;
	enc->label_break = (label){ .kind = L_END, .num = label_num };

	const node condition = statement_switch_get_condition(nd);
	//uni_printf(enc->sx->io, "!!!\n");
	const rvalue tmp_condtion = emit_expression(enc, &condition);
	//uni_printf(enc->sx->io, "!!!\n");
	const rvalue condition_rvalue =
		(tmp_condtion.kind == RVALUE_KIND_CONST) ? emit_load_of_immediate(enc, &tmp_condtion) : tmp_condtion;

	item_t default_index = -1;

	// Размещение меток согласно условиям
	const node body = statement_switch_get_body(nd);
	// printf("%zx\n", body.tree->size);
	const size_t amount = statement_compound_get_size(&body); // Гарантируется compound statement
	for (size_t i = 0; i < amount; i++)
	{
		const node substmt = statement_compound_get_substmt(&body, i);
		const item_t substmt_class = statement_get_class(&substmt);

		if (substmt_class == STMT_CASE)
		{
			const size_t case_num = enc->case_label_num++;
			const label label_case = { .kind = L_CASE, .num = case_num };
			const label label_case_condition = { .kind = L_CASE_CONDITION, .num = case_num };
			const label label_next_condition = { .kind = L_CASE_CONDITION, .num = case_num + 1 };
			emit_label_declaration(enc, &label_case_condition);

			const node case_expr = statement_case_get_expression(&substmt);
			const rvalue case_expr_rvalue = emit_literal_expression(enc, &case_expr);

			// Пользуемся тем, что это integer type
			const rvalue result_rvalue = { .from_lvalue = !FROM_LVALUE,
										   .kind = RVALUE_KIND_REGISTER,
										   .val.reg_num = get_register(enc),
										   .type = TYPE_INTEGER };
			enc->label_if_true = label_case;
			enc->label_if_false = label_next_condition;

			emit_binary_operation(enc, &result_rvalue, &condition_rvalue, &case_expr_rvalue, BIN_EQ);

			free_rvalue(enc, &result_rvalue);
		}
		else if (substmt_class == STMT_DEFAULT)
		{
			const label label_case_condition = { .kind = L_CASE_CONDITION, .num = enc->case_label_num };
			emit_label_declaration(enc, &label_case_condition);

			// Только получаем индекс, а размещение прыжка по нужной метке будет после всех case'ов
			default_index = enc->case_label_num++;
		}
	}

	if (default_index != -1)
	{
		const label label_default = { .kind = L_CASE, .num = (size_t)default_index };
		emit_unconditional_branch(enc, IC_RISCV_J, &label_default);
	}
	else
	{
		const label label_case_condition = { .kind = L_CASE_CONDITION, .num = enc->case_label_num++ };
		emit_label_declaration(enc, &label_case_condition);
		// Нет default => можем попасть в ситуацию, когда требуется пропустить все case'ы
		emit_unconditional_branch(enc, IC_RISCV_J, &enc->label_break);
	}

	free_rvalue(enc, &condition_rvalue);

	uni_printf(enc->sx->io, "\n");

	// Размещение тел всех case и default statements
	for (size_t i = 0; i < amount; i++)
	{
		const node substmt = statement_compound_get_substmt(&body, i);
		const item_t substmt_class = statement_get_class(&substmt);

		if (substmt_class == STMT_CASE)
		{
			//emit_case_statement(enc, &substmt, curr_case_label_num++);
			emit_case_statement(enc, &substmt, curr_case_label_num++, 0);
		}
		else if (substmt_class == STMT_DEFAULT)
		{
			emit_default_statement(enc, &substmt, curr_case_label_num++);
		}
		else
		{
			emit_statement(enc, &substmt);
		}
	}

	emit_label_declaration(enc, &enc->label_break);
	enc->label_break = old_label_break;

	enc->label_if_true = old_label_if_true;
	enc->label_if_false = old_label_if_false;
}

int switch_counter = 0;



static void emit_switch_statement(encoder *const enc, const node *const nd)
{
	switch_counter++;
	const size_t label_num = enc->label_num++;
	size_t curr_case_label_num = enc->case_label_num;

	const label old_label_if_true = enc->label_if_true;
	const label old_label_if_false = enc->label_if_false;

	const label old_label_break = enc->label_break;
	enc->label_break = (label){ .kind = L_END, .num = label_num };

	/*const node condition = statement_switch_get_condition(nd);
	const rvalue tmp_condtion = emit_expression(enc, &condition);
	const rvalue condition_rvalue =
		(tmp_condtion.kind == RVALUE_KIND_CONST) ? emit_load_of_immediate(enc, &tmp_condtion) : tmp_condtion;*/


	item_t default_index = -1;

	const node body = statement_switch_get_body(nd);
	const size_t amount = statement_compound_get_size(&body);

	//const rvalue case_expr_rvalue_tmp = emit_literal_expression(enc, &case_expr);
	int case_amount = 0;

	for (size_t i = 0; i < amount; i++)
	{
		//printf("%zu", i);
		const node substmt = statement_compound_get_substmt(&body, i);
		const item_t substmt_class = statement_get_class(&substmt);
		if (substmt_class == STMT_CASE)
		{
			case_amount++;
		}
		//printf("%i", case_amount);
	}

	uni_printf(enc->sx->io, "\t%s%i\n", "lw t0, ", case_amount);

	uni_printf(enc->sx->io, "\tslli t0, t0, 2\n"
	"\tli a7, 9\n"
	"\tmv a0, t0\n"
	"\tecall\n"
	"\tmv t1, a0\n");

	int case_counter_temp = 0;
	for (size_t i = 0; i < amount; i++)
	{
		const node substmt = statement_compound_get_substmt(&body, i);
		const item_t substmt_class = statement_get_class(&substmt);

		if (substmt_class == STMT_CASE)
		{
			case_counter_temp++;

			const size_t case_num = enc->case_label_num++;
			const label label_case = { .kind = L_CASE, .num = case_num };
			//const label label_case_condition = { .kind = L_CASE_CONDITION, .num = case_num };
			const label label_next_condition = { .kind = L_CASE_CONDITION, .num = case_num + 1 };
			//emit_label_declaration(enc, &label_case_condition);

			const node case_expr = statement_case_get_expression(&substmt);
			const rvalue case_expr_rvalue = emit_literal_expression(enc, &case_expr);

			// Пользуемся тем, что это integer type
			const rvalue result_rvalue = { .from_lvalue = !FROM_LVALUE,
										   .kind = RVALUE_KIND_REGISTER,
										   .val.reg_num = get_register(enc),
										   .type = TYPE_INTEGER };
			enc->label_if_true = label_case;
			enc->label_if_false = label_next_condition;
			//uni_printf(enc->sx->io, "!!!1\n");
			uni_printf(enc->sx->io, "%s%zu\n", "\tli t2 ", case_expr_rvalue.val.int_val);
			uni_printf(enc->sx->io, "%s%i%s%i\n", "\tli t3 CASE", case_counter_temp, "_", switch_counter);
			uni_printf(enc->sx->io, "%s%i\n", "\tcall CASE_INSERT_", switch_counter);
			//emit_binary_operation(enc, &result_rvalue, &condition_rvalue, &case_expr_rvalue, BIN_EQ);
			//uni_printf(enc->sx->io, "!!!2\n");

			free_rvalue(enc, &result_rvalue);
		}
		else if (substmt_class == STMT_DEFAULT)
		{
			//const label label_case_condition = { .kind = L_CASE_CONDITION, .num = enc->case_label_num };
			//emit_label_declaration(enc, &label_case_condition);

			// Только получаем индекс, а размещение прыжка по нужной метке будет после всех case'ов
			default_index = enc->case_label_num++;
		}
	}
	//const rvalue result_rvalue_tmp = { .from_lvalue = !FROM_LVALUE,
	//							   .kind = RVALUE_KIND_REGISTER,
	//							   .val.reg_num = get_register(enc),
	//							   .type = TYPE_INTEGER };

	const node condition = statement_switch_get_condition(nd); // понять, почему тут регистр не t1, а t0

	emit_expression(enc, &condition);
	//uni_printf(enc->sx->io, "\t%s%zu\n", "li t0, ", result_rvalue_tmp.val.int_val);
	uni_printf(enc->sx->io, "\t%s%i\n", "call CALL_CASE_CONDITION_", switch_counter);


	int case_counter = 0;
	for (size_t i = 0; i < amount; i++)
	{
		const node substmt = statement_compound_get_substmt(&body, i);
		const item_t substmt_class = statement_get_class(&substmt);

		if (substmt_class == STMT_CASE)
		{
			case_counter++;
			emit_case_statement(enc, &substmt, curr_case_label_num++, switch_counter);
			//uni_printf(enc->sx->io, "_");
			//uni_printf(enc->sx->io, "%i", switch_counter);

		}
		else if (substmt_class == STMT_DEFAULT)
		{
			emit_default_statement(enc, &substmt, switch_counter);
		}
		else
		{
			emit_statement(enc, &substmt);
		}
	}

	emit_label_declaration(enc, &enc->label_break);
	enc->label_break = old_label_break;

	enc->label_if_true = old_label_if_true;
	enc->label_if_false = old_label_if_false;

	uni_printf(enc->sx->io, "%s%i%s\n", "CASE_CONDITION_", switch_counter, ":");
	uni_printf(enc->sx->io, "%s%zu\n", "\tlw t0, ", case_counter);
	uni_printf(enc->sx->io, "\trem t4, t3, t0\n"
							"\tslli t5, t4, 2 \n"
							"\tlw t6, t1(t5)  \n");
	uni_printf(enc->sx->io, "\t%s%i\n", "beqz t6, DEFAULT", switch_counter);
	uni_printf(enc->sx->io, "\tjr t6\n");
		
	uni_printf(enc->sx->io, "%s%i%s\n", "CASE_INSERT_", switch_counter, ":");
	uni_printf(enc->sx->io, "%s%zu\n", "\tlw t0, ", case_counter);
	uni_printf(enc->sx->io, "\trem t4, t3, t0\n"
							"\tslli t5, t4, 2 \n"
							"\tsw t2, t1(t5)  \n"
							"\tret\n");

}


/**
 *	Emit while statement
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */
static void emit_while_statement(encoder *const enc, const node *const nd)
{
	const size_t label_num = enc->label_num++;
	const label label_begin = { .kind = L_BEGIN_CYCLE, .num = label_num };
	const label label_end = { .kind = L_END, .num = label_num };

	const label loop_body = { .kind = L_THEN, .num = label_num };

	const label old_continue = enc->label_continue;
	const label old_break = enc->label_break;

	enc->label_continue = label_begin;
	enc->label_break = label_end;

	emit_label_declaration(enc, &label_begin);

	const node condition = statement_while_get_condition(nd);

	const label old_label_if_true = enc->label_if_true;
	const label old_label_if_false = enc->label_if_false;

	enc->label_if_true = loop_body;
	enc->label_if_false = label_end;

	const rvalue value = emit_expression(enc, &condition);

	enc->label_if_true = old_label_if_true;
	enc->label_if_false = old_label_if_false;

	free_rvalue(enc, &value);

	const node body = statement_while_get_body(nd);
	emit_label_declaration(enc, &loop_body);
	emit_statement(enc, &body);

	emit_unconditional_branch(enc, IC_RISCV_J, &label_begin);
	emit_label_declaration(enc, &label_end);

	enc->label_continue = old_continue;
	enc->label_break = old_break;
}

/**
 *	Emit do statement
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */
static void emit_do_statement(encoder *const enc, const node *const nd)
{
	const size_t label_num = enc->label_num++;
	const label label_begin = { .kind = L_BEGIN_CYCLE, .num = label_num };
	emit_label_declaration(enc, &label_begin);

	const label label_condition = { .kind = L_NEXT, .num = label_num };
	const label label_end = { .kind = L_END, .num = label_num };

	const label old_continue = enc->label_continue;
	const label old_break = enc->label_break;
	enc->label_continue = label_condition;
	enc->label_break = label_end;

	const node body = statement_do_get_body(nd);
	emit_statement(enc, &body);
	emit_label_declaration(enc, &label_condition);

	const node condition = statement_do_get_condition(nd);

	const label old_label_if_true = enc->label_if_true;
	const label old_label_if_false = enc->label_if_false;

	enc->label_if_true = label_begin;
	enc->label_if_false = label_end;

	const rvalue value = emit_expression(enc, &condition);

	enc->label_if_true = old_label_if_true;
	enc->label_if_false = old_label_if_false;

	emit_label_declaration(enc, &label_end);
	free_rvalue(enc, &value);

	enc->label_continue = old_continue;
	enc->label_break = old_break;
}

/**
 *	Emit for statement
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */
static void emit_for_statement(encoder *const enc, const node *const nd)
{
	const size_t scope_displacement = enc->scope_displ;

	if (statement_for_has_inition(nd))
	{
		const node inition = statement_for_get_inition(nd);
		emit_statement(enc, &inition);
	}

	const size_t label_num = enc->label_num++;
	const label label_begin = { .kind = L_BEGIN_CYCLE, .num = label_num };
	const label label_end = { .kind = L_END, .num = label_num };
	const label label_body = { .kind = L_THEN, .num = label_num };

	const label old_continue = enc->label_continue;
	const label old_break = enc->label_break;
	enc->label_continue = label_begin;
	enc->label_break = label_end;

	const label old_label_if_true = enc->label_if_true;
	const label old_label_if_false = enc->label_if_false;

	emit_label_declaration(enc, &label_begin);
	if (statement_for_has_condition(nd))
	{
		const node condition = statement_for_get_condition(nd);

		enc->label_if_true = label_body;
		enc->label_if_false = label_end;
		const rvalue value = emit_expression(enc, &condition);

		free_rvalue(enc, &value);
	}

	const node body = statement_for_get_body(nd);

	emit_label(enc, &label_body);
	uni_printf(enc->sx->io, "\n");

	emit_statement(enc, &body);

	if (statement_for_has_increment(nd))
	{
		const node increment = statement_for_get_increment(nd);
		emit_void_expression(enc, &increment);
	}

	emit_unconditional_branch(enc, IC_RISCV_J, &label_begin);
	emit_label_declaration(enc, &label_end);

	enc->label_if_true = old_label_if_true;
	enc->label_if_false = old_label_if_false;

	enc->label_continue = old_continue;
	enc->label_break = old_break;

	enc->scope_displ = scope_displacement;
}

/**
 *	Emit continue statement
 *
 *	@param	enc					Encoder
 */
static void emit_continue_statement(encoder *const enc)
{
	emit_unconditional_branch(enc, IC_RISCV_J, &enc->label_continue);
}

/**
 *	Emit break statement
 *
 *	@param	enc					Encoder
 */
static void emit_break_statement(encoder *const enc)
{
	emit_unconditional_branch(enc, IC_RISCV_J, &enc->label_break);
}

/**
 *	Emit return statement
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */
static void emit_return_statement(encoder *const enc, const node *const nd)
{
	if (statement_return_has_expression(nd))
	{
		const node expression = statement_return_get_expression(nd);
		const rvalue value = emit_expression(enc, &expression);

		const lvalue return_lval = { .kind = LVALUE_KIND_REGISTER, .loc.reg_num = R_A0, .type = value.type };

		emit_store_of_rvalue(enc, &return_lval, &value);
		free_rvalue(enc, &value);
	}

	const label label_end = { .kind = L_FUNCEND, .num = enc->curr_function_ident };
	emit_unconditional_branch(enc, IC_RISCV_J, &label_end);
}

/**
 *	Emit statement
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */
static void emit_statement(encoder *const enc, const node *const nd)
{
	switch (statement_get_class(nd))
	{
		case STMT_DECL:
			emit_declaration_statement(enc, nd);
			break;

		case STMT_CASE:
			system_error(node_unexpected);
			break;

		case STMT_DEFAULT:
			system_error(node_unexpected);
			break;

		case STMT_COMPOUND:
			emit_compound_statement(enc, nd);
			break;

		case STMT_EXPR:
			emit_void_expression(enc, nd);
			break;

		case STMT_NULL:
			break;

		case STMT_IF:
			emit_if_statement(enc, nd);
			break;

		case STMT_SWITCH:
			emit_switch_statement(enc, nd);
			break;

		case STMT_WHILE:
			emit_while_statement(enc, nd);
			break;

		case STMT_DO:
			emit_do_statement(enc, nd);
			break;

		case STMT_FOR:
			emit_for_statement(enc, nd);
			break;

		case STMT_CONTINUE:
			emit_continue_statement(enc);
			break;

		case STMT_BREAK:
			emit_break_statement(enc);
			break;

		case STMT_RETURN:
			emit_return_statement(enc, nd);
			break;

		default:
			break;
	}

	uni_printf(enc->sx->io, "\n");
}

/**
 *	Emit translation unit
 *
 *	@param	enc					Encoder
 *	@param	nd					Node in AST
 */
static int emit_translation_unit(encoder *const enc, const node *const nd)
{
	const size_t size = translation_unit_get_size(nd);
	for (size_t i = 0; i < size; i++)
	{
		const node decl = translation_unit_get_declaration(nd, i);
		emit_declaration(enc, &decl);
	}

	return enc->sx->rprt.errors != 0;
}

// В дальнейшем при необходимости сюда можно передавать флаги вывода директив
// TODO: подписать, что значит каждая директива и команда
static void pregen(syntax *const sx)
{
	// Подпись "GNU As:" для директив GNU
	// Подпись "RISCV Assembler:" для директив ассемблера RISCV

	uni_printf(sx->io, "\t.section .mdebug.abi32\n"); // ?
	uni_printf(sx->io, "\t.previous\n"); // следующая инструкция будет перенесена в секцию, описанную выше
	uni_printf(sx->io, "\t.nan\tlegacy\n");		  // ?
	uni_printf(sx->io, "\t.module fp=xx\n");	  // ?
	uni_printf(sx->io, "\t.module nooddspreg\n"); // ?
	uni_printf(sx->io, "\t.abicalls\n");		  // ?
	uni_printf(sx->io, "\t.option pic0\n"); // как если бы при компиляции была включена опция "-fpic" (что означает?)
	uni_printf(sx->io, "\t.text\n"); // последующий код будет перенесён в текстовый сегмент памяти
	// выравнивание последующих данных / команд по границе, кратной 2^n байт (в данном случае 2^2 = 4)
	uni_printf(sx->io, "\t.align 2\n");

	// делает метку main глобальной -- её можно вызывать извне кода (например, используется при линковке)
	uni_printf(sx->io, "\n\t.globl\tmain\n");
	uni_printf(sx->io, "\t.ent\tmain\n");			  // начало процедуры main
	uni_printf(sx->io, "\t.type\tmain, @function\n"); // тип "main" -- функция
	uni_printf(sx->io, "main:\n");

	// инициализация gp
	// "__gnu_local_gp" -- локация в памяти, где лежит Global Pointer
	uni_printf(sx->io, "\tlui gp, %%hi(__gnu_local_gp)\n");
	uni_printf(sx->io, "\taddiu gp, gp, %%lo(__gnu_local_gp)\n");

	// FIXME: сделать для ra, sp и fp отдельные глобальные rvalue
	to_code_2R(sx->io, IC_RISCV_MOVE, R_FP, R_SP);
	to_code_2R_I(sx->io, IC_RISCV_ADDI, R_SP, R_SP, -4);
	to_code_R_I_R(sx->io, IC_RISCV_SW, R_RA, 0, R_SP);
	to_code_R_I(sx->io, IC_RISCV_LI, R_T0, (double)LOW_DYN_BORDER);
	to_code_R_I_R(sx->io, IC_RISCV_SW, R_T0, -(item_t)HEAP_DISPL - 60, R_GP);
	uni_printf(sx->io, "\n");
}

// get from `clang --target=riscv64 -march=rv32gc -S`
static void pregen_riscv(syntax *const sx)
{
	//uni_printf(sx->io, ".data\n"
	//				   "\terror_message: .string \"Ошибка\"\n");
	uni_printf(sx->io, "\t.text\n"
					   "\t.attribute 4, 16\n"
	//				   "\t.attribute 5, \"rv32i2p0_m2p0_a2p0_f2p0_d2p0_c2p0\"\n"
					   "\t.file \"test.c\"\n"
					   "\t.globl main\n"
					   "\t.p2align	1\n"
					   "\t.type main,@function\n");
}

static void standart_functions(syntax *const sx)
{
	uni_printf(sx->io, ".s:\n"
					   "\t.ascii \"\%%s\\0\"\n");
	uni_printf(sx->io, ".i:\n"
					   "\t.ascii \"\%%i\\0\"\n");
	uni_printf(sx->io, ".f:\n"
					   "\t.ascii \"\%%f\\0\"\n");
	uni_printf(sx->io, ".b:\n"
					   "\t.ascii \"\%%b\\0\"\n");
	uni_printf(sx->io, ".printid:\n"
					   "\t.ascii \"\%%i \\0\"\n");
}

// создаём метки всех строк в программе
static void strings_declaration(encoder *const enc)
{
	//uni_printf(enc->sx->io, "\t.rdata\n");
	//uni_printf(enc->sx->io, "\t.align 2\n");

	const size_t amount = strings_amount(enc->sx);
	for (size_t i = 0; i < amount; i++)
	{
		item_t args_for_printf = 0;
		const label string_label = { .kind = L_STRING, .num = i };
		emit_label_declaration(enc, &string_label);
		uni_printf(enc->sx->io, "\t.ascii \"");

		const char *string = string_get(enc->sx, i);
		for (size_t j = 0; string[j] != '\0'; j++)
		{
			const char ch = string[j];
			if (ch == '\n')
			{
				uni_printf(enc->sx->io, "\\n");
			}
			else if (ch == '%')
			{
				args_for_printf++;
				j++;

				uni_printf(enc->sx->io, "%c", ch);
				uni_printf(enc->sx->io, "%c", string[j]);
				if (amount != 1)
					uni_printf(enc->sx->io, "%c", ch);

				uni_printf(enc->sx->io, "\\0\"\n");
				const label another_str_label = { .kind = L_STRING, .num = (size_t)(i + args_for_printf * amount) };
				emit_label_declaration(enc, &another_str_label);
				uni_printf(enc->sx->io, "\t.ascii \"");
			}
			else
			{
				uni_printf(enc->sx->io, "%c", ch);
			}
		}
		uni_printf(enc->sx->io, "\\0\"\n");
		//uni_printf(enc->sx->io, "%%\\0\"\n");
	}
	//uni_printf(enc->sx->io, "\t.text\n");
	//uni_printf(enc->sx->io, "\t.align 2\n\n");

	// Прыжок на главную метку
	//uni_printf(enc->sx->io, "\tjal main\n");

	// Выход из программы в конце работы
	//to_code_R_I_R(enc->sx->io, IC_RISCV_LW, R_RA, 0, R_SP);
	//emit_register_branch(enc, IC_RISCV_JR, R_RA);
}

static void postgen(encoder *const enc)
{
	// FIXME: целиком runtime.s не вставить, т.к. не понятно, что делать с modetab
	// По этой причине вставляю только defarr
	uni_printf(enc->sx->io, "\n\n# defarr\n\
# объявление одномерного массива\n\
# $a0 -- адрес первого элемента\n\
# $a1 -- размер измерения\n\
DEFARR1:\n\
	sw $a1, 4($a0)			# Сохранение границы\n\
	li $v0, 4				# Загрузка размера слова\n\
	mul $v0, $v0, $a1		# Подсчёт размера первого измерения массива в байтах\n\
	sub $v0, $a0, $v0		# Считаем адрес после конца массива, т.е. $v0 -- на слово ниже последнего элемента\n\
	addi $v0, $v0, -4\n\
	jr $ra\n\
\n\
# объявление многомерного массива, но сначала обязана вызываться процедура DEFARR1\n\
# $a0 -- адрес первого элемента\n\
# $a1 -- размер измерения\n\
# $a2 -- адрес первого элемента предыдущего измерения\n\
# $a3 -- размер предыдущего измерения\n\
DEFARR2:\n\
	sw $a0, 0($a2)			# Сохраняем адрес в элементе предыдущего измерения\n\
	move $t0, $ra			# Запоминаем $ra, чтобы он не затёрся\n\
	jal DEFARR1				# Выделение памяти под массив\n\
	move $ra, $t0			# Восстанавливаем $ra\n\
	addi $a2, $a2, -4		# В $a2 следующий элемент в предыдущем измерении\n\
	addi $a0, $v0, -4		# В $a0 первый элемент массива в текущем измерении, плюс выделяется место под размеры\n\
	addi $a3, $a3, -1		# Уменьшаем счётчик\n\
	bne $a3, $0, DEFARR2	# Прыгаем, если ещё не всё выделили\n\
	jr $ra\n");

	uni_printf(enc->sx->io, "\n\n\t.end\tmain\n");
	uni_printf(enc->sx->io, "\t.size\tmain, .-main\n");
}

// from `clang --target=riscv64 -march=rv32gc -S`
// TODO: use DEFARR from postgen when we will have code generation for arrays
static void postgen_riscv(syntax *const sx)
{
//	uni_printf(sx->io, " \n\n# defarr\n\
//# объявление одномерного массива\n\
//# a0 -- адрес первого элемента\n\
//# a1 -- размер измерения\n\
//DEFARR1:\n\
//	sw a1, 4(a0)			# Сохранение границы\n\
//	li t0, 4				# Загрузка размера слова\n\
//	mul t0, t0, a1		# Подсчёт размера первого измерения массива в байтах\n\
//	sub t0, a0, t0		# Считаем адрес после конца массива, т.е. t0 -- на слово ниже последнего элемента\n\
//	jr ra\n ");
//
//	uni_printf(sx->io, " \n\n# defarr\n\
//DEFARR2:\n\
//  sw a0, 0(a2) \n\
//  mv t0, ra \n  jal ra, DEFARR1 \n\
//  mv ra, t0  \n\  addi a2, a2, -8 \n\  addi a0, t0, -8 \n  addi a3, a3, -1  \n\  bnez a3, DEFARR2 \n\  jr ra\n");

	//uni_printf(sx->io, "error:
	//				   "\tla x1, error_message\n"
	//				   "\tli x17, 4\n"
	//				   "\tecall\n" 
	//				   "\tli x10, 1\n"
	//				   "\tecall\n");
	uni_printf(sx->io, ".Lfunc_end0:\n"
					   "\t.size	main, .Lfunc_end0-main\n"
					   "\t.section	\".note.GNU-stack\",\"\",@progbits\n");
					   //"\t.addrsig\n");
}

/*
 *	 __     __   __     ______   ______     ______     ______   ______     ______     ______
 *	/\ \   /\ "-.\ \   /\__  _\ /\  ___\   /\  == \   /\  ___\ /\  __ \   /\  ___\   /\  ___\
 *	\ \ \  \ \ \-.  \  \/_/\ \/ \ \  __\   \ \  __<   \ \  __\ \ \  __ \  \ \ \____  \ \  __\
 *	 \ \_\  \ \_\\"\_\    \ \_\  \ \_____\  \ \_\ \_\  \ \_\    \ \_\ \_\  \ \_____\  \ \_____\
 *	  \/_/   \/_/ \/_/     \/_/   \/_____/   \/_/ /_/   \/_/     \/_/\/_/   \/_____/   \/_____/
 */


typedef struct
{
	char *data;		 
	size_t size;	 
	size_t capacity; 
} StringArray;

StringArray *createStringArray()
{
	StringArray *array = malloc(sizeof(StringArray));
	if (array == NULL)
	{
		return NULL;
	}

	array->data = NULL;
	array->size = 0;
	array->capacity = 0;

	return array;
}

int encode_to_riscv(const workspace *const ws, syntax *const sx)
{
	if (!ws_is_correct(ws) || sx == NULL)
	{
		return -1;
	}

	encoder enc;
	enc.sx = sx;
	enc.next_register = R_T0;
	enc.next_float_register = R_FT0;
	enc.label_num = 1;
	enc.case_label_num = 1;

	enc.scope_displ = 0;
	enc.global_displ = 0;

	enc.displacements = hash_create(HASH_TABLE_SIZE);

	for (size_t i = 0; i < TEMP_REG_AMOUNT + TEMP_FP_REG_AMOUNT; i++)
	{
		enc.registers[i] = false;
	}

	// pregen(sx);
	//strings_declaration(&enc);
	// TODO: нормальное получение корня
	strings_declaration(&enc);
	pregen_riscv(sx);
	standart_functions(sx);
	//StringArray *postgen_funcs = createStringArray();


	const node root = node_get_root(&enc.sx->tree);
	// printf("%zx", sx->tree.size);
	const int ret = emit_translation_unit(&enc, &root);
	postgen_riscv(sx);
	// postgen(&enc);

	hash_clear(&enc.displacements);
	return ret;
}

void emit_slicing(encoder *const enc, int link, int size, int i)
{
	uni_printf(enc->sx->io, "\t li a0, -", link, "(fp)");
	uni_printf(enc->sx->io, "\t li a2, ", size);
	uni_printf(enc->sx->io, "\t li a3, ", i);
	uni_printf(enc->sx->io, "\t mul t0, a3, a2");
	uni_printf(enc->sx->io, "\t add a0, a0, t0");
}