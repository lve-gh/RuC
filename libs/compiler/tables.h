/*
 *	Copyright 2019 Andrey Terekhov
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
#include "context.h"

#ifdef __cplusplus
extern "C"
{
#endif

	/**
	 * Save up a string array to reprtab
	 *
	 * @param context   RuC context
	 * @param str       Target string
	 *
	 * @return FIXME
	 */
	int toreprtab(compiler_context *context, char str[]);

	/**
	 * Mode table initialization
	 *
	 * @param context   RuC context
	 */
	void init_modetab(compiler_context *context);

#ifdef __cplusplus
} /* extern "C" */
#endif
