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

#pragma once

#include <limits.h>


#ifdef __cplusplus
extern "C" {
#endif

typedef enum TOKEN
{
	TK_MAIN = 0,					/**< The main function token */
	TK_EOF = SHRT_MIN,				/**< End of file */

	// Keywords [C99 6.4.1]
	TK_CHAR,						/**< 'char'		keyword */
	TK_DOUBLE,						/**< 'double'	keyword */
	TK_FLOAT,						/**< 'float'	keyword */
	TK_INT,							/**< 'int'		keyword	*/
	TK_LONG,						/**< 'long'		keyword */
	TK_STRUCT,						/**< 'struct'	keyword */
	TK_VOID,						/**< 'void'		keyword */
	TK_FILE,						/**< 'file'		keyword */
	TK_TYPEDEF,						/**< 'typedef'	keyword */
	TK_IF,							/**< 'if'		keyword */
	TK_ELSE,						/**< 'else'		keyword */
	TK_DO,							/**< 'do'		keyword */
	TK_WHILE,						/**< 'while'	keyword */
	TK_FOR,							/**< 'for'		keyword */
	TK_SWITCH,						/**< 'switch'	keyword */
	TK_CASE,						/**< 'case'		keyword */
	TK_DEFAULT,						/**< 'default'	keyword */
	TK_BREAK,						/**< 'break'	keyword */
	TK_CONTINUE,					/**< 'continue'	keyword */
	TK_GOTO,						/**< 'goto'		keyword */
	TK_RETURN,						/**< 'return'	keyword */

	// Identifiers [C99 6.4.2]
	TK_IDENTIFIER,					/**< Identifier [C99 6.4.2] */

	// Constants [C99 6.4.4]
	TK_INT_CONST,					/**< Integer Constant [C99 6.4.4.1] */
	TK_FLOAT_CONST,					/**< Floating Constant [C99 6.4.4.2] */
	TK_CHAR_CONST,					/**< Character Constant [C99 6.4.4.4] */
	TK_STRING,						/**< String Literal [C99 6.4.5] */

	// Punctuators [C99 6.4.6]
	TK_L_SQUARE,					/**< '['	punctuator */
	TK_L_PAREN,						/**< '('	punctuator */
	TK_L_BRACE,						/**< '{'	punctuator */
	TK_R_SQUARE		= 0b00000001,	/**< ']'	punctuator */
	TK_R_PAREN		= 0b00000010,	/**< ')'	punctuator */
	TK_R_BRACE		= 0b00000100,	/**< '}'	punctuator */
	TK_COMMA		= 0b00001000,	/**< ','	punctuator */
	TK_COLON		= 0b00010000,	/**< ':'	punctuator */
	TK_SEMICOLON	= 0b00100000,	/**< ';'	punctuator */
	TK_QUESTION 	= TK_L_BRACE+1,	/**< '?'	punctuator */
	TK_TILDE,						/**< '~'	punctuator */
	TK_PERIOD,						/**< '.'	punctuator */
	TK_PLUS,						/**< '+'	punctuator */
	TK_MINUS,						/**< '-'	punctuator */
	TK_STAR,						/**< '*'	punctuator */
	TK_SLASH,						/**< '/'	punctuator */
	TK_PERCENT,						/**< '%'	punctuator */
	TK_EXCLAIM,						/**< '!'	punctuator */
	TK_CARET,						/**< '^'	punctuator */
	TK_PIPE,						/**< '|'	punctuator */
	TK_AMP,							/**< '&'	punctuator */
	TK_EQUAL,						/**< '='	punctuator */
	TK_LESS,						/**< '<'	punctuator */
	TK_GREATER,						/**< '>'	punctuator */
	TK_ARROW,						/**< '->'	punctuator */
	TK_PLUS_EQUAL,					/**< '+='	punctuator */
	TK_PLUS_PLUS,					/**< '++'	punctuator */
	TK_MINUS_EQUAL,					/**< '-='	punctuator */
	TK_MINUS_MINUS,					/**< '--'	punctuator */
	TK_STAR_EQUAL,					/**< '*='	punctuator */
	TK_SLASH_EQUAL,					/**< '/='	punctuator */
	TK_PERCENT_EQUAL,				/**< '%='	punctuator */
	TK_EXCLAIM_EQUAL,				/**< '!='	punctuator */
	TK_CARET_EQUAL,					/**< '^='	punctuator */
	TK_PIPE_EQUAL,					/**< '|='	punctuator */
	TK_PIPE_PIPE,					/**< '||'	punctuator */
	TK_AMP_EQUAL,					/**< '&='	punctuator */
	TK_AMP_AMP,						/**< '&&'	punctuator */
	TK_EQUAL_EQUAL,					/**< '=='	punctuator */
	TK_LESS_EQUAL,					/**< '<='	punctuator */
	TK_LESS_LESS,					/**< '<<'	punctuator */
	TK_GREATER_EQUAL,				/**< '>='	punctuator */
	TK_GREATER_GREATER,				/**< '>>'	punctuator */
	TK_LESS_LESS_EQUAL,				/**< '<<='	punctuator */
	TK_GREATER_GREATER_EQUAL,		/**< '>>='	punctuator */

	TK_PRINT,						/**< 'print'	keyword	*/
	TK_PRINTF,						/**< 'printf'	keyword */
	TK_PRINTID,						/**< 'printid'	keyword */
	TK_SCANF,						/**< 'scanf'	keyword */
	TK_GETID,						/**< 'getid'	keyword */
	TK_ABS,							/**< 'abs'		keyword */
	TK_UPB,							/**< 'upb'		keyword */
	TK_CREATE_DIRECT,				/**< 't_create_direct'	keyword */
	TK_EXIT_DIRECT,					/**< 't_exit_direct'	keyword */
} token_t;

#ifdef __cplusplus
} /* extern "C" */
#endif
